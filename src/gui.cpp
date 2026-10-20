#include "gui_process.hpp"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include <SDL.h>
#include <json/json.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <pwd.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct SdlGuard {
    ~SdlGuard() { SDL_Quit(); }
};

enum class Operation {
    none,
    browse,
    play,
    stop,
    status,
    device_inspect,
    firmware_inspect,
    firmware_install,
    firmware_recover
};

enum class FirmwareAction {
    none,
    install,
    recover
};

struct DeviceProfile {
    std::string name;
    std::string host;
};

struct StatusView {
    bool valid = false;
    std::string connection = "unchecked";
    std::string host;
    std::string active_state;
    std::string pid;
    bool independent = false;
    std::string error;
};

struct DeviceInspection {
    bool valid = false;
    bool fresh = false;
    bool support_known = false;
    bool firmware_install_supported = false;
    bool recovery_support_known = false;
    bool firmware_recovery_supported = false;
    std::string host;
    std::string model;
    std::string architecture;
    std::string compatibility;
    std::string raw_json;
    std::string error;
    Json::Value report;
};

struct FirmwareAssets {
    bool valid = false;
    bool artifacts_verified = false;
    bool live_operations_enabled = false;
    std::string firmware_status;
    std::string raw_json;
    std::string error;
    Json::Value report;
};

constexpr std::size_t output_history_limit = 1024u * 1024u;
constexpr float minimum_ui_scale = 1.0f;
constexpr float maximum_ui_scale = 2.0f;

std::vector<DeviceProfile> default_profiles()
{
    return {{"UDM Pro", "root@192.168.1.1"}};
}

std::string trim_line_end(std::string value)
{
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
        value.pop_back();
    return value;
}

std::string lower_copy(std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const auto character : value)
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    return result;
}

bool valid_ssh_host(std::string_view host)
{
    const auto alpha = [](unsigned char value) {
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
    };
    const auto alnum = [&](unsigned char value) {
        return alpha(value) || (value >= '0' && value <= '9');
    };
    if (host.empty() || host.size() > 256 || host.front() == '-')
        return false;
    const auto at = host.find('@');
    const auto user = at == std::string_view::npos ? std::string_view{} : host.substr(0, at);
    const auto name = at == std::string_view::npos ? host : host.substr(at + 1);
    if (!user.empty()) {
        const auto first = static_cast<unsigned char>(user.front());
        if (!(alpha(first) || first == '_'))
            return false;
        if (!std::all_of(user.begin() + 1, user.end(), [](unsigned char value) {
                return ((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
                        (value >= '0' && value <= '9')) || value == '_' || value == '-';
            }))
            return false;
    }
    if (name.empty())
        return false;
    const auto first = static_cast<unsigned char>(name.front());
    if (!alnum(first))
        return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char value) {
        return ((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
                (value >= '0' && value <= '9')) || value == '_' || value == '.' || value == ':' || value == '-';
    });
}

std::string chooser_path(std::string_view output)
{
    std::string last;
    std::size_t begin = 0;
    while (begin <= output.size()) {
        const auto end = output.find('\n', begin);
        auto line = output.substr(begin, end == std::string_view::npos ? output.size() - begin : end - begin);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (!line.empty()) {
            std::error_code error;
            const auto path = fs::path(line);
            const auto extension = lower_copy(path.extension().string());
            const bool media_extension = extension == ".gif" || extension == ".mp4" || extension == ".mkv" ||
                                          extension == ".webm" || extension == ".png" || extension == ".jpg" ||
                                          extension == ".jpeg" || extension == ".bmp" || extension == ".webp" ||
                                          extension == ".mov" || extension == ".avi" || extension == ".m4v" ||
                                          extension == ".tif" || extension == ".tiff";
            if (media_extension && fs::is_regular_file(path, error) && !error)
                return std::string(line);
            if (media_extension)
                last.assign(line);
        }
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return last;
}

std::string operation_name(Operation operation)
{
    switch (operation) {
    case Operation::browse:
        return "Browse";
    case Operation::play:
        return "Play";
    case Operation::stop:
        return "Stop";
    case Operation::status:
        return "Status";
    case Operation::device_inspect:
        return "Inspect device";
    case Operation::firmware_inspect:
        return "Check firmware assets";
    case Operation::firmware_install:
        return "Install receiver";
    case Operation::firmware_recover:
        return "Recover stock";
    case Operation::none:
        break;
    }
    return {};
}

std::string progress_from_output(Operation operation, std::string_view output)
{
    const auto lower = lower_copy(output);
    if (lower.find("upload") != std::string::npos)
        return "Uploading media or firmware bundle";
    if (lower.find("download") != std::string::npos)
        return "Writing the selected firmware image";
    if (lower.find("start") != std::string::npos)
        return "Starting background playback";
    if (lower.find("pack:") != std::string::npos)
        return "Media pack prepared";
    if (operation == Operation::status && !output.empty())
        return "Status response received";
    if ((operation == Operation::device_inspect || operation == Operation::firmware_inspect) && !output.empty())
        return "Inspection response received";
    return {};
}

std::string json_pretty(const Json::Value& value)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    builder["commentStyle"] = "None";
    return Json::writeString(builder, value);
}

