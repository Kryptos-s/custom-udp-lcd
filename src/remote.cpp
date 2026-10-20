#include "remote.hpp"

#include "media.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <poll.h>
#include <random>
#include <signal.h>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace udm {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t remote_archive_cap = 96u * 1024u * 1024u;
constexpr std::size_t remote_output_cap = 16u * 1024u * 1024u;
constexpr std::string_view background_unit = "udm-lcd-media.service";
constexpr std::string_view background_description = "UDM LCD standalone media";
constexpr std::string_view background_fragment = "/run/systemd/transient/udm-lcd-media.service";
const fs::path target_base = "/tmp/udm-lcd-cpp";
const fs::path deploy_lock_path = "/run/lock/udm-lcd-deploy.lock";
const fs::path display_lock_path = "/run/lock/udm-static-frame.lock";

[[noreturn]] void fail(std::string message)
{
    throw std::runtime_error(std::move(message));
}

void ensure(bool condition, std::string_view message)
{
    if (!condition)
        fail(std::string(message));
}

bool all_name_characters(std::string_view value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '_' || character == '-' || character == '.';
    });
}

void validate_upload_name(std::string_view name)
{
    ensure(name.size() <= 128 && all_name_characters(name) && name != "." && name != ".." && name.front() != '-',
           "Unsafe upload member name");
}

void validate_host(std::string_view host)
{
    ensure(host.size() <= 256 && !host.empty() && host.front() != '-', "Invalid SSH destination");
    const auto at = host.find('@');
    const auto user = at == std::string_view::npos ? std::string_view{} : host.substr(0, at);
    const auto name = at == std::string_view::npos ? host : host.substr(at + 1);
    if (!user.empty()) {
        ensure((user.front() >= 'A' && user.front() <= 'Z') || (user.front() >= 'a' && user.front() <= 'z') || user.front() == '_',
               "Invalid SSH user");
        ensure(std::all_of(user.begin() + 1, user.end(), [](unsigned char character) {
            return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                   (character >= '0' && character <= '9') || character == '_' || character == '-';
        }), "Invalid SSH user");
    }
    ensure(!name.empty() && ((name.front() >= 'A' && name.front() <= 'Z') || (name.front() >= 'a' && name.front() <= 'z') ||
                             (name.front() >= '0' && name.front() <= '9')),
           "Invalid SSH host");
    ensure(std::all_of(name.begin(), name.end(), [](unsigned char character) {
        return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '_' || character == '.' || character == ':' ||
               character == '-';
    }), "Invalid SSH host");
}

std::string shell_quote(std::string_view value)
{
    ensure(value.find('\0') == std::string_view::npos, "NUL in remote argument");
    return quote(value);
}

std::string shell_join(const std::vector<std::string>& values)
{
    std::string result;
    for (const auto& value : values) {
        if (!result.empty())
            result.push_back(' ');
        result += shell_quote(value);
    }
    return result;
}

fs::path private_directory(std::string_view prefix, const fs::path& parent = fs::temp_directory_path())
{
    std::error_code error;
    fs::create_directories(parent, error);
    ensure(!error, "Unable to create private temporary parent");
    auto pattern = (parent / (std::string(prefix) + "XXXXXX")).string();
    std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
    mutable_pattern.push_back('\0');
    ensure(::mkdtemp(mutable_pattern.data()) != nullptr, "Unable to create private temporary directory");
    return fs::path(mutable_pattern.data());
}

class TemporaryDirectory {
    fs::path path_;
public:
    explicit TemporaryDirectory(fs::path path) : path_(std::move(path)) {}
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    TemporaryDirectory(TemporaryDirectory&&) = delete;
    TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;
    ~TemporaryDirectory() { std::error_code error; fs::remove_all(path_, error); }
    const fs::path& path() const { return path_; }
};

void tar_octal(std::array<std::uint8_t, 512>& header, std::size_t offset, std::size_t length, std::uint64_t value)
{
    ensure(length > 1, "Invalid tar field");
    std::string digits(length - 1, '0');
    for (std::size_t index = digits.size(); index > 0 && value; --index) {
        digits[index - 1] = static_cast<char>('0' + (value & 7u));
        value >>= 3u;
    }
    ensure(value == 0, "Tar field exceeds range");
    std::copy(digits.begin(), digits.end(), header.begin() + static_cast<std::ptrdiff_t>(offset));
    header[offset + length - 1] = 0;
}

void append_tar_member(Bytes& archive, std::string_view name, std::span<const std::uint8_t> data)
{
    validate_upload_name(name);
    ensure(name.size() <= 100, "Upload member name exceeds tar limit");
    std::array<std::uint8_t, 512> header{};
    std::copy(name.begin(), name.end(), header.begin());
    tar_octal(header, 100, 8, 0600);
    tar_octal(header, 108, 8, 0);
    tar_octal(header, 116, 8, 0);
    tar_octal(header, 124, 12, data.size());
    tar_octal(header, 136, 12, 0);
    std::fill(header.begin() + 148, header.begin() + 156, static_cast<std::uint8_t>(' '));
    header[156] = '0';
    const std::string magic = "ustar\0";
    std::copy(magic.begin(), magic.end(), header.begin() + 257);
    header[263] = '0';
    header[264] = '0';
    std::uint32_t checksum = 0;
    for (const auto value : header)
        checksum += value;
    tar_octal(header, 148, 7, checksum);
    header[154] = 0;
    header[155] = ' ';
    ensure(archive.size() <= remote_archive_cap && data.size() <= remote_archive_cap - archive.size(),
           "Remote upload exceeds 96 MiB cap");
    archive.insert(archive.end(), header.begin(), header.end());
    archive.insert(archive.end(), data.begin(), data.end());
    const auto padding = (512u - data.size() % 512u) % 512u;
    archive.insert(archive.end(), padding, 0);
}

