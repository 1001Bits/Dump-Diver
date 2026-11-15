#include "helper/Capture.h"

#include "helper/Analyze.h"
#include "helper/Process.h"
#include "helper/Records.h"
#include "helper/Report.h"
#include "helper/SnapshotFile.h"
#include "helper/Stack.h"
#include "helper/Util.h"

#include <timeapi.h>
#include <wct.h>

#pragma warning(push)
#pragma warning(disable : 4091)
#include <dbghelp.h>
#pragma warning(pop)

#include <algorithm>
#include <set>
#include <unordered_map>

namespace fs = std::filesystem;

namespace dd::helper
{
    namespace
    {
        constexpr std::size_t kStackBytes = 0x1000;  // also searched for game records
        constexpr int         kMaxFrames = 64;

        struct SampledThread
        {
            std::uint32_t tid = 0;
            HANDLE        handle = nullptr;
            std::size_t   index = 0;  // into snapshot.threads
            bool          parked = false;
        };

        const wchar_t* GameName(Game game)
        {
            return game == Game::kSkyrim ? L"Skyrim" : game == Game::kFallout4 ? L"Fallout4" : L"Game";
        }

        const wchar_t* ReasonName(CaptureReason reason)
        {
            switch (reason) {
            case CaptureReason::kStartup:
                return L"Startup";
            case CaptureReason::kManual:
                return L"Manual";
            case CaptureReason::kEndlessLoading:
                return L"Loading";
            default:
                return L"Freeze";
            }
        }

        void CollectWaitChains(Snapshot& snapshot)
        {
            HWCT session = OpenThreadWaitChainSession(0, nullptr);
            if (!session) {
                Log(L"wait chains unavailable (error %lu)", GetLastError());
                return;
            }
            for (const auto& thread : snapshot.threads) {
                WAITCHAIN_NODE_INFO nodes[WCT_MAX_NODE_COUNT]{};
                DWORD               count = WCT_MAX_NODE_COUNT;
                BOOL                cycle = FALSE;
                if (!GetThreadWaitChain(session, 0, WCTP_GETINFO_ALL_FLAGS, thread.tid, &count, nodes, &cycle))
                    continue;
                if (count < 2)
                    continue;
                WaitChain chain;
                chain.threadId = thread.tid;
                chain.cycle = cycle != FALSE;
                for (DWORD i = 0; i < count; ++i) {
                    WctNode node;
                    node.type = static_cast<WctObject>(nodes[i].ObjectType);
                    node.status = static_cast<std::uint32_t>(nodes[i].ObjectStatus);
                    if (nodes[i].ObjectType == WctThreadType) {
                        node.processId = nodes[i].ThreadObject.ProcessId;
                        node.threadId = nodes[i].ThreadObject.ThreadId;
                        node.waitTime = nodes[i].ThreadObject.WaitTime;
                        node.contextSwitches = nodes[i].ThreadObject.ContextSwitches;
                    } else {
                        node.objectName.assign(nodes[i].LockObject.ObjectName,
                                               wcsnlen(nodes[i].LockObject.ObjectName, WCT_OBJNAME_LENGTH));
                    }
                    chain.nodes.push_back(std::move(node));
                }
                snapshot.chains.push_back(std::move(chain));
            }
            CloseThreadWaitChainSession(session);
        }

        struct DialogSearch
        {
            DWORD                pid;
            std::vector<Dialog>* out;
        };

        BOOL CALLBACK ChildText(HWND hwnd, LPARAM param)
        {
            auto*   text = reinterpret_cast<std::wstring*>(param);
            wchar_t className[32]{};
            GetClassNameW(hwnd, className, 32);
            if (_wcsicmp(className, L"Static") != 0 && _wcsicmp(className, L"Edit") != 0)
                return TRUE;
            wchar_t   buffer[2048]{};
            DWORD_PTR length = 0;
            // The dialog's thread pumps messages, so WM_GETTEXT across processes answers.
            if (SendMessageTimeoutW(hwnd, WM_GETTEXT, 2048, reinterpret_cast<LPARAM>(buffer), SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                    500, &length) &&
                length) {
                if (!text->empty())
                    *text += L" | ";
                *text += buffer;
            }
            return TRUE;
        }

