#include "scan/scanner.hpp"

#include "scan/directory_reader.hpp"
#include "scan/scan_workspace.hpp"
#include "scan/snapshot_builder.hpp"
#include "scan/system_tree.hpp"

#include "gui_forms/threading.hpp"

#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/syslimits.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <string_view>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace cmac {
namespace {

// The first allocation of each workspace, before any growth. Sized for an
// even share of a full Data volume (about 1 M entries over 6 workers).
inline constexpr std::size_t attribute_buffer_bytes = 256 * 1024;
inline constexpr std::size_t initial_entries_per_workspace = 64 * 1024;
inline constexpr std::size_t initial_name_bytes_per_workspace = 2 * 1024 * 1024;
// ATTR_CMN_NAME returns at most NAME_MAX characters as UTF-8, three bytes
// each, plus the terminator.
inline constexpr std::size_t job_name_capacity = NAME_MAX * 3 + 1;

// An open directory. Shared by the jobs of its subdirectories, so it closes
// when the last of them has been opened (openat needs the parent open).
struct DirectoryDescriptor final {
    DirectoryDescriptor(const int descriptor_value, std::atomic<std::uint32_t>& open_count)
        : descriptor(descriptor_value), open_descriptors(&open_count) {}
    ~DirectoryDescriptor() {
        ::close(descriptor);
        (*open_descriptors).fetch_sub(1, std::memory_order_relaxed);
    }
    DirectoryDescriptor(const DirectoryDescriptor&) = delete;
    DirectoryDescriptor& operator=(const DirectoryDescriptor&) = delete;
    int descriptor{-1};
    std::atomic<std::uint32_t>* open_descriptors{nullptr};
};

// How a job gets its directory's entries. A full scan reads every directory
// fresh, except System-volume directories, which it fills from the catalog
// search (system_copy); an incremental one copies, relists or reads fresh by
// ReuseMark.
enum class JobMode : std::uint8_t { fresh, relist, copy, system_copy };

// Where a System-volume search stands. Jobs needing it wait until it is
// ready or failed (then they read their directories themselves).
enum class SystemSearch : std::uint8_t { off, running, ready, failed };

struct DirectoryJob final {
    EntryRef directory{};
    // The open parent; empty when the parent was copied rather than opened,
    // and then `path` names this directory absolutely.
    std::shared_ptr<DirectoryDescriptor> parent{};
    std::string path{};
    std::uint32_t expected_entries{UINT32_MAX};
    JobMode mode{JobMode::fresh};
    // This directory in the previous snapshot (relist and copy only).
    NodeIndex previous{no_node};
    // On the sealed System volume (not reached through a firmlink).
    bool system_volume{false};
    // The directory's file id (system_copy only).
    FileId id{};
    // NUL-terminated, relative to parent.
    std::array<char, job_name_capacity> name{};
};

std::string joined_path(const std::string& folder, const std::string_view name) {
    std::string result = folder;
    if (result.empty() || result.back() != '/') result.push_back('/');
    result.append(name);
    return result;
}

// Where other volumes are mounted, sorted, for directories filled from the
// catalog search (which cannot report mount status).
std::vector<std::string> mount_points() {
    std::vector<std::string> result{};
    struct statfs* mounts = nullptr;
    const int count = getmntinfo(&mounts, MNT_NOWAIT);
    for (int i = 0; i < count; ++i) result.emplace_back(mounts[i].f_mntonname);
    std::sort(result.begin(), result.end());
    return result;
}

bool has_flag_bits(const std::uint16_t flags, const NodeFlag flag) {
    const bool result = (flags & static_cast<std::uint16_t>(flag)) != 0;
    return result;
}

struct ByFileId final {
    bool operator()(const FileId& left, const FileId& right) const { return left < right; }
};

struct ByName final {
    bool operator()(const PreviousChild& left, const PreviousChild& right) const { return left.name < right.name; }
};

JobMode mode_for(const ReuseMark mark) {
    if (mark == ReuseMark::reuse) return JobMode::copy;
    if (mark == ReuseMark::relist) return JobMode::relist;
    return JobMode::fresh;
}

std::uint32_t logical_cpu_count() {
    int count = 0;
    std::size_t size = sizeof count;
    if (sysctlbyname("hw.logicalcpu", &count, &size, nullptr, 0) != 0 || count < 1) return 1;
    const std::uint32_t result = static_cast<std::uint32_t>(count);
    return result;
}

// Open parents are held until their children are opened, so a deep tree
// keeps many descriptors; the default soft limit (256) is too low.
void raise_descriptor_limit() {
    rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) return;
    rlim_t target = limit.rlim_max;
    if (target > static_cast<rlim_t>(OPEN_MAX)) target = static_cast<rlim_t>(OPEN_MAX);
    if (limit.rlim_cur >= target) return;
    limit.rlim_cur = target;
    (void)setrlimit(RLIMIT_NOFILE, &limit);
}

