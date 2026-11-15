#include "helper/SnapshotFile.h"

#include <cstdio>
#include <cstring>
#include <type_traits>

namespace dd::helper
{
    namespace
    {
        constexpr char          kMagic[8] = { 'D', 'D', 'S', 'N', 'A', 'P', '\r', '\n' };
        constexpr std::uint32_t kVersion = 3;  // 2: FormInfo::problem, 3: lock owners that exited, lock records
        thread_local std::uint32_t t_readVersion = kVersion;

        class Writer
        {
        public:
            template <class T>
            void Pod(const T& value)
            {
                static_assert(std::is_trivially_copyable_v<T>);
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
                data.insert(data.end(), bytes, bytes + sizeof(T));
            }
            void Text(const std::wstring& text)
            {
                Pod(static_cast<std::uint32_t>(text.size()));
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(text.data());
                data.insert(data.end(), bytes, bytes + text.size() * sizeof(wchar_t));
            }
            void Bytes(const std::vector<std::uint8_t>& bytes)
            {
                Pod(static_cast<std::uint32_t>(bytes.size()));
                data.insert(data.end(), bytes.begin(), bytes.end());
            }
            template <class T, class F>
            void List(const std::vector<T>& items, F&& each)
            {
                Pod(static_cast<std::uint32_t>(items.size()));
                for (const auto& item : items)
                    each(item);
            }

            std::vector<std::uint8_t> data;
        };

        class Reader
        {
        public:
            explicit Reader(const std::vector<std::uint8_t>& bytes) :
                data_(bytes)
            {}

            template <class T>
            T Pod()
            {
                static_assert(std::is_trivially_copyable_v<T>);
                T value{};
                if (!Need(sizeof(T)))
                    return value;
                std::memcpy(&value, data_.data() + pos_, sizeof(T));
                pos_ += sizeof(T);
                return value;
            }
            std::wstring Text()
            {
                const auto   count = Pod<std::uint32_t>();
                std::wstring text;
                if (!Need(static_cast<std::size_t>(count) * sizeof(wchar_t)))
                    return text;
                text.resize(count);
                std::memcpy(text.data(), data_.data() + pos_, count * sizeof(wchar_t));
                pos_ += count * sizeof(wchar_t);
                return text;
            }
            std::vector<std::uint8_t> Bytes()
            {
                const auto                count = Pod<std::uint32_t>();
                std::vector<std::uint8_t> bytes;
                if (!Need(count))
                    return bytes;
                bytes.assign(data_.begin() + static_cast<std::ptrdiff_t>(pos_), data_.begin() + static_cast<std::ptrdiff_t>(pos_ + count));
                pos_ += count;
                return bytes;
            }
            template <class T, class F>
            void List(std::vector<T>& items, F&& each)
            {
                const auto count = Pod<std::uint32_t>();
                if (!ok_ || count > data_.size())  // every item takes at least one byte
                    return Fail();
                items.resize(count);
                for (auto& item : items) {
                    each(item);
                    if (!ok_)
                        return;
                }
            }
            bool ok() const { return ok_; }
            bool AtEnd() const { return pos_ == data_.size(); }

        private:
            bool Need(std::size_t bytes)
            {
                if (!ok_ || data_.size() - pos_ < bytes) {
                    Fail();
                    return false;
                }
                return true;
            }
            void Fail() { ok_ = false; }

            const std::vector<std::uint8_t>& data_;
            std::size_t                      pos_ = 0;
            bool                             ok_ = true;
        };

        // One function describes the layout for both directions.
        template <class IO>
        void Fields(IO& io, Snapshot& s);

        template <class T>
        concept Plain = std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_same_v<T, Registers>;

