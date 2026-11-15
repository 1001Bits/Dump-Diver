#include "helper/Stack.h"

#include "helper/Process.h"
#include "helper/Util.h"

#pragma warning(push)
#pragma warning(disable : 4091)
#include <dbghelp.h>
#pragma warning(pop)

#include <algorithm>
#include <iterator>

namespace dd::helper
{
    namespace
    {
        class SuspendGuard
        {
        public:
            explicit SuspendGuard(HANDLE thread) :
                thread_(thread),
                ok_(SuspendThread(thread) != static_cast<DWORD>(-1))
            {}
            ~SuspendGuard()
            {
                if (ok_)
                    ResumeThread(thread_);
            }
            SuspendGuard(const SuspendGuard&) = delete;
            SuspendGuard& operator=(const SuspendGuard&) = delete;
            bool ok() const { return ok_; }

        private:
            HANDLE thread_;
            bool   ok_;
        };

        StackWalker* g_active = nullptr;  // DbgHelp callbacks carry no context; one walker samples at a time

        PVOID CALLBACK FunctionTableAccess(HANDLE process, DWORD64 address)
        {
            if (g_active) {
                if (const RUNTIME_FUNCTION* entry = g_active->LookupFunction(address))
                    return const_cast<RUNTIME_FUNCTION*>(entry);
            }
            return SymFunctionTableAccess64(process, address);
        }

        DWORD64 CALLBACK GetModuleBase(HANDLE process, DWORD64 address)
        {
            if (g_active) {
                if (const std::uint64_t base = g_active->ModuleBase(address))
                    return base;
            }
            return SymGetModuleBase64(process, address);
        }
    }

    std::uint64_t StackWalker::ModuleBase(std::uint64_t address) const
    {
        for (const auto& table : tables_) {
            if (address >= table.base && address < table.end)
                return table.base;
        }
        return 0;
    }

    const RUNTIME_FUNCTION* StackWalker::LookupFunction(std::uint64_t address)
    {
        for (auto& table : tables_) {
            if (address < table.base || address >= table.end)
                continue;
            if (!table.loaded) {
                table.loaded = true;
                IMAGE_DOS_HEADER dos{};
                IMAGE_NT_HEADERS64 nt{};
                if (ReadMemory(process_, table.base, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
                    ReadMemory(process_, table.base + static_cast<std::uint64_t>(dos.e_lfanew), &nt, sizeof(nt)) &&
                    nt.Signature == IMAGE_NT_SIGNATURE &&
                    nt.OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXCEPTION) {
                    const IMAGE_DATA_DIRECTORY& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
                    const std::size_t count = dir.Size / sizeof(RUNTIME_FUNCTION);
                    if (dir.VirtualAddress && count && count < 4'000'000) {
                        table.entries.resize(count);
                        if (!ReadMemory(process_, table.base + dir.VirtualAddress, table.entries.data(),
                                        count * sizeof(RUNTIME_FUNCTION)))
                            table.entries.clear();
                    }
                }
            }
            const auto rva = static_cast<DWORD>(address - table.base);
            const auto it = std::upper_bound(table.entries.begin(), table.entries.end(), rva,
                                             [](DWORD value, const RUNTIME_FUNCTION& f) { return value < f.BeginAddress; });
            if (it == table.entries.begin())
                return nullptr;
            const RUNTIME_FUNCTION& entry = *(it - 1);
            if (rva >= entry.EndAddress)
                return nullptr;  // a leaf function or code without unwind data
            RUNTIME_FUNCTION& slot = scratch_[scratchNext_++ % std::size(scratch_)];
            slot = entry;
            return &slot;
        }
        return nullptr;
    }

    StackWalker::~StackWalker()
    {
        if (ready_)
            SymCleanup(process_);
    }

    bool StackWalker::Init(HANDLE process, const std::vector<Module>& modules, const std::wstring& searchPath)
    {
        process_ = process;
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_NO_PROMPTS | SYMOPT_FAIL_CRITICAL_ERRORS |
                      SYMOPT_NO_UNQUALIFIED_LOADS);
        // An explicit search path keeps DbgHelp away from _NT_SYMBOL_PATH and symbol servers.
        if (!SymInitializeW(process, searchPath.c_str(), FALSE)) {
            Log(L"SymInitialize failed (error %lu)", GetLastError());
            return false;
        }
        ready_ = true;
        tables_.clear();
        for (const auto& module : modules) {
            const std::wstring& file = module.realPath.empty() ? module.path : module.realPath;
            SymLoadModuleExW(process, nullptr, file.c_str(), nullptr, module.base, static_cast<DWORD>(module.size),
                             nullptr, 0);
            FunctionTable table;
            table.base = module.base;
            table.end = module.base + module.size;
            tables_.push_back(std::move(table));
        }
        return true;
    }

