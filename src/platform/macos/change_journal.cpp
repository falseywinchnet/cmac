#include "platform/macos/change_journal.hpp"

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

#include <condition_variable>
#include <mutex>
#include <utility>

namespace cmac {
namespace {

inline constexpr double seconds_per_event_id = 4.0e-6;

// What the delivery callback writes and replay_journal reads, under mutex.
struct ReplayRecord final {
    std::mutex mutex{};
    std::condition_variable history_done{};
    std::uint64_t until{0};
    bool done{false};
    bool rescan_everything{false};
    std::uint64_t events{0};
    ChangedFolders folders{};
};

// FSEvents reports folders with a trailing slash ("/Users/me/"); the
// snapshot's paths have none, except the root "/".
std::string folder_path(const char* const reported) {
    std::string result(reported);
    while (result.size() > 1 && result.back() == '/') result.pop_back();
    return result;
}

// The stream's callback, on FSEvents' dispatch queue.
void deliver_events(ConstFSEventStreamRef, void* const context, const std::size_t count, void* const paths,
                    const FSEventStreamEventFlags flags[], const FSEventStreamEventId ids[]) {
    ReplayRecord& record = *static_cast<ReplayRecord*>(context);
    const char* const* const reported = static_cast<const char* const*>(paths);
    const FSEventStreamEventFlags unknowable = kFSEventStreamEventFlagUserDropped |
                                               kFSEventStreamEventFlagKernelDropped |
                                               kFSEventStreamEventFlagEventIdsWrapped |
                                               kFSEventStreamEventFlagRootChanged;
    bool finished = false;
    {
        const std::lock_guard<std::mutex> lock(record.mutex);
        for (std::size_t i = 0; i < count; ++i) {
            const FSEventStreamEventFlags flag = flags[i];
            if ((flag & kFSEventStreamEventFlagHistoryDone) != 0) {
                record.done = true;
                finished = true;
                continue;
            }
            // Later than the position this replay stops at: replayed next time.
            if (ids[i] > record.until) continue;
            record.events = record.events + 1;
            if ((flag & kFSEventStreamEventFlagMustScanSubDirs) != 0) {
                if ((flag & unknowable) != 0 && folder_path(reported[i]) == "/") record.rescan_everything = true;
                record.folders.rescan.push_back(folder_path(reported[i]));
                continue;
            }
            if ((flag & unknowable) != 0) {
                record.rescan_everything = true;
                continue;
            }
            record.folders.relist.push_back(folder_path(reported[i]));
        }
    }
    if (finished) record.history_done.notify_all();
}

// Runs on the stream's queue after everything already queued: once it has
// run, no delivery for the stream is pending.
void drain_marker(void*) {}

} // namespace

JournalPosition current_journal_position(const std::string& root) {
    JournalPosition result{};
    result.event_id = FSEventsGetCurrentEventId();
    struct stat status{};
    if (::stat(root.c_str(), &status) != 0) return result;
    const CFUUIDRef uuid = FSEventsCopyUUIDForDevice(status.st_dev);
    if (uuid == nullptr) return result;
    const CFStringRef text = CFUUIDCreateString(kCFAllocatorDefault, uuid);
    char buffer[64] = {};
    if (text != nullptr && CFStringGetCString(text, buffer, sizeof buffer, kCFStringEncodingUTF8)) result.store = buffer;
    if (text != nullptr) CFRelease(text);
    CFRelease(uuid);
    return result;
}

std::string current_system_build() {
    char buffer[64] = {};
    std::size_t size = sizeof buffer;
    if (sysctlbyname("kern.osversion", buffer, &size, nullptr, 0) != 0) return std::string{};
    const std::string result(buffer);
    return result;
}

double estimated_replay_seconds(const std::uint64_t since, const std::uint64_t until) {
    if (until <= since) return 0.0;
    const double ids = static_cast<double>(until - since);
    const double result = ids * seconds_per_event_id;
    return result;
}

JournalReplay replay_journal(const std::string& root, const std::uint64_t since, const std::uint64_t until,
                             const std::chrono::milliseconds timeout) {
    JournalReplay result{};
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    ReplayRecord record{};
    record.until = until;

    const CFStringRef root_text = CFStringCreateWithCString(kCFAllocatorDefault, root.c_str(), kCFStringEncodingUTF8);
    if (root_text == nullptr) return result;
    const void* path_values[1] = {root_text};
    const CFArrayRef paths = CFArrayCreate(kCFAllocatorDefault, path_values, 1, &kCFTypeArrayCallBacks);
    CFRelease(root_text);
    FSEventStreamContext context{};
    context.info = &record;
    const FSEventStreamRef stream = FSEventStreamCreate(kCFAllocatorDefault, deliver_events, &context, paths, since,
                                                        0.0, kFSEventStreamCreateFlagNone);
    CFRelease(paths);
    if (stream == nullptr) return result;
    const dispatch_queue_t queue = dispatch_queue_create("com.falseywinchnet.cmac.journal", DISPATCH_QUEUE_SERIAL);
    FSEventStreamSetDispatchQueue(stream, queue);
    const bool started_stream = FSEventStreamStart(stream);
    if (started_stream) {
        std::unique_lock<std::mutex> lock(record.mutex);
        const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + timeout;
        while (!record.done) {
            if (record.history_done.wait_until(lock, deadline) == std::cv_status::timeout) break;
        }
    }
    FSEventStreamStop(stream);
    FSEventStreamInvalidate(stream);
    dispatch_sync_f(queue, nullptr, drain_marker);
    FSEventStreamRelease(stream);
    dispatch_release(queue);

    const std::lock_guard<std::mutex> lock(record.mutex);
    result.complete = record.done;
    result.rescan_everything = record.rescan_everything;
    result.events = record.events;
    result.folders = std::move(record.folders);
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;
    result.seconds = elapsed.count();
    return result;
}

} // namespace cmac