        template <Plain T>
        void Field(Writer& w, T& value) { w.Pod(value); }
        template <Plain T>
        void Field(Reader& r, T& value) { value = r.template Pod<T>(); }
        void Field(Writer& w, std::wstring& text) { w.Text(text); }
        void Field(Reader& r, std::wstring& text) { text = r.Text(); }
        void Field(Writer& w, std::vector<std::uint8_t>& bytes) { w.Bytes(bytes); }
        void Field(Reader& r, std::vector<std::uint8_t>& bytes) { bytes = r.Bytes(); }
        template <class T>
        void Field(Writer& w, std::vector<T>& items)
        {
            w.List(items, [&](const T& item) { Field(w, const_cast<T&>(item)); });
        }
        template <class T>
        void Field(Reader& r, std::vector<T>& items)
        {
            r.List(items, [&](T& item) { Field(r, item); });
        }

        template <class IO> void Field(IO& io, Module& m)
        {
            Field(io, m.name); Field(io, m.path); Field(io, m.realPath); Field(io, m.modName); Field(io, m.version);
            Field(io, m.base); Field(io, m.size); Field(io, m.kind);
        }
        template <class IO> void Field(IO& io, Sample& s) { Field(io, s.frames); }
        template <class IO> void Field(IO& io, FormRef& f) { Field(io, f.form); Field(io, f.samples); Field(io, f.offset); }
        template <class IO> void Field(IO& io, ThreadData& t)
        {
            Field(io, t.tid); Field(io, t.name); Field(io, t.startAddress); Field(io, t.cpuTime100ns);
            Field(io, t.contextSwitches); Field(io, t.state); Field(io, t.waitReason); Field(io, t.isMain);
            Field(io, t.ranHeartbeat); Field(io, t.samples); Field(io, t.registers); Field(io, t.stackTop); Field(io, t.forms);
        }
        template <class IO> void Field(IO& io, WctNode& n)
        {
            Field(io, n.type); Field(io, n.processId); Field(io, n.threadId); Field(io, n.waitTime);
            Field(io, n.contextSwitches); Field(io, n.status); Field(io, n.objectName);
        }
        template <class IO> void Field(IO& io, WaitChain& c) { Field(io, c.threadId); Field(io, c.cycle); Field(io, c.nodes); }
        template <class IO> void Field(IO& io, Dialog& d)
        {
            Field(io, d.threadId); Field(io, d.title); Field(io, d.text); Field(io, d.className); Field(io, d.visible);
        }
        // Fields added in a later version: read only from files that have them.
        template <class IO, class T>
        void Since(IO& io, std::uint32_t version, T& value)
        {
            if constexpr (std::is_same_v<IO, Reader>) {
                if (t_readVersion < version)
                    return;
            }
            Field(io, value);
        }

        template <class IO> void Field(IO& io, LockHit& l)
        {
            Field(io, l.waiterTid); Field(io, l.lockAddress); Field(io, l.ownerTid); Field(io, l.count);
            Field(io, l.readWrite); Field(io, l.inGameData); Field(io, l.foundIn);
            Since(io, 3, l.ownerAlive); Since(io, 3, l.broken);
        }
        template <class IO> void Field(IO& io, LockRecordInfo& r)
        {
            Field(io, r.qpc); Field(io, r.lockAddress); Field(io, r.code); Field(io, r.threadId); Field(io, r.kind);
            Field(io, r.lockWord); Field(io, r.messageType); Field(io, r.current);
        }
        template <class IO> void Field(IO& io, TaskInFlight& t)
        {
            Field(io, t.threadId); Field(io, t.startQpc); Field(io, t.runAddress); Field(io, t.callerAddress);
            Field(io, t.queuedQpc); Field(io, t.kind);
        }
        template <class IO> void Field(IO& io, EventRecord& e) { Field(io, e.qpc); Field(io, e.type); Field(io, e.threadId); Field(io, e.a); }
        template <class IO> void Field(IO& io, FormInfo& f)
        {
            Field(io, f.address); Field(io, f.formId); Field(io, f.formType); Field(io, f.className); Field(io, f.files);
            Field(io, f.baseForm); Field(io, f.cell);
            Since(io, 2, f.problem);
        }

