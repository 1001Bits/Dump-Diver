// Turns a capture into a verdict.
//
// A module is only suspected for one of three reasons:
//   1. its code is what a thread keeps running while the game is stuck,
//   2. it holds what the stuck thread waits for (a lock, a task it queued, a dialog it opened),
//   3. it is the code that waits, on the main thread or on a thread the main thread waits for.
// Being somewhere lower on a stuck thread's stack is never enough: hooks on the main loop sit
// below every wait the game makes.

#include "helper/Analyze.h"

#include "helper/Process.h"
#include "helper/Util.h"

#include <algorithm>
#include <map>
#include <set>

namespace dd::helper
{
    const wchar_t* ConfidenceName(Confidence confidence)
    {
        switch (confidence) {
        case Confidence::kHigh:
            return L"High";
        case Confidence::kMedium:
            return L"Medium";
        case Confidence::kLow:
            return L"Low";
        default:
            return L"None";
        }
    }

    const wchar_t* WaitKindName(WaitKind kind)
    {
        switch (kind) {
        case WaitKind::kSleep:
            return L"sleep";
        case WaitKind::kObject:
            return L"wait for object";
        case WaitKind::kAlert:
            return L"lock or condition variable";
        case WaitKind::kKeyed:
            return L"keyed event";
        case WaitKind::kMessage:
            return L"window messages";
        case WaitKind::kFileIo:
            return L"file I/O";
        case WaitKind::kIoPort:
            return L"I/O completion port";
        case WaitKind::kWorker:
            return L"idle thread pool";
        case WaitKind::kAlpc:
            return L"inter-process call";
        default:
            return L"running";
        }
    }

    std::wstring FunctionName(const std::wstring& symbol)
    {
        const std::size_t bang = symbol.find(L'!');
        if (bang == std::wstring::npos)
            return {};
        const std::size_t plus = symbol.find(L'+', bang);
        return symbol.substr(bang + 1, plus == std::wstring::npos ? std::wstring::npos : plus - bang - 1);
    }

    WaitKind WaitKindOf(const std::wstring& function)
    {
        if (function.size() < 3)
            return WaitKind::kNone;
        std::wstring f = function;
        if (StartsWithI(f, L"Zw"))
            f = L"Nt" + f.substr(2);
        static const std::map<std::wstring, WaitKind> kinds = {
            { L"NtDelayExecution", WaitKind::kSleep },
            { L"NtWaitForSingleObject", WaitKind::kObject },
            { L"NtWaitForMultipleObjects", WaitKind::kObject },
            { L"NtSignalAndWaitForSingleObject", WaitKind::kObject },
            { L"NtWaitForAlertByThreadId", WaitKind::kAlert },
            { L"NtWaitForKeyedEvent", WaitKind::kKeyed },
            { L"NtUserMsgWaitForMultipleObjectsEx", WaitKind::kMessage },
            { L"NtUserGetMessage", WaitKind::kMessage },
            { L"NtUserWaitMessage", WaitKind::kMessage },
            { L"NtRemoveIoCompletion", WaitKind::kIoPort },
            { L"NtRemoveIoCompletionEx", WaitKind::kIoPort },
            { L"NtWaitForWorkViaWorkerFactory", WaitKind::kWorker },
            { L"NtAlpcSendWaitReceivePort", WaitKind::kAlpc },
            { L"NtReplyWaitReceivePort", WaitKind::kAlpc },
            { L"NtReplyWaitReceivePortEx", WaitKind::kAlpc },
            { L"NtRequestWaitReplyPort", WaitKind::kAlpc },
            { L"NtReadFile", WaitKind::kFileIo },
            { L"NtWriteFile", WaitKind::kFileIo },
            { L"NtCreateFile", WaitKind::kFileIo },
            { L"NtOpenFile", WaitKind::kFileIo },
            { L"NtQueryAttributesFile", WaitKind::kFileIo },
            { L"NtQueryFullAttributesFile", WaitKind::kFileIo },
            { L"NtQueryDirectoryFile", WaitKind::kFileIo },
            { L"NtQueryDirectoryFileEx", WaitKind::kFileIo },
            { L"NtFlushBuffersFile", WaitKind::kFileIo },
            { L"NtDeviceIoControlFile", WaitKind::kFileIo },
            { L"NtFsControlFile", WaitKind::kFileIo },
        };
        const auto it = kinds.find(f);
        return it == kinds.end() ? WaitKind::kNone : it->second;
    }

    namespace
    {
        class Analyzer
        {
        public:
            explicit Analyzer(const Snapshot& snapshot) :
                s_(snapshot)
            {}

            Verdict Run();

        private:
            const Snapshot& s_;
            Verdict         v_;
            std::map<std::uint32_t, std::size_t> index_;  // tid -> v_.threads index

            // --- helpers ---------------------------------------------------------------

            std::wstring Sym(std::uint64_t address) const
            {
                const auto it = s_.symbols.find(address);
                if (it != s_.symbols.end())
                    return it->second;
                const int m = s_.ModuleAt(address);
                return m >= 0 ? s_.modules[m].name + L"+" + Hex(address - s_.modules[m].base) : Hex(address);
            }

            std::wstring Func(std::uint64_t address) const { return FunctionName(Sym(address)); }

            ModuleKind KindAt(int module) const
            {
                if (module < 0)
                    return ModuleKind::kOther;
                const Module& m = s_.modules[static_cast<std::size_t>(module)];
                // captures made before the test harness had a kind of its own
                return m.kind == ModuleKind::kPlugin && IsTestHarness(m.name) ? ModuleKind::kTestHarness : m.kind;
            }

            bool Blameable(int module) const { return module >= 0 && IsBlameable(KindAt(module)); }

            std::wstring ModuleLabel(int module) const
            {
                if (module < 0)
                    return L"unknown code";
                const Module& m = s_.modules[static_cast<std::size_t>(module)];
                if (!m.modName.empty() && !EqualsI(m.modName, L"Overwrite"))
                    return m.name + L" (" + m.modName + L")";
                return m.name;
            }

            // Index of the first frame that is not Windows, a runtime, the script extender, the
            // mod manager or Dump Diver itself; -1 if none.
            int FirstMeaningful(const Sample& sample) const
            {
                for (std::size_t i = 0; i < sample.frames.size(); ++i) {
                    const int m = s_.ModuleAt(sample.frames[i]);
                    if (m < 0)
                        continue;
                    if (!IsInfrastructure(KindAt(m)))
                        return static_cast<int>(i);
                }
                return -1;
            }

            const ThreadSummary* Summary(std::uint32_t tid) const
            {
                const auto it = index_.find(tid);
                return it == index_.end() ? nullptr : &v_.threads[it->second];
            }

            // The stuck task usually starts right after the last heartbeat (it is what stopped it), so
            // allow a little time after the heartbeat.
            bool StartedWithFreeze(const TaskInFlight& task) const
            {
                if (!s_.heartbeatQpc)
                    return true;
                return task.startQpc <= s_.heartbeatQpc + 2 * s_.qpcFrequency;
            }

            const TaskInFlight* TaskOn(std::uint32_t tid) const
            {
                for (const auto& task : s_.tasks) {
                    if (task.threadId == tid && task.startQpc)
                        return &task;
                }
                return nullptr;
            }

            std::wstring ThreadLabel(std::uint32_t tid) const
            {
                if (tid == s_.mainThreadId)
                    return L"the main thread";
                std::wstring label = L"thread " + std::to_wstring(tid);
                if (const ThreadData* t = s_.Thread(tid); t && !t->name.empty())
                    label += L" \"" + t->name + L"\"";
                if (const ThreadSummary* ts = Summary(tid); ts && ts->startModule >= 0)
                    label += L" (started by " + s_.modules[static_cast<std::size_t>(ts->startModule)].name + L")";
                return label;
            }

            void AddSuspect(int module, Confidence confidence, const std::wstring& reason)
            {
                if (!Blameable(module))
                    return;
                for (auto& suspect : v_.suspects) {
                    if (suspect.module == module) {
                        suspect.confidence = std::max(suspect.confidence, confidence);
                        if (std::find(suspect.reasons.begin(), suspect.reasons.end(), reason) == suspect.reasons.end())
                            suspect.reasons.push_back(reason);
                        return;
                    }
                }
                v_.suspects.push_back({ module, confidence, { reason } });
            }

            void Evidence(const std::wstring& text) { v_.evidence.push_back(text); }

            // " (sleep, NtDelayExecution)", " (NtFoo)" or nothing: never an empty pair of brackets.
            static std::wstring WaitDetail(const ThreadSummary& t)
            {
                if (t.wait != WaitKind::kNone && !t.waitFunction.empty())
                    return L" (" + std::wstring(WaitKindName(t.wait)) + L", " + t.waitFunction + L")";
                return t.waitFunction.empty() ? std::wstring() : L" (" + t.waitFunction + L")";
            }
            static std::wstring InBrackets(const std::wstring& text) { return text.empty() ? text : L" (" + text + L")"; }
            static const wchar_t* WaitVerb(const ThreadSummary& t)
            {
                if (t.polling)
                    return L"spin-waiting";
                return t.wait == WaitKind::kNone ? L"idle, but not in a wait Dump Diver knows" : L"waiting";
            }

