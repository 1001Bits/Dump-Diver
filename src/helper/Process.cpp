#include "helper/Process.h"

#include "helper/Util.h"

#include <psapi.h>

#include <array>
#include <filesystem>

namespace fs = std::filesystem;

namespace dd::helper
{
    namespace
    {
        // Layouts of the x64 NtQuerySystemInformation(SystemProcessInformation) records.
        struct SystemThreadInformation
        {
            LARGE_INTEGER kernelTime;
            LARGE_INTEGER userTime;
            LARGE_INTEGER createTime;
            ULONG         waitTime;
            PVOID         startAddress;
            HANDLE        uniqueProcess;
            HANDLE        uniqueThread;
            LONG          priority;
            LONG          basePriority;
            ULONG         contextSwitches;
            ULONG         threadState;
            ULONG         waitReason;
        };
        static_assert(sizeof(SystemThreadInformation) == 0x50);

        struct SystemProcessInformation
        {
            ULONG         nextEntryOffset;
            ULONG         numberOfThreads;
            std::uint8_t  reserved1[0x48];
            HANDLE        uniqueProcessId;
            std::uint8_t  reserved2[0xA8];
            // SystemThreadInformation threads[numberOfThreads] follows at 0x100
        };
        static_assert(offsetof(SystemProcessInformation, uniqueProcessId) == 0x50);
        static_assert(sizeof(SystemProcessInformation) == 0x100);

        using NtQuerySystemInformationFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
        using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

        FARPROC NtProc(const char* name) { return GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name); }

        // "\Device\HarddiskVolume3\Games\x.dll" -> "C:\Games\x.dll"
        std::wstring DevicePathToDos(const std::wstring& path)
        {
            wchar_t drives[512]{};
            if (!GetLogicalDriveStringsW(511, drives))
                return path;
            for (const wchar_t* drive = drives; *drive; drive += wcslen(drive) + 1) {
                const std::wstring letter(drive, 2);  // "C:"
                wchar_t            device[MAX_PATH]{};
                if (!QueryDosDeviceW(letter.c_str(), device, MAX_PATH))
                    continue;
                const std::wstring prefix = device;
                if (path.size() > prefix.size() && StartsWithI(path, prefix) && path[prefix.size()] == L'\\')
                    return letter + path.substr(prefix.size());
            }
            return path;
        }

