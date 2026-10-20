#pragma once
#include <json/json.h>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace udm {
using Bytes = std::vector<std::uint8_t>;
class Fd {
    int value_ = -1;
public:
    Fd() = default;
    explicit Fd(int value);
    ~Fd();
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept;
    Fd& operator=(Fd&& other) noexcept;
    int get() const;
    int release();
    void reset(int value = -1);
};
struct ProcessResult {
    int code = -1;
    Bytes output;
    std::string errors;
};
void require(bool condition, std::string_view message);
double now();
Bytes read_file(const std::filesystem::path& path, std::size_t cap);
std::string text_file(const std::filesystem::path& path, std::size_t cap = 65536);
std::string node_text(const std::filesystem::path& path, std::size_t cap = 65536);
void write_file(const std::filesystem::path& path, std::span<const std::uint8_t> data, bool exclusive = true);
void write_text(const std::filesystem::path& path, std::string_view text, bool exclusive = true);
Json::Value parse_json(std::string_view text);
std::string json_text(const Json::Value& value, bool pretty = false);
std::string quote(std::string_view text);
ProcessResult execute(const std::vector<std::string>& command, std::span<const std::uint8_t> input = {}, double timeout = 90, std::size_t output_cap = 200 * 1024 * 1024);
std::filesystem::path executable_path();
std::string hex8(std::uint32_t value);
std::uint64_t positive_integer(std::string_view text, std::string_view name);
}
