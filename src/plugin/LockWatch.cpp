#include "plugin/LockWatch.h"

#include "plugin/Log.h"
#include "plugin/Plugin.h"

#include <emmintrin.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

// Nothing here runs while the game plays normally: the global locks are found once, on a background
// thread at startup, and afterwards a check reads only those few lock words (microseconds). Checks
// happen around the messages the script extender sends (startup, load, save) and when a thread exits.

namespace dd::plugin
{
    namespace
    {
        struct Range
        {
            std::uint64_t start = 0;
            std::uint64_t end = 0;
        };

        constexpr std::size_t kMaxOwned = 16;

        std::uint64_t      g_exeBase = 0;
        std::vector<Range> g_code;
        std::vector<Range> g_data;
        volatile LONG      g_exitChecks = 0;

        // Engine locks in the game's data, found once (sorted). Read-only after g_locksReady.
        std::vector<std::uint64_t> g_locks;
        std::atomic<bool>          g_locksReady{ false };

        using QueryThreadFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
        QueryThreadFn g_queryThread = nullptr;

        thread_local bool t_ranHandler = false;  // this thread ran a wrapped message handler

        // BSReadWriteLock { writer thread, count | 0x80000000 while written } and BSSpinLock
        // { owner thread, count }. A writer flag with no count is the state an unlock in the wrong
        // order leaves behind.
        bool LooksHeld(std::uint32_t word) { return word != 0 && (word & 0x7FFFFFFFu) <= 0xFFFFu; }

        bool InRanges(const std::vector<Range>& ranges, std::uint64_t address, std::uint64_t size = 1)
        {
            return std::any_of(ranges.begin(), ranges.end(),
                               [&](const Range& r) { return address >= r.start && address + size <= r.end; });
        }

        // --- finding the engine's global locks ---------------------------------------------
        //
        // Lock functions: small functions that call GetCurrentThreadId through the import table,
        // compare it with dword [r] and do lock cmpxchg / lock inc on dword [r+4] (BSReadWriteLock and
        // BSSpinLock in both games). Global locks: the address rcx gets from lea r64,[rip+X] right
        // before a call or jump to one of them, X in the executable's data.

