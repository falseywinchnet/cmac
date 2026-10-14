#pragma once
// Joins the workers' workspaces into one breadth-first Snapshot, counts
// each hard-linked file once, and sums sizes up the tree.

#include "scan/scan_workspace.hpp"
#include "scan/scanner.hpp"
#include "scan/snapshot.hpp"

#include <span>
#include <string_view>

namespace cmac {

// Reads every workspace (none may still be written) and replaces snapshot.
// Fills the count fields of statistics; leaves its timings and call counts.
// On an exception snapshot is left empty.
void build_snapshot(std::span<const Workspace> workspaces, std::string_view root_name, Snapshot& snapshot,
                    ScanStatistics& statistics);

} // namespace cmac