struct UploadData {
    std::string name;
    Bytes data;
};

std::uint16_t elf_u16(std::span<const std::uint8_t> data, std::size_t offset)
{
    ensure(offset <= data.size() && data.size() - offset >= 2, "Truncated runtime ELF");
    return static_cast<std::uint16_t>(data[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1]) << 8u);
}

std::uint64_t elf_u64(std::span<const std::uint8_t> data, std::size_t offset)
{
    ensure(offset <= data.size() && data.size() - offset >= 8, "Truncated runtime ELF");
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index)
        value |= static_cast<std::uint64_t>(data[offset + index]) << (index * 8u);
    return value;
}

std::uint32_t elf_u32(std::span<const std::uint8_t> data, std::size_t offset)
{
    ensure(offset <= data.size() && data.size() - offset >= 4, "Truncated runtime ELF");
    return static_cast<std::uint32_t>(data[offset]) |
           static_cast<std::uint32_t>(data[offset + 1]) << 8u |
           static_cast<std::uint32_t>(data[offset + 2]) << 16u |
           static_cast<std::uint32_t>(data[offset + 3]) << 24u;
}

void validate_runtime_elf(std::span<const std::uint8_t> data)
{
    ensure(data.size() >= 64 && data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' && data[3] == 'F' &&
               data[4] == 2 && data[5] == 1 && data[6] == 1,
           "Runtime must be ELF64 little-endian");
    const auto type = elf_u16(data, 16);
    ensure(type == 2 || type == 3, "Runtime ELF type is not executable");
    ensure(elf_u16(data, 18) == 183, "Runtime must target ARM64");
    ensure(elf_u16(data, 52) == 64, "Runtime ELF header size differs");
    const auto program_offset = elf_u64(data, 32);
    const auto program_size = elf_u16(data, 54);
    const auto program_count = elf_u16(data, 56);
    ensure(program_size >= 56 && program_count <= 128, "Runtime ELF program table differs");
    ensure(program_offset <= data.size() &&
               (program_count == 0 || program_size <= (data.size() - static_cast<std::size_t>(program_offset)) / program_count),
           "Runtime ELF program table is truncated");
    for (std::size_t index = 0; index < program_count; ++index) {
        const auto offset = static_cast<std::size_t>(program_offset) + index * program_size;
        ensure(elf_u32(data, offset) != 3, "Runtime must be statically linked without PT_INTERP");
    }
}

