#pragma once

#include "helper/Snapshot.h"

#include <string>
#include <vector>

namespace dd::helper
{
    enum class Confidence
    {
        kNone,
        kLow,
        kMedium,
        kHigh,
    };

    const wchar_t* ConfidenceName(Confidence confidence);

    struct Suspect
    {
        int                       module = -1;
        Confidence                confidence = Confidence::kLow;
        std::vector<std::wstring> reasons;
    };

    enum class WaitKind
    {
        kNone,
        kSleep,
        kObject,
        kAlert,    // WaitOnAddress: SRW locks, condition variables, critical sections
        kKeyed,
        kMessage,
        kFileIo,
        kIoPort,
        kWorker,   // idle thread-pool worker
        kAlpc,
    };

    struct ThreadSummary
    {
        std::uint32_t              tid = 0;
        double                     cpuPercent = 0;
        double                     switchesPerSecond = 0;
        double                     waitingShare = 0;
        bool                       running = false;
        WaitKind                   wait = WaitKind::kNone;
        std::wstring               waitFunction;
        int                        attributed = -1;  // module of the first frame outside Windows and runtimes
        double                     attributedShare = 0;
        int                        attributedFrame = -1;  // its index in the representative stack
        int                        startModule = -1;
        std::vector<std::uint64_t> stack;  // the most common stack
        bool                       ownIdleWorker = false;  // a plugin's own thread waiting in that plugin
        bool                       polling = false;        // busy, yet always in a sleep: spin-waiting for something
        // The blameable module nearest the top of the stack, sample by sample:
        int                        nearestPlugin = -1;
        double                     nearestShare = 0;       // samples where it is the nearest blameable module
        double                     nearestSelfShare = 0;   // samples where its own code is running (nothing above it)
        double                     nearestByExtender = 0;  // samples where the script extender called it
        int                        nearestDepth = 0;       // median count of game frames above it
    };

    // A game record (and the plugins it comes from) that stuck code was working on.
    struct RecordLead
    {
        int           form = -1;  // index into Snapshot::forms
        std::uint32_t threadId = 0;
        double        share = 0;  // of that thread's samples
        Confidence    confidence = Confidence::kLow;
        std::wstring  why;
        std::uint32_t offset = 0;  // nearest stack offset it was seen at (0 = a register)
    };

    struct Verdict
    {
        std::wstring               kind;
        std::wstring               title;
        std::wstring               summary;
        Confidence                 confidence = Confidence::kLow;
        std::vector<Suspect>       suspects;  // most likely first
        std::vector<RecordLead>    records;   // plugin records involved, most likely first
        std::vector<std::wstring>  evidence;
        std::vector<std::wstring>  advice;
        std::vector<ThreadSummary> threads;
        std::vector<std::uint32_t> path;  // threads the main thread waits on, in order
    };

    Verdict      Analyze(const Snapshot& snapshot);
    WaitKind     WaitKindOf(const std::wstring& function);
    std::wstring FunctionName(const std::wstring& symbol);
    const wchar_t* WaitKindName(WaitKind kind);
}
