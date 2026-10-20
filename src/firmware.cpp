#include "firmware.hpp"
#include "media.hpp"
#include "remote.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <poll.h>
#include <pwd.h>
#include <regex>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <variant>
#include <vector>

namespace udm {
namespace {

namespace fs = std::filesystem;

class Failure : public std::runtime_error {
public:
    explicit Failure(const std::string& message) : std::runtime_error(message) {}
};

std::string sha256(std::span<const std::uint8_t> bytes) {
    return hex_digest(digest256(bytes));
}

std::string md5(std::span<const std::uint8_t> bytes) {
    return hex_digest(digest_md5(bytes));
}

Bytes bytes_of(std::string_view value) {
    return Bytes(value.begin(), value.end());
}

std::uint16_t load16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 2) {
        throw Failure("ELF range outside input");
    }
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(bytes[offset + 1] << 8);
}

std::uint32_t load32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        throw Failure("ELF range outside input");
    }
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void store32(std::span<std::uint8_t> bytes, std::uint32_t value) {
    if (bytes.size() < 4) throw Failure("32-bit store outside output");
    bytes[0] = static_cast<std::uint8_t>(value);
    bytes[1] = static_cast<std::uint8_t>(value >> 8);
    bytes[2] = static_cast<std::uint8_t>(value >> 16);
    bytes[3] = static_cast<std::uint8_t>(value >> 24);
}

Bytes read_bounded(const fs::path& path, std::size_t maximum) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw Failure("unable to open " + path.string());
    }
    Bytes result;
    result.resize(maximum + 1);
    stream.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()));
    const auto count = stream.gcount();
    if (count < 0 || static_cast<std::size_t>(count) > maximum) {
        throw Failure("oversized artifact: " + path.string());
    }
    result.resize(static_cast<std::size_t>(count));
    return result;
}

void write_all(int descriptor, std::span<const std::uint8_t> data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto count = ::write(descriptor, data.data() + offset, data.size() - offset);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw Failure("write failed");
        }
        if (count == 0) {
            throw Failure("short write");
        }
        offset += static_cast<std::size_t>(count);
    }
}

void write_file(const fs::path& path, std::span<const std::uint8_t> data, bool exclusive = true) {
    const int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (exclusive ? O_EXCL : O_TRUNC);
    const int descriptor = ::open(path.c_str(), flags, 0600);
    if (descriptor < 0) {
        throw Failure("unable to create " + path.string() + ": " + std::strerror(errno));
    }
    try {
        write_all(descriptor, data);
        if (::fsync(descriptor) != 0) {
            throw Failure("fsync failed for " + path.string());
        }
        ::close(descriptor);
    } catch (...) {
        ::close(descriptor);
        throw;
    }
}

void fsync_directory(const fs::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        throw Failure("unable to open directory " + path.string());
    }
    if (::fsync(descriptor) != 0) {
        ::close(descriptor);
        throw Failure("directory fsync failed for " + path.string());
    }
    ::close(descriptor);
}

fs::path make_directory(const fs::path& parent, std::string_view prefix) {
    fs::create_directories(parent);
    std::string pattern = (parent / (std::string(prefix) + "XXXXXX")).string();
    std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
    mutable_pattern.push_back('\0');
    if (::mkdtemp(mutable_pattern.data()) == nullptr) {
        throw Failure("unable to create private build directory");
    }
    return fs::path(mutable_pattern.data());
}

struct ProcessResult {
    int code = -1;
    std::string output;
    std::string errors;
};

std::string read_text_bounded(const fs::path& path, std::size_t maximum) {
    const auto data = read_bounded(path, maximum);
    return std::string(data.begin(), data.end());
}

ProcessResult run_process(const std::vector<std::string>& arguments, const fs::path& directory,
                          std::size_t output_cap = 1024 * 1024) {
    if (arguments.empty()) {
        throw Failure("empty subprocess command");
    }
    const auto capture = make_directory(directory, "command-");
    const auto output_path = capture / "stdout";
    const auto error_path = capture / "stderr";
    const int output_descriptor = ::open(output_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    const int error_descriptor = ::open(error_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (output_descriptor < 0 || error_descriptor < 0) {
        if (output_descriptor >= 0) {
            ::close(output_descriptor);
        }
        if (error_descriptor >= 0) {
            ::close(error_descriptor);
        }
        throw Failure("unable to create subprocess capture files");
    }
    const auto child = ::fork();
    if (child < 0) {
        ::close(output_descriptor);
        ::close(error_descriptor);
        throw Failure("unable to fork subprocess");
    }
    if (child == 0) {
        if (::dup2(output_descriptor, STDOUT_FILENO) != STDOUT_FILENO ||
            ::dup2(error_descriptor, STDERR_FILENO) != STDERR_FILENO) _exit(126);
        ::close(output_descriptor);
        ::close(error_descriptor);
        if (::chdir(directory.c_str()) != 0) {
            _exit(126);
        }
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (const auto& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp(argv.front(), argv.data());
        _exit(127);
    }
    ::close(output_descriptor);
    ::close(error_descriptor);
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        throw Failure("unable to wait for subprocess");
    }
    ProcessResult result;
    result.code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    result.output = read_text_bounded(output_path, output_cap);
    result.errors = read_text_bounded(error_path, output_cap);
    fs::remove_all(capture);
    return result;
}

std::string find_executable(std::string_view name) {
    if (name.find('/') != std::string_view::npos) {
        if (::access(std::string(name).c_str(), X_OK) == 0) {
            return std::string(name);
        }
        throw Failure("executable is unavailable: " + std::string(name));
    }
    const char* environment = std::getenv("PATH");
    if (environment == nullptr) {
        throw Failure("PATH is unavailable while locating " + std::string(name));
    }
    std::string path(environment);
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const auto end = path.find(':', begin);
        const auto component = path.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        const auto candidate = fs::path(component.empty() ? "." : component) / name;
        if (::access(candidate.c_str(), X_OK) == 0) {
            return candidate.string();
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    throw Failure("executable is unavailable: " + std::string(name));
}

std::string find_linker() {
    try {
        return find_executable("ld.lld");
    } catch (const Failure&) {
    }
    const auto* account = ::getpwuid(::getuid());
    if (account == nullptr || account->pw_dir == nullptr) {
        throw Failure("installed LLVM linker is unavailable");
    }
    const auto toolchains = fs::path(account->pw_dir) / ".rustup/toolchains";
    if (!fs::exists(toolchains)) {
        throw Failure("installed LLVM linker is unavailable");
    }
    std::vector<fs::path> candidates;
    for (const auto& entry : fs::directory_iterator(toolchains)) {
        const auto candidate = entry.path() / "lib/rustlib/x86_64-unknown-linux-gnu/bin/gcc-ld/ld.lld";
        if (fs::is_regular_file(candidate) && ::access(candidate.c_str(), X_OK) == 0) candidates.push_back(candidate);
    }
    if (candidates.empty()) throw Failure("installed LLVM linker is unavailable");
    std::sort(candidates.begin(), candidates.end());
    return candidates.back().string();
}

struct ElfSection {
    std::uint32_t index = 0;
    std::uint32_t name = 0;
    std::uint32_t type = 0;
    std::uint32_t flags = 0;
    std::uint32_t address = 0;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    std::uint32_t link = 0;
    std::uint32_t info = 0;
    std::uint32_t alignment = 0;
    std::uint32_t entry_size = 0;
};

std::pair<std::uint32_t, std::uint32_t> checked_range(std::uint32_t offset, std::uint32_t size,
                                                      std::size_t limit, std::string_view label) {
    if (offset > limit || size > limit - offset) {
        throw Failure(std::string(label) + " is outside the input file");
    }
    return {offset, offset + size};
}

std::string elf_string(std::span<const std::uint8_t> table, std::uint32_t offset) {
    if (offset >= table.size()) {
        throw Failure("ELF string offset is outside input");
    }
    const auto* begin = table.data() + offset;
    const auto* end = std::find(begin, table.data() + table.size(), static_cast<std::uint8_t>(0));
    if (end == table.data() + table.size()) {
        throw Failure("ELF string is not NUL terminated");
    }
    return std::string(reinterpret_cast<const char*>(begin), reinterpret_cast<const char*>(end));
}

std::vector<ElfSection> read_sections(std::span<const std::uint8_t> data, std::uint16_t& type,
                                      std::uint32_t& program_offset, std::uint16_t& program_entry,
                                      std::uint16_t& program_count, std::uint32_t& section_offset,
                                      std::uint16_t& section_entry, std::uint16_t& section_count,
                                      std::uint16_t& names_index) {
    if (data.size() < 52) {
        throw Failure("ELF header is truncated");
    }
    if (data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F' ||
        data[4] != 1 || data[5] != 1 || data[6] != 1) {
        throw Failure("input is not ELF32 little-endian version 1");
    }
    type = load16(data, 16);
    const auto machine = load16(data, 18);
    const auto version = load32(data, 20);
    program_offset = load32(data, 28);
    section_offset = load32(data, 32);
    program_entry = load16(data, 42);
    program_count = load16(data, 44);
    section_entry = load16(data, 46);
    section_count = load16(data, 48);
    names_index = load16(data, 50);
    if (machine != 40 || version != 1 || section_entry != 40 || section_count < 2 || section_count > 256 ||
        names_index >= section_count) {
        throw Failure("invalid ARM ELF section table");
    }
    checked_range(section_offset, static_cast<std::uint32_t>(section_entry) * section_count, data.size(), "section table");
    if (program_count != 0 && (program_entry != 32 || program_count > 16)) {
        throw Failure("invalid ELF program headers");
    }
    std::vector<ElfSection> sections;
    sections.reserve(section_count);
    for (std::uint16_t index = 0; index < section_count; ++index) {
        const auto offset = section_offset + static_cast<std::uint32_t>(index) * section_entry;
        ElfSection section;
        section.index = index;
        section.name = load32(data, offset);
        section.type = load32(data, offset + 4);
        section.flags = load32(data, offset + 8);
        section.address = load32(data, offset + 12);
        section.offset = load32(data, offset + 16);
        section.size = load32(data, offset + 20);
        section.link = load32(data, offset + 24);
        section.info = load32(data, offset + 28);
        section.alignment = load32(data, offset + 32);
        section.entry_size = load32(data, offset + 36);
        if (section.type != 8) {
            checked_range(section.offset, section.size, data.size(), "ELF section");
        }
        if (section.link >= section_count && section.link != 0) {
            throw Failure("ELF section link is invalid");
        }
        sections.push_back(section);
    }
    return sections;
}

struct ObjectPayload {
    Bytes text;
    std::string elf_sha256;
};

ObjectPayload parse_object(const Bytes& input, std::string_view selected_name, std::size_t max_bytes) {
    if (input.size() > 16 * 1024 * 1024) {
        throw Failure("ELF exceeds parser limit");
    }
    std::uint16_t type = 0;
    std::uint32_t program_offset = 0;
    std::uint16_t program_entry = 0;
    std::uint16_t program_count = 0;
    std::uint32_t section_offset = 0;
    std::uint16_t section_entry = 0;
    std::uint16_t section_count = 0;
    std::uint16_t names_index = 0;
    const auto sections = read_sections(input, type, program_offset, program_entry, program_count,
                                        section_offset, section_entry, section_count, names_index);
    if (type != 1 || program_count != 0) {
        throw Failure("input must be an ARM ET_REL object without program headers");
    }
    const auto& string_section = sections[names_index];
    if (string_section.type != 3) {
        throw Failure("ELF section-name table is invalid");
    }
    const auto strings = std::span<const std::uint8_t>(input).subspan(string_section.offset, string_section.size);
    std::optional<ElfSection> selected;
    for (const auto& section : sections) {
        const auto name = elf_string(strings, section.name);
        if (name == selected_name) {
            if (selected.has_value()) {
                throw Failure("ELF has duplicate selected sections");
            }
            selected = section;
        }
    }
    if (!selected.has_value() || selected->type != 1 ||
        (selected->flags & 0x6U) != 0x6U || selected->size == 0 || selected->size > max_bytes) {
        throw Failure("selected executable section is invalid");
    }
    for (const auto& section : sections) {
        const auto name = elf_string(strings, section.name);
        if (section.size != 0 && (section.type == 4 || section.type == 9)) {
            throw Failure("relocation section is not empty: " + name);
        }
        if (section.index != selected->index && section.size != 0 && (section.flags & 0x2U) != 0) {
            throw Failure("unexpected allocated section " + name);
        }
    }
    ObjectPayload result;
    result.text.assign(input.begin() + selected->offset, input.begin() + selected->offset + selected->size);
    result.elf_sha256 = sha256(input);
    return result;
}

struct LinkedPayload {
    Bytes text;
    std::map<std::string, std::uint32_t> symbols;
};

LinkedPayload parse_linked(const Bytes& input, std::uint32_t extension, std::size_t max_payload,
                           const std::set<std::string>& required) {
    if (input.size() < 52 || input.size() > 16 * 1024 * 1024) {
        throw Failure("invalid linked ELF size");
    }
    std::uint16_t type = 0;
    std::uint32_t program_offset = 0;
    std::uint16_t program_entry = 0;
    std::uint16_t program_count = 0;
    std::uint32_t section_offset = 0;
    std::uint16_t section_entry = 0;
    std::uint16_t section_count = 0;
    std::uint16_t names_index = 0;
    const auto sections = read_sections(input, type, program_offset, program_entry, program_count,
                                        section_offset, section_entry, section_count, names_index);
    if (type != 2 || section_count <= 1) {
        throw Failure("expected little-endian ARM ELF32 executable");
    }
    const auto& names_section = sections[names_index];
    if (names_section.type != 3) {
        throw Failure("section name table is invalid");
    }
    const auto names = std::span<const std::uint8_t>(input).subspan(names_section.offset, names_section.size);
    std::optional<Bytes> text;
    std::map<std::string, std::uint32_t> symbols;
    for (const auto& section : sections) {
        const auto name = elf_string(names, section.name);
        if ((section.type == 4 || section.type == 6 || section.type == 9 || section.type == 11) && section.size != 0) {
            throw Failure("linked relocations or dynamic state are forbidden");
        }
        if ((section.flags & 0x2U) != 0 && section.size != 0) {
            if (text.has_value() || name != ".text" || section.type != 1 || (section.flags & 0x7U) != 0x6U ||
                (section.flags & ~0x36U) != 0 || section.address != extension || section.size > max_payload) {
                throw Failure("unexpected allocated section");
            }
            text = Bytes(input.begin() + section.offset, input.begin() + section.offset + section.size);
        } else if (section.type != 8) {
            checked_range(section.offset, section.size, input.size(), "linked section");
        }
        if (section.type == 2) {
            if (section.entry_size != 16 || section.size % 16 != 0 || section.link >= sections.size() ||
                sections[section.link].type != 3) {
                throw Failure("invalid linked symbol table");
            }
            const auto table_section = sections[section.link];
            const auto table = std::span<const std::uint8_t>(input).subspan(table_section.offset, table_section.size);
            for (std::uint32_t offset = section.offset; offset < section.offset + section.size; offset += 16) {
                const auto symbol_name = load32(input, offset);
                const auto value = load32(input, offset + 4);
                const auto size = load32(input, offset + 8);
                const auto bindtype = input[offset + 12];
                const auto target = load16(input, offset + 14);
                const auto label = elf_string(table, symbol_name);
                if (target == 0 && (!label.empty() || value != 0 || size != 0)) {
                    throw Failure("unresolved linked symbol: " + label);
                }
                if (required.contains(label)) {
                    if (symbols.contains(label) || (bindtype & 0x0fU) != 2 || (value & 1U) == 0) {
                        throw Failure("invalid Thumb function symbol: " + label);
                    }
                    symbols.emplace(label, value & ~1U);
                }
            }
        }
    }
    if (!text.has_value() || symbols.size() != required.size()) {
        if (!text.has_value()) {
            throw Failure("linked payload is missing .text");
        }
        for (const auto& symbol : required) {
            if (!symbols.contains(symbol)) {
                throw Failure("linked payload is missing symbol: " + symbol);
            }
        }
    }
    for (const auto& [label, address] : symbols) {
        if (address < extension || address >= extension + text->size()) {
            throw Failure("function symbol lies outside payload: " + label);
        }
    }
    if (program_count != 0) {
        checked_range(program_offset, static_cast<std::uint32_t>(program_entry) * program_count, input.size(), "program headers");
        for (std::uint16_t index = 0; index < program_count; ++index) {
            const auto offset = program_offset + static_cast<std::uint32_t>(index) * program_entry;
            const auto segment_type = load32(input, offset);
            const auto segment_offset = load32(input, offset + 4);
            const auto virtual_address = load32(input, offset + 8);
            const auto physical_address = load32(input, offset + 12);
            const auto file_size = load32(input, offset + 16);
            const auto memory_size = load32(input, offset + 20);
            const auto permissions = load32(input, offset + 24);
            if (segment_type == 1 && (file_size != memory_size || (permissions & 2U) != 0 ||
                                      virtual_address != physical_address || virtual_address < extension ||
                                      virtual_address + memory_size > extension + text->size())) {
                throw Failure("unexpected linked load segment");
            }
            checked_range(segment_offset, file_size, input.size(), "program segment");
        }
    }
    return LinkedPayload{std::move(*text), std::move(symbols)};
}

std::string format_address(std::uint32_t address) {
    std::ostringstream output;
    output << "0x" << std::hex << std::setfill('0') << std::setw(8) << address;
    return output.str();
}

std::string json_escape(std::string_view value) {
    std::ostringstream output;
    output << '"';
    for (const auto character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                output << "\\u" << std::hex << std::setfill('0') << std::setw(4)
                       << static_cast<unsigned>(static_cast<unsigned char>(character)) << std::dec;
            } else {
                output << character;
            }
        }
    }
    output << '"';
    return output.str();
}

std::string join_json(const std::vector<std::string>& values) {
    std::ostringstream output;
    output << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        output << values[index];
    }
    output << ']';
    return output.str();
}

