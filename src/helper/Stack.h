#pragma once

#include "helper/Snapshot.h"

#include <windows.h>

#include <string>
#include <vector>

namespace dd::helper
{
    // Walks the stacks of another process's threads with DbgHelp. Not thread-safe (DbgHelp
    // is not); the helper samples from one thread.
    class StackWalker
    {
    public:
        StackWalker() = default;
        ~StackWalker();
        StackWalker(const StackWalker&) = delete;
        StackWalker& operator=(const StackWalker&) = delete;

        bool Init(HANDLE process, const std::vector<Module>& modules, const std::wstring& searchPath);

        // Suspends the thread, reads its context and walks its stack, then resumes it.
        // stackTop receives up to stackBytes bytes from RSP.
        bool Sample(HANDLE thread, Sample& out, Registers* registers, std::vector<std::uint8_t>* stackTop,
                    std::size_t stackBytes, int maxFrames);

        std::wstring Symbolize(std::uint64_t address, const std::vector<Module>& modules);

        // Unwind data read from the game's memory rather than from the files: the game
        // executables are encrypted on disk, and Mod Organizer's paths may not open from here.
        struct FunctionTable
        {
            std::uint64_t base = 0;
            std::uint64_t end = 0;
            bool          loaded = false;
            std::vector<RUNTIME_FUNCTION> entries;
        };
        const RUNTIME_FUNCTION* LookupFunction(std::uint64_t address);
        std::uint64_t           ModuleBase(std::uint64_t address) const;

    private:
        HANDLE                     process_ = nullptr;
        bool                       ready_ = false;
        std::vector<FunctionTable> tables_;
        RUNTIME_FUNCTION           scratch_[16]{};
        std::size_t                scratchNext_ = 0;
    };
}
