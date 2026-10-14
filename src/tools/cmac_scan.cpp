// cmac-scan: scan a tree and print what the scan found and what it cost.
// The measurement tool for stages S1 and S2 (planning/PLAN.md).
//
// usage: cmac-scan [--threads N] [--read-to-end] [--cross-mounts] [--top N] [--progress]
//                  [--cache FILE [--verify]] <root>
//
// With --cache: load the saved scan in FILE if there is one, bring it up to
// date (journal replay and an incremental scan, or a full scan when that
// cannot be trusted), and save the result back. --verify then also scans in
// full and compares the two, node by node.

#include "platform/macos/privacy.hpp"
#include "refresh/refresh.hpp"
#include "scan/scanner.hpp"
#include "scan/snapshot.hpp"
#include "scan/snapshot_file.hpp"

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct CommandLine final {
    cmac::ScanOptions options{};
    std::uint32_t top{10};
    bool show_progress{false};
    std::string cache{};
    bool verify{false};
    bool valid{false};
};

CommandLine parse_command_line(const int argc, char** const argv) {
    CommandLine result{};
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        const bool has_value = i + 1 < argc;
        if (argument == "--threads" && has_value) {
            i = i + 1;
            result.options.thread_count = static_cast<std::uint32_t>(std::strtoul(argv[i], nullptr, 10));
        } else if (argument == "--top" && has_value) {
            i = i + 1;
            result.top = static_cast<std::uint32_t>(std::strtoul(argv[i], nullptr, 10));
        } else if (argument == "--cache" && has_value) {
            i = i + 1;
            result.cache = argv[i];
        } else if (argument == "--verify") {
            result.verify = true;
        } else if (argument == "--read-to-end") {
            result.options.stop_at_entry_count = false;
        } else if (argument == "--cross-mounts") {
            result.options.prune_mount_points = false;
        } else if (argument == "--progress") {
            result.show_progress = true;
        } else if (!argument.empty() && argument[0] != '-' && result.options.root.empty()) {
            result.options.root = std::string(argument);
        } else {
            return result;
        }
    }
    result.valid = !result.options.root.empty() && (!result.verify || !result.cache.empty());
    return result;
}

double gibibytes(const std::uint64_t bytes) {
    const double result = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    return result;
}

double seconds_of(const timeval& value) {
    const double result = static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1e6;
    return result;
}

double seconds_since(const std::chrono::steady_clock::time_point start) {
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    const double result = elapsed.count();
    return result;
}

const char* outcome_name(const cmac::ScanOutcome outcome) {
    if (outcome == cmac::ScanOutcome::complete) return "complete";
    if (outcome == cmac::ScanOutcome::cancelled) return "cancelled";
    if (outcome == cmac::ScanOutcome::failed) return "failed";
    return "running";
}

struct LargerAllocation final {
    const cmac::Snapshot* snapshot{nullptr};
    bool operator()(const cmac::NodeIndex left, const cmac::NodeIndex right) const {
        const cmac::Node& a = (*snapshot).nodes[left.value];
        const cmac::Node& b = (*snapshot).nodes[right.value];
        if (a.allocated_bytes != b.allocated_bytes) return a.allocated_bytes > b.allocated_bytes;
        return left.value < right.value;
    }
};

void print_top_children(const cmac::Snapshot& snapshot, const std::uint32_t top) {
    if (top == 0) return;
    const cmac::Node& root = snapshot.nodes[cmac::root_node.value];
    std::vector<cmac::NodeIndex> children{};
    children.reserve(root.child_count);
    for (std::uint32_t k = 0; k < root.child_count; ++k) {
        children.push_back(cmac::NodeIndex{root.first_child.value + k});
    }
    std::sort(children.begin(), children.end(), LargerAllocation{&snapshot});
    const std::size_t shown = std::min<std::size_t>(children.size(), top);
    std::printf("largest children of the root:\n");
    for (std::size_t i = 0; i < shown; ++i) {
        const cmac::Node& node = snapshot.nodes[children[i].value];
        const std::string name(snapshot.name_of(node));
        std::printf("  %10.2f GiB  %9u objects  %s\n", gibibytes(node.allocated_bytes), node.descendant_count,
                    name.c_str());
    }
}

void print_statistics(const cmac::ScanStatistics& statistics) {
    std::printf("threads %u, peak open descriptors %u, bulk calls %llu\n", statistics.thread_count,
                statistics.peak_open_descriptors, static_cast<unsigned long long>(statistics.bulk_calls));
    std::printf("directories listed from disk %llu, copied from the saved scan %llu, kept closed %llu\n",
                static_cast<unsigned long long>(statistics.directories_listed),
                static_cast<unsigned long long>(statistics.directories_reused),
                static_cast<unsigned long long>(statistics.directories_withheld));
    std::printf("walk %.3f s, build %.3f s\n", statistics.walk_seconds, statistics.build_seconds);
    std::printf("files %llu, directories %llu, symbolic links %llu, others %llu\n",
                static_cast<unsigned long long>(statistics.files),
                static_cast<unsigned long long>(statistics.directories),
                static_cast<unsigned long long>(statistics.symbolic_links),
                static_cast<unsigned long long>(statistics.others));
    std::printf("refused %llu, failed %llu, mount points %llu, hard-link repeats %llu, entry errors %llu\n",
                static_cast<unsigned long long>(statistics.refused_directories),
                static_cast<unsigned long long>(statistics.failed_directories),
                static_cast<unsigned long long>(statistics.mount_points),
                static_cast<unsigned long long>(statistics.hard_link_repeats),
                static_cast<unsigned long long>(statistics.entries_with_errors));
}

