#include "media.hpp"

#include <openssl/sha.h>
#include <lz4.h>
#include <lz4hc.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

namespace udm {

namespace {

using Path = std::filesystem::path;

constexpr std::size_t ffprobe_output_cap = 1024u * 1024u;
constexpr std::string_view indexed_filter =
    "scale=240:240:force_original_aspect_ratio=decrease:flags=lanczos,"
    "pad=240:240:(ow-iw)/2:(oh-ih)/2:color=black,format=rgb24";

[[noreturn]] void fail(std::string message)
{
    throw std::runtime_error(std::move(message));
}

void ensure(bool condition, std::string_view message)
{
    if (!condition)
        fail(std::string(message));
}

std::uint64_t json_uint(const Json::Value& value, std::uint64_t maximum, std::string_view name)
{
    if (!value.isIntegral() || value.isBool())
        fail("Invalid " + std::string(name));
    const auto signed_value = value.asLargestInt();
    if (signed_value < 0)
        fail("Invalid " + std::string(name));
    const auto result = value.asLargestUInt();
    if (result > maximum)
        fail("Invalid " + std::string(name));
    return result;
}

std::string json_string(const Json::Value& value, std::string_view name)
{
    if (!value.isString())
        fail("Invalid " + std::string(name));
    return value.asString();
}

std::string upper_crc(std::uint32_t value)
{
    static constexpr std::string_view digits = "0123456789ABCDEF";
    std::string result(8, '0');
    for (std::size_t index = 0; index < result.size(); ++index)
        result[index] = digits[(value >> ((result.size() - index - 1u) * 4u)) & 0x0fu];
    return result;
}

std::string lower_hex(std::span<const std::uint8_t> data)
{
    static constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(data.size() * 2u);
    for (const auto value : data) {
        result.push_back(digits[value >> 4u]);
        result.push_back(digits[value & 0x0fu]);
    }
    return result;
}

std::string error_detail(const ProcessResult& result)
{
    if (result.errors.empty())
        return "";
    return result.errors.substr(0, 4096);
}

Bytes run_command(const std::vector<std::string>& command, std::size_t cap)
{
    const auto result = execute(command, {}, 90.0, cap);
    if (result.code != 0) {
        const auto detail = error_detail(result);
        fail("Media conversion failed" + (detail.empty() ? std::string() : ": " + detail));
    }
    return result.output;
}

std::uint16_t rgb565(std::uint8_t red, std::uint8_t green, std::uint8_t blue)
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(red >> 3u) << 11u) |
                                      (static_cast<std::uint16_t>(green >> 2u) << 5u) |
                                      static_cast<std::uint16_t>(blue >> 3u));
}

std::array<std::uint8_t, 3> expand565(std::uint16_t color)
{
    const auto red = static_cast<std::uint8_t>((color >> 11u) & 31u);
    const auto green = static_cast<std::uint8_t>((color >> 5u) & 63u);
    const auto blue = static_cast<std::uint8_t>(color & 31u);
    return {static_cast<std::uint8_t>((red << 3u) | (red >> 2u)),
            static_cast<std::uint8_t>((green << 2u) | (green >> 4u)),
            static_cast<std::uint8_t>((blue << 3u) | (blue >> 2u))};
}

std::vector<std::uint16_t> palette_words(std::span<const std::uint8_t> palette)
{
    ensure(palette.size() == media_palette_bytes,
           "Palette must contain exactly 256 little-endian RGB565 entries");
    std::vector<std::uint16_t> result(media_colors);
    for (std::size_t index = 0; index < result.size(); ++index)
        result[index] = static_cast<std::uint16_t>(palette[index * 2u]) |
                        static_cast<std::uint16_t>(palette[index * 2u + 1u] << 8u);
    return result;
}

struct ColorPoint {
    std::uint16_t color = 0;
    std::uint32_t count = 0;
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
};

struct ColorBox {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::uint64_t count = 0;
    std::uint8_t red_min = 0;
    std::uint8_t red_max = 0;
    std::uint8_t green_min = 0;
    std::uint8_t green_max = 0;
    std::uint8_t blue_min = 0;
    std::uint8_t blue_max = 0;
};

