#include "scan/scanner.hpp"

#include "scan/directory_reader.hpp"
#include "scan/scan_workspace.hpp"
#include "scan/snapshot_builder.hpp"

#include "gui_forms/threading.hpp"

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/syslimits.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
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

struct DirectoryJob final {
    EntryRef directory{};
    std::shared_ptr<DirectoryDescriptor> parent{};
    std::uint32_t expected_entries{UINT32_MAX};
    // NUL-terminated, relative to parent.
    std::array<char, job_name_capacity> name{};
};

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

    bool joined{false};
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
            if (!jobs.empty()) {
                job = std::move(jobs.back());
                jobs.pop_back();
                active = active + 1;
                return true;
            }
            if (active == 0) {
                lock.unlock();
                work_available.notify_all();
                return false;
            }
            work_available.wait(lock);
        }
    }

    // Publishes the children found by one job and retires it.
    void finish_job(std::vector<DirectoryJob>& children) {
        const std::size_t pushed = children.size();
        bool drained_now = false;
        {
            const std::lock_guard<std::mutex> lock(queue_mutex);
            for (std::size_t i = 0; i < pushed; ++i) jobs.push_back(std::move(children[i]));
            active = active - 1;
            drained_now = jobs.empty() && active == 0;
        }
        children.clear();
        if (pushed > 1 || drained_now) {
            work_available.notify_all();
        } else if (pushed == 1) {
            work_available.notify_one();
        }
    }

    // Opens and lists one directory; queues its subdirectories in children.
    void process_job(const std::uint32_t workspace_index, DirectoryJob& job, std::vector<DirectoryJob>& children) {
        Workspace& workspace = workspaces[workspace_index];
        const int parent_descriptor = (*job.parent).descriptor;
        const int descriptor = ::openat(parent_descriptor, job.name.data(),
                                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
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

        std::uint64_t allocated = 0;
        for (std::uint32_t k = 0; k < result.entry_count; ++k) {
            const std::uint32_t entry_index = result.first_entry + k;
            const ScanEntry& entry = workspace.entries[entry_index];
            allocated += entry.allocated_bytes;
            if (entry.kind != NodeKind::directory) continue;
            if (entry.directory_state != DirectoryState::failed) continue;
            if (entry.name.length >= job_name_capacity) continue;
            children.emplace_back();
            DirectoryJob& child = children.back();
            child.directory = EntryRef{workspace_index, entry_index};
            child.parent = directory;
            child.expected_entries = entry.expected_entries;
            std::memcpy(child.name.data(), workspace.names.data() + entry.name.offset, entry.name.length);
            child.name[entry.name.length] = '\0';
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
        }
        bool failed = false;
        {
            const std::lock_guard<std::mutex> lock(failure_mutex);
            failed = !failure_message.empty();
        }
        statistics.thread_count = static_cast<std::uint32_t>(workspaces.size());
        statistics.peak_open_descriptors = peak_open_descriptors.load(std::memory_order_relaxed);
        for (const Workspace& workspace : workspaces) statistics.bulk_calls += workspace.bulk_calls;
        statistics.walk_seconds = seconds_between(started, walked);
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
    state.stop_all();
}

void Scan::wait() {
    State& state = *state_;
    if (state.joined) return;
    for (std::unique_ptr<gui_forms::Worker>& worker : state.workers) (*worker).join();
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