        std::uint64_t ImportSlot(const std::uint8_t* base, const char* name)
        {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if (!dir.VirtualAddress)
                return 0;
            for (const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc) {
                const DWORD names = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
                const auto* thunk = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + names);
                for (std::size_t i = 0; thunk[i].u1.AddressOfData; ++i) {
                    if (IMAGE_SNAP_BY_ORDINAL64(thunk[i].u1.Ordinal))
                        continue;
                    const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + thunk[i].u1.AddressOfData);
                    if (std::strcmp(reinterpret_cast<const char*>(byName->Name), name) == 0)
                        return reinterpret_cast<std::uint64_t>(base) + desc->FirstThunk + i * sizeof(std::uint64_t);
                }
            }
            return 0;
        }

        bool LockShape(const std::uint8_t* body, std::size_t size)
        {
            // bit r: the owner dword [r] is compared (39 /r, 3B /r) or loaded for a compare (8B /r)
            unsigned cmpRegs = 0;
            for (std::size_t k = 0; k + 1 < size; ++k) {
                const std::uint8_t modrm = body[k + 1];
                if ((body[k] == 0x39 || body[k] == 0x3B || body[k] == 0x8B) && (modrm & 0xC0) == 0x00 && (modrm & 7) != 4 &&
                    (modrm & 7) != 5)
                    cmpRegs |= 1u << (modrm & 7);
            }
            if (!cmpRegs)
                return false;
            for (std::size_t k = 0; k + 5 <= size; ++k) {
                if (body[k] != 0xF0)
                    continue;
                // lock cmpxchg dword [r+4], reg
                if (body[k + 1] == 0x0F && body[k + 2] == 0xB1 && (body[k + 3] & 0xC0) == 0x40 && body[k + 4] == 4 &&
                    (cmpRegs & (1u << (body[k + 3] & 7))))
                    return true;
                // lock inc dword [r+4]
                if (body[k + 1] == 0xFF && (body[k + 2] & 0xF8) == 0x40 && body[k + 3] == 4 && (cmpRegs & (1u << (body[k + 2] & 7))))
                    return true;
            }
            return false;
        }

        void FindLocks()
        {
            const std::uint64_t started = Qpc();
            const auto*         base = reinterpret_cast<const std::uint8_t*>(g_exeBase);
            const std::uint64_t slot = ImportSlot(base, "GetCurrentThreadId");
            const auto*         dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto*         nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            const auto&         exc = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
            if (!slot || !exc.VirtualAddress) {
                DD_LOG_INFO("engine locks: the game has no import or unwind table to read; only thread exits are checked");
                return;
            }
            const auto*       functions = reinterpret_cast<const RUNTIME_FUNCTION*>(base + exc.VirtualAddress);
            const std::size_t functionCount = exc.Size / sizeof(RUNTIME_FUNCTION);
            auto              functionOf = [&](std::uint64_t address) -> const RUNTIME_FUNCTION* {
                const auto rva = static_cast<DWORD>(address - g_exeBase);
                const auto* end = functions + functionCount;
                const auto* it = std::upper_bound(functions, end, rva, [](DWORD v, const RUNTIME_FUNCTION& f) { return v < f.BeginAddress; });
                if (it == functions)
                    return nullptr;
                --it;
                return rva < it->EndAddress ? it : nullptr;
            };

            std::vector<std::uint64_t> lockFunctions;
            for (const Range& code : g_code) {
                const auto* p = reinterpret_cast<const std::uint8_t*>(code.start);
                const std::size_t size = static_cast<std::size_t>(code.end - code.start);
                for (std::size_t i = 0; i + 6 <= size; ++i) {
                    if (p[i] != 0xFF || p[i + 1] != 0x15)
                        continue;
                    std::int32_t disp;
                    std::memcpy(&disp, p + i + 2, 4);
                    if (code.start + i + 6 + static_cast<std::int64_t>(disp) != slot)
                        continue;
                    const RUNTIME_FUNCTION* f = functionOf(code.start + i);
                    if (!f || f->EndAddress - f->BeginAddress > 0xC0)
                        continue;
                    if (LockShape(base + f->BeginAddress, f->EndAddress - f->BeginAddress))
                        lockFunctions.push_back(g_exeBase + f->BeginAddress);
                }
            }
            std::sort(lockFunctions.begin(), lockFunctions.end());
            lockFunctions.erase(std::unique(lockFunctions.begin(), lockFunctions.end()), lockFunctions.end());
            if (lockFunctions.empty()) {
                DD_LOG_INFO("engine locks: no lock functions found in this game; only thread exits are checked");
                return;
            }

            std::vector<std::uint64_t> locks;
            const std::uint64_t        lowest = lockFunctions.front();
            const std::uint64_t        highest = lockFunctions.back();
            for (const Range& code : g_code) {
                const auto*       p = reinterpret_cast<const std::uint8_t*>(code.start);
                const std::size_t size = static_cast<std::size_t>(code.end - code.start);
                for (std::size_t i = 40; i + 5 <= size; ++i) {
                    if (p[i] != 0xE8 && p[i] != 0xE9)
                        continue;
                    std::int32_t disp;
                    std::memcpy(&disp, p + i + 1, 4);
                    const std::uint64_t target = code.start + i + 5 + static_cast<std::int64_t>(disp);
                    if (target < lowest || target > highest || !std::binary_search(lockFunctions.begin(), lockFunctions.end(), target))
                        continue;
                    // The last lea r64,[rip+X] into data in the 40 bytes before, if rcx is that register.
                    std::uint64_t found = 0;
                    for (std::size_t j = i - 40; j + 7 <= i; ++j) {
                        const std::uint8_t rex = p[j];
                        const std::uint8_t modrm = p[j + 2];
                        if ((rex != 0x48 && rex != 0x4C) || p[j + 1] != 0x8D || (modrm & 0xC7) != 0x05)
                            continue;
                        std::int32_t leaDisp;
                        std::memcpy(&leaDisp, p + j + 3, 4);
                        const std::uint64_t x = code.start + j + 7 + static_cast<std::int64_t>(leaDisp);
                        if (!InRanges(g_data, x, 8))
                            continue;
                        const unsigned reg = ((modrm >> 3) & 7) | (rex == 0x4C ? 8u : 0u);
                        bool           intoRcx = reg == 1;
                        for (std::size_t k = j + 7; !intoRcx && k + 3 <= i; ++k) {
                            // mov rcx, reg: 48/49 8B C8|r, or 48/4C 89 C1|r<<3
                            if (reg < 8)
                                intoRcx = (p[k] == 0x48 && p[k + 1] == 0x8B && p[k + 2] == (0xC8 | reg)) ||
                                          (p[k] == 0x48 && p[k + 1] == 0x89 && p[k + 2] == (0xC1 | (reg << 3)));
                            else
                                intoRcx = (p[k] == 0x49 && p[k + 1] == 0x8B && p[k + 2] == (0xC8 | (reg - 8))) ||
                                          (p[k] == 0x4C && p[k + 1] == 0x89 && p[k + 2] == (0xC1 | ((reg - 8) << 3)));
                        }
                        if (intoRcx)
                            found = x;
                    }
                    if (found)
                        locks.push_back(found);
                }
            }
            std::sort(locks.begin(), locks.end());
            locks.erase(std::unique(locks.begin(), locks.end()), locks.end());
            g_locks = std::move(locks);
            const double ms = static_cast<double>(Qpc() - started) * 1000.0 / static_cast<double>(Shared()->qpcFrequency);
            DD_LOG_INFO("engine locks: %zu lock functions, %zu global locks found in %.0f ms (in the background)",
                        lockFunctions.size(), g_locks.size(), ms);
            if (!g_locks.empty())
                g_locksReady.store(true, std::memory_order_release);
        }

        void FindLocksSafely()
        {
            __try {
                FindLocks();
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                g_locksReady.store(false);
            }
        }

        // --- checking ------------------------------------------------------------------------

        // Known global locks this thread holds: a few dozen reads.
        std::size_t KnownLocksHeld(std::uint32_t tid, std::uint64_t* out, std::size_t max)
        {
            std::size_t n = 0;
            for (const std::uint64_t lock : g_locks) {
                const auto* words = reinterpret_cast<const volatile std::uint32_t*>(lock);
                if (words[0] == tid && LooksHeld(words[1]) && n < max)
                    out[n++] = lock;
            }
            return n;
        }

        // Every lock-shaped pair in the game's data naming this thread: a full pass over the data
        // (about 2 ms). Only for a thread that ran mod message handlers, once, when it exits.
        std::size_t AllLocksHeld(std::uint32_t tid, std::uint64_t* out, std::size_t max)
        {
            std::size_t   n = 0;
            const __m128i key = _mm_set1_epi32(static_cast<int>(tid));
            for (const Range& r : g_data) {
                const auto*       p = reinterpret_cast<const std::uint32_t*>(r.start);
                const std::size_t count = static_cast<std::size_t>(r.end - r.start) / 4;
                std::size_t       i = 0;
                for (; i + 5 <= count && n < max; i += 4) {
                    const int mask = _mm_movemask_ps(
                        _mm_castsi128_ps(_mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i)), key)));
                    for (int k = 0; mask && k < 4 && n < max; ++k) {
                        if ((mask & (1 << k)) && LooksHeld(p[i + k + 1]))
                            out[n++] = reinterpret_cast<std::uint64_t>(p + i + k);
                    }
                }
            }
            return n;
        }

        template <class Fn>
        bool Guarded(Fn&& fn, std::size_t& count)
        {
            __try {
                count = fn();
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                count = 0;
                return false;
            }
        }

        std::uint32_t ReadWord(std::uint64_t lock)
        {
            __try {
                return *reinterpret_cast<const volatile std::uint32_t*>(lock + 4);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        void Record(LockRecordKind kind, std::uint64_t lock, std::uint64_t code, std::uint32_t tid, std::uint32_t messageType)
        {
            Header* shared = Shared();
            if (!shared)
                return;
            const LONG index = InterlockedIncrement(reinterpret_cast<volatile LONG*>(&shared->lockRecordCount)) - 1;
            if (index < 0 || static_cast<std::uint32_t>(index) >= kLockRecords)
                return;
            LockRecord& r = shared->lockRecords[index];
            r.qpc = Qpc();
            r.lockAddress = lock;
            r.code = code;
            r.threadId = tid;
            r.lockWord = ReadWord(lock);
            r.messageType = messageType;
            _WriteBarrier();
            r.kind = static_cast<std::uint32_t>(kind);  // last: the record is complete
        }

        bool Contains(const std::uint64_t* list, std::size_t n, std::uint64_t value)
        {
            return std::find(list, list + n, value) != list + n;
        }

        // --- message handlers ------------------------------------------------------------

        using RegisterFn = bool (*)(xse::PluginHandle, const char*, xse::EventCallback);

        // Identical position-independent thunks, written once: each passes its own address to the
        // wrapper, which finds the handler by the thunk's index. Nothing is written to the page after
        // it becomes executable.
        constexpr std::size_t kThunkSize = 32;
        constexpr std::size_t kThunks = 256;

        RegisterFn         g_register = nullptr;
        std::uint8_t*      g_thunks = nullptr;
        xse::EventCallback g_handlers[kThunks];
        volatile LONG      g_handlerCount = 0;

        void ListenerWrapper(xse::Message* message, const std::uint8_t* thunk)
        {
            const auto handler = g_handlers[static_cast<std::size_t>(thunk - g_thunks) / kThunkSize];
            t_ranHandler = true;
            if (!message || !g_locksReady.load(std::memory_order_acquire) || !message->sender ||
                std::strcmp(message->sender, xse::kLoaderName) != 0) {
                handler(message);
                return;
            }
            const std::uint32_t type = message->type;
            const std::uint32_t tid = GetCurrentThreadId();
            std::uint64_t       before[kMaxOwned];
            std::size_t         beforeCount = 0;
            const bool          checked = Guarded([&] { return KnownLocksHeld(tid, before, kMaxOwned); }, beforeCount);
            handler(message);
            std::uint64_t after[kMaxOwned];
            std::size_t   afterCount = 0;
            if (!checked || !Guarded([&] { return KnownLocksHeld(tid, after, kMaxOwned); }, afterCount))
                return;
            for (std::size_t i = 0; i < afterCount; ++i) {
                if (!Contains(before, beforeCount, after[i]))
                    Record(LockRecordKind::kLeftByHandler, after[i], reinterpret_cast<std::uint64_t>(handler), tid, type);
            }
        }

        bool HookRegisterListener(xse::PluginHandle plugin, const char* sender, xse::EventCallback handler)
        {
            if (!handler)
                return g_register(plugin, sender, handler);
            const LONG index = InterlockedIncrement(&g_handlerCount) - 1;
            if (index < 0 || static_cast<std::size_t>(index) >= kThunks)
                return g_register(plugin, sender, handler);
            g_handlers[index] = handler;
            _WriteBarrier();
            return g_register(plugin, sender, reinterpret_cast<xse::EventCallback>(g_thunks + static_cast<std::size_t>(index) * kThunkSize));
        }

        bool MakeThunks()
        {
            auto* page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kThunks * kThunkSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            if (!page)
                return false;
            const auto wrapper = reinterpret_cast<std::uint64_t>(&ListenerWrapper);
            for (std::size_t i = 0; i < kThunks; ++i) {
                std::uint8_t code[kThunkSize] = {
                    0x48, 0x8D, 0x15, 0xF9, 0xFF, 0xFF, 0xFF,  // lea rdx, [rip-7]: this thunk
                    0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,        // jmp [rip]
                    0, 0, 0, 0, 0, 0, 0, 0,                    // ListenerWrapper
                };
                std::memcpy(code + 13, &wrapper, 8);
                std::memset(code + 21, 0xCC, kThunkSize - 21);
                std::memcpy(page + i * kThunkSize, code, kThunkSize);
            }
            DWORD old = 0;
            if (!VirtualProtect(page, kThunks * kThunkSize, PAGE_EXECUTE_READ, &old)) {
                VirtualFree(page, 0, MEM_RELEASE);
                return false;
            }
            FlushInstructionCache(GetCurrentProcess(), page, kThunks * kThunkSize);
            g_thunks = page;
            return true;
        }

        std::string ModuleOffset(std::uint64_t address)
        {
            HMODULE module = nullptr;
            char    text[400];
            if (!address ||
                !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(address), &module) ||
                !module) {
                snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
                return text;
            }
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(module, path, MAX_PATH);
            const wchar_t* name = wcsrchr(path, L'\\');
            snprintf(text, sizeof(text), "%ls+0x%llx", name ? name + 1 : path,
                     static_cast<unsigned long long>(address - reinterpret_cast<std::uint64_t>(module)));
            return text;
        }
    }

    bool InitLockWatch()
    {
        const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            return false;
        g_exeBase = reinterpret_cast<std::uint64_t>(base);
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
            const bool          writable = (section->Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
            const bool          executable = (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
            const std::uint64_t start = g_exeBase + section->VirtualAddress;
            const Range         range{ start, start + (section->Misc.VirtualSize & ~std::uint64_t{ 3 }) };
            if (range.end - range.start < 8)
                continue;
            if (executable && !writable)
                g_code.push_back(range);
            else if (writable && !executable)
                g_data.push_back(range);
        }
        Header* shared = Shared();
        if (g_data.empty() || !shared)
            return false;
        if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
            g_queryThread = reinterpret_cast<QueryThreadFn>(GetProcAddress(ntdll, "NtQueryInformationThread"));
        InterlockedExchange(&g_exitChecks, 1);
        shared->lockWatch = shared->lockWatch | 2u;
        if (!g_code.empty())
            std::thread(FindLocksSafely).detach();
        return true;
    }

    bool InstallListenerTracking(xse::MessagingInterface* messaging)
    {
        Header* shared = Shared();
        if (!shared || !messaging || !messaging->RegisterListener || g_code.empty() || g_register)
            return false;
        if (!MakeThunks())
            return false;
        g_register = messaging->RegisterListener;
        DWORD old = 0;
        if (!VirtualProtect(&messaging->RegisterListener, sizeof(void*), PAGE_READWRITE, &old)) {
            DD_LOG_WARN("message handler checks off: cannot write the messaging interface (error %lu)", GetLastError());
            g_register = nullptr;
            return false;
        }
        messaging->RegisterListener = &HookRegisterListener;
        VirtualProtect(&messaging->RegisterListener, sizeof(void*), old, &old);
        FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
        shared->lockWatch = shared->lockWatch | 1u;
        return true;
    }

    bool RegisterOwnListener(xse::MessagingInterface* messaging, xse::PluginHandle self, const char* sender,
                             xse::EventCallback handler)
    {
        const RegisterFn fn = g_register ? g_register : (messaging ? messaging->RegisterListener : nullptr);
        return fn && fn(self, sender, handler);
    }

    void OnThreadExit()
    {
        // Runs under the loader lock: no logging here (the heartbeat thread logs new records).
        if (!g_exitChecks || !Shared())
            return;
        const std::uint32_t tid = GetCurrentThreadId();
        std::uint64_t       held[kMaxOwned];
        std::size_t         n = 0;
        if (g_locksReady.load(std::memory_order_acquire))
            Guarded([&] { return KnownLocksHeld(tid, held, kMaxOwned); }, n);
        if (!n && t_ranHandler)
            Guarded([&] { return AllLocksHeld(tid, held, kMaxOwned); }, n);
        if (!n)
            return;
        std::uint64_t start = 0;
        if (g_queryThread)
            g_queryThread(GetCurrentThread(), 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), nullptr);
        for (std::size_t i = 0; i < n; ++i)
            Record(LockRecordKind::kHeldAtExit, held[i], start, tid, 0);
    }

    void LogLockRecords()
    {
        static std::uint32_t logged = 0;
        Header*              shared = Shared();
        if (!shared)
            return;
        const std::uint32_t total = shared->lockRecordCount < kLockRecords ? shared->lockRecordCount : kLockRecords;
        while (logged < total) {
            const LockRecord& r = shared->lockRecords[logged];
            if (r.kind == static_cast<std::uint32_t>(LockRecordKind::kNone))
                return;  // still being written
            const std::string lock = ModuleOffset(r.lockAddress);
            const std::string code = ModuleOffset(r.code);
            if (r.kind == static_cast<std::uint32_t>(LockRecordKind::kLeftByHandler))
                DD_LOG_WARN("thread %u still holds the engine lock at %s (state %08x) after the message handler %s returned "
                            "(message %u)",
                            r.threadId, lock.c_str(), r.lockWord, code.c_str(), r.messageType);
            else  // often harmless (a thread id kept next to a counter); it matters only if the game later waits on it
                DD_LOG_INFO("thread %u (started at %s) exited while %s still named it as owner (state %08x)", r.threadId,
                            code.c_str(), lock.c_str(), r.lockWord);
            ++logged;
        }
    }
}
