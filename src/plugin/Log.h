#pragma once

#include <filesystem>

namespace dd::plugin
{
    void LogOpen(const std::filesystem::path& file);
    void LogWrite(const char* level, const char* format, ...);
}

#define DD_LOG_INFO(...) ::dd::plugin::LogWrite("info", __VA_ARGS__)
#define DD_LOG_WARN(...) ::dd::plugin::LogWrite("warn", __VA_ARGS__)
#define DD_LOG_ERROR(...) ::dd::plugin::LogWrite("error", __VA_ARGS__)
