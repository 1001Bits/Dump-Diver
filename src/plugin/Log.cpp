#include "plugin/Log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace dd::plugin
{
    namespace
    {
        std::mutex g_lock;
        FILE*      g_file = nullptr;
    }

    void LogOpen(const std::filesystem::path& file)
    {
        std::lock_guard lock(g_lock);
        if (g_file)
            return;
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        g_file = _wfsopen(file.c_str(), L"w", _SH_DENYWR);
    }

    void LogWrite(const char* level, const char* format, ...)
    {
        char    message[2048];
        va_list args;
        va_start(args, format);
        vsnprintf(message, sizeof(message), format, args);
        va_end(args);

        SYSTEMTIME time{};
        GetLocalTime(&time);

        std::lock_guard lock(g_lock);
        if (!g_file)
            return;
        fprintf(g_file, "%02u:%02u:%02u.%03u [%s] %s\n", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
                level, message);
        fflush(g_file);
    }
}
