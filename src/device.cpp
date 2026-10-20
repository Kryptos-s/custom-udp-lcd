#include "device.hpp"

#include "common.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace udm {
namespace {

constexpr std::size_t inspect_output_cap = 256 * 1024;
constexpr double inspect_timeout = 45;
constexpr std::string_view expected_system_id = "ea15";
constexpr std::string_view expected_shortname = "UDMPRO";
constexpr std::string_view expected_architecture = "aarch64";
constexpr std::string_view expected_firmware_version = "v2.5.7-0-gd26945b";
constexpr std::string_view expected_firmware_sha256 = "dc5404fe0de1f9a6e122f80efa03f685a93dbd907a4cc2820b1f01580cf4e2d2";
constexpr std::string_view expected_firmware_prefix = "8c9321956b415adaff5c1ff586b6634f";
constexpr std::string_view expected_stock_firmware_sha256 = "933eeae931a276ac19b74711542408e840238721282c76a5ca59c01e045008b1";
constexpr std::string_view expected_stock_firmware_prefix = "7d257b2b2cb958c93ffb447fa1de5b39";
constexpr std::string_view expected_stock_firmware_size = "937888";
constexpr std::string_view expected_daemon_sha256 = "5c1f0dc8e6673d0d5095e149d86b55c8b082adb94112feb342bbca7a0af096db";
constexpr std::string_view expected_dfu_util_sha256 = "3c71fe929becb030481865e08d53c991807937bd08e65f7f46cf8132a736975b";
constexpr std::string_view expected_service_sha256 = "1273fa681ff355ae16b20798bbb9eb8fd09ab534a22b5240a49f246c8bd51b69";
constexpr std::string_view expected_rom_vid_pid = "0483:df11";
constexpr std::string_view expected_rom_path = "1-2";
constexpr std::string_view expected_application_vid_pid = "1f9b:157d";
constexpr std::string_view expected_application_path = "1-2";
constexpr std::string_view firmware_binary_path = "/usr/share/firmware/udm-pro-lcm-fw.bin";
constexpr std::string_view firmware_hash_path = "/usr/share/firmware/udm-pro-lcm-fw.hash";
constexpr std::string_view firmware_version_path = "/usr/share/firmware/udm-pro-lcm-fw.version";

constexpr std::string_view inspect_script = R"script(
set -u

emit_info() {
    info_key=$1
    info_value=$2
    info_value=$(printf '%s' "$info_value" | tr '\r\n\t' '   ' | sed 's/[[:space:]]*$//' | cut -c1-256)
    [ -n "$info_value" ] || info_value=-
    printf 'I\t%s\t%s\n' "$info_key" "$info_value"
}

emit_file() {
    file_path=$(printf '%s' "$1" | tr '\r\n\t' '   ' | cut -c1-256)
    file_state=$2
    file_size=$3
    file_hash=$4
    printf 'F\t%s\t%s\t%s\t%s\n' "$file_path" "$file_state" "$file_size" "$file_hash"
}

emit_usb() {
    usb_path=$1
    usb_vid=$2
    usb_pid=$3
    usb_product=$4
    usb_serial=$5
    usb_product=$(printf '%s' "$usb_product" | tr '\r\n\t' '   ' | sed 's/[[:space:]]*$//' | cut -c1-128)
    usb_serial=$(printf '%s' "$usb_serial" | tr '\r\n\t' '   ' | sed 's/[[:space:]]*$//' | cut -c1-128)
    [ -n "$usb_product" ] || usb_product=-
    [ -n "$usb_serial" ] || usb_serial=-
    printf 'U\t%s\t%s:%s\t%s\t%s\n' "$usb_path" "$usb_vid" "$usb_pid" "$usb_product" "$usb_serial"
}

read_first() {
    [ -r "$1" ] || return 0
    sed -n '1p' "$1" 2>/dev/null | tr -d '\r\n' | tr '\t' ' ' | cut -c1-256
}

read_field() {
    field_key=$1
    shift
    for field_path in "$@"; do
        [ -r "$field_path" ] || continue
        field_value=$(sed -n "s/^${field_key}=//p" "$field_path" 2>/dev/null | sed -n '1p')
        if [ -n "$field_value" ]; then
            printf '%s' "$field_value"
            return 0
        fi
    done
}

read_identity() {
    for identity_key in "$@"; do
        identity_value=$(read_field "$identity_key" /etc/board.info /proc/ubnthal/system.info)
        if [ -n "$identity_value" ]; then
            printf '%s' "$identity_value"
            return 0
        fi
    done
}

emit_artifact() {
    artifact_path=$1
    if [ -f "$artifact_path" ]; then
        artifact_size=$(wc -c < "$artifact_path" 2>/dev/null | tr -d '[:space:]')
        case "$artifact_size" in
            ''|*[!0-9]*) emit_file "$artifact_path" present - unavailable; return 0 ;;
        esac
        if [ "$artifact_size" -le 16777216 ]; then
            artifact_hash=$(sha256sum "$artifact_path" 2>/dev/null | sed -n '1p' | cut -d ' ' -f1)
            [ -n "$artifact_hash" ] || artifact_hash=unavailable
            emit_file "$artifact_path" present "$artifact_size" "$artifact_hash"
        else
            emit_file "$artifact_path" oversize "$artifact_size" skipped
        fi
    else
        emit_file "$artifact_path" missing - unavailable
    fi
}

