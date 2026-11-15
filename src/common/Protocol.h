#pragma once

// Shared memory between the game plugin and DumpDiverHelper.exe.
// The plugin creates "Local\DumpDiver.<pid>"; the helper maps it read-only.
// Bump kProtocolVersion on any layout change.

#include <cstdint>

namespace dd
{
    inline constexpr std::uint32_t kMagic = 0x31564444;  // "DDV1"
    inline constexpr std::uint32_t kProtocolVersion = 4;
    inline constexpr wchar_t       kMappingPrefix[] = L"Local\\DumpDiver.";

    enum class Game : std::uint32_t
    {
        kUnknown = 0,
        kSkyrim = 1,
        kFallout4 = 2,
    };

    enum StateFlag : std::uint32_t
    {
        kState_DataLoaded = 1u << 0,
        kState_LoadingSave = 1u << 1,
        kState_Saving = 1u << 2,
    };

    enum class EventType : std::uint32_t
    {
        kNone = 0,
        kPluginLoaded,
        kHelperStarted,
        kPostLoad,
        kInputLoaded,
        kDataLoaded,
        kNewGame,
        kPreLoadGame,
        kPostLoadGame,  // a = 1 when the load succeeded
        kSaveGame,
        kPostSaveGame,
        kDeleteGame,
        kFirstHeartbeat,
        kLoadingScreenOpened,
        kLoadingScreenClosed,  // a = milliseconds it was up
    };

    struct Event
    {
        std::uint64_t qpc;
        std::uint32_t type;
        std::uint32_t threadId;
        std::uint64_t a;
        std::uint64_t b;
    };
    static_assert(sizeof(Event) == 32);

    // One slot per thread that has run a task queued through the script extender's task
    // interface. Only the owning thread writes it; sequence is odd while it does.
    struct TaskSlot
    {
        volatile std::uint32_t threadId;
        volatile std::uint32_t sequence;
        volatile std::uint64_t startQpc;       // 0 when the thread runs no tracked task
        volatile std::uint64_t runAddress;     // the task's Run function
        volatile std::uint64_t callerAddress;  // return address of the AddTask call that queued it
        volatile std::uint64_t queuedQpc;
        volatile std::uint64_t completed;
        volatile std::uint32_t kind;  // 0 = AddTask, 1 = AddUITask
        volatile std::uint32_t depth;
    };
    static_assert(sizeof(TaskSlot) == 56);

    // Settings the plugin read from its ini, handed to the helper so it never has to find the
    // ini itself (under Mod Organizer the file may only exist in the virtual Data folder).
    struct HelperSettings
    {
        std::uint32_t freezeSeconds;
        std::uint32_t loadingSeconds;
        std::uint32_t startupSeconds;
        std::uint32_t sampleMilliseconds;
        std::uint32_t sampleHz;
        std::uint32_t writeDump;
        std::uint32_t fullMemoryDump;
        std::uint32_t keepRecovered;
        std::uint32_t hotkeyModifiers;
        std::uint32_t hotkeyKey;
        std::uint32_t maxReports;
        std::uint32_t loadingScreenSeconds;   // a loading screen up longer than this is endless
        std::uint32_t loadingScreenAdaptive;  // 1: the limit grows to 3x the longest loading screen seen
        std::uint32_t freezePrompt;           // 1: after a freeze report, ask whether to quit and open it
        wchar_t       outputDir[520];
    };

    // An engine lock (BSReadWriteLock / BSSpinLock in the game executable's data) that a thread
    // still held at a moment it should have held none: when a plugin's message handler returned,
    // or when the thread exited. Nothing else releases such a lock, so the next thread that
    // wants it waits forever.
    enum class LockRecordKind : std::uint32_t
    {
        kNone = 0,
        kLeftByHandler = 1,  // a message handler returned holding it; code = the handler
        kHeldAtExit = 2,     // the thread exited holding it; code = the thread's start address
    };

    struct LockRecord
    {
        volatile std::uint64_t qpc;
        volatile std::uint64_t lockAddress;
        volatile std::uint64_t code;
        volatile std::uint32_t threadId;  // the thread holding the lock
        volatile std::uint32_t kind;
        volatile std::uint32_t lockWord;  // the lock's second half when it was seen
        volatile std::uint32_t messageType;
    };
    static_assert(sizeof(LockRecord) == 40);

    inline constexpr std::uint32_t kTaskSlots = 64;
    inline constexpr std::uint32_t kEventCount = 512;
    inline constexpr std::uint32_t kLockRecords = 32;

    struct Header
    {
        std::uint32_t magic;
        std::uint32_t version;
        std::uint32_t size;
        std::uint32_t game;
        std::uint32_t pluginVersion;
        std::uint32_t runtimeVersion;  // packed the way the script extender reports it
        std::uint32_t processId;
        std::uint32_t mainThreadId;  // the thread that loaded the plugin
        std::uint64_t qpcFrequency;
        std::uint64_t startQpc;

        volatile std::uint64_t heartbeatQpc;  // 0 until the first heartbeat task ran
        volatile std::uint64_t heartbeatCount;
        volatile std::uint32_t heartbeatThreadId;
        volatile std::uint32_t stateFlags;
        volatile std::uint64_t loadStartQpc;
        volatile std::uint32_t helperProcessId;
        volatile std::uint32_t taskTracking;  // 1 when other plugins' tasks are tracked
        volatile std::uint32_t taskSlotCount;
        volatile std::uint32_t captureRequests;  // the settings page's "Write a report now": each change asks for a manual report
        volatile std::uint64_t tasksQueued;
        volatile std::uint64_t eventIndex;  // total events written; the ring holds the last kEventCount

        // Loading screen, seen through the game's LoadingMenu: opened at its first AdvanceMovie,
        // closed when it is destroyed or stops advancing while the game still runs tasks.
        volatile std::uint64_t loadingOpenQpc;     // 0 while no loading screen is up
        volatile std::uint64_t loadingAdvanceQpc;  // last AdvanceMovie of the loading screen
        volatile std::uint32_t loadingWatch;       // 1 when the LoadingMenu hooks are installed
        volatile std::uint32_t loadingCount;
        volatile std::uint32_t loadingLastMs;
        volatile std::uint32_t loadingLongestMs;

        HelperSettings settings;
        TaskSlot       tasks[kTaskSlots];
        Event          events[kEventCount];

        volatile std::uint32_t lockWatch;        // bit 0: message handlers checked, bit 1: thread exits checked
        volatile std::uint32_t lockRecordCount;  // total written; the ring holds the first kLockRecords
        LockRecord             lockRecords[kLockRecords];
    };

    inline constexpr std::uint32_t PackVersion(std::uint32_t major, std::uint32_t minor, std::uint32_t build)
    {
        return (major << 24) | (minor << 16) | ((build & 0xFFF) << 4);
    }
}
