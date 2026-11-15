#include "helper/Util.h"

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <mutex>

namespace dd::helper
{
    std::string Utf8(std::wstring_view text)
    {
        if (text.empty())
            return {};
        const int   n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<std::size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
        return out;
    }

    std::wstring Wide(std::string_view text)
    {
        if (text.empty())
            return {};
        const int    n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring out(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
        return out;
    }

    std::wstring Hex(std::uint64_t value)
    {
        wchar_t buffer[32];
        swprintf_s(buffer, L"0x%llx", static_cast<unsigned long long>(value));
        return buffer;
    }

    std::wstring Lower(std::wstring_view text)
    {
        std::wstring out(text);
        for (auto& c : out)
            c = static_cast<wchar_t>(std::towlower(c));
        return out;
    }

    bool EqualsI(std::wstring_view a, std::wstring_view b)
    {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (std::towlower(a[i]) != std::towlower(b[i]))
                return false;
        }
        return true;
    }

    bool StartsWithI(std::wstring_view text, std::wstring_view prefix)
    {
        return text.size() >= prefix.size() && EqualsI(text.substr(0, prefix.size()), prefix);
    }

    bool ContainsI(std::wstring_view text, std::wstring_view part)
    {
        return Lower(text).find(Lower(part)) != std::wstring::npos;
    }

    std::wstring Format(const wchar_t* format, ...)
    {
        va_list args;
        va_start(args, format);
        const int n = _vscwprintf(format, args);
        va_end(args);
        if (n <= 0)
            return {};
        std::wstring out(static_cast<std::size_t>(n) + 1, L'\0');
        va_start(args, format);
        vswprintf_s(out.data(), out.size(), format, args);
        va_end(args);
        out.resize(static_cast<std::size_t>(n));
        return out;
    }

    namespace
    {
        std::mutex g_logLock;
        FILE*      g_log = nullptr;
    }

    void LogOpen(const std::filesystem::path& file)
    {
        std::lock_guard lock(g_logLock);
        if (g_log)
            return;
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        g_log = _wfsopen(file.c_str(), L"w, ccs=UTF-8", _SH_DENYWR);
    }

    void Log(const wchar_t* format, ...)
    {
        wchar_t message[4096];
        va_list args;
        va_start(args, format);
        vswprintf_s(message, format, args);
        va_end(args);

        SYSTEMTIME time{};
        GetLocalTime(&time);
        std::lock_guard lock(g_logLock);
        if (!g_log)
            return;
        fwprintf(g_log, L"%02u:%02u:%02u.%03u %s\n", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, message);
        fflush(g_log);
    }
}
