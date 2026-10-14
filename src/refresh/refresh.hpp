#pragma once
// How cmac brings a saved snapshot up to date: replay the FSEvents journal
// since the saved position and mark only the changed folders for reading,
// or, when that cannot be trusted or would cost more than reading, scan in
// full. Blocks for the replay: run it off the UI thread.

#include "platform/macos/change_journal.hpp"
#include "scan/reuse_plan.hpp"
#include "scan/snapshot_file.hpp"

#include <chrono>
#include <string>

namespace cmac {

struct RefreshPolicy final {
    // Replay only when it is expected to take less than this; a full walk of
    // the M4's volume group takes about 9.6 s.
    double replay_budget_seconds{3.0};
    std::chrono::milliseconds replay_timeout{10000};
};

struct RefreshPlan final {
    // Incremental: scan with the saved snapshot and `reuse`. Otherwise a
    // full scan.
    bool incremental{false};
    // Why, for the status line and the record.
    std::string reason{};
    // The journal's position when planning began: stored with the new
    // snapshot, so the next replay starts here.
    JournalPosition position{};
    JournalReplay replay{};
    ReusePlan reuse{};
};

// saved may be null (no saved scan). Reads the journal; does not scan.
[[nodiscard]] RefreshPlan plan_refresh(const LoadedSnapshot* saved, const std::string& root,
                                       const RefreshPolicy& policy);

} // namespace cmac