Json::Value parse_json_output(std::string_view output, std::string& error)
{
    Json::CharReaderBuilder builder;
    builder["rejectDupKeys"] = true;
    builder["failIfExtra"] = true;
    builder["stackLimit"] = 64;
    std::vector<std::pair<std::size_t, std::size_t>> objects;
    bool quoted = false;
    bool escaped = false;
    std::size_t begin = 0;
    int depth = 0;
    for (std::size_t index = 0; index < output.size(); ++index) {
        const auto character = output[index];
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (character == '\\')
                escaped = true;
            else if (character == '"')
                quoted = false;
            continue;
        }
        if (character == '"') {
            quoted = true;
        } else if (character == '{') {
            if (depth == 0)
                begin = index;
            ++depth;
        } else if (character == '}' && depth > 0) {
            --depth;
            if (depth == 0)
                objects.emplace_back(begin, index + 1);
        }
    }
    std::string parse_errors;
    Json::Value fallback;
    bool have_fallback = false;
    for (auto object = objects.rbegin(); object != objects.rend(); ++object) {
        std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        Json::Value value;
        parse_errors.clear();
        if (!reader->parse(output.data() + object->first, output.data() + object->second, &value, &parse_errors) ||
            !value.isObject())
            continue;
        if (!have_fallback) {
            fallback = value;
            have_fallback = true;
        }
    }
    if (have_fallback)
        return fallback;
    error = parse_errors.empty() ? "No JSON object was returned." : trim_line_end(parse_errors);
    return {};
}

bool is_status_object(const Json::Value& value)
{
    return value.isObject() && value["service"].isObject() && value.isMember("independent_of_pc");
}

StatusView status_from_json(std::string_view output)
{
    StatusView result;
    const auto value = parse_json_output(output, result.error);
    if (!value.isObject())
        return result;
    const auto& service = value["service"];
    if (!service.isObject() || !value.isMember("independent_of_pc") || !is_status_object(value)) {
        result.error = "Status response did not contain the managed service state.";
        return result;
    }
    result.active_state = service["ActiveState"].asString();
    if (result.active_state.empty())
        result.active_state = value["ActiveState"].asString();
    if (result.active_state.empty()) {
        result.error = "Status response did not contain an active state.";
        return result;
    }
    result.valid = true;
    result.pid = value["pid"].asString();
    if (result.pid.empty())
        result.pid = service["MainPID"].asString();
    result.independent = value["independent_of_pc"].asBool();
    return result;
}

std::string json_field(const Json::Value& value, std::string_view key)
{
    if (!value.isObject() || !value.isMember(std::string(key)))
        return {};
    const auto& field = value[std::string(key)];
    if (field.isString())
        return field.asString();
    if (field.isBool())
        return field.asBool() ? "true" : "false";
    if (field.isNumeric())
        return field.asString();
    return {};
}

std::string nested_json_field(const Json::Value& value, std::initializer_list<std::string_view> keys)
{
    for (const auto key : keys) {
        const auto direct = json_field(value, key);
        if (!direct.empty())
            return direct;
    }
    for (const auto container_name : {std::string_view("device"), std::string_view("target"),
                                      std::string_view("hardware"), std::string_view("identity")}) {
        if (!value.isObject() || !value[std::string(container_name)].isObject())
            continue;
        for (const auto key : keys) {
            const auto nested = json_field(value[std::string(container_name)], key);
            if (!nested.empty())
                return nested;
        }
    }
    return {};
}

std::optional<bool> find_bool(const Json::Value& value, std::string_view key)
{
    if (value.isObject()) {
        if (value.isMember(std::string(key)) && value[std::string(key)].isBool())
            return value[std::string(key)].asBool();
        for (const auto& member : value.getMemberNames()) {
            const auto result = find_bool(value[member], key);
            if (result.has_value())
                return result;
        }
    } else if (value.isArray()) {
        for (const auto& child : value) {
            const auto result = find_bool(child, key);
            if (result.has_value())
                return result;
        }
    }
    return std::nullopt;
}

Json::Value sanitize_json(const Json::Value& value)
{
    if (value.isObject()) {
        Json::Value result(Json::objectValue);
        for (const auto& member : value.getMemberNames()) {
            const auto key = lower_copy(member);
            if (key.find("password") != std::string::npos || key.find("passphrase") != std::string::npos ||
                key.find("secret") != std::string::npos || key.find("token") != std::string::npos ||
                key.find("credential") != std::string::npos || key.find("private_key") != std::string::npos ||
                key.find("private-key") != std::string::npos)
                continue;
            result[member] = sanitize_json(value[member]);
        }
        return result;
    }
    if (value.isArray()) {
        Json::Value result(Json::arrayValue);
        for (const auto& child : value)
            result.append(sanitize_json(child));
        return result;
    }
    return value;
}

fs::path config_path()
{
    const char* configured = std::getenv("XDG_CONFIG_HOME");
    if (configured != nullptr && configured[0] != '\0')
        return fs::path(configured) / "udm-lcd" / "gui.json";
    const auto* account = ::getpwuid(::getuid());
    if (account == nullptr || account->pw_dir == nullptr)
        return fs::path(".config") / "udm-lcd" / "gui.json";
    return fs::path(account->pw_dir) / ".config" / "udm-lcd" / "gui.json";
}

fs::path last_inspection_path()
{
    return config_path().parent_path() / "last-device-inspection.json";
}

void save_last_inspection(const Json::Value& report)
{
    const auto path = last_inspection_path();
    fs::create_directories(path.parent_path());
    ::chmod(path.parent_path().c_str(), 0700);
    const auto text = json_pretty(sanitize_json(report)) + "\n";
    auto temporary = path;
    temporary += ".tmp-" + std::to_string(static_cast<long long>(::getpid()));
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
            throw std::runtime_error("Cannot save the last device inspection.");
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.flush();
        if (!stream)
            throw std::runtime_error("Cannot write the last device inspection.");
    }
    ::chmod(temporary.c_str(), 0600);
    std::error_code rename_error;
    fs::rename(temporary, path, rename_error);
    if (rename_error) {
        std::error_code remove_error;
        fs::remove(temporary, remove_error);
        throw std::runtime_error("Cannot replace the last device inspection.");
    }
    ::chmod(path.c_str(), 0600);
}

