#pragma once

// What the stand-in game hands the test DLLs.

#include <cstddef>
#include <cstdint>

struct TestTaskInterface
{
    std::uint32_t interfaceVersion;
    void (*AddTask)(void* task);
    void (*AddUITask)(void* task);
};

// Same layout as the engine's BSSpinLock.
struct EngineLock
{
    volatile long owner;
    volatile long count;
};

// Same layout as the script extender's messaging interface.
struct TestMessagingInterface
{
    std::uint32_t interfaceVersion;
    bool (*RegisterListener)(std::uint32_t plugin, const char* sender, void (*handler)(void* message));
};

struct HostApi
{
    TestTaskInterface* tasks;  // the script extender's task interface, as plugins see it
    void (*EngineLoopForever)();
    void (*EngineLockAcquire)(EngineLock* lock);
    EngineLock* engineLock;
    void (*EngineEnterCriticalSection)();
    void* (*EngineAllocate)(std::size_t size);  // a game helper that does most of the work per call
    TestMessagingInterface* messaging;          // as plugins see it (Dump Diver may have wrapped it)
};

// SKSE task ABI: Run, then Dispose.
class TestTask
{
public:
    virtual void Run() = 0;
    virtual void Dispose() = 0;
};