            ThreadSummary Summarize(const ThreadData& t) const;
            void          SuspectTask(const TaskInFlight& task, Confidence confidence, const std::wstring& what);
            void          ExplainThread(std::uint32_t tid, bool strongLink, int depth, std::set<std::uint32_t>& seen);
            bool          CheckDialog(const ThreadSummary& main);
            void          MainRunning(const ThreadSummary& main);
            void          MainWaiting(const ThreadSummary& main);
            bool          IsGpuWait(const ThreadSummary& t) const;
            const WaitChain* ChainOf(std::uint32_t tid) const;
            std::vector<int> CallPathModules(const ThreadData& t, int belowFrame) const;

            // DLLs below a loop in game code are named as context only: hooks on the main loop
            // sit below everything the game does.
            bool BlameLoopAroundEngine(const ThreadSummary& ts, const std::wstring& label, bool strongLink);
            bool BlameBrokenRecords(const ThreadData& t, const std::wstring& label);
            bool HoldsBrokenRecord(const ThreadData& t) const;
            bool LoopsOnRecord(const ThreadData& t) const;
            int  WaitsOnOwnLock(const ThreadSummary& ts) const;
            bool BlameOwnLock(const ThreadSummary& ts, const std::wstring& label);
            void SafetyNet();

            // Engine locks nothing will ever release: the owner thread exited, or the game plugin saw a
            // message handler return still holding the lock (LockRecord in Protocol.h).
            const LockRecordInfo* RecordFor(const LockHit& lock, LockRecordKind kind) const;
            bool                  LeftHeld(const LockHit& lock) const;
            void                  ExplainLeftHeldLock(const LockHit& lock, Confidence handlerConfidence);
            void                  ExplainLockOwner(const LockHit& lock, bool strongLink, int depth, std::set<std::uint32_t>& seen);
            bool                  LockFromRecords(const ThreadSummary& main);
            std::wstring          MessageName(std::uint32_t type) const;
            int                   leftBy_ = -1;  // the DLL whose handler left the lock held
            std::set<std::uint64_t> explainedLocks_;

            void CallPathEvidence(const ThreadData& t, int belowFrame, const std::wstring& label)
            {
                std::wstring names;
                for (int m : CallPathModules(t, belowFrame))
                    names += (names.empty() ? L"" : L", ") + ModuleLabel(m);
                if (!names.empty())
                    Evidence(Format(L"below the loop on %s: %s (on the call path only, not a cause by itself)", label.c_str(), names.c_str()));
            }
            void          Context();
            void          Finish();
            void          EndlessLoading(const ThreadSummary& main);
            void          AddRecords(std::uint32_t tid, std::size_t max, const std::wstring& why, bool mayBeMedium);
            void          CollectRecords();
            std::vector<std::uint32_t> loadingThreads_;
        };

        ThreadSummary Analyzer::Summarize(const ThreadData& t) const
        {
            ThreadSummary ts;
            ts.tid = t.tid;
            const double seconds = s_.sampleSeconds > 0 ? s_.sampleSeconds : 1;
            ts.cpuPercent = static_cast<double>(t.cpuTime100ns) / (seconds * 1e7) * 100.0;
            ts.switchesPerSecond = static_cast<double>(t.contextSwitches) / seconds;
            ts.startModule = s_.ModuleAt(t.startAddress);

            std::map<std::wstring, int> waitFunctions;
            std::map<int, int>          attributed;
            std::map<int, int>          nearest, nearestSelf, nearestByExtender;
            std::map<int, std::vector<int>> nearestDepths;
            std::map<std::vector<std::uint64_t>, int> stacks;
            int                         waiting = 0;
            for (const auto& sample : t.samples) {
                if (sample.frames.empty())
                    continue;
                const std::wstring top = Func(sample.frames[0]);
                if (WaitKindOf(top) != WaitKind::kNone) {
                    ++waiting;
                    ++waitFunctions[top];
                }
                const int first = FirstMeaningful(sample);
                ++attributed[first >= 0 ? s_.ModuleAt(sample.frames[static_cast<std::size_t>(first)]) : -1];
                std::vector<std::uint64_t> key(sample.frames.begin(),
                                               sample.frames.begin() + std::min<std::size_t>(sample.frames.size(), 12));
                ++stacks[key];

                // The nearest blameable module: how deep it sits below game code, and who called it.
                for (std::size_t i = 0; i < sample.frames.size(); ++i) {
                    const int m = s_.ModuleAt(sample.frames[i]);
                    if (!Blameable(m))
                        continue;
                    ++nearest[m];
                    int depth = 0;
                    for (std::size_t j = 0; j < i; ++j) {
                        const int above = s_.ModuleAt(sample.frames[j]);
                        if (above >= 0 && !IsInfrastructure(KindAt(above)))
                            ++depth;
                    }
                    nearestDepths[m].push_back(depth);
                    if (depth == 0)
                        ++nearestSelf[m];
                    std::size_t k = i;
                    while (k < sample.frames.size() && s_.ModuleAt(sample.frames[k]) == m)
                        ++k;
                    if (k < sample.frames.size() && KindAt(s_.ModuleAt(sample.frames[k])) == ModuleKind::kScriptExtender)
                        ++nearestByExtender[m];
                    break;
                }
            }
            const double count = static_cast<double>(std::max<std::size_t>(t.samples.size(), 1));
            ts.waitingShare = waiting / count;

            if (!waitFunctions.empty()) {
                const auto best = std::max_element(waitFunctions.begin(), waitFunctions.end(),
                                                   [](const auto& a, const auto& b) { return a.second < b.second; });
                ts.waitFunction = best->first;
                ts.wait = WaitKindOf(best->first);
            }
            if (!attributed.empty()) {
                const auto best = std::max_element(attributed.begin(), attributed.end(),
                                                   [](const auto& a, const auto& b) { return a.second < b.second; });
                ts.attributed = best->first;
                ts.attributedShare = best->second / count;
            }
            // The most common stack, from the samples whose first 12 frames agree most often.
            if (!stacks.empty()) {
                const auto best = std::max_element(stacks.begin(), stacks.end(),
                                                   [](const auto& a, const auto& b) { return a.second < b.second; });
                for (const auto& sample : t.samples) {
                    if (sample.frames.size() >= best->first.size() &&
                        std::equal(best->first.begin(), best->first.end(), sample.frames.begin())) {
                        ts.stack = sample.frames;
                        break;
                    }
                }
                Sample representative{ ts.stack };
                ts.attributedFrame = FirstMeaningful(representative);
            }

            if (!nearest.empty()) {
                const auto best = std::max_element(nearest.begin(), nearest.end(),
                                                   [](const auto& a, const auto& b) { return a.second < b.second; });
                ts.nearestPlugin = best->first;
                ts.nearestShare = best->second / count;
                ts.nearestSelfShare = nearestSelf[best->first] / count;
                ts.nearestByExtender = nearestByExtender[best->first] / count;
                auto depths = nearestDepths[best->first];
                std::nth_element(depths.begin(), depths.begin() + static_cast<std::ptrdiff_t>(depths.size() / 2), depths.end());
                ts.nearestDepth = depths[depths.size() / 2];
            }

            // Busy when it used a real share of a core, or ran in most samples. A thread that burns CPU
            // but is in a sleep in nearly every sample is spin-waiting for something else.
            const bool haveCpu = t.cpuTime100ns > 0 || t.contextSwitches > 0;
            // File reads are the exception: a thread that burns a core while always "in" a read is issuing
            // reads in a loop that each return at once (SKSE reads its cosave that way), not waiting for the disk.
            ts.polling = haveCpu && ts.cpuPercent >= 30.0 && ts.wait != WaitKind::kFileIo &&
                         (ts.waitingShare >= 0.8 ||
                          (ts.wait == WaitKind::kSleep && ts.waitingShare >= 0.4 && ts.switchesPerSecond >= 200));
            ts.running = !ts.polling && (haveCpu ? (ts.cpuPercent >= 30.0 || (ts.cpuPercent >= 10.0 && ts.waitingShare < 0.5))
                                                 : ts.waitingShare < 0.5);
            if (ts.running)
                ts.wait = WaitKind::kNone;

            ts.ownIdleWorker = !ts.running && ts.startModule >= 0 && ts.startModule == ts.attributed &&
                               IsBlameable(KindAt(ts.startModule));
            return ts;
        }

        const WaitChain* Analyzer::ChainOf(std::uint32_t tid) const
        {
            for (const auto& chain : s_.chains) {
                if (chain.threadId == tid)
                    return &chain;
            }
            return nullptr;
        }

