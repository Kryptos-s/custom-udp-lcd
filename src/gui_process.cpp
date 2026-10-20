#include "gui_process.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <string_view>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace udm {
namespace {

constexpr std::size_t password_limit = 1024;
constexpr std::size_t output_limit = 256u * 1024u;
constexpr auto operation_timeout = std::chrono::seconds(300);
constexpr std::string_view timeout_message = "GUI command timed out after 300 seconds\n";
constexpr std::string_view password_socket_name = "UDM_GUI_PASSWORD_SOCKET";

void clear_secret(std::string& value)
{
    volatile char* data = value.data();
    for (std::size_t index = 0; index < value.size(); ++index)
        data[index] = 0;
    value.clear();
    value.shrink_to_fit();
}

int duplicate_above_stdio(int descriptor)
{
    if (descriptor > STDERR_FILENO)
        return descriptor;
    const int duplicate = ::fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (duplicate < 0)
        return -1;
    ::close(descriptor);
    return duplicate;
}

void close_descriptor(int& descriptor)
{
    if (descriptor >= 0) {
        ::close(descriptor);
        descriptor = -1;
    }
}

bool write_all(int descriptor, const char* data, std::size_t size)
{
    std::size_t offset = 0;
    while (offset < size) {
        const auto count = ::write(descriptor, data + offset, size - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

bool output_secret(const char* data, std::size_t size)
{
    if (!write_all(STDOUT_FILENO, data, size))
        return false;
    const char newline = '\n';
    return write_all(STDOUT_FILENO, &newline, 1);
}

bool read_socket_secret(std::string_view path)
{
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path))
        return false;
    int socket_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0)
        return false;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::copy(path.begin(), path.end(), address.sun_path);
    if (::connect(socket_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket_fd);
        return false;
    }
    std::array<char, password_limit + 1> response{};
    std::size_t size = 0;
    while (size < response.size()) {
        const auto count = ::read(socket_fd, response.data() + size, response.size() - size);
        if (count > 0) {
            size += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0)
            break;
        if (errno == EINTR)
            continue;
        ::close(socket_fd);
        return false;
    }
    ::close(socket_fd);
    if (size == response.size())
        return false;
    if (size > 0 && response[size - 1] == '\n')
        return write_all(STDOUT_FILENO, response.data(), size);
    return output_secret(response.data(), size);
}

std::string executable_path()
{
    return "/proc/" + std::to_string(::getpid()) + "/exe";
}

std::vector<std::string> child_environment(std::string askpass, std::string socket_path)
{
    std::vector<std::string> result;
    for (char** current = environ; current != nullptr && *current != nullptr; ++current) {
        const std::string_view entry(*current);
        const auto separator = entry.find('=');
        const auto name = entry.substr(0, separator);
        if (name == "SSH_ASKPASS" || name == "SSH_ASKPASS_REQUIRE" || name == password_socket_name)
            continue;
        result.emplace_back(entry);
    }
    result.push_back("SSH_ASKPASS=" + std::move(askpass));
    result.emplace_back("SSH_ASKPASS_REQUIRE=force");
    result.push_back(std::string(password_socket_name) + '=' + std::move(socket_path));
    return result;
}

std::vector<char*> pointer_vector(std::vector<std::string>& values)
{
    std::vector<char*> result;
    result.reserve(values.size() + 1);
    for (auto& value : values)
        result.push_back(value.data());
    result.push_back(nullptr);
    return result;
}

struct PasswordSocket {
    int descriptor = -1;
    std::string directory;
    std::string path;
};

PasswordSocket create_password_socket()
{
    std::array<char, 64> pattern{};
    std::snprintf(pattern.data(), pattern.size(), "/tmp/udm-gui-XXXXXX");
    if (::mkdtemp(pattern.data()) == nullptr)
        throw std::runtime_error("Cannot create GUI password directory");
    PasswordSocket result;
    result.directory = pattern.data();
    result.path = result.directory + "/askpass.sock";
    if (result.path.size() >= sizeof(sockaddr_un::sun_path)) {
        ::rmdir(result.directory.c_str());
        throw std::runtime_error("GUI password socket path is too long");
    }
    result.descriptor = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (result.descriptor < 0) {
        ::rmdir(result.directory.c_str());
        throw std::runtime_error("Cannot create GUI password socket");
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::copy(result.path.begin(), result.path.end(), address.sun_path);
    if (::bind(result.descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::chmod(result.path.c_str(), 0600) != 0 || ::listen(result.descriptor, 4) != 0) {
        close_descriptor(result.descriptor);
        ::unlink(result.path.c_str());
        ::rmdir(result.directory.c_str());
        throw std::runtime_error("Cannot bind GUI password socket");
    }
    return result;
}

void remove_password_socket(const std::string& path, const std::string& directory)
{
    if (!path.empty())
        ::unlink(path.c_str());
    if (!directory.empty())
        ::rmdir(directory.c_str());
}

}

struct GuiCommand::State {
    mutable std::mutex mutex;
    std::condition_variable server_condition;
    std::string captured;
    std::string password;
    std::string socket_path;
    std::string socket_directory;
    int read_descriptor = -1;
    int listen_descriptor = -1;
    pid_t process = -1;
    pid_t process_group = -1;
    int status = -1;
    bool finished = false;
    bool server_stop = false;
    bool server_done = false;
    bool server_started = false;

    ~State()
    {
        clear_secret(password);
    }
};

namespace {

void send_password(const std::shared_ptr<GuiCommand::State>& state, int client)
{
    struct ucred credentials{};
    socklen_t credentials_size = sizeof(credentials);
    if (::getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_size) != 0 ||
        credentials.uid != ::getuid()) {
        ::close(client);
        return;
    }
    std::array<char, password_limit + 1> response{};
    std::size_t size = 0;
    {
        std::lock_guard lock(state->mutex);
        size = state->password.size();
        if (size > password_limit) {
            ::close(client);
            return;
        }
        std::copy(state->password.begin(), state->password.end(), response.begin());
    }
    response[size++] = '\n';
    std::size_t offset = 0;
    while (offset < size) {
        const auto count = ::send(client, response.data() + offset, size - offset, MSG_NOSIGNAL);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
    std::fill(response.begin(), response.end(), 0);
    ::close(client);
}

void password_server(const std::shared_ptr<GuiCommand::State>& state)
{
    for (;;) {
        {
            std::lock_guard lock(state->mutex);
            if (state->server_stop)
                break;
        }
        pollfd descriptor{state->listen_descriptor, POLLIN, 0};
        const int result = ::poll(&descriptor, 1, 100);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (result == 0)
            continue;
        if ((descriptor.revents & POLLIN) == 0)
            continue;
        const int client = ::accept4(state->listen_descriptor, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EBADF || errno == EINVAL)
                break;
            continue;
        }
        send_password(state, client);
    }
    close_descriptor(state->listen_descriptor);
    {
        std::lock_guard lock(state->mutex);
        state->server_done = true;
    }
    state->server_condition.notify_all();
}

void stop_password_server(const std::shared_ptr<GuiCommand::State>& state)
{
    {
        std::lock_guard lock(state->mutex);
        state->server_stop = true;
    }
    state->server_condition.notify_all();
    std::unique_lock lock(state->mutex);
    state->server_condition.wait(lock, [&] { return state->server_done; });
    lock.unlock();
    remove_password_socket(state->socket_path, state->socket_directory);
}

void append_output(const std::shared_ptr<GuiCommand::State>& state, const char* data, std::size_t size)
{
    std::lock_guard lock(state->mutex);
    if (state->captured.size() >= output_limit)
        return;
    const auto count = std::min(size, output_limit - state->captured.size());
    state->captured.append(data, count);
}

void append_timeout(const std::shared_ptr<GuiCommand::State>& state)
{
    std::lock_guard lock(state->mutex);
    if (state->captured.size() + timeout_message.size() > output_limit)
        state->captured.resize(output_limit - timeout_message.size());
    state->captured.append(timeout_message);
}

void reap_process(const std::shared_ptr<GuiCommand::State>& state)
{
    const auto deadline = std::chrono::steady_clock::now() + operation_timeout;
    bool process_reaped = false;
    int process_status = -1;
    bool pipe_closed = false;
    bool timed_out = false;
    std::array<char, 8192> buffer{};
    auto trigger_timeout = [&] {
        if (timed_out || std::chrono::steady_clock::now() < deadline)
            return;
        timed_out = true;
        if (!process_reaped && state->process_group > 0)
            ::kill(-state->process_group, SIGKILL);
        if (!process_reaped && state->process > 0)
            ::kill(state->process, SIGKILL);
        append_timeout(state);
        close_descriptor(state->read_descriptor);
        pipe_closed = true;
    };
    while (!process_reaped || !pipe_closed) {
        trigger_timeout();
        if (!process_reaped) {
            int status = 0;
            const pid_t result = ::waitpid(state->process, &status, WNOHANG);
            if (result == state->process) {
                process_reaped = true;
                process_status = status;
            } else if (result < 0 && errno == ECHILD) {
                process_reaped = true;
            } else if (result < 0 && errno != EINTR) {
                process_reaped = true;
            }
        }
        if (!pipe_closed) {
            pollfd descriptor{state->read_descriptor, POLLIN | POLLHUP | POLLERR, 0};
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            const int timeout = timed_out ? 0 : static_cast<int>(std::clamp<std::int64_t>(
                remaining.count(), 0, 100));
            const int result = ::poll(&descriptor, 1, timeout);
            if (result < 0) {
                if (errno != EINTR)
                    pipe_closed = true;
            } else if (result > 0) {
                for (;;) {
                    const auto count = ::read(state->read_descriptor, buffer.data(), buffer.size());
                    if (count > 0) {
                        append_output(state, buffer.data(), static_cast<std::size_t>(count));
                        trigger_timeout();
                        if (pipe_closed)
                            break;
                        continue;
                    }
                    if (count == 0) {
                        pipe_closed = true;
                        break;
                    }
                    if (errno == EINTR)
                        continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        break;
                    pipe_closed = true;
                    break;
                }
            }
        } else if (!process_reaped) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            const int timeout = timed_out ? 10 : static_cast<int>(std::clamp<std::int64_t>(
                remaining.count(), 0, 100));
            ::poll(nullptr, 0, timeout);
        }
    }
    close_descriptor(state->read_descriptor);
    if (!process_reaped) {
        int status = 0;
        pid_t result = -1;
        do {
            result = ::waitpid(state->process, &status, 0);
        } while (result < 0 && errno == EINTR);
        if (result == state->process)
            process_status = status;
    }
    int exit_code = -1;
    if (timed_out)
        exit_code = 124;
    else if (WIFEXITED(process_status))
        exit_code = WEXITSTATUS(process_status);
    else if (WIFSIGNALED(process_status))
        exit_code = 128 + WTERMSIG(process_status);
    stop_password_server(state);
    {
        std::lock_guard lock(state->mutex);
        state->status = exit_code;
        state->finished = true;
    }
}

}