emit_info architecture "$(uname -m 2>/dev/null)"
emit_info kernel "$(uname -r 2>/dev/null)"
emit_info model "$(read_identity model board.name board.model)"
emit_info shortname "$(read_identity shortname board.shortname)"
emit_info board "$(read_identity board board.name)"
emit_info systemid "$(read_identity systemid board.sysid)"
emit_info firmware_version "$(read_first /usr/share/firmware/udm-pro-lcm-fw.version)"
emit_info firmware_info_version "$(read_field version firmware_version /etc/board.info /proc/ubnthal/system.info)"
emit_info firmware_hash_value "$(read_first /usr/share/firmware/udm-pro-lcm-fw.hash)"
emit_info os_version "$(read_first /etc/version)"

emit_artifact /usr/bin/ulcmd
emit_artifact /usr/bin/dfu-util
emit_artifact /lib/systemd/system/ulcmd.service
emit_artifact /usr/share/firmware/udm-pro-lcm-fw.bin
emit_artifact /usr/share/firmware/udm-pro-lcm-fw.hash
emit_artifact /usr/share/firmware/udm-pro-lcm-fw.version

emit_info ulcmd_available "$(if [ -x /usr/bin/ulcmd ]; then printf true; else printf false; fi)"
emit_info dfu_util_available "$(if [ -x /usr/bin/dfu-util ]; then printf true; else printf false; fi)"
emit_info service_unit_present "$(if [ -f /lib/systemd/system/ulcmd.service ]; then printf true; else printf false; fi)"
if command -v systemctl >/dev/null 2>&1; then
    emit_info systemd_state "$(systemctl show ulcmd.service -p LoadState -p ActiveState 2>/dev/null | tr '\r\n\t' '   ' | cut -c1-256)"
else
    emit_info systemd_state unavailable
fi
emit_info display_tty "$(if [ -e /dev/ttyACM0 ]; then printf /dev/ttyACM0; else printf missing; fi)"

usb_records=0
for usb_device in /sys/bus/usb/devices/*; do
    [ "$usb_records" -lt 16 ] || break
    [ -f "$usb_device/idVendor" ] || continue
    usb_name=${usb_device##*/}
    case "$usb_name" in
        *:*) continue ;;
    esac
    usb_vendor=$(read_first "$usb_device/idVendor")
    usb_product_id=$(read_first "$usb_device/idProduct")
    case "$usb_vendor:$usb_product_id" in
        1f9b:157d|0483:df11)
            emit_usb "$usb_name" "$usb_vendor" "$usb_product_id" "$(read_first "$usb_device/product")" "$(read_first "$usb_device/serial")"
            usb_records=$((usb_records + 1))
            ;;
    esac
