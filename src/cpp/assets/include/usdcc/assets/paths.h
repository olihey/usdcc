#pragma once

// Files on disk as UTF-8 text and back. C++20's u8string() is a std::u8string,
// which nothing else here takes.

#include <filesystem>
#include <string>

namespace usdcc::assets {

// With forward slashes, on Windows too.
inline std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

inline std::filesystem::path fromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

}  // namespace usdcc::assets