        BOOL CALLBACK FindDialogs(HWND hwnd, LPARAM param)
        {
            auto* search = reinterpret_cast<DialogSearch*>(param);
            DWORD pid = 0;
            const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
            if (pid != search->pid)
                return TRUE;
            wchar_t className[64]{};
            GetClassNameW(hwnd, className, 64);
            if (wcscmp(className, L"#32770") != 0)
                return TRUE;
            Dialog dialog;
            dialog.threadId = tid;
            dialog.className = className;
            dialog.visible = IsWindowVisible(hwnd) != FALSE;
            wchar_t title[512]{};
            GetWindowTextW(hwnd, title, 512);
            dialog.title = title;
            EnumChildWindows(hwnd, &ChildText, reinterpret_cast<LPARAM>(&dialog.text));
            search->out->push_back(std::move(dialog));
            return TRUE;
        }

        // Finds locks held by other threads near threads that sleep in a retry loop inside the
        // game: the engine's BSSpinLock and BSReadWriteLock spin with Sleep, so Windows' wait
        // chains never see them.
        void ScanLocks(HANDLE process, Snapshot& snapshot)
        {
            int exeIndex = -1;
            for (std::size_t i = 0; i < snapshot.modules.size(); ++i) {
                if (snapshot.modules[i].kind == ModuleKind::kGame)
                    exeIndex = static_cast<int>(i);
            }
            if (exeIndex < 0)
                return;
            const Module&        exe = snapshot.modules[static_cast<std::size_t>(exeIndex)];
            std::vector<Section> data;
            for (const auto& section : ImageSections(process, exe.base)) {
                if (section.writable && !section.executable)
                    data.push_back(section);
            }
            std::set<std::uint32_t> tids;
            for (const auto& t : snapshot.threads)
                tids.insert(t.tid);

            std::set<std::pair<std::uint32_t, std::uint64_t>> found;
            for (const auto& t : snapshot.threads) {
                if (t.samples.empty())
                    continue;
                const Sample& last = t.samples.back();
                if (last.frames.empty())
                    continue;
                const auto sym = snapshot.symbols.find(last.frames[0]);
                if (sym == snapshot.symbols.end() || WaitKindOf(FunctionName(sym->second)) != WaitKind::kSleep)
                    continue;
                // The first frame outside Windows must be the game: the game is the one retrying.
                std::size_t gameFrame = 0;
                bool        isGame = false;
                for (std::size_t i = 0; i < last.frames.size(); ++i) {
                    const int m = snapshot.ModuleAt(last.frames[i]);
                    if (m < 0 || IsInfrastructure(snapshot.modules[static_cast<std::size_t>(m)].kind))
                        continue;
                    isGame = m == exeIndex;
                    gameFrame = i;
                    break;
                }
                if (!isGame)
                    continue;

                // GPU query poll?
                const std::uint64_t returnAddress = last.frames[gameFrame];
                std::uint8_t        code[0x80]{};
                if (ReadMemory(process, returnAddress - sizeof(code), code, sizeof(code)))
                    snapshot.gpuPollSites[returnAddress] = LooksLikeGpuPoll(code, sizeof(code));

                // Values to test: non-volatile registers and the retrying function's own frame.
                std::vector<std::pair<std::uint64_t, std::wstring>> values = {
                    { t.registers.rbx, L"RBX" }, { t.registers.rbp, L"RBP" }, { t.registers.rsi, L"RSI" },
                    { t.registers.rdi, L"RDI" }, { t.registers.r12, L"R12" }, { t.registers.r13, L"R13" },
                    { t.registers.r14, L"R14" }, { t.registers.r15, L"R15" },
                };
                std::size_t limit = t.stackTop.size();
                if (gameFrame + 1 < last.frames.size()) {
                    for (std::size_t off = 0; off + 8 <= t.stackTop.size(); off += 8) {
                        std::uint64_t value = 0;
                        memcpy(&value, t.stackTop.data() + off, 8);
                        if (value == last.frames[gameFrame + 1]) {
                            // up to the return address, plus the 0x20-byte home area above it where
                            // the function may have spilled its parameters (the lock pointer)
                            limit = std::min(off + 8 + 0x20, t.stackTop.size());
                            break;
                        }
                    }
                }
                for (std::size_t off = 0; off + 8 <= limit; off += 8) {
                    std::uint64_t value = 0;
                    memcpy(&value, t.stackTop.data() + off, 8);
                    values.emplace_back(value, Format(L"RSP+%#zx", off));
                }

                for (const auto& [value, where] : values) {
                    if (value < 0x10000 || (value & 3))
                        continue;
                    bool inData = false;
                    for (const auto& section : data) {
                        if (value >= section.start && value + 8 <= section.end)
                            inData = true;
                    }
                    if (!inData) {
                        MEMORY_BASIC_INFORMATION info{};
                        if (!VirtualQueryEx(process, reinterpret_cast<LPCVOID>(value), &info, sizeof(info)) ||
                            info.State != MEM_COMMIT || info.Type != MEM_PRIVATE ||
                            (info.Protect & (PAGE_READWRITE | PAGE_GUARD)) != PAGE_READWRITE)
                            continue;
                    }
                    std::uint64_t raw = 0;
                    if (!ReadMemory(process, value, &raw, sizeof(raw)))
                        continue;
                    std::uint32_t owner = 0, count = 0;
                    bool          readWrite = false;
                    if (!DecodeLock(raw, owner, count, readWrite) || owner == t.tid)
                        continue;
                    // An owner that no longer exists only counts for a lock in the game's own data,
                    // where these locks guard global tables; on the heap it is too likely to be noise.
                    // Thread ids stay far below a million; a larger "owner" with a 0x7FFx count is the two halves
                    // of a pointer.
                    const bool alive = tids.count(owner) != 0;
                    if (!alive && (!inData || owner >= 0x100000))
                        continue;
                    if (!found.insert({ t.tid, value }).second)
                        continue;
                    LockHit hit;
                    hit.waiterTid = t.tid;
                    hit.lockAddress = value;
                    hit.ownerTid = owner;
                    hit.count = count;
                    hit.readWrite = readWrite;
                    hit.inGameData = inData;
                    hit.foundIn = where;
                    hit.ownerAlive = alive;
                    hit.broken = readWrite && count == 0;
                    snapshot.locks.push_back(hit);
                }
            }
        }

