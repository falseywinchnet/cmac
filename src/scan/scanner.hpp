#pragma once
// One scan of a directory tree. The owner thread constructs a Scan, which
// opens the root and starts one gui_forms::Worker per thread; the workers
// drain one shared directory queue, and the last to finish builds the
// snapshot. The owner polls finished(), then wait()s and takes the result.

#include "scan/snapshot.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace cmac {

struct ScanOptions final {
    // An absolute path to a directory. "/" scans the volume group (System
    // and Data through their firmlinks, each object once).
    std::string root{};
    // 0 means one per logical CPU, the measured best (SCANNER_STUDY.md §3.1).
    std::uint32_t thread_count{0};
    bool stop_at_entry_count{true};
    // Do not cross into other mounted volumes or automount triggers.
    bool prune_mount_points{true};
};

// Updated by the workers once per directory with relaxed order; for
// display while the scan runs, not for totals.
struct ScanProgress final {
    std::atomic<std::uint64_t> directories_read{0};
    std::atomic<std::uint64_t> entries_seen{0};
    std::atomic<std::uint64_t> allocated_bytes{0};
};

struct ScanStatistics final {
    std::uint64_t files{0};
    std::uint64_t directories{0};
    std::uint64_t symbolic_links{0};
    std::uint64_t others{0};
    std::uint64_t refused_directories{0};
    std::uint64_t failed_directories{0};
    std::uint64_t mount_points{0};
    std::uint64_t hard_link_repeats{0};
    std::uint64_t entries_with_errors{0};
    std::uint64_t bulk_calls{0};
    std::uint32_t thread_count{0};
    std::uint32_t peak_open_descriptors{0};
    // From the first worker's start to the last worker draining the queue,
    // then the snapshot build, in seconds.
    double walk_seconds{0.0};
    double build_seconds{0.0};
};

enum class ScanOutcome : std::uint8_t { running, complete, cancelled, failed };

class Scan final {
public:
    // Opens the root and starts the workers. Throws std::system_error when
    // the root cannot be opened as a directory, and std::invalid_argument
    // for an empty root. Raises the process's descriptor soft limit to its
    // hard limit (at most OPEN_MAX), since open parents are held while
    // their children are opened.
    explicit Scan(ScanOptions options);
    // Requests cancellation and joins the workers.
    ~Scan();
    Scan(const Scan&) = delete;
    Scan& operator=(const Scan&) = delete;
    Scan(Scan&&) = delete;
    Scan& operator=(Scan&&) = delete;

    // True once the snapshot is built or the scan has stopped. Any thread.
    [[nodiscard]] bool finished() const noexcept;
    // Asks every worker to stop after its current directory. Any thread.
    void request_cancel() noexcept;
    // Joins the workers. Owner thread only; harmless to repeat. Afterwards
    // outcome(), statistics() and take_snapshot() are valid.
    void wait();

    [[nodiscard]] const ScanProgress& progress() const noexcept;
    [[nodiscard]] ScanOutcome outcome() const;
    [[nodiscard]] const ScanStatistics& statistics() const;
    // Why the scan failed, when outcome() is failed.
    [[nodiscard]] const std::string& failure() const;
    // Moves the snapshot out, leaving the scan's empty. Complete scans only;
    // throws std::logic_error otherwise.
    [[nodiscard]] Snapshot take_snapshot();

private:
    struct State;
    std::unique_ptr<State> state_{};
};

} // namespace cmac