done
emit_info usb_records "$usb_records"
)script";

struct Artifact {
    std::string state;
    std::string size;
    std::string sha256;
};

struct UsbDevice {
    std::string path;
    std::string vid_pid;
    std::string product;
    std::string serial;
};

struct Snapshot {
    std::map<std::string, std::string> info;
    std::map<std::string, Artifact> artifacts;
    std::vector<UsbDevice> usb;
};

void validate_host(std::string_view host) {
    require(host.size() <= 256 && !host.empty() && host.front() != '-', "Invalid SSH destination");
    const auto at = host.find('@');
    const auto user = at == std::string_view::npos ? std::string_view{} : host.substr(0, at);
    const auto name = at == std::string_view::npos ? host : host.substr(at + 1);
    if (!user.empty()) {
        require((user.front() >= 'A' && user.front() <= 'Z') || (user.front() >= 'a' && user.front() <= 'z') || user.front() == '_', "Invalid SSH user");
        require(std::all_of(user.begin() + 1, user.end(), [](unsigned char value) {
            return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '_' || value == '-';
        }), "Invalid SSH user");
    }
    require(!name.empty() && ((name.front() >= 'A' && name.front() <= 'Z') || (name.front() >= 'a' && name.front() <= 'z') ||
                              (name.front() >= '0' && name.front() <= '9')), "Invalid SSH host");
    require(std::all_of(name.begin(), name.end(), [](unsigned char value) {
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
               value == '_' || value == '.' || value == ':' || value == '-';
    }), "Invalid SSH host");
}

std::vector<std::string> fields(std::string_view line) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin <= line.size()) {
        const auto end = line.find('\t', begin);
        result.emplace_back(line.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin));
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return result;
}

Snapshot parse_snapshot(std::span<const std::uint8_t> bytes) {
    Snapshot result;
    const std::string text(bytes.begin(), bytes.end());
    std::size_t begin = 0;
    while (begin < text.size()) {
        const auto end = text.find('\n', begin);
        const auto line_end = end == std::string::npos ? text.size() : end;
        auto line = std::string_view(text).substr(begin, line_end - begin);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        require(line.size() <= 2048, "Device inspection record exceeds limit");
        if (!line.empty()) {
            const auto values = fields(line);
            require(!values.empty(), "Empty device inspection record");
            if (values[0] == "I") {
                require(values.size() == 3 && !values[1].empty(), "Malformed device identity record");
                require(!result.info.contains(values[1]), "Duplicate device identity record");
                result.info.emplace(values[1], values[2] == "-" ? std::string() : values[2]);
            } else if (values[0] == "F") {
                require(values.size() == 5 && !values[1].empty(), "Malformed device artifact record");
                require(!result.artifacts.contains(values[1]), "Duplicate device artifact record");
                result.artifacts.emplace(values[1], Artifact{values[2], values[3], values[4]});
            } else if (values[0] == "U") {
                require(values.size() == 5 && !values[1].empty() && !values[2].empty(), "Malformed device USB record");
                result.usb.push_back(UsbDevice{values[1], values[2], values[3] == "-" ? std::string() : values[3], values[4] == "-" ? std::string() : values[4]});
                require(result.usb.size() <= 16, "Too many device USB records");
            } else {
                throw std::runtime_error("Unknown device inspection record");
            }
        }
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    require(!result.info.empty(), "Device inspection returned no identity");
    return result;
}

std::string info(const Snapshot& snapshot, std::string_view key) {
    const auto found = snapshot.info.find(std::string(key));
    return found == snapshot.info.end() ? std::string() : found->second;
}

const Artifact* artifact(const Snapshot& snapshot, std::string_view path) {
    const auto found = snapshot.artifacts.find(std::string(path));
    return found == snapshot.artifacts.end() ? nullptr : &found->second;
}

bool artifact_hash_matches(const Snapshot& snapshot, std::string_view path, std::string_view expected, std::vector<std::string>& reasons) {
    const auto found = artifact(snapshot, path);
    if (found == nullptr || found->state != "present") {
        reasons.emplace_back("missing_artifact:" + std::string(path));
        return false;
    }
    if (found->sha256 != expected) {
        reasons.emplace_back("artifact_hash_mismatch:" + std::string(path));
        return false;
    }
    return true;
}

bool has_usb(const Snapshot& snapshot, std::string_view path, std::string_view vid_pid, std::string_view serial = {}) {
    return std::any_of(snapshot.usb.begin(), snapshot.usb.end(), [&](const UsbDevice& device) {
        return device.path == path && device.vid_pid == vid_pid && (serial.empty() || device.serial == serial);
    });
}

bool valid_rom_serial(std::string_view serial) {
    return serial.size() == 12 && std::all_of(serial.begin(), serial.end(), [](unsigned char value) {
        return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'F');
    });
}