double seconds_between(const std::chrono::steady_clock::time_point start,
                       const std::chrono::steady_clock::time_point end) {
    const std::chrono::duration<double> elapsed = end - start;
    const double result = elapsed.count();
    return result;
}

} // namespace

struct Scan::State final {
    // A worker's borrowed view: its state and the workspace it alone writes.
    struct TaskContext final {
        State* state{nullptr};
        std::uint32_t workspace{0};
    };

    ScanOptions options{};
    ScanProgress progress{};

    // The shared stack of directories to read, last in first out, so a
    // worker tends to go deep and parents close early. `active` counts jobs
    // being processed: the scan is drained when both are empty.
    std::mutex queue_mutex{};
    std::condition_variable work_available{};
    std::vector<DirectoryJob> jobs{};
    std::uint32_t active{0};
    bool stopping{false};

    std::atomic<std::uint32_t> open_descriptors{0};
    std::atomic<std::uint32_t> peak_open_descriptors{0};

    std::mutex failure_mutex{};
    std::string failure_message{};
    std::atomic<bool> cancel_requested{false};

    std::chrono::steady_clock::time_point started{};
    std::vector<Workspace> workspaces{};
    std::vector<TaskContext> contexts{};

    // Written only by the last worker to drain, then published by
    // `finished` (release); read by the owner after an acquire.
    std::atomic<std::uint32_t> drained{0};
    std::atomic<bool> finished{false};
    ScanOutcome outcome{ScanOutcome::running};
    ScanStatistics statistics{};
    Snapshot snapshot{};

    // The System volume's catalog search, written by its worker before it
    // publishes `system_search` (under queue_mutex); read after.
    SystemSearch system_search{SystemSearch::off};
    // Fixed before the workers start; read freely.
    bool system_search_enabled{false};
    SystemTree system_tree{};
    std::string system_search_error{};
    std::vector<std::string> mounted{};
    // System-volume jobs waiting for the search (under queue_mutex).
    std::vector<DirectoryJob> deferred_jobs{};

    bool joined{false};
    // The search's worker, joined before the tree it writes is destroyed.
    std::unique_ptr<gui_forms::Worker> system_worker{};
    // Declared last: destroyed first, after the destructor has stopped and
    // joined them, so no worker outlives what it borrows.
    std::vector<std::unique_ptr<gui_forms::Worker>> workers{};

    void stop_all() {
        {
            const std::lock_guard<std::mutex> lock(queue_mutex);
            stopping = true;
        }
        work_available.notify_all();
    }

    void record_failure(const char* const message) {
        {
            const std::lock_guard<std::mutex> lock(failure_mutex);
            if (failure_message.empty()) failure_message = message;
        }
        stop_all();
    }

    void note_open() {
        const std::uint32_t now = open_descriptors.fetch_add(1, std::memory_order_relaxed) + 1;
        std::uint32_t peak = peak_open_descriptors.load(std::memory_order_relaxed);
        while (now > peak) {
            if (peak_open_descriptors.compare_exchange_weak(peak, now, std::memory_order_relaxed)) break;
        }
    }

