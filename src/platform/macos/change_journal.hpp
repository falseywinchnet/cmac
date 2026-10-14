#pragma once
// The FSEvents journal as cmac uses it: where it stands now, and which
// folders changed between two of its positions. Foreign interface:
// CoreServices' FSEvents C API.
//
// Delivery runs on a serial dispatch queue that FSEvents owns for the stream
// (FSEventStreamSetDispatchQueue, the non-deprecated way to receive events).
// The callback only appends to a mutex-guarded record; no cmac work runs
// there, and replay_journal drains the queue before it returns.

#include "scan/reuse_plan.hpp"

#include <chrono>
#include <cstdint>
#include <string>

namespace cmac {

struct JournalPosition final {
    std::uint64_t event_id{0};
    // The FSEvents store's UUID for the root's device; empty if unknown.
    std::string store{};
};

[[nodiscard]] JournalPosition current_journal_position(const std::string& root);

// kern.osversion, the OS build; the sealed System volume changes with it.
[[nodiscard]] std::string current_system_build();

struct JournalReplay final {
    // The history up to `until` was delivered before the timeout.
    bool complete{false};
    // FSEvents dropped events, wrapped its ids or saw the root change: the
    // changes cannot be known, so everything must be read again.
    bool rescan_everything{false};
    ChangedFolders folders{};
    std::uint64_t events{0};
    double seconds{0.0};
};

// The folders changed under root after `since` and up to `until` (directory
// granularity). Blocks until FSEvents reports the history done or the
// timeout passes. Events after `until` are ignored: they stay in the
// journal for the next replay. A file still held open by a writer is
// reported only when it is closed (fdu research, 2026-09-27).
[[nodiscard]] JournalReplay replay_journal(const std::string& root, std::uint64_t since, std::uint64_t until,
                                           std::chrono::milliseconds timeout);

// What replaying from since to until is expected to cost: fseventsd works
// through the whole volume's journal behind the cursor, about 4 µs per event
// id on the owner's MacBook (SCANNER_STUDY.md §6b).
[[nodiscard]] double estimated_replay_seconds(std::uint64_t since, std::uint64_t until);

} // namespace cmac