ColorBox make_box(const std::vector<ColorPoint>& points, std::size_t begin, std::size_t end)
{
    ColorBox box;
    box.begin = begin;
    box.end = end;
    box.red_min = box.green_min = box.blue_min = std::numeric_limits<std::uint8_t>::max();
    for (std::size_t index = begin; index < end; ++index) {
        const auto& point = points[index];
        box.count += point.count;
        box.red_min = std::min(box.red_min, point.red);
        box.red_max = std::max(box.red_max, point.red);
        box.green_min = std::min(box.green_min, point.green);
        box.green_max = std::max(box.green_max, point.green);
        box.blue_min = std::min(box.blue_min, point.blue);
        box.blue_max = std::max(box.blue_max, point.blue);
    }
    return box;
}

std::uint32_t channel_range(const ColorBox& box, int axis)
{
    if (axis == 0)
        return box.red_max - box.red_min;
    if (axis == 1)
        return box.green_max - box.green_min;
    return box.blue_max - box.blue_min;
}

int split_axis(const ColorBox& box)
{
    const auto red_range = channel_range(box, 0);
    const auto green_range = channel_range(box, 1);
    const auto blue_range = channel_range(box, 2);
    if (red_range >= green_range && red_range >= blue_range)
        return 0;
    if (green_range >= blue_range)
        return 1;
    return 2;
}

std::uint16_t box_color(const std::vector<ColorPoint>& points, const ColorBox& box)
{
    std::uint64_t red = 0;
    std::uint64_t green = 0;
    std::uint64_t blue = 0;
    for (std::size_t index = box.begin; index < box.end; ++index) {
        const auto& point = points[index];
        red += static_cast<std::uint64_t>(point.red) * point.count;
        green += static_cast<std::uint64_t>(point.green) * point.count;
        blue += static_cast<std::uint64_t>(point.blue) * point.count;
    }
    const auto average = [count = box.count](std::uint64_t value) {
        return static_cast<std::uint8_t>(std::min<std::uint64_t>(255u, (value + count / 2u) / count));
    };
    return rgb565(average(red), average(green), average(blue));
}

Bytes make_palette(std::span<const std::uint8_t> raw)
{
    ensure(raw.size() % 3u == 0, "Palette training requires complete RGB frames");
    std::array<std::uint32_t, 65536> histogram{};
    for (std::size_t offset = 0; offset < raw.size(); offset += 3u)
        ++histogram[rgb565(raw[offset], raw[offset + 1u], raw[offset + 2u])];

    std::vector<ColorPoint> points;
    points.reserve(65535u);
    for (std::size_t color = 1; color < histogram.size(); ++color) {
        if (histogram[color] == 0)
            continue;
        const auto expanded = expand565(static_cast<std::uint16_t>(color));
        points.push_back({static_cast<std::uint16_t>(color), histogram[color], expanded[0], expanded[1], expanded[2]});
    }

    std::vector<ColorBox> boxes;
    if (!points.empty()) {
        boxes.push_back(make_box(points, 0, points.size()));
        while (boxes.size() < media_colors - 1u) {
            std::size_t selected = boxes.size();
            std::uint64_t selected_score = 0;
            for (std::size_t index = 0; index < boxes.size(); ++index) {
                const auto& box = boxes[index];
                const auto range = std::max({channel_range(box, 0), channel_range(box, 1), channel_range(box, 2)});
                if (box.end - box.begin <= 1u || range == 0)
                    continue;
                const auto score = static_cast<std::uint64_t>(range) * box.count;
                if (selected == boxes.size() || score > selected_score ||
                    (score == selected_score && box.count > boxes[selected].count)) {
                    selected = index;
                    selected_score = score;
                }
            }
            if (selected == boxes.size())
                break;
            auto box = boxes[selected];
            const auto axis = split_axis(box);
            std::stable_sort(points.begin() + static_cast<std::ptrdiff_t>(box.begin),
                             points.begin() + static_cast<std::ptrdiff_t>(box.end),
                             [axis](const ColorPoint& left, const ColorPoint& right) {
                                 const auto component = [axis](const ColorPoint& point) {
                                     return axis == 0 ? point.red : axis == 1 ? point.green : point.blue;
                                 };
                                 const auto left_component = component(left);
                                 const auto right_component = component(right);
                                 return left_component == right_component ? left.color < right.color
                                                                          : left_component < right_component;
                             });
            const auto target = (box.count + 1u) / 2u;
            std::uint64_t accumulated = 0;
            std::size_t split = box.begin + 1u;
            for (std::size_t index = box.begin; index + 1u < box.end; ++index) {
                accumulated += points[index].count;
                if (accumulated >= target) {
                    split = index + 1u;
                    break;
                }
            }
            const auto left = make_box(points, box.begin, split);
            const auto right = make_box(points, split, box.end);
            boxes[selected] = left;
            boxes.push_back(right);
        }
    }

    std::vector<std::uint16_t> colors;
    colors.reserve(media_colors);
    colors.push_back(0);
    for (const auto& box : boxes) {
        const auto color = box_color(points, box);
        if (std::find(colors.begin(), colors.end(), color) == colors.end())
            colors.push_back(color);
        if (colors.size() == media_colors)
            break;
    }
    colors.resize(media_colors, 0);

    Bytes result(media_palette_bytes);
    for (std::size_t index = 0; index < colors.size(); ++index) {
        result[index * 2u] = static_cast<std::uint8_t>(colors[index]);
        result[index * 2u + 1u] = static_cast<std::uint8_t>(colors[index] >> 8u);
    }
    return result;
}

