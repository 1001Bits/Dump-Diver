#include "helper/Report.h"

#include "helper/Process.h"
#include "helper/Util.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace dd::helper
{
    namespace
    {
        const wchar_t* GameTitle(Game game)
        {
            return game == Game::kSkyrim ? L"Skyrim" : game == Game::kFallout4 ? L"Fallout 4" : L"unknown game";
        }

        const wchar_t* ReasonText(CaptureReason reason)
        {
            switch (reason) {
            case CaptureReason::kStartup:
                return L"the game never reached its main loop";
            case CaptureReason::kManual:
                return L"manual capture";
            case CaptureReason::kEndlessLoading:
                return L"endless loading screen";
            default:
                return L"freeze";
            }
        }

        const wchar_t* EventName(EventType type)
        {
            switch (type) {
            case EventType::kPluginLoaded:
                return L"Dump Diver loaded";
            case EventType::kHelperStarted:
                return L"helper started";
            case EventType::kPostLoad:
                return L"all plugins loaded";
            case EventType::kInputLoaded:
                return L"input loaded";
            case EventType::kDataLoaded:
                return L"game data loaded";
            case EventType::kNewGame:
                return L"new game";
            case EventType::kPreLoadGame:
                return L"loading a save";
            case EventType::kPostLoadGame:
                return L"save loaded";
            case EventType::kSaveGame:
                return L"saving";
            case EventType::kPostSaveGame:
                return L"saved";
            case EventType::kDeleteGame:
                return L"save deleted";
            case EventType::kFirstHeartbeat:
                return L"main loop running";
            case EventType::kLoadingScreenOpened:
                return L"loading screen opened";
            case EventType::kLoadingScreenClosed:
                return L"loading screen closed";
            default:
                return L"?";
            }
        }

        std::wstring ModuleLabel(const Snapshot& s, int module)
        {
            if (module < 0)
                return L"unknown code";
            const Module& m = s.modules[static_cast<std::size_t>(module)];
            std::wstring  label = m.name;
            if (!m.modName.empty())
                label += L" [" + m.modName + L"]";
            return label;
        }

        std::wstring Sym(const Snapshot& s, std::uint64_t address)
        {
            const auto it = s.symbols.find(address);
            return it != s.symbols.end() ? it->second : Hex(address);
        }

        std::wstring VersionText(std::uint32_t packed)
        {
            if (!packed)
                return L"?";
            return Format(L"%u.%u.%u", packed >> 24, (packed >> 16) & 0xFF, (packed >> 4) & 0xFFF);
        }

        std::wstring FormText(const Snapshot& s, int index)
        {
            if (index < 0 || static_cast<std::size_t>(index) >= s.forms.size())
                return L"?";
            const FormInfo& f = s.forms[static_cast<std::size_t>(index)];
            std::wstring    text = f.className + Format(L" %08X", f.formId);
            if (!f.files.empty()) {
                text += L" from " + f.files.front();
                if (f.files.size() > 1)
                    text += L", last changed by " + f.files.back();
            } else if ((f.formId >> 24) == 0xFF) {
                text += L" (created during play)";
            }
            return text;
        }

        void Stack(std::wostringstream& out, const Snapshot& s, const std::vector<std::uint64_t>& frames, std::size_t max,
                   const wchar_t* indent)
        {
            for (std::size_t i = 0; i < frames.size() && i < max; ++i)
                out << indent << Format(L"#%02zu ", i) << Sym(s, frames[i]) << L"\n";
            if (frames.size() > max)
                out << indent << L"... " << (frames.size() - max) << L" more\n";
        }

        void ThreadBlock(std::wostringstream& out, const Snapshot& s, const ThreadSummary& ts, std::size_t frames)
        {
            const ThreadData* t = s.Thread(ts.tid);
            out << L"  Thread " << ts.tid;
            if (t && !t->name.empty())
                out << L" \"" << t->name << L"\"";
            if (ts.tid == s.mainThreadId)
                out << L" (main thread)";
            out << L"\n";
            out << L"    started by " << ModuleLabel(s, ts.startModule) << Format(L", CPU %.0f%%, %.0f switches/s, ", ts.cpuPercent, ts.switchesPerSecond);
            if (ts.running)
                out << L"running";
            else
                out << L"waiting (" << WaitKindName(ts.wait) << L", " << ts.waitFunction << L")";
            out << Format(L", %zu samples\n", t ? t->samples.size() : 0);
            out << L"    mostly in " << ModuleLabel(s, ts.attributed) << Format(L" (%.0f%% of samples)\n", ts.attributedShare * 100);
            if (t && !t->forms.empty()) {
                out << L"    records it was working on:\n";
                const double count = static_cast<double>(std::max<std::size_t>(t->samples.size(), 1));
                for (std::size_t i = 0; i < t->forms.size() && i < 6; ++i) {
                    const FormRef& ref = t->forms[i];
                    const std::wstring where = ref.offset ? Format(L"stack +%#x", ref.offset) : std::wstring(L"register");
                    out << L"      " << FormText(s, ref.form)
                        << Format(L"  (%.0f%% of samples, %s)\n", ref.samples * 100.0 / count, where.c_str());
                }
            }
            Stack(out, s, ts.stack, frames, L"      ");
        }

        // --- JSON -------------------------------------------------------------------------

        std::string Escape(std::wstring_view text)
        {
            std::string out;
            for (const char c : Utf8(text)) {
                switch (c) {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buffer[8];
                        snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                        out += buffer;
                    } else {
                        out += c;
                    }
                }
            }
            return out;
        }

        std::string Q(std::wstring_view text) { return "\"" + Escape(text) + "\""; }
    }

    std::wstring ReportText(const Snapshot& s, const Verdict& v)
    {
        std::wostringstream out;
        out << L"Dump Diver " << Wide(DD_VERSION_STRING) << L" - " << GameTitle(s.game) << L" freeze report\n";
        out << L"Captured " << s.timeText << L" (" << ReasonText(s.reason) << L")";
        if (s.heartbeatQpc && s.heartbeatAgeSeconds >= 1.0)
            out << Format(L", no heartbeat for %.1f s", s.heartbeatAgeSeconds);
        out << L"\n";
        out << L"Game: " << s.exePath << L", runtime " << VersionText(s.runtimeVersion) << L"\n\n";

        out << L"VERDICT: " << v.title << L"   [confidence: " << ConfidenceName(v.confidence) << L"]\n";
        out << v.summary << L"\n\n";

        out << L"Suspects\n";
        if (v.suspects.empty())
            out << L"  No mod can be named from this capture.\n";
        for (std::size_t i = 0; i < v.suspects.size(); ++i) {
            const Suspect& suspect = v.suspects[i];
            const Module&  m = s.modules[static_cast<std::size_t>(suspect.module)];
            out << L"  " << (i + 1) << L". " << m.name;
            if (!m.modName.empty())
                out << L"  (mod: " << m.modName << L")";
            out << L"  - " << ConfidenceName(suspect.confidence) << L"\n";
            for (const auto& reason : suspect.reasons)
                out << L"     - " << reason << L"\n";
        }
        out << L"\n";

        if (!v.records.empty()) {
            out << L"Records involved (plugin records the stuck code was working on)\n";
            for (std::size_t i = 0; i < v.records.size(); ++i) {
                const RecordLead& lead = v.records[i];
                const FormInfo&   f = s.forms[static_cast<std::size_t>(lead.form)];
                out << L"  " << (i + 1) << L". " << FormText(s, lead.form) << L"  - " << ConfidenceName(lead.confidence) << L"\n";
                if (f.baseForm >= 0)
                    out << L"     base object: " << FormText(s, f.baseForm) << L"\n";
                if (f.cell >= 0)
                    out << L"     cell: " << FormText(s, f.cell) << L"\n";
                if (f.files.size() > 2) {
                    out << L"     in:";
                    for (const auto& file : f.files)
                        out << L" " << file;
                    out << L"\n";
                }
                out << L"     - " << lead.why << L"\n";
            }
            out << L"\n";
        }

        if (!v.advice.empty()) {
            out << L"What to try\n";
            for (const auto& advice : v.advice)
                out << L"  - " << advice << L"\n";
            out << L"\n";
        }

        out << L"Evidence\n";
        for (const auto& line : v.evidence)
            out << L"  - " << line << L"\n";
        out << L"\n";
        out << L"Confidence: High = the evidence ties the freeze to that DLL directly. Medium = a strong link, but\n"
               L"confirm it by disabling the mod. Low = a lead only. A freeze is only proven when it stops with the\n"
               L"mod disabled.\n\n";

        auto summaryOf = [&](std::uint32_t tid) -> const ThreadSummary* {
            for (const auto& ts : v.threads) {
                if (ts.tid == tid)
                    return &ts;
            }
            return nullptr;
        };

        out << L"Threads involved\n";
        std::set<std::uint32_t> shown;
        for (const std::uint32_t tid : v.path) {
            if (!shown.insert(tid).second)
                continue;
            if (const ThreadSummary* ts = summaryOf(tid))
                ThreadBlock(out, s, *ts, 24);
        }
        out << L"\n";

        std::vector<const ThreadSummary*> busy;
        for (const auto& ts : v.threads) {
            if (ts.running && !shown.count(ts.tid))
                busy.push_back(&ts);
        }
        std::sort(busy.begin(), busy.end(), [](auto* a, auto* b) { return a->cpuPercent > b->cpuPercent; });
        if (!busy.empty()) {
            out << L"Other busy threads\n";
            for (const ThreadSummary* ts : busy)
                ThreadBlock(out, s, *ts, 12);
            out << L"\n";
        }

        if (!s.tasks.empty()) {
            out << L"Tasks still running (queued through " << (s.game == Game::kFallout4 ? L"F4SE" : L"SKSE") << L")\n";
            for (const auto& task : s.tasks) {
                out << Format(L"  thread %u: %s, queued by %s, running for %.1f s\n", task.threadId, Sym(s, task.runAddress).c_str(),
                              Sym(s, task.callerAddress).c_str(), s.Seconds(task.startQpc, s.captureQpc));
            }
            out << L"\n";
        }

        if (!s.locks.empty()) {
            out << L"Engine locks held by other threads\n";
            for (const auto& lock : s.locks) {
                out << Format(L"  thread %u waits for %s lock %s (%s) held by thread %u, count %u%s%s%s\n", lock.waiterTid,
                              lock.readWrite ? L"read/write" : L"spin", Sym(s, lock.lockAddress).c_str(), lock.foundIn.c_str(),
                              lock.ownerTid, lock.count, lock.inGameData ? L"" : L" (heap; less certain)",
                              lock.ownerAlive ? L"" : L", owner thread has exited", lock.broken ? L", broken state" : L"");
            }
            out << L"\n";
        }

        if (!s.lockRecords.empty()) {
            out << L"Engine locks left held (seen by the game plugin)\n";
            for (const auto& r : s.lockRecords) {
                const bool handler = r.kind == LockRecordKind::kLeftByHandler;
                out << Format(L"  %s: thread %u, lock %s (then %08x, now %016llx), %s %s\n",
                              handler ? L"after a message handler" : L"at thread exit", r.threadId,
                              Sym(s, r.lockAddress).c_str(), r.lockWord, static_cast<unsigned long long>(r.current),
                              handler ? L"handler" : L"thread start", Sym(s, r.code).c_str());
            }
            out << L"\n";
        }

        if (!s.chains.empty()) {
            out << L"Windows wait chains\n";
            for (const auto& chain : s.chains) {
                out << L"  " << chain.threadId << (chain.cycle ? L" (CYCLE)" : L"") << L":";
                for (std::size_t i = 1; i < chain.nodes.size(); ++i) {
                    const auto& node = chain.nodes[i];
                    if (node.type == WctObject::kThread)
                        out << L" -> thread " << node.threadId << (node.processId != s.processId ? L" (other process)" : L"");
                    else
                        out << L" -> " << static_cast<int>(node.type) << (node.objectName.empty() ? L"" : L" " + node.objectName);
                }
                out << L"\n";
            }
            out << L"\n";
        }

        if (!s.dialogs.empty()) {
            out << L"Dialog windows\n";
            for (const auto& d : s.dialogs)
                out << L"  thread " << d.threadId << (d.visible ? L"" : L" (hidden)") << L": \"" << d.title << L"\" " << d.text << L"\n";
            out << L"\n";
        }

        if (!s.events.empty()) {
            out << L"Recent events (seconds before the capture)\n";
            const std::size_t start = s.events.size() > 30 ? s.events.size() - 30 : 0;
            for (std::size_t i = start; i < s.events.size(); ++i) {
                const auto& e = s.events[i];
                out << Format(L"  %9.1f  %s\n", s.Seconds(e.qpc, s.captureQpc), EventName(e.type));
            }
            out << L"\n";
        }

        out << L"All threads\n";
        out << L"     tid  cpu%  sw/s  state    mostly in                          started by\n";
        for (const auto& ts : v.threads) {
            const std::wstring state = ts.running ? L"running" : WaitKindName(ts.wait);
            out << Format(L"  %6u  %4.0f  %4.0f  %-7.7s  %-34.34s %s\n", ts.tid, ts.cpuPercent, ts.switchesPerSecond, state.c_str(),
                          ModuleLabel(s, ts.attributed).c_str(), ModuleLabel(s, ts.startModule).c_str());
        }
        out << L"\n";

        out << L"Modules\n";
        for (const auto& m : s.modules) {
            out << Format(L"  %-40.40s %-17S %-14s %s", m.name.c_str(), KindName(m.kind), m.version.c_str(), Hex(m.base).c_str());
            if (!m.modName.empty())
                out << L"  mod: " << m.modName;
            out << L"\n";
        }
        return out.str();
    }

    std::string ReportJson(const Snapshot& s, const Verdict& v)
    {
        std::ostringstream out;
        out << "{\n";
        out << "  \"tool\": \"Dump Diver\",\n  \"version\": \"" << DD_VERSION_STRING << "\",\n";
        out << "  \"game\": " << Q(GameTitle(s.game)) << ",\n";
        out << "  \"runtime\": " << Q(VersionText(s.runtimeVersion)) << ",\n";
        out << "  \"captured\": " << Q(s.timeText) << ",\n";
        out << "  \"reason\": " << Q(ReasonText(s.reason)) << ",\n";
        out << "  \"heartbeat_age_seconds\": " << s.heartbeatAgeSeconds << ",\n";
        out << "  \"loading_screen_seconds\": " << s.loadingScreenSeconds << ",\n";
        out << "  \"recovered\": " << (s.recovered ? "true" : "false") << ",\n";
        out << "  \"verdict\": {\n";
        out << "    \"kind\": " << Q(v.kind) << ",\n";
        out << "    \"title\": " << Q(v.title) << ",\n";
        out << "    \"summary\": " << Q(v.summary) << ",\n";
        out << "    \"confidence\": " << Q(ConfidenceName(v.confidence)) << ",\n";
        out << "    \"suspects\": [";
        for (std::size_t i = 0; i < v.suspects.size(); ++i) {
            const auto&   suspect = v.suspects[i];
            const Module& m = s.modules[static_cast<std::size_t>(suspect.module)];
            out << (i ? "," : "") << "\n      {\"module\": " << Q(m.name) << ", \"mod\": " << Q(m.modName)
                << ", \"path\": " << Q(m.realPath) << ", \"confidence\": " << Q(ConfidenceName(suspect.confidence))
                << ", \"reasons\": [";
            for (std::size_t r = 0; r < suspect.reasons.size(); ++r)
                out << (r ? ", " : "") << Q(suspect.reasons[r]);
            out << "]}";
        }
        out << "\n    ],\n    \"records\": [";
        auto formJson = [&](int index) {
            std::ostringstream f;
            if (index < 0) {
                f << "null";
                return f.str();
            }
            const FormInfo& form = s.forms[static_cast<std::size_t>(index)];
            f << "{\"class\": " << Q(form.className) << ", \"form_id\": " << Q(Format(L"%08X", form.formId)) << ", \"plugins\": [";
            for (std::size_t i = 0; i < form.files.size(); ++i)
                f << (i ? ", " : "") << Q(form.files[i]);
            f << "]}";
            return f.str();
        };
        for (std::size_t i = 0; i < v.records.size(); ++i) {
            const RecordLead& lead = v.records[i];
            const FormInfo&   form = s.forms[static_cast<std::size_t>(lead.form)];
            out << (i ? "," : "") << "\n      {\"record\": " << formJson(lead.form) << ", \"base\": " << formJson(form.baseForm)
                << ", \"cell\": " << formJson(form.cell) << ", \"confidence\": " << Q(ConfidenceName(lead.confidence))
                << ", \"thread\": " << lead.threadId << ", \"why\": " << Q(lead.why) << "}";
        }
        out << "\n    ],\n    \"evidence\": [";
        for (std::size_t i = 0; i < v.evidence.size(); ++i)
            out << (i ? "," : "") << "\n      " << Q(v.evidence[i]);
        out << "\n    ],\n    \"path\": [";
        for (std::size_t i = 0; i < v.path.size(); ++i)
            out << (i ? ", " : "") << v.path[i];
        out << "]\n  },\n";
        out << "  \"main_thread\": " << s.mainThreadId << ",\n";
        out << "  \"threads\": [";
        for (std::size_t i = 0; i < v.threads.size(); ++i) {
            const auto& ts = v.threads[i];
            out << (i ? "," : "") << "\n    {\"tid\": " << ts.tid << ", \"cpu_percent\": " << ts.cpuPercent
                << ", \"running\": " << (ts.running ? "true" : "false") << ", \"polling\": " << (ts.polling ? "true" : "false")
                << ", \"waiting_share\": " << ts.waitingShare << ", \"wait\": " << Q(ts.waitFunction)
                << ", \"mostly_in\": " << Q(ts.attributed >= 0 ? s.modules[static_cast<std::size_t>(ts.attributed)].name : L"")
                << ", \"started_by\": " << Q(ts.startModule >= 0 ? s.modules[static_cast<std::size_t>(ts.startModule)].name : L"")
                << ", \"stack\": [";
            for (std::size_t f = 0; f < ts.stack.size() && f < 32; ++f)
                out << (f ? ", " : "") << Q(Sym(s, ts.stack[f]));
            out << "]}";
        }
        out << "\n  ],\n  \"modules\": [";
        for (std::size_t i = 0; i < s.modules.size(); ++i) {
            const auto& m = s.modules[i];
            out << (i ? "," : "") << "\n    {\"name\": " << Q(m.name) << ", \"kind\": \"" << KindName(m.kind)
                << "\", \"mod\": " << Q(m.modName) << ", \"version\": " << Q(m.version) << ", \"path\": " << Q(m.realPath) << "}";
        }
        out << "\n  ]\n}\n";
        return out.str();
    }

    std::wstring WriteReports(const Snapshot& snapshot, const Verdict& verdict, const fs::path& stem)
    {
        const std::wstring text = ReportText(snapshot, verdict);
        const std::string  json = ReportJson(snapshot, verdict);

        fs::path txt = stem;
        txt += L".txt";
        if (FILE* f = _wfsopen(txt.c_str(), L"wb", _SH_DENYWR)) {
            const std::string utf8 = Utf8(text);
            fwrite("\xEF\xBB\xBF", 1, 3, f);
            fwrite(utf8.data(), 1, utf8.size(), f);
            fclose(f);
        }
        fs::path js = stem;
        js += L".json";
        if (FILE* f = _wfsopen(js.c_str(), L"wb", _SH_DENYWR)) {
            fwrite(json.data(), 1, json.size(), f);
            fclose(f);
        }

        std::wstring line = verdict.title + L" [" + ConfidenceName(verdict.confidence) + L"]";
        if (!verdict.suspects.empty())
            line += L": " + ModuleLabel(snapshot, verdict.suspects.front().module);
        return line;
    }
}