        std::wstring FileVersion(const std::wstring& file)
        {
            DWORD       handle = 0;
            const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &handle);
            if (!size)
                return {};
            std::vector<std::uint8_t> data(size);
            VS_FIXEDFILEINFO*         info = nullptr;
            UINT                      length = 0;
            if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data()) ||
                !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info) {
                return {};
            }
            return Format(L"%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                          HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
        }

        bool InList(const std::wstring& name, std::initializer_list<const wchar_t*> list)
        {
            for (const wchar_t* item : list) {
                if (EqualsI(name, item))
                    return true;
            }
            return false;
        }
    }

    bool ReadMemory(HANDLE process, std::uint64_t address, void* out, std::size_t size)
    {
        SIZE_T read = 0;
        return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), out, size, &read) && read == size;
    }

    std::wstring ProcessImagePath(HANDLE process)
    {
        std::wstring buffer(32768, L'\0');
        DWORD        size = static_cast<DWORD>(buffer.size());
        if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &size))
            return {};
        buffer.resize(size);
        return buffer;
    }

    std::wstring ModNameFromPath(const std::wstring& realPath)
    {
        const std::wstring lower = Lower(realPath);
        const std::size_t  mods = lower.rfind(L"\\mods\\");
        if (mods != std::wstring::npos) {
            const std::size_t start = mods + 6;
            const std::size_t end = realPath.find(L'\\', start);
            if (end != std::wstring::npos && end > start)
                return realPath.substr(start, end - start);
        }
        if (lower.find(L"\\overwrite\\") != std::wstring::npos)
            return L"Overwrite";
        return {};
    }

    ModuleKind ClassifyModule(const Module& module, const std::wstring& gameDir, std::uint64_t exeBase)
    {
        const std::wstring& name = module.name;
        const std::wstring  path = Lower(module.path);

        if (module.base == exeBase)
            return ModuleKind::kGame;
        if (EqualsI(name, L"DumpDiver.dll"))
            return ModuleKind::kSelf;
        if (IsTestHarness(name))
            return ModuleKind::kTestHarness;

        if (InList(name, { L"ucrtbase.dll", L"vcruntime140.dll", L"vcruntime140_1.dll", L"msvcp140.dll",
                           L"msvcp140_1.dll", L"msvcp140_2.dll", L"msvcp140_atomic_wait.dll", L"concrt140.dll",
                           L"vcomp140.dll", L"msvcrt.dll" }))
            return ModuleKind::kRuntime;

        if (InList(name, { L"usvfs_x64.dll", L"usvfs_proxy_x64.dll" }))
            return ModuleKind::kModTool;

        if (StartsWithI(name, L"skse64_") || StartsWithI(name, L"sksevr_") || StartsWithI(name, L"f4se_") ||
            StartsWithI(name, L"f4sevr_"))
            return ModuleKind::kScriptExtender;

        if (InList(name, { L"GameOverlayRenderer64.dll", L"DiscordHook64.dll", L"RTSSHooks64.dll", L"nvspcap64.dll",
                           L"graphics-hook64.dll", L"overlay64.dll", L"medal-hook64.dll", L"fraps64.dll",
                           L"action_x64.dll", L"bdcamvk64.dll", L"EOSOVH-Win64-Shipping.dll", L"GameBarPresenceWriter.dll" }))
            return ModuleKind::kOverlay;

        const bool inGameDir = !gameDir.empty() && StartsWithI(module.path, gameDir);
        const bool graphicsName =
            InList(name, { L"d3d11.dll", L"dxgi.dll", L"d3d12.dll", L"d3d9.dll", L"d3d12core.dll", L"d3d11on12.dll",
                           L"d3d10warp.dll", L"dxcore.dll", L"d3dcompiler_47.dll", L"d3dcompiler_46e.dll",
                           L"d3dcompiler_43.dll", L"nvapi64.dll", L"nvwgf2umx.dll", L"nvldumdx.dll", L"nvd3dumx.dll",
                           L"nvgpucomp64.dll", L"atidxx64.dll", L"amdxx64.dll", L"amdxc64.dll", L"aticfx64.dll",
                           L"amdihk64.dll", L"igd10iumd64.dll", L"igd12umd64.dll", L"igdusc64.dll", L"igc64.dll" });
        if (inGameDir && InList(name, { L"d3d11.dll", L"dxgi.dll", L"d3d12.dll", L"d3d9.dll" }))
            return ModuleKind::kGraphicsInjector;  // ENB, ReShade, DXVK or similar
        if (StartsWithI(name, L"ReShade") || StartsWithI(name, L"enb"))
            return ModuleKind::kGraphicsInjector;
        if (graphicsName || path.find(L"\\driverstore\\") != std::wstring::npos)
            return ModuleKind::kGraphics;

        if (InList(name, { L"bink2w64.dll", L"binkw64.dll", L"steam_api64.dll", L"Galaxy64.dll", L"steamclient64.dll",
                           L"tier0_s64.dll", L"vstdlib_s64.dll", L"EOSSDK-Win64-Shipping.dll", L"flexExtRelease_x64.dll",
                           L"flexRelease_x64.dll", L"nvToolsExt64_1.dll", L"cudart64_75.dll", L"xinput1_3.dll",
                           L"x3daudio1_7.dll", L"xaudio2_7.dll" }) ||
            StartsWithI(name, L"GFSDK_") || StartsWithI(name, L"PhysX"))
            return ModuleKind::kGameLibrary;

        wchar_t windowsDir[MAX_PATH]{};
        GetWindowsDirectoryW(windowsDir, MAX_PATH);
        if (windowsDir[0] && StartsWithI(module.path, windowsDir))
            return ModuleKind::kSystem;

        if (inGameDir || !module.modName.empty())
            return ModuleKind::kPlugin;
        return ModuleKind::kOther;
    }

    bool IsInfrastructure(ModuleKind kind)
    {
        switch (kind) {
        case ModuleKind::kSystem:
        case ModuleKind::kRuntime:
        case ModuleKind::kModTool:
        case ModuleKind::kScriptExtender:
        case ModuleKind::kSelf:
        case ModuleKind::kTestHarness:
            return true;
        default:
            return false;
        }
    }

    bool IsTestHarness(const std::wstring& name)
    {
        return EqualsI(name, L"ModTestProbe.dll");  // Mod Test Lab's in-game probe
    }

    // Plugins that change game records while the game runs (distributors, patchers, swappers). Broken data
    // that no plugin file can hold points at them.
    bool IsRuntimeRecordPatcher(const std::wstring& name)
    {
        return InList(name, { L"SkyPatcher.dll", L"po3_SpellPerkItemDistributor.dll", L"po3_KeywordItemDistributor.dll",
                              L"po3_BaseObjectSwapper.dll", L"po3_ItemDistributor.dll", L"FormListManipulator.dll",
                              L"po3_SpellPerkItemDistributorVR.dll", L"SkyPatcherVR.dll", L"FLM.dll" });
    }

    // The runtime patchers that can set an NPC's face template (the rest add keywords, items or swap objects).
    bool CanChangeNpcFaces(const std::wstring& name)
    {
        return InList(name, { L"SkyPatcher.dll", L"SkyPatcherVR.dll" });
    }

    bool IsBlameable(ModuleKind kind)
    {
        switch (kind) {
        case ModuleKind::kPlugin:
        case ModuleKind::kGraphicsInjector:
        case ModuleKind::kOverlay:
        case ModuleKind::kOther:
            return true;
        default:
            return false;
        }
    }

    const char* KindName(ModuleKind kind)
    {
        switch (kind) {
        case ModuleKind::kGame:
            return "game";
        case ModuleKind::kSystem:
            return "windows";
        case ModuleKind::kRuntime:
            return "runtime";
        case ModuleKind::kGraphics:
            return "graphics";
        case ModuleKind::kGraphicsInjector:
            return "graphics-injector";
        case ModuleKind::kOverlay:
            return "overlay";
        case ModuleKind::kScriptExtender:
            return "script-extender";
        case ModuleKind::kGameLibrary:
            return "game-library";
        case ModuleKind::kModTool:
            return "mod-manager";
        case ModuleKind::kSelf:
            return "dump-diver";
        case ModuleKind::kTestHarness:
            return "test-harness";
        case ModuleKind::kPlugin:
            return "plugin";
        default:
            return "other";
        }
    }

    std::vector<Module> EnumerateModules(HANDLE process, const std::wstring& exePath)
    {
        std::vector<Module>  modules;
        std::vector<HMODULE> handles(1024);
        DWORD                needed = 0;
        for (;;) {
            if (!EnumProcessModulesEx(process, handles.data(), static_cast<DWORD>(handles.size() * sizeof(HMODULE)),
                                      &needed, LIST_MODULES_64BIT))
                return modules;
            if (needed <= handles.size() * sizeof(HMODULE))
                break;
            handles.resize(needed / sizeof(HMODULE) + 64);
        }
        handles.resize(needed / sizeof(HMODULE));

        const std::wstring gameDir = fs::path(exePath).parent_path().wstring() + L"\\";
        std::uint64_t      exeBase = 0;

        for (HMODULE handle : handles) {
            Module       module;
            MODULEINFO   info{};
            std::wstring buffer(32768, L'\0');
            if (!GetModuleInformation(process, handle, &info, sizeof(info)))
                continue;
            module.base = reinterpret_cast<std::uint64_t>(info.lpBaseOfDll);
            module.size = info.SizeOfImage;
            DWORD n = GetModuleFileNameExW(process, handle, buffer.data(), static_cast<DWORD>(buffer.size()));
            module.path.assign(buffer.c_str(), n);
            n = GetMappedFileNameW(process, info.lpBaseOfDll, buffer.data(), static_cast<DWORD>(buffer.size()));
            module.realPath = n ? DevicePathToDos(std::wstring(buffer.c_str(), n)) : module.path;
            module.name = fs::path(module.path).filename().wstring();
            module.modName = ModNameFromPath(module.realPath);
            module.version = FileVersion(module.realPath);
            if (exeBase == 0 && EqualsI(module.path, exePath))
                exeBase = module.base;
            modules.push_back(std::move(module));
        }
        if (!exeBase && !modules.empty())
            exeBase = modules.front().base;  // EnumProcessModules lists the executable first
        for (auto& module : modules)
            module.kind = ClassifyModule(module, gameDir, exeBase);
        return modules;
    }

    std::vector<SystemThread> QuerySystemThreads(std::uint32_t pid)
    {
        std::vector<SystemThread> threads;
        static const auto query = reinterpret_cast<NtQuerySystemInformationFn>(NtProc("NtQuerySystemInformation"));
        if (!query)
            return threads;
        std::vector<std::uint8_t> buffer(1 << 20);
        for (int attempt = 0; attempt < 8; ++attempt) {
            ULONG      needed = 0;
            const LONG status = query(5 /*SystemProcessInformation*/, buffer.data(), static_cast<ULONG>(buffer.size()), &needed);
            if (status == 0)
                break;
            if (status != static_cast<LONG>(0xC0000004))  // STATUS_INFO_LENGTH_MISMATCH
                return threads;
            buffer.resize(std::max<std::size_t>(buffer.size() * 2, needed + 65536));
        }
        std::size_t offset = 0;
        for (;;) {
            const auto* process = reinterpret_cast<const SystemProcessInformation*>(buffer.data() + offset);
            if (static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(process->uniqueProcessId)) == pid) {
                const auto* entries = reinterpret_cast<const SystemThreadInformation*>(process + 1);
                for (ULONG i = 0; i < process->numberOfThreads; ++i) {
                    SystemThread t;
                    t.tid = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(entries[i].uniqueThread));
                    t.cpuTime100ns = static_cast<std::uint64_t>(entries[i].kernelTime.QuadPart + entries[i].userTime.QuadPart);
                    t.contextSwitches = entries[i].contextSwitches;
                    t.state = entries[i].threadState;
                    t.waitReason = entries[i].waitReason;
                    threads.push_back(t);
                }
                break;
            }
            if (!process->nextEntryOffset)
                break;
            offset += process->nextEntryOffset;
        }
        return threads;
    }

    std::vector<Section> ImageSections(HANDLE process, std::uint64_t base)
    {
        std::vector<Section> sections;
        IMAGE_DOS_HEADER     dos{};
        if (!ReadMemory(process, base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE)
            return sections;
        IMAGE_NT_HEADERS64 nt{};
        if (!ReadMemory(process, base + static_cast<std::uint64_t>(dos.e_lfanew), &nt, sizeof(nt)) ||
            nt.Signature != IMAGE_NT_SIGNATURE)
            return sections;
        const std::uint64_t first = base + static_cast<std::uint64_t>(dos.e_lfanew) +
                                    offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
        std::vector<IMAGE_SECTION_HEADER> headers(nt.FileHeader.NumberOfSections);
        if (headers.empty() || !ReadMemory(process, first, headers.data(), headers.size() * sizeof(IMAGE_SECTION_HEADER)))
            return sections;
        for (const auto& h : headers) {
            Section s;
            s.start = base + h.VirtualAddress;
            s.end = s.start + std::max<std::uint64_t>(h.Misc.VirtualSize, h.SizeOfRawData);
            s.writable = (h.Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
            s.executable = (h.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
            sections.push_back(s);
        }
        return sections;
    }

    std::uint64_t ThreadStartAddress(HANDLE thread)
    {
        static const auto query = reinterpret_cast<NtQueryInformationThreadFn>(NtProc("NtQueryInformationThread"));
        if (!query)
            return 0;
        PVOID start = nullptr;
        if (query(thread, 9 /*ThreadQuerySetWin32StartAddress*/, &start, sizeof(start), nullptr) != 0)
            return 0;
        return reinterpret_cast<std::uint64_t>(start);
    }

    std::wstring ThreadName(HANDLE thread)
    {
        PWSTR description = nullptr;
        if (FAILED(GetThreadDescription(thread, &description)) || !description)
            return {};
        std::wstring name = description;
        LocalFree(description);
        return name;
    }

    namespace
    {
        struct WindowSearch
        {
            DWORD pid;
            HWND  best;
            LONG  bestArea;
        };

        BOOL CALLBACK FindWindowCallback(HWND hwnd, LPARAM param)
        {
            auto* search = reinterpret_cast<WindowSearch*>(param);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != search->pid || GetWindow(hwnd, GW_OWNER) || !IsWindowVisible(hwnd))
                return TRUE;
            wchar_t className[64]{};
            GetClassNameW(hwnd, className, 64);
            if (wcscmp(className, L"#32770") == 0)
                return TRUE;  // a dialog, not the game window
            RECT rect{};
            GetWindowRect(hwnd, &rect);
            const LONG area = (rect.right - rect.left) * (rect.bottom - rect.top);
            if (!search->best || area > search->bestArea) {
                search->best = hwnd;
                search->bestArea = area;
            }
            return TRUE;
        }
    }

    HWND FindGameWindow(std::uint32_t pid)
    {
        WindowSearch search{ pid, nullptr, 0 };
        EnumWindows(&FindWindowCallback, reinterpret_cast<LPARAM>(&search));
        return search.best;
    }
}