std::vector<UploadData> read_uploads(const std::vector<Upload>& files)
{
    ensure(!files.empty() && files.size() <= 64, "Invalid upload file set");
    std::vector<UploadData> result;
    result.reserve(files.size());
    std::size_t total = 0;
    bool runtime = false;
    for (const auto& file : files) {
        validate_upload_name(file.name);
        ensure(std::none_of(result.begin(), result.end(), [&](const UploadData& current) { return current.name == file.name; }),
               "Duplicate upload member name");
        struct stat info{};
        ensure(::lstat(file.path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_nlink == 1,
               "Upload source must be a regular single-link file: " + file.path.string());
        auto data = read_file(file.path, remote_archive_cap);
        ensure(total <= remote_archive_cap - data.size(), "Remote upload exceeds 96 MiB cap");
        total += data.size();
        if (file.name == "udm-lcd") {
            ensure(!runtime, "Duplicate runtime upload");
            runtime = true;
            validate_runtime_elf(data);
        }
        result.push_back({file.name, std::move(data)});
    }
    ensure(runtime, "Upload set must contain udm-lcd runtime");
    return result;
}

std::string archive_members(const std::vector<UploadData>& files)
{
    std::string result;
    for (const auto& file : files) {
        if (!result.empty())
            result.push_back('\n');
        result += file.name;
    }
    return result;
}

std::string runtime_hash(const std::vector<UploadData>& files)
{
    const auto item = std::find_if(files.begin(), files.end(), [](const UploadData& file) { return file.name == "udm-lcd"; });
    ensure(item != files.end(), "Runtime upload is missing");
    return hex_digest(digest256(item->data));
}

std::string upload_script(const std::vector<UploadData>& files, const std::vector<std::string>& target_arguments)
{
    std::string script;
    script += "set -eu\n";
    script += "umask 077\n";
    script += "archive=$(mktemp /run/udm-lcd-upload.XXXXXX)\n";
    script += "stage=$(mktemp -d /tmp/udm-lcd-stageXXXXXX)\n";
    script += "cleanup() { rm -rf -- \"$archive\" \"$stage\"; }\n";
    script += "trap cleanup EXIT HUP INT TERM\n";
    script += "head -c 100663297 > \"$archive\"\n";
    script += "size=$(wc -c < \"$archive\")\n";
    script += "test \"$size\" -le 100663296\n";
    script += "actual=$(tar -tf \"$archive\")\n";
    script += "expected=" + shell_quote(archive_members(files)) + "\n";
    script += "test \"$actual\" = \"$expected\"\n";
    for (const auto& file : files) {
        script += "tar -xOf \"$archive\" " + shell_quote(file.name) + " > \"$stage/" + file.name + "\"\n";
        script += "test -f \"$stage/" + file.name + "\"\n";
    }
    script += "test \"$(sha256sum \"$stage/udm-lcd\" | cut -d' ' -f1)\" = " + shell_quote(runtime_hash(files)) + "\n";
    script += "chmod 755 \"$stage/udm-lcd\"\n";
    for (const auto& file : files)
        if (file.name != "udm-lcd") script += "chmod 600 \"$stage/" + file.name + "\"\n";
    script += "cd \"$stage\"\n";
    std::vector<std::string> command;
    command.reserve(target_arguments.size() + 1);
    command.push_back("./udm-lcd");
    command.insert(command.end(), target_arguments.begin(), target_arguments.end());
    script += shell_join(command) + "\n";
    return script;
}

std::filesystem::path resolved_runtime(const Connection& connection)
{
    const auto path = connection.runtime.empty() ? project_root() / "build/aarch64/udm-lcd" : connection.runtime;
    return path.is_absolute() ? path : fs::absolute(path);
}

std::vector<Upload> media_uploads(const Connection& connection, const PlayOptions& options)
{
    const auto runtime = resolved_runtime(connection);
    load_pack(options.pack);
    return {{"udm-lcd", runtime}, {"manifest.json", options.pack / "manifest.json"},
            {"palette.bin", options.pack / "palette.bin"}, {"frames.bin", options.pack / "frames.bin"}};
}

std::string number_text(double value)
{
    std::ostringstream output;
    output << std::setprecision(17) << value;
    return output.str();
}

std::vector<std::string> install_arguments(const PlayOptions& options)
{
    std::vector<std::string> result{"target-install", ".", "--fps", number_text(options.fps),
                                    "--still-seconds", number_text(options.still_seconds)};
    if (options.background)
        result.push_back("--background");
    if (options.loop)
        result.push_back("--loop");
    else {
        result.push_back("--cycles");
        result.push_back(std::to_string(options.cycles));
    }
    if (!options.swap_rb)
        result.push_back("--native-colors");
    return result;
}

std::vector<std::string> playback_arguments(const PlayOptions& options)
{
    std::vector<std::string> result{"target-play", "--pack", target_base.string(), "--fps", number_text(options.fps),
                                    "--still-seconds", number_text(options.still_seconds), "--lease"};
    if (options.loop)
        result.push_back("--loop");
    else {
        result.push_back("--cycles");
        result.push_back(std::to_string(options.cycles));
    }
    if (!options.swap_rb)
        result.push_back("--native-colors");
    return result;
}

void write_stream(int descriptor, std::span<const std::uint8_t> bytes)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EPIPE || errno == EBADF))
            return;
        ensure(count > 0, "Output stream write failed");
        offset += static_cast<std::size_t>(count);
    }
}

void print_result(const ProcessResult& result)
{
    write_stream(STDOUT_FILENO, result.output);
    write_stream(STDERR_FILENO, {reinterpret_cast<const std::uint8_t*>(result.errors.data()), result.errors.size()});
}

class SignalGuard {
    struct sigaction old_int_{};
    struct sigaction old_pipe_{};
    bool active_ = false;
public:
    SignalGuard()
    {
        request_stop(0);
        struct sigaction action{};
        action.sa_handler = request_stop;
        sigemptyset(&action.sa_mask);
        ensure(::sigaction(SIGINT, &action, &old_int_) == 0, "SIGINT handler setup failed");
        struct sigaction ignore{};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        ensure(::sigaction(SIGPIPE, &ignore, &old_pipe_) == 0, "SIGPIPE handler setup failed");
        active_ = true;
    }
    SignalGuard(const SignalGuard&) = delete;
    SignalGuard& operator=(const SignalGuard&) = delete;
    ~SignalGuard()
    {
        if (active_) {
            ::sigaction(SIGINT, &old_int_, nullptr);
            ::sigaction(SIGPIPE, &old_pipe_, nullptr);
            request_stop(0);
        }
    }
};

struct ChildPipes {
    Fd input_read;
    Fd input_write;
    Fd output_read;
    Fd output_write;
    Fd error_read;
    Fd error_write;
};

ChildPipes make_child_pipes()
{
    std::array<int, 2> input{};
    std::array<int, 2> output{};
    std::array<int, 2> error{};
    ensure(::pipe2(input.data(), O_CLOEXEC) == 0, "SSH input pipe failed");
    Fd input_read(input[0]);
    Fd input_write(input[1]);
    ensure(::pipe2(output.data(), O_CLOEXEC) == 0, "SSH output pipe failed");
    Fd output_read(output[0]);
    Fd output_write(output[1]);
    ensure(::pipe2(error.data(), O_CLOEXEC) == 0, "SSH error pipe failed");
    Fd error_read(error[0]);
    Fd error_write(error[1]);
    return {std::move(input_read), std::move(input_write), std::move(output_read), std::move(output_write),
            std::move(error_read), std::move(error_write)};
}