    // Takes the next job. False when the scan is drained or stopped.
    bool pop_job(const gui_forms::CancellationFlag& cancellation, DirectoryJob& job) {
        std::unique_lock<std::mutex> lock(queue_mutex);
        while (true) {
            if (stopping || cancellation.requested()) return false;
            if (jobs.empty() && !deferred_jobs.empty() && system_search != SystemSearch::running) release_deferred();
            if (!jobs.empty()) {
                job = std::move(jobs.back());
                jobs.pop_back();
                active = active + 1;
                return true;
            }
            const bool waiting_for_search = !deferred_jobs.empty();
            if (active == 0 && !waiting_for_search) {
                lock.unlock();
                work_available.notify_all();
                return false;
            }
            work_available.wait(lock);
        }
    }

    // Under queue_mutex, once the search has finished: its waiting jobs
    // become ordinary jobs; if it failed they read their directories by path.
    void release_deferred() {
        const bool failed = system_search == SystemSearch::failed;
        for (DirectoryJob& job : deferred_jobs) {
            if (failed) job.mode = JobMode::fresh;
            jobs.push_back(std::move(job));
        }
        deferred_jobs.clear();
    }

    // Publishes the children found by one job and retires it.
    void finish_job(std::vector<DirectoryJob>& children) {
        const std::size_t pushed = children.size();
        bool drained_now = false;
        {
            const std::lock_guard<std::mutex> lock(queue_mutex);
            for (std::size_t i = 0; i < pushed; ++i) {
                DirectoryJob& child = children[i];
                const bool waits = child.mode == JobMode::system_copy && system_search == SystemSearch::running;
                if (child.mode == JobMode::system_copy && system_search == SystemSearch::failed) child.mode = JobMode::fresh;
                if (waits) {
                    deferred_jobs.push_back(std::move(child));
                } else {
                    jobs.push_back(std::move(child));
                }
            }
            active = active - 1;
            drained_now = jobs.empty() && active == 0 && deferred_jobs.empty();
        }
        children.clear();
        if (pushed > 1 || drained_now) {
            work_available.notify_all();
        } else if (pushed == 1) {
            work_available.notify_one();
        }
    }

    void process_job(const std::uint32_t workspace_index, DirectoryJob& job, std::vector<DirectoryJob>& children) {
        if (job.mode == JobMode::copy) {
            copy_job(workspace_index, job, children);
        } else if (job.mode == JobMode::system_copy) {
            system_copy_job(workspace_index, job, children);
        } else {
            read_job(workspace_index, job, children);
        }
    }

    // True for a directory that must not be opened (ScanOptions::withheld).
    bool withheld(const FileId file_id) const {
        const bool result = std::binary_search(options.withheld.begin(), options.withheld.end(), file_id, ByFileId{});
        return result;
    }

    // Fills the workspace's scratch with a previous directory's children,
    // sorted by name.
    void gather_previous_children(Workspace& workspace, const NodeIndex directory) const {
        const Snapshot& previous = *options.previous;
        const Node& node = previous.nodes[directory.value];
        workspace.previous_children.clear();
        for (std::uint32_t k = 0; k < node.child_count; ++k) {
            const NodeIndex child{node.first_child.value + k};
            const std::string_view name = previous.name_of(previous.nodes[child.value]);
            workspace.previous_children.push_back(PreviousChild{name, child});
        }
        std::sort(workspace.previous_children.begin(), workspace.previous_children.end(), ByName{});
    }

    // The previous snapshot's directory with this name and file id, or
    // no_node.
    NodeIndex match_previous(const Workspace& workspace, const std::string_view name, const FileId file_id) const {
        const PreviousChild probe{name, no_node};
        const std::vector<PreviousChild>::const_iterator found = std::lower_bound(
            workspace.previous_children.begin(), workspace.previous_children.end(), probe, ByName{});
        if (found == workspace.previous_children.end() || (*found).name != name) return no_node;
        const Node& node = (*options.previous).nodes[(*found).node.value];
        if (node.kind != NodeKind::directory || node.file_id != file_id) return no_node;
        return (*found).node;
    }