        // Blameable modules found in at least 90% of the thread's samples, below the given frame
        // of the representative stack. Used only as low-confidence context.
        std::vector<int> Analyzer::CallPathModules(const ThreadData& t, int belowFrame) const
        {
            std::map<int, int> counts;
            for (const auto& sample : t.samples) {
                std::set<int> seen;
                for (std::size_t i = static_cast<std::size_t>(std::max(belowFrame, 0)) + 1; i < sample.frames.size(); ++i) {
                    const int m = s_.ModuleAt(sample.frames[i]);
                    if (Blameable(m))
                        seen.insert(m);
                }
                for (int m : seen)
                    ++counts[m];
            }
            std::vector<int> out;
            const double     needed = 0.9 * static_cast<double>(t.samples.size());
            for (const auto& [module, n] : counts) {
                if (n >= needed && n > 0)
                    out.push_back(module);
            }
            return out;
        }

        void Analyzer::SuspectTask(const TaskInFlight& task, Confidence confidence, const std::wstring& what)
        {
            int owner = s_.ModuleAt(task.runAddress);
            if (!Blameable(owner))
                owner = s_.ModuleAt(task.callerAddress);
            const double seconds = s_.Seconds(task.startQpc, s_.captureQpc);
            const std::wstring text = Format(L"%s is running a task queued by %s; it started %.1f s before the capture and has not finished",
                                             what.c_str(), ModuleLabel(owner).c_str(), seconds);
            Evidence(text);
            AddSuspect(owner, confidence, text);
        }

        bool Analyzer::IsGpuWait(const ThreadSummary& t) const
        {
            if (t.attributed >= 0) {
                const ModuleKind kind = KindAt(t.attributed);
                if (kind == ModuleKind::kGraphics || kind == ModuleKind::kGraphicsInjector)
                    return true;
            }
            if (t.attributedFrame >= 0 && static_cast<std::size_t>(t.attributedFrame) < t.stack.size()) {
                const auto it = s_.gpuPollSites.find(t.stack[static_cast<std::size_t>(t.attributedFrame)]);
                if (it != s_.gpuPollSites.end() && it->second)
                    return true;
            }
            return false;
        }

        // Describes what a thread is doing and suspects whatever is responsible for it.
        // strongLink: the main thread provably waits for this thread (Windows wait chain or a
        // lock it owns), so the conclusion can be firmer.
        void Analyzer::ExplainThread(std::uint32_t tid, bool strongLink, int depth, std::set<std::uint32_t>& seen)
        {
            if (depth > 4 || !seen.insert(tid).second)
                return;
            const ThreadSummary* ts = Summary(tid);
            const ThreadData*    t = s_.Thread(tid);
            if (!ts || !t) {
                Evidence(Format(L"%s was not sampled", ThreadLabel(tid).c_str()));
                return;
            }
            v_.path.push_back(tid);
            const std::wstring label = ThreadLabel(tid);

            if (ts->running) {
                Evidence(Format(L"%s is busy (%.0f%% CPU); %.0f%% of its samples are in %s", label.c_str(), ts->cpuPercent,
                                ts->attributedShare * 100, ModuleLabel(ts->attributed).c_str()));
                if (Blameable(ts->attributed) && ts->attributedShare >= 0.5) {
                    // Two independent signals: the thread runs this DLL's code, and runs an unfinished task
                    // this same DLL queued before the game stopped.
                    const TaskInFlight* task = TaskOn(tid);
                    const bool ownTask = task && (s_.ModuleAt(task->runAddress) == ts->attributed ||
                                                  s_.ModuleAt(task->callerAddress) == ts->attributed) &&
                                         StartedWithFreeze(*task);
                    const Confidence c = (strongLink || ownTask) && ts->attributedShare >= 0.8 ? Confidence::kHigh : Confidence::kMedium;
                    AddSuspect(ts->attributed, c,
                               Format(L"%s keeps running this DLL's code (%.0f%% of samples) while the game is stuck",
                                      label.c_str(), ts->attributedShare * 100));
                    if (task)
                        SuspectTask(*task, ownTask ? c : Confidence::kLow, label);
                } else if (BlameLoopAroundEngine(*ts, label, strongLink)) {
                    if (const TaskInFlight* task = TaskOn(tid))
                        SuspectTask(*task, Confidence::kLow, label);
                } else if (const TaskInFlight* task = TaskOn(tid)) {
                    SuspectTask(*task, strongLink ? Confidence::kMedium : Confidence::kLow, label);
                } else if (KindAt(ts->attributed) == ModuleKind::kGame) {
                    BlameBrokenRecords(*t, label);
                    CallPathEvidence(*t, ts->attributedFrame, label);
                }
                return;
            }

            Evidence(Format(L"%s is %s%s, in %s", label.c_str(), WaitVerb(*ts), WaitDetail(*ts).c_str(),
                            ModuleLabel(ts->attributed).c_str()));

            if (const TaskInFlight* task = TaskOn(tid))
                SuspectTask(*task, strongLink ? Confidence::kMedium : Confidence::kLow, label);

            // Follow what this thread waits for.
            bool followed = false;
            if (const WaitChain* chain = ChainOf(tid); chain && chain->nodes.size() > 2) {
                for (std::size_t i = 1; i < chain->nodes.size(); ++i) {
                    const WctNode& node = chain->nodes[i];
                    if (node.type == WctObject::kThread && node.processId == s_.processId && node.threadId != tid) {
                        Evidence(Format(L"%s waits for %s (Windows wait chain)", label.c_str(), ThreadLabel(node.threadId).c_str()));
                        ExplainThread(node.threadId, true, depth + 1, seen);
                        followed = true;
                        break;
                    }
                }
            }
            for (const auto& lock : s_.locks) {
                if (lock.waiterTid != tid || followed)
                    continue;
                Evidence(Format(L"%s retries a %s lock at %s that %s holds", label.c_str(),
                                lock.readWrite ? L"read/write" : L"spin", Sym(lock.lockAddress).c_str(),
                                ThreadLabel(lock.ownerTid).c_str()));
                ExplainLockOwner(lock, lock.inGameData || strongLink, depth + 1, seen);
                followed = true;
            }

            if (!followed && Blameable(ts->attributed) && !ts->ownIdleWorker) {
                AddSuspect(ts->attributed, strongLink ? Confidence::kMedium : Confidence::kLow,
                           Format(L"%s is waiting inside this DLL's code%s", label.c_str(), InBrackets(ts->waitFunction).c_str()));
            }
        }

        // A busy thread whose samples land mostly in game code can still be looping in a DLL: one that
        // calls game functions (allocation, lookups) on every pass. Two shapes say so, and neither
        // fits a hook that merely sits below an engine loop:
        //  - the thread has been inside the DLL's script-extender callback (an event or a task it
        //    handles) in nearly every sample;
        //  - the DLL sits right below a few game frames in nearly every sample and its own code is on
        //    top in some: the thread keeps returning to it. An engine loop never returns to a hook.
        bool Analyzer::BlameLoopAroundEngine(const ThreadSummary& ts, const std::wstring& label, bool strongLink)
        {
            if (!Blameable(ts.nearestPlugin) || ts.nearestShare < 0.95)
                return false;
            if (ts.nearestByExtender >= 0.9) {
                // Callbacks return within milliseconds; one that has not returned for the whole sample window
                // while burning a core is the stuck work, whether its own code or the game functions it calls.
                const Confidence c = ts.nearestSelfShare > 0 || strongLink || ts.cpuPercent >= 50.0 ? Confidence::kHigh
                                                                                                    : Confidence::kMedium;
                AddSuspect(ts.nearestPlugin, c,
                           Format(L"%s has been inside this DLL's script extender callback (an event or task it handles) "
                                  L"in %.0f%% of samples, %.0f%% in its own code and the rest in game functions it calls",
                                  label.c_str(), ts.nearestShare * 100, ts.nearestSelfShare * 100));
                return true;
            }
            if (ts.nearestDepth <= 6 && ts.nearestSelfShare > 0) {
                const Confidence c = ts.nearestSelfShare >= 0.05 || strongLink ? Confidence::kHigh : Confidence::kMedium;
                AddSuspect(ts.nearestPlugin, c,
                           Format(L"%s keeps coming back to this DLL's code: %.0f%% of samples in its own code, and in "
                                  L"%.0f%% it is right below the game functions it calls (%d frames)",
                                  label.c_str(), ts.nearestSelfShare * 100, ts.nearestShare * 100, ts.nearestDepth));
                return true;
            }
            return false;
        }

        // When no rule names a mod, the mods busy threads kept running are still worth a look: a weak
        // lead instead of silence. Left out: a hook that only sits below the main thread's loop (its own
        // code never on top), and the main thread during endless loading, where it only draws frames.
        // Records the thread holds in nearly every sample that the capture found broken (a face NPC loop and
        // the like). No plugin file can hold such data, but DLLs that change records while the game runs can.
        bool Analyzer::HoldsBrokenRecord(const ThreadData& t) const
        {
            const double count = static_cast<double>(std::max<std::size_t>(t.samples.size(), 1));
            for (const auto& ref : t.forms) {
                if (ref.form < 0 || ref.samples / count < 0.5)
                    continue;
                const FormInfo& form = s_.forms[static_cast<std::size_t>(ref.form)];
                if (!form.problem.empty())
                    return true;
                if (form.baseForm >= 0 && !s_.forms[static_cast<std::size_t>(form.baseForm)].problem.empty())
                    return true;
            }
            return false;
        }

