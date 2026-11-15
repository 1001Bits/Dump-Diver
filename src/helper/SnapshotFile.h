#pragma once

// A capture's raw data on disk (.ddsnap), so the analysis can be run again later: on real freezes
// kept as tests, and on files players send in, with a newer Dump Diver.

#include "helper/Snapshot.h"

#include <filesystem>

namespace dd::helper
{
    bool SaveSnapshot(const Snapshot& snapshot, const std::filesystem::path& file);
    bool LoadSnapshot(const std::filesystem::path& file, Snapshot& snapshot);
}
