#pragma once
// Turns the folders a change journal reports into the marks of an
// incremental scan (ScanOptions::marks).

#include "scan/scanner.hpp"
#include "scan/snapshot.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cmac {

// Absolute folder paths, as FSEvents reports them (canonical, "/Users/...").
struct ChangedFolders final {
    // Folders whose own entries changed: they are read again.
    std::vector<std::string> relist{};
    // Folders to read again with everything below them.
    std::vector<std::string> rescan{};
};

struct ReusePlan final {
    // One per previous node.
    std::vector<ReuseMark> marks{};
    std::uint64_t relisted{0};
    std::uint64_t rescanned{0};
};

// Marks every directory of previous: reuse unless a changed folder (or its
// nearest ancestor that existed) says otherwise; rescan beats relist.
// Directories that were refused or failed are always rescanned, so a grant
// of Full Disk Access shows on the next refresh, and a directory holding a
// file whose allocation has not settled (APFS preallocation, released with
// no event) is always relisted.
[[nodiscard]] ReusePlan plan_reuse(const Snapshot& previous, const ChangedFolders& changes);

} // namespace cmac