Bytes index_frame(std::span<const std::uint8_t> rgb, std::span<const std::uint8_t> palette)
{
    ensure(rgb.size() == media_frame_bytes * 3u, "Decoded frame size differs from 240x240 RGB24");
    const auto colors = palette_words(palette);
    std::array<std::array<std::uint8_t, 3>, media_colors> expanded{};
    for (std::size_t index = 0; index < expanded.size(); ++index)
        expanded[index] = expand565(colors[index]);

    Bytes result(media_frame_bytes);
    for (std::size_t pixel = 0; pixel < result.size(); ++pixel) {
        const auto source_color = rgb565(rgb[pixel * 3u], rgb[pixel * 3u + 1u], rgb[pixel * 3u + 2u]);
        if (source_color == 0) {
            result[pixel] = 0;
            continue;
        }
        const auto red = rgb[pixel * 3u];
        const auto green = rgb[pixel * 3u + 1u];
        const auto blue = rgb[pixel * 3u + 2u];
        std::uint32_t best_distance = std::numeric_limits<std::uint32_t>::max();
        std::uint8_t best = 0;
        for (std::size_t index = 1; index < expanded.size(); ++index) {
            const auto red_delta = static_cast<int>(red) - expanded[index][0];
            const auto green_delta = static_cast<int>(green) - expanded[index][1];
            const auto blue_delta = static_cast<int>(blue) - expanded[index][2];
            const auto distance = static_cast<std::uint32_t>(red_delta * red_delta +
                                                              green_delta * green_delta +
                                                              blue_delta * blue_delta);
            if (distance < best_distance) {
                best_distance = distance;
                best = static_cast<std::uint8_t>(index);
            }
        }
        result[pixel] = best;
    }
    return result;
}

struct Timing {
    std::uint64_t start_us = 0;
    std::uint64_t duration_us = 0;
};

struct StreamInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string codec;
};

long double numeric_value(const Json::Value& value, std::string_view name)
{
    std::string text;
    if (value.isString())
        text = value.asString();
    else if (value.isNumeric() && !value.isBool())
        text = value.asString();
    else
        fail("Invalid " + std::string(name));
    if (text.empty())
        fail("Invalid " + std::string(name));
    std::size_t consumed = 0;
    long double parsed = 0;
    try {
        parsed = std::stold(text, &consumed);
    } catch (...) {
        fail("Invalid " + std::string(name));
    }
    if (consumed != text.size() || !std::isfinite(parsed))
        fail("Invalid " + std::string(name));
    return parsed;
}