        bool Analyzer::BlameBrokenRecords(const ThreadData& t, const std::wstring& label)
        {
            const double count = static_cast<double>(std::max<std::size_t>(t.samples.size(), 1));
            bool         found = false;
            bool         faceOnly = true;  // every broken record is a face NPC loop
            for (const auto& ref : t.forms) {
                if (ref.form < 0 || ref.samples / count < 0.5)
                    continue;
                const FormInfo& form = s_.forms[static_cast<std::size_t>(ref.form)];
                const FormInfo* broken = !form.problem.empty() ? &form : nullptr;
                if (!broken && form.baseForm >= 0 && !s_.forms[static_cast<std::size_t>(form.baseForm)].problem.empty())
                    broken = &s_.forms[static_cast<std::size_t>(form.baseForm)];
                if (!broken)
                    continue;
                found = true;
                faceOnly &= broken->className == L"TESNPC" && broken->problem.find(L"face NPC") != std::wstring::npos;
                Evidence(Format(L"%s works on %s %08X, and %s", label.c_str(), broken->className.c_str(), broken->formId,
                                broken->problem.c_str()));
            }
            if (!found)
                return false;
            std::vector<int> patchers;
            for (std::size_t i = 0; i < s_.modules.size(); ++i) {
                if (IsRuntimeRecordPatcher(s_.modules[i].name))
                    patchers.push_back(static_cast<int>(i));
            }
            if (faceOnly) {
                // only a patcher that sets face templates can make that loop; keep the rest out unless none can
                std::vector<int> face;
                for (const int m : patchers) {
                    if (CanChangeNpcFaces(s_.modules[static_cast<std::size_t>(m)].name))
                        face.push_back(m);
                }
                if (!face.empty())
                    patchers = std::move(face);
            }
            for (const int m : patchers) {
                AddSuspect(m, patchers.size() == 1 ? Confidence::kMedium : Confidence::kLow,
                           L"a record the stuck loop works on is broken in a way no plugin file can store, and this DLL "
                           L"changes records while the game runs");
            }
            if (patchers.empty())
                v_.advice.push_back(L"A record the loop works on is broken; check the plugins (and any runtime patcher configs) that change it.");
            return true;
        }

        // An engine loop that holds the same game record in a register in nearly every sample is working
        // through that record's data: the data is the likelier cause than whoever started the work.
        bool Analyzer::LoopsOnRecord(const ThreadData& t) const
        {
            const double count = static_cast<double>(std::max<std::size_t>(t.samples.size(), 1));
            for (const auto& ref : t.forms) {
                if (ref.form < 0 || ref.offset != 0 || ref.samples / count < 0.9)
                    continue;
                if (s_.forms[static_cast<std::size_t>(ref.form)].formId != 0x14)  // the player is everywhere
                    return true;
            }
            return false;
        }

        // A thread that waits to take a lock (SRW lock, critical section, std::mutex) inside DLL X after it
        // entered X a second time further down the same stack: if X's outer call took that lock, the thread
        // waits for itself and never wakes. Windows wait chains cannot see SRW locks, so this is the only
        // trace such a self-deadlock leaves. Returns X, or -1.
        int Analyzer::WaitsOnOwnLock(const ThreadSummary& ts) const
        {
            if (ts.running || ts.stack.empty())
                return -1;
            static const wchar_t* const kAcquire[] = { L"AcquireSRWLock", L"RtlpWaitOnCriticalSection", L"EnterCriticalSection",
                                                       L"_Mtx_lock", L"_Smtx_lock", L"RtlpWaitOnAddress" };
            std::size_t first = ts.stack.size();
            bool        acquiring = false;
            for (std::size_t i = 0; i < ts.stack.size() && i < 8; ++i) {
                const std::wstring fn = Func(ts.stack[i]);
                for (const wchar_t* name : kAcquire)
                    acquiring |= fn.find(name) != std::wstring::npos;
                if (Blameable(s_.ModuleAt(ts.stack[i]))) {
                    first = i;
                    break;
                }
            }
            if (!acquiring || first >= ts.stack.size())
                return -1;
            const int  owner = s_.ModuleAt(ts.stack[first]);
            bool       left = false;
            for (std::size_t j = first + 1; j < ts.stack.size(); ++j) {
                const int m = s_.ModuleAt(ts.stack[j]);
                if (m != owner)
                    left = true;
                else if (left)
                    return owner;  // back in the same DLL below another module: it was entered twice
            }
            // No other thread is anywhere in the DLL's code: nobody else can be holding a lock it takes,
            // so this thread already holds it, or it was taken and never released.
            for (const auto& other : s_.threads) {
                if (other.tid == ts.tid)
                    continue;
                for (const auto& sample : other.samples) {
                    for (const std::uint64_t frame : sample.frames) {
                        if (s_.ModuleAt(frame) == owner)
                            return -1;
                    }
                }
            }
            return owner;
        }

        bool Analyzer::BlameOwnLock(const ThreadSummary& ts, const std::wstring& label)
        {
            const int owner = WaitsOnOwnLock(ts);
            if (owner < 0)
                return false;
            // Another thread waiting in the same DLL could be the real holder; then the case is weaker.
            bool others = false;
            for (const auto& other : v_.threads) {
                if (other.tid != ts.tid && !other.ownIdleWorker && other.attributed == owner)
                    others = true;
            }
            AddSuspect(owner, others ? Confidence::kMedium : Confidence::kHigh,
                       Format(L"%s waits for a lock this DLL takes (%s), and no other thread is in the DLL's code to "
                              L"hold it: the lock is held by this thread's own outer call or was never released",
                              label.c_str(), ts.waitFunction.c_str()));
            Evidence(Format(L"%s re-entered %s and waits for a lock there", label.c_str(), ModuleLabel(owner).c_str()));
            return true;
        }

        std::wstring Analyzer::MessageName(std::uint32_t type) const
        {
            static const wchar_t* const kSkyrim[] = { L"PostLoad", L"PostPostLoad", L"PreLoadGame", L"PostLoadGame", L"SaveGame",
                                                      L"DeleteGame", L"InputLoaded", L"NewGame", L"DataLoaded" };
            static const wchar_t* const kFallout4[] = { L"PostLoad", L"PostPostLoad", L"PreLoadGame", L"PostLoadGame",
                                                        L"PreSaveGame", L"PostSaveGame", L"DeleteGame", L"InputLoaded",
                                                        L"NewGame", L"GameLoaded", L"GameDataReady" };
            if (s_.game == Game::kFallout4 && type < std::size(kFallout4))
                return kFallout4[type];
            if (s_.game != Game::kFallout4 && type < std::size(kSkyrim))
                return kSkyrim[type];
            return Format(L"%u", type);
        }

        const LockRecordInfo* Analyzer::RecordFor(const LockHit& lock, LockRecordKind kind) const
        {
            for (const auto& record : s_.lockRecords) {
                if (record.kind == kind && record.lockAddress == lock.lockAddress && record.threadId == lock.ownerTid)
                    return &record;
            }
            return nullptr;
        }

        bool Analyzer::LeftHeld(const LockHit& lock) const
        {
            return !lock.ownerAlive || RecordFor(lock, LockRecordKind::kLeftByHandler) != nullptr;
        }

        void Analyzer::ExplainLeftHeldLock(const LockHit& lock, Confidence handlerConfidence)
        {
            if (!explainedLocks_.insert(lock.lockAddress).second)
                return;
            const std::wstring where = Sym(lock.lockAddress);
            if (lock.broken)
                Evidence(Format(L"the lock at %s is marked as written with no writer count left: it was released in the "
                                L"wrong order",
                                where.c_str()));
            if (const LockRecordInfo* r = RecordFor(lock, LockRecordKind::kLeftByHandler)) {
                const int          module = s_.ModuleAt(r->code);
                const std::wstring text =
                    Format(L"its handler for the %s message returned while %s still held the lock at %s (%.0f s before the "
                           L"capture), and nothing released it afterwards",
                           MessageName(r->messageType).c_str(), ThreadLabel(r->threadId).c_str(), where.c_str(),
                           s_.Seconds(r->qpc, s_.captureQpc));
                Evidence(ModuleLabel(module) + L": " + text);
                if (Blameable(module)) {
                    AddSuspect(module, handlerConfidence, text);
                    leftBy_ = module;
                }
            } else if (const LockRecordInfo* exit = RecordFor(lock, LockRecordKind::kHeldAtExit)) {
                const int module = s_.ModuleAt(exit->code);
                Evidence(Format(L"thread %u (started by %s) exited %.0f s before the capture while still holding the lock "
                                L"at %s",
                                exit->threadId, ModuleLabel(module).c_str(), s_.Seconds(exit->qpc, s_.captureQpc), where.c_str()));
                AddSuspect(module, Confidence::kMedium,
                           Format(L"a thread this DLL started took the lock at %s and exited without releasing it", where.c_str()));
            } else if (!lock.ownerAlive) {
                Evidence(Format(L"thread %u holds the lock at %s but no longer exists: it took the lock and exited without "
                                L"releasing it",
                                lock.ownerTid, where.c_str()));
            }
            if (leftBy_ < 0) {
                if (!(s_.lockWatch & 1))
                    Evidence(L"message handlers were not checked for locks they leave held (an older plugin, or "
                             L"TrackLocks=0), so the code that left it locked could not be recorded");
                else
                    Evidence(L"no message handler registered after Dump Diver loaded was seen leaving it held");
            }
        }