struct ScanRun final {
    bool complete{false};
    cmac::Snapshot snapshot{};
    cmac::ScanStatistics statistics{};
    std::string failure{};
};

ScanRun run_scan(cmac::ScanOptions options, const bool show_progress) {
    ScanRun result{};
    cmac::Scan scan(std::move(options));
    std::chrono::steady_clock::time_point last_report = std::chrono::steady_clock::now();
    while (!scan.finished()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!show_progress) continue;
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (now - last_report < std::chrono::milliseconds(500)) continue;
        last_report = now;
        const cmac::ScanProgress& progress = scan.progress();
        std::fprintf(stderr, "  %llu directories, %llu entries, %.2f GiB\n",
                     static_cast<unsigned long long>(progress.directories_read.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(progress.entries_seen.load(std::memory_order_relaxed)),
                     gibibytes(progress.allocated_bytes.load(std::memory_order_relaxed)));
    }
    scan.wait();
    result.statistics = scan.statistics();
    result.complete = scan.outcome() == cmac::ScanOutcome::complete;
    if (!result.complete) {
        result.failure = std::string(outcome_name(scan.outcome())) + ": " + scan.failure();
        return result;
    }
    result.snapshot = scan.take_snapshot();
    return result;
}

// One node's identity for comparison: a hash of its path, built down the
// tree, with what a treemap shows of it.
struct NodeKey final {
    std::uint64_t path_hash{0};
    std::uint64_t allocated_bytes{0};
    cmac::NodeKind kind{cmac::NodeKind::other};
    std::uint32_t node{0};
};

struct ByPathThenSize final {
    bool operator()(const NodeKey& left, const NodeKey& right) const {
        if (left.path_hash != right.path_hash) return left.path_hash < right.path_hash;
        if (left.allocated_bytes != right.allocated_bytes) return left.allocated_bytes < right.allocated_bytes;
        return static_cast<int>(left.kind) < static_cast<int>(right.kind);
    }
};

std::vector<NodeKey> keys_of(const cmac::Snapshot& snapshot) {
    std::vector<std::uint64_t> hashes(snapshot.nodes.size(), 14695981039346656037ull);
    std::vector<NodeKey> keys(snapshot.nodes.size());
    for (std::size_t n = 0; n < snapshot.nodes.size(); ++n) {
        const cmac::Node& node = snapshot.nodes[n];
        std::uint64_t hash = n == 0 ? 14695981039346656037ull : hashes[node.parent.value];
        hash = (hash ^ 0x2Fu) * 1099511628211ull;
        for (const char c : snapshot.name_of(node)) hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
        hashes[n] = hash;
        const bool counted = !cmac::has_flag(node, cmac::NodeFlag::hard_link_repeat);
        keys[n] = NodeKey{hash, counted ? node.allocated_bytes : 0, node.kind, static_cast<std::uint32_t>(n)};
    }
    std::sort(keys.begin(), keys.end(), ByPathThenSize{});
    return keys;
}

// Prints how two snapshots of the same root differ: objects only in one, and
// objects whose size differs. A live system changes between two scans, so a
// few differences are expected; their paths say whether they are real.
void compare_snapshots(const cmac::Snapshot& refreshed, const cmac::Snapshot& full) {
    const std::vector<NodeKey> a = keys_of(refreshed);
    const std::vector<NodeKey> b = keys_of(full);
    std::size_t i = 0;
    std::size_t j = 0;
    std::size_t only_refreshed = 0;
    std::size_t only_full = 0;
    std::size_t shown = 0;
    while (i < a.size() || j < b.size()) {
        const bool take_a = j >= b.size() || (i < a.size() && ByPathThenSize{}(a[i], b[j]));
        const bool take_b = i >= a.size() || (j < b.size() && ByPathThenSize{}(b[j], a[i]));
        if (!take_a && !take_b) {
            i = i + 1;
            j = j + 1;
            continue;
        }
        if (take_a) {
            only_refreshed = only_refreshed + 1;
            if (shown < 12) {
                std::printf("  only in refreshed: %s (%llu bytes)\n", cmac::path_of(refreshed, cmac::NodeIndex{a[i].node}).c_str(),
                            static_cast<unsigned long long>(a[i].allocated_bytes));
                shown = shown + 1;
            }
            i = i + 1;
        } else {
            only_full = only_full + 1;
            if (shown < 12) {
                std::printf("  only in full:      %s (%llu bytes)\n", cmac::path_of(full, cmac::NodeIndex{b[j].node}).c_str(),
                            static_cast<unsigned long long>(b[j].allocated_bytes));
                shown = shown + 1;
            }
            j = j + 1;
        }
    }
    std::printf("verify: %zu objects refreshed, %zu full; %zu differ only in the refreshed scan, %zu only in the full\n",
                a.size(), b.size(), only_refreshed, only_full);
    std::printf("verify: root %.3f GiB refreshed, %.3f GiB full\n",
                gibibytes(refreshed.nodes[cmac::root_node.value].allocated_bytes),
                gibibytes(full.nodes[cmac::root_node.value].allocated_bytes));
}