std::int64_t micros(const Json::Value& value, std::string_view name)
{
    const auto parsed = numeric_value(value, name) * 1000000.0L;
    if (!std::isfinite(parsed) || std::abs(parsed) > static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        fail("Invalid " + std::string(name));
    return static_cast<std::int64_t>(std::round(parsed));
}

bool still_codec(std::string_view codec)
{
    static constexpr std::array<std::string_view, 12> codecs = {
        "png", "mjpeg", "jpeg2000", "bmp", "tiff", "webp", "pam", "ppm", "pgm", "pcx", "targa", "qoi"};
    return std::find(codecs.begin(), codecs.end(), codec) != codecs.end();
}

std::tuple<StreamInfo, std::vector<Timing>, std::uint64_t, bool> read_timeline(const Json::Value& info,
                                                                                std::uint64_t still_duration_us)
{
    ensure(still_duration_us >= 1000u && still_duration_us <= 10000000u &&
               still_duration_us <= media_duration_cap_us,
           "Invalid still duration");
    const auto& streams = info["streams"];
    const auto& frames = info["frames"];
    ensure(streams.isArray() && streams.size() == 1u && frames.isArray() &&
               frames.size() >= 1u && frames.size() <= media_frame_count_cap,
           "Exactly one bounded video stream is required");
    const auto& stream = streams[0];
    const auto width = json_uint(stream["width"], 8192u, "source width");
    const auto height = json_uint(stream["height"], 8192u, "source height");
    ensure(width * height <= media_source_pixel_cap, "Source pixel count exceeds cap");
    const auto codec = json_string(stream["codec_name"], "source codec");
    StreamInfo result_stream{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), codec};
    if (frames.size() == 1u && still_codec(codec))
        return {result_stream, {{0, still_duration_us}}, still_duration_us, true};

    std::vector<std::int64_t> timestamps;
    timestamps.reserve(frames.size());
    for (const auto& frame : frames)
        timestamps.push_back(micros(frame["best_effort_timestamp_time"], "source presentation time"));
    const auto first = timestamps.front();
    std::vector<Timing> timeline;
    timeline.reserve(frames.size());
    std::vector<std::int64_t> starts;
    starts.reserve(frames.size());
    for (const auto timestamp : timestamps)
        starts.push_back(timestamp - first);
    for (std::size_t index = 1; index < starts.size(); ++index)
        ensure(starts[index] > starts[index - 1u], "Source presentation timestamps are not strictly increasing");

    std::vector<std::int64_t> durations;
    durations.reserve(frames.size());
    for (std::size_t index = 1; index < starts.size(); ++index)
        durations.push_back(starts[index] - starts[index - 1u]);
    const auto& last_frame = frames[frames.size() - 1u];
    std::int64_t last_duration = 0;
    if (!last_frame["duration_time"].isNull())
        last_duration = micros(last_frame["duration_time"], "source final frame duration");
    else if (!last_frame["pkt_duration_time"].isNull())
        last_duration = micros(last_frame["pkt_duration_time"], "source final frame duration");
    else if (!stream["duration"].isNull())
        last_duration = micros(stream["duration"], "source stream duration") - starts.back();
    else if (!durations.empty())
        last_duration = durations.back();
    else
        fail("Source final frame duration is unavailable");
    durations.push_back(last_duration);
    for (const auto duration : durations)
        ensure(duration >= 1000 && duration <= 10000000, "Source frame duration exceeds bounds");
    std::uint64_t total = 0;
    for (std::size_t index = 0; index < starts.size(); ++index) {
        ensure(starts[index] >= 0, "Source presentation time is negative after normalization");
        const auto start = static_cast<std::uint64_t>(starts[index]);
        const auto duration = static_cast<std::uint64_t>(durations[index]);
        ensure(start <= media_duration_cap_us - duration, "Source duration exceeds 60 seconds");
        total = start + duration;
        timeline.push_back({start, duration});
    }
    return {result_stream, timeline, total, false};
}

Bytes packbits_encode(std::span<const std::uint8_t> data)
{
    ensure(!data.empty() && data.size() <= media_frame_bytes, "PackBits input exceeds frame bounds");
    Bytes result;
    result.reserve(data.size() + (data.size() + 127u) / 128u);
    std::size_t cursor = 0;
    while (cursor < data.size()) {
        std::size_t run = 1;
        while (run < 128u && cursor + run < data.size() && data[cursor + run] == data[cursor])
            ++run;
        if (run >= 3u) {
            result.push_back(static_cast<std::uint8_t>(127u + run));
            result.push_back(data[cursor]);
            cursor += run;
            continue;
        }
        const auto begin = cursor;
        cursor += run;
        while (cursor < data.size() && cursor - begin < 128u) {
            run = 1;
            while (run < 3u && cursor + run < data.size() && data[cursor + run] == data[cursor])
                ++run;
            if (run >= 3u)
                break;
            cursor += std::min(run, 128u - (cursor - begin));
        }
        result.push_back(static_cast<std::uint8_t>(cursor - begin - 1u));
        result.insert(result.end(), data.begin() + static_cast<std::ptrdiff_t>(begin),
                      data.begin() + static_cast<std::ptrdiff_t>(cursor));
    }
    ensure(result.size() <= data.size() + (data.size() + 127u) / 128u,
           "PackBits output exceeds its literal encoding bound");
    return result;
}