        void Analyzer::ExplainLockOwner(const LockHit& lock, bool strongLink, int depth, std::set<std::uint32_t>& seen)
        {
            if (LeftHeld(lock)) {
                ExplainLeftHeldLock(lock, strongLink ? Confidence::kHigh : Confidence::kMedium);
                return;
            }
            ExplainThread(lock.ownerTid, strongLink, depth, seen);
        }

        // The main thread spin-waits in the game's code and no lock was found near it, but a lock the plugin
        // recorded as left held is still held exactly as recorded: very likely the one it waits for.
        bool Analyzer::LockFromRecords(const ThreadSummary& main)
        {
            if (main.wait != WaitKind::kSleep || KindAt(main.attributed) != ModuleKind::kGame)
                return false;
            bool found = false;
            for (const auto& record : s_.lockRecords) {
                const auto owner = static_cast<std::uint32_t>(record.current);
                const auto word = static_cast<std::uint32_t>(record.current >> 32);
                if (owner != record.threadId || owner == main.tid || !word || (word & 0x7FFFFFFFu) > 0xFFFFu)
                    continue;
                LockHit lock;
                lock.waiterTid = main.tid;
                lock.lockAddress = record.lockAddress;
                lock.ownerTid = owner;
                lock.readWrite = (word & 0x80000000u) != 0;
                lock.count = word & 0x7FFFFFFFu;
                lock.broken = lock.readWrite && lock.count == 0;
                lock.inGameData = true;
                lock.ownerAlive = s_.Thread(owner) != nullptr;
                lock.foundIn = L"the game plugin's record";
                if (!LeftHeld(lock) && !RecordFor(lock, LockRecordKind::kHeldAtExit))
                    continue;
                if (!found) {
                    v_.kind = L"lock_left_held";
                    v_.title = L"A game lock was left locked";
                    Evidence(L"the main thread spin-waits in the game's code while a game lock recorded as left held is "
                             L"still held");
                }
                found = true;
                ExplainLeftHeldLock(lock, Confidence::kMedium);
            }
            return found;
        }

        void Analyzer::SafetyNet()
        {
            const std::wstring& k = v_.kind;
            if (!v_.suspects.empty() || k == L"gpu_wait" || k == L"no_data" || k == L"not_frozen" || k == L"hidden_dialog")
                return;
            for (const auto& ts : v_.threads) {
                if (!ts.running || ts.ownIdleWorker || !Blameable(ts.nearestPlugin) || ts.nearestShare < 0.8)
                    continue;
                if (ts.tid == s_.mainThreadId && (ts.nearestSelfShare <= 0 || k == L"endless_loading"))
                    continue;
                AddSuspect(ts.nearestPlugin, Confidence::kLow,
                           Format(L"%s had this DLL on its stack in %.0f%% of samples while the game was stuck; no stronger "
                                  L"link was found",
                                  ThreadLabel(ts.tid).c_str(), ts.nearestShare * 100));
            }
        }

        bool Analyzer::CheckDialog(const ThreadSummary& main)
        {
            if (main.running)
                return false;
            // A dialog on the main thread, or on another thread while the main thread waits (on
            // AE the script extender runs tasks on a game job thread the main thread waits for).
            const Dialog* dialog = nullptr;
            for (const auto& d : s_.dialogs) {
                if (d.visible && (!dialog || d.threadId == s_.mainThreadId))
                    dialog = &d;
            }
            if (!dialog)
                return false;
            const bool           onMain = dialog->threadId == s_.mainThreadId;
            const ThreadSummary* owner = onMain ? &main : Summary(dialog->threadId);
            if (!owner)
                return false;

            // The caller of MessageBox / DialogBox on the dialog's thread.
            int caller = -1;
            int deepest = -1;
            for (std::size_t i = 0; i < owner->stack.size(); ++i) {
                const std::wstring f = Func(owner->stack[i]);
                if (ContainsI(f, L"MessageBox") || ContainsI(f, L"DialogBox") || ContainsI(f, L"SoftModal"))
                    deepest = static_cast<int>(i);
            }
            // Without names for user32's internals, the first frame outside Windows is the caller.
            for (std::size_t i = static_cast<std::size_t>(deepest + 1); i < owner->stack.size(); ++i) {
                const int m = s_.ModuleAt(owner->stack[i]);
                if (m >= 0 && !IsInfrastructure(KindAt(m))) {
                    caller = m;
                    break;
                }
            }
            if (!onMain) {
                v_.path.push_back(dialog->threadId);
                Evidence(Format(L"the message box belongs to %s; the main thread waits (%s)", ThreadLabel(dialog->threadId).c_str(),
                                main.waitFunction.c_str()));
            }

            v_.kind = L"hidden_dialog";
            v_.title = L"A message box is waiting";
            v_.summary = Format(L"The game is not frozen: %s opened a message box (\"%s\") and the game waits until it is "
                                L"closed. It is probably behind the game window; Alt-Tab to it and read it.",
                                caller >= 0 ? ModuleLabel(caller).c_str() : L"something", dialog->title.c_str());
            Evidence(Format(L"message box \"%s\": %s", dialog->title.c_str(), dialog->text.c_str()));
            if (Blameable(caller)) {
                AddSuspect(caller, onMain ? Confidence::kHigh : Confidence::kMedium,
                           onMain ? L"it opened the message box the main thread is waiting on"
                                  : L"it opened a message box on a game thread while the main thread waits");
                v_.advice.push_back(L"The message box text usually says what the DLL is missing or which version it needs.");
            } else {
                v_.advice.push_back(L"The message comes from the game or the script extender; its text names the problem.");
            }
            v_.confidence = Blameable(caller) && onMain ? Confidence::kHigh : Confidence::kMedium;
            return true;
        }

        void Analyzer::MainRunning(const ThreadSummary& main)
        {
            const ThreadData& t = *s_.Thread(main.tid);
            Evidence(Format(L"the main thread is busy (%.0f%% CPU); %.0f%% of its samples are in %s", main.cpuPercent,
                            main.attributedShare * 100, ModuleLabel(main.attributed).c_str()));
            if (main.waitingShare >= 0.5 && WaitKindOf(main.waitFunction) == WaitKind::kFileIo) {
                Evidence(Format(L"it is in a file read (%s) in %.0f%% of samples while using a full core: it reads in a loop, "
                                L"each read returning at once, rather than waiting for the disk",
                                main.waitFunction.c_str(), main.waitingShare * 100));
            }
            const ModuleKind kind = KindAt(main.attributed);

            if (Blameable(main.attributed) && main.attributedShare >= 0.5 && HoldsBrokenRecord(t)) {
                // A DLL that hooks the engine's record code (RaceMenu's face code, say) loops when it walks a record
                // that refers back to itself. The DLL that broke the record is the cause, not the one reading it.
                v_.kind = L"main_thread_loop_in_plugin";
                v_.title = L"The main thread is stuck walking a broken record";
                BlameBrokenRecords(t, L"the main thread's loop");
                AddSuspect(main.attributed, Confidence::kLow,
                           Format(L"the main thread kept running this DLL's code in %.0f%% of the samples, but it walks a "
                                  L"broken record there: it may only be reading data another mod broke",
                                  main.attributedShare * 100));
                v_.summary = Format(L"The main thread keeps running %s on a record that is broken in a way no plugin file "
                                    L"can store, and never finishes the frame. The mod that broke the record is the likelier "
                                    L"cause than the code that reads it.",
                                    ModuleLabel(main.attributed).c_str());
                v_.advice.push_back(L"Check the runtime patchers and distributors that change this record (their config files too).");
                return;
            }
            if (Blameable(main.attributed) && main.attributedShare >= 0.5) {
                v_.kind = L"main_thread_loop_in_plugin";
                v_.title = L"The main thread is stuck running a DLL's code";
                AddSuspect(main.attributed, main.attributedShare >= 0.8 ? Confidence::kHigh : Confidence::kMedium,
                           Format(L"the main thread kept running this DLL's code in %.0f%% of the samples",
                                  main.attributedShare * 100));
                v_.summary = Format(L"The main thread never stopped running code in %s, so the game cannot draw the next "
                                    L"frame. That DLL is looping or doing far too much work.",
                                    ModuleLabel(main.attributed).c_str());
                v_.advice.push_back(L"Disable this mod and try the same place again; if the freeze is gone, report it to its author with this file.");
                return;
            }
            if (kind == ModuleKind::kGraphics || kind == ModuleKind::kGraphicsInjector) {
                v_.kind = L"main_thread_busy_in_graphics";
                v_.title = L"The main thread is stuck in the graphics driver";
                if (kind == ModuleKind::kGraphicsInjector)
                    AddSuspect(main.attributed, Confidence::kMedium, L"the main thread kept running this graphics injector's code");
                v_.summary = Format(L"The main thread keeps running code in %s. That points at the graphics driver or a "
                                    L"graphics injector rather than a gameplay mod.",
                                    ModuleLabel(main.attributed).c_str());
                v_.advice.push_back(L"Update or roll back the graphics driver, and test without ENB/ReShade/upscalers.");
                return;
            }

            if (BlameLoopAroundEngine(main, L"the main thread", false)) {
                v_.kind = L"main_thread_loop_in_plugin";
                v_.title = L"The main thread is stuck in a DLL's loop around game functions";
                v_.summary = Format(L"The main thread keeps running %s and the game functions it calls, and never finishes "
                                    L"the frame. Most samples land in the game's code (memory, lookups), but the loop "
                                    L"itself is in that DLL.",
                                    ModuleLabel(main.nearestPlugin).c_str());
                v_.advice.push_back(L"Disable this mod and try again; if the freeze is gone, report it to its author with this file.");
                return;
            }

            v_.kind = L"main_thread_loop_in_game";
            v_.title = L"The main thread is stuck in a loop in the game's code";
            const bool brokenData = BlameBrokenRecords(t, L"the main thread's loop");
            if (const TaskInFlight* task = TaskOn(main.tid)) {
                SuspectTask(*task, brokenData || LoopsOnRecord(t) ? Confidence::kLow : Confidence::kMedium, L"the main thread");
            }
            CallPathEvidence(t, main.attributedFrame, L"the main thread");
            v_.summary = L"The main thread keeps running the game's own code without finishing a frame. Engine loops "
                         L"like this usually come from bad data: a broken record in a plugin, or a damaged save.";
            v_.advice.push_back(L"Load an earlier save. If that one works, the save or a recent change in the load order is the cause.");
            v_.advice.push_back(L"If it happens at the same place every time, disable plugins that edit that area or the characters there.");
        }