std::vector<DeviceProfile> load_profiles(float& ui_scale, std::string& error)
{
    auto result = default_profiles();
    const auto path = config_path();
    std::error_code status_error;
    if (!fs::exists(path, status_error))
        return result;
    if (status_error || !fs::is_regular_file(path, status_error) || status_error) {
        error = "Saved GUI configuration is not a regular file.";
        return result;
    }
    const auto size = fs::file_size(path, status_error);
    if (status_error || size > 1024u * 1024u) {
        error = "Saved GUI configuration is too large.";
        return result;
    }
    std::ifstream stream(path);
    if (!stream) {
        error = "Saved GUI configuration could not be opened.";
        return result;
    }
    std::ostringstream contents;
    contents << stream.rdbuf();
    Json::CharReaderBuilder builder;
    builder["rejectDupKeys"] = true;
    builder["failIfExtra"] = true;
    Json::Value root;
    std::string parse_error;
    const auto text = contents.str();
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(text.data(), text.data() + text.size(), &root, &parse_error) || !root.isObject()) {
        error = "Saved GUI configuration is invalid.";
        return result;
    }
    if (root["ui_scale"].isNumeric())
        ui_scale = std::clamp(root["ui_scale"].asFloat(), minimum_ui_scale, maximum_ui_scale);
    if (!root["profiles"].isArray())
        return result;
    result.clear();
    for (const auto& entry : root["profiles"]) {
        if (!entry.isObject() || !entry["name"].isString() || !entry["host"].isString())
            continue;
        const auto name = entry["name"].asString();
        const auto host = entry["host"].asString();
        if (!name.empty() && name.size() <= 256 && valid_ssh_host(host))
            result.push_back({name, host});
    }
    if (result.empty())
        result = default_profiles();
    return result;
}

void save_profiles(const std::vector<DeviceProfile>& profiles, float ui_scale)
{
    const auto path = config_path();
    fs::create_directories(path.parent_path());
    ::chmod(path.parent_path().c_str(), 0700);
    Json::Value root(Json::objectValue);
    root["ui_scale"] = ui_scale;
    root["profiles"] = Json::Value(Json::arrayValue);
    for (const auto& profile : profiles) {
        Json::Value entry(Json::objectValue);
        entry["name"] = profile.name;
        entry["host"] = profile.host;
        root["profiles"].append(entry);
    }
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    builder["commentStyle"] = "None";
    const auto text = Json::writeString(builder, root) + "\n";
    auto temporary = path;
    temporary += ".tmp-" + std::to_string(static_cast<long long>(::getpid()));
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
            throw std::runtime_error("Cannot save GUI configuration.");
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.flush();
        if (!stream)
            throw std::runtime_error("Cannot write GUI configuration.");
    }
    ::chmod(temporary.c_str(), 0600);
    std::error_code rename_error;
    fs::rename(temporary, path, rename_error);
    if (rename_error) {
        std::error_code remove_error;
        fs::remove(temporary, remove_error);
        throw std::runtime_error("Cannot replace GUI configuration.");
    }
    ::chmod(path.c_str(), 0600);
}

void wipe(std::string& value)
{
    std::fill(value.begin(), value.end(), '\0');
    value.clear();
}

void wipe(std::array<char, 512>& value)
{
    std::fill(value.begin(), value.end(), '\0');
}

void copy_to_buffer(std::array<char, 256>& target, std::string_view value)
{
    std::fill(target.begin(), target.end(), '\0');
    const auto count = std::min(value.size(), target.size() - 1);
    std::copy_n(value.begin(), count, target.begin());
}

void copy_to_buffer(std::array<char, 257>& target, std::string_view value)
{
    std::fill(target.begin(), target.end(), '\0');
    const auto count = std::min(value.size(), target.size() - 1);
    std::copy_n(value.begin(), count, target.begin());
}

void copy_to_buffer(std::array<char, 4096>& target, std::string_view value)
{
    std::fill(target.begin(), target.end(), '\0');
    const auto count = std::min(value.size(), target.size() - 1);
    std::copy_n(value.begin(), count, target.begin());
}

bool exact_pinned_udm_pro(const DeviceInspection& inspection)
{
    const auto model = lower_copy(inspection.model);
    std::string compact;
    compact.reserve(model.size());
    for (const auto character : model)
        if (std::isalnum(static_cast<unsigned char>(character)))
            compact.push_back(character);
    return compact == "udmpro" || compact == "ubiquititudmpro";
}

void append_output(std::string& history, Operation operation, std::string_view output, int exit_code)
{
    if (!history.empty())
        history += "\n";
    history += "[" + operation_name(operation) + "]\n";
    history += output.empty() ? "(no output)\n" : std::string(output);
    if (history.empty() || history.back() != '\n')
        history += '\n';
    history += "[exit code: " + std::to_string(exit_code) + "]\n";
    if (history.size() > output_history_limit) {
        const auto excess = history.size() - output_history_limit;
        const auto boundary = history.find('\n', excess);
        history.erase(0, boundary == std::string::npos ? excess : boundary + 1);
    }
}