        void CopyShared(const Header& shared, Snapshot& snapshot)
        {
            const std::uint32_t slots = shared.taskSlotCount;
            for (std::uint32_t i = 0; i < std::min(slots, kTaskSlots); ++i) {
                const TaskSlot& slot = shared.tasks[i];
                for (int attempt = 0; attempt < 4; ++attempt) {
                    const std::uint32_t before = slot.sequence;
                    TaskInFlight        task;
                    task.threadId = slot.threadId;
                    task.startQpc = slot.startQpc;
                    task.runAddress = slot.runAddress;
                    task.callerAddress = slot.callerAddress;
                    task.queuedQpc = slot.queuedQpc;
                    task.kind = slot.kind;
                    if ((before & 1) == 0 && before == slot.sequence) {
                        if (task.startQpc)
                            snapshot.tasks.push_back(task);
                        break;
                    }
                    Sleep(0);
                }
            }
            snapshot.lockWatch = shared.lockWatch;
            const std::uint32_t records = std::min(static_cast<std::uint32_t>(shared.lockRecordCount), kLockRecords);
            for (std::uint32_t i = 0; i < records; ++i) {
                const LockRecord& r = shared.lockRecords[i];
                const auto        kind = static_cast<LockRecordKind>(r.kind);
                if (kind == LockRecordKind::kNone)
                    continue;  // still being written
                LockRecordInfo info;
                info.qpc = r.qpc;
                info.lockAddress = r.lockAddress;
                info.code = r.code;
                info.threadId = r.threadId;
                info.kind = kind;
                info.lockWord = r.lockWord;
                info.messageType = r.messageType;
                snapshot.lockRecords.push_back(info);
            }
            const std::uint64_t total = shared.eventIndex;
            const std::uint64_t first = total > kEventCount ? total - kEventCount : 0;
            for (std::uint64_t i = first; i < total; ++i) {
                const Event& e = shared.events[i % kEventCount];
                snapshot.events.push_back({ e.qpc, static_cast<EventType>(e.type), e.threadId, e.a });
            }
        }

        bool LooksLikePointer(std::uint64_t value)
        {
            return value >= 0x10000 && value < 0x7FFFFFFF0000ull && (value & 7) == 0;
        }