struct Json {
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;
    using Value = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object>;
    Value value;
    Json() : value(nullptr) {}
    Json(bool input) : value(input) {}
    Json(std::int64_t input) : value(input) {}
    Json(double input) : value(input) {}
    Json(std::string input) : value(std::move(input)) {}
    Json(const char* input) : value(std::string(input)) {}
    Json(Array input) : value(std::move(input)) {}
    Json(Object input) : value(std::move(input)) {}
};

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    Json parse() {
        skip();
        const auto value = parse_value();
        skip();
        if (position_ != input_.size()) {
            throw Failure("trailing JSON data");
        }
        return value;
    }

private:
    void skip() {
        while (position_ < input_.size() && (input_[position_] == ' ' || input_[position_] == '\n' ||
                                             input_[position_] == '\r' || input_[position_] == '\t')) {
            ++position_;
        }
    }

    char take() {
        if (position_ >= input_.size()) {
            throw Failure("truncated JSON");
        }
        return input_[position_++];
    }

    void expect(char expected) {
        if (take() != expected) {
            throw Failure("invalid JSON token");
        }
    }

    Json parse_value() {
        skip();
        if (position_ >= input_.size()) {
            throw Failure("truncated JSON value");
        }
        switch (input_[position_]) {
        case '{': return parse_object();
        case '[': return parse_array();
        case '"': return Json(parse_string());
        case 't': consume("true"); return Json(true);
        case 'f': consume("false"); return Json(false);
        case 'n': consume("null"); return Json();
        default: return parse_number();
        }
    }

    void consume(std::string_view token) {
        if (input_.substr(position_, token.size()) != token) {
            throw Failure("invalid JSON literal");
        }
        position_ += token.size();
    }

    std::string parse_string() {
        expect('"');
        std::string result;
        while (position_ < input_.size()) {
            const auto character = take();
            if (character == '"') {
                return result;
            }
            if (character == '\\') {
                const auto escape = take();
                switch (escape) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u': {
                    if (position_ + 4 > input_.size()) {
                        throw Failure("truncated JSON unicode escape");
                    }
                    unsigned value = 0;
                    for (unsigned index = 0; index < 4; ++index) {
                        const auto digit = input_[position_++];
                        value <<= 4;
                        if (digit >= '0' && digit <= '9') value |= static_cast<unsigned>(digit - '0');
                        else if (digit >= 'a' && digit <= 'f') value |= static_cast<unsigned>(digit - 'a' + 10);
                        else if (digit >= 'A' && digit <= 'F') value |= static_cast<unsigned>(digit - 'A' + 10);
                        else throw Failure("invalid JSON unicode escape");
                    }
                    if (value < 0x80) result.push_back(static_cast<char>(value));
                    else if (value < 0x800) {
                        result.push_back(static_cast<char>(0xc0 | (value >> 6)));
                        result.push_back(static_cast<char>(0x80 | (value & 0x3f)));
                    } else {
                        result.push_back(static_cast<char>(0xe0 | (value >> 12)));
                        result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
                        result.push_back(static_cast<char>(0x80 | (value & 0x3f)));
                    }
                    break;
                }
                default: throw Failure("invalid JSON escape");
                }
            } else {
                if (static_cast<unsigned char>(character) < 0x20) {
                    throw Failure("control character in JSON string");
                }
                result.push_back(character);
            }
        }
        throw Failure("unterminated JSON string");
    }

    Json parse_object() {
        expect('{');
        Json::Object object;
        skip();
        if (position_ < input_.size() && input_[position_] == '}') {
            ++position_;
            return Json(std::move(object));
        }
        while (true) {
            skip();
            const auto key = parse_string();
            skip();
            expect(':');
            const auto [iterator, inserted] = object.emplace(key, parse_value());
            if (!inserted) {
                throw Failure("duplicate JSON object key");
            }
            skip();
            const auto delimiter = take();
            if (delimiter == '}') {
                return Json(std::move(object));
            }
            if (delimiter != ',') {
                throw Failure("invalid JSON object delimiter");
            }
        }
    }

    Json parse_array() {
        expect('[');
        Json::Array array;
        skip();
        if (position_ < input_.size() && input_[position_] == ']') {
            ++position_;
            return Json(std::move(array));
        }
        while (true) {
            array.push_back(parse_value());
            skip();
            const auto delimiter = take();
            if (delimiter == ']') {
                return Json(std::move(array));
            }
            if (delimiter != ',') {
                throw Failure("invalid JSON array delimiter");
            }
        }
    }

    Json parse_number() {
        const auto begin = position_;
        if (input_[position_] == '-') {
            ++position_;
        }
        while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
            ++position_;
        }
        bool floating = false;
        if (position_ < input_.size() && input_[position_] == '.') {
            floating = true;
            ++position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            floating = true;
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
        }
        const auto token = input_.substr(begin, position_ - begin);
        if (token.empty() || token == "-") {
            throw Failure("invalid JSON number");
        }
        try {
            if (floating) {
                return Json(std::stod(std::string(token)));
            }
            return Json(static_cast<std::int64_t>(std::stoll(std::string(token))));
        } catch (const std::exception&) {
            throw Failure("invalid JSON number");
        }
    }

    std::string_view input_;
    std::size_t position_ = 0;
};

const Json::Object& object(const Json& value, std::string_view label) {
    const auto* result = std::get_if<Json::Object>(&value.value);
    if (result == nullptr) {
        throw Failure(std::string(label) + " is not an object");
    }
    return *result;
}

const Json& field(const Json::Object& value, std::string_view name) {
    const auto iterator = value.find(std::string(name));
    if (iterator == value.end()) {
        throw Failure("missing JSON field: " + std::string(name));
    }
    return iterator->second;
}

std::string string_value(const Json& value, std::string_view label) {
    const auto* result = std::get_if<std::string>(&value.value);
    if (result == nullptr) {
        throw Failure(std::string(label) + " is not a string");
    }
    return *result;
}

std::int64_t integer_value(const Json& value, std::string_view label) {
    const auto* result = std::get_if<std::int64_t>(&value.value);
    if (result == nullptr) {
        throw Failure(std::string(label) + " is not an integer");
    }
    return *result;
}

bool boolean_value(const Json& value, std::string_view label) {
    const auto* result = std::get_if<bool>(&value.value);
    if (result == nullptr) {
        throw Failure(std::string(label) + " is not a boolean");
    }
    return *result;
}

const Json::Array& array(const Json& value, std::string_view label) {
    const auto* result = std::get_if<Json::Array>(&value.value);
    if (result == nullptr) {
        throw Failure(std::string(label) + " is not an array");
    }
    return *result;
}

bool has_string(const Json::Object& object_value, std::string_view name, std::string_view expected) {
    return string_value(field(object_value, name), name) == expected;
}

constexpr std::uint32_t flash_base = 0x08000000U;
constexpr std::uint32_t flash_size = 0x100000U;
constexpr std::uint32_t hashed_size = 0xE4F98U;
constexpr std::uint32_t footer_offset = 0xE4F98U;
constexpr std::uint32_t stock_package_size = 0xE4FA0U;
constexpr std::uint32_t extension = 0x080E5000U;
constexpr std::uint32_t max_payload = 0x1B000U;
constexpr std::string_view base_sha256 = "68f4a31444c7cbece17fb0f511f62fb8a66403ef9b7a394db39dcbf635f6dd99";
constexpr std::string_view base_prefix_md5 = "7d257b2b2cb958c93ffb447fa1de5b39";
constexpr std::string_view candidate_sha256 = "dc5404fe0de1f9a6e122f80efa03f685a93dbd907a4cc2820b1f01580cf4e2d2";
constexpr std::string_view candidate_prefix_md5 = "8c9321956b415adaff5c1ff586b6634f";
constexpr std::string_view stock_package_sha256 = "933eeae931a276ac19b74711542408e840238721282c76a5ca59c01e045008b1";
constexpr std::string_view stock_sidecar_sha256 = "486bc8f7431ca38af1b039ee154d196668e0f7923180b63c09b0c13bf4683290";
constexpr std::string_view source_sha256 = "2e030e2f2a18aa9a61d49559beaf4a721479f994930d99f3690c931186429409";
constexpr std::string_view footer_hex = "15ea00003206a459";

struct PairPin {
    std::string name;
    std::string binary_sha256;
    std::string sidecar_sha256;
    std::string prefix_md5;
    std::string flash_sha256;
};

PairPin candidate_pin(std::string name, std::string binary, std::string prefix) {
    const auto sidecar = bytes_of(prefix);
    return PairPin{std::move(name), std::move(binary), sha256(sidecar), std::move(prefix), {}};
}

std::vector<PairPin> known_pairs() {
    std::vector<PairPin> result;
    result.push_back(PairPin{"stock", std::string(stock_package_sha256), std::string(stock_sidecar_sha256),
                             std::string(base_prefix_md5), std::string(base_sha256)});
    auto current = candidate_pin("candidate", std::string(candidate_sha256), std::string(candidate_prefix_md5));
    current.flash_sha256 = current.binary_sha256;
    result.push_back(std::move(current));
    const std::array<std::pair<std::string_view, std::pair<std::string_view, std::string_view>>, 26> historical{
        std::pair{"candidate-v26", std::pair{"aab066d5c6abb79d3caf07e61d32663cbb91e271f2dd0e9c0389f344dbbff61f", "1f0171a1a3294c8eece12cfaaf9d08ff"}},
        std::pair{"candidate-v25", std::pair{"cda9f1fdb21b5d83722b4f6e7fddab142e86c4d61f68efad5ceb753c02bf3ca2", "3863fe848bedcd4c8fef5688b532c0e7"}},
        std::pair{"candidate-v24", std::pair{"fd29168c7c65904578d61c2d4253d6850198b700ec370548bbadbb4d20eb180b", "a99644a4cae9e75be90f43698c8f2eda"}},
        std::pair{"candidate-v23", std::pair{"b75c6814301708328515576b6cea3eb4f467c85c9b337f9165e7558cb69fbe2c", "dfe6e0d04565299681d4a6c9c96c4f30"}},
        std::pair{"candidate-v22", std::pair{"e79e5ea51356b920a1b998d360ab8c1e856ff3879714f9bf27f5e07422a4d0f6", "24a4f7c46424247abe001a3997367b07"}},
        std::pair{"candidate-v21", std::pair{"8e88b81b8077c7c14905a8bc5fa0dda08f1f6daa24666a3191a458c877aefba5", "b54c4d1aceff93ca117565915a96b262"}},
        std::pair{"candidate-v20", std::pair{"69b0c6df9d83e6fabc648084e7964b678560bbb28c761c5d3178ffc950f15708", "6eb18f6e1aa28a623040272726dc67ae"}},
        std::pair{"candidate-v19", std::pair{"792fd1933186a0f776b11416a1b4a1f61b7f94634bce09b5a45099a2ac562315", "a35d2fd2e604665938ab869f45c19a9f"}},
        std::pair{"candidate-v18", std::pair{"332104348eba0479de74ce95f211a45c2c847c2bfa3b97d7841a9ce120c684ff", "d9f191d3028c05bba717bbd98a838067"}},
        std::pair{"candidate-v17", std::pair{"76b6403b56d31143503d1fb0d5ed5feb2ab7310a9b608a9934ba997f047aba20", "9aa9054e38393450c04a3b6cc0e3c9e4"}},
        std::pair{"candidate-v16", std::pair{"8cbfc0194156b0136e01514aea3ecf391992a00b3e9af67ee80f51ae35cbe5aa", "e23cd4912c6eb697caf5b6c26a469ae6"}},
        std::pair{"candidate-v15", std::pair{"d575045a1a4df0af5d810e24b67620ca1b54b5fff5334f1593e6b96ca5ace736", "724d1cf3d3aa0835ca9af1205eb6b643"}},
        std::pair{"candidate-v14", std::pair{"8d7384268727e172016f5d870f4f5f81b109cbe8de7396d376064911c704b428", "b0c47530c9ad251ae40750fc4e938107"}},
        std::pair{"candidate-v13", std::pair{"a52e105a9466a68194b58a9335600391b768481922fe77d13f99066e28e4bf13", "2d23593ff062e38dc9354abb162442a0"}},
        std::pair{"candidate-v12", std::pair{"2d9bf2b079986b1ca1ebd1a2848e549dfee4143cd9af16290fb117c3bb910991", "724bd355038d22a139c443cae0312b39"}},
        std::pair{"candidate-v11", std::pair{"011a1c93851d928cd2887ef3c7c828fa333c12d85260a4e5c6fe55afc8b2da70", "ea9b63524facf0b1e8389b512ebb56cc"}},
        std::pair{"candidate-v10", std::pair{"388e3a656c7c49dfe072e23059fdf9ea58e8c86891d78f1dcf827c11722f95ea", "1bf3466892bef92c24132748dd8cacac"}},
        std::pair{"candidate-v9", std::pair{"e67f17e6e11643e3815e1ebf94c9406f2f9367a27e89d88e7f27ba46bdf2f7ba", "5d508990453e2670ad0d1babf3df8676"}},
        std::pair{"candidate-v8", std::pair{"d86f6194b4e73d51992796bfb6fe7d329738c462220c60382621e242fe50fffa", "f384e8a0d864e80f6d364161785d143e"}},
        std::pair{"candidate-v7", std::pair{"f0aac9661f61def02cb7599a70cdf76e58f0d1efb02a2b29db2d85588083153d", "f384e8a0d864e80f6d364161785d143e"}},
        std::pair{"candidate-v6", std::pair{"50c7fa8200c50053a9479bfba6df90bffd00cabed55a2fb1b514abc383d619ad", "00773c38af2a9d2816a16aec3ca51aae"}},
        std::pair{"candidate-v5", std::pair{"0ae658405a180479f467d356eefd646f4381f28df23178e7fc4e854d6b01d2c6", "00773c38af2a9d2816a16aec3ca51aae"}},
        std::pair{"candidate-v4", std::pair{"b007aa549da452c134c75029e21474d6791458d2b54065bdf8a3c7f0dca69ba7", "00773c38af2a9d2816a16aec3ca51aae"}},
        std::pair{"candidate-v3", std::pair{"42dc94fb052b32c2594dad69f8b3902bf65ebecbb8de984a43748eb84eb8b9f2", "9df5351c987492bf45f944354aee6c1b"}},
        std::pair{"candidate-v2", std::pair{"d510c85e888010a7ba405f6638b88f30d07df8861f63c97b724ff024ad22daf3", "b0df2f19ada5737e613363d4cb06618b"}},
        std::pair{"candidate-v1", std::pair{"14774478b60a22ed102f2a6d51319947146b27a52895f2baa65edfda637b28d1", "2945adb2dcad1d56804f5f4d88da2251"}}};
    for (const auto& [name, values] : historical) {
        auto pin = candidate_pin(std::string(name), std::string(values.first), std::string(values.second));
        pin.flash_sha256 = pin.binary_sha256;
        result.push_back(std::move(pin));
    }
    return result;
}

std::vector<std::string> known_prefixes() {
    std::vector<std::string> result;
    for (const auto& pin : known_pairs()) {
        if (std::find(result.begin(), result.end(), pin.prefix_md5) == result.end()) {
            result.push_back(pin.prefix_md5);
        }
    }
    return result;
}

std::vector<std::string> known_flash_hashes() {
    std::vector<std::string> result;
    for (const auto& pin : known_pairs()) {
        if (std::find(result.begin(), result.end(), pin.flash_sha256) == result.end()) {
            result.push_back(pin.flash_sha256);
        }
    }
    return result;
}

