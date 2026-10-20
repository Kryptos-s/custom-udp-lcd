#pragma once

#include "common.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace udm {

inline constexpr std::uint32_t media_width = 240;
inline constexpr std::uint32_t media_height = 240;
inline constexpr std::uint32_t media_colors = 256;
inline constexpr std::size_t media_frame_bytes = static_cast<std::size_t>(media_width) * media_height;
inline constexpr std::size_t media_palette_bytes = media_colors * sizeof(std::uint16_t);
inline constexpr std::size_t media_source_cap = 32u * 1024u * 1024u;
inline constexpr std::size_t media_frame_count_cap = 1024;
inline constexpr std::size_t media_source_pixel_cap = 16u * 1024u * 1024u;
inline constexpr std::uint64_t media_duration_cap_us = 60ull * 1000000ull;
inline constexpr std::size_t media_pack_cap = media_frame_bytes * media_frame_count_cap;
inline constexpr std::size_t media_wire_payload_cap = media_frame_bytes + (media_frame_bytes + 127u) / 128u;
inline constexpr std::uint32_t media_wire_swap_rb = 0x100u;

struct FrameRecord {
    std::uint32_t index = 0;
    std::uint64_t start_us = 0;
    std::uint64_t duration_us = 0;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::string crc32;
    std::string sha256;
};

struct Pack {
    Json::Value manifest;
    Bytes palette;
    Bytes frames;
    std::vector<FrameRecord> frame_records;
};

std::uint32_t crc32(std::span<const std::uint8_t> data);
std::array<std::uint8_t, 32> digest256(std::span<const std::uint8_t> data);
std::array<std::uint8_t, 16> digest_md5(std::span<const std::uint8_t> data);
std::string hex_digest(std::span<const std::uint8_t> data);
std::string hex_digest(const std::array<std::uint8_t, 32>& data);
std::string hex_digest(const std::array<std::uint8_t, 16>& data);

Pack load_pack(const std::filesystem::path& directory);
void prepare_media(const std::filesystem::path& source, const std::filesystem::path& destination,
                   std::uint64_t still_duration_us = 3000000);
Bytes wire_frame(std::uint32_t session, std::uint32_t sequence,
                 std::span<const std::uint8_t> indices,
                 std::span<const std::uint8_t> palette, bool swap_rb);

}
