#pragma once

#include <filesystem>
#include <string>

namespace brockdj::stems {

inline std::filesystem::path pathFromUtf8(const std::string& path)
{
    return std::filesystem::path(std::u8string(path.begin(), path.end()));
}

inline std::string pathToUtf8(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return { text.begin(), text.end() };
}

} // namespace brockdj::stems
