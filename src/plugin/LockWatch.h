#pragma once

#include "plugin/XseAbi.h"

namespace dd::plugin
{
    // Finds the game executable's data sections, where the engine locks that guard global tables
    // live, and starts checking threads that exit.
    bool InitLockWatch();

    // Routes other plugins' RegisterListener calls through a wrapper. Around every message the
    // script extender itself sends (load, data loaded, save...), the wrapper notes which engine
    // locks the thread holds; a handler that returns holding one it did not hold before is
    // recorded. Covers handlers registered after this call.
    bool InstallListenerTracking(xse::MessagingInterface* messaging);

    // Registers one of this plugin's own handlers without wrapping it.
    bool RegisterOwnListener(xse::MessagingInterface* messaging, xse::PluginHandle self, const char* sender,
                             xse::EventCallback handler);

    // From DllMain: records engine locks the exiting thread still holds.
    void OnThreadExit();

    // Writes records not logged yet to the plugin log. Never call it under the loader lock.
    void LogLockRecords();
}