        // Counts, per value, the samples it appeared in (registers or the top of the stack) and
        // the nearest stack offset it was seen at.
        void CountValues(const ThreadData& data, std::unordered_map<std::uint64_t, ValueStat>& counts)
        {
            std::vector<std::pair<std::uint64_t, std::uint32_t>> seen;
            const Registers& r = data.registers;
            for (const std::uint64_t value : { r.rax, r.rbx, r.rcx, r.rdx, r.rsi, r.rdi, r.rbp, r.r8, r.r9, r.r10, r.r11,
                                               r.r12, r.r13, r.r14, r.r15 }) {
                if (LooksLikePointer(value))
                    seen.emplace_back(value, 0);
            }
            for (std::size_t off = 0; off + 8 <= data.stackTop.size(); off += 8) {
                std::uint64_t value = 0;
                memcpy(&value, data.stackTop.data() + off, 8);
                if (LooksLikePointer(value))
                    seen.emplace_back(value, static_cast<std::uint32_t>(off + 8));
            }
            std::sort(seen.begin(), seen.end());
            for (std::size_t i = 0; i < seen.size(); ++i) {
                if (i && seen[i].first == seen[i - 1].first)
                    continue;  // once per sample; the first has the smallest offset
                ValueStat& stat = counts[seen[i].first];
                ++stat.samples;
                stat.offset = std::min(stat.offset, seen[i].second);
            }
        }

