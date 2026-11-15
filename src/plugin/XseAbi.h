#pragma once

// The parts of the SKSE64 / F4SE plugin interface this plugin uses, declared by hand so the
// plugin needs no CommonLib and loads on every runtime of both games.

#include <cstdint>

namespace xse
{
    using PluginHandle = std::uint32_t;

    // Same prefix in SKSE64, SKSEVR, F4SE and F4SEVR.
    struct Interface
    {
        std::uint32_t xseVersion;
        std::uint32_t runtimeVersion;
        std::uint32_t editorVersion;
        std::uint32_t isEditor;
        void* (*QueryInterface)(std::uint32_t id);
        PluginHandle (*GetPluginHandle)();
    };

    struct Message
    {
        const char*   sender;
        std::uint32_t type;
        std::uint32_t dataLen;
        void*         data;
    };

    using EventCallback = void (*)(Message*);

    struct MessagingInterface
    {
        std::uint32_t interfaceVersion;
        bool (*RegisterListener)(PluginHandle listener, const char* sender, EventCallback handler);
    };

    struct TaskInterface
    {
        std::uint32_t interfaceVersion;
        void (*AddTask)(void* task);
        void (*AddUITask)(void* task);
    };

    struct PluginInfo
    {
        std::uint32_t infoVersion;
        const char*   name;
        std::uint32_t version;
    };

    // SKSE runs a task with Run() and then calls Dispose().
    class SkseTask
    {
    public:
        virtual void Run() = 0;
        virtual void Dispose() = 0;
    };

    // F4SE runs a task with Run() and then deletes it through the virtual destructor.
    class F4seTask
    {
    public:
        virtual ~F4seTask() = default;
        virtual void Run() = 0;
    };

    namespace skse
    {
        inline constexpr std::uint32_t kInterface_Task = 4;
        inline constexpr std::uint32_t kInterface_Messaging = 5;

        enum : std::uint32_t
        {
            kPostLoad = 0,
            kPostPostLoad,
            kPreLoadGame,
            kPostLoadGame,
            kSaveGame,
            kDeleteGame,
            kInputLoaded,
            kNewGame,
            kDataLoaded,
        };
    }

    namespace f4se
    {
        inline constexpr std::uint32_t kInterface_Messaging = 1;
        inline constexpr std::uint32_t kInterface_Task = 5;

        enum : std::uint32_t
        {
            kPostLoad = 0,
            kPostPostLoad,
            kPreLoadGame,
            kPostLoadGame,
            kPreSaveGame,
            kPostSaveGame,
            kDeleteGame,
            kInputLoaded,
            kNewGame,
            kGameLoaded,
            kGameDataReady,
        };
    }

#if defined(DD_GAME_SKYRIM)
    using Task = SkseTask;
    inline constexpr std::uint32_t kTaskRunSlot = 0;  // vtable index of Run
    inline constexpr std::uint32_t kInterface_Task = skse::kInterface_Task;
    inline constexpr std::uint32_t kInterface_Messaging = skse::kInterface_Messaging;
    inline constexpr const char*   kLoaderName = "SKSE";
#elif defined(DD_GAME_FALLOUT4)
    using Task = F4seTask;
    inline constexpr std::uint32_t kTaskRunSlot = 1;  // slot 0 is the deleting destructor
    inline constexpr std::uint32_t kInterface_Task = f4se::kInterface_Task;
    inline constexpr std::uint32_t kInterface_Messaging = f4se::kInterface_Messaging;
    inline constexpr const char*   kLoaderName = "F4SE";
#else
#    error "Define DD_GAME_SKYRIM or DD_GAME_FALLOUT4"
#endif
}