        // Maps are written as key/value lists.
        struct SymbolEntry
        {
            std::uint64_t address = 0;
            std::wstring  text;
        };
        template <class IO> void Field(IO& io, SymbolEntry& e) { Field(io, e.address); Field(io, e.text); }
        struct PollEntry
        {
            std::uint64_t address = 0;
            bool          polls = false;
        };
        template <class IO> void Field(IO& io, PollEntry& e) { Field(io, e.address); Field(io, e.polls); }

        template <class IO>
        void Fields(IO& io, Snapshot& s)
        {
            Field(io, s.game); Field(io, s.reason); Field(io, s.processId); Field(io, s.mainThreadId);
            Field(io, s.heartbeatThreadId); Field(io, s.runtimeVersion); Field(io, s.stateFlags); Field(io, s.qpcFrequency);
            Field(io, s.captureQpc); Field(io, s.heartbeatQpc); Field(io, s.loadStartQpc); Field(io, s.heartbeatAgeSeconds);
            Field(io, s.sampleSeconds); Field(io, s.windowHung); Field(io, s.windowForeground); Field(io, s.recovered);
            Field(io, s.taskTracking); Field(io, s.loadingWatch); Field(io, s.loadingScreenSeconds); Field(io, s.exePath);
            Field(io, s.timeText);
            Field(io, s.modules); Field(io, s.threads); Field(io, s.chains); Field(io, s.dialogs); Field(io, s.locks);
            Field(io, s.tasks); Field(io, s.events); Field(io, s.forms);
            Since(io, 3, s.lockWatch); Since(io, 3, s.lockRecords);
        }
    }

    bool SaveSnapshot(const Snapshot& snapshot, const std::filesystem::path& file)
    {
        Writer w;
        w.data.insert(w.data.end(), std::begin(kMagic), std::end(kMagic));
        w.Pod(kVersion);
        Fields(w, const_cast<Snapshot&>(snapshot));
        std::vector<SymbolEntry> symbols;
        for (const auto& [address, text] : snapshot.symbols)
            symbols.push_back({ address, text });
        Field(w, symbols);
        std::vector<PollEntry> polls;
        for (const auto& [address, value] : snapshot.gpuPollSites)
            polls.push_back({ address, value });
        Field(w, polls);

        FILE* f = _wfsopen(file.c_str(), L"wb", _SH_DENYWR);
        if (!f)
            return false;
        const bool ok = fwrite(w.data.data(), 1, w.data.size(), f) == w.data.size();
        fclose(f);
        return ok;
    }

    bool LoadSnapshot(const std::filesystem::path& file, Snapshot& snapshot)
    {
        FILE* f = _wfsopen(file.c_str(), L"rb", _SH_DENYWR);
        if (!f)
            return false;
        std::vector<std::uint8_t> bytes;
        std::uint8_t              chunk[65536];
        for (std::size_t n; (n = fread(chunk, 1, sizeof(chunk), f)) > 0;)
            bytes.insert(bytes.end(), chunk, chunk + n);
        fclose(f);
        if (bytes.size() < sizeof(kMagic) + 4 || std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0)
            return false;

        std::vector<std::uint8_t> body(bytes.begin() + sizeof(kMagic), bytes.end());
        Reader                    r(body);
        t_readVersion = r.Pod<std::uint32_t>();
        if (t_readVersion < 1 || t_readVersion > kVersion)
            return false;
        Snapshot s;
        Fields(r, s);
        std::vector<SymbolEntry> symbols;
        Field(r, symbols);
        std::vector<PollEntry> polls;
        Field(r, polls);
        if (!r.ok() || !r.AtEnd())
            return false;
        for (auto& entry : symbols)
            s.symbols[entry.address] = std::move(entry.text);
        for (const auto& entry : polls)
            s.gpuPollSites[entry.address] = entry.polls;
        snapshot = std::move(s);
        return true;
    }
}
