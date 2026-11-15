#pragma once

#include <filesystem>

namespace dd::plugin::menu
{
    // SKSE Menu Framework (optional, Skyrim only): adds the page Dump Diver/Settings to its Mod Control Panel at
    // kPostLoad when SKSEMenuFramework.dll is loaded and has every export the page uses; otherwise nothing happens.
    void Register(const std::filesystem::path& ini);
}