    // Copies a directory's listing from the previous snapshot: no system
    // call. Its subdirectories are queued by their own marks; any that must
    // be read are opened by absolute path, since this one was not opened.
    void copy_job(const std::uint32_t workspace_index, DirectoryJob& job, std::vector<DirectoryJob>& children) {
        Workspace& workspace = workspaces[workspace_index];
        const Snapshot& previous = *options.previous;
        const Node& directory = previous.nodes[job.previous.value];
        job.parent.reset();
        Listing listing{};
        listing.directory = job.directory;
        listing.first_entry = static_cast<std::uint32_t>(workspace.entries.size());
        listing.entry_count = directory.child_count;
        listing.state = directory.directory_state;
        std::uint64_t allocated = 0;
        for (std::uint32_t k = 0; k < directory.child_count; ++k) {
            const NodeIndex child_index{directory.first_child.value + k};
            const Node& child = previous.nodes[child_index.value];
            const std::string_view name = previous.name_of(child);
            ScanEntry entry{};
            entry.allocated_bytes = child.allocated_bytes;
            entry.logical_bytes = child.logical_bytes;
            entry.file_id = child.file_id;
            entry.link_count = child.link_count;
            // The builder decides hard-link repeats again for the new tree.
            entry.flags = static_cast<std::uint16_t>(child.flags & ~static_cast<std::uint16_t>(NodeFlag::hard_link_repeat));
            entry.kind = child.kind;
            entry.directory_state = child.directory_state;
            if (child.kind == NodeKind::directory) {
                const bool mounted = child.directory_state == DirectoryState::mount_point;
                entry.directory_state = mounted ? DirectoryState::mount_point : DirectoryState::failed;
                // A snapshot directory's sizes are its subtree's totals; as an
                // entry it has none of its own, as a fresh read reports.
                entry.allocated_bytes = 0;
                entry.logical_bytes = 0;
            } else {
                allocated += child.allocated_bytes;
            }
            const std::size_t offset = workspace.names.size();
            workspace.names.insert(workspace.names.end(), name.begin(), name.end());
            entry.name.offset = static_cast<std::uint32_t>(offset);
            entry.name.length = static_cast<std::uint32_t>(name.size());
            const bool keep_closed = entry.kind == NodeKind::directory &&
                                     entry.directory_state == DirectoryState::failed && withheld(entry.file_id);
            if (keep_closed) {
                entry.directory_state = DirectoryState::refused;
                workspace.directories_withheld = workspace.directories_withheld + 1;
            }
            const std::uint32_t entry_index = static_cast<std::uint32_t>(workspace.entries.size());
            workspace.entries.push_back(entry);
            if (entry.kind != NodeKind::directory || entry.directory_state != DirectoryState::failed) continue;
            if (name.size() >= job_name_capacity) continue;
            const JobMode mode = mode_for(options.marks[child_index.value]);
            children.emplace_back();
            DirectoryJob& next = children.back();
            next.directory = EntryRef{workspace_index, entry_index};
            next.mode = mode;
            next.previous = mode == JobMode::fresh ? no_node : child_index;
            if (mode != JobMode::copy) next.path = path_of(previous, child_index);
            std::memcpy(next.name.data(), name.data(), name.size());
            next.name[name.size()] = '\0';
        }
        workspace.listings.push_back(listing);
        workspace.directories_reused = workspace.directories_reused + 1;
        progress.directories_read.fetch_add(1, std::memory_order_relaxed);
        progress.entries_seen.fetch_add(directory.child_count, std::memory_order_relaxed);
        progress.allocated_bytes.fetch_add(allocated, std::memory_order_relaxed);
    }

    bool is_mount_point(const std::string& path) const {
        const bool result = std::binary_search(mounted.begin(), mounted.end(), path);
        return result;
    }

