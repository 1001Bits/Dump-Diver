#pragma once

#include "helper/Snapshot.h"

#include <windows.h>

#include <string>
#include <vector>

namespace dd::helper
{
    struct SystemThread
    {
        std::uint32_t tid = 0;
        std::uint64_t cpuTime100ns = 0;
        std::uint32_t contextSwitches = 0;
        std::uint32_t state = 0;
        std::uint32_t waitReason = 0;
    };

    struct Section
    {
        std::uint64_t start = 0;
        std::uint64_t end = 0;
        bool          writable = false;
        bool          executable = false;
    };

    std::vector<Module>       EnumerateModules(HANDLE process, const std::wstring& exePath);
    std::vector<SystemThread> QuerySystemThreads(std::uint32_t pid);
    std::vector<Section>      ImageSections(HANDLE process, std::uint64_t base);
    std::uint64_t             ThreadStartAddress(HANDLE thread);
    std::wstring              ThreadName(HANDLE thread);
    std::wstring              ProcessImagePath(HANDLE process);
    HWND                      FindGameWindow(std::uint32_t pid);
    bool                      ReadMemory(HANDLE process, std::uint64_t address, void* out, std::size_t size);

    // Mod Organizer keeps each mod in <instance>\mods\<name>; returns <name>, "Overwrite", or "".
    std::wstring ModNameFromPath(const std::wstring& realPath);
    ModuleKind   ClassifyModule(const Module& module, const std::wstring& gameDir, std::uint64_t exeBase);

    bool        IsInfrastructure(ModuleKind kind);  // never the culprit and skipped when attributing
    bool        IsBlameable(ModuleKind kind);
    bool        IsTestHarness(const std::wstring& name);
    bool        IsRuntimeRecordPatcher(const std::wstring& name);
    bool        CanChangeNpcFaces(const std::wstring& name);
    const char* KindName(ModuleKind kind);
}