std::string classify_pair(std::span<const std::uint8_t> binary, std::span<const std::uint8_t> sidecar) {
    const auto binary_hash = sha256(binary);
    const auto sidecar_hash = sha256(sidecar);
    const auto pins = known_pairs();
    for (const auto& pin : pins) {
        if (pin.binary_sha256 == binary_hash && pin.sidecar_sha256 == sidecar_hash) {
            return pin.name;
        }
    }
    std::string binary_name;
    std::string sidecar_name;
    for (const auto& pin : pins) {
        if (pin.binary_sha256 == binary_hash) binary_name = pin.name;
        if (pin.sidecar_sha256 == sidecar_hash) sidecar_name = pin.name;
    }
    if (binary_name.empty() || sidecar_name.empty()) return "unknown";
    if (binary_name == sidecar_name) return binary_name;
    return "crossed:" + binary_name + "+" + sidecar_name;
}

fs::path find_root(const std::optional<fs::path>& requested) {
    const auto is_source_root = [](const fs::path& root) {
        return fs::is_regular_file(root / "CMakeLists.txt") &&
               fs::is_regular_file(root / "firmware/frame_stream.cpp") &&
               fs::is_regular_file(root / "firmware/indexed_stream.cpp");
    };
    if (requested.has_value()) {
        const auto root = fs::absolute(*requested);
        if (!is_source_root(root)) {
            throw Failure("requested firmware root is not a repository: " + root.string());
        }
        return root;
    }
    auto current = fs::current_path();
    while (true) {
        if (is_source_root(current)) {
            return current;
        }
        if (!current.has_parent_path() || current == current.parent_path()) break;
        current = current.parent_path();
    }
    throw Failure("unable to locate the custom udp lcd source root; pass --root");
}

std::string read_source_name(const fs::path& firmware_root, std::string_view stem) {
    const auto cpp = firmware_root / (std::string(stem) + ".cpp");
    if (fs::exists(cpp)) {
        auto relative = fs::path(stem);
        relative += ".cpp";
        return relative.generic_string();
    }
    throw Failure("missing current C++ firmware source: " + cpp.string());
}

fs::path source_path(const fs::path& firmware_root, std::string_view name) {
    const auto selected = firmware_root / std::string(name);
    if (!fs::exists(selected)) {
        throw Failure("missing firmware source: " + selected.string());
    }
    return selected;
}

struct CompiledPayload {
    Bytes payload;
    std::map<std::string, std::uint32_t> symbols;
    std::vector<std::string> source_names;
    std::map<std::string, std::string> source_hashes;
    std::string source_sha256;
    std::string payload_sha256;
    std::uint32_t max_stack = 0;
    std::vector<std::string> stack_usage;
};

Bytes compile_thumb_source(const fs::path& directory, std::string_view source, std::string_view stem,
                           std::size_t maximum) {
    const auto assembly_path = directory / (std::string(stem) + ".S");
    const auto object_path = directory / (std::string(stem) + ".o");
    write_file(assembly_path, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(source.data()), source.size()));
    const auto clang = find_executable("clang");
    const auto result = run_process({clang, "--target=arm-none-eabi", "-mcpu=cortex-m3", "-mthumb", "-c",
                                     assembly_path.string(), "-o", object_path.string()}, directory);
    if (result.code != 0) {
        throw Failure("Thumb assembly failed: " + result.errors);
    }
    return parse_object(read_bounded(object_path, 16 * 1024 * 1024), ".text", maximum).text;
}

std::uint32_t decode_branch(std::uint32_t source, std::span<const std::uint8_t> encoded) {
    if (encoded.size() != 4) throw Failure("Thumb branch requires four bytes");
    const auto first = load16(encoded, 0);
    const auto second = load16(encoded, 2);
    if ((first & 0xf800U) != 0xf000U || (second & 0xd000U) != 0x9000U) {
        throw Failure("unexpected Thumb branch encoding: " + hex_digest(encoded));
    }
    const auto sign = (first >> 10) & 1U;
    const auto first_bit = 1U ^ ((second >> 13) & 1U) ^ sign;
    const auto second_bit = 1U ^ ((second >> 11) & 1U) ^ sign;
    std::int32_t displacement = static_cast<std::int32_t>((sign << 24) | (first_bit << 23) | (second_bit << 22) |
                                                           ((first & 0x3ffU) << 12) | ((second & 0x7ffU) << 1));
    if (sign != 0) displacement -= 1 << 25;
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(source) + 4 + displacement);
}

Bytes make_branch(const fs::path& directory, std::uint32_t source, std::uint32_t target, std::string_view stem) {
    if ((source & 1U) != 0 || (target & 1U) != 0) throw Failure("Thumb branch addresses must be halfword aligned");
    const auto assembly = ".syntax unified\n.cpu cortex-m3\n.thumb\n.section .text,\"ax\",%progbits\nb.w . + (" +
                          format_address(target) + " - " + format_address(source) + ")\n";
    const auto encoded = compile_thumb_source(directory, assembly, stem, 4);
    if (encoded.size() != 4 || decode_branch(source, encoded) != target) {
        throw Failure("Thumb branch target did not round trip for " + std::string(stem) + " bytes=" + hex_digest(encoded));
    }
    return encoded;
}

Bytes make_call(const fs::path& directory, std::uint32_t source, std::uint32_t target, std::string_view stem) {
    if ((source & 1U) != 0 || (target & 1U) != 0) throw Failure("Thumb call addresses must be halfword aligned");
    const auto assembly = ".syntax unified\n.cpu cortex-m3\n.thumb\n.section .text,\"ax\",%progbits\nbl . + (" +
                          format_address(target) + " - " + format_address(source) + ")\n";
    const auto encoded = compile_thumb_source(directory, assembly, stem, 4);
    const auto first = load16(encoded, 0);
    const auto second = load16(encoded, 2);
    if (encoded.size() != 4 || (first & 0xf800U) != 0xf000U || (second & 0xd000U) != 0xd000U) {
        throw Failure("unexpected Thumb call encoding");
    }
    Bytes branch = encoded;
    branch[3] = static_cast<std::uint8_t>(branch[3] & ~0x40U);
    if (decode_branch(source, branch) != target) throw Failure("Thumb call target did not round trip");
    return encoded;
}

std::vector<std::string> indexed_source_names(const fs::path& firmware_root) {
    return {
        read_source_name(firmware_root, "frame_stream"),
        "frame_stream.h",
        "frame_stream.S",
        read_source_name(firmware_root, "jpeg_frame"),
        "jpeg_frame.h",
        read_source_name(firmware_root, "vendor/tjpgd/tjpgd"),
        "vendor/tjpgd/tjpgd.h",
        "vendor/tjpgd/tjpgdcnf.h",
        read_source_name(firmware_root, "indexed_stream"),
        "indexed_stream.h",
        read_source_name(firmware_root, "indexed_target"),
        "indexed_target.h",
        "indexed_target.S",
        read_source_name(firmware_root, "indexed_heap"),
        "indexed_heap.h"};
}

std::vector<std::string> indexed_compile_names(const fs::path& firmware_root) {
    return {
        read_source_name(firmware_root, "frame_stream"),
        "frame_stream.S",
        read_source_name(firmware_root, "jpeg_frame"),
        read_source_name(firmware_root, "vendor/tjpgd/tjpgd"),
        read_source_name(firmware_root, "indexed_stream"),
        read_source_name(firmware_root, "indexed_target"),
        "indexed_target.S",
        read_source_name(firmware_root, "indexed_heap")};
}

CompiledPayload compile_indexed_payload(const fs::path& root, const fs::path& directory) {
    const auto firmware_root = root / "firmware";
    const auto compile_names = indexed_compile_names(firmware_root);
    const auto all_names = indexed_source_names(firmware_root);
    const auto clang_c = find_executable("clang");
    const auto clang_cpp = find_executable("clang++");
    const std::vector<std::string> common_flags{
        "--target=arm-none-eabi", "-mcpu=cortex-m3", "-mthumb", "-ffreestanding", "-fno-builtin", "-fwrapv",
        "-O3", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-ffunction-sections", "-fdata-sections",
        "-fstack-usage", "-Wall", "-Wextra", "-Werror", "-DFRAME_STREAM_INDEXED"};
    std::vector<fs::path> objects;
    for (std::size_t index = 0; index < compile_names.size(); ++index) {
        const auto name = compile_names[index];
        const auto source = source_path(firmware_root, name);
        const auto object = directory / (std::to_string(index) + "_" + name.substr(name.find_last_of('/') + 1) + ".o");
        auto flags = common_flags;
        std::string compiler = clang_c;
        const auto source_extension = source.extension().string();
        fs::path compile_source = source;
        if (source_extension == ".cpp") {
            compiler = clang_cpp;
            const auto wrapper = directory / (std::to_string(index) + "_" + name.substr(name.find_last_of('/') + 1) + ".wrapper.cpp");
            const auto absolute_source = fs::absolute(source).string();
            const auto wrapper_text = std::string("extern \"C\" {\n#include \"") + absolute_source + "\"\n}\n";
            write_file(wrapper, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(wrapper_text.data()), wrapper_text.size()));
            compile_source = wrapper;
            flags.push_back("-std=c++23");
            flags.push_back("-fno-exceptions");
            flags.push_back("-fno-rtti");
        }
        std::vector<std::string> command{compiler};
        command.insert(command.end(), flags.begin(), flags.end());
        command.insert(command.end(), {"-c", compile_source.string(), "-o", object.string()});
        const auto result = run_process(command, directory, 4 * 1024 * 1024);
        if (result.code != 0) {
            throw Failure("ARM compilation failed for " + source.string() + ": " + result.errors);
        }
        objects.push_back(object);
    }
    const auto linker_script = directory / "payload.ld";
    const auto script = "ENTRY(stream_native_entry)\nSECTIONS { . = 0x080E5000; .text : { *(.text*) *(.rodata*) } /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) } }\n";
    write_file(linker_script, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(script), std::strlen(script)));
    const auto elf = directory / "payload.elf";
    std::vector<std::string> link_command{find_linker(), "-T", linker_script.string(), "--no-undefined", "--nmagic"};
    for (const auto& object : objects) link_command.push_back(object.string());
    link_command.insert(link_command.end(), {"-o", elf.string()});
    const auto link_result = run_process(link_command, directory, 4 * 1024 * 1024);
    if (link_result.code != 0) {
        throw Failure("ARM link failed: " + link_result.errors);
    }
    const std::set<std::string> required{
        "stream_native_entry", "stream_control_entry", "stream_dispose_entry", "stream_clear_entry",
        "frame_stream_control", "frame_stream_paint", "frame_stream_dispose", "frame_stream_has_full_frame",
        "frame_stream_note_native", "fs_clock_ms", "fs_blit_rgb565", "fs_raw_bitmap_selector", "fs_read_paint_clip",
        "indexed_current_handle", "indexed_binary_entry", "indexed_binary_feed_entry", "indexed_color_dispose_entry",
        "indexed_target_control", "indexed_target_paint", "indexed_target_has_canvas", "indexed_target_dispose",
        "indexed_target_binary_begin", "indexed_target_binary_feed", "indexed_native_invalidate", "indexed_comm_idle_entry",
        "indexed_target_idle_ticks", "indexed_gui_ready", "indexed_native_clip", "indexed_gui_lock_entry"};
    const auto linked = parse_linked(read_bounded(elf, 16 * 1024 * 1024), extension, max_payload, required);
    CompiledPayload result;
    result.payload = linked.text;
    result.symbols = linked.symbols;
    result.source_names = all_names;
    Bytes source_bytes;
    for (const auto& name : all_names) {
        const auto content = read_bounded(source_path(firmware_root, name), 16 * 1024 * 1024);
        result.source_hashes[name] = sha256(content);
        const auto name_bytes = bytes_of(name + std::string(1, '\0'));
        source_bytes.insert(source_bytes.end(), name_bytes.begin(), name_bytes.end());
        source_bytes.insert(source_bytes.end(), content.begin(), content.end());
    }
    result.source_sha256 = sha256(source_bytes);
    result.payload_sha256 = sha256(result.payload);
    std::vector<fs::path> usage_files;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".su") usage_files.push_back(entry.path());
    }
    std::sort(usage_files.begin(), usage_files.end());
    for (std::size_t usage_index = 0; usage_index < usage_files.size(); ++usage_index) {
        const auto& usage_file = usage_files[usage_index];
        if (usage_index != 0) result.stack_usage.emplace_back();
        const auto text = read_text_bounded(usage_file, 1024 * 1024);
        std::size_t begin = 0;
        while (begin <= text.size()) {
            const auto end = text.find('\n', begin);
            const auto line = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            if (!line.empty()) {
                auto normalized_line = line;
                const auto firmware_marker = normalized_line.find("/firmware/");
                if (firmware_marker != std::string::npos) normalized_line = normalized_line.substr(firmware_marker + 1);
                result.stack_usage.push_back(normalized_line);
                const auto first = normalized_line.find('\t');
                const auto second = first == std::string::npos ? std::string::npos : normalized_line.find('\t', first + 1);
                if (first != std::string::npos) {
                    const auto frame_text = normalized_line.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
                    try {
                        result.max_stack = std::max(result.max_stack, static_cast<std::uint32_t>(std::stoul(frame_text)));
                    } catch (const std::exception&) {
                        throw Failure("invalid compiler stack usage record");
                    }
                }
            }
            if (end == std::string::npos) break;
            begin = end + 1;
        }
    }
    if (result.stack_usage.empty() || result.max_stack > 384) {
        throw Failure("unexpected compiler stack frame");
    }
    return result;
}

struct PatchRecord {
    std::uint32_t address = 0;
    Bytes expected;
    Bytes replacement;
    std::string purpose;
};

void apply_patch(Bytes& image, const Bytes& base, std::vector<std::uint8_t>& covered, const PatchRecord& patch) {
    if (patch.expected.size() != patch.replacement.size() || patch.expected.empty() || patch.address < flash_base) {
        throw Failure("invalid firmware patch: " + patch.purpose);
    }
    const auto offset = static_cast<std::size_t>(patch.address - flash_base);
    if (offset + patch.expected.size() > image.size() || offset + patch.expected.size() > covered.size() ||
        (offset < hashed_size + 8 && offset + patch.expected.size() > hashed_size)) {
        throw Failure("firmware patch is out of bounds: " + patch.purpose);
    }
    const auto begin = static_cast<Bytes::difference_type>(offset);
    const auto end = begin + static_cast<Bytes::difference_type>(patch.expected.size());
    if (std::any_of(covered.begin() + begin, covered.begin() + end, [](auto value) { return value != 0; })) {
        throw Failure("overlapping firmware patch: " + patch.purpose);
    }
    if (!std::equal(patch.expected.begin(), patch.expected.end(), base.begin() + begin)) {
        throw Failure("unexpected stock bytes at " + format_address(patch.address));
    }
    std::copy(patch.replacement.begin(), patch.replacement.end(), image.begin() + begin);
    std::fill(covered.begin() + begin, covered.begin() + end, static_cast<std::uint8_t>(1));
}

std::string patch_json(const PatchRecord& patch) {
    std::ostringstream output;
    output << "{\"address\":" << json_escape(format_address(patch.address))
           << ",\"expected_sha256\":" << json_escape(sha256(patch.expected))
           << ",\"offset\":" << (patch.address - flash_base)
           << ",\"purpose\":" << json_escape(patch.purpose)
           << ",\"replacement_sha256\":" << json_escape(sha256(patch.replacement))
           << ",\"size\":" << patch.replacement.size() << '}';
    return output.str();
}

std::string map_json(const std::map<std::string, std::string>& values) {
    std::ostringstream output;
    output << '{';
    bool first = true;
    for (const auto& [key, value] : values) {
        if (!first) output << ',';
        first = false;
        output << json_escape(key) << ':' << json_escape(value);
    }
    output << '}';
    return output.str();
}

std::string payload_json(const CompiledPayload& payload) {
    std::ostringstream output;
    output << "{\"max_compiler_stack_frame\":" << payload.max_stack
           << ",\"optimization\":\"O3\",\"sha256\":" << json_escape(payload.payload_sha256)
           << ",\"size\":" << payload.payload.size() << ",\"source_sha256\":"
           << json_escape(payload.source_sha256) << ",\"sources\":" << map_json(payload.source_hashes)
           << ",\"stack_usage\":[";
    for (std::size_t index = 0; index < payload.stack_usage.size(); ++index) {
        if (index != 0) output << ',';
        output << json_escape(payload.stack_usage[index]);
    }
    output << "],\"symbols\":{";
    bool first = true;
    for (const auto& [name, address] : payload.symbols) {
        if (!first) output << ',';
        first = false;
        output << json_escape(name) << ':' << address;
    }
    output << "}}";
    return output.str();
}

Bytes hex_bytes(std::string_view value) {
    if (value.size() % 2 != 0) throw Failure("odd hexadecimal byte string");
    Bytes result;
    result.reserve(value.size() / 2);
    for (std::size_t index = 0; index < value.size(); index += 2) {
        const auto convert = [](char digit) -> std::uint8_t {
            if (digit >= '0' && digit <= '9') return static_cast<std::uint8_t>(digit - '0');
            if (digit >= 'a' && digit <= 'f') return static_cast<std::uint8_t>(digit - 'a' + 10);
            if (digit >= 'A' && digit <= 'F') return static_cast<std::uint8_t>(digit - 'A' + 10);
            throw Failure("invalid hexadecimal byte string");
        };
        result.push_back(static_cast<std::uint8_t>((convert(value[index]) << 4) | convert(value[index + 1])));
    }
    return result;
}

