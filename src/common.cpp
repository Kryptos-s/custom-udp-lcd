#include "common.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <memory>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace udm {
Fd::Fd(int value) : value_(value) { require(value >= 0, std::string("Open failed: ") + std::strerror(errno)); }
Fd::~Fd() { reset(); }
Fd::Fd(Fd&& other) noexcept : value_(other.release()) {}
Fd& Fd::operator=(Fd&& other) noexcept { if (this != &other) reset(other.release()); return *this; }
int Fd::get() const { return value_; }
int Fd::release() { int value = value_; value_ = -1; return value; }
void Fd::reset(int value) { if (value_ >= 0) ::close(value_); value_ = value; }
void require(bool condition, std::string_view message) { if (!condition) throw std::runtime_error(std::string(message)); }
double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
Bytes read_file(const std::filesystem::path& path, std::size_t cap) {
    Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat info{};
    require(::fstat(fd.get(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0 && static_cast<std::uint64_t>(info.st_size) <= cap, "Expected bounded regular file: " + path.string());
    Bytes data;
    data.reserve(static_cast<std::size_t>(info.st_size));
    std::array<std::uint8_t, 65536> block{};
    for (;;) {
        auto count = ::read(fd.get(), block.data(), std::min(block.size(), cap + 1 - data.size()));
        if (count < 0 && errno == EINTR) continue;
        require(count >= 0, "File read failed: " + path.string());
        if (!count) break;
        data.insert(data.end(), block.begin(), block.begin() + count);
        require(data.size() <= cap, "File exceeds limit: " + path.string());
    }
    require(data.size() == static_cast<std::size_t>(info.st_size), "File changed while reading: " + path.string());
    return data;
}
std::string text_file(const std::filesystem::path& path, std::size_t cap) {
    auto data = read_file(path, cap);
    return {data.begin(), data.end()};
}
std::string node_text(const std::filesystem::path& path, std::size_t cap) {
    Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    std::string result;
    std::array<char, 4096> block{};
    for (;;) {
        auto count = ::read(fd.get(), block.data(), std::min(block.size(), cap + 1 - result.size()));
        if (count < 0 && errno == EINTR) continue;
        require(count >= 0, "Node read failed: " + path.string());
        if (!count) break;
        result.append(block.data(), static_cast<std::size_t>(count));
        require(result.size() <= cap, "Node exceeds limit: " + path.string());
    }
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ')) result.pop_back();
    return result;
}
void write_file(const std::filesystem::path& path, std::span<const std::uint8_t> data, bool exclusive) {
    Fd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | (exclusive ? O_EXCL : O_TRUNC), 0600));
    std::size_t offset = 0;
    while (offset < data.size()) {
        auto count = ::write(fd.get(), data.data() + offset, data.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "File write failed: " + path.string());
        offset += static_cast<std::size_t>(count);
    }
    require(::fsync(fd.get()) == 0, "File sync failed: " + path.string());
}
void write_text(const std::filesystem::path& path, std::string_view text, bool exclusive) {
    write_file(path, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, exclusive);
}
Json::Value parse_json(std::string_view text) {
    Json::CharReaderBuilder builder;
    builder["rejectDupKeys"] = true;
    builder["failIfExtra"] = true;
    builder["stackLimit"] = 64;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value value;
    std::string errors;
    require(reader->parse(text.data(), text.data() + text.size(), &value, &errors), "Invalid JSON: " + errors.substr(0, 256));
    return value;
}
std::string json_text(const Json::Value& value, bool pretty) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = pretty ? "  " : "";
    builder["commentStyle"] = "None";
    return Json::writeString(builder, value);
}
std::string quote(std::string_view text) {
    std::string result = "'";
    for (char value : text) result += value == '\'' ? "'\\''" : std::string(1, value);
    return result + "'";
}
ProcessResult execute(const std::vector<std::string>& command, std::span<const std::uint8_t> input, double timeout, std::size_t output_cap) {
    require(!command.empty() && timeout > 0 && output_cap > 0, "Invalid process limits");
    std::array<int, 2> in{}, out{}, err{};
    require(::pipe2(in.data(), O_CLOEXEC) == 0, "Input pipe failed");
    Fd in_read(in[0]), in_write(in[1]);
    require(::pipe2(out.data(), O_CLOEXEC) == 0, "Output pipe failed");
    Fd out_read(out[0]), out_write(out[1]);
    require(::pipe2(err.data(), O_CLOEXEC) == 0, "Error pipe failed");
    Fd err_read(err[0]), err_write(err[1]);
    std::vector<char*> arguments;
    for (const auto& value : command) arguments.push_back(const_cast<char*>(value.c_str()));
    arguments.push_back(nullptr);
    bool separate_group = std::filesystem::path(command.front()).filename() != "ssh";
    auto pid = ::fork();
    require(pid >= 0, "Cannot start process");
    if (!pid) {
        if (separate_group && ::setpgid(0, 0) < 0) ::_exit(126);
        if (::dup2(in_read.get(), STDIN_FILENO) < 0 || ::dup2(out_write.get(), STDOUT_FILENO) < 0 || ::dup2(err_write.get(), STDERR_FILENO) < 0) ::_exit(126);
        ::execvp(arguments.front(), arguments.data());
        ::_exit(127);
    }
    if (separate_group && ::setpgid(pid, pid) < 0 && errno != EACCES && errno != ESRCH) {
        ::kill(pid, SIGKILL);
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        throw std::runtime_error("Process group setup failed");
    }
    in_read.reset(); out_write.reset(); err_write.reset();
    for (int fd : {in_write.get(), out_read.get(), err_read.get()}) ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    ProcessResult result;
    std::size_t sent = 0;
    int status = 0;
    bool waited = false;
    double deadline = now() + timeout;
    std::array<std::uint8_t, 65536> block{};
    if (input.empty()) in_write.reset();
    try {
        while (!waited || out_read.get() >= 0 || err_read.get() >= 0) {
            require(now() < deadline, "Process timed out: " + command.front());
            std::array<pollfd, 3> events{{{in_write.get(), POLLOUT, 0}, {out_read.get(), POLLIN, 0}, {err_read.get(), POLLIN, 0}}};
            auto ready = ::poll(events.data(), events.size(), 50);
            require(ready >= 0 || errno == EINTR, "Process polling failed");
            if (events[0].revents && in_write.get() >= 0) {
                auto count = ::write(in_write.get(), input.data() + sent, std::min<std::size_t>(65536, input.size() - sent));
                if (count > 0) sent += static_cast<std::size_t>(count);
                else if (count < 0 && errno != EAGAIN && errno != EINTR && errno != EPIPE) throw std::runtime_error("Process input failed");
                if (sent == input.size() || (count < 0 && errno == EPIPE)) in_write.reset();
            }
            for (int index : {1, 2}) {
                auto& fd = index == 1 ? out_read : err_read;
                if (fd.get() < 0 || !events[index].revents) continue;
                auto count = ::read(fd.get(), block.data(), block.size());
                if (!count) { fd.reset(); continue; }
                if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                require(count > 0, "Process output failed");
                if (index == 1) {
                    require(result.output.size() + count <= output_cap, "Process output exceeds limit");
                    result.output.insert(result.output.end(), block.begin(), block.begin() + count);
                } else {
                    require(result.errors.size() + count <= 65536, "Process error output exceeds limit");
                    result.errors.append(reinterpret_cast<const char*>(block.data()), static_cast<std::size_t>(count));
                }
            }
            if (!waited) {
                auto child = ::waitpid(pid, &status, WNOHANG);
                require(child >= 0 || errno == EINTR, "Process wait failed");
                waited = child == pid;
            }
        }
    } catch (...) {
        ::kill(separate_group ? -pid : pid, SIGKILL);
        if (!waited) while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        throw;
    }
    result.code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return result;
}
std::filesystem::path executable_path() { return std::filesystem::canonical("/proc/self/exe"); }
std::string hex8(std::uint32_t value) { std::ostringstream output; output << std::uppercase << std::hex << std::setfill('0') << std::setw(8) << value; return output.str(); }
std::uint64_t positive_integer(std::string_view text, std::string_view name) {
    std::uint64_t value = 0;
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    require(result.ec == std::errc{} && result.ptr == text.data() + text.size() && value > 0, "Invalid " + std::string(name));
    return value;
}
}
