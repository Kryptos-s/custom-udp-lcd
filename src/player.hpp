#pragma once
#include "common.hpp"

namespace udm {
struct PlayOptions {
    std::filesystem::path pack;
    std::uint64_t cycles = 2;
    double fps = 30;
    double still_seconds = 5;
    bool loop = false;
    bool swap_rb = true;
    bool lease = false;
    bool background = false;
};
int target_play(const PlayOptions& options);
Json::Value target_inspect();
Json::Value service_state(std::string_view unit);
Json::Value tty_owners();
void request_stop(int signal);
bool stop_requested();
}