void validate_base_image(const Bytes& base) {
    if (base.size() != flash_size || sha256(base) != base_sha256) {
        throw Failure("base image does not match the reconstructed live readback");
    }
    const auto footer = hex_bytes(footer_hex);
    if (!std::equal(footer.begin(), footer.end(), base.begin() + footer_offset)) {
        throw Failure("device System ID record does not match");
    }
    if (!std::all_of(base.begin() + 0xE4FA0, base.end(), [](auto value) { return value == 0xff; })) {
        throw Failure("proposed extension range is not erased");
    }
}

void validate_stock_package(const Bytes& stock) {
    if (stock.size() != stock_package_size || sha256(stock) != stock_package_sha256) {
        throw Failure("stock image does not match the pinned UDM Pro stock package; provide the exact stock.bin input");
    }
    if (!std::all_of(stock.begin() + footer_offset, stock.end(), [](auto value) { return value == 0xff; })) {
        throw Failure("stock image footer is not erased");
    }
}

Bytes reconstruct_base_image(const Bytes& stock) {
    validate_stock_package(stock);
    Bytes base(flash_size, 0xff);
    std::copy(stock.begin(), stock.end(), base.begin());
    const auto footer = hex_bytes(footer_hex);
    std::copy(footer.begin(), footer.end(), base.begin() + footer_offset);
    validate_base_image(base);
    return base;
}

struct CandidateBuild {
    Bytes image;
    Bytes payload;
    std::string manifest;
};

CandidateBuild build_indexed_candidate(const fs::path& root, const Bytes& base, const fs::path& directory) {
    validate_base_image(base);
    const auto compiled = compile_indexed_payload(root, directory);
    if (compiled.payload.size() > max_payload) throw Failure("payload exceeds erased tail");
    std::vector<PatchRecord> patches{
        {0x0800D1CAU, hex_bytes("c04724e0"), make_branch(directory, 0x0800D1CAU,
                                                          compiled.symbols.at("stream_native_entry"), "stream_native_entry_branch"), "stream_native_entry"},
        {0x08037268U, hex_bytes("30b50d46"), make_branch(directory, 0x08037268U,
                                                          compiled.symbols.at("stream_control_entry"), "stream_control_entry_branch"), "stream_control_entry"},
        {0x08037CA4U, hex_bytes("f8b50446"), make_branch(directory, 0x08037CA4U,
                                                          compiled.symbols.at("stream_dispose_entry"), "stream_dispose_entry_branch"), "stream_dispose_entry"},
        {0x0800D2D6U, hex_bytes("0cf06dfe"), make_branch(directory, 0x0800D2D6U,
                                                          compiled.symbols.at("stream_clear_entry"), "stream_clear_entry_branch"), "stream_clear_entry"},
        {0x0800D3A6U, hex_bytes("0024"), hex_bytes("0424"), "native owned pointer capacity"},
        {extension, Bytes(compiled.payload.size(), 0xff), compiled.payload, "runtime receiver payload"}};
    patches.push_back({0x0800B63AU, hex_bytes("0021"), hex_bytes("0121"), "GUI semaphore initially available"});
    patches.push_back({0x0800B660U, hex_bytes("024b0021"), make_branch(directory, 0x0800B660U,
                                                                       compiled.symbols.at("indexed_gui_lock_entry"), "indexed_gui_lock_branch"), "GUI lock successful semaphore acquisition"});
    Bytes binary_entry(4);
    const auto binary_address = compiled.symbols.at("indexed_binary_entry") | 1U;
    store32(binary_entry, binary_address);
    patches.push_back({0x080CEDF8U, hex_bytes("79b80208"), binary_entry, "indexed binary header"});
    Bytes binary_feed_entry(4);
    const auto binary_feed_address = compiled.symbols.at("indexed_binary_feed_entry") | 1U;
    store32(binary_feed_entry, binary_feed_address);
    patches.push_back({0x080CEDFCU, hex_bytes("71960208"), binary_feed_entry, "indexed binary body"});
    patches.push_back({0x08037224U, hex_bytes("10b50446"), make_branch(directory, 0x08037224U,
                                                                       compiled.symbols.at("indexed_color_dispose_entry"), "indexed_dispose_branch"), "test.lcd owned state cleanup"});
    patches.push_back({0x0802CFDCU, hex_bytes("d4f7c4ff"), make_call(directory, 0x0802CFDCU,
                                                                       compiled.symbols.at("indexed_comm_idle_entry"), "indexed_idle_branch"), "direct indexed owner communication idle polling"});
    Bytes image = base;
    std::vector<std::uint8_t> covered(image.size(), 0);
    for (const auto& patch : patches) apply_patch(image, base, covered, patch);
    for (std::size_t index = 0; index < image.size(); ++index) {
        if (image[index] != base[index] && covered[index] == 0) {
            throw Failure("unreviewed indexed difference");
        }
    }
    const auto footer = hex_bytes(footer_hex);
    if (!std::equal(footer.begin(), footer.end(), image.begin() + footer_offset)) {
        throw Failure("System ID record changed");
    }
    std::ostringstream manifest;
    manifest << "{\n"
             << "  \"base_sha256\":\"" << base_sha256 << "\",\n"
             << "  \"classification\":\"build_only_runtime_frame_candidate\",\n"
             << "  \"flash_approved\":false,\n"
             << "  \"limitations\":[\"Indexed palette quantization uses 256 colors per frame\",\"Incomplete progressive upload requires stop and normal UI restoration\",\"Paint acknowledgement confirms returned draw calls rather than physical scanout\",\"Physical performance and allocator availability require hardware measurement\"],\n"
             << "  \"patches\":[";
    for (std::size_t index = 0; index < patches.size(); ++index) {
        if (index != 0) manifest << ',';
        manifest << patch_json(patches[index]);
    }
    manifest << "],\n  \"payload\":" << payload_json(compiled)
             << ",\n  \"render_path\":\"router_decoded_indexed8_bulk_v1\""
             << ",\n  \"running_prefix_md5\":\"" << md5(std::span<const std::uint8_t>(image).first(hashed_size)) << "\""
             << ",\n  \"sha256\":\"" << sha256(image) << "\",\n"
             << "  \"size\":" << image.size() << ",\n"
             << "  \"stream\":"
             << R"({"band_bytes":1920,"band_rows":4,"bottom_bytes":33600,"bulk_input_block_cap":64,"direct_binary_frames":true,"direct_owner_fast_poll":true,"fast_poll_gui_ready_gate":true,"general_top_block_bytes":4096,"general_top_blocks":6,"general_top_last_bytes":3520,"gui_lock_ownership":true,"height":240,"lz4_blocks":true,"mcu_jpeg_decode":false,"native_context_clip":true,"native_index8_draw":true,"owner_screen":"test.lcd","paint_status_query":true,"palette_colors":256,"pixel_format":"INDEX8_RGB565_PALETTE","progressive_receive":true,"safe_heap_fit":true,"state_bytes":1252,"top_bytes":24000,"width":240,"wire_cap":58594})"
             << ",\n"
             << "  \"system_id_record_hex\":\"" << footer_hex << "\"\n}\n";
    return CandidateBuild{std::move(image), compiled.payload, manifest.str()};
}

struct ArtifactSummary {
    std::string candidate_hash;
    std::string candidate_prefix;
    std::string rollback_hash;
    std::string rollback_prefix;
    std::string stock_hash;
    std::string stock_sidecar_hash;
    std::size_t patch_count = 0;
};

void require_condition(bool condition, std::string_view message) {
    if (!condition) throw Failure(std::string(message));
}

void validate_manifest_stream(const Json::Object& stream) {
    const std::vector<std::pair<std::string_view, std::int64_t>> integers{
        {"band_bytes", 1920}, {"band_rows", 4}, {"bottom_bytes", 33600}, {"bulk_input_block_cap", 64},
        {"general_top_block_bytes", 4096}, {"general_top_blocks", 6}, {"general_top_last_bytes", 3520},
        {"height", 240}, {"palette_colors", 256}, {"state_bytes", 1252}, {"top_bytes", 24000},
        {"width", 240}, {"wire_cap", 58594}};
    for (const auto& [name, expected] : integers) {
        require_condition(integer_value(field(stream, name), name) == expected, "manifest stream geometry or capacity changed");
    }
    const std::vector<std::pair<std::string_view, bool>> booleans{
        {"direct_binary_frames", true}, {"direct_owner_fast_poll", true}, {"fast_poll_gui_ready_gate", true},
        {"gui_lock_ownership", true}, {"lz4_blocks", true}, {"mcu_jpeg_decode", false},
        {"native_context_clip", true}, {"native_index8_draw", true}, {"paint_status_query", true},
        {"progressive_receive", true}, {"safe_heap_fit", true}};
    for (const auto& [name, expected] : booleans) {
        require_condition(boolean_value(field(stream, name), name) == expected, "manifest stream geometry or capacity changed");
    }
    const std::vector<std::pair<std::string_view, std::string_view>> strings{
        {"owner_screen", "test.lcd"}, {"pixel_format", "INDEX8_RGB565_PALETTE"}};
    for (const auto& [name, expected] : strings) {
        require_condition(string_value(field(stream, name), name) == expected, "manifest stream geometry or capacity changed");
    }
    require_condition(stream.size() == integers.size() + booleans.size() + strings.size(),
                      "manifest stream geometry or capacity changed");
}

ArtifactSummary validate_artifacts(const fs::path& root) {
    const auto asset_root = root / "assets/firmware";
    if (!fs::is_directory(asset_root)) {
        throw Failure("firmware assets are missing; run firmware build --stock PATH --output assets/firmware");
    }
    const auto manifest_path = asset_root / "manifest.json";
    const auto candidate_path = asset_root / "candidate.bin";
    const auto base_path = asset_root / "base.bin";
    const auto rollback_path = asset_root / "rollback.bin";
    const auto stock_path = asset_root / "stock.bin";
    const auto sidecar_path = asset_root / "stock.hash";
    Json manifest;
    try {
        manifest = JsonParser(read_text_bounded(manifest_path, 128 * 1024)).parse();
    } catch (const Failure& error) {
        throw Failure("invalid candidate manifest: " + std::string(error.what()));
    }
    const auto& manifest_object = object(manifest, "candidate manifest");
    require_condition(has_string(manifest_object, "base_sha256", base_sha256), "manifest base_sha256 changed");
    require_condition(has_string(manifest_object, "classification", "build_only_runtime_frame_candidate"), "manifest classification changed");
    require_condition(!boolean_value(field(manifest_object, "flash_approved"), "flash_approved"), "manifest flash_approved changed");
    require_condition(has_string(manifest_object, "render_path", "router_decoded_indexed8_bulk_v1"), "manifest render_path changed");
    require_condition(has_string(manifest_object, "running_prefix_md5", candidate_prefix_md5), "manifest running_prefix_md5 changed");
    require_condition(has_string(manifest_object, "sha256", candidate_sha256), "manifest sha256 changed");
    require_condition(integer_value(field(manifest_object, "size"), "size") == flash_size, "manifest size changed");
    require_condition(has_string(manifest_object, "system_id_record_hex", footer_hex), "manifest footer changed");
    validate_manifest_stream(object(field(manifest_object, "stream"), "stream"));
    const auto& payload = object(field(manifest_object, "payload"), "payload");
    require_condition(has_string(payload, "source_sha256", source_sha256),
                      "manifest payload source SHA-256 changed");
    require_condition(has_string(payload, "optimization", "O3"), "manifest compiler optimization changed");
    const auto candidate = read_bounded(candidate_path, flash_size);
    const auto base = read_bounded(base_path, flash_size);
    const auto rollback = read_bounded(rollback_path, flash_size);
    const auto stock = read_bounded(stock_path, 2 * flash_size);
    const auto sidecar = fs::exists(sidecar_path) ? read_bounded(sidecar_path, 64) : bytes_of(base_prefix_md5);
    require_condition(candidate.size() == flash_size, "candidate length is not 1 MiB");
    require_condition(base.size() == flash_size, "rollback snapshot length is not 1 MiB");
    require_condition(rollback.size() == flash_size, "rollback artifact length is not 1 MiB");
    require_condition(stock.size() == stock_package_size, "stock package length changed");
    require_condition(sha256(candidate) == candidate_sha256, "candidate SHA-256 changed");
    require_condition(sha256(base) == base_sha256, "rollback snapshot SHA-256 changed");
    require_condition(sha256(rollback) == base_sha256, "rollback artifact SHA-256 changed");
    require_condition(rollback == base, "rollback artifact differs from base snapshot");
    require_condition(sha256(stock) == stock_package_sha256, "stock package SHA-256 changed");
    require_condition(sha256(sidecar) == stock_sidecar_sha256, "stock sidecar SHA-256 changed");
    require_condition(classify_pair(candidate, bytes_of(candidate_prefix_md5)) == "candidate", "candidate binary/sidecar pair changed");
    require_condition(classify_pair(stock, sidecar) == "stock", "stock binary/sidecar pair changed");
    require_condition(std::string(sidecar.begin(), sidecar.end()) == base_prefix_md5, "stock sidecar content changed");
    require_condition(std::equal(stock.begin(), stock.begin() + hashed_size, base.begin()), "stock package and rollback prefix differ");
    const auto footer = hex_bytes(footer_hex);
    require_condition(std::all_of(stock.begin() + footer_offset, stock.begin() + footer_offset + 8, [](auto value) { return value == 0xff; }),
                      "stock package footer is not erased");
    const auto footer_begin = static_cast<Bytes::difference_type>(footer_offset);
    const auto footer_end = footer_begin + static_cast<Bytes::difference_type>(footer.size());
    require_condition(std::equal(base.begin() + footer_begin, base.begin() + footer_end, footer.begin()), "rollback footer changed");
    require_condition(std::equal(candidate.begin() + footer_begin, candidate.begin() + footer_end, footer.begin()), "candidate footer changed");
    require_condition(std::all_of(base.begin() + 0xE4FA0U, base.end(), [](auto value) { return value == 0xff; }), "rollback physical tail is not erased");
    require_condition(md5(std::span<const std::uint8_t>(base).first(hashed_size)) == base_prefix_md5, "rollback prefix MD5 changed");
    require_condition(md5(std::span<const std::uint8_t>(candidate).first(hashed_size)) == candidate_prefix_md5, "candidate prefix MD5 changed");
    const auto& patches = array(field(manifest_object, "patches"), "patches");
    require_condition(!patches.empty(), "manifest patches missing");
    std::vector<std::uint8_t> covered(flash_size, 0);
    for (const auto& patch_value : patches) {
        const auto& patch = object(patch_value, "manifest patch");
        const auto offset = integer_value(field(patch, "offset"), "patch offset");
        const auto size = integer_value(field(patch, "size"), "patch size");
        require_condition(size > 0 && offset >= 0 && offset + size <= flash_size, "manifest patch range is invalid");
        const auto expected_address = format_address(flash_base + static_cast<std::uint32_t>(offset));
        require_condition(string_value(field(patch, "address"), "patch address") == expected_address, "patch address does not match offset");
        const auto begin = static_cast<std::size_t>(offset);
        const auto patch_size = static_cast<std::size_t>(size);
        const auto end = begin + patch_size;
        const auto iterator_begin = static_cast<Bytes::difference_type>(begin);
        const auto iterator_end = static_cast<Bytes::difference_type>(end);
        require_condition(sha256(std::span<const std::uint8_t>(base).subspan(begin, patch_size)) == string_value(field(patch, "expected_sha256"), "expected hash"),
                          "patch base bytes changed");
        require_condition(sha256(std::span<const std::uint8_t>(candidate).subspan(begin, patch_size)) == string_value(field(patch, "replacement_sha256"), "replacement hash"),
                          "patch replacement bytes changed");
        require_condition(!std::equal(base.begin() + iterator_begin, base.begin() + iterator_end,
                                      candidate.begin() + iterator_begin), "patch makes no change");
        require_condition(std::none_of(covered.begin() + iterator_begin, covered.begin() + iterator_end,
                                       [](auto value) { return value != 0; }), "patch ranges overlap");
        std::fill(covered.begin() + iterator_begin, covered.begin() + iterator_end, static_cast<std::uint8_t>(1));
    }
    for (std::size_t index = 0; index < candidate.size(); ++index) {
        if (candidate[index] != base[index] && covered[index] == 0) throw Failure("candidate differs outside manifest patches");
    }
    return ArtifactSummary{std::string(candidate_sha256), std::string(candidate_prefix_md5), std::string(base_sha256),
                           std::string(base_prefix_md5), std::string(stock_package_sha256), std::string(stock_sidecar_sha256), patches.size()};
}

