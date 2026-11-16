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
/* An absolute path in Windows' long form (\\?\C:\... or \\?\UNC\server\...), so opening it is not limited to 260
 * characters (issue #20: a disc folder deep in the user's folders failed at the first long movie name). Other paths as they are. */
inline std::filesystem::path long_path(const std::filesystem::path& p)
{
#ifdef _WIN32
    std::error_code ec;
    std::filesystem::path a = p.is_absolute() ? p : std::filesystem::absolute(p, ec);
    if (ec) return p;
    std::wstring w = a.lexically_normal().make_preferred().native();
    if (w.rfind(L"\\\\?\\", 0) == 0) return a;
    if (w.rfind(L"\\\\", 0) == 0) return std::filesystem::path(L"\\\\?\\UNC\\" + w.substr(2));
    return std::filesystem::path(L"\\\\?\\" + w);
#else
    return p;
#endif
}
}  // namespace todsetup