std::optional<Bytes> lz4_encode(std::span<const std::uint8_t> data)
{
    ensure(data.size() == media_frame_bytes, "LZ4 input must be exactly one 240x240 index frame");
    const auto bound = LZ4_compressBound(static_cast<int>(data.size()));
    if (bound <= 0 || static_cast<std::size_t>(bound) > media_wire_payload_cap)
        return std::nullopt;
    Bytes result(static_cast<std::size_t>(bound));
    const auto count = LZ4_compress_HC(reinterpret_cast<const char*>(data.data()),
                                       reinterpret_cast<char*>(result.data()), static_cast<int>(data.size()),
                                       bound, 9);
    if (count <= 0 || static_cast<std::size_t>(count) > media_wire_payload_cap)
        return std::nullopt;
    result.resize(static_cast<std::size_t>(count));
    return result;
}

void append_u32(Bytes& output, std::uint32_t value)
{
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8u));
    output.push_back(static_cast<std::uint8_t>(value >> 16u));
    output.push_back(static_cast<std::uint8_t>(value >> 24u));
}

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const Path& parent)
    {
        const auto pattern = (parent / ".indexed-media-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        if (mkdtemp(writable.data()) == nullptr)
            fail("Unable to create media staging directory");
        path_ = Path(writable.data());
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory()
    {
        if (!path_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }

    const Path& path() const
    {
        return path_;
    }

    Path release()
    {
        auto result = std::move(path_);
        path_.clear();
        return result;
    }

private:
    Path path_;
};

void publish_no_replace(const Path& source, const Path& destination)
{
#ifdef SYS_renameat2
    constexpr unsigned rename_noreplace = 1u;
    const auto result = ::syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(),
                                  rename_noreplace);
    if (result == 0)
        return;
    if (errno == EEXIST)
        fail("Output directory already exists: " + destination.string());
#endif
    fail("Atomic no-overwrite directory publish is unavailable");
}

void validate_blob(const Json::Value& record, std::string_view name, std::string_view filename,
                   std::span<const std::uint8_t> data)
{
    ensure(record.isObject(), "Invalid indexed media " + std::string(name) + " record");
    ensure(json_string(record["name"], "indexed media file name") == filename,
           "Indexed media " + std::string(name) + " identity differs");
    ensure(json_uint(record["bytes"], media_pack_cap, "indexed media byte count") == data.size(),
           "Indexed media " + std::string(name) + " identity differs");
    ensure(json_string(record["crc32"], "indexed media CRC32") == upper_crc(crc32(data)),
           "Indexed media " + std::string(name) + " identity differs");
    ensure(json_string(record["sha256"], "indexed media SHA256") == hex_digest(digest256(data)),
           "Indexed media " + std::string(name) + " identity differs");
}

void publish_pack(const Path& destination, const Bytes& palette, const Bytes& frames, const Json::Value& manifest)
{
    std::error_code error;
    const auto parent = destination.parent_path().empty() ? Path(".") : destination.parent_path();
    std::filesystem::create_directories(parent, error);
    if (error)
        fail("Unable to create media destination parent: " + error.message());
    const auto destination_status = std::filesystem::symlink_status(destination, error);
    if (!error && destination_status.type() != std::filesystem::file_type::not_found)
        fail("Output directory already exists: " + destination.string());
    error.clear();
    const auto absolute_parent = std::filesystem::absolute(parent, error);
    if (error)
        fail("Unable to resolve media destination parent: " + error.message());
    TemporaryDirectory staging(absolute_parent);
    const auto staging_path = staging.path();
    write_file(staging_path / "palette.bin", palette, true);
    write_file(staging_path / "frames.bin", frames, true);
    const auto text = json_text(manifest, true) + "\n";
    write_file(staging_path / "manifest.json",
               std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), true);
    const auto absolute_destination = std::filesystem::absolute(destination, error);
    if (error)
        fail("Unable to resolve media destination: " + error.message());
    publish_no_replace(staging_path, absolute_destination);
    staging.release();
}

}