std::string json_object(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::ostringstream output;
    output << '{';
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index != 0) output << ',';
        output << json_escape(fields[index].first) << ':' << fields[index].second;
    }
    output << '}';
    return output.str();
}

std::string dfu_command_json(std::string_view operation, std::string_view label) {
    const std::vector<std::string> values{
        "/usr/bin/dfu-util", "-d", "0483:df11", "-p", "1-2", "-c", "1", "-i", "0", "-S", "<transaction-pinned-serial>",
        "-a", "0", "-s", "0x08000000:0x100000", std::string(operation), std::string(label)};
    std::vector<std::string> encoded;
    encoded.reserve(values.size());
    for (const auto& value : values) encoded.push_back(json_escape(value));
    return join_json(encoded);
}

std::string make_step(std::string_view name, const std::vector<std::pair<std::string, std::string>>& fields = {}) {
    std::vector<std::pair<std::string, std::string>> values;
    values.emplace_back("step", json_escape(name));
    values.insert(values.end(), fields.begin(), fields.end());
    return json_object(values);
}

std::string dfu_identity_json() {
    return json_object({
        {"vid_pid", json_escape("0483:df11")}, {"physical_path", json_escape("1-2")}, {"configuration", "1"},
        {"interface", "0"}, {"protocol", json_escape("02")}, {"serial", json_escape("captured once from the target ROM DFU device and pinned for the transaction")},
        {"alternate", "0"}, {"map", json_escape("@Internal Flash  /0x08000000/04*016Kg,01*064Kg,07*128Kg")}});
}

std::string target_identity_json() {
    return json_object({
        {"system_id", json_escape("ea15")}, {"application_vid_pid", json_escape("1f9b:157d")},
        {"physical_path", json_escape("1-2")}, {"tty", json_escape("/dev/ttyACM0")},
        {"service", json_escape("ulcmd.service")}, {"service_executable", json_escape("/usr/bin/ulcmd")},
        {"service_sha256", json_escape("5c1f0dc8e6673d0d5095e149d86b55c8b082adb94112feb342bbca7a0af096db")},
        {"dfu_util", json_escape("/usr/bin/dfu-util")},
        {"dfu_util_sha256", json_escape("3c71fe929becb030481865e08d53c991807937bd08e65f7f46cf8132a736975b")},
        {"service_unit_sha256", json_escape("1273fa681ff355ae16b20798bbb9eb8fd09ab534a22b5240a49f246c8bd51b69")},
        {"controls", join_json({json_escape("/sys/devices/platform/soc/soc:ulcm/dfu_or_reset"),
                                  json_escape("/sys/devices/platform/ulcm/dfu_or_reset")})}});
}

std::string plan_json(std::string_view mode) {
    require_condition(mode == "install" || mode == "restore", "invalid plan mode");
    const bool install = mode == "install";
    const auto pins = known_pairs();
    std::vector<std::string> accepted_pairs;
    for (const auto& pin : pins) {
        accepted_pairs.push_back(json_escape("exact-" + pin.name));
    }
    if (!install) {
        for (const auto& binary : pins) {
            for (const auto& sidecar : pins) {
                if (binary.name != sidecar.name) {
                    accepted_pairs.push_back(json_escape("crossed-" + binary.name + "-bin+" + sidecar.name + "-hash"));
                }
            }
        }
    }
    std::vector<std::string> accepted_prefixes;
    for (const auto& prefix : known_prefixes()) accepted_prefixes.push_back(json_escape(prefix));
    if (!install) accepted_prefixes.push_back(json_escape("unavailable-in-exact-rom-dfu"));
    const auto target_sha = install ? candidate_sha256 : base_sha256;
    const auto target_md5 = install ? candidate_prefix_md5 : base_prefix_md5;
    const auto target_image = install ? "private:pinned-candidate-1MiB" : "private:pinned-reconstructed-rollback-1MiB";
    const auto target_pair = install ? "candidate-1MiB+candidate-prefix-md5" : "original-stock-package+original-stock-sidecar";
    std::vector<std::string> live_states;
    if (install) {
        live_states.push_back(json_object({{"transport", json_escape("application")}, {"running_prefix_md5", json_escape(base_prefix_md5)}, {"root_pair", json_escape("exact-stock")}}));
        live_states.push_back(json_object({{"transport", json_escape("application")}, {"running_prefix_md5", json_escape(candidate_prefix_md5)}, {"root_pair", json_escape("exact-candidate")}}));
        for (const auto& pin : pins) {
            if (pin.name != "stock" && pin.name != "candidate") {
                live_states.push_back(json_object({{"transport", json_escape("application")}, {"running_prefix_md5", json_escape(pin.prefix_md5)}, {"root_pair", json_escape("exact-" + pin.name)}}));
            }
        }
    } else {
        live_states.push_back(json_object({{"transport", json_escape("application")}, {"running_prefix_md5", json_escape("pinned-known-prefix-md5")}, {"root_pair", json_escape("any-pinned-exact-or-crossed-pair")}}));
        live_states.push_back(json_object({{"transport", json_escape("exact-rom-dfu")}, {"service", json_escape("inactive")}, {"tty_owner", json_escape("none")}, {"root_pair", json_escape("any-pinned-exact-or-crossed-pair")}}));
    }
    std::vector<std::string> steps;
    std::vector<std::string> accepted_hashes;
    for (const auto& value : known_flash_hashes()) accepted_hashes.push_back(json_escape(value));
    steps.push_back(make_step("validate_local_pins", {{"mutation", "false"}}));
    steps.push_back(make_step("validate_target_identity_and_tool_hashes", {{"mutation", "false"}, {"accepted_running_prefix_md5", join_json(accepted_prefixes)}}));
    steps.push_back(make_step("validate_root_firmware_pair", {{"mutation", "false"}, {"accepted_pairs", join_json(accepted_pairs)}}));
    steps.push_back(make_step("classify_live_source_state", {{"mutation", "false"}, {"accepted_states", join_json(live_states)}, {"requirement", json_escape(install ? "install rejects crossed or unknown states before mutation" : "restore accepts only fully pinned exact/crossed pairs and exact application or stopped ROM-DFU transport")}}));
    steps.push_back(make_step("create_private_transaction_directory", {{"mutation", "true"}, {"requirement", json_escape("root-owned mode 0700 on the target filesystem")}}));
    steps.push_back(make_step("stage_and_rehash_pinned_inputs", {{"mutation", "true"}, {"files", join_json({json_escape("private:pinned-base-1MiB"), json_escape("private:pinned-candidate-1MiB"), json_escape("private:pinned-reconstructed-rollback-1MiB"), json_escape("private:pinned-stock-package"), json_escape("private:pinned-stock-sidecar"), json_escape("private:pinned-candidate-sidecar"), json_escape("private:canonical-manifest")})}}));
    steps.push_back(make_step("preserve_original_root_files", {{"mutation", "true"}, {"requirement", json_escape("fresh private copies with SHA-256 and mode metadata; never overwrite them")}}));
    steps.push_back(make_step("stop_service", {{"mutation", "true"}, {"service", json_escape("ulcmd.service")}, {"when", json_escape("application transport; skipped for stopped exact ROM-DFU restore")}}));
    steps.push_back(make_step("verify_tty_unowned", {{"mutation", "false"}, {"tty", json_escape("/dev/ttyACM0")}, {"when", json_escape("after service stop or in initial exact ROM-DFU restore state")}}));
    steps.push_back(make_step("enter_dfu", {{"mutation", "true"}, {"value", json_escape("1")}, {"controls", join_json({json_escape("/sys/devices/platform/soc/soc:ulcm/dfu_or_reset"), json_escape("/sys/devices/platform/ulcm/dfu_or_reset")})}, {"when", json_escape("application transport; skipped for stopped exact ROM-DFU restore")}}));
    steps.push_back(make_step("verify_exact_dfu_identity", {{"mutation", "false"}, {"identity", dfu_identity_json()}}));
    steps.push_back(make_step("read_prewrite_flash", {{"mutation", "false"}, {"command", dfu_command_json("-U", "private:fresh-prewrite-readback")}}));
    steps.push_back(make_step("classify_prewrite_flash", {{"mutation", "false"}, {"accepted_sha256", install ? join_json(accepted_hashes) : "[\"evidence-only-any-sha256\"]"}, {"requirement", json_escape(install ? "install requires an exact known 1 MiB image" : "restore records the exact 1 MiB prewrite SHA as evidence and permits unknown or partial flash")}}));
    steps.push_back(make_step("download_target", {{"mutation", "true"}, {"command", dfu_command_json("-D", target_image)}, {"sha256", json_escape(target_sha)}}));
    steps.push_back(make_step("refresh_exact_dfu_identity", {{"mutation", "false"}, {"identity", dfu_identity_json()}}));
    steps.push_back(make_step("read_postwrite_flash", {{"mutation", "false"}, {"command", dfu_command_json("-U", "private:fresh-postwrite-readback")}}));
    steps.push_back(make_step("verify_postwrite_flash", {{"mutation", "false"}, {"size", std::to_string(flash_size)}, {"sha256", json_escape(target_sha)}, {"footer_hex", json_escape(footer_hex)}}));
    steps.push_back(make_step("publish_root_firmware_pair_while_service_stopped", {{"mutation", "true"}, {"pair", json_escape(target_pair)}, {"prefix_md5", json_escape(target_md5)}, {"requirement", json_escape("fresh per-file rename after fsync; preserved originals remain private")}}));
    steps.push_back(make_step("exit_dfu", {{"mutation", "true"}, {"value", json_escape("0")}, {"same_control_as_entry", "true"}}));
    steps.push_back(make_step("wait_for_application_identity", {{"mutation", "false"}, {"vid_pid", json_escape("1f9b:157d")}, {"physical_path", json_escape("1-2")}, {"tty", json_escape("/dev/ttyACM0")}}));
    steps.push_back(make_step("restart_service", {{"mutation", "true"}, {"service", json_escape("ulcmd.service")}}));
    steps.push_back(make_step("verify_service_owner_and_running_prefix", {{"mutation", "false"}, {"executable", json_escape("/usr/bin/ulcmd")}, {"tty", json_escape("/dev/ttyACM0")}, {"prefix_md5", json_escape(target_md5)}}));
    const auto rollback = join_json({
        make_step("single_recovery_download", {{"mutation", "true"}, {"attempts", "1"}, {"command", dfu_command_json("-D", "private:pinned-reconstructed-rollback-1MiB")}, {"sha256", json_escape(base_sha256)}}),
        make_step("recovery_readback", {{"mutation", "false"}, {"command", dfu_command_json("-U", "private:fresh-recovery-readback")}}),
        make_step("verify_recovery_readback", {{"mutation", "false"}, {"size", std::to_string(flash_size)}, {"sha256", json_escape(base_sha256)}, {"footer_hex", json_escape(footer_hex)}}),
        make_step("publish_pinned_stock_root_pair", {{"mutation", "true"}, {"package_sha256", json_escape(stock_package_sha256)}, {"sidecar_sha256", json_escape(stock_sidecar_sha256)}, {"requirement", json_escape("only after exact recovery readback; preserved pre-transaction files remain private")}}),
        make_step("exit_dfu_wait_restart_verify", {{"mutation", "true"}, {"requirement", json_escape("only after exact recovery readback and exact stock pair")}})});
    return json_object({
        {"mode", json_escape(mode)}, {"steps", join_json(steps)},
        {"on_failure_before_download_started", join_json({
            make_step("exit_dfu_if_entry_was_attempted", {{"mutation", "true"}, {"value", json_escape("0")}, {"same_control_as_entry", "true"}}),
            make_step("wait_for_exact_application_and_tty", {{"mutation", "false"}, {"vid_pid", json_escape("1f9b:157d")}, {"physical_path", json_escape("1-2")}, {"tty", json_escape("/dev/ttyACM0")}}),
            make_step("restart_service_only_after_application_and_tty", {{"mutation", "true"}, {"service", json_escape("ulcmd.service")}}),
            make_step("otherwise_leave_service_stopped", {{"mutation", "false"}})})},
        {"on_failure_after_download_started", rollback},
        {"if_recovery_unverified", json_object({{"firmware_status", json_escape("unknown_or_partial")}, {"recovery_status", json_escape("failed_after_one_attempt")}, {"required_state", json_escape("leave MCU in DFU and ulcmd.service stopped")}})},
        {"forbidden_dfu_options", join_json({json_escape("-R"), json_escape("--reset"), json_escape(":leave"), json_escape("unprotect")})}});
}

std::string artifact_json(const ArtifactSummary& summary) {
    return json_object({
        {"manifest", json_escape("assets/firmware/manifest.json")},
        {"base", json_object({{"size", std::to_string(flash_size)}, {"sha256", json_escape(base_sha256)}, {"prefix_md5", json_escape(base_prefix_md5)}})},
        {"candidate", json_object({{"size", std::to_string(flash_size)}, {"sha256", json_escape(summary.candidate_hash)}, {"prefix_md5", json_escape(summary.candidate_prefix)}})},
        {"rollback", json_object({{"size", std::to_string(flash_size)}, {"sha256", json_escape(summary.rollback_hash)}, {"prefix_md5", json_escape(summary.rollback_prefix)}, {"provenance", json_escape("reconstructed")}})},
        {"stock_package", json_object({{"size", std::to_string(stock_package_size)}, {"sha256", json_escape(summary.stock_hash)}})},
        {"stock_sidecar", json_object({{"size", "32"}, {"sha256", json_escape(summary.stock_sidecar_hash)}, {"content", json_escape(std::string(base_prefix_md5))}})},
        {"footer_hex", json_escape(footer_hex)}, {"patch_count", std::to_string(summary.patch_count)}, {"manifest_flash_approved", "false"}});
}

std::string offline_report(std::string_view mode, const ArtifactSummary& summary, bool include_plan) {
    std::vector<std::pair<std::string, std::string>> values{
        {"schema", "1"}, {"operation", json_escape("dry-run")}, {"requested_plan", json_escape(mode)},
        {"live_operations_enabled", "true"}, {"firmware_status", json_escape("offline_artifacts_verified")},
        {"recovery_status", json_escape("offline_rollback_artifact_verified")}, {"artifacts", artifact_json(summary)},
        {"target_identity", target_identity_json()}, {"dfu_identity", dfu_identity_json()},
        {"execution_boundary", json_escape("this command validates and plans only; explicit live execution uses the native C++ target controller through firmware --host --live or firmware --target --bundle-dir")},
        {"hardware_uncertainties", join_json({json_escape("the encoded-swatches candidate has not booted or been visually observed"), json_escape("the exact cause of a grayscale-looking photo remains unresolved despite colored hardcoded bars"), json_escape("direct per-pixel GUI API latency has not been measured on hardware"), json_escape("failed-write rollback and interrupted-power recovery have not been exercised")})}};
    if (include_plan) values.emplace_back("plan", plan_json(mode));
    return json_object(values);
}

std::string bundle_manifest_json(const std::map<std::string, std::pair<std::size_t, std::string>>& files) {
    std::ostringstream output;
    output << "{\"classification\":\"offline-static-frame-apply-bundle\",\"files\":{";
    bool first = true;
    for (const auto& [name, details] : files) {
        if (!first) output << ',';
        first = false;
        output << json_escape(name) << ":{\"sha256\":" << json_escape(details.second) << ",\"size\":" << details.first << '}';
    }
    output << "},\"schema\":1}\n";
    return output.str();
}

std::string prepare_bundle(const fs::path& root, const fs::path& destination) {
    const auto summary = validate_artifacts(root);
    if (fs::exists(destination)) throw Failure("bundle destination already exists");
    if (!fs::create_directory(destination)) throw Failure("unable to create bundle destination");
    ::chmod(destination.c_str(), 0700);
    const std::map<std::string, fs::path> sources{
        {"base.bin", root / "assets/firmware/base.bin"},
        {"candidate.bin", root / "assets/firmware/candidate.bin"},
        {"rollback.bin", root / "assets/firmware/rollback.bin"},
        {"stock.bin", root / "assets/firmware/stock.bin"},
        {"manifest.json", root / "assets/firmware/manifest.json"},
        {"candidate.hash", fs::path{}},
        {"stock.hash", fs::path{}}};
    std::map<std::string, std::pair<std::size_t, std::string>> files;
    try {
        for (const auto& [name, source] : sources) {
            Bytes content;
            if (name == "candidate.hash" || name == "stock.hash") content = bytes_of(name == "candidate.hash" ? candidate_prefix_md5 : base_prefix_md5);
            else content = read_bounded(source, 2 * flash_size);
            write_file(destination / name, content);
            files.emplace(name, std::make_pair(content.size(), sha256(content)));
        }
        const auto manifest = bundle_manifest_json(files);
        write_file(destination / "bundle.json", std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(manifest.data()), manifest.size()));
        fsync_directory(destination);
    } catch (...) {
        fs::remove_all(destination);
        throw;
    }
    return json_object({{"operation", json_escape("prepare")}, {"firmware_status", json_escape("unchanged")}, {"recovery_status", json_escape("not_started")}, {"bundle", json_escape(destination.string())}, {"artifacts", artifact_json(summary)}});
}