    // Fills a System-volume directory from the catalog search: no system
    // call. Firmlinks are handed to the walk by absolute path (their contents
    // are on the Data volume); mount points are not entered.
    void system_copy_job(const std::uint32_t workspace_index, DirectoryJob& job, std::vector<DirectoryJob>& children) {
        Workspace& workspace = workspaces[workspace_index];
        const std::span<const SystemEntry> found = system_tree.children_of(job.id);
        job.parent.reset();
        Listing listing{};
        listing.directory = job.directory;
        listing.first_entry = static_cast<std::uint32_t>(workspace.entries.size());
        listing.entry_count = static_cast<std::uint32_t>(found.size());
        listing.state = DirectoryState::listed;
        std::uint64_t allocated = 0;
        for (const SystemEntry& item : found) {
            const std::string_view name = system_tree.name_of(item);
            ScanEntry entry{};
            entry.kind = item.kind;
            entry.file_id = item.id;
            entry.link_count = item.link_count;
            entry.allocated_bytes = item.allocated_bytes;
            entry.logical_bytes = item.logical_bytes;
            std::uint16_t flags = 0;
            if ((item.bsd_flags & UF_COMPRESSED) != 0) flags |= static_cast<std::uint16_t>(NodeFlag::compressed);
            if ((item.bsd_flags & SF_DATALESS) != 0) flags |= static_cast<std::uint16_t>(NodeFlag::dataless);
            const bool firmlink = (item.bsd_flags & SF_FIRMLINK) != 0;
            if (firmlink) flags |= static_cast<std::uint16_t>(NodeFlag::firmlink);
            entry.flags = flags;
            if (item.kind == NodeKind::directory) entry.directory_state = DirectoryState::failed;
            allocated += item.allocated_bytes;
            const std::size_t offset = workspace.names.size();
            workspace.names.insert(workspace.names.end(), name.begin(), name.end());
            entry.name.offset = static_cast<std::uint32_t>(offset);
            entry.name.length = static_cast<std::uint32_t>(name.size());
            std::string path{};
            bool queue_child = false;
            if (item.kind == NodeKind::directory) {
                path = joined_path(job.path, name);
                const bool mounted_here = is_mount_point(path);
                if (mounted_here && options.prune_mount_points) {
                    entry.directory_state = DirectoryState::mount_point;
                } else if (!firmlink && withheld(item.id)) {
                    entry.directory_state = DirectoryState::refused;
                    workspace.directories_withheld = workspace.directories_withheld + 1;
                } else {
                    queue_child = name.size() < job_name_capacity;
                }
            }
            const std::uint32_t entry_index = static_cast<std::uint32_t>(workspace.entries.size());
            workspace.entries.push_back(entry);
            if (!queue_child) continue;
            const bool stays_on_system = !firmlink && !is_mount_point(path);
            children.emplace_back();
            DirectoryJob& next = children.back();
            next.directory = EntryRef{workspace_index, entry_index};
            next.mode = stays_on_system ? JobMode::system_copy : JobMode::fresh;
            next.system_volume = stays_on_system;
            next.id = item.id;
            next.path = path;
            std::memcpy(next.name.data(), name.data(), name.size());
            next.name[name.size()] = '\0';
        }
        workspace.listings.push_back(listing);
        workspace.directories_searched = workspace.directories_searched + 1;
        progress.directories_read.fetch_add(1, std::memory_order_relaxed);
        progress.entries_seen.fetch_add(found.size(), std::memory_order_relaxed);
        progress.allocated_bytes.fetch_add(allocated, std::memory_order_relaxed);
    }