std::uint32_t crc32(std::span<const std::uint8_t> data)
{
    std::uint32_t result = 0xffffffffu;
    for (const auto value : data) {
        result ^= value;
        for (unsigned bit = 0; bit < 8u; ++bit)
            result = (result >> 1u) ^ ((result & 1u) != 0u ? 0xedb88320u : 0u);
    }
    return ~result;
}

std::array<std::uint8_t, 32> digest256(std::span<const std::uint8_t> data)
{
    std::array<std::uint8_t, 32> result{};
    static constexpr std::uint8_t empty = 0;
    SHA256(data.empty() ? &empty : data.data(), data.size(), result.data());
    return result;
}

std::array<std::uint8_t, 16> digest_md5(std::span<const std::uint8_t> data)
{
    std::array<std::uint8_t, 16> result{};
    static constexpr std::uint8_t empty = 0;
    unsigned length = 0;
    ensure(EVP_Digest(data.empty() ? &empty : data.data(), data.size(), result.data(), &length,
                      EVP_md5(), nullptr) == 1 && length == result.size(),
           "MD5 digest failed");
    return result;
}

std::string hex_digest(std::span<const std::uint8_t> data)
{
    return lower_hex(data);
}

std::string hex_digest(const std::array<std::uint8_t, 32>& data)
{
    return lower_hex(data);
}

std::string hex_digest(const std::array<std::uint8_t, 16>& data)
{
    return lower_hex(data);
}

Pack load_pack(const Path& directory)
{
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error)
        fail("Media pack is missing: select a media file or prepare a pack with `udm-lcd prepare MEDIA PACK`");
    if (!std::filesystem::is_regular_file(directory / "manifest.json", error) || error)
        fail("Media pack manifest is missing: select a media file or prepare a pack with `udm-lcd prepare MEDIA PACK`");
    const auto manifest_data = read_file(directory / "manifest.json", 1024u * 1024u);
    const auto manifest = parse_json(std::string_view(reinterpret_cast<const char*>(manifest_data.data()), manifest_data.size()));
    ensure(manifest.isObject() && manifest["format"].isString() && manifest["format"].asString() == "INDEXED_MEDIA_V1" &&
               json_uint(manifest["width"], media_width, "indexed media width") == media_width &&
               json_uint(manifest["height"], media_height, "indexed media height") == media_height &&
               manifest["pixel_format"].isString() && manifest["pixel_format"].asString() == "INDEX8_RGB565_LE" &&
               json_uint(manifest["frame_bytes"], media_frame_bytes, "indexed media frame bytes") == media_frame_bytes,
           "Unsupported indexed media format");
    const auto palette = read_file(directory / "palette.bin", media_palette_bytes);
    const auto frames = read_file(directory / "frames.bin", media_pack_cap);
    palette_words(palette);
    validate_blob(manifest["palette"], "palette", "palette.bin", palette);
    validate_blob(manifest["pack"], "pack", "frames.bin", frames);

    const auto& records = manifest["frames"];
    ensure(records.isArray() && records.size() >= 1u && records.size() <= media_frame_count_cap,
           "Invalid indexed frame count");
    std::uint64_t offset = 0;
    std::uint64_t start = 0;
    std::vector<FrameRecord> frame_records;
    frame_records.reserve(records.size());
    for (Json::ArrayIndex index = 0; index < records.size(); ++index) {
        const auto& record = records[index];
        ensure(record.isObject(), "Invalid indexed frame record");
        const auto record_index = json_uint(record["index"], media_frame_count_cap - 1u, "frame index");
        const auto record_start = json_uint(record["start_us"], media_duration_cap_us, "frame start");
        const auto duration = json_uint(record["duration_us"], 10000000u, "frame duration");
        const auto record_offset = json_uint(record["offset"], media_pack_cap, "frame offset");
        const auto length = json_uint(record["length"], media_frame_bytes, "frame length");
        ensure(record_index == index && record_start == start && record_offset == offset &&
                   length == media_frame_bytes,
               "Frame sequence, offset, size or timing differs");
        ensure(duration >= 1000u && duration <= 10000000u, "Invalid frame duration");
        ensure(offset <= frames.size() && media_frame_bytes <= frames.size() - offset,
               "Indexed frame extends beyond pack");
        const auto frame = std::span<const std::uint8_t>(frames).subspan(offset, media_frame_bytes);
        const auto crc = upper_crc(crc32(frame));
        const auto sha = hex_digest(digest256(frame));
        ensure(record["crc32"].isString() && record["crc32"].asString() == crc &&
                   record["sha256"].isString() && record["sha256"].asString() == sha,
               "Indexed frame checksum differs");
        frame_records.push_back({static_cast<std::uint32_t>(record_index), record_start, duration,
                                 record_offset, length, crc, sha});
        offset += media_frame_bytes;
        ensure(start <= media_duration_cap_us - duration, "Indexed media duration exceeds cap");
        start += duration;
    }
    const auto& source = manifest["source"];
    ensure(source.isObject() && json_uint(source["frames"], media_frame_count_cap, "source frame count") == records.size() &&
               json_uint(source["duration_us"], media_duration_cap_us, "source duration") == start &&
               start <= media_duration_cap_us && offset == frames.size(),
           "Indexed media frame length or duration differs");
    return {manifest, palette, frames, std::move(frame_records)};
}

