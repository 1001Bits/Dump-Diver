#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace dd::helper
{
    std::string  Utf8(std::wstring_view text);
    std::wstring Wide(std::string_view text);
    std::wstring Hex(std::uint64_t value);
    std::wstring Lower(std::wstring_view text);
    bool         EqualsI(std::wstring_view a, std::wstring_view b);
    bool         StartsWithI(std::wstring_view text, std::wstring_view prefix);
    bool         ContainsI(std::wstring_view text, std::wstring_view part);
    std::wstring Format(const wchar_t* format, ...);

    inline std::uint64_t Qpc()
    {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        return static_cast<std::uint64_t>(counter.QuadPart);
    }

    void LogOpen(const std::filesystem::path& file);
    void Log(const wchar_t* format, ...);
}