float detected_ui_scale(SDL_Window* window)
{
    float display_dpi = 0.0f;
    float horizontal_dpi = 0.0f;
    float vertical_dpi = 0.0f;
    const auto display = SDL_GetWindowDisplayIndex(window);
    if (display >= 0 && SDL_GetDisplayDPI(display, &display_dpi, &horizontal_dpi, &vertical_dpi) == 0 &&
        std::isfinite(display_dpi) && display_dpi > 0.0f)
        return std::clamp(display_dpi / 120.0f, minimum_ui_scale, maximum_ui_scale);
    return minimum_ui_scale;
}

}

int main()
{
    try {
        udm::gui_askpass();
        const auto cli = fs::read_symlink("/proc/self/exe").parent_path() / "udm-lcd";
        if (!fs::is_regular_file(cli))
            throw std::runtime_error("udm-lcd must be beside udm-lcd-gui");
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
            throw std::runtime_error(SDL_GetError());
        const SdlGuard quit;
        auto window = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>(
            SDL_CreateWindow("UDM LCD", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 760, 620,
                             SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE),
            SDL_DestroyWindow);
        if (!window)
            throw std::runtime_error(SDL_GetError());
        auto renderer = std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)>(
            SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC),
            SDL_DestroyRenderer);
        if (!renderer)
            renderer.reset(SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_SOFTWARE));
        if (!renderer)
            throw std::runtime_error(SDL_GetError());
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        ImGui::StyleColorsDark();
        const ImGuiStyle base_style = ImGui::GetStyle();
        const float base_font_global_scale = io.FontGlobalScale;
        if (!ImGui_ImplSDL2_InitForSDLRenderer(window.get(), renderer.get()) ||
            !ImGui_ImplSDLRenderer2_Init(renderer.get()))
            throw std::runtime_error("Cannot initialize ImGui SDL renderer");

        std::array<char, 4096> media{};
        std::array<char, 257> host{};
        std::array<char, 256> profile_name{};
        std::array<char, 512> password{};
        const std::string file_filter =
            "Media files (*.gif *.mp4 *.mkv *.webm *.png *.jpg *.jpeg *.bmp *.webp);;All files (*)";
        std::string config_error;
        float ui_scale = detected_ui_scale(window.get());
        auto profiles = load_profiles(ui_scale, config_error);
        int selected_profile = 0;
        copy_to_buffer(profile_name, profiles.front().name);
        copy_to_buffer(host, profiles.front().host);

        udm::GuiCommand command;
        Operation operation = Operation::none;
        FirmwareAction pending_firmware_action = FirmwareAction::none;
        std::string pending_firmware_host;
        std::string pending_firmware_profile;
        std::string operation_host;
        std::string operation_password;
        std::size_t operation_output_offset = 0;
        std::string output_history;
        std::string active_output;
        std::string message = config_error.empty() ? "Select a media file or media pack directory before playing." : config_error;
        std::string progress;
        std::string observed_host = host.data();
        StatusView status;
        DeviceInspection inspection;
        FirmwareAssets firmware_assets;
        bool open = true;

        auto apply_ui_scale = [&]() {
            ImGui::GetStyle() = base_style;
            ImGui::GetStyle().ScaleAllSizes(ui_scale);
            io.FontGlobalScale = base_font_global_scale * ui_scale;
        };
        apply_ui_scale();

        auto save_configuration = [&]() {
            try {
                save_profiles(profiles, ui_scale);
            } catch (const std::exception& error) {
                message = error.what();
            }
        };

        auto reset_host_state = [&]() {
            status = {};
            inspection = {};
            observed_host = host.data();
        };

        auto switch_profile = [&](int index) {
            if (index < 0 || static_cast<std::size_t>(index) >= profiles.size())
                return;
            selected_profile = index;
            copy_to_buffer(profile_name, profiles[static_cast<std::size_t>(selected_profile)].name);
            copy_to_buffer(host, profiles[static_cast<std::size_t>(selected_profile)].host);
            wipe(password);
            reset_host_state();
            message = "Selected " + profiles[static_cast<std::size_t>(selected_profile)].name + ". Inspect the device before firmware actions.";
        };

        auto start_operation = [&](Operation next, std::vector<std::string> arguments, std::string supplied_password) {
            auto password_copy = supplied_password;
            std::string host_snapshot;
            for (std::size_t index = 0; index + 1 < arguments.size(); ++index)
                if (arguments[index] == "--host") {
                    host_snapshot = arguments[index + 1];
                    break;
                }
            try {
                command.start(std::move(arguments), std::move(supplied_password));
                operation = next;
                operation_host = std::move(host_snapshot);
                operation_password = std::move(password_copy);
                operation_output_offset = 0;
                active_output.clear();
                progress.clear();
                message = "Command started; waiting for output.";
            } catch (const std::exception& error) {
                wipe(password_copy);
                operation = Operation::none;
                operation_host.clear();
                wipe(operation_password);
                message = error.what();
            }
        };

        auto start_status = [&](std::string supplied_password, std::string target_host) {
            if (target_host.empty())
                target_host = host.data();
            start_operation(Operation::status, {cli.string(), "status", "--host", std::move(target_host)},
                            std::move(supplied_password));
        };

        auto start_firmware = [&](FirmwareAction action, std::string target_host) {
            const auto mode = action == FirmwareAction::install ? "install" : "recover";
            const auto next = action == FirmwareAction::install ? Operation::firmware_install : Operation::firmware_recover;
            start_operation(next, {cli.string(), "firmware", mode, "--host", std::move(target_host), "--live"},
                            std::string(password.data()));
        };

        auto finish_operation = [&]() {
            const auto finished_operation = operation;
            const auto finished_host = operation_host;
            const auto finished_full_output = command.output();
            const auto output_begin = std::min(operation_output_offset, finished_full_output.size());
            const auto finished_visible_output = finished_full_output.substr(output_begin);
            const auto exit_code = command.exit_code();
            active_output.clear();
            append_output(output_history, finished_operation, finished_visible_output, exit_code);
            operation = Operation::none;
            operation_host.clear();
            operation_output_offset = 0;
            progress.clear();

            if (finished_operation == Operation::browse) {
                if (exit_code == 0) {
                    const auto selected = chooser_path(finished_full_output);
                    if (!selected.empty()) {
                        copy_to_buffer(media, selected);
                        message = "Selected " + selected;
                    } else {
                        message = "File chooser returned no path.";
                    }
                } else if (exit_code == 1) {
                    message = "File chooser canceled.";
                } else {
                    message = "File chooser failed with exit code " + std::to_string(exit_code) + ".";
                }
                wipe(operation_password);
                return;
            }

            if (finished_operation == Operation::status) {
                if (exit_code == 0) {
                    auto parsed_status = status_from_json(finished_full_output);
                    parsed_status.host = finished_host;
                    parsed_status.connection = "connected";
                    status = std::move(parsed_status);
                    message = status.valid ? "Status refreshed." : "Status response could not be parsed.";
                } else {
                    status.host = finished_host;
                    status.connection = "check failed";
                    status.valid = false;
                    status.error = "Status command exited with code " + std::to_string(exit_code) + ".";
                    message = status.error;
                }
                wipe(operation_password);
                return;
            }

            if (finished_operation == Operation::device_inspect) {
                inspection = {};
                inspection.host = finished_host;
                if (exit_code == 0) {
                    std::string parse_error;
                    inspection.report = parse_json_output(finished_full_output, parse_error);
                    if (inspection.report.isObject()) {
                        inspection.valid = true;
                        inspection.fresh = true;
                        inspection.raw_json = json_pretty(inspection.report);
                        inspection.model = nested_json_field(inspection.report, {"model", "product", "device_model"});
                        inspection.architecture = nested_json_field(inspection.report, {"architecture", "arch", "platform"});
                        inspection.compatibility = nested_json_field(inspection.report, {"compatibility", "compatibility_status"});
                        const auto supported = find_bool(inspection.report, "firmware_install_supported");
                        if (supported.has_value()) {
                            inspection.support_known = true;
                            inspection.firmware_install_supported = *supported;
                        }
                        const auto recovery_supported = find_bool(inspection.report, "firmware_recovery_supported");
                        if (recovery_supported.has_value()) {
                            inspection.recovery_support_known = true;
                            inspection.firmware_recovery_supported = *recovery_supported;
                        }
                        try {
                            save_last_inspection(inspection.report);
                            message = "Device inspection refreshed. Saved " + last_inspection_path().string() + ".";
                        } catch (const std::exception& save_error) {
                            message = "Device inspection refreshed, but saving the report failed: " + std::string(save_error.what());
                        }
                        if (!inspection.support_known || !inspection.recovery_support_known)
                            message += " One or more firmware actions are unknown.";
                    } else {
                        inspection.error = parse_error;
                        message = inspection.error;
                    }
                } else {
                    inspection.error = "Device inspect command exited with code " + std::to_string(exit_code) + ".";
                    message = inspection.error;
                }
                wipe(operation_password);
                return;
            }

            if (finished_operation == Operation::firmware_inspect) {
                firmware_assets = {};
                if (exit_code == 0) {
                    std::string parse_error;
                    firmware_assets.report = parse_json_output(finished_full_output, parse_error);
                    if (firmware_assets.report.isObject()) {
                        firmware_assets.valid = true;
                        firmware_assets.raw_json = json_pretty(firmware_assets.report);
                        firmware_assets.firmware_status = firmware_assets.report["firmware_status"].asString();
                        firmware_assets.artifacts_verified = firmware_assets.firmware_status == "offline_artifacts_verified";
                        firmware_assets.live_operations_enabled = firmware_assets.report["live_operations_enabled"].isBool() &&
                                                                  firmware_assets.report["live_operations_enabled"].asBool();
                        message = firmware_assets.artifacts_verified ? "Local firmware assets are verified." :
                                                                        "Firmware report did not verify the local asset set.";
                    } else {
                        firmware_assets.error = parse_error;
                        message = firmware_assets.error;
                    }
                } else {
                    firmware_assets.error = "Firmware inspect exited with code " + std::to_string(exit_code) + ".";
                    message = firmware_assets.error;
                }
                wipe(operation_password);
                return;
            }

            if (finished_operation == Operation::firmware_install || finished_operation == Operation::firmware_recover) {
                if (exit_code == 0) {
                    inspection.fresh = false;
                    message = operation_name(finished_operation) + " completed. Inspect the device again before another live action.";
                } else {
                    message = operation_name(finished_operation) + " failed with exit code " + std::to_string(exit_code) + ".";
                }
                wipe(operation_password);
                return;
            }

            if (exit_code != 0) {
                const bool host_was_verified = status.host == finished_host && status.connection == "connected";
                status.host = finished_host;
                status.connection = host_was_verified ? "connected" : "unchecked";
                status.valid = false;
                message = operation_name(finished_operation) + " failed with exit code " + std::to_string(exit_code) + ".";
                status.error = message;
                wipe(operation_password);
                return;
            }

            message = operation_name(finished_operation) + " completed.";
            status.host = finished_host;
            status.connection = "connected";
            status.valid = false;
            status.error = "Refreshing status.";
            if (finished_operation == Operation::play || finished_operation == Operation::stop)
                start_status(std::move(operation_password), finished_host);
            else
                wipe(operation_password);
        };

        auto start_play = [&]() {
            const std::string host_value = host.data();
            if (host_value.empty()) {
                message = "Enter an SSH host.";
                return;
            }
            std::vector<std::string> arguments{cli.string(), "play"};
            const std::string media_value = media.data();
            if (media_value.empty()) {
                message = "Select a media file or media pack directory before playing.";
                return;
            }
            std::error_code error;
            const auto media_path = fs::absolute(fs::path(media_value), error);
            if (!error && fs::is_directory(media_path, error) && !error)
                arguments.insert(arguments.end(), {"--pack", media_path.string()});
            else
                arguments.push_back(media_value);
            arguments.insert(arguments.end(), {"--background", "--host", host_value});
            start_operation(Operation::play, std::move(arguments), password.data());
        };

        auto start_stop = [&]() {
            const std::string host_value = host.data();
            if (host_value.empty()) {
                message = "Enter an SSH host.";
                return;
            }
            start_operation(Operation::stop, {cli.string(), "stop", "--host", host_value}, password.data());
        };

        auto start_manual_status = [&]() {
            const std::string host_value = host.data();
            if (host_value.empty()) {
                message = "Enter an SSH host.";
                return;
            }
            start_status(password.data(), host_value);
        };

        auto start_device_inspect = [&]() {
            const std::string host_value = host.data();
            if (host_value.empty()) {
                message = "Enter an SSH host.";
                return;
            }
            start_operation(Operation::device_inspect, {cli.string(), "device", "inspect", "--host", host_value},
                            password.data());
        };

        auto start_firmware_inspect = [&]() {
            start_operation(Operation::firmware_inspect, {cli.string(), "firmware", "inspect"}, {});
        };

        auto clear_console = [&]() {
            output_history.clear();
            if (operation != Operation::none)
                operation_output_offset = command.output().size();
            else
                operation_output_offset = 0;
            active_output.clear();
        };

        auto flash_allowed = [&](FirmwareAction action) {
            const std::string current_host = host.data();
            const bool supported = action == FirmwareAction::install ?
                (inspection.support_known && inspection.firmware_install_supported) :
                (action == FirmwareAction::recover && inspection.recovery_support_known && inspection.firmware_recovery_supported);
            return operation == Operation::none && !current_host.empty() && firmware_assets.valid &&
                   firmware_assets.artifacts_verified && firmware_assets.live_operations_enabled && inspection.valid &&
                   inspection.fresh && inspection.host == current_host && supported && exact_pinned_udm_pro(inspection);
        };

        while (open) {
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL2_ProcessEvent(&event);
                if (event.type == SDL_QUIT ||
                    (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE))
                    open = false;
            }

            if (operation != Operation::none) {
                try {
                    command.poll();
                    const auto full_output = command.output();
                    const auto begin = std::min(operation_output_offset, full_output.size());
                    active_output = full_output.substr(begin);
                    progress = progress_from_output(operation, full_output);
                    if (!command.busy())
                        finish_operation();
                } catch (const std::exception& error) {
                    message = error.what();
                    operation = Operation::none;
                    operation_output_offset = 0;
                    active_output.clear();
                    progress.clear();
                    wipe(operation_password);
                }
            }

            const std::string current_host = host.data();
            if (current_host != observed_host || (!status.host.empty() && status.host != current_host)) {
                observed_host = current_host;
                status.valid = false;
                status.connection = "unchecked";
                status.host.clear();
                status.error = current_host.empty() ? "Enter an SSH host and query status." :
                                                       "Host changed; inspect or query the new host.";
                inspection.fresh = false;
            }

            ImGui_ImplSDLRenderer2_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            ImGui::NewFrame();
            ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::Begin("UDM LCD", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
            ImGui::TextUnformatted("Remote display control");
            if (operation != Operation::none) {
                ImGui::SameLine();
                ImGui::Text("Working: %s", operation_name(operation).c_str());
                if (!progress.empty()) {
                    ImGui::SameLine();
                    ImGui::Text("Stage: %s", progress.c_str());
                }
            }
            ImGui::TextWrapped("%s", message.c_str());

            ImGui::BeginChild("Content", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            if (ImGui::BeginTabBar("Main tabs")) {
                if (ImGui::BeginTabItem("Media")) {
                    ImGui::TextUnformatted("Playback");
                    ImGui::TextUnformatted("Media path");
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::InputTextWithHint("##media-path", "Select a media file or pack directory", media.data(), media.size());
                    ImGui::BeginDisabled(operation != Operation::none);
                    if (ImGui::Button("Browse"))
                        start_operation(Operation::browse, {"kdialog", "--getopenfilename", ".", file_filter}, {});
                    ImGui::EndDisabled();
                    ImGui::TextWrapped("Select a media file or media pack directory before playing.");
                    ImGui::BeginDisabled(operation != Operation::none);
                    if (ImGui::Button("Play background"))
                        start_play();
                    ImGui::SameLine();
                    if (ImGui::Button("Stop"))
                        start_stop();
                    ImGui::SameLine();
                    if (ImGui::Button("Status"))
                        start_manual_status();
                    ImGui::EndDisabled();
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Device")) {
                    ImGui::TextUnformatted("Device profiles");
                    const auto profile_preview = profiles.empty() ? "No profiles" : profiles[static_cast<std::size_t>(selected_profile)].name.c_str();
                    ImGui::BeginDisabled(operation != Operation::none);
                    if (ImGui::BeginCombo("Device profile", profile_preview)) {
                        for (std::size_t index = 0; index < profiles.size(); ++index) {
                            const bool selected = static_cast<int>(index) == selected_profile;
                            if (ImGui::Selectable(profiles[index].name.c_str(), selected))
                                switch_profile(static_cast<int>(index));
                            if (selected)
                                ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TextUnformatted("Profile name");
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::InputText("##profile-name", profile_name.data(), profile_name.size());
                    ImGui::TextUnformatted("SSH host");
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::InputText("##ssh-host", host.data(), host.size());
                    ImGui::EndDisabled();
                    ImGui::BeginDisabled(operation != Operation::none);
                    if (ImGui::Button("Add profile")) {
                        std::string name = profile_name.data();
                        if (name.empty())
                            name = "Device " + std::to_string(profiles.size() + 1);
                        const std::string host_value = host.data();
                        if (!valid_ssh_host(host_value)) {
                            message = "Enter a valid SSH destination before adding a profile.";
                        } else {
                            profiles.push_back({name, host_value});
                            selected_profile = static_cast<int>(profiles.size() - 1);
                            copy_to_buffer(profile_name, name);
                            wipe(password);
                            reset_host_state();
                            save_configuration();
                            message = "Added profile " + name + ".";
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Save profile")) {
                        const std::string name = profile_name.data();
                        const std::string host_value = host.data();
                        if (name.empty() || !valid_ssh_host(host_value)) {
                            message = "Profile name and a valid SSH destination are required.";
                        } else if (selected_profile < 0 || static_cast<std::size_t>(selected_profile) >= profiles.size()) {
                            message = "Select a profile before saving.";
                        } else {
                            profiles[static_cast<std::size_t>(selected_profile)] = {name, host_value};
                            reset_host_state();
                            save_configuration();
                            message = "Saved profile " + name + ".";
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Remove profile")) {
                        if (profiles.size() <= 1) {
                            message = "Keep at least one device profile.";
                        } else {
                            profiles.erase(profiles.begin() + selected_profile);
                            selected_profile = std::min(selected_profile, static_cast<int>(profiles.size() - 1));
                            switch_profile(selected_profile);
                            save_configuration();
                            message = "Removed the selected profile.";
                        }
                    }
                    ImGui::EndDisabled();
                    ImGui::Separator();
                    ImGui::TextUnformatted("SSH password");
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::InputText("##ssh-password", password.data(), password.size(), ImGuiInputTextFlags_Password);
                    ImGui::TextWrapped("Passwords are held only for this session and are cleared when switching profiles. They are never saved.");
                    ImGui::BeginDisabled(operation != Operation::none || current_host.empty());
                    if (ImGui::Button("Inspect device"))
                        start_device_inspect();
                    ImGui::SameLine();
                    if (ImGui::Button("Refresh status"))
                        start_manual_status();
                    ImGui::EndDisabled();

                    ImGui::Separator();
                    ImGui::TextUnformatted("Overall status");
                    ImGui::Text("Connection: %s", status.connection.c_str());
                    if (!status.host.empty())
                        ImGui::Text("Host: %s", status.host.c_str());
                    if (status.valid) {
                        ImGui::Text("Service: %s", status.active_state.empty() ? "unknown" : status.active_state.c_str());
                        ImGui::Text("PID: %s", status.pid.empty() ? "unknown" : status.pid.c_str());
                        ImGui::Text("Independent of this PC: %s", status.independent ? "yes" : "no");
                    } else if (!status.error.empty()) {
                        ImGui::TextWrapped("%s", status.error.c_str());
                    } else {
                        ImGui::TextUnformatted("No status queried yet.");
                    }

                    ImGui::Separator();
                    ImGui::TextUnformatted("Device inspection");
                    if (!inspection.valid) {
                        ImGui::TextUnformatted("No device inspection is available for this host.");
                    } else {
                        if (!inspection.model.empty())
                            ImGui::Text("Model: %s", inspection.model.c_str());
                        if (!inspection.architecture.empty())
                            ImGui::Text("Architecture: %s", inspection.architecture.c_str());
                        if (!inspection.compatibility.empty())
                            ImGui::Text("Compatibility: %s", inspection.compatibility.c_str());
                        if (inspection.support_known)
                            ImGui::Text("Firmware install supported: %s", inspection.firmware_install_supported ? "yes" : "no");
                        else
                            ImGui::TextUnformatted("Firmware install supported: unknown");
                        if (inspection.recovery_support_known)
                            ImGui::Text("Firmware recovery supported: %s", inspection.firmware_recovery_supported ? "yes" : "no");
                        else
                            ImGui::TextUnformatted("Firmware recovery supported: unknown");
                        if (!inspection.fresh || inspection.host != current_host)
                            ImGui::TextUnformatted("Inspection is stale for the edited host; inspect again before flashing.");
                        if (!inspection.raw_json.empty()) {
                            const auto child_height = ImGui::GetTextLineHeightWithSpacing() * 8.0f;
                            ImGui::BeginChild("Device inspection JSON", ImVec2(0, child_height), true,
                                              ImGuiWindowFlags_HorizontalScrollbar);
                            ImGui::TextUnformatted(inspection.raw_json.c_str());
                            ImGui::EndChild();
                        }
                    }
                    ImGui::TextWrapped("The firmware backend revalidates the exact live DFU serial during each transaction. A matching model alone does not enable flashing.");
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Firmware")) {
                    ImGui::TextUnformatted("Firmware assets and live actions");
                    ImGui::BeginDisabled(operation != Operation::none);
                    if (ImGui::Button("Check assets"))
                        start_firmware_inspect();
                    ImGui::EndDisabled();
                    if (!firmware_assets.valid) {
                        ImGui::TextUnformatted("Run Check assets before a live firmware action.");
                    } else {
                        ImGui::Text("Asset status: %s", firmware_assets.firmware_status.empty() ? "unknown" : firmware_assets.firmware_status.c_str());
                        ImGui::Text("Live operations enabled by assets: %s", firmware_assets.live_operations_enabled ? "yes" : "no");
                        ImGui::Text("Pinned artifacts verified: %s", firmware_assets.artifacts_verified ? "yes" : "no");
                        if (!firmware_assets.raw_json.empty()) {
                            const auto child_height = ImGui::GetTextLineHeightWithSpacing() * 8.0f;
                            ImGui::BeginChild("Firmware inspection JSON", ImVec2(0, child_height), true,
                                              ImGuiWindowFlags_HorizontalScrollbar);
                            ImGui::TextUnformatted(firmware_assets.raw_json.c_str());
                            ImGui::EndChild();
                        }
                    }
                    ImGui::Separator();
                    const bool install_allowed = flash_allowed(FirmwareAction::install);
                    const bool recovery_allowed = flash_allowed(FirmwareAction::recover);
                    if (!install_allowed || !recovery_allowed) {
                        ImGui::TextWrapped("Each live action requires verified local assets, a fresh inspection for the selected host, the exact pinned UDM Pro model, and its matching firmware support flag. The backend revalidates the controller's current DFU serial during the transaction.");
                    }
                    ImGui::BeginDisabled(!install_allowed);
                    if (ImGui::Button("Install receiver")) {
                        pending_firmware_action = FirmwareAction::install;
                        pending_firmware_host = current_host;
                        pending_firmware_profile = profiles[static_cast<std::size_t>(selected_profile)].name;
                        ImGui::OpenPopup("Confirm live firmware operation");
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!recovery_allowed);
                    if (ImGui::Button("Recover stock")) {
                        pending_firmware_action = FirmwareAction::recover;
                        pending_firmware_host = current_host;
                        pending_firmware_profile = profiles[static_cast<std::size_t>(selected_profile)].name;
                        ImGui::OpenPopup("Confirm live firmware operation");
                    }
                    ImGui::EndDisabled();
                    ImGui::TextWrapped("A live firmware action starts only after the confirmation below.");
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Console")) {
                    if (ImGui::Button("Clear console"))
                        clear_console();
                    ImGui::SameLine();
                    ImGui::TextUnformatted("Clearing hides existing output without cancelling the active command.");
                    ImGui::BeginChild("Command output", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
                    const bool at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;
                    if (!output_history.empty())
                        ImGui::TextUnformatted(output_history.c_str());
                    if (!active_output.empty()) {
                        if (!output_history.empty())
                            ImGui::TextUnformatted("\n[current command]\n");
                        ImGui::TextUnformatted(active_output.c_str());
                    }
                    if (output_history.empty() && active_output.empty())
                        ImGui::TextUnformatted("No command output.");
                    if (at_bottom)
                        ImGui::SetScrollHereY(1.0f);
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Settings")) {
                    ImGui::TextUnformatted("Interface");
                    float ui_scale_percent = ui_scale * 100.0f;
                    const float previous_scale = ui_scale;
                    ImGui::TextUnformatted("UI scale");
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::SliderFloat("##ui-scale", &ui_scale_percent, 100.0f, 200.0f, "%.0f%%")) {
                        ui_scale = std::clamp(ui_scale_percent / 100.0f, minimum_ui_scale, maximum_ui_scale);
                        if (std::abs(previous_scale - ui_scale) > std::numeric_limits<float>::epsilon()) {
                            apply_ui_scale();
                            save_configuration();
                        }
                    }
                    ImGui::TextUnformatted("Text and control size.");
                    ImGui::TextWrapped("Configuration stores profile names, hosts, and UI scale in a private local file. Passwords remain in memory only.");
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::EndChild();
            ImGui::End();

            if (ImGui::BeginPopupModal("Confirm live firmware operation", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                const auto action_name = pending_firmware_action == FirmwareAction::install ? "Install receiver" : "Recover stock";
                ImGui::Text("Action: %s", action_name);
                ImGui::Text("Profile: %s", pending_firmware_profile.c_str());
                ImGui::Text("Target host: %s", pending_firmware_host.c_str());
                ImGui::Separator();
                ImGui::TextWrapped("This executes a live firmware operation on the target host. Confirm only after checking the host, assets, and device inspection.");
                if (ImGui::Button("Confirm flash")) {
                    const auto action = pending_firmware_action;
                    const auto target = pending_firmware_host;
                    pending_firmware_action = FirmwareAction::none;
                    pending_firmware_host.clear();
                    pending_firmware_profile.clear();
                    ImGui::CloseCurrentPopup();
                    if (target != std::string(host.data()) || !flash_allowed(action))
                        message = "Live firmware action blocked because the selected host or fresh action-specific inspection no longer matches.";
                    else
                        start_firmware(action, target);
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) {
                    pending_firmware_action = FirmwareAction::none;
                    pending_firmware_host.clear();
                    pending_firmware_profile.clear();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::Render();
            SDL_SetRenderDrawColor(renderer.get(), 32, 32, 32, 255);
            SDL_RenderClear(renderer.get());
            ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer.get());
            SDL_RenderPresent(renderer.get());
            SDL_Delay(10);
        }

        save_configuration();
        wipe(operation_password);
        wipe(password);
        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "udm-lcd-gui: %s\n", error.what());
        return 1;
    }
}
