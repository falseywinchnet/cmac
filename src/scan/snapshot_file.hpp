#pragma once
// The saved snapshot: cmac's own record of the last scan, read at launch so
// the picture appears at once (owner, D7). This file format is the
// "MFT-like record" of the plan: a header, one fixed-size record per node,
// and the name pool. Little-endian; Apple silicon only (D9).
//
// Layout, version 1:
//   "CMACSNAP"                 8 bytes
//   format version             u32 (1)
//   record size                u32 (56)
//   node count                 u64
//   name bytes                 u64
//   journal event id           u64  FSEvents id taken when the scan started
//   written at                 i64  seconds since 1970
//   root, journal store UUID, system build: each u32 length + UTF-8 bytes
//   node records               node count × 56 bytes:
//     name offset u32, name length u32, parent u32, first child u32,
//     child count u32, descendant count u32, allocated u64, logical u64,
//     file id u64, link count u32, flags u16, kind u8, directory state u8
//   names                      name bytes

#include "scan/snapshot.hpp"

#include <cstdint>
#include <string>

namespace cmac {

struct SnapshotInfo final {
    // The scanned path; the root node's name.
    std::string root{};
    // FSEvents position at the start of the scan: changes after it are
    // replayed on the next launch.
    std::uint64_t journal_event_id{0};
    // The FSEvents store's UUID for the root's device; a different UUID means
    // the journal was reset and cannot be replayed from this id.
    std::string journal_store{};
    // The OS build (kern.osversion). The sealed System volume changes only
    // with it.
    std::string system_build{};
    std::int64_t written_unix_seconds{0};
};

struct LoadedSnapshot final {
    Snapshot snapshot{};
    SnapshotInfo info{};
};

// Writes snapshot to a temporary file beside path, then renames it over
// path, so a reader sees the old file or the new one, never half of either.
// False with a reason on failure; an existing file is left as it was.
[[nodiscard]] bool write_snapshot_file(const std::string& path, const Snapshot& snapshot, const SnapshotInfo& info,
                                       std::string& error);

// Reads and validates a file written by write_snapshot_file: every link and
// range is checked, so a damaged file is rejected, never trusted. False with
// a reason; loaded is then empty.
[[nodiscard]] bool read_snapshot_file(const std::string& path, LoadedSnapshot& loaded, std::string& error);

// ~/Library/Caches/com.falseywinchnet.cmac/snapshot-<hash of root>.cmacsnap,
// creating the folder. Empty when HOME is unset.
[[nodiscard]] std::string default_snapshot_path(const std::string& root);

} // namespace cmac
