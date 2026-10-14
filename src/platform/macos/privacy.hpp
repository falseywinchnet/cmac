#pragma once
// macOS privacy protection (TCC) as cmac meets it. Without Full Disk Access,
// opening a protected folder makes macOS stop the opening thread and ask the
// user; the prompt can outlive the app and wedge (owner's MacBook,
// 2026-10-14: the Apple Music media-library prompt stayed on screen, unable
// to be clicked, after cmac had quit). cmac therefore never opens a
// protected folder unless it already has Full Disk Access: such folders are
// shown as protected, their size unknown, and the window says how to grant
// access.

#include "scan/snapshot.hpp"

#include <string>
#include <vector>

namespace cmac {

struct PrivacyAccess final {
    bool full_disk_access{false};
    // Protected folders found under the users' homes, by file id, sorted;
    // empty with Full Disk Access.
    std::vector<FileId> withheld{};
    std::vector<std::string> withheld_paths{};
};

// Tests for Full Disk Access by opening the TCC database, which succeeds only
// with it and otherwise fails without a prompt. Then, without it, finds the
// protected folders with lstat (the folder's own metadata never prompts).
[[nodiscard]] PrivacyAccess assess_privacy_access();

// Opens System Settings at Privacy & Security > Full Disk Access.
[[nodiscard]] bool open_full_disk_access_settings();

} // namespace cmac
