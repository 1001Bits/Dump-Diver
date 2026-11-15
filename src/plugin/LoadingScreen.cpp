#include "plugin/LoadingScreen.h"

#include "plugin/Log.h"
#include "plugin/Plugin.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace dd::plugin
{
    namespace
    {
#if defined(DD_GAME_SKYRIM)
        constexpr std::size_t kAdvanceSlot = 5;  // IMenu::AdvanceMovie(float, std::uint32_t)
#else
        constexpr std::size_t kAdvanceSlot = 4;  // IMenu::AdvanceMovie(float, std::uint64_t)
#endif
        constexpr std::size_t kDestructorSlot = 0;
        constexpr double      kClosedAfterSeconds = 1.5;


        struct ImageSection
        {
            const std::uint8_t* start = nullptr;
            std::size_t         size = 0;
            bool                writable = false;
            bool                executable = false;
        };

        std::vector<ImageSection> Sections(const std::uint8_t* base)
        {
            std::vector<ImageSection> out;
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
                return out;
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
                return out;
            const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
            for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
                ImageSection s;
                s.start = base + section->VirtualAddress;
                s.size = section->Misc.VirtualSize;
                s.writable = (section->Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
                s.executable = (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
                if (s.size)
                    out.push_back(s);
            }
            return out;
        }

        bool InExecutable(const std::vector<ImageSection>& sections, const void* address)
        {
            const auto* p = static_cast<const std::uint8_t*>(address);
            return std::any_of(sections.begin(), sections.end(),
                               [&](const ImageSection& s) { return s.executable && p >= s.start && p < s.start + s.size; });
        }

        double Seconds(std::uint64_t ticks)
        {
            const Header* shared = Shared();
            return shared && shared->qpcFrequency ? static_cast<double>(ticks) / static_cast<double>(shared->qpcFrequency) : 0;
        }

        void Close(std::uint64_t end)
        {
            Header* shared = Shared();
            if (!shared)
                return;
            const std::uint64_t open = shared->loadingOpenQpc;
            if (!open)
                return;
            if (InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&shared->loadingOpenQpc), 0,
                                             static_cast<LONG64>(open)) != static_cast<LONG64>(open))
                return;
            const auto ms = static_cast<std::uint32_t>(Seconds(end > open ? end - open : 0) * 1000.0);
            shared->loadingLastMs = ms;
            if (ms > shared->loadingLongestMs)
                shared->loadingLongestMs = ms;
            PushEvent(EventType::kLoadingScreenClosed, ms);
            DD_LOG_INFO("loading screen closed after %u ms", ms);
        }

        void OnAdvance()
        {
            if (Header* shared = Shared()) {
                const std::uint64_t now = Qpc();
                shared->loadingAdvanceQpc = now;
                if (!shared->loadingOpenQpc &&
                    InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&shared->loadingOpenQpc),
                                                 static_cast<LONG64>(now), 0) == 0) {
                    shared->loadingCount = shared->loadingCount + 1;
                    PushEvent(EventType::kLoadingScreenOpened);
                    DD_LOG_INFO("loading screen opened");
                }
            }
        }

        void OnDestroyed() { Close(Qpc()); }

        // A thunk that calls notify() with every argument register saved, restores them and jumps
        // to the original function. It works for any signature and leaves no frame behind, so a
        // wrong vtable slot could only give a wrong signal, never break the call.
        void* MakeNotifyThunk(void (*notify)(), void* original)
        {
            std::uint8_t code[] = {
                0x51,                                      // push rcx
                0x52,                                      // push rdx
                0x41, 0x50,                                // push r8
                0x41, 0x51,                                // push r9
                0x48, 0x83, 0xEC, 0x68,                    // sub rsp, 68h (shadow space + xmm0-3; keeps 16-byte alignment)
                0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20,        // movdqu [rsp+20h], xmm0
                0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30,        // movdqu [rsp+30h], xmm1
                0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40,        // movdqu [rsp+40h], xmm2
                0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50,        // movdqu [rsp+50h], xmm3
                0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,        // mov rax, notify
                0xFF, 0xD0,                                // call rax
                0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20,        // movdqu xmm0, [rsp+20h]
                0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30,        // movdqu xmm1, [rsp+30h]
                0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40,        // movdqu xmm2, [rsp+40h]
                0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50,        // movdqu xmm3, [rsp+50h]
                0x48, 0x83, 0xC4, 0x68,                    // add rsp, 68h
                0x41, 0x59,                                // pop r9
                0x41, 0x58,                                // pop r8
                0x5A,                                      // pop rdx
                0x59,                                      // pop rcx
                0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,        // mov rax, original
                0xFF, 0xE0,                                // jmp rax
            };
            const auto notifyAddress = reinterpret_cast<std::uint64_t>(notify);
            const auto originalAddress = reinterpret_cast<std::uint64_t>(original);
            std::memcpy(code + 36, &notifyAddress, 8);
            static_assert(sizeof(code) == 92, "the immediates below are at fixed offsets");
            std::memcpy(code + 82, &originalAddress, 8);
            void* page = VirtualAlloc(nullptr, sizeof(code), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!page)
                return nullptr;
            std::memcpy(page, code, sizeof(code));
            DWORD old = 0;
            VirtualProtect(page, sizeof(code), PAGE_EXECUTE_READ, &old);
            FlushInstructionCache(GetCurrentProcess(), page, sizeof(code));
            return page;
        }
    }

    VtableSearch FindVtable(const std::uint8_t* base, const char* mangledName)
    {
        VtableSearch result;
        const auto   sections = Sections(base);
        const std::size_t nameLength = std::strlen(mangledName) + 1;  // with the terminating zero

        for (const auto& data : sections) {
            if (!data.writable || data.executable)
                continue;  // type descriptors live in writable data
            const std::uint8_t* end = data.start + data.size;
            for (const std::uint8_t* p = data.start + 0x10; p + nameLength <= end; ++p) {
                p = static_cast<const std::uint8_t*>(std::memchr(p, mangledName[0], static_cast<std::size_t>(end - p)));
                if (!p || p + nameLength > end)
                    break;
                if (std::memcmp(p, mangledName, nameLength) != 0)
                    continue;
                ++result.typeDescriptors;
                const auto descriptorRva = static_cast<std::int32_t>((p - 0x10) - base);

                for (const auto& rdata : sections) {
                    if (rdata.writable || rdata.executable)
                        continue;
                    const std::uint8_t* first = rdata.start;
                    const std::uint8_t* last = rdata.start + rdata.size;
                    for (const std::uint8_t* q = first; q + 24 <= last; q += 4) {
                        std::int32_t locator[6];
                        std::memcpy(locator, q, sizeof(locator));
                        // signature 1 (x64), offset 0 (primary vtable), type descriptor, self RVA
                        if (locator[0] != 1 || locator[1] != 0 || locator[3] != descriptorRva ||
                            locator[5] != static_cast<std::int32_t>(q - base))
                            continue;
                        ++result.locators;
                        const auto locatorAddress = reinterpret_cast<std::uint64_t>(q);
                        for (const auto& vdata : sections) {
                            if (vdata.writable || vdata.executable)
                                continue;
                            for (const std::uint8_t* v = vdata.start; v + 16 <= vdata.start + vdata.size; v += 8) {
                                std::uint64_t value = 0;
                                std::memcpy(&value, v, 8);
                                if (value == locatorAddress) {
                                    result.vtable = const_cast<void**>(reinterpret_cast<void* const*>(v + 8));
                                    return result;
                                }
                            }
                        }
                    }
                }
            }
        }
        return result;
    }

    bool InstallLoadingScreenWatch()
    {
        const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
        const auto  start = Qpc();
        const auto  search = FindVtable(base, ".?AVLoadingMenu@@");
        const auto  ms = Seconds(Qpc() - start) * 1000.0;
        if (!search.vtable) {
            DD_LOG_WARN("loading screens cannot be watched: LoadingMenu not found (%d type descriptor(s), %d locator(s), %.0f ms)",
                        search.typeDescriptors, search.locators, ms);
            return false;
        }
        void** vtable = search.vtable;
        const auto sections = Sections(base);
        if (!InExecutable(sections, vtable[kAdvanceSlot]) || !InExecutable(sections, vtable[kDestructorSlot])) {
            DD_LOG_WARN("loading screens cannot be watched: LoadingMenu's vtable does not look as expected");
            return false;
        }

        DWORD old = 0;
        if (!VirtualProtect(vtable, sizeof(void*) * (kAdvanceSlot + 1), PAGE_READWRITE, &old)) {
            DD_LOG_WARN("loading screens cannot be watched: cannot write LoadingMenu's vtable (error %lu)", GetLastError());
            return false;
        }
        void* advanceThunk = MakeNotifyThunk(&OnAdvance, vtable[kAdvanceSlot]);
        void* destructorThunk = MakeNotifyThunk(&OnDestroyed, vtable[kDestructorSlot]);
        if (!advanceThunk || !destructorThunk) {
            VirtualProtect(vtable, sizeof(void*) * (kAdvanceSlot + 1), old, &old);
            DD_LOG_WARN("loading screens cannot be watched: no memory for the hooks");
            return false;
        }
        vtable[kAdvanceSlot] = advanceThunk;
        vtable[kDestructorSlot] = destructorThunk;
        VirtualProtect(vtable, sizeof(void*) * (kAdvanceSlot + 1), old, &old);

        if (Header* shared = Shared())
            shared->loadingWatch = 1;
        DD_LOG_INFO("watching loading screens (LoadingMenu vtable at exe+0x%llx, found in %.0f ms)",
                    static_cast<unsigned long long>(reinterpret_cast<const std::uint8_t*>(vtable) - base), ms);
        return true;
    }

    void CheckLoadingScreenClosed(std::uint64_t now)
    {
        const Header* shared = Shared();
        if (!shared || !shared->loadingOpenQpc)
            return;
        const std::uint64_t advance = shared->loadingAdvanceQpc;
        if (now > advance && Seconds(now - advance) > kClosedAfterSeconds)
            Close(advance);
    }
}