constexpr std::string_view target_root_binary = "/usr/share/firmware/udm-pro-lcm-fw.bin";
constexpr std::string_view target_root_hash = "/usr/share/firmware/udm-pro-lcm-fw.hash";
constexpr std::string_view target_state_root = "/root/.local/state/udm-static-frame";
constexpr std::string_view target_operation_lock = "/run/lock/udm-static-frame.lock";
constexpr std::string_view target_system_info = "/proc/ubnthal/system.info";
constexpr std::string_view target_log = "/var/log/ulcmd.log";
constexpr std::string_view target_tty = "/dev/ttyACM0";
constexpr std::string_view target_physical_usb = "1-2";
std::string target_dfu_serial;
constexpr std::string_view target_internal_flash_map = "@Internal Flash  /0x08000000/04*016Kg,01*064Kg,07*128Kg";

std::string trim_node(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ')) value.pop_back();
    for (auto& character : value) if (character == '\0') character = ' ';
    return value;
}

std::optional<std::string> optional_node(const fs::path& path, std::size_t cap = 65536) {
    try {
        return trim_node(udm::node_text(path, cap));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void write_node(const fs::path& path, std::string_view value) {
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) throw Failure("unable to open control " + path.string() + ": " + std::strerror(errno));
    try {
        write_all(descriptor, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
        ::close(descriptor);
    } catch (...) {
        ::close(descriptor);
        throw;
    }
}

void sync_directory(const fs::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) throw Failure("unable to open directory " + path.string());
    if (::fsync(descriptor) != 0) {
        ::close(descriptor);
        throw Failure("directory sync failed for " + path.string());
    }
    ::close(descriptor);
}

std::string wall_seconds() {
    const auto now_value = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::duration<double>>(now_value).count();
    std::ostringstream output;
    output << std::setprecision(17) << seconds;
    return output.str();
}

class TargetOperationLock {
public:
    TargetOperationLock() : descriptor_(::open(std::string(target_operation_lock).c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600)) {
        struct stat info{};
        require_condition(::fstat(descriptor_, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == 0 && info.st_gid == 0 &&
                              (info.st_mode & 0777) == 0600 && info.st_nlink == 1,
                          "display operation lock metadata differs");
        if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) throw Failure("the display is already in use; stop existing playback first");
        if (::ftruncate(descriptor_, 0) != 0) throw Failure("unable to reset display operation lock");
        const auto pid = std::to_string(::getpid()) + "\n";
        write_all(descriptor_, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(pid.data()), pid.size()));
        if (::fsync(descriptor_) != 0) throw Failure("unable to sync display operation lock");
    }
    ~TargetOperationLock() {
        if (descriptor_ >= 0) {
            ::flock(descriptor_, LOCK_UN);
            ::close(descriptor_);
        }
    }
    TargetOperationLock(const TargetOperationLock&) = delete;
    TargetOperationLock& operator=(const TargetOperationLock&) = delete;
private:
    int descriptor_ = -1;
};

class TargetSignalMask {
public:
    TargetSignalMask() {
        if (::sigemptyset(&blocked_) != 0 || ::sigaddset(&blocked_, SIGHUP) != 0 ||
            ::sigaddset(&blocked_, SIGINT) != 0 || ::sigaddset(&blocked_, SIGTERM) != 0 ||
            ::sigprocmask(SIG_BLOCK, &blocked_, &previous_) != 0) {
            throw Failure("unable to block target transaction signals");
        }
        active_ = true;
    }

    ~TargetSignalMask() {
        if (active_) ::sigprocmask(SIG_SETMASK, &previous_, nullptr);
    }

    TargetSignalMask(const TargetSignalMask&) = delete;
    TargetSignalMask& operator=(const TargetSignalMask&) = delete;

private:
    sigset_t blocked_{};
    sigset_t previous_{};
    bool active_ = false;
};

struct TargetUsbDevice {
    std::map<std::string, std::string> fields;
    std::vector<std::map<std::string, std::string>> interfaces;
};

std::optional<TargetUsbDevice> target_usb_device(std::string_view name) {
    const auto root = fs::path("/sys/bus/usb/devices") / std::string(name);
    std::error_code error;
    if (!fs::is_directory(root, error) || error) return std::nullopt;
    TargetUsbDevice result;
    for (const auto field_name : {"idVendor", "idProduct", "manufacturer", "product", "serial", "busnum", "devnum", "bNumInterfaces"}) {
        result.fields.emplace(field_name, optional_node(root / field_name).value_or(""));
    }
    for (const auto& entry : fs::directory_iterator(root.parent_path(), error)) {
        if (error) break;
        const auto filename = entry.path().filename().string();
        const auto prefix = std::string(name) + ":";
        if (filename.rfind(prefix, 0) != 0 || !fs::is_directory(entry.path(), error) || error) continue;
        std::map<std::string, std::string> interface;
        for (const auto field_name : {"bInterfaceNumber", "bInterfaceClass", "bInterfaceSubClass", "bInterfaceProtocol", "bAlternateSetting", "interface"}) {
            interface.emplace(field_name, optional_node(entry.path() / field_name).value_or(""));
        }
        result.interfaces.push_back(std::move(interface));
    }
    return result;
}

bool target_acm_on_physical() {
    std::error_code error;
    const auto device = fs::weakly_canonical("/sys/class/tty/ttyACM0/device", error);
    if (error || !fs::exists(device, error) || error) return false;
    for (auto current = device; !current.empty() && current != current.parent_path(); current = current.parent_path()) {
        if (current.filename() == target_physical_usb) return true;
    }
    return false;
}

struct TargetAcmOwner {
    int pid = 0;
    std::string executable;
    std::string fd;
    std::string target;
};

std::vector<TargetAcmOwner> target_acm_owners() {
    std::vector<TargetAcmOwner> result;
    std::error_code error;
    for (const auto& process : fs::directory_iterator("/proc", error)) {
        if (error) break;
        const auto name = process.path().filename().string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) continue;
        int pid = 0;
        try { pid = std::stoi(name); } catch (const std::exception&) { continue; }
        std::error_code local_error;
        const auto executable_path = fs::weakly_canonical(process.path() / "exe", local_error);
        if (local_error) continue;
        for (const auto& descriptor : fs::directory_iterator(process.path() / "fd", local_error)) {
            if (local_error) break;
            std::error_code link_error;
            const auto target = fs::weakly_canonical(descriptor.path(), link_error);
            if (link_error) continue;
            const auto target_text = target.string();
            if (target_text.rfind("/dev/ttyACM", 0) != 0) continue;
            result.push_back(TargetAcmOwner{pid, executable_path.string(), descriptor.path().filename().string(), target_text});
        }
    }
    return result;
}

struct TargetServiceState {
    int returncode = -1;
    std::map<std::string, std::string> values;
};