    // Opens and lists one directory from disk; queues its subdirectories.
    void read_job(const std::uint32_t workspace_index, DirectoryJob& job, std::vector<DirectoryJob>& children) {
        Workspace& workspace = workspaces[workspace_index];
        int descriptor = -1;
        if (job.parent) {
            descriptor = ::openat((*job.parent).descriptor, job.name.data(),
                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        } else {
            descriptor = ::open(job.path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        const int open_error = errno;
        job.parent.reset();
        Listing listing{};
        listing.directory = job.directory;
        listing.first_entry = static_cast<std::uint32_t>(workspace.entries.size());
        if (descriptor < 0) {
            const bool refused = open_error == EPERM || open_error == EACCES;
            listing.state = refused ? DirectoryState::refused : DirectoryState::failed;
            workspace.listings.push_back(listing);
            return;
        }
        note_open();
        const std::shared_ptr<DirectoryDescriptor> directory =
            std::make_shared<DirectoryDescriptor>(descriptor, open_descriptors);

        DirectoryReadRequest request{};
        request.directory_descriptor = descriptor;
        request.expected_entries = options.stop_at_entry_count ? job.expected_entries : UINT32_MAX;
        request.prune_mount_points = options.prune_mount_points;
        const DirectoryReadResult result = read_directory(request, workspace);
        listing.first_entry = result.first_entry;
        listing.entry_count = result.entry_count;
        listing.state = result.state;
        workspace.listings.push_back(listing);
        workspace.directories_listed = workspace.directories_listed + 1;
        const bool matching = job.mode == JobMode::relist;
        if (matching) gather_previous_children(workspace, job.previous);

        std::uint64_t allocated = 0;
        for (std::uint32_t k = 0; k < result.entry_count; ++k) {
            const std::uint32_t entry_index = result.first_entry + k;
            ScanEntry& entry = workspace.entries[entry_index];
            allocated += entry.allocated_bytes;
            if (entry.kind != NodeKind::directory) continue;
            if (entry.directory_state != DirectoryState::failed) continue;
            if (withheld(entry.file_id)) {
                entry.directory_state = DirectoryState::refused;
                workspace.directories_withheld = workspace.directories_withheld + 1;
                continue;
            }
            if (entry.name.length >= job_name_capacity) continue;
            const std::string_view name(workspace.names.data() + entry.name.offset, entry.name.length);
            NodeIndex previous_child = no_node;
            if (matching) previous_child = match_previous(workspace, name, entry.file_id);
            JobMode mode = JobMode::fresh;
            if (previous_child != no_node) mode = mode_for(options.marks[previous_child.value]);
            // A System-volume directory (not a firmlink) is filled from the
            // catalog search when one runs.
            const bool firmlink = has_flag_bits(entry.flags, NodeFlag::firmlink);
            const bool system_child = job.system_volume && !firmlink;
            if (system_child && system_search_enabled && mode == JobMode::fresh) mode = JobMode::system_copy;
            children.emplace_back();
            DirectoryJob& child = children.back();
            child.directory = EntryRef{workspace_index, entry_index};
            child.mode = mode;
            child.previous = mode == JobMode::fresh || mode == JobMode::system_copy ? no_node : previous_child;
            child.system_volume = system_child;
            child.id = entry.file_id;
            if (mode == JobMode::system_copy) child.path = joined_path(job.path, name);
            // A copied directory is never opened, so it need not hold this one open.
            if (mode != JobMode::copy && mode != JobMode::system_copy) child.parent = directory;
            child.expected_entries = entry.expected_entries;
            std::memcpy(child.name.data(), name.data(), name.size());
            child.name[name.size()] = '\0';
        }
        progress.directories_read.fetch_add(1, std::memory_order_relaxed);
        progress.entries_seen.fetch_add(result.entry_count, std::memory_order_relaxed);
        progress.allocated_bytes.fetch_add(allocated, std::memory_order_relaxed);
    }

    // Called once by every worker as it leaves the drain loop. The last one
    // builds the snapshot: by then no other worker writes a workspace, and
    // the acquire-release increment orders their writes before the build.
    void finish_worker() {
        const std::uint32_t count = drained.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (count != workspaces.size()) return;
        const std::chrono::steady_clock::time_point walked = std::chrono::steady_clock::now();
        {
            const std::lock_guard<std::mutex> lock(queue_mutex);
            jobs.clear();
            deferred_jobs.clear();
        }
        bool failed = false;
        {
            const std::lock_guard<std::mutex> lock(failure_mutex);
            failed = !failure_message.empty();
        }
        statistics.thread_count = static_cast<std::uint32_t>(workspaces.size());
        statistics.peak_open_descriptors = peak_open_descriptors.load(std::memory_order_relaxed);
        for (const Workspace& workspace : workspaces) {
            statistics.bulk_calls += workspace.bulk_calls;
            statistics.directories_listed += workspace.directories_listed;
            statistics.directories_reused += workspace.directories_reused;
            statistics.directories_withheld += workspace.directories_withheld;
            statistics.directories_searched += workspace.directories_searched;
        }
        statistics.walk_seconds = seconds_between(started, walked);
        {
            // The search may still be running if no System directory waited
            // for it; its fields are read only under the lock, once published.
            const std::lock_guard<std::mutex> lock(queue_mutex);
            statistics.system_searched = system_search == SystemSearch::ready;
            statistics.system_search_error = system_search_error;
            if (statistics.system_searched) statistics.system_search_seconds = system_tree.seconds;
        }
        if (failed) {
            outcome = ScanOutcome::failed;
        } else if (cancel_requested.load(std::memory_order_relaxed)) {
            outcome = ScanOutcome::cancelled;
        } else {
            try {
                build_snapshot(workspaces, options.root, snapshot, statistics);
                outcome = ScanOutcome::complete;
            } catch (const std::exception& error) {
                const std::lock_guard<std::mutex> lock(failure_mutex);
                failure_message = error.what();
                snapshot = Snapshot{};
                outcome = ScanOutcome::failed;
            }
            const std::chrono::steady_clock::time_point built = std::chrono::steady_clock::now();
            statistics.build_seconds = seconds_between(walked, built);
        }
        finished.store(true, std::memory_order_release);
    }

    // The System volume's catalog search, on its own worker beside the walk.
    // Publishes the tree under queue_mutex and wakes the walkers waiting
    // for it.
    static void run_system_search(const gui_forms::CancellationFlag& cancellation, void* const address) {
        State& state = *static_cast<State*>(address);
        SystemTree tree{};
        std::string error{};
        bool ok = false;
        try {
            ok = read_system_tree("/", cancellation, tree, error);
        } catch (const std::exception& failure) {
            error = failure.what();
        }
        {
            const std::lock_guard<std::mutex> lock(state.queue_mutex);
            if (ok) state.system_tree = std::move(tree);
            state.system_search_error = error;
            state.system_search = ok ? SystemSearch::ready : SystemSearch::failed;
        }
        state.work_available.notify_all();
    }

    static void run_worker(const gui_forms::CancellationFlag& cancellation, void* const address) {
        TaskContext& context = *static_cast<TaskContext*>(address);
        State& state = *context.state;
        std::vector<DirectoryJob> children{};
        DirectoryJob job{};
        try {
            children.reserve(256);
            while (state.pop_job(cancellation, job)) {
                try {
                    state.process_job(context.workspace, job, children);
                } catch (const std::exception& error) {
                    state.record_failure(error.what());
                }
                job.parent.reset();
                state.finish_job(children);
            }
        } catch (const std::exception& error) {
            state.record_failure(error.what());
        }
        state.finish_worker();
    }
};

Scan::Scan(ScanOptions options) : state_(std::make_unique<State>()) {
    State& state = *state_;
    if (options.root.empty()) throw std::invalid_argument("scan root is empty");
    if (options.thread_count == 0) options.thread_count = logical_cpu_count();
    if (options.previous && options.marks.size() != (*options.previous).nodes.size())
        throw std::invalid_argument("reuse marks do not match the previous snapshot");
    if (!std::is_sorted(options.withheld.begin(), options.withheld.end(), ByFileId{}))
        throw std::invalid_argument("withheld directories are not sorted");
    state.options = std::move(options);
    raise_descriptor_limit();

    const int root_descriptor = ::open(state.options.root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root_descriptor < 0) {
        const int error = errno;
        throw std::system_error(error, std::generic_category(), "cannot open scan root " + state.options.root);
    }
    state.note_open();
    DirectoryJob root_job{};
    root_job.directory = root_entry;
    root_job.parent = std::make_shared<DirectoryDescriptor>(root_descriptor, state.open_descriptors);
    root_job.name[0] = '.';
    root_job.name[1] = '\0';
    if (state.options.previous) {
        root_job.mode = mode_for(state.options.marks[root_node.value]);
        root_job.previous = root_job.mode == JobMode::fresh ? no_node : root_node;
    }
    root_job.path = state.options.root;
    state.mounted = mount_points();
    const bool full_scan_of_root = !state.options.previous && state.options.root == "/";
    state.system_search_enabled =
        state.options.search_system_volume && full_scan_of_root && sealed_and_searchable("/");
    root_job.system_volume = state.system_search_enabled;
    state.jobs.push_back(std::move(root_job));

    const std::uint32_t thread_count = state.options.thread_count;
    state.workspaces.resize(thread_count);
    state.contexts.resize(thread_count);
    for (std::uint32_t w = 0; w < thread_count; ++w) {
        Workspace& workspace = state.workspaces[w];
        workspace.attribute_buffer.resize(attribute_buffer_bytes);
        workspace.entries.reserve(initial_entries_per_workspace);
        workspace.names.reserve(initial_name_bytes_per_workspace);
        workspace.listings.reserve(initial_entries_per_workspace / 8);
        state.contexts[w] = State::TaskContext{&state, w};
    }
    state.workers.reserve(thread_count);
    state.started = std::chrono::steady_clock::now();
    if (state.system_search_enabled) {
        state.system_search = SystemSearch::running;
        state.system_worker = std::make_unique<gui_forms::Worker>(State::run_system_search, &state);
    }
    try {
        for (std::uint32_t w = 0; w < thread_count; ++w) {
            state.workers.push_back(std::make_unique<gui_forms::Worker>(State::run_worker, &state.contexts[w]));
        }
    } catch (...) {
        // Workers already started would wait for siblings that never come.
        state.cancel_requested.store(true, std::memory_order_relaxed);
        state.stop_all();
        for (std::unique_ptr<gui_forms::Worker>& worker : state.workers) (*worker).join();
        state.workers.clear();
        if (state.system_worker) {
            (*state.system_worker).request_cancel();
            (*state.system_worker).join();
        }
        throw;
    }
}

Scan::~Scan() {
    request_cancel();
    try {
        wait();
    } catch (...) {
        // A worker's failure was already recorded; destruction cannot report it.
    }
}

bool Scan::finished() const noexcept {
    const State& state = *state_;
    const bool result = state.finished.load(std::memory_order_acquire);
    return result;
}

void Scan::request_cancel() noexcept {
    State& state = *state_;
    if (state.finished.load(std::memory_order_acquire)) return;
    state.cancel_requested.store(true, std::memory_order_relaxed);
    for (std::unique_ptr<gui_forms::Worker>& worker : state.workers) (*worker).request_cancel();
    if (state.system_worker) (*state.system_worker).request_cancel();
    state.stop_all();
}

void Scan::wait() {
    State& state = *state_;
    if (state.joined) return;
    for (std::unique_ptr<gui_forms::Worker>& worker : state.workers) (*worker).join();
    if (state.system_worker) (*state.system_worker).join();
    state.joined = true;
}

const ScanProgress& Scan::progress() const noexcept {
    const State& state = *state_;
    return state.progress;
}

ScanOutcome Scan::outcome() const {
    const State& state = *state_;
    if (!state.finished.load(std::memory_order_acquire)) return ScanOutcome::running;
    return state.outcome;
}

const ScanStatistics& Scan::statistics() const {
    const State& state = *state_;
    if (!state.joined) throw std::logic_error("scan statistics read before wait()");
    return state.statistics;
}

const std::string& Scan::failure() const {
    const State& state = *state_;
    if (!state.joined) throw std::logic_error("scan failure read before wait()");
    return state.failure_message;
}

Snapshot Scan::take_snapshot() {
    State& state = *state_;
    if (!state.joined || state.outcome != ScanOutcome::complete)
        throw std::logic_error("take_snapshot needs a complete, joined scan");
    Snapshot result = std::move(state.snapshot);
    state.snapshot = Snapshot{};
    return result;
}

} // namespace cmac