        void Analyzer::MainWaiting(const ThreadSummary& main)
        {
            std::set<std::uint32_t> seen;
            seen.insert(main.tid);
            Evidence(Format(L"the main thread is %s%s, called from %s", WaitVerb(main), WaitDetail(main).c_str(),
                            ModuleLabel(main.attributed).c_str()));

            // A deadlock Windows can see.
            const WaitChain* chain = ChainOf(main.tid);
            if (chain && chain->cycle) {
                v_.kind = L"deadlock";
                v_.title = L"Deadlock";
                std::map<int, int>      mentions;
                std::wstring            members;
                std::set<std::uint32_t> memberTids;
                for (const auto& node : chain->nodes) {
                    if (node.type != WctObject::kThread || node.processId != s_.processId ||
                        !memberTids.insert(node.threadId).second)
                        continue;
                    const ThreadSummary* ts = Summary(node.threadId);
                    if (!members.empty())
                        members += L", ";
                    members += ThreadLabel(node.threadId);
                    v_.path.push_back(node.threadId);
                    if (!ts)
                        continue;
                    int module = Blameable(ts->attributed) ? ts->attributed : -1;
                    if (module < 0 && Blameable(ts->startModule))
                        module = ts->startModule;
                    if (module >= 0) {
                        ++mentions[module];
                        AddSuspect(module, Confidence::kMedium,
                                   Format(L"%s is part of the deadlock and waits in or belongs to this DLL",
                                          ThreadLabel(node.threadId).c_str()));
                    }
                }
                for (const auto& [module, n] : mentions) {
                    if (n >= 2)
                        AddSuspect(module, Confidence::kHigh, L"it is on both sides of the deadlock");
                }
                Evidence(L"Windows reports a wait cycle between: " + members);
                v_.summary = L"Threads are waiting for each other and none can continue (" + members + L").";
                v_.advice.push_back(L"Deadlocks need two pieces of code that take the same locks in different orders; report it to the author of the suspected DLL with this file.");
                return;
            }

            // The main thread waits for another thread Windows can name.
            if (chain && chain->nodes.size() > 2) {
                for (std::size_t i = 1; i < chain->nodes.size(); ++i) {
                    const WctNode& node = chain->nodes[i];
                    if (node.type == WctObject::kThread && node.processId == s_.processId && node.threadId != main.tid) {
                        v_.kind = L"main_waits_for_thread";
                        v_.title = L"The main thread waits for another thread";
                        Evidence(Format(L"Windows wait chain: the main thread waits for %s", ThreadLabel(node.threadId).c_str()));
                        ExplainThread(node.threadId, true, 1, seen);
                        v_.summary = Format(L"The main thread waits for %s, which does not finish.", ThreadLabel(node.threadId).c_str());
                        return;
                    }
                }
            }

            if (IsGpuWait(main)) {
                v_.kind = L"gpu_wait";
                v_.title = L"The game waits for the graphics card";
                if (KindAt(main.attributed) == ModuleKind::kGraphicsInjector)
                    AddSuspect(main.attributed, Confidence::kLow, L"the wait happens inside this graphics injector");
                v_.summary = L"The main thread waits for the graphics card or its driver to finish work. No gameplay mod "
                             L"can be named from this; the cause is on the graphics side.";
                v_.advice.push_back(L"Update (or roll back) the graphics driver, and undo GPU overclocks or undervolts.");
                v_.advice.push_back(L"Test without ENB / ReShade / frame generation / upscalers, and with lower texture settings (video memory).");
                std::wstring injectors;
                for (const auto& m : s_.modules) {
                    if (m.kind == ModuleKind::kGraphicsInjector)
                        injectors += (injectors.empty() ? L"" : L", ") + m.name;
                }
                if (!injectors.empty())
                    Evidence(L"graphics injectors loaded: " + injectors);
                return;
            }

            if (main.wait == WaitKind::kFileIo) {
                v_.kind = L"file_io_wait";
                v_.title = L"The main thread waits for the disk";
                AddSuspect(main.attributed, Confidence::kLow, L"it started the file operation the main thread waits on");
                v_.summary = L"The main thread is blocked in a file operation that does not return. That is usually "
                             L"antivirus scanning, a failing or network drive, or the mod manager's virtual file system.";
                v_.advice.push_back(L"Add the game and mod manager folders to the antivirus exclusions and check the drive's health.");
                return;
            }

            // The main thread waits inside a mod's own code.
            if (Blameable(main.attributed)) {
                v_.kind = L"main_waits_in_plugin";
                v_.title = L"The main thread waits inside a DLL";
                if (!BlameOwnLock(main, L"the main thread"))
                    AddSuspect(main.attributed, Confidence::kMedium,
                               Format(L"the main thread is waiting inside this DLL's code%s", InBrackets(main.waitFunction).c_str()));
                for (const auto& lock : s_.locks) {
                    if (lock.waiterTid == main.tid)
                        ExplainLockOwner(lock, true, 1, seen);
                }
                for (const auto& ts : v_.threads) {
                    if (ts.tid != main.tid && ts.running && ts.attributed == main.attributed)
                        AddSuspect(main.attributed, Confidence::kHigh, Format(L"%s is busy in the same DLL", ThreadLabel(ts.tid).c_str()));
                }
                v_.summary = Format(L"The main thread is stuck waiting inside %s.", ModuleLabel(main.attributed).c_str());
                v_.advice.push_back(L"Disable this mod and try the same place again; if the freeze is gone, report it to its author with this file.");
                return;
            }

            // The game itself waits: for a job, a lock, or a sleep loop.
            v_.kind = L"engine_wait";
            v_.title = L"The game waits for work that never finishes";
            bool linked = false;
            bool leftHeld = false;
            for (const auto& lock : s_.locks) {
                if (lock.waiterTid != main.tid)
                    continue;
                if (LeftHeld(lock)) {
                    v_.kind = L"lock_left_held";
                    v_.title = L"A game lock was left locked";
                    Evidence(Format(L"the main thread retries a %s lock at %s held by %s", lock.readWrite ? L"read/write" : L"spin",
                                    Sym(lock.lockAddress).c_str(), ThreadLabel(lock.ownerTid).c_str()));
                    ExplainLeftHeldLock(lock, Confidence::kHigh);
                    linked = leftHeld = true;
                    continue;
                }
                v_.kind = L"engine_lock_wait";
                v_.title = L"The main thread waits for a lock another thread holds";
                Evidence(Format(L"the main thread retries a %s lock at %s held by %s", lock.readWrite ? L"read/write" : L"spin",
                                Sym(lock.lockAddress).c_str(), ThreadLabel(lock.ownerTid).c_str()));
                ExplainLockOwner(lock, lock.inGameData, 1, seen);
                linked = true;
            }
            if (!linked && LockFromRecords(main))
                linked = leftHeld = true;

            if (!linked) {
                // Tasks other plugins queued that are still running.
                for (const auto& task : s_.tasks) {
                    if (!task.startQpc || task.threadId == main.tid)
                        continue;
                    if (!StartedWithFreeze(task))
                        continue;  // started well after the freeze began
                    if (const ThreadSummary* ts = Summary(task.threadId); ts && ts->running)
                        continue;  // explained with the busy threads below, where its code is weighed too
                    SuspectTask(task, Confidence::kMedium, ThreadLabel(task.threadId));
                    // A task from the game, the script extender or a test tool names nobody: what its
                    // thread waits in is still looked at below.
                    if (Blameable(s_.ModuleAt(task.runAddress)) || Blameable(s_.ModuleAt(task.callerAddress)))
                        seen.insert(task.threadId);
                }
                // Busy threads, most CPU first.
                std::vector<const ThreadSummary*> busy;
                for (const auto& ts : v_.threads) {
                    if (ts.tid != main.tid && ts.running)
                        busy.push_back(&ts);
                }
                std::sort(busy.begin(), busy.end(), [](auto* a, auto* b) { return a->cpuPercent > b->cpuPercent; });
                // The main thread spin-waits in the game's own code and exactly one busy thread runs a DLL's
                // own code, a thread the game started (a job or worker the DLL hooks): the game is waiting
                // for that thread, so the link is as strong as a wait chain.
                std::uint32_t sole = 0;
                if (main.polling && KindAt(main.attributed) == ModuleKind::kGame) {
                    int candidates = 0;
                    for (const ThreadSummary* ts : busy) {
                        if (Blameable(ts->attributed) && ts->attributedShare >= 0.8) {
                            ++candidates;
                            if (KindAt(ts->startModule) == ModuleKind::kGame)
                                sole = ts->tid;
                        }
                    }
                    if (candidates != 1)
                        sole = 0;
                }
                if (sole) {
                    Evidence(Format(L"the main thread spin-waits while %s is the only busy thread in a mod's code: the game "
                                    L"is waiting for it",
                                    ThreadLabel(sole).c_str()));
                }
                for (const ThreadSummary* ts : busy)
                    ExplainThread(ts->tid, ts->tid == sole, 1, seen);

                // Game threads (started by the game) that wait inside a mod's code: the game may be
                // waiting for exactly that thread. A plugin's own idle threads look the same and are
                // ignored.
                for (const auto& ts : v_.threads) {
                    if (ts.tid == main.tid || ts.running || seen.count(ts.tid))
                        continue;
                    if (KindAt(ts.startModule) != ModuleKind::kGame || !Blameable(ts.attributed))
                        continue;
                    if (BlameOwnLock(ts, Format(L"game thread %u", ts.tid)))
                        continue;
                    AddSuspect(ts.attributed, Confidence::kLow,
                               Format(L"game thread %u is waiting inside this DLL's code%s", ts.tid, InBrackets(ts.waitFunction).c_str()));
                    Evidence(Format(L"game thread %u waits inside %s", ts.tid, ModuleLabel(ts.attributed).c_str()));
                }
                // Other threads retrying engine locks.
                for (const auto& lock : s_.locks) {
                    if (!seen.count(lock.waiterTid)) {
                        Evidence(Format(L"%s retries a lock held by %s", ThreadLabel(lock.waiterTid).c_str(),
                                        ThreadLabel(lock.ownerTid).c_str()));
                        ExplainLockOwner(lock, false, 1, seen);
                    }
                }
            }

            if (leftHeld) {
                if (leftBy_ >= 0)
                    v_.summary = Format(L"The main thread waits for a game lock that %s left locked. Nothing will ever "
                                        L"release it, so the game cannot continue.",
                                        ModuleLabel(leftBy_).c_str());
                else
                    v_.summary = L"The main thread waits for a game lock that a thread took and never released. That "
                                 L"thread has exited, so nothing will release it.";
                if (leftBy_ >= 0)
                    v_.advice.push_back(L"Disable this mod and try again; report it to its author with this file.");
                else
                    v_.advice.push_back(L"Code that ran at startup or during a load left the lock held; the mod log of "
                                        L"recently added SKSE/F4SE plugins is the place to look.");
            } else if (v_.suspects.empty()) {
                v_.summary = L"The main thread waits inside the game's own code and no thread it could be waiting for "
                             L"is running a mod's code. The cause is not visible in this capture.";
                v_.advice.push_back(L"If it repeats at the same place, note where and when; an engine fix mod or an earlier save may help.");
            } else {
                v_.summary = Format(L"The main thread waits inside the game's own code. %s",
                                    linked ? L"It waits for a lock held by another thread, described below."
                                           : L"Threads the game may be waiting for are running or waiting in mod code, listed below.");
            }
        }

