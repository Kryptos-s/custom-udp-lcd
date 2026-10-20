#include "player.hpp"
#include "media.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <poll.h>
#include <random>
#include <regex>
#include <set>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <termios.h>
#include <thread>
#include <unistd.h>

namespace udm {
namespace {
constexpr std::size_t frame_size = 57600;
constexpr std::string_view receiver_sha = "dc5404fe0de1f9a6e122f80efa03f685a93dbd907a4cc2820b1f01580cf4e2d2";
constexpr std::string_view receiver_prefix = "8c9321956b415adaff5c1ff586b6634f";
constexpr std::string_view daemon_sha = "5c1f0dc8e6673d0d5095e149d86b55c8b082adb94112feb342bbca7a0af096db";
std::atomic<int> stopped{0};
static_assert(std::atomic<int>::is_always_lock_free);
std::atomic<bool> cleaning{false};
class Stopped : public std::runtime_error {
public:
    Stopped() : std::runtime_error("Playback stopped by signal " + std::to_string(stopped.load())) {}
};
void check_stop() { if (stopped.load() && !cleaning.load()) throw Stopped(); }
void sleep_for(double seconds) {
    auto end = now() + std::max(0.0, seconds);
    while (now() < end) { check_stop(); std::this_thread::sleep_for(std::chrono::duration<double>(std::min(.05, end - now()))); }
}
class Signals {
    std::array<int, 4> numbers_{SIGHUP, SIGINT, SIGTERM, SIGALRM};
    std::array<struct sigaction, 4> old_{};
public:
    Signals() {
        stopped = 0;
        cleaning = false;
        struct sigaction action{};
        action.sa_handler = request_stop;
        sigemptyset(&action.sa_mask);
        for (std::size_t i = 0; i < numbers_.size(); ++i) require(sigaction(numbers_[i], &action, &old_[i]) == 0, "Signal handler setup failed");
    }
    ~Signals() { alarm(0); for (std::size_t i = 0; i < numbers_.size(); ++i) sigaction(numbers_[i], &old_[i], nullptr); cleaning = false; }
};
class Lease {
    std::atomic<bool> finished_{false};
    std::thread thread_;
public:
    explicit Lease(bool enabled) {
        if (!enabled) return;
        thread_ = std::thread([this] {
            std::string pending;
            double expires = now() + 10;
            int reason = SIGHUP;
            while (!finished_.load() && now() < expires) {
                pollfd event{STDIN_FILENO, POLLIN, 0};
                auto result = ::poll(&event, 1, 200);
                if (result < 0 && errno == EINTR) continue;
                if (result < 0) break;
                if (!result) continue;
                std::array<char, 256> block{};
                auto count = ::read(STDIN_FILENO, block.data(), block.size());
                if (count <= 0) break;
                pending.append(block.data(), static_cast<std::size_t>(count));
                bool stop = false;
                for (auto pos = pending.find('\n'); pos != std::string::npos; pos = pending.find('\n')) {
                    auto line = pending.substr(0, pos);
                    pending.erase(0, pos + 1);
                    if (line == "KEEP") expires = now() + 10;
                    else { reason = line == "STOP" ? SIGINT : SIGHUP; stop = true; break; }
                }
                if (stop || pending.size() > 64) break;
            }
            if (!finished_.load()) {
                request_stop(reason);
                double end = now() + 65;
                while (!finished_.load() && now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (!finished_.load()) ::_exit(124);
            }
        });
    }
    ~Lease() { finished_ = true; if (thread_.joinable()) thread_.join(); }
};
class OperationLock {
    Fd fd_;
public:
    OperationLock() : fd_(::open("/run/lock/udm-static-frame.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600)) {
        struct stat info{};
        require(fstat(fd_.get(), &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == 0 && info.st_gid == 0 && (info.st_mode & 0777) == 0600 && info.st_nlink == 1, "Display operation lock metadata differs");
        require(flock(fd_.get(), LOCK_EX | LOCK_NB) == 0, "The display is already in use; stop existing playback first");
    }
};
void ready_fd(int fd, short events, double deadline) {
    while (now() < deadline) {
        check_stop();
        pollfd event{fd, events, 0};
        auto timeout = static_cast<int>(std::max(1.0, std::min(100.0, (deadline - now()) * 1000)));
        int result = ::poll(&event, 1, timeout);
        if (result < 0 && errno == EINTR) continue;
        require(result >= 0, "Endpoint polling failed");
        if (!result) continue;
        require(!(event.revents & (POLLERR | POLLNVAL)), "Endpoint disconnected");
        if (event.revents & events) return;
        require(!(event.revents & POLLHUP), "Endpoint closed");
    }
    throw std::runtime_error("Endpoint deadline expired");
}
void send_bytes(int fd, std::span<const std::uint8_t> bytes, double deadline) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        ready_fd(fd, POLLOUT, deadline);
        auto count = ::write(fd, bytes.data() + offset, std::min<std::size_t>(16384, bytes.size() - offset));
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        require(count > 0, "Endpoint accepted no bytes");
        offset += static_cast<std::size_t>(count);
    }
}
Json::Value daemon_command(std::string_view name, const Json::Value& parameter, double deadline) {
    Json::Value request;
    request["command"] = std::string(name);
    request["sender"] = "udm-lcd";
    if (!parameter.isNull()) request["parameter"] = json_text(parameter);
    auto text = json_text(request) + '\n';
    require(text.size() <= 2048, "Daemon request exceeds limit");
    Fd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::string path = "/var/run/ulcmd_uds_server";
    std::copy(path.begin(), path.end(), address.sun_path);
    auto end = std::min(deadline, now() + 8);
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        require(errno == EINPROGRESS, "Display daemon connection failed");
        ready_fd(fd.get(), POLLOUT, end);
        int error = 0;
        socklen_t size = sizeof(error);
        require(::getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &error, &size) == 0 && !error, "Display daemon connection rejected");
    }
    send_bytes(fd.get(), {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, end);
    std::string response;
    while (response.find('\n') == std::string::npos) {
        ready_fd(fd.get(), POLLIN, end);
        std::array<char, 4096> block{};
        auto count = ::read(fd.get(), block.data(), block.size());
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        require(count > 0, "Daemon closed before replying");
        response.append(block.data(), static_cast<std::size_t>(count));
        require(response.size() <= 65536, "Daemon response exceeds limit");
    }
    auto newline = response.find('\n');
    require(response.find_first_not_of(" \t\r\n", newline) == std::string::npos, "Multiple daemon responses");
    auto result = parse_json(std::string_view(response).substr(0, newline));
    require(result.isObject() && result["status"] == "ok", "Display daemon rejected request: " + json_text(result).substr(0, 256));
    return result;
}
struct Event {
    std::string kind;
    std::uint32_t session = 0;
    std::uint32_t sequence = 0;
    std::uint32_t value = 0;
};
void scan_events(const Json::Value& value, std::deque<Event>& output, unsigned depth = 0, std::size_t* nodes = nullptr) {
    std::size_t count = 0;
    if (!nodes) nodes = &count;
    require(depth <= 16 && ++*nodes <= 4096, "Event nesting exceeds limit");
    if (value.isString()) {
        static const std::regex pattern(R"(FS1\s+r(allocated|top|bottom|ready|direct|armed|pending|painted|error|stopped|paints|bytes):([0-9A-F]{8})([0-9A-F]{8})([0-9A-F]{8})(?:$|\s))");
        auto text = value.asString();
        for (std::sregex_iterator item(text.begin(), text.end(), pattern), end; item != end; ++item) {
            output.push_back({(*item)[1], static_cast<std::uint32_t>(std::stoul((*item)[2], nullptr, 16)), static_cast<std::uint32_t>(std::stoul((*item)[3], nullptr, 16)), static_cast<std::uint32_t>(std::stoul((*item)[4], nullptr, 16))});
            require(output.size() <= 512, "Event queue exceeds limit");
        }
    } else if (value.isArray() || value.isObject()) for (const auto& child : value) scan_events(child, output, depth + 1, nodes);
}
class LogTail {
    Fd fd_;
    ino_t inode_ = 0;
    dev_t device_ = 0;
    std::string pending_;
    std::size_t budget_ = 2 * 1024 * 1024;
    void open(bool end) {
        fd_.reset(::open("/var/log/ulcmd.log", O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        require(fd_.get() >= 0, "Display log is unavailable");
        struct stat info{};
        require(fstat(fd_.get(), &info) == 0 && S_ISREG(info.st_mode), "Display log is not regular");
        inode_ = info.st_ino; device_ = info.st_dev;
        if (end) require(lseek(fd_.get(), 0, SEEK_END) >= 0, "Display log seek failed");
        pending_.clear();
    }
public:
    LogTail() { open(true); }
    std::vector<std::string> poll(bool& rotated) {
        struct stat current{};
        rotated = false;
        if (::stat("/var/log/ulcmd.log", &current) < 0) { require(errno == ENOENT, "Display log stat failed"); return {}; }
        auto offset = lseek(fd_.get(), 0, SEEK_CUR);
        if (current.st_ino != inode_ || current.st_dev != device_ || current.st_size < offset) { open(false); rotated = true; }
        std::array<char, 8192> block{};
        auto count = ::read(fd_.get(), block.data(), block.size());
        if (count < 0 && errno == EINTR) return {};
        require(count >= 0 && static_cast<std::size_t>(count) <= budget_, "Display log exceeds limit");
        budget_ -= static_cast<std::size_t>(count);
        pending_.append(block.data(), static_cast<std::size_t>(count));
        std::vector<std::string> lines;
        for (auto pos = pending_.find('\n'); pos != std::string::npos; pos = pending_.find('\n')) {
            require(pos <= 16384, "Display log line exceeds limit");
            lines.push_back(pending_.substr(0, pos));
            pending_.erase(0, pos + 1);
        }
        require(pending_.size() <= 16384, "Display log line exceeds limit");
        return lines;
    }
    void wait(std::string_view kind, std::uint32_t session, std::uint32_t sequence, double deadline) {
        while (now() < deadline) {
            bool rotated = false;
            for (const auto& line : poll(rotated)) {
                std::deque<Event> events;
                auto position = line.find(" Error LCM: ");
                if (position == std::string::npos) continue;
                auto payload = parse_json(std::string_view(line).substr(position + 12));
                scan_events(payload, events);
                for (const auto& event : events) if (event.session == session) {
                    require(event.kind != "error" && !(event.kind == kind && event.sequence == sequence && event.value), "MCU rejected startup: " + hex8(event.value));
                    if (event.kind == kind && event.sequence == sequence) return;
                }
            }
            sleep_for(.01);
        }
        throw std::runtime_error("MCU startup acknowledgement timed out: " + std::string(kind));
    }
};
Json::Value application_identity() {
    struct stat tty{};
    require(::stat("/dev/ttyACM0", &tty) == 0 && S_ISCHR(tty.st_mode) && major(tty.st_rdev) == 166 && minor(tty.st_rdev) == 0, "Application tty character-device identity differs");
    auto usb = std::filesystem::canonical("/sys/bus/usb/devices/1-2");
    auto path = std::filesystem::canonical("/sys/class/tty/ttyACM0/device");
    require(path.string().starts_with(usb.string() + '/'), "Application tty is attached to another USB device");
    Json::Value result;
    for (const auto& name : {"idVendor", "idProduct", "bcdDevice", "bNumInterfaces", "bConfigurationValue", "busnum", "devpath", "speed", "manufacturer", "product"}) result[name] = node_text(usb / name, 1024);
    require(result["idVendor"] == "1f9b" && result["idProduct"] == "157d", "Application USB VID/PID differs");
    if (std::filesystem::exists(usb / "serial")) result["serial"] = node_text(usb / "serial", 1024);
    result["physical_path"] = usb.string();
    result["tty_path"] = path.string();
    for (const auto& entry : std::filesystem::directory_iterator("/sys/bus/usb/devices")) {
        if (!std::filesystem::exists(entry.path() / "idVendor")) continue;
        require(node_text(entry.path() / "idVendor", 128) != "0483" || node_text(entry.path() / "idProduct", 128) != "df11", "A DFU device is present");
    }
    return result;
}
bool application_usb() {
    try { application_identity(); return true; }
    catch (const std::exception&) { return false; }
}
Json::Value stable_application() {
    auto first = application_identity();
    sleep_for(.25);
    auto second = application_identity();
    require(first == second, "Application USB identity changed between samples");
    return second;
}
void verify_service_contract(const Json::Value& service) {
    require(service["FragmentPath"] == "/lib/systemd/system/ulcmd.service" && service["DropInPaths"].asString().empty(), "Display service definition differs");
    require(hex_digest(digest256(read_file("/lib/systemd/system/ulcmd.service", 65536))) == "1273fa681ff355ae16b20798bbb9eb8fd09ab534a22b5240a49f246c8bd51b69", "Display service unit hash differs");
    auto start = service["ExecStart"].asString();
    require(start.starts_with("{ path=/usr/bin/ulcmd ; argv[]=/usr/bin/ulcmd ; ignore_errors=no ; ") && start.find("{ path=", 1) == std::string::npos, "Display service executable arguments differ");
}
void reject_updater() {
    const std::set<std::string> names{"dfu-util", "apply_static_frame.py", "transfer_static_frame.py", "probe-udm-display.py", "ubnt-upgrade", "fwupdate", "upgrade_firmware"};
    for (const auto& entry : std::filesystem::directory_iterator("/proc")) {
        auto name = entry.path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) continue;
        int handle = ::open((entry.path() / "cmdline").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (handle < 0 && (errno == ENOENT || errno == ESRCH)) continue;
        Fd descriptor(handle);
        std::string command;
        std::array<char, 4096> block{};
        while (command.size() < 16384) {
            auto count = ::read(descriptor.get(), block.data(), std::min(block.size(), 16384 - command.size()));
            if (count < 0 && errno == EINTR) continue;
            require(count >= 0, "Cannot inspect updater process arguments");
            if (!count) break;
            command.append(block.data(), static_cast<std::size_t>(count));
        }
        std::vector<std::string> words;
        for (std::size_t start = 0; start < command.size();) {
            auto end = command.find('\0', start);
            auto word = command.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!word.empty()) words.push_back(std::move(word));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        bool updater = std::any_of(words.begin(), words.end(), [&](const auto& word) { return names.contains(std::filesystem::path(word).filename().string()); });
        updater = updater || (!words.empty() && std::filesystem::path(words.front()).filename() == "ubnt-systool" && std::find(words.begin(), words.end(), "fwupdate") != words.end());
        require(!updater, "Display or firmware updater activity is present");
    }
}
bool daemon_owns(const Json::Value& service, const Json::Value& owners) {
    return owners.size() == 1 && owners[0]["pid"].asString() == service["MainPID"].asString() && owners[0]["exe"] == "/usr/bin/ulcmd" && owners[0]["target"] == "/dev/ttyACM0";
}
std::filesystem::path reset_control() {
    for (const auto& name : {"/sys/devices/platform/soc/soc:ulcm/dfu_or_reset", "/sys/devices/platform/ulcm/dfu_or_reset"}) {
        struct stat info{};
        if (::stat(name, &info) == 0 && info.st_uid == 0 && (info.st_mode & S_IWUSR)) return name;
    }
    throw std::runtime_error("Validated display reset control is unavailable");
}
void run_checked(const std::vector<std::string>& args, double deadline) {
    require(now() < deadline, "Service operation deadline expired");
    auto result = execute(args, {}, std::min(10.0, deadline - now()), 65536);
    require(result.code == 0, "Service operation failed: " + result.errors.substr(0, 256));
}
void verify_target() {
    require(geteuid() == 0, "Target playback requires root");
    std::istringstream info(node_text("/proc/ubnthal/system.info"));
    std::map<std::string, std::string> fields;
    for (std::string line; std::getline(info, line);) {
        auto split = line.find('=');
        if (split != std::string::npos) { auto key = line.substr(0, split); require(!fields.contains(key), "Duplicate model identity field"); fields[key] = line.substr(split + 1); }
    }
    require(fields["systemid"] == "ea15" && fields["shortname"] == "UDMPRO", "Requires the verified UDM Pro model");
    require(node_text("/usr/share/firmware/udm-pro-lcm-fw.version") == "v2.5.7-0-gd26945b", "Display firmware version differs");
    require(hex_digest(digest256(read_file("/usr/bin/ulcmd", 16 * 1024 * 1024))) == daemon_sha, "Display daemon binary differs");
    auto firmware = read_file("/usr/share/firmware/udm-pro-lcm-fw.bin", 0x100000);
    require(firmware.size() == 0x100000 && hex_digest(digest256(firmware)) == receiver_sha, "Exact indexed receiver is not installed");
    require(node_text("/usr/share/firmware/udm-pro-lcm-fw.hash") == receiver_prefix && hex_digest(digest_md5(std::span(firmware).first(0xE4F98))) == receiver_prefix, "Installed receiver prefix differs");
    stable_application();
    reject_updater();
}
void verify_live_hash(LogTail& tail, double deadline) {
    Json::Value request;
    request["system"]["get"] = "hash";
    daemon_command("message", request, deadline);
    std::map<std::string, std::string> fields;
    static const std::regex pattern(R"(Response: (md5|target) (.+?) unhandled *$)");
    auto end = std::min(deadline, now() + 8);
    while (now() < end) {
        bool rotated = false;
        auto lines = tail.poll(rotated);
        if (rotated) { fields.clear(); daemon_command("message", request, end); continue; }
        for (const auto& line : lines) {
            std::smatch match;
            if (std::regex_search(line, match, pattern)) {
                auto key = match[1].str();
                require(!fields.contains(key), "Ambiguous duplicate MCU identity response");
                auto value = parse_json(match[2].str());
                require(value.isString(), "Malformed MCU identity response");
                fields[key] = value.asString();
            }
        }
        if (fields.size() == 2) {
            require(fields["md5"] == receiver_prefix && fields["target"] == "udm-pro-lcm-fw", "Running MCU identity differs");
            return;
        }
        sleep_for(.02);
    }
    throw std::runtime_error("Fresh running MCU identity was not received");
}
class Tty {
    Fd fd_;
    termios previous_{};
    bool configured_ = false;
public:
    void open() {
        require(service_state("ulcmd.service")["ActiveState"] == "inactive" && tty_owners().empty(), "Exclusive tty takeover requires an inactive daemon and no owners");
        fd_.reset(::open("/dev/ttyACM0", O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC));
        require(fd_.get() >= 0, "Cannot open application tty");
        require(tcgetattr(fd_.get(), &previous_) == 0, "Cannot read tty settings");
        require(ioctl(fd_.get(), TIOCEXCL) == 0, "Cannot exclusively own tty");
        auto raw = previous_;
        cfmakeraw(&raw);
        require(tcsetattr(fd_.get(), TCSANOW, &raw) == 0, "Cannot set raw tty mode");
        configured_ = true;
        auto owners = tty_owners();
        require(!owners.empty(), "TTY ownership could not be confirmed");
        for (const auto& owner : owners) require(owner["pid"].asString() == std::to_string(getpid()), "Another process owns the tty");
    }
    int get() const { return fd_.get(); }
    void close() {
        if (fd_.get() < 0) return;
        int error = 0;
        if (configured_ && tcsetattr(fd_.get(), TCSANOW, &previous_) < 0) error = errno;
        ioctl(fd_.get(), TIOCNXCL);
        fd_.reset();
        configured_ = false;
        require(!error || error == EIO || error == ENODEV, "TTY settings restoration failed");
    }
    ~Tty() { try { close(); } catch (...) {} }
};
struct Rejected : std::runtime_error {
    Json::Value reply;
    explicit Rejected(Json::Value value) : std::runtime_error("MCU rejected request: " + json_text(value).substr(0, 256)), reply(std::move(value)) {}
};
class Serial {
    int fd_;
    std::string pending_;
    std::map<std::uint32_t, Json::Value> replies_;
    std::deque<Event> events_;
    std::uint32_t identifier_ = 1;
    std::size_t remaining_ = 4 * 1024 * 1024;
    void poll(double deadline) {
        ready_fd(fd_, POLLIN, deadline);
        std::array<char, 8192> bytes{};
        auto count = ::read(fd_, bytes.data(), bytes.size());
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) return;
        require(count > 0 && static_cast<std::size_t>(count) <= remaining_, "Serial disconnected or response budget exceeded");
        remaining_ -= static_cast<std::size_t>(count);
        pending_.append(bytes.data(), static_cast<std::size_t>(count));
        for (auto pos = pending_.find('\n'); pos != std::string::npos; pos = pending_.find('\n')) {
            require(pos <= 16384, "Serial line exceeds limit");
            auto line = pending_.substr(0, pos);
            pending_.erase(0, pos + 1);
            if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
            auto value = parse_json(line);
            require(value.isObject(), "Serial response must be an object");
            scan_events(value, events_);
            if (value["id"].isUInt() && value.isMember("status")) {
                auto id = value["id"].asUInt();
                require(!replies_.contains(id) && replies_.size() < 32, "Duplicate or excessive serial replies");
                replies_.emplace(id, std::move(value));
            }
        }
        require(pending_.size() <= 16384, "Serial line exceeds limit");
    }
public:
    explicit Serial(int fd) : fd_(fd), identifier_(std::random_device{}() % 1000000000 + 1) {}
    void write(std::span<const std::uint8_t> data, double deadline) { send_bytes(fd_, data, deadline); }
    void errors(std::uint32_t session) const {
        for (const auto& event : events_) require(event.session != session || event.kind != "error", "MCU frame error: " + hex8(event.value));
    }
    Json::Value request(Json::Value payload, double deadline) {
        require(identifier_ < 2147483647, "Serial request identifiers exhausted");
        auto id = ++identifier_;
        payload["id"] = id;
        auto text = json_text(payload) + '\n';
        require(text.size() < 1024, "Serial request exceeds firmware limit");
        remaining_ = 4 * 1024 * 1024;
        write({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, deadline);
        while (!replies_.contains(id)) poll(deadline);
        auto result = std::move(replies_.at(id));
        replies_.erase(id);
        if (result["status"] != "ok") throw Rejected(result);
        return result;
    }
    Event event(std::string_view kind, std::uint32_t session, double deadline) {
        for (;;) {
            while (!events_.empty()) {
                auto event = std::move(events_.front());
                events_.pop_front();
                if (event.session != session) continue;
                require(event.kind != "error", "MCU indexed error: " + hex8(event.value));
                if (event.kind == kind) { require(!event.value, "MCU acknowledgement contains an error"); return event; }
            }
            poll(deadline);
        }
    }
    void discard(std::uint32_t session, std::uint32_t sequence) {
        std::erase_if(events_, [=](const Event& event) { return event.kind != "error" && event.session == session && event.sequence <= sequence; });
    }
};
Json::Value control(std::string_view record) { Json::Value result; result["ui"]["test"] = "lcd"; result["ui"]["parameter"] = std::string(record); return result; }
void reset_application(const std::filesystem::path& path, const Json::Value& expected, double deadline) {
    require(service_state("ulcmd.service")["ActiveState"] == "inactive" && tty_owners().empty(), "Display reset requires unowned tty and stopped daemon");
    Fd fd(::open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW));
    require(::write(fd.get(), "0", 1) == 1, "Display application reset failed");
    sleep_for(1);
    auto end = std::min(deadline, now() + 12);
    std::string reason;
    while (now() < end) {
        try { require(stable_application() == expected, "Application USB identity changed after reset"); return; } catch (const std::exception& error) { reason = error.what(); }
        sleep_for(.1);
    }
    throw std::runtime_error("Display application did not return after reset: " + reason);
}
void restore_daemon(double deadline) {
    auto current = service_state("ulcmd.service");
    verify_service_contract(current);
    if (current["ActiveState"] == "active" && daemon_owns(current, tty_owners()) && application_usb()) return;
    require(application_usb() && tty_owners().empty(), "Daemon restoration requires unowned application tty");
    stable_application();
    run_checked({"systemctl", "start", "ulcmd.service"}, deadline);
    auto end = std::min(deadline, now() + 10);
    while (now() < end) { current = service_state("ulcmd.service"); if (current["ActiveState"] == "active" && daemon_owns(current, tty_owners())) return; sleep_for(.1); }
    throw std::runtime_error("Display daemon ownership did not return");
}
}
void request_stop(int signal) { stopped.store(signal); }
bool stop_requested() { return stopped.load() != 0; }
Json::Value service_state(std::string_view unit) {
    auto process = execute({"systemctl", "show", std::string(unit), "-p", "LoadState", "-p", "ActiveState", "-p", "SubState", "-p", "MainPID", "-p", "Description", "-p", "FragmentPath", "-p", "DropInPaths", "-p", "ExecStart", "-p", "Result"}, {}, 8, 65536);
    std::string text(process.output.begin(), process.output.end());
    require(!text.empty(), "Service state unavailable: " + process.errors.substr(0, 256));
    Json::Value result;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        auto line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        auto split = line.find('=');
        if (split != std::string::npos) result[line.substr(0, split)] = line.substr(split + 1);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}
Json::Value tty_owners() {
    Json::Value result(Json::arrayValue);
    for (const auto& entry : std::filesystem::directory_iterator("/proc")) {
        auto pid = entry.path().filename().string();
        if (pid.empty() || !std::all_of(pid.begin(), pid.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) continue;
        try {
            auto executable = std::filesystem::read_symlink(entry.path() / "exe").string();
            for (const auto& fd : std::filesystem::directory_iterator(entry.path() / "fd")) {
                std::error_code error;
                auto target = std::filesystem::read_symlink(fd.path(), error).string();
                if (!error && target.starts_with("/dev/ttyACM")) {
                    Json::Value owner;
                    owner["pid"] = pid; owner["exe"] = executable; owner["target"] = target;
                    result.append(owner);
                }
            }
        } catch (const std::filesystem::filesystem_error&) {}
    }
    return result;
}
Json::Value target_inspect() {
    Json::Value result;
    result["background"] = service_state("udm-lcd-media.service");
    result["display_daemon"] = service_state("ulcmd.service");
    result["tty_owners"] = tty_owners();
    result["application_usb"] = application_usb();
    auto firmware = read_file("/usr/share/firmware/udm-pro-lcm-fw.bin", 0x100000);
    result["firmware_sha256"] = hex_digest(digest256(firmware));
    result["firmware_prefix_md5"] = hex_digest(digest_md5(std::span(firmware).first(std::min<std::size_t>(firmware.size(), 0xE4F98))));
    if (result["display_daemon"]["ActiveState"] == "active" && daemon_owns(result["display_daemon"], result["tty_owners"])) result["display"] = daemon_command("dump", {}, now() + 8)["response"];
    return result;
}
int target_play(const PlayOptions& options) {
    require(options.fps > 0 && options.fps <= 30 && options.still_seconds >= 1 && options.still_seconds <= 30 && options.cycles > 0, "Invalid playback options");
    auto pack = load_pack(options.pack);
    Signals signals;
    Lease lease(options.lease);
    OperationLock lock;
    alarm(120);
    auto session = std::random_device{}();
    if (!session) session = 1;
    const auto& records = pack.manifest["frames"];
    auto count = records.size();
    auto duration_us = pack.manifest["source"]["duration_us"].asUInt64();
    std::vector<Bytes> cache(count);
    Tty tty;
    std::unique_ptr<Serial> serial;
    bool changed = false, takeover = false, raw = false, night = false, reset_needed = false;
    std::filesystem::path reset_path;
    Json::Value usb_before;
    std::uint32_t sequence = 0;
    std::uint64_t uploaded = 0, animated = 0;
    double animation_start = 0, held_from = 0, finished = 0, latency_sum = 0, latency_max = 0;
    std::uint64_t queries = 0, retries = 0;
    Json::Value summary;
    summary["session"] = hex8(session);
    summary["completed"] = false;
    summary["cleanup_errors"] = Json::Value(Json::arrayValue);
    summary["playback_completed"] = false;
    summary["loop"] = options.loop;
    summary["requested_cycles"] = options.loop ? Json::Value{} : Json::Value(Json::UInt64(options.cycles));
    auto frame_indices = [&](Json::ArrayIndex index) {
        auto offset = records[index]["offset"].asUInt64();
        return std::span<const std::uint8_t>(pack.frames).subspan(static_cast<std::size_t>(offset), frame_size);
    };
    auto frame = [&](Json::ArrayIndex index) {
        check_stop();
        require(sequence < std::numeric_limits<std::uint32_t>::max(), "Frame sequence exhausted");
        auto next = sequence + 1;
        auto wire = cache[index].empty() ? wire_frame(session, next, frame_indices(index), pack.palette, options.swap_rb) : cache[index];
        if (!cache[index].empty()) for (unsigned byte = 0; byte < 4; ++byte) wire[8 + byte] = static_cast<std::uint8_t>(next >> (8 * byte));
        auto start = now();
        auto deadline = start + 8;
        raw = true;
        Json::Value header;
        header["binary"] = Json::Value(Json::objectValue);
        serial->request(header, deadline);
        serial->errors(session);
        serial->write(wire, deadline);
        serial->request(Json::Value(Json::objectValue), deadline);
        raw = false;
        serial->errors(session);
        bool ready = false;
        while (now() < deadline) {
            Json::Value query;
            query["binary"]["painted"] = Json::Value{};
            ++queries;
            try { serial->request(query, deadline); ready = true; }
            catch (const Rejected& error) {
                serial->errors(session);
                require(error.reply["status"] == "error" && error.reply["error"] == "Binary messages not supported" && error.reply.size() == 3, "Unexpected MCU readiness rejection");
                ++retries;
            }
            serial->errors(session);
            if (ready) break;
            sleep_for(.005);
        }
        require(ready, "MCU ACTIVE readiness timed out");
        serial->discard(session, next);
        sequence = next;
        uploaded += wire.size();
        latency_sum += now() - start;
        latency_max = std::max(latency_max, now() - start);
    };
    try {
        auto deadline = now() + 85;
        auto cache_end = now() + 10;
        for (Json::ArrayIndex index = 0; index < count && now() < cache_end; ++index) { check_stop(); cache[index] = wire_frame(session, 1, frame_indices(index), pack.palette, options.swap_rb); }
        verify_target();
        usb_before = application_identity();
        auto service = service_state("ulcmd.service");
        verify_service_contract(service);
        require(service["ActiveState"] == "active" && daemon_owns(service, tty_owners()), "Exact display daemon tty ownership is required");
        reset_path = reset_control();
        LogTail log;
        auto state = daemon_command("dump", {}, deadline)["response"];
        require(state["night.mode"].isBool() && state["state"] == "ready" && (state["screen"] == "menu.main" || state["screen"] == "menu.network" || state["screen"] == "network.throughput"), "Known ready menu and night mode are required");
        night = state["night.mode"].asBool();
        verify_live_hash(log, deadline);
        changed = true;
        if (night) { Json::Value light; light["ui"]["backlight"] = true; daemon_command("message", light, deadline); }
        auto prefix = "R:";
        auto startup = [&](char command, std::string_view marker, std::uint32_t value) {
            auto record = std::string(prefix) + command + ':' + hex8(session);
            daemon_command("message", control(record), deadline);
            log.wait(marker, session, value, std::min(deadline, now() + 8));
        };
        startup('B', "allocated", 0);
        for (std::uint32_t index = 1; index <= 6; ++index) startup('A', "top", index);
        startup('A', "bottom", 0); startup('A', "ready", 0); startup('D', "direct", 0);
        takeover = true;
        run_checked({"systemctl", "stop", "ulcmd.service"}, deadline);
        tty.open();
        serial = std::make_unique<Serial>(tty.get());
        alarm(0);
        frame(0);
        held_from = now();
        std::cerr << "First frame acknowledged.\n" << std::flush;
        std::cout << "READY " << hex8(session) << '\n' << std::flush;
        auto hold_end = count == 1 ? (options.loop ? std::numeric_limits<double>::infinity() : now() + options.still_seconds) : now() + 1;
        auto health = now() + 1;
        while (now() < hold_end) {
            if (count == 1 && options.loop && now() >= health) {
                Json::Value query;
                query["binary"]["painted"] = Json::Value{};
                serial->request(query, now() + 8);
                serial->errors(session);
                health = now() + 1;
                ++queries;
            }
            sleep_for(std::min(.1, hold_end - now()));
        }
        if (count > 1) {
            animation_start = now();
            std::pair<std::uint64_t, Json::ArrayIndex> previous{std::numeric_limits<std::uint64_t>::max(), 0};
            auto next = animation_start;
            for (;;) {
                check_stop();
                auto time = now();
                auto elapsed = static_cast<std::uint64_t>(std::max(0.0, time - animation_start) * 1000000);
                auto cycle = elapsed / duration_us;
                if (!options.loop && cycle >= options.cycles) break;
                if (time < next) { sleep_for(std::min(.01, next - time)); continue; }
                auto phase = elapsed % duration_us;
                Json::ArrayIndex index = 0;
                while (index + 1 < count && records[index + 1]["start_us"].asUInt64() <= phase) ++index;
                std::pair<std::uint64_t, Json::ArrayIndex> current{cycle, index};
                if (current != previous) { frame(index); ++animated; previous = current; next = time + 1 / options.fps; }
                else next = time + std::min(.01, 1 / options.fps);
            }
        }
        summary["playback_completed"] = true;
    } catch (const Stopped& error) {
        if (stopped.load() == SIGINT || stopped.load() == SIGTERM) { summary["stopped_by_user"] = true; summary["stop_reason"] = "user"; }
        else summary["failure"] = error.what();
    } catch (const std::exception& error) { summary["failure"] = error.what(); }
    finished = now();
    cleaning = true;
    alarm(0);
    auto cleanup_deadline = now() + 45;
    auto cleanup_error = [&](std::string_view stage, const std::exception& error) { summary["cleanup_errors"].append(std::string(stage) + ": " + error.what()); };
    reset_needed = raw;
    if (serial && !raw) {
        try {
            auto deadline = std::min(cleanup_deadline, now() + 4);
            serial->request(control("R:S:" + hex8(session)), deadline);
            serial->event("stopped", session, deadline);
            summary["stream_detachment"] = "matching MCU stopped acknowledgement";
        } catch (const std::exception& error) { cleanup_error("indexed stop", error); reset_needed = true; }
    }
    try { tty.close(); } catch (const std::exception& error) { cleanup_error("tty close", error); reset_needed = true; }
    bool safe = !reset_needed;
    if (reset_needed) {
        try { reset_application(reset_path, usb_before, cleanup_deadline); safe = true; summary["stream_detachment"] = "display application reset confirmed"; }
        catch (const std::exception& error) { cleanup_error("reset required; daemon left stopped", error); }
    }
    bool restored = !takeover;
    if (takeover && safe) {
        try { restore_daemon(cleanup_deadline); restored = true; }
        catch (const std::exception& error) { cleanup_error("daemon restore", error); }
    }
    if (changed && restored) {
        try {
            Json::Value display;
            display["ui"]["state"] = "blank";
            daemon_command("message", display, cleanup_deadline);
            sleep_for(.2);
            display["ui"]["state"] = "ready";
            daemon_command("message", display, cleanup_deadline);
            if (night) { Json::Value light; light["ui"]["backlight"] = false; daemon_command("message", light, cleanup_deadline); }
            summary["normal_display"] = "blank/ready and prior night backlight accepted";
        } catch (const std::exception& error) { cleanup_error("normal display", error); }
    }
    auto elapsed = animation_start > 0 ? finished - animation_start : 0;
    auto cycles = animation_start > 0 ? static_cast<std::uint64_t>(elapsed * 1000000) / duration_us : 0;
    summary["painted_frames"] = sequence;
    summary["animation_painted_frames"] = Json::UInt64(animated);
    summary["animation_elapsed_seconds"] = elapsed;
    summary["animation_effective_fps"] = elapsed > 0 ? static_cast<double>(animated) / elapsed : 0;
    summary["cycles_completed"] = Json::UInt64(options.loop ? cycles : std::min(cycles, options.cycles));
    summary["uploaded_wire_bytes"] = Json::UInt64(uploaded);
    summary["source_frames"] = count;
    summary["readiness_queries"] = Json::UInt64(queries);
    summary["readiness_retries"] = Json::UInt64(retries);
    summary["frame_latency_mean_seconds"] = sequence ? latency_sum / sequence : 0;
    summary["frame_latency_max_seconds"] = latency_max;
    summary["raw_body_uncertain"] = raw;
    summary["daemon_restored"] = restored;
    if (count == 1 && held_from > 0) summary["still_hold_seconds"] = finished - held_from;
    summary["completed"] = !summary.isMember("failure") && summary["cleanup_errors"].empty() && restored && (summary["playback_completed"].asBool() || summary.get("stopped_by_user", false).asBool());
    std::cout << json_text(summary, true) << '\n' << std::flush;
    return summary["completed"].asBool() ? 0 : 1;
}
}