void gui_askpass()
{
    const char* socket_text = std::getenv(password_socket_name.data());
    if (socket_text == nullptr)
        return;
    const bool completed = read_socket_secret(socket_text);
    ::_exit(completed ? 0 : 1);
}

GuiCommand::GuiCommand() = default;

GuiCommand::~GuiCommand() = default;

void GuiCommand::start(std::vector<std::string> argv, std::string password)
{
    if (argv.empty())
        throw std::runtime_error("GUI command is empty");
    for (const auto& argument : argv)
        if (argument.find('\0') != std::string::npos)
            throw std::runtime_error("GUI command contains NUL");
    if (password.size() > password_limit || password.find('\0') != std::string::npos ||
        password.find('\n') != std::string::npos || password.find('\r') != std::string::npos)
        throw std::runtime_error("SSH password is invalid");
    if (state_ && busy())
        throw std::runtime_error("GUI command is already running");

    auto state = std::make_shared<State>();
    state->password = std::move(password);
    PasswordSocket socket;
    int read_fd = -1;
    int write_fd = -1;
    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attributes{};
    bool actions_initialized = false;
    bool attributes_initialized = false;
    bool spawned = false;
    try {
        socket = create_password_socket();
        state->listen_descriptor = socket.descriptor;
        state->socket_path = socket.path;
        state->socket_directory = socket.directory;
        socket.descriptor = -1;
        std::thread(password_server, state).detach();
        state->server_started = true;

        int descriptors[2]{};
        if (::pipe2(descriptors, O_CLOEXEC) != 0)
            throw std::runtime_error("Cannot create GUI output pipe");
        read_fd = duplicate_above_stdio(descriptors[0]);
        write_fd = duplicate_above_stdio(descriptors[1]);
        if (read_fd < 0 || write_fd < 0)
            throw std::runtime_error("Cannot prepare GUI output pipe");
        const int flags = ::fcntl(read_fd, F_GETFL);
        if (flags < 0 || ::fcntl(read_fd, F_SETFL, flags | O_NONBLOCK) != 0)
            throw std::runtime_error("Cannot configure GUI output pipe");

        std::vector<std::string> environment = child_environment(executable_path(), state->socket_path);
        std::vector<char*> child_argv = pointer_vector(argv);
        std::vector<char*> child_environment_pointers = pointer_vector(environment);

        if (posix_spawn_file_actions_init(&actions) != 0)
            throw std::runtime_error("Cannot initialize GUI process actions");
        actions_initialized = true;
        if (posix_spawn_file_actions_addclose(&actions, read_fd) != 0 ||
            posix_spawn_file_actions_adddup2(&actions, write_fd, STDOUT_FILENO) != 0 ||
            posix_spawn_file_actions_adddup2(&actions, write_fd, STDERR_FILENO) != 0 ||
            posix_spawn_file_actions_addclose(&actions, write_fd) != 0)
            throw std::runtime_error("Cannot configure GUI process actions");
        if (posix_spawnattr_init(&attributes) != 0)
            throw std::runtime_error("Cannot initialize GUI process attributes");
        attributes_initialized = true;
        if (posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP) != 0 ||
            posix_spawnattr_setpgroup(&attributes, 0) != 0)
            throw std::runtime_error("Cannot configure GUI process attributes");
        const int result = ::posix_spawnp(&state->process, argv.front().c_str(), &actions, &attributes,
                                          child_argv.data(), child_environment_pointers.data());
        if (result != 0)
            throw std::runtime_error(std::string("Cannot start GUI command: ") + std::strerror(result));
        spawned = true;
        state->process_group = state->process;
        state->read_descriptor = read_fd;
        read_fd = -1;
        close_descriptor(write_fd);
        if (actions_initialized)
            posix_spawn_file_actions_destroy(&actions);
        actions_initialized = false;
        if (attributes_initialized)
            posix_spawnattr_destroy(&attributes);
        attributes_initialized = false;
        std::thread(reap_process, state).detach();
        state_ = std::move(state);
    } catch (...) {
        if (actions_initialized)
            posix_spawn_file_actions_destroy(&actions);
        if (attributes_initialized)
            posix_spawnattr_destroy(&attributes);
        close_descriptor(read_fd);
        close_descriptor(write_fd);
        close_descriptor(socket.descriptor);
        if (spawned) {
            if (state->process_group > 0)
                ::kill(-state->process_group, SIGKILL);
            if (state->process > 0)
                ::kill(state->process, SIGKILL);
            int status = 0;
            pid_t result = -1;
            do {
                result = ::waitpid(state->process, &status, 0);
            } while (result < 0 && errno == EINTR);
            close_descriptor(state->read_descriptor);
            state->read_descriptor = -1;
            state->process = -1;
            state->process_group = -1;
        }
        if (state->server_started) {
            stop_password_server(state);
        } else {
            close_descriptor(state->listen_descriptor);
            remove_password_socket(state->socket_path, state->socket_directory);
        }
        throw;
    }
}

void GuiCommand::poll()
{
}

bool GuiCommand::busy() const
{
    if (!state_)
        return false;
    std::lock_guard lock(state_->mutex);
    return !state_->finished;
}

std::string GuiCommand::output() const
{
    if (!state_)
        return {};
    std::lock_guard lock(state_->mutex);
    return state_->captured;
}

int GuiCommand::exit_code() const
{
    if (!state_)
        return -1;
    std::lock_guard lock(state_->mutex);
    return state_->status;
}

}
