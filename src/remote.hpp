#pragma once

#include "common.hpp"
#include "player.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace udm {

struct Connection {
    std::string host = "root@192.168.1.1";
    std::filesystem::path runtime;
};

struct Upload {
    std::string name;
    std::filesystem::path path;
};

ProcessResult upload_remote(Connection connection, const std::vector<Upload>& files,
                            const std::vector<std::string>& target_arguments,
                            double timeout = 240);
std::filesystem::path project_root();
int host_play(Connection connection, const PlayOptions& options);
int host_control(Connection connection, std::string action);
int target_install(const std::filesystem::path& stage, const PlayOptions& options);
Json::Value target_control(std::string action);

}
