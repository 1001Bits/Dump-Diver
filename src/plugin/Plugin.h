#pragma once

#include "common/Protocol.h"
#include "plugin/XseAbi.h"

#include <windows.h>

namespace dd::plugin
{
    bool Load(HMODULE module, const xse::Interface* loader);

    Header* Shared();
    void    PushEvent(EventType type, std::uint64_t a = 0, std::uint64_t b = 0);

    inline std::uint64_t Qpc()
    {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        return static_cast<std::uint64_t>(counter.QuadPart);
    }
}
