#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace logger::detail {

inline constexpr std::size_t record_size = 256;
inline constexpr std::size_t record_header_size = 32;
inline constexpr std::size_t record_crc_offset = 28;
inline constexpr std::uint8_t record_version = 1;

inline std::uint16_t read_u16(const std::uint8_t *p) {
    return static_cast<std::uint16_t>(p[0] | p[1] << 8);
}

inline std::uint32_t read_u32(const std::uint8_t *p) {
    return read_u16(p) | static_cast<std::uint32_t>(read_u16(p + 2)) << 16;
}

inline std::uint32_t crc32(std::uint32_t crc, const std::uint8_t *data, std::size_t size) {
    crc = ~crc;
    while (size-- > 0) {
        crc ^= *data++;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

struct record_view_t {
    std::uint32_t sequence, boot_id, time_ms;
    std::uint8_t level;
    std::string_view task, message;
};

inline std::size_t decode_record(std::span<const std::uint8_t> bytes, record_view_t &out) {
    const auto *p = bytes.data();
    if (bytes.size() < record_header_size || std::memcmp(p, "LOG1", 4) != 0 ||
        p[4] != record_version || p[5] < 1 || p[5] > 3) return 0;

    const auto size = read_u32(p + 8);
    const std::size_t payload = read_u16(p + 24);
    const std::size_t name = p[26];
    if (size > bytes.size() || size > record_size || name > payload ||
        size != ((record_header_size + payload + 3U) & ~std::size_t{3})) return 0;

    const auto crc = crc32(0, p, record_crc_offset);
    if (read_u32(p + record_crc_offset) != crc32(crc, p + record_header_size, payload)) return 0;
    const char *text = reinterpret_cast<const char *>(p + record_header_size);
    out = {read_u32(p + 12), read_u32(p + 16), read_u32(p + 20), p[5],
           {text, name}, {text + name, payload - name}};
    return size;
}

}