int run_with_cache(const CommandLine& command_line) {
    const std::string& root = command_line.options.root;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::shared_ptr<cmac::LoadedSnapshot> saved = std::make_shared<cmac::LoadedSnapshot>();
    std::string error{};
    const bool have_saved = cmac::read_snapshot_file(command_line.cache, *saved, error);
    std::printf("load: %s in %.3f s%s%s\n", have_saved ? "saved scan read" : "no saved scan", seconds_since(started),
                have_saved ? "" : " - ", have_saved ? "" : error.c_str());

    started = std::chrono::steady_clock::now();
    cmac::RefreshPlan plan = cmac::plan_refresh(have_saved ? saved.get() : nullptr, root, cmac::RefreshPolicy{});
    std::printf("plan: %s, in %.3f s: %s\n", plan.incremental ? "incremental" : "full", seconds_since(started),
                plan.reason.c_str());

    cmac::ScanOptions options = command_line.options;
    if (plan.incremental) {
        // The previous snapshot is shared from the loaded record it lives in.
        options.previous = std::shared_ptr<const cmac::Snapshot>(saved, &(*saved).snapshot);
        options.marks = std::move(plan.reuse.marks);
    }
    started = std::chrono::steady_clock::now();
    ScanRun run = run_scan(std::move(options), command_line.show_progress);
    std::printf("scan: %s in %.3f s\n", run.complete ? "complete" : run.failure.c_str(), seconds_since(started));
    if (!run.complete) return 1;
    print_statistics(run.statistics);

    started = std::chrono::steady_clock::now();
    cmac::SnapshotInfo info{};
    info.root = root;
    info.journal_event_id = plan.position.event_id;
    info.journal_store = plan.position.store;
    info.system_build = cmac::current_system_build();
    info.written_unix_seconds = static_cast<std::int64_t>(std::time(nullptr));
    const bool written = cmac::write_snapshot_file(command_line.cache, run.snapshot, info, error);
    std::printf("save: %s in %.3f s\n", written ? "written" : error.c_str(), seconds_since(started));

    const cmac::Node& top = run.snapshot.nodes[cmac::root_node.value];
    std::printf("root: %.2f GiB allocated, %u objects\n", gibibytes(top.allocated_bytes), top.descendant_count);
    if (command_line.verify) {
        ScanRun full = run_scan(command_line.options, false);
        if (!full.complete) {
            std::printf("verify: the full scan failed: %s\n", full.failure.c_str());
            return 1;
        }
        compare_snapshots(run.snapshot, full.snapshot);
    }
    print_top_children(run.snapshot, command_line.top);
    return 0;
}

} // namespace

int main(const int argc, char** const argv) {
    const CommandLine command_line = parse_command_line(argc, argv);
    if (!command_line.valid) {
        std::fprintf(stderr, "usage: cmac-scan [--threads N] [--read-to-end] [--cross-mounts] [--top N] [--progress]\n"
                             "                 [--cache FILE [--verify]] <root>\n");
        return 64;
    }
    try {
        int status = 0;
        // As the app does: protected folders stay closed without Full Disk
        // Access, so no consent prompt can stop the scan.
        CommandLine scan_command = command_line;
        const cmac::PrivacyAccess privacy = cmac::assess_privacy_access();
        scan_command.options.withheld = privacy.withheld;
        std::printf("full disk access: %s; %zu protected folders kept closed\n", privacy.full_disk_access ? "yes" : "no",
                    privacy.withheld.size());
        if (!scan_command.cache.empty()) {
            status = run_with_cache(scan_command);
        } else {
            ScanRun run = run_scan(scan_command.options, scan_command.show_progress);
            std::printf("outcome %s\n", run.complete ? "complete" : run.failure.c_str());
            print_statistics(run.statistics);
            if (!run.complete) {
                status = 1;
            } else {
                const cmac::Node& root = run.snapshot.nodes[cmac::root_node.value];
                std::printf("root: %.2f GiB allocated, %.2f GiB logical, %u objects, %zu name bytes\n",
                            gibibytes(root.allocated_bytes), gibibytes(root.logical_bytes), root.descendant_count,
                            run.snapshot.names.size());
                print_top_children(run.snapshot, command_line.top);
            }
        }
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        // ru_maxrss is in bytes on macOS.
        std::printf("cpu user %.2f s, sys %.2f s, peak resident %.1f MiB\n", seconds_of(usage.ru_utime),
                    seconds_of(usage.ru_stime), static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0));
        return status;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "cmac-scan: %s\n", error.what());
        return 1;
    }
}