        void Analyzer::Context()
        {
            if (s_.reason == CaptureReason::kManual)
                Evidence(L"captured with the manual hotkey");
            if (s_.reason == CaptureReason::kStartup)
                Evidence(L"the game never reached its main loop (no heartbeat since the plugin loaded)");
            if (s_.stateFlags & kState_LoadingSave)
                Evidence(Format(L"a save was being loaded (%.0f s before the capture)", s_.Seconds(s_.loadStartQpc, s_.captureQpc)));
            if (s_.stateFlags & kState_Saving)
                Evidence(L"the game was saving");
            if (s_.heartbeatQpc && s_.heartbeatAgeSeconds >= 1.0)
                Evidence(Format(L"no heartbeat for %.1f s", s_.heartbeatAgeSeconds));
            if (s_.windowHung)
                Evidence(L"Windows reports the game window as not responding");
            if (s_.recovered)
                Evidence(L"the game recovered during the capture: this was a long stall, not a permanent freeze");
            if (!s_.taskTracking)
                Evidence(L"task tracking was off; tasks queued by plugins could not be attributed");
            if (s_.loadingScreenSeconds > 0 && s_.reason != CaptureReason::kEndlessLoading)
                Evidence(Format(L"a loading screen was up (%.0f s)", s_.loadingScreenSeconds));
            if (!s_.loadingWatch)
                Evidence(L"loading screens were not watched in this game (see the plugin log)");
        }

        void Analyzer::Finish()
        {
            std::stable_sort(v_.suspects.begin(), v_.suspects.end(),
                             [](const Suspect& a, const Suspect& b) { return a.confidence > b.confidence; });
            v_.confidence = v_.suspects.empty() ? Confidence::kLow : std::max(v_.confidence, v_.suspects.front().confidence);
            if (s_.recovered && v_.confidence == Confidence::kHigh)
                v_.confidence = Confidence::kMedium;
            // A named suspect always comes with the first thing to try.
            if (!v_.suspects.empty() && v_.suspects.front().confidence >= Confidence::kMedium &&
                std::none_of(v_.advice.begin(), v_.advice.end(),
                             [](const std::wstring& line) { return line.starts_with(L"Disable this mod"); })) {
                v_.advice.insert(v_.advice.begin(),
                                 Format(L"Disable %s and try again; if the freeze is gone, report it to its author with this file.",
                                        ModuleLabel(v_.suspects.front().module).c_str()));
            }
            if (s_.reason == CaptureReason::kManual && !s_.windowHung && s_.heartbeatAgeSeconds < 2.0) {
                v_.evidence.insert(v_.evidence.begin(),
                                   L"the game was still responding when this manual capture was taken; it shows normal activity");
                v_.confidence = std::min(v_.confidence, Confidence::kLow);
            }
        }

