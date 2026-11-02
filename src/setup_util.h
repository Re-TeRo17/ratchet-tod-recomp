/* Small helpers for the setup code: big-endian access and UTF-8 paths. */
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

static inline uint16_t tod_be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t tod_be32(const uint8_t* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline uint64_t tod_be64(const uint8_t* p) { return (uint64_t)tod_be32(p) << 32 | tod_be32(p + 4); }
static inline void tod_put_be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

namespace todsetup {
inline std::string utf8(const std::filesystem::path& p)
{
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
inline std::filesystem::path from_utf8(const std::string& s)
{
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}
}  // namespace todsetup
