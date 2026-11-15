// Real captures kept as tests. Each .ddsnap in the corpus folder is analyzed again and must still
// give what expect.tsv says:
//
//   capture.ddsnap <TAB> verdict kind or * <TAB> top suspect or - (none) <TAB> least confidence [<TAB> record text]
//
//   dd_replay_tests <corpus folder>

#include "helper/Analyze.h"
#include "helper/SnapshotFile.h"
#include "helper/Util.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace dd::helper;

namespace
{
    int Rank(const std::wstring& confidence)
    {
        return confidence == L"High" ? 3 : confidence == L"Medium" ? 2 : confidence == L"Low" ? 1 : 0;
    }

    std::vector<std::wstring> SplitTabs(const std::wstring& line)
    {
        std::vector<std::wstring> fields;
        std::wstringstream        in(line);
        for (std::wstring field; std::getline(in, field, L'\t');)
            fields.push_back(field);
        return fields;
    }

    std::wstring Widen(const std::string& text)
    {
        if (text.empty())
            return {};
        const int    size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
        return wide;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        std::printf("usage: dd_replay_tests <corpus folder>\n");
        return 2;
    }
    const fs::path dir = argv[1];
    std::ifstream  expect(dir / L"expect.tsv", std::ios::binary);
    if (!expect) {
        std::printf("no expect.tsv in the corpus folder\n");
        return 2;
    }

    int                   failed = 0, total = 0;
    std::set<std::wstring> listed;
    for (std::string raw; std::getline(expect, raw);) {
        if (!raw.empty() && raw.back() == '\r')
            raw.pop_back();
        if (raw.empty() || raw[0] == '#')
            continue;
        const auto fields = SplitTabs(Widen(raw));
        if (fields.size() < 4) {
            std::printf("bad line: %s\n", raw.c_str());
            ++failed;
            continue;
        }
        ++total;
        listed.insert(fields[0]);
        Snapshot snapshot;
        if (!LoadSnapshot(dir / fields[0], snapshot)) {
            wprintf(L"FAIL %s: cannot read the capture\n", fields[0].c_str());
            ++failed;
            continue;
        }
        const Verdict v = Analyze(snapshot);
        const std::wstring top = v.suspects.empty() ? L"-" : snapshot.modules[static_cast<std::size_t>(v.suspects.front().module)].name;
        const std::wstring confidence = v.suspects.empty() ? L"" : ConfidenceName(v.suspects.front().confidence);
        bool ok = (fields[1] == L"*" || fields[1] == v.kind) && top == fields[2];
        if (fields[2] != L"-")
            ok = ok && Rank(confidence) >= Rank(fields[3]);
        if (fields.size() >= 5 && !fields[4].empty()) {
            bool found = false;
            for (const auto& lead : v.records) {
                const FormInfo& form = snapshot.forms[static_cast<std::size_t>(lead.form)];
                const std::wstring text = form.className + L" " + Format(L"%08X", form.formId);
                found |= text.find(fields[4]) != std::wstring::npos;
            }
            ok = ok && found;
        }
        wprintf(L"%s %s: %s, top suspect %s %s (expected %s, %s %s)\n", ok ? L"pass" : L"FAIL", fields[0].c_str(),
                v.kind.c_str(), top.c_str(), confidence.c_str(), fields[1].c_str(), fields[2].c_str(), fields[3].c_str());
        if (!ok)
            ++failed;
    }
    std::error_code ec;
    for (const auto& item : fs::directory_iterator(dir, ec)) {
        if (item.path().extension() == L".ddsnap" && !listed.count(item.path().filename().wstring()))
            wprintf(L"note: %s has no line in expect.tsv\n", item.path().filename().c_str());
    }
    std::printf("%d of %d captures failed\n", failed, total);
    return failed;
}