void nonblocking(int descriptor)
{
    const auto flags = ::fcntl(descriptor, F_GETFL, 0);
    ensure(flags >= 0 && ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0, "SSH pipe configuration failed");
}

bool send_pipe_line(int descriptor, std::string_view line, double deadline)
{
    std::size_t offset = 0;
    while (offset < line.size()) {
        if (now() >= deadline)
            return false;
        pollfd event{descriptor, POLLOUT, 0};
        const auto timeout = static_cast<int>(std::max(1.0, std::min(100.0, (deadline - now()) * 1000.0)));
        const auto ready = ::poll(&event, 1, timeout);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 || event.revents & (POLLERR | POLLHUP | POLLNVAL))
            return false;
        if (!ready)
            continue;
        const auto count = ::write(descriptor, line.data() + offset, line.size() - offset);
        if (count < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (count <= 0)
            return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

int close_child(std::int32_t pid, int signal)
{
    if (pid > 0)
        ::kill(pid, signal);
    int status = 0;
    while (pid > 0 && ::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return status;
}

int foreground_remote(const Connection& connection, const PlayOptions& options)
{
    validate_host(connection.host);
    SignalGuard signals;
    const auto arguments = playback_arguments(options);
    std::vector<std::string> remote_arguments{(target_base / "udm-lcd").string()};
    remote_arguments.insert(remote_arguments.end(), arguments.begin(), arguments.end());
    const auto remote_command = shell_join(remote_arguments);
    std::vector<std::string> command{"ssh", "-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=8",
                                     "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2", connection.host,
                                     remote_command};
    auto pipes = make_child_pipes();
    sigset_t blocked{};
    sigset_t previous{};
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    ensure(::sigprocmask(SIG_BLOCK, &blocked, &previous) == 0, "SIGINT mask setup failed");
    const auto child = ::fork();
    if (child < 0) {
        ::sigprocmask(SIG_SETMASK, &previous, nullptr);
        fail("Cannot start SSH playback");
    }
    if (child == 0) {
        if (::dup2(pipes.input_read.get(), STDIN_FILENO) < 0 || ::dup2(pipes.output_write.get(), STDOUT_FILENO) < 0 ||
            ::dup2(pipes.error_write.get(), STDERR_FILENO) < 0)
            ::_exit(126);
        std::vector<char*> argv;
        argv.reserve(command.size() + 1);
        for (auto& value : command)
            argv.push_back(const_cast<char*>(value.c_str()));
        argv.push_back(nullptr);
        ::execvp(argv.front(), argv.data());
        ::_exit(127);
    }
    ensure(::sigprocmask(SIG_SETMASK, &previous, nullptr) == 0, "SIGINT mask restoration failed");
    pipes.input_read.reset();
    pipes.output_write.reset();
    pipes.error_write.reset();
    nonblocking(pipes.input_write.get());
    nonblocking(pipes.output_read.get());
    nonblocking(pipes.error_read.get());
    bool input_open = true;
    bool output_open = true;
    bool error_open = true;
    bool child_done = false;
    bool stop_sent = false;
    double stop_deadline = 0;
    double heartbeat = now() + 2;
    int status = 0;
    std::size_t output_bytes = 0;
    std::size_t error_bytes = 0;
    std::array<std::uint8_t, 65536> block{};
    try {
        while (input_open || output_open || error_open || !child_done) {
            if (stop_requested() && !stop_sent) {
                if (input_open && !send_pipe_line(pipes.input_write.get(), "STOP\n", now() + 2)) {
                    pipes.input_write.reset();
                    input_open = false;
                }
                stop_sent = true;
                stop_deadline = now() + 65;
            }
            if (!stop_sent && input_open && now() >= heartbeat) {
                if (!send_pipe_line(pipes.input_write.get(), "KEEP\n", now() + 2)) {
                    pipes.input_write.reset();
                    input_open = false;
                }
                heartbeat = now() + 2;
            }
            if (stop_sent && now() >= stop_deadline && !child_done) {
                status = close_child(child, SIGTERM);
                child_done = true;
                input_open = false;
            }
            std::array<pollfd, 3> events{{{output_open ? pipes.output_read.get() : -1, POLLIN, 0},
                                          {error_open ? pipes.error_read.get() : -1, POLLIN, 0},
                                          {input_open ? pipes.input_write.get() : -1, POLLOUT, 0}}};
            const auto ready = ::poll(events.data(), events.size(), 100);
            if (ready < 0 && errno != EINTR)
                fail("SSH playback polling failed");
            for (std::size_t index = 0; index < 2; ++index) {
                auto& open = index == 0 ? output_open : error_open;
                auto& descriptor = index == 0 ? pipes.output_read : pipes.error_read;
                if (!open || !events[index].revents)
                    continue;
                const auto count = ::read(descriptor.get(), block.data(), block.size());
                if (!count) {
                    descriptor.reset();
                    open = false;
                    continue;
                }
                if (count < 0 && (errno == EINTR || errno == EAGAIN))
                    continue;
                ensure(count > 0, "SSH playback output failed");
                if (index == 0) {
                    output_bytes += static_cast<std::size_t>(count);
                    ensure(output_bytes <= remote_output_cap, "SSH playback stdout exceeds limit");
                    write_stream(STDOUT_FILENO, {block.data(), static_cast<std::size_t>(count)});
                } else {
                    error_bytes += static_cast<std::size_t>(count);
                    ensure(error_bytes <= remote_output_cap, "SSH playback stderr exceeds limit");
                    write_stream(STDERR_FILENO, {block.data(), static_cast<std::size_t>(count)});
                }
            }
            if (!child_done) {
                const auto waited = ::waitpid(child, &status, WNOHANG);
                if (waited == child) {
                    child_done = true;
                    if (input_open) {
                        pipes.input_write.reset();
                        input_open = false;
                    }
                } else
                    ensure(waited >= 0 || errno == EINTR, "SSH playback wait failed");
            }
        }
    } catch (...) {
        pipes.input_write.reset();
        if (!child_done)
            close_child(child, SIGTERM);
        throw;
    }
    request_stop(0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

struct Metadata {
    struct stat info{};
};

Metadata lstat_checked(const fs::path& path, std::string_view label)
{
    Metadata metadata;
    ensure(::lstat(path.c_str(), &metadata.info) == 0, std::string(label) + " is unavailable: " + path.string());
    return metadata;
}

void require_root_directory(const fs::path& path, mode_t mode, std::string_view label)
{
    const auto metadata = lstat_checked(path, label).info;
    ensure(S_ISDIR(metadata.st_mode) && metadata.st_uid == 0 && metadata.st_gid == 0 &&
               (metadata.st_mode & 07777) == mode,
           std::string(label) + " metadata differs");
}

void require_root_file(const fs::path& path, mode_t mode, std::string_view label)
{
    const auto metadata = lstat_checked(path, label).info;
    ensure(S_ISREG(metadata.st_mode) && metadata.st_uid == 0 && metadata.st_gid == 0 &&
               (metadata.st_mode & 07777) == mode && metadata.st_nlink == 1,
           std::string(label) + " metadata differs");
}

void make_root_directory(const fs::path& path, mode_t mode, std::string_view label)
{
    std::error_code error;
    if (!fs::exists(path, error)) {
        ensure(!error && ::mkdir(path.c_str(), mode) == 0, "Unable to create " + std::string(label));
    }
    require_root_directory(path, mode, label);
}

void ensure_deploy_parent()
{
    const fs::path path = "/run/lock";
    std::error_code error;
    if (!fs::exists(path, error)) {
        ensure(!error && ::mkdir(path.c_str(), 0700) == 0, "Unable to create deployment lock directory");
        return;
    }
    const auto metadata = lstat_checked(path, "deployment lock directory").info;
    ensure(S_ISDIR(metadata.st_mode) && metadata.st_uid == 0 && metadata.st_gid == 0,
           "deployment lock directory metadata differs");
}

class ExclusiveLock {
    Fd descriptor_;
public:
    ExclusiveLock(const fs::path& path, mode_t mode, std::string_view label)
    {
        descriptor_.reset(::open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, mode));
        require_root_file(path, mode, label);
        ensure(::flock(descriptor_.get(), LOCK_EX | LOCK_NB) == 0,
               std::string(label) + " is already held");
    }
};

void verify_display_lock_free()
{
    ensure_deploy_parent();
    Fd descriptor(::open(display_lock_path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600));
    require_root_file(display_lock_path, 0600, "display operation lock");
    ensure(::flock(descriptor.get(), LOCK_EX | LOCK_NB) == 0, "The display is already in use; stop existing playback first");
}

bool service_exists(const Json::Value& service)
{
    return service["LoadState"].asString() != "not-found";
}

bool managed_service(const Json::Value& service)
{
    if (!service_exists(service))
        return false;
    ensure(service["Description"].asString() == background_description &&
               service["FragmentPath"].asString() == background_fragment,
           "Refusing to operate an unrelated service with the managed unit name");
    return true;
}

bool service_active(const Json::Value& service)
{
    const auto state = service["ActiveState"].asString();
    return state == "active" || state == "activating" || state == "deactivating";
}

std::string read_tail(const fs::path& path, std::size_t cap)
{
    const auto metadata = lstat_checked(path, "Background log").info;
    ensure(S_ISREG(metadata.st_mode) && metadata.st_nlink == 1 && metadata.st_uid == 0 && metadata.st_gid == 0,
           "Background log metadata differs");
    Fd descriptor(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    const auto end = ::lseek(descriptor.get(), 0, SEEK_END);
    ensure(end >= 0, "Background log seek failed");
    const auto begin = std::max<off_t>(0, end - static_cast<off_t>(cap));
    ensure(::lseek(descriptor.get(), begin, SEEK_SET) >= 0, "Background log seek failed");
    std::string result;
    result.reserve(static_cast<std::size_t>(end - begin));
    std::array<char, 8192> block{};
    while (result.size() < cap) {
        const auto count = ::read(descriptor.get(), block.data(), std::min(block.size(), cap - result.size()));
        if (count < 0 && errno == EINTR)
            continue;
        ensure(count >= 0, "Background log read failed");
        if (!count)
            break;
        result.append(block.data(), static_cast<std::size_t>(count));
    }
    return result;
}

Json::Value process_report(const Json::Value& service)
{
    Json::Value result;
    const auto pid_text = service["MainPID"].asString();
    if (pid_text.empty() || pid_text == "0" || !std::all_of(pid_text.begin(), pid_text.end(), [](unsigned char value) { return value >= '0' && value <= '9'; }))
        return result;
    const auto pid = positive_integer(pid_text, "service PID");
    try {
        Json::Value io;
        std::istringstream stream(node_text(fs::path("/proc") / pid_text / "io", 65536));
        for (std::string line; std::getline(stream, line);) {
            const auto split = line.find(':');
            if (split == std::string::npos)
                continue;
            const auto key = line.substr(0, split);
            const auto value = line.substr(split + 1);
            const auto first = value.find_first_not_of(" \t");
            if (first == std::string::npos)
                continue;
            std::uint64_t number = 0;
            const auto parsed = std::from_chars(value.data() + static_cast<std::ptrdiff_t>(first), value.data() + value.size(), number);
            if (parsed.ec == std::errc{})
                io[key] = Json::UInt64(number);
        }
        result["process_io"] = io;
        std::istringstream status(node_text(fs::path("/proc") / pid_text / "status", 65536));
        for (std::string line; std::getline(status, line);)
            if (line.starts_with("PPid:")) {
                const auto value = line.substr(5);
                const auto first = value.find_first_not_of(" \t");
                if (first != std::string::npos)
                    result["process_parent_pid"] = Json::UInt64(positive_integer(value.substr(first), "parent PID"));
            }
    } catch (const std::exception&) {
    }
    result["pid"] = Json::UInt64(pid);
    return result;
}

Json::Value background_report()
{
    const auto service = service_state(background_unit);
    const auto managed = managed_service(service);
    Json::Value result;
    result["unit"] = std::string(background_unit);
    result["managed"] = managed;
    result["service"] = service;
    result["independent_of_pc"] = managed && service["ActiveState"] == "active";
    result["survives_udm_reboot"] = false;
    std::error_code error;
    if (fs::exists(target_base, error)) {
        require(!error, "Target media directory stat failed");
        require_root_directory(target_base, 0700, "Target media directory");
        Json::Value files(Json::arrayValue);
        for (const auto& entry : fs::directory_iterator(target_base)) files.append(entry.path().filename().string());
        result["base_files"] = files;
        const auto log = target_base / "play.log";
        if (fs::exists(log, error)) {
            require(!error, "Background log stat failed");
            result["log_tail"] = read_tail(log, 16384);
        }
    }
    const auto process = process_report(service);
    for (const auto& key : process.getMemberNames()) result[key] = process[key];
    return result;
}

void verify_normal_display()
{
    const auto inspection = target_inspect();
    const auto& daemon = inspection["display_daemon"];
    ensure(daemon.isObject() && daemon["ActiveState"].asString() == "active", "Display daemon did not return active");
    const auto& owners = inspection["tty_owners"];
    ensure(owners.isArray() && owners.size() == 1, "Display daemon tty ownership was not restored");
    ensure(owners[0]["pid"].asString() == daemon["MainPID"].asString() && owners[0]["exe"].asString() == "/usr/bin/ulcmd" &&
               owners[0]["target"].asString() == "/dev/ttyACM0",
           "Display daemon does not own the application tty");
    ensure(inspection["application_usb"].asBool(), "Display application identity was not restored");
    const auto& display = inspection["display"];
    ensure(display.isObject() && display["state"].asString() == "ready", "Display was not restored to ready state");
    ensure(display["screen"].asString() == "menu.main" || display["screen"].asString() == "menu.network" ||
               display["screen"].asString() == "network.throughput",
           "Display was not restored to a normal menu");
}

void verify_normal_display_after_stop()
{
    const auto deadline = now() + 15;
    std::string reason = "display restoration did not complete";
    while (now() < deadline) {
        try {
            verify_normal_display();
            return;
        } catch (const std::exception& error) {
            reason = error.what();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    fail("Display cleanup verification failed: " + reason);
}

void stop_managed_service()
{
    auto service = service_state(background_unit);
    if (!managed_service(service) || !service_active(service))
        return;
    const auto result = execute({"systemctl", "stop", std::string(background_unit)}, {}, 80, 65536);
    ensure(result.code == 0, "Managed background stop failed: " + result.errors.substr(0, 512));
    const auto deadline = now() + 75;
    while (now() < deadline) {
        service = service_state(background_unit);
        if (!service_exists(service) || service["ActiveState"] == "inactive" || service["ActiveState"] == "failed")
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ensure(!service_active(service), "Managed background playback did not stop before its deadline");
    verify_normal_display_after_stop();
}

void check_stage(const fs::path& stage)
{
    const auto canonical = fs::canonical(stage);
    require_root_directory(canonical, 0700, "Upload stage");
    ensure(canonical.parent_path() == fs::path("/tmp") && canonical.filename().string().starts_with("udm-lcd-stage"),
           "Upload stage path differs");
    const std::set<std::string> allowed{"udm-lcd", "manifest.json", "palette.bin", "frames.bin"};
    for (const auto& entry : fs::directory_iterator(canonical)) {
        const auto name = entry.path().filename().string();
        ensure(allowed.contains(name), "Unexpected upload stage file");
        const auto mode = name == "udm-lcd" ? 0755 : 0600;
        require_root_file(entry.path(), mode, "Upload stage file");
    }
    for (const auto& name : allowed) ensure(fs::exists(canonical / name), "Missing upload stage file");
}

void check_base_contents()
{
    make_root_directory(target_base, 0700, "Target media directory");
    const std::set<std::string> allowed{"udm-lcd", "manifest.json", "palette.bin", "frames.bin", "play.log"};
    for (const auto& entry : fs::directory_iterator(target_base)) {
        const auto name = entry.path().filename().string();
        ensure(allowed.contains(name), "Unexpected target media file");
        const auto mode = name == "udm-lcd" ? 0755 : 0600;
        require_root_file(entry.path(), mode, "Target media file");
    }
}

void create_empty_log()
{
    Fd descriptor(::open((target_base / "play.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600));
    require_root_file(target_base / "play.log", 0600, "Background log");
    require(::fsync(descriptor.get()) == 0, "Background log sync failed");
}

void fsync_directory(const fs::path& path)
{
    Fd descriptor(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    require(::fsync(descriptor.get()) == 0, "Target media directory sync failed");
}

void copy_install_files(const fs::path& stage, const Pack& pack)
{
    const auto temporary = private_directory(".install-", target_base);
    {
        TemporaryDirectory cleanup(temporary);
        const auto runtime = read_file(stage / "udm-lcd", remote_archive_cap);
        const auto manifest = read_file(stage / "manifest.json", 1024u * 1024u);
        write_file(temporary / "udm-lcd", runtime, true);
        write_file(temporary / "manifest.json", manifest, true);
        write_file(temporary / "palette.bin", pack.palette, true);
        write_file(temporary / "frames.bin", pack.frames, true);
        require(::chmod((temporary / "udm-lcd").c_str(), 0755) == 0, "Runtime mode update failed");
        for (const auto& name : {"manifest.json", "palette.bin", "frames.bin"})
            require(::chmod((temporary / name).c_str(), 0600) == 0, "Media mode update failed");
        fsync_directory(temporary);
        for (const auto& name : {"udm-lcd", "manifest.json", "palette.bin", "frames.bin"})
            require(::rename((temporary / name).c_str(), (target_base / name).c_str()) == 0, "Target media replacement failed");
    }
    fsync_directory(target_base);
    check_base_contents();
}

void wait_background_ready()
{
    const auto deadline = now() + 125;
    while (now() < deadline) {
        const auto service = service_state(background_unit);
        require(managed_service(service), "Managed background unit changed during startup");
        const auto log = target_base / "play.log";
        const auto text = fs::exists(log) ? read_tail(log, 16384) : std::string();
        if (service["ActiveState"] == "active" && text.find("READY ") != std::string::npos)
            return;
        if (!service_active(service))
            fail("Background playback failed during startup: " + text.substr(text.size() > 8192 ? text.size() - 8192 : 0));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    fail("Background first-frame acknowledgement timed out");
}

void start_background(const PlayOptions& options)
{
    ensure(options.loop, "Background playback requires loop mode");
    create_empty_log();
    std::vector<std::string> command{"systemd-run", "--quiet", "--collect", "--unit=" + std::string(background_unit),
                                    "--description=" + std::string(background_description), "--property=KillSignal=SIGINT",
                                    "--property=KillMode=mixed", "--property=TimeoutStopSec=70", "--property=Restart=no",
                                    "--property=UMask=0077", "--property=StandardInput=null",
                                    "--property=Environment=UDM_LCD_BACKGROUND=1",
                                    "--property=StandardOutput=append:" + (target_base / "play.log").string(),
                                    "--property=StandardError=append:" + (target_base / "play.log").string(),
                                    (target_base / "udm-lcd").string(), "target-play", "--pack", target_base.string(),
                                    "--loop", "--fps", number_text(options.fps), "--still-seconds", number_text(options.still_seconds)};
    if (!options.swap_rb)
        command.push_back("--native-colors");
    const auto result = execute(command, {}, 20, 65536);
    ensure(result.code == 0, "Managed background start failed: " + result.errors.substr(0, 512));
    try {
        wait_background_ready();
    } catch (...) {
        stop_managed_service();
        throw;
    }
}

void add_ancestor_candidates(std::vector<fs::path>& candidates, fs::path path)
{
    std::error_code error;
    if (!path.is_absolute())
        path = fs::absolute(path, error);
    if (error)
        return;
    path = fs::weakly_canonical(path, error);
    if (error)
        return;
    for (;;) {
        candidates.push_back(path);
        if (path == path.root_path())
            break;
        path = path.parent_path();
    }
}

bool usable_project_root(const fs::path& candidate)
{
    std::error_code error;
    if (!fs::is_regular_file(candidate / "CMakeLists.txt", error) || error)
        return false;
    error.clear();
    if (!fs::is_regular_file(candidate / "firmware/frame_stream.cpp", error) || error)
        return false;
    error.clear();
    return fs::is_regular_file(candidate / "firmware/indexed_stream.cpp", error) && !error;
}

}

std::filesystem::path project_root()
{
    std::vector<fs::path> candidates;
    const fs::path source_file = __FILE__;
    if (source_file.is_absolute())
        add_ancestor_candidates(candidates, source_file.parent_path());
    std::error_code error;
    add_ancestor_candidates(candidates, fs::current_path(error));
    error.clear();
    const auto executable = fs::read_symlink("/proc/self/exe", error);
    if (!error && !executable.empty()) {
        add_ancestor_candidates(candidates, executable.parent_path());
        candidates.push_back(executable.parent_path() / "../share/udm-lcd");
    }
    for (const auto& candidate : candidates) {
        if (!usable_project_root(candidate))
            continue;
        error.clear();
        const auto canonical = fs::weakly_canonical(candidate, error);
        return error ? candidate : canonical;
    }
    fail("custom udp lcd project root is unavailable");
}

ProcessResult upload_remote(Connection connection, const std::vector<Upload>& files,
                            const std::vector<std::string>& target_arguments, double timeout)
{
    validate_host(connection.host);
    ensure(timeout > 0, "Invalid SSH timeout");
    for (const auto& argument : target_arguments)
        ensure(argument.size() <= 4096 && argument.find('\0') == std::string::npos, "Invalid remote argument");
    auto upload_files = files;
    if (std::none_of(upload_files.begin(), upload_files.end(), [](const Upload& file) { return file.name == "udm-lcd"; }))
        upload_files.insert(upload_files.begin(), {"udm-lcd", resolved_runtime(connection)});
    const auto data = read_uploads(upload_files);
    Bytes archive;
    archive.reserve(remote_archive_cap);
    for (const auto& file : data)
        append_tar_member(archive, file.name, file.data);
    archive.insert(archive.end(), 1024, 0);
    ensure(archive.size() <= remote_archive_cap, "Remote upload exceeds 96 MiB cap");
    TemporaryDirectory temporary(private_directory("udm-lcd-upload-"));
    write_file(temporary.path() / "upload.tar", archive, true);
    const auto verified_archive = read_file(temporary.path() / "upload.tar", remote_archive_cap);
    const auto script = upload_script(data, target_arguments);
    return execute({"ssh", "-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=8", "-o", "ServerAliveInterval=5",
                    "-o", "ServerAliveCountMax=2", connection.host, script}, verified_archive, timeout, remote_output_cap);
}

int host_play(Connection connection, const PlayOptions& options)
{
    ensure(!options.pack.empty(), "Playback pack is required");
    ensure(options.fps > 0 && options.fps <= 30 && options.still_seconds >= 1 && options.still_seconds <= 30 && options.cycles > 0,
           "Invalid playback options");
    const auto uploads = media_uploads(connection, options);
    const auto install = upload_remote(connection, uploads, install_arguments(options), 240);
    print_result(install);
    if (install.code != 0 || options.background)
        return install.code;
    return foreground_remote(connection, options);
}

int host_control(Connection connection, std::string action)
{
    ensure(action == "status" || action == "stop" || action == "inspect", "Unknown target control action");
    const auto runtime = resolved_runtime(connection);
    const auto result = upload_remote(connection, {{"udm-lcd", runtime}}, {"target-" + action}, 240);
    print_result(result);
    return result.code;
}

int target_install(const std::filesystem::path& stage, const PlayOptions& options)
{
    ensure(geteuid() == 0, "Target installation requires root");
    check_stage(stage);
    const auto canonical = fs::canonical(stage);
    const auto pack = load_pack(canonical);
    ensure(options.fps > 0 && options.fps <= 30 && options.still_seconds >= 1 && options.still_seconds <= 30 && options.cycles > 0,
           "Invalid playback options");
    ensure_deploy_parent();
    ExclusiveLock deployment(deploy_lock_path, 0600, "deployment lock");
    const auto service = service_state(background_unit);
    if (managed_service(service))
        ensure(!service_active(service), "Background media is already running; stop it before changing media");
    verify_display_lock_free();
    check_base_contents();
    copy_install_files(canonical, pack);
    if (options.background)
        start_background(options);
    Json::Value result;
    result["installed"] = true;
    result["base"] = target_base.string();
    result["background"] = options.background;
    if (options.background)
        result["service"] = background_report()["service"];
    std::cout << json_text(result, true) << '\n' << std::flush;
    return 0;
}

Json::Value target_control(std::string action)
{
    ensure(action == "status" || action == "stop" || action == "inspect", "Unknown target control action");
    if (action == "inspect")
        return target_inspect();
    if (action == "stop") {
        ensure_deploy_parent();
        ExclusiveLock deployment(deploy_lock_path, 0600, "deployment lock");
        stop_managed_service();
    }
    return background_report();
}

}