Json::Value artifact_json(const Artifact* value) {
    Json::Value result;
    if (value == nullptr) {
        result["state"] = "unobserved";
        return result;
    }
    result["state"] = value->state;
    if (value->size != "-") {
        std::uint64_t size = 0;
        const auto parsed = std::from_chars(value->size.data(), value->size.data() + value->size.size(), size);
        if (parsed.ec == std::errc{} && parsed.ptr == value->size.data() + value->size.size())
            result["size"] = Json::UInt64(size);
    }
    if (value->sha256 != "unavailable" && value->sha256 != "skipped")
        result["sha256"] = value->sha256;
    return result;
}

Json::Value inspect_remote(const Connection& connection) {
    validate_host(connection.host);
    const auto script = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(inspect_script.data()), inspect_script.size());
    const auto process = execute({"ssh", "-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=8",
                                  "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2", connection.host, "sh", "-s"},
                                 script, inspect_timeout, inspect_output_cap);
    require(process.code == 0, "Device inspection SSH failed (exit " + std::to_string(process.code) + "): " + process.errors.substr(0, 4096));
    const auto snapshot = parse_snapshot(process.output);

    Json::Value result;
    result["schema"] = 1;
    result["operation"] = "device_inspect";
    result["read_only"] = true;
    result["host"] = connection.host;
    result["architecture"] = info(snapshot, "architecture");
    const auto shortname = info(snapshot, "shortname");
    result["model"] = shortname.empty() || shortname == "(null)" ? info(snapshot, "model") : shortname;
    result["version"] = !info(snapshot, "firmware_version").empty() ? info(snapshot, "firmware_version") :
                        !info(snapshot, "firmware_info_version").empty() ? info(snapshot, "firmware_info_version") : info(snapshot, "os_version");

    Json::Value identity;
    identity["model"] = info(snapshot, "model");
    identity["shortname"] = info(snapshot, "shortname");
    identity["board"] = info(snapshot, "board");
    identity["systemid"] = info(snapshot, "systemid");
    identity["architecture"] = info(snapshot, "architecture");
    identity["kernel"] = info(snapshot, "kernel");
    identity["firmware_version"] = info(snapshot, "firmware_version");
    identity["firmware_info_version"] = info(snapshot, "firmware_info_version");
    identity["os_version"] = info(snapshot, "os_version");
    result["identity"] = identity;

    Json::Value files;
    files[std::string(firmware_binary_path)] = artifact_json(artifact(snapshot, firmware_binary_path));
    files[std::string(firmware_hash_path)] = artifact_json(artifact(snapshot, firmware_hash_path));
    files[std::string(firmware_version_path)] = artifact_json(artifact(snapshot, firmware_version_path));
    files["/usr/bin/ulcmd"] = artifact_json(artifact(snapshot, "/usr/bin/ulcmd"));
    files["/usr/bin/dfu-util"] = artifact_json(artifact(snapshot, "/usr/bin/dfu-util"));
    files["/lib/systemd/system/ulcmd.service"] = artifact_json(artifact(snapshot, "/lib/systemd/system/ulcmd.service"));
    Json::Value firmware;
    firmware["files"] = files;
    firmware["hash_value"] = info(snapshot, "firmware_hash_value");
    result["firmware"] = firmware;

    Json::Value usb(Json::arrayValue);
    for (const auto& device : snapshot.usb) {
        Json::Value item;
        item["path"] = device.path;
        item["vid_pid"] = device.vid_pid;
        item["product"] = device.product;
        item["serial"] = device.serial;
        usb.append(item);
    }
    result["usb"] = usb;

    Json::Value daemon;
    daemon["ulcmd_available"] = info(snapshot, "ulcmd_available") == "true";
    daemon["dfu_util_available"] = info(snapshot, "dfu_util_available") == "true";
    daemon["service_unit_present"] = info(snapshot, "service_unit_present") == "true";
    daemon["systemd_state"] = info(snapshot, "systemd_state");
    daemon["display_tty"] = info(snapshot, "display_tty");
    result["daemon"] = daemon;

    std::vector<std::string> reasons;
    const bool model_ok = info(snapshot, "shortname") == expected_shortname;
    const bool system_id_ok = info(snapshot, "systemid") == expected_system_id;
    const bool architecture_ok = info(snapshot, "architecture") == expected_architecture;
    const bool firmware_version_ok = info(snapshot, "firmware_version") == expected_firmware_version;
    const auto firmware_file = artifact(snapshot, firmware_binary_path);
    const auto firmware_sidecar = artifact(snapshot, firmware_hash_path);
    const auto firmware_hash_value = info(snapshot, "firmware_hash_value");
    const bool candidate_pair_ok = firmware_file != nullptr && firmware_file->state == "present" && firmware_file->size == "1048576" &&
                                   firmware_file->sha256 == expected_firmware_sha256 && firmware_sidecar != nullptr && firmware_sidecar->state == "present" &&
                                   firmware_hash_value == expected_firmware_prefix;
    const bool stock_pair_ok = firmware_file != nullptr && firmware_file->state == "present" && firmware_file->size == expected_stock_firmware_size &&
                               firmware_file->sha256 == expected_stock_firmware_sha256 && firmware_sidecar != nullptr && firmware_sidecar->state == "present" &&
                               firmware_hash_value == expected_stock_firmware_prefix;
    const bool firmware_pair_ok = candidate_pair_ok || stock_pair_ok;
    if (!model_ok)
        reasons.emplace_back("model_not_supported");
    if (!system_id_ok)
        reasons.emplace_back("system_id_not_ea15");
    if (!architecture_ok)
        reasons.emplace_back("architecture_not_aarch64");
    if (!firmware_version_ok)
        reasons.emplace_back("display_firmware_version_mismatch");
    if (!firmware_pair_ok)
        reasons.emplace_back("display_firmware_pair_not_pinned");
    const bool daemon_hash_ok = artifact_hash_matches(snapshot, "/usr/bin/ulcmd", expected_daemon_sha256, reasons);
    const bool dfu_hash_ok = artifact_hash_matches(snapshot, "/usr/bin/dfu-util", expected_dfu_util_sha256, reasons);
    const bool service_hash_ok = artifact_hash_matches(snapshot, "/lib/systemd/system/ulcmd.service", expected_service_sha256, reasons);
    const bool app_usb_ok = has_usb(snapshot, expected_application_path, expected_application_vid_pid);
    const bool rom_usb_present = has_usb(snapshot, expected_rom_path, expected_rom_vid_pid);
    std::string rom_serial;
    for (const auto& device : snapshot.usb)
        if (device.path == expected_rom_path && device.vid_pid == expected_rom_vid_pid && !device.serial.empty()) {
            rom_serial = device.serial;
            break;
        }
    if (!app_usb_ok)
        reasons.emplace_back("application_usb_identity_not_observed");

    Json::Value transport;
    transport["application_vid_pid"] = std::string(expected_application_vid_pid);
    transport["application_physical_path"] = std::string(expected_application_path);
    transport["application_observed"] = app_usb_ok;
    transport["rom_dfu_vid_pid"] = std::string(expected_rom_vid_pid);
    transport["rom_dfu_physical_path"] = std::string(expected_rom_path);
    transport["rom_dfu_observed"] = rom_usb_present;
    transport["rom_dfu_serial_observed"] = !rom_serial.empty();
    result["display_transport"] = transport;

    const bool rom_serial_valid = rom_usb_present && valid_rom_serial(rom_serial);
    const bool recovery_transport_ok = app_usb_ok || rom_serial_valid;
    const bool install_supported = model_ok && system_id_ok && architecture_ok && firmware_version_ok && firmware_pair_ok &&
                                   daemon_hash_ok && dfu_hash_ok && service_hash_ok && app_usb_ok;
    const bool recovery_supported = model_ok && system_id_ok && architecture_ok && firmware_version_ok && firmware_pair_ok &&
                                    daemon_hash_ok && dfu_hash_ok && service_hash_ok && recovery_transport_ok;
    std::vector<std::string> recovery_reasons;
    for (const auto& reason : reasons)
        if (reason != "application_usb_identity_not_observed")
            recovery_reasons.push_back(reason);
    if (!recovery_transport_ok)
        recovery_reasons.emplace_back(rom_usb_present ? "rom_dfu_serial_invalid" : "recovery_transport_not_observed");
    result["firmware_check_supported"] = true;
    result["firmware_root_pair"] = candidate_pair_ok ? "candidate" : stock_pair_ok ? "stock" : "unknown";
    result["firmware"]["root_pair"] = result["firmware_root_pair"];
    result["firmware_install_supported"] = install_supported;
    result["firmware_recovery_supported"] = recovery_supported;
    result["firmware_install_requires_rom_serial_verification"] = true;
    result["firmware_install_serial_verification"] = "deferred_to_backend_transaction";
    result["rom_dfu_identity_observed"] = rom_usb_present;
    result["rom_dfu_serial_observed"] = !rom_serial.empty();
    result["rom_dfu_serial_valid"] = rom_serial_valid;
    if (!rom_serial.empty())
        result["rom_dfu_serial"] = rom_serial;
    result["firmware_install_reasons"] = Json::Value(Json::arrayValue);
    for (const auto& reason : reasons)
        result["firmware_install_reasons"].append(reason);
    result["firmware_recovery_reasons"] = Json::Value(Json::arrayValue);
    for (const auto& reason : recovery_reasons)
        result["firmware_recovery_reasons"].append(reason);
    Json::Value capabilities;
    capabilities["firmware_check_supported"] = true;
    capabilities["firmware_install_supported"] = install_supported;
    capabilities["firmware_recovery_supported"] = recovery_supported;
    capabilities["display_transport_observed"] = app_usb_ok || rom_usb_present;
    capabilities["rom_dfu_identity_observed"] = rom_usb_present;
    capabilities["rom_dfu_serial_observed"] = !rom_serial.empty();
    capabilities["rom_dfu_serial_valid"] = rom_serial_valid;
    capabilities["read_only_inspection"] = true;
    result["capabilities"] = capabilities;
    return result;
}

}

int device_command(const std::vector<std::string>& args) {
    require(!args.empty() && args.front() == "inspect", "Usage: udm-lcd device inspect --host USER@HOST");
    Connection connection;
    for (std::size_t index = 1; index < args.size(); ++index) {
        require(args[index] == "--host", "Unknown device option: " + args[index]);
        require(index + 1 < args.size(), "Missing value for --host");
        connection.host = args[++index];
    }
    std::cout << json_text(inspect_remote(connection), true) << '\n';
    return 0;
}

}
