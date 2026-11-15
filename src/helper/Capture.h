#pragma once

#include "common/Protocol.h"
#include "helper/Snapshot.h"

#include <windows.h>

#include <filesystem>
#include <string>

namespace dd::helper
{
    struct Target
    {
        HANDLE        process = nullptr;
        std::uint32_t pid = 0;
        const Header* shared = nullptr;
        Game          game = Game::kUnknown;
        std::wstring  exePath;
        HWND          window = nullptr;
    };

    // Samples the game, analyses it and writes the report (and dump). Returns the report path.
    std::filesystem::path Capture(Target& target, CaptureReason reason, std::uint64_t heartbeatCountAtStart);

    // Code shape of a GPU query poll: a call through vtable slot 0xE8
    // (ID3D11DeviceContext::GetData) shortly before the return address.
    bool LooksLikeGpuPoll(const std::uint8_t* code, std::size_t size);

    // Reads {owner thread, count} as a BSSpinLock, or a write-held BSReadWriteLock.
    bool DecodeLock(std::uint64_t raw, std::uint32_t& owner, std::uint32_t& count, bool& readWrite);
}