void prepare_media(const Path& source, const Path& destination, std::uint64_t still_duration_us)
{
    std::error_code error;
    const auto source_path = std::filesystem::canonical(source, error);
    if (error)
        fail("Unable to resolve media source: " + error.message());
    const auto source_data = read_file(source_path, media_source_cap);
    const auto source_sha = hex_digest(digest256(source_data));
    const auto probe = run_command({"ffprobe", "-v", "error", "-protocol_whitelist", "file,pipe",
                                    "-select_streams", "v:0", "-show_streams", "-show_frames", "-show_entries",
                                    "stream=width,height,codec_name,duration:frame=best_effort_timestamp_time,"
                                    "duration_time,pkt_duration_time",
                                    "-of", "json", source_path.string()},
                                   ffprobe_output_cap);
    const auto info = parse_json(std::string_view(reinterpret_cast<const char*>(probe.data()), probe.size()));
    const auto [stream, timeline, duration, still] = read_timeline(info, still_duration_us);
    ensure(timeline.size() <= std::numeric_limits<std::size_t>::max() / (media_frame_bytes * 3u),
           "Decoded media frame count exceeds bounds");
    const auto raw = run_command({"ffmpeg", "-v", "error", "-nostdin", "-protocol_whitelist", "file,pipe", "-i",
                                  source_path.string(), "-map", "0:v:0", "-vf", std::string(indexed_filter),
                                  "-fps_mode", "passthrough", "-frames:v", std::to_string(timeline.size()), "-an",
                                  "-sn", "-dn", "-f", "rawvideo", "-pix_fmt", "rgb24", "pipe:1"},
                                 timeline.size() * media_frame_bytes * 3u);
    ensure(raw.size() == timeline.size() * media_frame_bytes * 3u,
           "Decoded frame count differs from the source timing table");
    const auto palette = make_palette(raw);
    Bytes payload;
    payload.reserve(timeline.size() * media_frame_bytes);
    Json::Value frame_manifest(Json::arrayValue);
    for (std::size_t index = 0; index < timeline.size(); ++index) {
        const auto rgb = std::span<const std::uint8_t>(raw).subspan(index * media_frame_bytes * 3u,
                                                                    media_frame_bytes * 3u);
        const auto indices = index_frame(rgb, palette);
        const auto crc = crc32(indices);
        const auto sha = digest256(indices);
        Json::Value record(Json::objectValue);
        record["index"] = static_cast<Json::UInt64>(index);
        record["start_us"] = static_cast<Json::UInt64>(timeline[index].start_us);
        record["duration_us"] = static_cast<Json::UInt64>(timeline[index].duration_us);
        record["offset"] = static_cast<Json::UInt64>(payload.size());
        record["length"] = static_cast<Json::UInt64>(indices.size());
        record["crc32"] = upper_crc(crc);
        record["sha256"] = hex_digest(sha);
        frame_manifest.append(record);
        payload.insert(payload.end(), indices.begin(), indices.end());
    }
    ensure(payload.size() <= media_pack_cap, "Indexed media pack exceeds cap");
    const auto source_after = read_file(source_path, media_source_cap);
    ensure(source_after.size() == source_data.size() && hex_digest(digest256(source_after)) == source_sha,
           "Source changed during preparation");

    Json::Value manifest(Json::objectValue);
    manifest["format"] = "INDEXED_MEDIA_V1";
    manifest["width"] = media_width;
    manifest["height"] = media_height;
    manifest["pixel_format"] = "INDEX8_RGB565_LE";
    manifest["frame_bytes"] = static_cast<Json::UInt64>(media_frame_bytes);
    Json::Value source_manifest(Json::objectValue);
    source_manifest["name"] = source_path.filename().string();
    source_manifest["bytes"] = static_cast<Json::UInt64>(source_data.size());
    source_manifest["sha256"] = source_sha;
    source_manifest["width"] = stream.width;
    source_manifest["height"] = stream.height;
    source_manifest["codec"] = stream.codec;
    source_manifest["frames"] = static_cast<Json::UInt64>(timeline.size());
    source_manifest["duration_us"] = static_cast<Json::UInt64>(duration);
    source_manifest["still"] = still;
    manifest["source"] = source_manifest;
    Json::Value preparation(Json::objectValue);
    preparation["filter"] = std::string(indexed_filter);
    preparation["pillow"] = "native-c++";
    preparation["dither"] = "none";
    preparation["palette_method"] = "global MEDIANCUT 255 adaptive colors plus exact black; rounded RGB565 before remapping";
    preparation["color_compensation"] = "canonical RGB565; optional R/B swap only at transport boundary";
    manifest["preparation"] = preparation;
    Json::Value palette_manifest(Json::objectValue);
    palette_manifest["name"] = "palette.bin";
    palette_manifest["entries"] = media_colors;
    palette_manifest["bytes"] = static_cast<Json::UInt64>(palette.size());
    palette_manifest["crc32"] = upper_crc(crc32(palette));
    palette_manifest["sha256"] = hex_digest(digest256(palette));
    manifest["palette"] = palette_manifest;
    Json::Value pack_manifest(Json::objectValue);
    pack_manifest["name"] = "frames.bin";
    pack_manifest["bytes"] = static_cast<Json::UInt64>(payload.size());
    pack_manifest["crc32"] = upper_crc(crc32(payload));
    pack_manifest["sha256"] = hex_digest(digest256(payload));
    manifest["pack"] = pack_manifest;
    manifest["frames"] = frame_manifest;
    publish_pack(destination, palette, payload, manifest);
}

