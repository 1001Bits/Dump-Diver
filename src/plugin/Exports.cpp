// Script extender entry points. One source, built once for SKSE and once for F4SE.

#include "plugin/LockWatch.h"
#include "plugin/Plugin.h"

namespace
{
    HMODULE g_module = nullptr;

    constexpr std::uint32_t kPluginVersion =
        (DD_VERSION_MAJOR << 24) | (DD_VERSION_MINOR << 16) | (DD_VERSION_PATCH << 4);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        g_module = module;
    else if (reason == DLL_THREAD_DETACH)
        dd::plugin::OnThreadExit();  // thread notifications stay on for this
    return TRUE;
}

#if defined(DD_GAME_SKYRIM)

struct SKSEPluginVersionData
{
    std::uint32_t dataVersion;
    std::uint32_t pluginVersion;
    char          name[256];
    char          author[256];
    char          supportEmail[252];
    std::uint32_t versionIndependenceEx;
    std::uint32_t versionIndependence;
    std::uint32_t compatibleVersions[16];
    std::uint32_t seVersionRequired;
};

// No game addresses and no game structures, so SKSE loads it on any AE runtime.
extern "C" __declspec(dllexport) SKSEPluginVersionData SKSEPlugin_Version = {
    1,
    kPluginVersion,
    "DumpDiver",
    "Asciimov",
    "",
    1 << 0,  // kVersionIndependentEx_NoStructUse
    1 << 1,  // kVersionIndependent_Signatures
    { 0 },
    0,
};

// SE 1.5.97 and VR
extern "C" __declspec(dllexport) bool SKSEPlugin_Query(const xse::Interface* skse, xse::PluginInfo* info)
{
    info->infoVersion = 1;
    info->name = "DumpDiver";
    info->version = kPluginVersion;
    return skse && !skse->isEditor;
}

extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const xse::Interface* skse)
{
    return dd::plugin::Load(g_module, skse);
}

#else

struct F4SEPluginVersionData
{
    std::uint32_t dataVersion;
    std::uint32_t pluginVersion;
    char          name[256];
    char          author[256];
    std::uint32_t addressIndependence;
    std::uint32_t structureIndependence;
    std::uint32_t compatibleVersions[16];
    std::uint32_t xseMinimum;
    std::uint32_t reservedNonBreaking;
    std::uint32_t reservedBreaking;
    std::uint8_t  reserved[512];
};
static_assert(sizeof(F4SEPluginVersionData) == 0x45C);

// NG and AE (F4SE 0.7+): no game addresses and no game structures.
extern "C" __declspec(dllexport) F4SEPluginVersionData F4SEPlugin_Version = {
    1,
    kPluginVersion,
    "DumpDiver",
    "Asciimov",
    1 << 0,  // address independent (uses no game addresses)
    1 << 0,  // structure independent (uses no game structures)
    { 0 },
    0,
    0,
    0,
    { 0 },
};

// OG 1.10.163 and VR
extern "C" __declspec(dllexport) bool F4SEPlugin_Query(const xse::Interface* f4se, xse::PluginInfo* info)
{
    info->infoVersion = 1;
    info->name = "DumpDiver";
    info->version = kPluginVersion;
    return f4se && !f4se->isEditor;
}

extern "C" __declspec(dllexport) bool F4SEPlugin_Load(const xse::Interface* f4se)
{
    return dd::plugin::Load(g_module, f4se);
}

#endif