    bool StackWalker::Sample(HANDLE thread, dd::helper::Sample& out, Registers* registers,
                             std::vector<std::uint8_t>* stackTop, std::size_t stackBytes, int maxFrames)
    {
        out.frames.clear();
        if (!ready_)
            return false;

        SuspendGuard suspend(thread);
        if (!suspend.ok())
            return false;

        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (!GetThreadContext(thread, &context))
            return false;

        if (registers) {
            registers->rip = context.Rip;
            registers->rsp = context.Rsp;
            registers->rbp = context.Rbp;
            registers->rbx = context.Rbx;
            registers->rsi = context.Rsi;
            registers->rdi = context.Rdi;
            registers->r12 = context.R12;
            registers->r13 = context.R13;
            registers->r14 = context.R14;
            registers->r15 = context.R15;
            registers->rax = context.Rax;
            registers->rcx = context.Rcx;
            registers->rdx = context.Rdx;
            registers->r8 = context.R8;
            registers->r9 = context.R9;
            registers->r10 = context.R10;
            registers->r11 = context.R11;
        }
        if (stackTop) {
            stackTop->assign(stackBytes, 0);
            SIZE_T read = 0;
            ReadProcessMemory(process_, reinterpret_cast<LPCVOID>(context.Rsp), stackTop->data(), stackBytes, &read);
            stackTop->resize(read);
        }

        STACKFRAME_EX frame{};
        frame.StackFrameSize = sizeof(frame);
        frame.AddrPC.Offset = context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rsp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;

        g_active = this;
        std::uint64_t lastStack = 0;
        for (int i = 0, calls = 0; i < maxFrames && calls < maxFrames * 4; ++calls) {
            frame.InlineFrameContext = INLINE_FRAME_CONTEXT_IGNORE;
            if (!StackWalkEx(IMAGE_FILE_MACHINE_AMD64, process_, thread, &frame, &context, nullptr, &FunctionTableAccess,
                             &GetModuleBase, nullptr, SYM_STKWALK_DEFAULT))
                break;
            if (!frame.AddrPC.Offset)
                break;
            if (frame.InlineFrameContext != INLINE_FRAME_CONTEXT_IGNORE &&
                ((frame.InlineFrameContext >> 8) & 0xFF) == 0x02)  // STACK_FRAME_TYPE_INLINE
                continue;  // an inlined call inside the frame just walked
            ++i;
            if (i > 1 && frame.AddrStack.Offset <= lastStack)
                break;  // the walk stopped making progress
            lastStack = frame.AddrStack.Offset;
            out.frames.push_back(frame.AddrPC.Offset);
        }
        g_active = nullptr;
        if (out.frames.empty())
            out.frames.push_back(context.Rip);
        return true;
    }

    std::wstring StackWalker::Symbolize(std::uint64_t address, const std::vector<Module>& modules)
    {
        const Module* owner = nullptr;
        for (const auto& module : modules) {
            if (module.Contains(address)) {
                owner = &module;
                break;
            }
        }
        if (!owner)
            return Hex(address);

        const std::wstring plain = owner->name + L"+" + Hex(address - owner->base);
        if (!ready_ || owner->kind == ModuleKind::kGame)
            return plain;  // the game ships without symbols; nearest-export names would mislead

        alignas(SYMBOL_INFOW) std::uint8_t buffer[sizeof(SYMBOL_INFOW) + 512 * sizeof(wchar_t)]{};
        auto*                              symbol = reinterpret_cast<SYMBOL_INFOW*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
        symbol->MaxNameLen = 512;
        DWORD64 displacement = 0;
        if (!SymFromAddrW(process_, address, &displacement, symbol) || displacement > 0x4000)
            return plain;
        return owner->name + L"!" + std::wstring(symbol->Name, symbol->NameLen) + L"+" + Hex(displacement);
    }
}