TargetServiceState target_service_state() {
    const auto result = udm::execute({"systemctl", "show", "ulcmd.service", "-p", "ActiveState", "-p", "SubState", "-p", "MainPID"}, {}, 5, 8192);
    TargetServiceState state;
    state.returncode = result.code;
    const std::string output(result.output.begin(), result.output.end());
    std::size_t begin = 0;
    while (begin <= output.size()) {
        const auto end = output.find('\n', begin);
        const std::string line(output.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        const auto equal = line.find('=');
        if (equal != std::string::npos) state.values[line.substr(0, equal)] = line.substr(equal + 1);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return state;
}

bool target_exact_daemon_owner(const TargetServiceState& service, const std::vector<TargetAcmOwner>& owners) {
    const auto iterator = service.values.find("MainPID");
    if (iterator == service.values.end() || iterator->second.empty() || owners.size() != 1) return false;
    int pid = 0;
    try { pid = std::stoi(iterator->second); } catch (const std::exception&) { return false; }
    return pid > 0 && owners[0].pid == pid && owners[0].executable == "/usr/bin/ulcmd" && owners[0].target == target_tty;
}

bool target_application_ready(TargetServiceState* service_out = nullptr) {
    const auto usb = target_usb_device(target_physical_usb);
    const auto service = target_service_state();
    const auto owners = target_acm_owners();
    if (service_out != nullptr) *service_out = service;
    if (!usb.has_value()) return false;
    const auto vendor = usb->fields.find("idVendor");
    const auto product = usb->fields.find("idProduct");
    const auto active = service.values.find("ActiveState");
    return vendor != usb->fields.end() && product != usb->fields.end() && vendor->second == "1f9b" && product->second == "157d" &&
           active != service.values.end() && active->second == "active" && target_acm_on_physical() && target_exact_daemon_owner(service, owners);
}

bool target_dfu_ready(TargetServiceState* service_out = nullptr) {
    const auto usb = target_usb_device(target_physical_usb);
    const auto service = target_service_state();
    if (service_out != nullptr) *service_out = service;
    if (!usb.has_value()) return false;
    const auto vendor = usb->fields.find("idVendor");
    const auto product = usb->fields.find("idProduct");
    const auto serial = usb->fields.find("serial");
    if (vendor == usb->fields.end() || product == usb->fields.end() || serial == usb->fields.end() || vendor->second != "0483" ||
        product->second != "df11" || serial->second.size() != 12 ||
        serial->second.find_first_not_of("0123456789ABCDEF") != std::string::npos ||
        (!target_dfu_serial.empty() && serial->second != target_dfu_serial)) return false;
    return std::any_of(usb->interfaces.begin(), usb->interfaces.end(), [](const auto& interface) {
        const auto value = [&](std::string_view name) { const auto found = interface.find(std::string(name)); return found == interface.end() ? std::string() : found->second; };
        return value("bInterfaceClass") == "fe" && value("bInterfaceSubClass") == "01" && value("bInterfaceProtocol") == "02";
    });
}

void target_wait_usb(std::string_view vendor, std::string_view product, bool require_dfu, bool require_acm, double timeout) {
    const auto deadline = now() + timeout;
    while (now() < deadline) {
        const auto usb = target_usb_device(target_physical_usb);
        if (usb.has_value()) {
            const auto found_vendor = usb->fields.find("idVendor");
            const auto found_product = usb->fields.find("idProduct");
            const bool identity = found_vendor != usb->fields.end() && found_product != usb->fields.end() && found_vendor->second == vendor && found_product->second == product;
            const bool dfu = !require_dfu || std::any_of(usb->interfaces.begin(), usb->interfaces.end(), [](const auto& interface) {
                const auto value = [&](std::string_view name) { const auto found = interface.find(std::string(name)); return found == interface.end() ? std::string() : found->second; };
                return value("bInterfaceClass") == "fe" && value("bInterfaceSubClass") == "01" && value("bInterfaceProtocol") == "02";
            });
            if (identity && dfu && (!require_acm || target_acm_on_physical())) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    throw Failure("expected USB identity/interface did not appear");
}

bool target_dfu_descriptor() {
    const auto descriptor = read_bounded(fs::path("/sys/bus/usb/devices") / std::string(target_physical_usb) / "descriptors", 65536);
    bool configuration = false;
    bool interface = false;
    unsigned functional_count = 0;
    bool can_upload = false;
    bool can_download = false;
    std::size_t offset = 0;
    while (offset < descriptor.size()) {
        if (descriptor.size() - offset < 2) throw Failure("truncated USB descriptor header");
        const auto length = descriptor[offset];
        const auto kind = descriptor[offset + 1];
        if (length < 2 || descriptor.size() - offset < length) throw Failure("malformed USB descriptor length");
        if (kind == 2) {
            if (length < 9) throw Failure("short USB configuration descriptor");
            configuration = descriptor[offset + 5] == 1;
            interface = false;
        } else if (kind == 4) {
            if (length < 9) throw Failure("short USB interface descriptor");
            interface = configuration && descriptor[offset + 2] == 0 && descriptor[offset + 3] == 0 && descriptor[offset + 5] == 0xfe &&
                        descriptor[offset + 6] == 1 && descriptor[offset + 7] == 2;
        } else if (kind == 0x21 && interface) {
            if (length != 9) throw Failure("unexpected DFU functional descriptor length");
            ++functional_count;
            can_download = (descriptor[offset + 2] & 1U) != 0;
            can_upload = (descriptor[offset + 2] & 2U) != 0;
        }
        offset += length;
    }
    return functional_count == 1 && can_upload && can_download;
}

void target_validate_dfu_list(std::string_view output) {
    require_condition(output.size() <= 8192, "dfu-util list output exceeds bound");
    const std::regex pattern(R"dfu(^Found DFU: \[([0-9a-f]{4}):([0-9a-f]{4})\] ver=([0-9a-f]{4}), devnum=([0-9]+), cfg=([0-9]+), intf=([0-9]+), path="([^"]+)", alt=([0-9]+), name="([^"]+)", serial="([^"]+)"$)dfu");
    std::smatch match;
    unsigned matches = 0;
    std::size_t begin = 0;
    while (begin <= output.size()) {
        const auto end = output.find('\n', begin);
        const std::string line(output.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        if (line.rfind("Found DFU:", 0) == 0) {
            require_condition(std::regex_match(line, match, pattern), "malformed dfu-util list record");
            if (match[1].str() == "0483" && match[2].str() == "df11" && match[3].str() == "2200" && match[7].str() == target_physical_usb &&
                match[10].str() == target_dfu_serial && match[5].str() == "1" && match[6].str() == "0" && match[8].str() == "0") {
                ++matches;
                require_condition(match[9].str() == target_internal_flash_map,
                                  "exact internal-flash DFU target not found");
            }
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    require_condition(matches == 1, "exact internal-flash DFU target not found");
}

void target_ensure_dfu() {
    target_wait_usb("0483", "df11", true, false, 12);
    require_condition(target_dfu_ready(), "DFU identity differs from the transaction target");
    const auto usb = target_usb_device(target_physical_usb);
    require_condition(usb.has_value() && usb->fields.contains("serial"), "DFU serial is unavailable");
    const auto& serial = usb->fields.at("serial");
    require_condition(serial.size() == 12 && serial.find_first_not_of("0123456789ABCDEF") == std::string::npos,
                      "DFU serial format is not supported");
    if (target_dfu_serial.empty()) target_dfu_serial = serial;
    require_condition(serial == target_dfu_serial, "DFU serial changed during the transaction");
    const auto listing = udm::execute({"/usr/bin/dfu-util", "--list"}, {}, 8, 8192);
    require_condition(listing.code == 0, "dfu-util listing failed");
    target_validate_dfu_list(std::string(listing.output.begin(), listing.output.end()));
    require_condition(target_dfu_descriptor(), "DFU descriptor lacks upload/download capability");
}

std::vector<std::string> target_dfu_command(std::string_view operation, const fs::path& path) {
    require_condition(operation == "-D" || operation == "-U", "unsupported DFU operation");
    return {"/usr/bin/dfu-util", "-d", "0483:df11", "-p", "1-2", "-c", "1", "-i", "0", "-S", std::string(target_dfu_serial),
            "-a", "0", "-s", "0x08000000:0x100000", std::string(operation), path.string()};
}

std::string target_run_checked(const std::vector<std::string>& command, double timeout, std::size_t output_cap) {
    const auto result = udm::execute(command, {}, timeout, output_cap);
    if (result.code != 0) {
        const std::string error(result.errors.begin(), result.errors.end());
        throw Failure("command failed: " + command.front() + " rc=" + std::to_string(result.code) + (error.empty() ? std::string() : " " + error.substr(0, 4096)));
    }
    return std::string(result.output.begin(), result.output.end());
}

struct TargetFileMetadata {
    uid_t uid = 0;
    gid_t gid = 0;
    mode_t mode = 0;
};

TargetFileMetadata target_file_metadata(const fs::path& path, std::initializer_list<mode_t> allowed_modes) {
    struct stat info{};
    require_condition(::lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode), "root firmware metadata differs: " + path.string());
    const auto mode = static_cast<mode_t>(info.st_mode & 0777);
    require_condition(info.st_uid == 0 && info.st_gid == 0 && std::find(allowed_modes.begin(), allowed_modes.end(), mode) != allowed_modes.end(),
                      "root firmware metadata differs: " + path.string());
    return TargetFileMetadata{info.st_uid, info.st_gid, mode};
}

void target_append(const fs::path& path, std::string_view data) {
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) throw Failure("unable to open journal " + path.string());
    try {
        write_all(descriptor, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
        if (::fsync(descriptor) != 0) throw Failure("journal sync failed");
        ::close(descriptor);
    } catch (...) {
        ::close(descriptor);
        throw;
    }
}

constexpr std::string_view canonical_manifest_sha256 = "98f61388dfbcb6dc510a6d8fa95111cd452cadbbeb00e2203092fd355bfc8481";
constexpr std::string_view target_ulcmd_sha256 = "5c1f0dc8e6673d0d5095e149d86b55c8b082adb94112feb342bbca7a0af096db";
constexpr std::string_view target_dfu_util_sha256 = "3c71fe929becb030481865e08d53c991807937bd08e65f7f46cf8132a736975b";
constexpr std::string_view target_service_unit_sha256 = "1273fa681ff355ae16b20798bbb9eb8fd09ab534a22b5240a49f246c8bd51b69";

struct TargetBundle {
    fs::path directory;
    Bytes base;
    Bytes candidate;
    Bytes rollback;
    Bytes stock;
    Bytes candidate_hash;
    Bytes stock_hash;
    Bytes manifest;

    static TargetBundle load(const fs::path& path) {
        std::error_code error;
        require_condition(fs::is_directory(path, error) && !error, "firmware bundle is not a directory");
        TargetBundle result;
        result.directory = fs::absolute(path);
        result.base = read_bounded(result.directory / "base.bin", flash_size);
        result.candidate = read_bounded(result.directory / "candidate.bin", flash_size);
        result.rollback = read_bounded(result.directory / "rollback.bin", flash_size);
        result.stock = read_bounded(result.directory / "stock.bin", 2 * flash_size);
        result.candidate_hash = read_bounded(result.directory / "candidate.hash", 64);
        result.stock_hash = read_bounded(result.directory / "stock.hash", 64);
        result.manifest = read_bounded(result.directory / "manifest.json", 128 * 1024);
        require_condition(result.base.size() == flash_size, "bundle base length is not 1 MiB");
        require_condition(result.candidate.size() == flash_size, "bundle candidate length is not 1 MiB");
        require_condition(result.rollback.size() == flash_size, "bundle rollback length is not 1 MiB");
        require_condition(result.stock.size() == 0xE4FA0U, "bundle stock package length changed");
        require_condition(result.base == result.rollback, "bundle base and rollback differ");
        require_condition(sha256(result.base) == base_sha256, "bundle base SHA-256 mismatch");
        require_condition(sha256(result.rollback) == base_sha256, "bundle rollback SHA-256 mismatch");
        require_condition(sha256(result.candidate) == candidate_sha256, "bundle candidate SHA-256 mismatch");
        require_condition(sha256(result.stock) == stock_package_sha256, "bundle stock SHA-256 mismatch");
        require_condition(std::string(result.candidate_hash.begin(), result.candidate_hash.end()) == candidate_prefix_md5,
                          "bundle candidate sidecar mismatch");
        require_condition(std::string(result.stock_hash.begin(), result.stock_hash.end()) == base_prefix_md5,
                          "bundle stock sidecar mismatch");
        require_condition(sha256(result.candidate_hash) == sha256(bytes_of(candidate_prefix_md5)),
                          "bundle candidate sidecar digest mismatch");
        require_condition(sha256(result.stock_hash) == stock_sidecar_sha256, "bundle stock sidecar digest mismatch");
        require_condition(sha256(result.manifest) == canonical_manifest_sha256, "bundle manifest identity mismatch");
        const auto footer = hex_bytes(footer_hex);
        const auto footer_begin = static_cast<Bytes::difference_type>(footer_offset);
        const auto footer_end = footer_begin + static_cast<Bytes::difference_type>(footer.size());
        require_condition(std::equal(result.candidate.begin() + footer_begin,
                                     result.candidate.begin() + footer_end, footer.begin()),
                          "bundle candidate footer mismatch");
        require_condition(std::equal(result.rollback.begin() + footer_begin,
                                     result.rollback.begin() + footer_end, footer.begin()),
                          "bundle rollback footer mismatch");
        return result;
    }
};

struct TargetRootPair {
    std::string classification;
    Bytes binary;
    Bytes sidecar;
    TargetFileMetadata binary_metadata;
    TargetFileMetadata sidecar_metadata;
};

TargetRootPair target_root_pair() {
    const auto binary_path = fs::path(target_root_binary);
    const auto sidecar_path = fs::path(target_root_hash);
    TargetRootPair result;
    result.binary = read_bounded(binary_path, 2 * flash_size);
    result.sidecar = read_bounded(sidecar_path, 64);
    require_condition(!result.binary.empty() && result.sidecar.size() == 32, "root firmware pair size is invalid");
    result.classification = classify_pair(result.binary, result.sidecar);
    result.binary_metadata = target_file_metadata(binary_path, {0600, 0644, 0755});
    result.sidecar_metadata = target_file_metadata(sidecar_path, {0600, 0644});
    return result;
}

std::string target_system_id() {
    const auto text = optional_node(fs::path(target_system_info), 65536);
    require_condition(text.has_value(), "system identity node is unavailable");
    std::size_t begin = 0;
    while (begin <= text->size()) {
        const auto end = text->find('\n', begin);
        const auto line = text->substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        const auto equal = line.find('=');
        if (equal != std::string::npos && line.substr(0, equal) == "systemid") return trim_node(line.substr(equal + 1));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return {};
}

void target_validate_platform() {
    require_condition(target_system_id() == "ea15", "target system ID mismatch");
    const std::vector<std::pair<fs::path, std::string_view>> artifacts{
        {"/usr/bin/ulcmd", target_ulcmd_sha256},
        {"/usr/bin/dfu-util", target_dfu_util_sha256},
        {"/lib/systemd/system/ulcmd.service", target_service_unit_sha256}};
    for (const auto& [path, expected] : artifacts) {
        require_condition(sha256(read_bounded(path, 16 * 1024 * 1024)) == expected,
                          "target artifact identity mismatch: " + path.string());
    }
}

std::string target_query_running_hash(bool allow_unknown) {
    const fs::path log_path(target_log);
    Fd log(::open(log_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat identity{};
    require_condition(::fstat(log.get(), &identity) == 0 && S_ISREG(identity.st_mode), "ulcmd log is not a regular file");
    const auto start = ::lseek(log.get(), 0, SEEK_END);
    require_condition(start >= 0, "unable to seek ulcmd log");
    const auto request = udm::execute({"/usr/bin/ulcmd", "--command", "message", "--parameter",
                                       "{\"system\":{\"get\":\"hash\"}}", "--sender", "static-frame-deploy"},
                                      {}, 8, 8192);
    require_condition(request.code == 0, "MCU hash request failed");
    const std::regex pattern(R"(^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2} Warning LCM: Response: (md5|target) ("[^"]*") unhandled *$)");
    std::map<std::string, std::string> found;
    std::string pending;
    std::size_t budget = 65536;
    const auto deadline = now() + 8;
    std::array<char, 8192> block{};
    while (now() < deadline && found.size() != 2) {
        struct stat current{};
        require_condition(::fstat(log.get(), &current) == 0 && current.st_dev == identity.st_dev &&
                              current.st_ino == identity.st_ino && current.st_size >= start,
                          "ulcmd log rotated or truncated");
        const auto count = ::read(log.get(), block.data(), std::min(block.size(), budget));
        if (count > 0) {
            budget -= static_cast<std::size_t>(count);
            pending.append(block.data(), static_cast<std::size_t>(count));
            std::size_t begin = 0;
            while (true) {
                const auto end = pending.find('\n', begin);
                if (end == std::string::npos) {
                    pending.erase(0, begin);
                    break;
                }
                const std::string line = pending.substr(begin, end - begin);
                begin = end + 1;
                std::smatch match;
                if (!std::regex_match(line, match, pattern)) continue;
                require_condition(match.size() == 3, "malformed MCU hash response");
                const auto key = match[1].str();
                require_condition(!found.contains(key), "duplicate MCU hash response");
                auto value = match[2].str();
                require_condition(value.size() >= 2 && value.front() == '"' && value.back() == '"', "malformed MCU hash value");
                found.emplace(key, value.substr(1, value.size() - 2));
            }
            require_condition(pending.size() <= 4096 && budget > 0, "MCU hash response bound exceeded");
        } else if (count < 0 && errno != EAGAIN && errno != EINTR) {
            throw Failure("ulcmd log read failed");
        }
        if (found.size() != 2) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    require_condition(found.contains("md5") && found.contains("target"), "MCU hash response timed out");
    require_condition(found.at("target") == "udm-pro-lcm-fw", "unexpected MCU target name");
    const auto& prefix = found.at("md5");
    require_condition(prefix.size() == 32 && prefix.find_first_not_of("0123456789abcdef") == std::string::npos,
                      "unexpected MCU prefix MD5");
    if (!allow_unknown) {
        const auto prefixes = known_prefixes();
        require_condition(std::find(prefixes.begin(), prefixes.end(), prefix) != prefixes.end(), "unrecognized MCU prefix MD5");
    }
    return prefix;
}

void target_wait_application(double timeout) {
    const auto deadline = now() + timeout;
    while (now() < deadline) {
        if (target_application_ready()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    throw Failure("exact application identity and daemon owner did not appear");
}

struct TargetObservation {
    std::string transport;
    std::string root_pair;
    std::optional<std::string> running_prefix;
};

class TargetTransaction {
public:
    explicit TargetTransaction(TargetBundle bundle) : bundle_(std::move(bundle)) {}

    std::string run(std::string_view mode) {
        require_condition(mode == "install" || mode == "recover", "invalid target firmware mode");
        TargetResult result;
        try {
            const auto observation = observe(mode);
            prepare(mode, observation);
            bool download_started = false;
            try {
                if (observation.transport == "application") {
                    stop_service();
                    enter_dfu();
                }
                ensure_dfu();
                const auto prewrite = upload("prewrite");
                if (mode == "install") {
                    const auto flash_hashes = known_flash_hashes();
                    require_condition(std::find(flash_hashes.begin(), flash_hashes.end(), prewrite) != flash_hashes.end(),
                                      "install prewrite is not a pinned flash image");
                }
                download_started = true;
                download(mode == "install" ? "candidate" : "rollback");
                ensure_dfu();
                const auto postwrite = upload("postwrite");
                const auto expected = mode == "install" ? std::string(candidate_sha256) : std::string(base_sha256);
                require_condition(postwrite == expected, "postwrite readback SHA-256 mismatch");
                publish_pair(mode == "install" ? "candidate" : "stock");
                exit_dfu();
                start_and_verify(mode == "install" ? std::string(candidate_prefix_md5) : std::string(base_prefix_md5));
                result.firmware_status = mode == "install" ? "candidate_verified" : "stock_verified";
                result.recovery_status = "not_needed";
                result.transport = observation.transport;
                return result.json(mode);
            } catch (const std::exception& error) {
                result.error = error.what();
                if (!download_started) {
                    try { result.recovery_status = cleanup_before_download(observation); }
                    catch (const std::exception& cleanup_error) { result.recovery_status = "cleanup_failed: " + std::string(cleanup_error.what()); }
                } else {
                    try {
                        recover_once();
                        result.recovery_status = "stock_verified";
                    } catch (const std::exception& recovery_error) {
                        result.recovery_status = "failed_after_one_attempt";
                        result.safe_state_error = recovery_error.what();
                        try { fail_safe_dfu_stopped(); }
                        catch (const std::exception& safe_error) { result.safe_state_error += "; " + std::string(safe_error.what()); }
                    }
                }
                return result.json(mode);
            }
        } catch (const std::exception& error) {
            result.error = error.what();
            return result.json(mode);
        }
    }

private:
    struct TargetResult {
        std::string firmware_status = "unchanged_or_unknown";
        std::string recovery_status = "not_started";
        std::string transport;
        std::string error;
        std::string safe_state_error;

        std::string json(std::string_view mode) const {
            std::vector<std::pair<std::string, std::string>> fields{
                {"operation", json_escape(mode)}, {"firmware_status", json_escape(firmware_status)},
                {"recovery_status", json_escape(recovery_status)}};
            if (!transport.empty()) fields.emplace_back("initial_transport", json_escape(transport));
            if (!error.empty()) fields.emplace_back("error", json_escape(error));
            if (!safe_state_error.empty()) fields.emplace_back("safe_state_error", json_escape(safe_state_error));
            return json_object(fields);
        }
    };

    TargetObservation observe(std::string_view mode) {
        target_validate_platform();
        const auto root = target_root_pair();
        require_condition(root.classification != "unknown", "root firmware pair is not pinned");
        const bool application = target_application_ready();
        const bool dfu = target_dfu_ready();
        TargetObservation result;
        result.root_pair = root.classification;
        if (application) {
            result.transport = "application";
            result.running_prefix = target_query_running_hash(mode == "recover");
        } else if (dfu && target_service_state().values.at("ActiveState") == "inactive" && target_acm_owners().empty()) {
            result.transport = "dfu";
        } else {
            throw Failure("neither exact owned application nor stopped exact ROM-DFU state");
        }
        if (mode == "install") {
            require_condition(result.transport == "application", "install requires the exact application transport");
            const auto pins = known_pairs();
            const auto pin = std::find_if(pins.begin(), pins.end(), [&](const auto& value) {
                return value.name == result.root_pair;
            });
            require_condition(pin != pins.end(), "install requires an exact pinned root firmware pair");
            require_condition(result.running_prefix.has_value(), "install requires a running MCU prefix MD5");
            require_condition(*result.running_prefix == pin->prefix_md5,
                              "install running prefix and package pair do not match");
        }
        return result;
    }

    void prepare(std::string_view mode, const TargetObservation& observation) {
        const fs::path state(target_state_root);
        std::error_code error;
        if (fs::exists(state, error)) {
            struct stat info{};
            require_condition(::lstat(state.c_str(), &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == 0 && info.st_gid == 0 &&
                                  (info.st_mode & 0777) == 0700, "state directory ownership or mode mismatch");
        } else {
            fs::create_directories(state.parent_path());
            require_condition(::mkdir(state.c_str(), 0700) == 0, "unable to create state directory");
        }
        sync_directory(state.parent_path());
        transaction_ = make_directory(state, "transaction-");
        ::chmod(transaction_.c_str(), 0700);
        journal_ = transaction_ / "journal.ndjson";
        write_file(journal_, {});
        journal("transaction_created", json_object({{"mode", json_escape(mode)}, {"transport", json_escape(observation.transport)},
                                                     {"root_pair", json_escape(observation.root_pair)}}));
        const std::vector<std::pair<std::string, std::span<const std::uint8_t>>> inputs{
            {"base.bin", bundle_.base}, {"candidate.bin", bundle_.candidate}, {"rollback.bin", bundle_.rollback},
            {"stock.bin", bundle_.stock}, {"candidate.hash", bundle_.candidate_hash}, {"stock.hash", bundle_.stock_hash},
            {"manifest.json", bundle_.manifest}};
        for (const auto& [name, data] : inputs) {
            const auto path = transaction_ / name;
            write_file(path, data);
            const auto copied = read_bounded(path, std::max<std::size_t>(data.size(), 1));
            require_condition(copied.size() == data.size() && sha256(copied) == sha256(data), "staged input verification failed: " + name);
            inputs_.emplace(name, path);
            journal("stage_input", json_object({{"name", json_escape(name)}, {"size", std::to_string(data.size())},
                                                  {"sha256", json_escape(sha256(data))}}));
        }
        sync_directory(transaction_);
        const auto root = target_root_pair();
        root_metadata_.emplace("bin", root.binary_metadata);
        root_metadata_.emplace("hash", root.sidecar_metadata);
        journal("preserve_root_pair", json_object({{"classification", json_escape(root.classification)}}));
        write_file(transaction_ / "original.bin", root.binary);
        write_file(transaction_ / "original.hash", root.sidecar);
        sync_directory(transaction_);
        const std::array<std::string_view, 2> controls{
            "/sys/devices/platform/soc/soc:ulcm/dfu_or_reset", "/sys/devices/platform/ulcm/dfu_or_reset"};
        for (const auto path_text : controls) {
            struct stat info{};
            const fs::path path(path_text);
            if (::lstat(path.c_str(), &info) == 0 && info.st_uid == 0 && info.st_gid == 0 && S_ISREG(info.st_mode) && (info.st_mode & S_IWUSR)) {
                control_ = path;
                break;
            }
        }
        require_condition(!control_.empty(), "no pinned root-writable DFU control");
        journal("selected_control", json_object({{"path", json_escape(control_.string())}}));
        stage_root_pair("candidate", bundle_.candidate, bundle_.candidate_hash);
        stage_root_pair("stock", bundle_.stock, bundle_.stock_hash);
        sync_directory(fs::path(target_root_binary).parent_path());
    }

    void stage_root_pair(std::string_view kind, std::span<const std::uint8_t> binary, std::span<const std::uint8_t> sidecar) {
        const auto token = transaction_.filename().string();
        const auto bin_path = fs::path(target_root_binary).parent_path() / (".udm-static-frame-" + token + "-" + std::string(kind) + ".bin");
        const auto hash_path = fs::path(target_root_hash).parent_path() / (".udm-static-frame-" + token + "-" + std::string(kind) + ".hash");
        write_file(bin_path, binary);
        write_file(hash_path, sidecar);
        require_condition(sha256(read_bounded(bin_path, 2 * flash_size)) == sha256(binary), "staged root binary verification failed");
        require_condition(sha256(read_bounded(hash_path, 64)) == sha256(sidecar), "staged root sidecar verification failed");
        staged_[std::string(kind)] = {bin_path, hash_path};
        journal("stage_root_pair", json_object({{"kind", json_escape(kind)}, {"bin", json_escape(bin_path.string())},
                                                  {"hash", json_escape(hash_path.string())}}));
    }

    void journal(std::string_view event, std::string_view details) {
        const auto record = json_object({{"time", wall_seconds()}, {"event", json_escape(event)}, {"details", std::string(details)}}) + "\n";
        target_append(journal_, record);
        sync_directory(transaction_);
    }

    void stop_service() {
        journal("stop_service_intent", "{}");
        target_run_checked({"systemctl", "stop", "ulcmd.service"}, 10, 8192);
        const auto deadline = now() + 10;
        while (now() < deadline) {
            const auto service = target_service_state();
            if (service.values.contains("ActiveState") && service.values.at("ActiveState") == "inactive" && target_acm_owners().empty()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        throw Failure("service stop or tty ownership check failed");
    }

    void enter_dfu() {
        journal("enter_dfu_intent", json_object({{"control", json_escape(control_.string())}}));
        write_node(control_, "1");
    }

    void ensure_dfu() {
        target_ensure_dfu();
        journal("verify_exact_dfu_identity", json_object({{"serial", json_escape(target_dfu_serial)}, {"path", json_escape(target_physical_usb)}}));
    }

    std::string upload(std::string_view label) {
        const auto path = transaction_ / (std::string(label) + ".bin");
        journal("upload_intent", json_object({{"label", json_escape(label)}, {"path", json_escape(path.string())}}));
        target_run_checked(target_dfu_command("-U", path), 90, 131072);
        const auto data = read_bounded(path, flash_size + 1);
        require_condition(data.size() == flash_size, "DFU upload size mismatch");
        const auto digest = sha256(data);
        journal("upload_verified", json_object({{"label", json_escape(label)}, {"sha256", json_escape(digest)}}));
        return digest;
    }

    void download(std::string_view kind) {
        const auto iterator = inputs_.find(kind == "candidate" ? "candidate.bin" : "rollback.bin");
        require_condition(iterator != inputs_.end(), "missing staged download input");
        journal("download_may_have_started", json_object({{"kind", json_escape(kind)}}));
        target_run_checked(target_dfu_command("-D", iterator->second), 120, 131072);
    }

    void publish_pair(std::string_view kind) {
        const auto iterator = staged_.find(std::string(kind));
        require_condition(iterator != staged_.end(), "missing staged root pair");
        const auto expected_binary = kind == "candidate" ? std::string(candidate_sha256) : std::string(stock_package_sha256);
        const auto expected_sidecar = kind == "candidate" ? sha256(bytes_of(candidate_prefix_md5)) : std::string(stock_sidecar_sha256);
        for (const auto& [key, path] : std::array<std::pair<std::string_view, fs::path>, 2>{{{"bin", iterator->second.first}, {"hash", iterator->second.second}}}) {
            const auto metadata = root_metadata_.at(std::string(key));
            require_condition(::chown(path.c_str(), metadata.uid, metadata.gid) == 0, "unable to preserve root firmware ownership");
            require_condition(::chmod(path.c_str(), metadata.mode) == 0, "unable to preserve root firmware mode");
            const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            require_condition(descriptor >= 0 && ::fsync(descriptor) == 0, "unable to sync staged root firmware");
            ::close(descriptor);
        }
        journal("publish_bin_intent", json_object({{"kind", json_escape(kind)}}));
        require_condition(::rename(iterator->second.first.c_str(), std::string(target_root_binary).c_str()) == 0, "unable to publish root firmware binary");
        sync_directory(fs::path(target_root_binary).parent_path());
        require_condition(sha256(read_bounded(fs::path(target_root_binary), 2 * flash_size)) == expected_binary,
                          "published root firmware binary verification failed");
        journal("publish_hash_intent", json_object({{"kind", json_escape(kind)}}));
        require_condition(::rename(iterator->second.second.c_str(), std::string(target_root_hash).c_str()) == 0, "unable to publish root firmware sidecar");
        sync_directory(fs::path(target_root_hash).parent_path());
        const auto pair = target_root_pair();
        require_condition(sha256(pair.binary) == expected_binary && sha256(pair.sidecar) == expected_sidecar,
                          "published root firmware pair verification failed");
        staged_.erase(iterator);
    }

    void exit_dfu() {
        journal("exit_dfu_intent", json_object({{"control", json_escape(control_.string())}}));
        write_node(control_, "0");
    }

    void start_and_verify(std::string_view expected_prefix) {
        target_wait_usb("1f9b", "157d", false, true, 15);
        journal("start_service_intent", json_object({{"expected_md5", json_escape(expected_prefix)}}));
        target_run_checked({"systemctl", "start", "ulcmd.service"}, 10, 8192);
        target_wait_application(10);
        require_condition(target_query_running_hash(false) == expected_prefix, "application/service/hash verification failed");
    }

    std::string cleanup_before_download(const TargetObservation& observation) {
        if (observation.transport == "dfu") return "dfu_preserved_service_stopped";
        try {
            if (target_dfu_ready()) exit_dfu();
            require_condition(observation.running_prefix.has_value(), "original MCU prefix MD5 is unavailable");
            start_and_verify(*observation.running_prefix);
            return "original_application_verified";
        } catch (...) {
            try {
                if (target_service_state().values.at("ActiveState") != "inactive") stop_service();
                return "service_stopped_state_unverified";
            } catch (...) {
                return "service_state_unknown";
            }
        }
    }

    void recover_to_dfu() {
        const auto service = target_service_state();
        if (service.values.find("ActiveState") == service.values.end() || service.values.at("ActiveState") != "inactive") stop_service();
        if (!target_dfu_ready()) enter_dfu();
        ensure_dfu();
    }

    void recover_once() {
        journal("recovery_attempt", json_object({{"attempt", "1"}}));
        recover_to_dfu();
        journal("recovery_download_may_have_started", json_object({{"attempt", "1"}}));
        target_run_checked(target_dfu_command("-D", inputs_.at("rollback.bin")), 120, 131072);
        ensure_dfu();
        const auto recovery = upload("recovery");
        require_condition(recovery == base_sha256, "recovery readback SHA-256 mismatch");
        publish_pair("stock");
        exit_dfu();
        start_and_verify(base_prefix_md5);
    }

    void fail_safe_dfu_stopped() {
        const auto service = target_service_state();
        if (service.values.find("ActiveState") == service.values.end() || service.values.at("ActiveState") != "inactive") stop_service();
        require_condition(target_acm_owners().empty(), "display tty remains owned in fail-safe path");
        if (!target_dfu_ready()) enter_dfu();
        ensure_dfu();
    }

    TargetBundle bundle_;
    fs::path transaction_;
    fs::path journal_;
    fs::path control_;
    std::map<std::string, fs::path> inputs_;
    std::map<std::string, std::pair<fs::path, fs::path>> staged_;
    std::map<std::string, TargetFileMetadata> root_metadata_;
};

int target_apply_command(std::string_view mode, const fs::path& bundle_path) {
    TargetSignalMask signals;
    TargetOperationLock lock;
    target_dfu_serial.clear();
    const auto result = TargetTransaction(TargetBundle::load(bundle_path)).run(mode);
    std::cout << result << '\n';
    const auto expected = mode == "install" ? "candidate_verified" : "stock_verified";
    return result.find("\"firmware_status\":\"" + std::string(expected) + "\"") != std::string::npos &&
                   result.find("\"error\":") == std::string::npos ? 0 : 1;
}

struct CommandOptions {
    std::optional<fs::path> root;
    std::optional<fs::path> base;
    std::optional<fs::path> stock;
    std::optional<fs::path> rollback;
    std::optional<fs::path> output;
    std::optional<fs::path> prepare;
    std::optional<fs::path> bundle_dir;
    std::optional<std::string> host;
    std::optional<fs::path> runtime;
    std::string plan = "install";
    bool live = false;
    bool target = false;
};

CommandOptions parse_options(const std::vector<std::string>& args, std::size_t begin) {
    CommandOptions result;
    for (std::size_t index = begin; index < args.size(); ++index) {
        if (args[index] == "--root") {
            if (++index >= args.size()) throw Failure("missing value for --root");
            result.root = fs::path(args[index]);
        } else if (args[index] == "--base") {
            if (++index >= args.size()) throw Failure("missing value for --base");
            result.base = fs::path(args[index]);
        } else if (args[index] == "--stock") {
            if (++index >= args.size()) throw Failure("missing value for --stock");
            result.stock = fs::path(args[index]);
        } else if (args[index] == "--rollback") {
            if (++index >= args.size()) throw Failure("missing value for --rollback");
            result.rollback = fs::path(args[index]);
        } else if (args[index] == "--output") {
            if (++index >= args.size()) throw Failure("missing value for --output");
            result.output = fs::path(args[index]);
        } else if (args[index] == "--prepare") {
            if (++index >= args.size()) throw Failure("missing value for --prepare");
            result.prepare = fs::path(args[index]);
        } else if (args[index] == "--bundle-dir") {
            if (++index >= args.size()) throw Failure("missing value for --bundle-dir");
            result.bundle_dir = fs::path(args[index]);
        } else if (args[index] == "--host") {
            if (++index >= args.size()) throw Failure("missing value for --host");
            result.host = args[index];
        } else if (args[index] == "--runtime") {
            if (++index >= args.size()) throw Failure("missing value for --runtime");
            result.runtime = fs::path(args[index]);
        } else if (args[index] == "--target") {
            result.target = true;
        } else if (args[index] == "--plan") {
            if (++index >= args.size()) throw Failure("missing value for --plan");
            result.plan = args[index];
        } else if (args[index] == "--live") {
            result.live = true;
        } else if (args[index] == "--help" || args[index] == "-h") {
            throw Failure("usage: firmware build|inspect|install|recover [--root PATH] [--stock PATH] [--base PATH] [--rollback PATH] [--output PATH] [--prepare PATH] [--bundle-dir PATH] [--host USER@HOST] [--runtime PATH] [--target] [--live] [--plan install|restore]");
        } else {
            throw Failure("unknown firmware option: " + args[index]);
        }
    }
    return result;
}

int build_command(const CommandOptions& options) {
    const auto root = find_root(options.root);
    const auto stock_path = options.stock.value_or(root / "assets/firmware/stock.bin");
    if (!fs::is_regular_file(stock_path)) {
        throw Failure("pinned stock image is required; pass --stock PATH to the exact UDM Pro stock.bin");
    }
    Bytes stock;
    try {
        stock = read_bounded(stock_path, stock_package_size);
    } catch (const Failure& error) {
        throw Failure("invalid pinned stock image: " + std::string(error.what()) + "; provide the exact 937888-byte stock.bin");
    }
    auto base = reconstruct_base_image(stock);
    if (options.base.has_value()) {
        const auto supplied_base = read_bounded(*options.base, flash_size);
        validate_base_image(supplied_base);
        if (supplied_base != base) {
            throw Failure("supplied base image differs from the deterministic stock reconstruction; provide the exact pinned pair");
        }
        base = supplied_base;
    }
    auto rollback = base;
    if (options.rollback.has_value()) {
        rollback = read_bounded(*options.rollback, flash_size);
        validate_base_image(rollback);
        if (rollback != base) {
            throw Failure("base and rollback images differ; source-only bootstrap requires the exact pinned identical pair");
        }
    }
    const auto output_directory = options.output.has_value() ? [&]() {
        if (fs::exists(*options.output)) throw Failure("build output directory already exists");
        if (!options.output->parent_path().empty()) fs::create_directories(options.output->parent_path());
        if (!fs::create_directory(*options.output)) throw Failure("unable to create build output directory");
        return *options.output;
    }() : make_directory(root / "build/firmware", "indexed-stream-build-cpp-");
    const auto compile_directory = options.output.has_value()
                                       ? make_directory(output_directory.parent_path(), ".indexed-stream-build-cpp-")
                                       : output_directory;
    try {
        const auto result = build_indexed_candidate(root, base, compile_directory);
        write_file(output_directory / "stock.bin", stock);
        write_file(output_directory / "base.bin", base);
        write_file(output_directory / "rollback.bin", rollback);
        write_file(output_directory / "candidate.bin", result.image);
        write_file(output_directory / "stock.hash", bytes_of(base_prefix_md5));
        write_file(output_directory / "candidate.hash", bytes_of(candidate_prefix_md5));
        write_file(output_directory / "payload.bin", result.payload);
        write_file(output_directory / "candidate-build-only.bin", result.image);
        write_file(output_directory / "manifest.json", std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(result.manifest.data()), result.manifest.size()));
        fsync_directory(output_directory);
        std::cout << (output_directory / "manifest.json").string() << '\n';
    } catch (...) {
        if (!options.output.has_value()) fs::remove_all(output_directory);
        if (options.output.has_value()) fs::remove_all(compile_directory);
        throw;
    }
    if (options.output.has_value()) fs::remove_all(compile_directory);
    return 0;
}

int inspect_command(const CommandOptions& options) {
    const auto root = find_root(options.root);
    const auto summary = validate_artifacts(root);
    std::cout << offline_report(options.plan, summary, true) << '\n';
    return 0;
}

int remote_deployment(std::string_view mode, const CommandOptions& options, const fs::path& root) {
    require_condition(options.host.has_value(), "live firmware deployment requires --host");
    require_condition(!options.prepare.has_value(), "--prepare cannot be combined with live deployment");
    fs::path bundle;
    fs::path temporary;
    if (options.bundle_dir.has_value()) {
        bundle = fs::absolute(*options.bundle_dir);
    } else {
        temporary = make_directory(root / "build/firmware", "firmware-remote-");
        bundle = temporary / "bundle";
        prepare_bundle(root, bundle);
    }
    try {
        TargetBundle::load(bundle);
        const auto runtime = options.runtime.value_or(project_root() / "build/aarch64/udm-lcd");
        const std::vector<Upload> uploads{
            {"udm-lcd", runtime}, {"base.bin", bundle / "base.bin"}, {"candidate.bin", bundle / "candidate.bin"},
            {"rollback.bin", bundle / "rollback.bin"}, {"stock.bin", bundle / "stock.bin"},
            {"candidate.hash", bundle / "candidate.hash"}, {"stock.hash", bundle / "stock.hash"},
            {"manifest.json", bundle / "manifest.json"}};
        Connection connection;
        connection.host = *options.host;
        if (options.runtime.has_value()) connection.runtime = *options.runtime;
        const auto result = upload_remote(connection, uploads,
                                          {"firmware", "target", std::string(mode), "--bundle-dir", "."}, 240);
        if (!result.output.empty()) std::cout.write(reinterpret_cast<const char*>(result.output.data()),
                                                    static_cast<std::streamsize>(result.output.size()));
        if (!result.errors.empty()) std::cerr << result.errors;
        if (!temporary.empty()) fs::remove_all(temporary);
        return result.code;
    } catch (...) {
        if (!temporary.empty()) fs::remove_all(temporary);
        throw;
    }
}

int deployment_command(std::string_view mode, const CommandOptions& options) {
    if (options.target) {
        require_condition(options.bundle_dir.has_value(), "target firmware execution requires --bundle-dir");
        return target_apply_command(mode, *options.bundle_dir);
    }
    const auto root = find_root(options.root);
    const auto summary = validate_artifacts(root);
    if (options.prepare.has_value()) {
        std::cout << prepare_bundle(root, *options.prepare) << '\n';
        return 0;
    }
    if (options.live || options.host.has_value()) return remote_deployment(mode, options, root);
    std::cout << offline_report(mode == "recover" ? "restore" : "install", summary, true) << '\n';
    return 0;
}

}

int firmware_command(const std::vector<std::string>& input_args) {
    try {
        std::vector<std::string> args = input_args;
        if (!args.empty() && args.front() == "firmware") args.erase(args.begin());
        if (args.empty()) throw Failure("usage: firmware build|inspect|install|recover");
        const auto command = args.front();
        if (command == "target") {
            if (args.size() < 2) throw Failure("usage: firmware target install|recover --bundle-dir PATH");
            const auto mode = args[1];
            const auto options = parse_options(args, 2);
            if (mode != "install" && mode != "recover") throw Failure("unknown target firmware command: " + mode);
            if (!options.bundle_dir.has_value()) throw Failure("target firmware execution requires --bundle-dir");
            return target_apply_command(mode, *options.bundle_dir);
        }
        const auto options = parse_options(args, 1);
        if (command == "build") return build_command(options);
        if (command == "inspect") return inspect_command(options);
        if (command == "install" || command == "recover") return deployment_command(command, options);
        throw Failure("unknown firmware command: " + command);
    } catch (const std::exception& error) {
        std::cerr << "firmware: " << error.what() << '\n';
        return 1;
    }
}

}
