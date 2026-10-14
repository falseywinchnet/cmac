// cmac-scan: scan a tree and print what the scan found and what it cost.
// The measurement tool for stage S1 (planning/PLAN.md).
//
// usage: cmac-scan [--threads N] [--read-to-end] [--cross-mounts] [--top N] [--progress] <root>

#include "scan/scanner.hpp"
#include "scan/snapshot.hpp"

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct CommandLine final {
    cmac::ScanOptions options{};
    std::uint32_t top{10};
    bool show_progress{false};
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
    result.valid = !result.options.root.empty();
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

} // namespace

int main(const int argc, char** const argv) {
    const CommandLine command_line = parse_command_line(argc, argv);
    if (!command_line.valid) {
        std::fprintf(stderr,
                     "usage: cmac-scan [--threads N] [--read-to-end] [--cross-mounts] [--top N] [--progress] <root>\n");
        return 64;
    }
    try {
        cmac::Scan scan(command_line.options);
        std::chrono::steady_clock::time_point last_report = std::chrono::steady_clock::now();
        while (!scan.finished()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (!command_line.show_progress) continue;
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
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        const cmac::ScanOutcome outcome = scan.outcome();
        std::printf("outcome %s\n", outcome_name(outcome));
        print_statistics(scan.statistics());
        // ru_maxrss is in bytes on macOS.
        std::printf("cpu user %.2f s, sys %.2f s, peak resident %.1f MiB\n", seconds_of(usage.ru_utime),
                    seconds_of(usage.ru_stime), static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0));
        if (outcome != cmac::ScanOutcome::complete) {
            std::printf("failure: %s\n", scan.failure().c_str());
            return 1;
        }
        const cmac::Snapshot snapshot = scan.take_snapshot();
        const cmac::Node& root = snapshot.nodes[cmac::root_node.value];
        std::printf("root: %.2f GiB allocated, %.2f GiB logical, %u objects, %zu name bytes\n",
                    gibibytes(root.allocated_bytes), gibibytes(root.logical_bytes), root.descendant_count,
                    snapshot.names.size());
        print_top_children(snapshot, command_line.top);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "cmac-scan: %s\n", error.what());
        return 1;
    }
    return 0;
}
