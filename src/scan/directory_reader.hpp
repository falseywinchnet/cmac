#pragma once
// The scanner's one foreign interface: list an open directory with
// getattrlistbulk(2) and append its entries to a workspace.

#include "scan/scan_workspace.hpp"

#include <cstdint>

namespace cmac {

struct DirectoryReadRequest final {
    // An open directory descriptor, borrowed for the call; not closed.
    int directory_descriptor{-1};
    // Stop once this many records have been read, saving the final call
    // that only reports the end (SCANNER_STUDY.md §3.1). At least one call
    // is always made. UINT32_MAX reads to the end. A directory that grew
    // after its parent was listed can lose entries added in that window;
    // one that shrank is read to the end.
    std::uint32_t expected_entries{UINT32_MAX};
    // Mark directories with another volume mounted on them as mount points.
    bool prune_mount_points{true};
};

struct DirectoryReadResult final {
    // Entries appended: [first_entry, first_entry + entry_count).
    std::uint32_t first_entry{0};
    std::uint32_t entry_count{0};
    // `listed`, or `failed` when getattrlistbulk or a record was rejected;
    // entries appended before the failure are kept.
    DirectoryState state{DirectoryState::listed};
    // errno of the failure, or 0.
    int error{0};
};

// Appends one ScanEntry per record and its name to workspace. The attribute
// buffer must already be sized. Throws std::bad_alloc only if the
// workspace cannot grow.
[[nodiscard]] DirectoryReadResult read_directory(const DirectoryReadRequest& request, Workspace& workspace);

} // namespace cmac