        void Analyzer::EndlessLoading(const ThreadSummary& main)
        {
            v_.kind = L"endless_loading";
            v_.title = L"The loading screen never finishes";
            Evidence(Format(L"the loading screen has been up for %.0f s while the game keeps running", s_.loadingScreenSeconds));
            std::set<std::uint32_t> seen;
            seen.insert(main.tid);

            // The main thread is not suspected here: the game still runs frames (the heartbeat is
            // fresh), so it is drawing the loading screen, not doing the loading. Frame limiters
            // and hooks on the frame loop would look guilty for no reason.
            Evidence(Format(L"the main thread keeps running frames (%.0f%% CPU), mostly in %s", main.cpuPercent,
                            ModuleLabel(main.attributed).c_str()));

            // Tasks that started long ago and are still running.
            for (const auto& task : s_.tasks) {
                if (!task.startQpc || task.threadId == main.tid || s_.Seconds(task.startQpc, s_.captureQpc) < 5.0)
                    continue;
                SuspectTask(task, Confidence::kMedium, ThreadLabel(task.threadId));
                if (seen.insert(task.threadId).second) {
                    loadingThreads_.push_back(task.threadId);
                    v_.path.push_back(task.threadId);
                }
            }

            // Busy threads: loading that never ends is often a thread that never stops working.
            std::vector<const ThreadSummary*> busy;
            for (const auto& ts : v_.threads) {
                if (ts.tid != main.tid && ts.running && !seen.count(ts.tid))
                    busy.push_back(&ts);
            }
            std::sort(busy.begin(), busy.end(), [](auto* a, auto* b) { return a->cpuPercent > b->cpuPercent; });
            for (const ThreadSummary* ts : busy) {
                loadingThreads_.push_back(ts->tid);
                ExplainThread(ts->tid, false, 1, seen);
            }

            bool fileStall = false;
            for (const auto& ts : v_.threads) {
                if (seen.count(ts.tid) || ts.running)
                    continue;
                const bool gameThread = KindAt(ts.startModule) == ModuleKind::kGame;
                if (ts.wait == WaitKind::kFileIo && ts.waitingShare >= 0.9) {
                    fileStall = true;
                    Evidence(Format(L"%s sat in file I/O (%s) for the whole capture", ThreadLabel(ts.tid).c_str(), ts.waitFunction.c_str()));
                    AddSuspect(ts.attributed, Confidence::kLow, Format(L"%s waits for a file operation it started", ThreadLabel(ts.tid).c_str()));
                    loadingThreads_.push_back(ts.tid);
                    v_.path.push_back(ts.tid);
                    seen.insert(ts.tid);
                } else if (gameThread && Blameable(ts.attributed)) {
                    AddSuspect(ts.attributed, Confidence::kLow,
                               Format(L"game thread %u is waiting inside this DLL's code%s", ts.tid, InBrackets(ts.waitFunction).c_str()));
                    Evidence(Format(L"game thread %u waits inside %s", ts.tid, ModuleLabel(ts.attributed).c_str()));
                    loadingThreads_.push_back(ts.tid);
                    v_.path.push_back(ts.tid);
                    seen.insert(ts.tid);
                }
            }

            for (const auto& lock : s_.locks) {
                if (seen.count(lock.waiterTid))
                    continue;
                Evidence(Format(L"%s retries a lock held by %s", ThreadLabel(lock.waiterTid).c_str(), ThreadLabel(lock.ownerTid).c_str()));
                loadingThreads_.push_back(lock.ownerTid);
                ExplainLockOwner(lock, lock.inGameData, 1, seen);
            }

            if (!v_.suspects.empty()) {
                v_.summary = Format(L"The loading screen has been up for %.0f s. The threads doing the loading are stuck in "
                                    L"mod code, listed below.",
                                    s_.loadingScreenSeconds);
            } else if (fileStall) {
                v_.summary = Format(L"The loading screen has been up for %.0f s and a thread is stuck reading or writing a "
                                    L"file. That is usually antivirus scanning, a failing or network drive, or the mod "
                                    L"manager's virtual file system.",
                                    s_.loadingScreenSeconds);
                v_.advice.push_back(L"Add the game and mod manager folders to the antivirus exclusions and check the drive's health.");
            } else {
                v_.summary = Format(L"The loading screen has been up for %.0f s and no thread is running a mod's code. The "
                                    L"records the game was loading are listed below; a broken record or a damaged save "
                                    L"is the usual cause.",
                                    s_.loadingScreenSeconds);
            }
            v_.advice.push_back(L"Try loading an earlier save, or loading into a different place (coc to another cell from the main menu).");
            v_.advice.push_back(L"If the same place never loads, disable the plugins named under Records involved one at a time.");
        }

        void Analyzer::AddRecords(std::uint32_t tid, std::size_t max, const std::wstring& why, bool mayBeMedium)
        {
            const ThreadData*    t = s_.Thread(tid);
            const ThreadSummary* ts = Summary(tid);
            if (!t || t->forms.empty())
                return;
            const double count = static_cast<double>(std::max<std::size_t>(t->samples.size(), 1));
            std::size_t  added = 0;
            for (const FormRef& ref : t->forms) {
                if (added >= max)
                    break;
                const FormInfo& form = s_.forms[static_cast<std::size_t>(ref.form)];
                if (form.className == L"PlayerCharacter")
                    continue;  // on nearly every game thread's stack; says nothing
                const double share = ref.samples / count;
                // A busy thread moves on quickly, so a record in a sixth of its samples still matters; a
                // waiting thread shows everything in every sample.
                if (share < (ts && ts->running ? 0.15 : 0.3))
                    continue;
                Confidence confidence = Confidence::kLow;
                if (mayBeMedium && ts && ts->running && share >= 0.9 && ref.offset <= 0x100)
                    confidence = Confidence::kMedium;
                ++added;
                bool merged = false;
                for (auto& lead : v_.records) {
                    if (lead.form == ref.form) {
                        lead.confidence = std::max(lead.confidence, confidence);
                        lead.share = std::max(lead.share, share);
                        merged = true;
                        break;
                    }
                }
                if (!merged) {
                    v_.records.push_back({ ref.form, tid, share, confidence,
                                           Format(L"%s (%s, %.0f%% of its samples)", why.c_str(), ThreadLabel(tid).c_str(), share * 100),
                                           ref.offset });
                }
            }
        }

        void Analyzer::CollectRecords()
        {
            const std::uint32_t main = s_.mainThreadId;
            const std::wstring& k = v_.kind;
            if (k == L"main_thread_loop_in_game") {
                AddRecords(main, 5, L"the main thread's loop was working on it", true);
            } else if (k == L"engine_wait" || k == L"engine_lock_wait" || k == L"main_waits_for_thread") {
                for (const std::uint32_t tid : v_.path) {
                    if (tid != main)
                        AddRecords(tid, 3, L"a thread the main thread waits for was working on it", true);
                }
                AddRecords(main, 3, L"the main thread was working on it", false);
            } else if (k == L"endless_loading") {
                for (const std::uint32_t tid : loadingThreads_)
                    AddRecords(tid, 4, L"a thread that is part of the loading was working on it", true);
                for (const std::uint32_t tid : v_.path) {
                    if (tid != main)
                        AddRecords(tid, 3, L"a thread that is part of the loading was working on it", true);
                }
                AddRecords(main, 3, L"the main thread was working on it", false);
            } else if (k == L"deadlock") {
                for (const std::uint32_t tid : v_.path)
                    AddRecords(tid, 3, L"a thread in the deadlock was working on it", false);
            } else if (k != L"gpu_wait" && k != L"no_data") {
                AddRecords(main, 3, L"the main thread was working on it", false);
            }
            // A reference's base object and cell are shown with the reference.
            std::set<int> shownWithReference;
            for (const auto& lead : v_.records) {
                const FormInfo& form = s_.forms[static_cast<std::size_t>(lead.form)];
                if (form.baseForm >= 0)
                    shownWithReference.insert(form.baseForm);
                if (form.cell >= 0)
                    shownWithReference.insert(form.cell);
            }
            std::erase_if(v_.records, [&](const RecordLead& lead) { return shownWithReference.count(lead.form) > 0; });
            std::stable_sort(v_.records.begin(), v_.records.end(), [](const RecordLead& a, const RecordLead& b) {
                if (a.confidence != b.confidence)
                    return a.confidence > b.confidence;
                if (a.share != b.share)
                    return a.share > b.share;
                return a.offset < b.offset;
            });
            if (v_.records.size() > 10)
                v_.records.resize(10);
            // Low-confidence records are only bystanders on the stack; they get no advice of their own.
            if (!v_.records.empty() && v_.records.front().confidence >= Confidence::kMedium) {
                v_.advice.push_back(L"Records involved: open them in xEdit (SSEEdit / FO4Edit) and check the plugin that last "
                                    L"changed them for errors; disabling that plugin is the quickest test.");
            }
        }

        Verdict Analyzer::Run()
        {
            for (const auto& t : s_.threads) {
                index_[t.tid] = v_.threads.size();
                v_.threads.push_back(Summarize(t));
            }
            Context();

            const ThreadData* main = s_.Thread(s_.mainThreadId);
            if (!main || main->samples.empty()) {
                v_.kind = L"no_data";
                v_.title = L"The main thread could not be sampled";
                v_.summary = L"Dump Diver could not read the main thread, so it cannot say what the game was doing.";
                Finish();
                return v_;
            }
            const ThreadSummary mainSummary = *Summary(main->tid);
            v_.path.push_back(main->tid);

            if (s_.reason == CaptureReason::kEndlessLoading) {
                EndlessLoading(mainSummary);
            } else if (s_.reason == CaptureReason::kManual && !s_.windowHung && s_.heartbeatAgeSeconds < 2.0) {
                v_.kind = L"not_frozen";
                v_.title = L"The game was running normally";
                v_.summary = L"This manual capture was taken while the game still answered. It shows what each thread "
                             L"was doing at that moment; nothing in it is a fault by itself.";
                Evidence(Format(L"the main thread used %.0f%% CPU, mostly in %s", mainSummary.cpuPercent,
                                ModuleLabel(mainSummary.attributed).c_str()));
            } else if (!CheckDialog(mainSummary)) {
                if (mainSummary.running)
                    MainRunning(mainSummary);
                else
                    MainWaiting(mainSummary);
            }
            SafetyNet();
            CollectRecords();
            Finish();
            return v_;
        }
    }

    Verdict Analyze(const Snapshot& snapshot) { return Analyzer(snapshot).Run(); }
}
