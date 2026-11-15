#pragma once

#include "helper/Analyze.h"
#include "helper/Snapshot.h"

#include <filesystem>
#include <string>

namespace dd::helper
{
    // Writes <stem>.txt (for people) and <stem>.json (for tools). Returns a one-line summary.
    std::wstring WriteReports(const Snapshot& snapshot, const Verdict& verdict, const std::filesystem::path& stem);

    std::wstring ReportText(const Snapshot& snapshot, const Verdict& verdict);
    std::string  ReportJson(const Snapshot& snapshot, const Verdict& verdict);
}