Bytes wire_frame(std::uint32_t session, std::uint32_t sequence, std::span<const std::uint8_t> indices,
                 std::span<const std::uint8_t> palette, bool swap_rb)
{
    ensure(session != 0u, "Invalid session");
    ensure(sequence != 0u, "Invalid sequence");
    ensure(indices.size() == media_frame_bytes, "Expected exactly one 240x240 index frame");
    palette_words(palette);
    const auto packbits = packbits_encode(indices);
    std::uint32_t codec = 0;
    Bytes payload(indices.begin(), indices.end());
    if (packbits.size() < payload.size()) {
        codec = 1;
        payload = packbits;
    }
    if (const auto lz4 = lz4_encode(indices); lz4.has_value() &&
        (lz4->size() < payload.size() || (lz4->size() == payload.size() && codec > 2u))) {
        codec = 2;
        payload = *lz4;
    }
    ensure(payload.size() <= media_wire_payload_cap, "Encoded wire frame exceeds cap");
    if (swap_rb)
        codec |= media_wire_swap_rb;
    Bytes result;
    result.reserve(32u + palette.size() + payload.size());
    result.insert(result.end(), {'U', 'I', 'F', '1'});
    append_u32(result, session);
    append_u32(result, sequence);
    append_u32(result, codec);
    append_u32(result, static_cast<std::uint32_t>(payload.size()));
    append_u32(result, media_frame_bytes);
    append_u32(result, crc32(palette));
    append_u32(result, crc32(indices));
    result.insert(result.end(), palette.begin(), palette.end());
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

}
