#include "common.hpp"
#include "device.hpp"
#include "firmware.hpp"
#include "media.hpp"
#include "player.hpp"
#include "remote.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fcntl.h>
#include <iostream>
#include <random>
#include <signal.h>
#include <unistd.h>

namespace {
struct Arguments {
    udm::PlayOptions play;
    udm::Connection connection;
    std::filesystem::path source;
    bool cycles = false;
    bool loop = false;
};
double number(std::string_view text, std::string_view name) {
    double value = 0;
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    udm::require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(value), "Invalid " + std::string(name));
    return value;
}
Arguments options(const std::vector<std::string>& args) {
    Arguments result;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const auto& argument = args[index];
        auto value = [&]() -> const std::string& { udm::require(index + 1 < args.size(), "Missing value for " + argument); return args[++index]; };
        if (argument == "--pack") result.play.pack = value();
        else if (argument == "--host") result.connection.host = value();
        else if (argument == "--runtime") result.connection.runtime = value();
        else if (argument == "--cycles") { result.play.cycles = udm::positive_integer(value(), "cycle count"); result.cycles = true; }
        else if (argument == "--loop") { result.play.loop = true; result.loop = true; }
        else if (argument == "--background") { result.play.background = true; result.play.loop = true; }
        else if (argument == "--lease") result.play.lease = true;
        else if (argument == "--native-colors") result.play.swap_rb = false;
        else if (argument == "--fps") result.play.fps = number(value(), "FPS");
        else if (argument == "--still-seconds") result.play.still_seconds = number(value(), "picture hold");
        else if (!argument.empty() && argument.front() != '-') { udm::require(result.source.empty(), "Only one media file is allowed"); result.source = argument; }
        else throw std::runtime_error("Unknown option: " + argument);
    }
    udm::require(!(result.cycles && (result.loop || result.play.background)), "--cycles cannot be combined with --loop or --background");
    udm::require(result.play.fps > 0 && result.play.fps <= 30, "FPS must be greater than zero and at most 30");
    udm::require(result.play.still_seconds >= 1 && result.play.still_seconds <= 30, "Picture hold must be between 1 and 30 seconds");
    return result;
}
void usage() {
    std::cout << "udm-lcd prepare MEDIA PACK\n"
              << "udm-lcd play (MEDIA | --pack PACK) [--loop | --cycles N] [--background]\n"
              << "             [--host USER@HOST] [--runtime ARM64_BINARY] [--fps N]\n"
              << "             [--still-seconds N] [--native-colors]\n"
              << "udm-lcd status|stop|inspect [--host USER@HOST] [--runtime ARM64_BINARY]\n"
              << "udm-lcd device inspect [--host USER@HOST]\n"
              << "udm-lcd firmware build|install|recover|inspect ...\n";
}
}
int main(int argc, char** argv) {
    try {
        signal(SIGPIPE, SIG_IGN);
        for (int fd = 0; fd <= 2; ++fd) if (fcntl(fd, F_GETFD) < 0) {
            int opened = open("/dev/null", fd == 0 ? O_RDONLY : O_WRONLY);
            udm::require(opened >= 0 && (opened == fd || dup2(opened, fd) == fd), "Cannot initialize standard stream");
            if (opened != fd) close(opened);
        }
        if (argc < 2 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "help") { usage(); return 0; }
        std::string command = argv[1];
        std::vector<std::string> arguments;
        for (int index = 2; index < argc; ++index) arguments.emplace_back(argv[index]);
        if (command == "firmware") return udm::firmware_command(arguments);
        if (command == "device") return udm::device_command(arguments);
        if (command == "version" || command == "--version") { std::cout << "udm-lcd 1.0\n"; return 0; }
        if (command == "prepare") {
            udm::require(arguments.size() == 2, "Usage: udm-lcd prepare MEDIA PACK");
            udm::prepare_media(arguments[0], arguments[1]);
            auto pack = udm::load_pack(arguments[1]);
            std::cout << udm::json_text(pack.manifest["source"], true) << '\n' << "Pack: " << arguments[1] << '\n';
            return 0;
        }
        if (command == "target-status" || command == "target-stop") {
            udm::require(arguments.empty(), "Unexpected target control arguments");
            std::cout << udm::json_text(udm::target_control(command == "target-status" ? "status" : "stop"), true) << '\n';
            return 0;
        }
        if (command == "target-inspect") {
            udm::require(arguments.empty(), "Unexpected target inspect arguments");
            std::cout << udm::json_text(udm::target_inspect(), true) << '\n';
            return 0;
        }
        if (command == "target-install") {
            udm::require(!arguments.empty(), "Target staging directory is required");
            auto stage = std::filesystem::path(arguments.front());
            arguments.erase(arguments.begin());
            auto parsed = options(arguments);
            return udm::target_install(stage, parsed.play);
        }
        auto parsed = options(arguments);
        if (command == "target-play") {
            udm::require(!parsed.play.pack.empty() && parsed.source.empty(), "Target playback requires a prepared pack");
            return udm::target_play(parsed.play);
        }
        if (command == "status" || command == "stop" || command == "inspect") {
            udm::require(parsed.source.empty() && parsed.play.pack.empty(), "Media is not accepted for this command");
            return udm::host_control(parsed.connection, command);
        }
        udm::require(command == "play", "Unknown command: " + command);
        udm::require(!parsed.source.empty() || !parsed.play.pack.empty(), "Select a media file or pass --pack PACK; use prepare MEDIA PACK to create a media pack");
        udm::require(parsed.source.empty() || parsed.play.pack.empty(), "Choose a source media file or a prepared pack");
        udm::require(!parsed.play.lease, "--lease is reserved for target playback");
        if (!parsed.source.empty()) {
            auto directory = udm::project_root() / "media";
            std::filesystem::create_directories(directory);
            auto name = "pack-" + std::to_string(getpid()) + '-' + udm::hex8(std::random_device{}());
            parsed.play.pack = directory / name;
            udm::prepare_media(parsed.source, parsed.play.pack);
            std::cout << "Pack: " << parsed.play.pack << '\n' << std::flush;
        }
        return udm::host_play(parsed.connection, parsed.play);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