        // Finds the game records each relevant thread was working on.
        void ResolveRecords(HANDLE process, Snapshot& snapshot, std::vector<std::unordered_map<std::uint64_t, ValueStat>>& values)
        {
            const Module* exe = nullptr;
            for (const auto& m : snapshot.modules) {
                if (m.kind == ModuleKind::kGame)
                    exe = &m;
            }
            if (!exe || snapshot.game == Game::kUnknown)
                return;
            FormResolver        resolver(process, *exe, snapshot.game);
            const std::uint64_t start = Qpc();
            const double        window = snapshot.sampleSeconds > 0 ? snapshot.sampleSeconds : 1;

            for (std::size_t i = 0; i < snapshot.threads.size() && i < values.size(); ++i) {
                ThreadData& t = snapshot.threads[i];
                const int   startModule = snapshot.ModuleAt(t.startAddress);
                const bool  gameThread = startModule >= 0 && snapshot.modules[static_cast<std::size_t>(startModule)].kind == ModuleKind::kGame;
                const bool  busy = static_cast<double>(t.cpuTime100ns) / (window * 1e7) >= 0.1;
                const bool  task = std::any_of(snapshot.tasks.begin(), snapshot.tasks.end(),
                                               [&](const TaskInFlight& x) { return x.threadId == t.tid; });
                if (!(t.isMain || gameThread || busy || task))
                    continue;

                std::vector<std::pair<std::uint64_t, ValueStat>> candidates(values[i].begin(), values[i].end());
                std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
                    return a.second.samples != b.second.samples ? a.second.samples > b.second.samples : a.second.offset < b.second.offset;
                });
                if (candidates.size() > 800)
                    candidates.resize(800);
                for (const auto& [value, stat] : candidates) {
                    const int form = resolver.Resolve(value, snapshot);
                    if (form >= 0)
                        t.forms.push_back({ form, stat.samples, stat.offset });
                }
                std::stable_sort(t.forms.begin(), t.forms.end(), [](const FormRef& a, const FormRef& b) {
                    return a.samples != b.samples ? a.samples > b.samples : a.offset < b.offset;
                });
                if (t.forms.size() > 12)
                    t.forms.resize(12);
                if (static_cast<double>(Qpc() - start) / static_cast<double>(snapshot.qpcFrequency) > 4.0)
                    break;  // time budget
            }
            values.clear();
        }

        // The threads whose OS thread block a small dump keeps: the main thread, every thread the verdict or the
        // wait chains point at, busy threads and lock owners. Every thread's stack is in the dump.
        std::set<std::uint32_t> DumpThreads(const Snapshot& snapshot, const Verdict& verdict)
        {
            std::set<std::uint32_t> keep(verdict.path.begin(), verdict.path.end());
            for (const auto& thread : snapshot.threads) {
                if (thread.isMain || thread.ranHeartbeat)
                    keep.insert(thread.tid);
            }
            std::set<int> suspects;
            for (const auto& suspect : verdict.suspects)
                suspects.insert(suspect.module);
            for (const auto& thread : verdict.threads) {
                if (thread.running || thread.cpuPercent >= 1.0 || suspects.count(thread.attributed) || suspects.count(thread.nearestPlugin))
                    keep.insert(thread.tid);
            }
            for (const auto& record : verdict.records)
                keep.insert(record.threadId);
            for (const auto& chain : snapshot.chains) {
                if (!keep.count(chain.threadId))
                    continue;
                for (const auto& node : chain.nodes) {
                    if (node.threadId)
                        keep.insert(node.threadId);
                }
            }
            for (const auto& lock : snapshot.locks) {
                keep.insert(lock.waiterTid);
                keep.insert(lock.ownerTid);
            }
            for (const auto& record : snapshot.lockRecords)
                keep.insert(record.threadId);
            for (const auto& dialog : snapshot.dialogs)
                keep.insert(dialog.threadId);
            keep.erase(0);
            return keep;
        }

        // The OS data of every thread and of the process (MiniDumpWithProcessThreadData) is most of a
        // small dump's size, so the dump adds only the thread blocks (TEBs) of the threads that matter.
        struct DumpExtras
        {
            const std::set<std::uint32_t>*                    keep = nullptr;
            std::vector<std::pair<std::uint64_t, std::uint32_t>> ranges;
            std::size_t                                       next = 0;
        };

        BOOL CALLBACK DumpCallback(PVOID param, const PMINIDUMP_CALLBACK_INPUT input, PMINIDUMP_CALLBACK_OUTPUT output)
        {
            if (!input || !output)
                return TRUE;
            auto* extras = static_cast<DumpExtras*>(param);
            if (input->CallbackType == ThreadCallback && extras->keep->count(input->Thread.ThreadId)) {
                struct
                {
                    LONG          exitStatus;
                    std::uint64_t teb;
                    std::uint64_t clientId[2];
                    std::uint64_t affinity;
                    LONG          priority;
                    LONG          basePriority;
                } basic{};
                using QueryFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
                static const auto query =
                    reinterpret_cast<QueryFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
                if (query && query(input->Thread.ThreadHandle, 0, &basic, sizeof(basic), nullptr) >= 0 && basic.teb)
                    extras->ranges.emplace_back(basic.teb, 0x2000u);
            }
            else if (input->CallbackType == MemoryCallback && extras->next < extras->ranges.size()) {
                output->MemoryBase = extras->ranges[extras->next].first;
                output->MemorySize = extras->ranges[extras->next].second;
                ++extras->next;
            }
            return TRUE;
        }

        bool WriteDump(HANDLE process, std::uint32_t pid, const fs::path& file, bool full, const std::wstring& comment,
                       const std::set<std::uint32_t>& keepThreads)
        {
            HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
                return false;
            const auto type = static_cast<MINIDUMP_TYPE>(
                full ? (MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                        MiniDumpWithFullMemoryInfo | MiniDumpWithProcessThreadData)
                     : (MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
                        MiniDumpWithUnloadedModules));
            MINIDUMP_USER_STREAM stream{};
            stream.Type = CommentStreamW;
            stream.BufferSize = static_cast<ULONG>((comment.size() + 1) * sizeof(wchar_t));
            stream.Buffer = const_cast<wchar_t*>(comment.c_str());
            MINIDUMP_USER_STREAM_INFORMATION streams{ 1, &stream };
            DumpExtras                    extras{ &keepThreads };
            MINIDUMP_CALLBACK_INFORMATION callback{ DumpCallback, &extras };
            const BOOL ok = MiniDumpWriteDump(process, pid, handle, type, nullptr, &streams, full ? nullptr : &callback);
            const DWORD error = GetLastError();
            CloseHandle(handle);
            if (!ok) {
                Log(L"MiniDumpWriteDump failed (error %lu)", error);
                std::error_code ec;
                fs::remove(file, ec);
            }
            return ok != FALSE;
        }

        void Retain(const fs::path& dir, std::uint32_t maxReports)
        {
            struct Entry
            {
                fs::path        stem;
                fs::file_time_type time;
            };
            std::vector<Entry> reports;
            std::error_code    ec;
            for (const auto& item : fs::directory_iterator(dir, ec)) {
                const fs::path& path = item.path();
                if (path.extension() != L".txt" || !StartsWithI(path.filename().wstring(), L"DumpDiver_"))
                    continue;
                reports.push_back({ path.parent_path() / path.stem(), item.last_write_time(ec) });
            }
            std::sort(reports.begin(), reports.end(), [](const Entry& a, const Entry& b) { return a.time > b.time; });
            // dumps are the large part: only the newest reports keep one
            constexpr std::size_t kMaxDumps = 5;
            for (std::size_t i = kMaxDumps; i < reports.size() && i < maxReports; ++i) {
                fs::path file = reports[i].stem;
                file += L".dmp";
                fs::remove(file, ec);
            }
            for (std::size_t i = maxReports; i < reports.size(); ++i) {
                for (const wchar_t* extension : { L".txt", L".json", L".dmp", L".ddsnap" }) {
                    fs::path file = reports[i].stem;
                    file += extension;
                    fs::remove(file, ec);
                }
            }
        }
    }

    bool LooksLikeGpuPoll(const std::uint8_t* code, std::size_t size)
    {
        const auto disp0xE8 = [&](std::size_t at) {
            return at + 4 <= size && code[at] == 0xE8 && code[at + 1] == 0x00 && code[at + 2] == 0x00 && code[at + 3] == 0x00;
        };
        for (std::size_t i = 0; i + 6 <= size; ++i) {
            // call qword ptr [reg+0E8h]: FF /2, mod=10, a register base (not 100 = SIB).
            if (code[i] == 0xFF && (code[i + 1] & 0xF8) == 0x90 && code[i + 1] != 0x94 && disp0xE8(i + 2))
                return true;
            // mov reg, qword ptr [reg+0E8h] (REX.W 8B, mod=10) followed by call reg (FF D0-D7).
            if ((code[i] & 0xF8) == 0x48 && i + 7 <= size && code[i + 1] == 0x8B && (code[i + 2] & 0xC0) == 0x80 &&
                (code[i + 2] & 0x07) != 0x04 && disp0xE8(i + 3)) {
                const std::uint8_t reg = (code[i + 2] >> 3) & 0x07;
                const bool         extended = (code[i] & 0x04) != 0;  // REX.R
                for (std::size_t j = i + 7; j + 2 <= size && j < i + 16; ++j) {
                    const bool plain = !extended && code[j] == 0xFF && code[j + 1] == (0xD0 | reg);
                    const bool rex = extended && code[j] == 0x41 && j + 3 <= size && code[j + 1] == 0xFF &&
                                     code[j + 2] == (0xD0 | reg);
                    if (plain || rex)
                        return true;
                }
            }
        }
        return false;
    }

    bool DecodeLock(std::uint64_t raw, std::uint32_t& owner, std::uint32_t& count, bool& readWrite)
    {
        owner = static_cast<std::uint32_t>(raw);
        const auto high = static_cast<std::uint32_t>(raw >> 32);
        if (owner == 0 || (owner & 3) != 0)
            return false;  // thread ids are multiples of 4
        if (high & 0x80000000u) {
            count = high & 0x7FFFFFFFu;
            readWrite = true;
            return count <= 0xFFFF;  // 0: the writer flag left behind by an unlock in the wrong order
        }
        count = high;
        readWrite = false;
        return count >= 1 && count <= 0xFFFF;
    }

    fs::path Capture(Target& target, CaptureReason reason, std::uint64_t heartbeatCountAtStart)
    {
        const Header& shared = *target.shared;
        Snapshot      snapshot;
        snapshot.game = target.game;
        snapshot.reason = reason;
        snapshot.processId = target.pid;
        snapshot.exePath = target.exePath;
        snapshot.qpcFrequency = shared.qpcFrequency ? shared.qpcFrequency : 1;
        snapshot.captureQpc = Qpc();
        snapshot.heartbeatQpc = shared.heartbeatQpc;
        snapshot.heartbeatThreadId = shared.heartbeatThreadId;
        snapshot.heartbeatAgeSeconds = snapshot.Seconds(snapshot.heartbeatQpc, snapshot.captureQpc);
        snapshot.stateFlags = shared.stateFlags;
        snapshot.loadStartQpc = shared.loadStartQpc;
        snapshot.runtimeVersion = shared.runtimeVersion;
        snapshot.taskTracking = shared.taskTracking != 0;
        snapshot.loadingWatch = shared.loadingWatch != 0;
        snapshot.loadingScreenSeconds = snapshot.Seconds(shared.loadingOpenQpc, snapshot.captureQpc);
        snapshot.mainThreadId = shared.mainThreadId;
        if (target.window && IsWindow(target.window)) {
            snapshot.windowHung = IsHungAppWindow(target.window) != FALSE;
            snapshot.windowForeground = GetForegroundWindow() == target.window;
            DWORD pid = 0;
            if (const DWORD tid = GetWindowThreadProcessId(target.window, &pid); tid && pid == target.pid)
                snapshot.mainThreadId = tid;
        }
        SYSTEMTIME now{};
        GetLocalTime(&now);
        snapshot.timeText = Format(L"%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                                   now.wSecond);
        Log(L"capture (%s): no heartbeat for %.1f s", ReasonName(reason), snapshot.heartbeatAgeSeconds);

        snapshot.modules = EnumerateModules(target.process, target.exePath);

        // Threads and their CPU counters at the start of the window.
        std::unordered_map<std::uint32_t, SystemThread> before;
        for (const auto& t : QuerySystemThreads(target.pid))
            before[t.tid] = t;
        std::vector<SampledThread> threads;
        std::vector<std::unordered_map<std::uint64_t, ValueStat>> values;
        for (const auto& [tid, info] : before) {
            HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
            if (!handle)
                continue;
            ThreadData data;
            data.tid = tid;
            data.name = ThreadName(handle);
            data.startAddress = ThreadStartAddress(handle);
            data.isMain = tid == snapshot.mainThreadId;
            data.ranHeartbeat = tid == snapshot.heartbeatThreadId;
            threads.push_back({ tid, handle, snapshot.threads.size(), false });
            snapshot.threads.push_back(std::move(data));
        }
        values.resize(snapshot.threads.size());
        std::sort(threads.begin(), threads.end(), [&](const SampledThread& a, const SampledThread& b) {
            return snapshot.threads[a.index].isMain > snapshot.threads[b.index].isMain;
        });

        std::wstring searchPath;
        {
            std::set<std::wstring> dirs;
            for (const auto& m : snapshot.modules) {
                if (m.kind == ModuleKind::kPlugin || m.kind == ModuleKind::kGraphicsInjector || m.kind == ModuleKind::kSelf)
                    dirs.insert(fs::path(m.realPath).parent_path().wstring());
            }
            for (const auto& dir : dirs)
                searchPath += dir + L";";
        }
        StackWalker walker;
        walker.Init(target.process, snapshot.modules, searchPath);

        // Sample every thread for the configured window. Threads that sit in the same wait are
        // sampled less often after the first rounds.
        const HelperSettings& settings = shared.settings;
        const std::uint32_t   hz = std::max<std::uint32_t>(settings.sampleHz, 1);
        const std::uint32_t   rounds = std::max<std::uint32_t>(settings.sampleMilliseconds * hz / 1000, 4);
        const double          interval = 1000.0 / hz;
        timeBeginPeriod(1);
        const std::uint64_t windowStart = Qpc();
        for (std::uint32_t round = 0; round < rounds; ++round) {
            const std::uint64_t roundStart = Qpc();
            for (auto& thread : threads) {
                if (thread.parked && round % 8 != 0)
                    continue;
                ThreadData& data = snapshot.threads[thread.index];
                Sample      sample;
                if (!walker.Sample(thread.handle, sample, &data.registers, &data.stackTop, kStackBytes, kMaxFrames))
                    continue;
                data.samples.push_back(std::move(sample));
                CountValues(data, values[thread.index]);
                if (round == 3 && data.samples.size() >= 4 && !data.isMain) {
                    const auto& s = data.samples;
                    thread.parked = std::all_of(s.begin(), s.end(), [&](const Sample& x) { return x.frames == s.front().frames; });
                }
            }
            const double spent = static_cast<double>(Qpc() - roundStart) * 1000.0 / static_cast<double>(snapshot.qpcFrequency);
            if (spent < interval)
                Sleep(static_cast<DWORD>(interval - spent));
        }
        const std::uint64_t windowEnd = Qpc();
        {
            std::size_t samples = 0;
            for (const auto& t : snapshot.threads)
                samples += t.samples.size();
            Log(L"sampled %zu threads, %zu stacks in %.2f s", snapshot.threads.size(), samples,
                static_cast<double>(windowEnd - windowStart) / static_cast<double>(snapshot.qpcFrequency));
        }
        timeEndPeriod(1);
        snapshot.sampleSeconds = static_cast<double>(windowEnd - windowStart) / static_cast<double>(snapshot.qpcFrequency);

        for (const auto& t : QuerySystemThreads(target.pid)) {
            const auto it = before.find(t.tid);
            if (it == before.end())
                continue;
            for (auto& data : snapshot.threads) {
                if (data.tid != t.tid)
                    continue;
                data.cpuTime100ns = t.cpuTime100ns >= it->second.cpuTime100ns ? t.cpuTime100ns - it->second.cpuTime100ns : 0;
                data.contextSwitches = t.contextSwitches >= it->second.contextSwitches
                                           ? t.contextSwitches - it->second.contextSwitches
                                           : 0;
                data.state = t.state;
                data.waitReason = t.waitReason;
            }
        }

        // Busy threads get a quick burst of extra samples: a DLL looping around game functions returns to
        // its own code only briefly, and those moments are what show the loop is the DLL's.
        for (auto& thread : threads) {
            ThreadData& data = snapshot.threads[thread.index];
            if (snapshot.sampleSeconds <= 0 || static_cast<double>(data.cpuTime100ns) / (snapshot.sampleSeconds * 1e7) < 0.3)
                continue;
            for (int i = 0; i < 200; ++i) {
                Sample sample;
                if (!walker.Sample(thread.handle, sample, &data.registers, &data.stackTop, kStackBytes, kMaxFrames))
                    break;
                data.samples.push_back(std::move(sample));
                CountValues(data, values[thread.index]);
            }
        }

        CollectWaitChains(snapshot);
        DialogSearch dialogSearch{ target.pid, &snapshot.dialogs };
        EnumWindows(&FindDialogs, reinterpret_cast<LPARAM>(&dialogSearch));

        CopyShared(shared, snapshot);
        // Names for every frame seen, and for the running tasks.
        for (const auto& t : snapshot.threads) {
            for (const auto& s : t.samples) {
                for (const std::uint64_t address : s.frames) {
                    if (!snapshot.symbols.count(address))
                        snapshot.symbols[address] = walker.Symbolize(address, snapshot.modules);
                }
            }
        }
        for (const auto& task : snapshot.tasks) {
            for (const std::uint64_t address : { task.runAddress, task.callerAddress }) {
                if (address && !snapshot.symbols.count(address))
                    snapshot.symbols[address] = walker.Symbolize(address, snapshot.modules);
            }
        }
        ScanLocks(target.process, snapshot);
        for (auto& record : snapshot.lockRecords) {
            ReadMemory(target.process, record.lockAddress, &record.current, sizeof(record.current));
            if (!snapshot.symbols.count(record.code))
                snapshot.symbols[record.code] = walker.Symbolize(record.code, snapshot.modules);
        }
        const std::uint64_t recordsStart = Qpc();
        ResolveRecords(target.process, snapshot, values);
        Log(L"%zu game records found in %.2f s; %zu engine locks held by other threads", snapshot.forms.size(),
            static_cast<double>(Qpc() - recordsStart) / static_cast<double>(snapshot.qpcFrequency), snapshot.locks.size());
        snapshot.recovered = (reason == CaptureReason::kFreeze || reason == CaptureReason::kStartup) &&
                             shared.heartbeatCount != heartbeatCountAtStart;

        const Verdict verdict = Analyze(snapshot);

        const fs::path dir = settings.outputDir;
        std::error_code ec;
        fs::create_directories(dir, ec);
        const fs::path stem = dir / Format(L"DumpDiver_%s_%04u%02u%02u_%02u%02u%02u_%s", GameName(target.game), now.wYear,
                                           now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, ReasonName(reason));
        const std::wstring summary = WriteReports(snapshot, verdict, stem);
        fs::path           raw = stem;
        raw += L".ddsnap";
        if (!SaveSnapshot(snapshot, raw))
            Log(L"could not write %s", raw.c_str());
        fs::path           report = stem;
        report += L".txt";
        Log(L"report: %s", report.c_str());
        Log(L"verdict: %s", summary.c_str());

        if (settings.writeDump && (!snapshot.recovered || settings.keepRecovered)) {
            fs::path dump = stem;
            dump += L".dmp";
            const std::set<std::uint32_t> keep = DumpThreads(snapshot, verdict);
            if (WriteDump(target.process, target.pid, dump, settings.fullMemoryDump != 0, summary, keep)) {
                std::error_code sizeError;
                Log(L"dump: %s (%.1f MB, thread blocks of %zu threads)", dump.c_str(),
                    static_cast<double>(fs::file_size(dump, sizeError)) / (1024.0 * 1024.0), keep.size());
            }
        }

        for (auto& thread : threads)
            CloseHandle(thread.handle);
        Retain(dir, settings.maxReports);
        return report;
    }
}
