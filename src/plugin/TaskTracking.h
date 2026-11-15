#pragma once

#include "plugin/XseAbi.h"

namespace dd::plugin
{
    // Remembers the script extender's AddTask / AddUITask so this plugin can queue its own
    // tasks without tracking them.
    bool InitTasks(xse::TaskInterface* tasks);

    // Routes other plugins' AddTask / AddUITask calls through a wrapper that records, per
    // thread, which task is running and who queued it. The script extender keeps one
    // interface table that every plugin calls through, so patching its two function pointers
    // covers plugins that loaded before this one too.
    bool InstallTaskTracking(xse::TaskInterface* tasks);

    // Queues one of this plugin's own tasks without tracking it.
    bool QueueUntracked(xse::Task* task);
}
