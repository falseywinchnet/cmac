// Stage S2 tests: the saved-snapshot file, incremental scans and the
// FSEvents journal, on a fixture tree in a temporary directory.

#include "platform/macos/change_journal.hpp"
#include "refresh/refresh.hpp"
#include "scan/reuse_plan.hpp"
#include "scan/scanner.hpp"
#include "scan/snapshot.hpp"
#include "scan/snapshot_file.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

void require(const bool condition, const char* const message) {
    if (!condition) throw std::runtime_error(message);
}

void make_directory(const std::string& path) {
    require(::mkdir(path.c_str(), 0755) == 0, "mkdir failed");
}

void write_file(const std::string& path, const std::size_t size) {
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    require(descriptor >= 0, "create failed");
    const std::vector<char> bytes(size, 'x');
    if (size > 0) require(::write(descriptor, bytes.data(), size) == static_cast<ssize_t>(size), "write failed");
    ::close(descriptor);
}

// The fixture's canonical path (FSEvents reports /private/var/..., not /var/...).
std::string build_fixture() {
    const char* const temporary = std::getenv("TMPDIR");
    std::string pattern = std::string(temporary != nullptr ? temporary : "/tmp/") + "/cmac-refresh-tests-XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    require(::mkdtemp(buffer.data()) != nullptr, "mkdtemp failed");
    char resolved[PATH_MAX] = {};
    require(::realpath(buffer.data(), resolved) != nullptr, "realpath failed");
    const std::string root(resolved);
    make_directory(root + "/a");
    write_file(root + "/a/f1", 1000);
    write_file(root + "/a/f2", 5000);
    std::string chain = root + "/deep";
    make_directory(chain);
    for (int level = 1; level < 40; ++level) {
        chain += "/d";
        make_directory(chain);
    }
    write_file(chain + "/bottom", 300);
    make_directory(root + "/links");
    write_file(root + "/links/h1", 16384);
    require(::link((root + "/links/h1").c_str(), (root + "/links/h2").c_str()) == 0, "link failed");
    make_directory(root + "/gone");
    write_file(root + "/gone/old", 2048);
    return root;
}

// Changes of every kind: a file added, one removed, one grown, a folder tree
// created and one removed.
void change_fixture(const std::string& root) {
    write_file(root + "/a/f3", 7000);
    require(::unlink((root + "/a/f1").c_str()) == 0, "unlink failed");
    write_file(root + "/a/f2", 90000);
    make_directory(root + "/new");
    make_directory(root + "/new/inner");
    write_file(root + "/new/inner/file", 12345);
    require(::unlink((root + "/gone/old").c_str()) == 0, "unlink failed");
    require(::rmdir((root + "/gone").c_str()) == 0, "rmdir failed");
}

void remove_fixture(const std::string& root) {
    const std::string command = "/bin/rm -rf '" + root + "'";
    (void)std::system(command.c_str());
}

cmac::Snapshot scan(cmac::ScanOptions options, cmac::ScanStatistics& statistics) {
    cmac::Scan running(std::move(options));
    running.wait();
    require(running.outcome() == cmac::ScanOutcome::complete, "scan did not complete");
    statistics = running.statistics();
    cmac::Snapshot result = running.take_snapshot();
    return result;
}

cmac::Snapshot full_scan(const std::string& root) {
    cmac::ScanOptions options{};
    options.root = root;
    options.thread_count = 4;
    cmac::ScanStatistics statistics{};
    cmac::Snapshot result = scan(options, statistics);
    return result;
}

// Every node as "path kind bytes", sorted: two snapshots of the same tree
// give the same list whatever order their workers met the folders in.
std::vector<std::string> listing_of(const cmac::Snapshot& snapshot) {
    std::vector<std::string> result{};
    for (std::size_t n = 0; n < snapshot.nodes.size(); ++n) {
        const cmac::Node& node = snapshot.nodes[n];
        const bool repeat = cmac::has_flag(node, cmac::NodeFlag::hard_link_repeat);
        const std::uint64_t bytes = repeat ? 0 : node.allocated_bytes;
        std::string line = cmac::path_of(snapshot, cmac::NodeIndex{static_cast<std::uint32_t>(n)});
        line += " " + std::to_string(static_cast<int>(node.kind)) + " " + std::to_string(bytes) + " " +
                std::to_string(node.descendant_count);
        result.push_back(line);
    }
    std::sort(result.begin(), result.end());
    return result;
}

// Prints the lines only one listing has, before a comparison fails.
void print_difference(const std::vector<std::string>& left, const std::vector<std::string>& right) {
    std::vector<std::string> only_left{};
    std::vector<std::string> only_right{};
    std::set_difference(left.begin(), left.end(), right.begin(), right.end(), std::back_inserter(only_left));
    std::set_difference(right.begin(), right.end(), left.begin(), left.end(), std::back_inserter(only_right));
    for (const std::string& line : only_left) std::fprintf(stderr, "  incremental only: %s\n", line.c_str());
    for (const std::string& line : only_right) std::fprintf(stderr, "  full only:        %s\n", line.c_str());
}

void test_snapshot_file(const std::string& root) {
    const cmac::Snapshot snapshot = full_scan(root);
    const std::string path = root + ".cmacsnap";
    cmac::SnapshotInfo info{};
    info.root = root;
    info.journal_event_id = 1234;
    info.journal_store = "store";
    info.system_build = "build";
    info.written_unix_seconds = 42;
    std::string error{};
    require(cmac::write_snapshot_file(path, snapshot, info, error), "the snapshot is written");
    cmac::LoadedSnapshot loaded{};
    require(cmac::read_snapshot_file(path, loaded, error), "the snapshot is read back");
    require(loaded.info.root == root && loaded.info.journal_event_id == 1234 && loaded.info.journal_store == "store" &&
                loaded.info.system_build == "build" && loaded.info.written_unix_seconds == 42,
            "the header round-trips");
    require(listing_of(loaded.snapshot) == listing_of(snapshot), "every node round-trips");
    require(loaded.snapshot.names == snapshot.names, "the name pool round-trips");

    // A truncated file and a file with a broken link are rejected.
    struct stat status{};
    require(::stat(path.c_str(), &status) == 0, "stat failed");
    require(::truncate(path.c_str(), status.st_size - 3) == 0, "truncate failed");
    require(!cmac::read_snapshot_file(path, loaded, error), "a truncated file is rejected");
    require(cmac::write_snapshot_file(path, snapshot, info, error), "rewritten");
    // The first node record's parent field (after the header) set to 0.
    const std::size_t header = 8 + 4 + 4 + 8 + 8 + 8 + 8 + 4 + root.size() + 4 + 5 + 4 + 5;
    const int descriptor = ::open(path.c_str(), O_WRONLY);
    require(descriptor >= 0, "open failed");
    const std::uint32_t zero = 0;
    require(::pwrite(descriptor, &zero, sizeof zero, static_cast<off_t>(header + 8)) == sizeof zero, "pwrite failed");
    ::close(descriptor);
    require(!cmac::read_snapshot_file(path, loaded, error), "a file whose root has a parent is rejected");
    ::unlink(path.c_str());
}

// An incremental scan that rereads only the changed folders gives the same
// tree as a full scan, and copies the rest.
void test_incremental_equals_full(const std::string& root) {
    const std::shared_ptr<const cmac::Snapshot> before = std::make_shared<const cmac::Snapshot>(full_scan(root));
    change_fixture(root);
    cmac::ChangedFolders changes{};
    changes.relist.push_back(root);
    changes.relist.push_back(root + "/a");
    const cmac::ReusePlan plan = cmac::plan_reuse(*before, changes);
    require(plan.relisted == 2, "two folders are relisted");

    cmac::ScanOptions options{};
    options.root = root;
    options.thread_count = 4;
    options.previous = before;
    options.marks = plan.marks;
    cmac::ScanStatistics statistics{};
    const cmac::Snapshot refreshed = scan(options, statistics);
    const cmac::Snapshot full = full_scan(root);
    const std::vector<std::string> refreshed_listing = listing_of(refreshed);
    const std::vector<std::string> full_listing = listing_of(full);
    if (refreshed_listing != full_listing) print_difference(refreshed_listing, full_listing);
    require(refreshed_listing == full_listing, "the incremental scan equals a full scan");
    require(statistics.directories_listed == 4, "only the changed folders and the new tree are read");
    require(statistics.directories_reused >= 40, "the unchanged folders are copied");
}

// FSEvents reports the folders changed after a position.
void test_journal(const std::string& root) {
    const cmac::JournalPosition start = cmac::current_journal_position(root);
    require(start.event_id > 0 && !start.store.empty(), "the journal has a position");
    write_file(root + "/a/journalled", 100);
    make_directory(root + "/journal-folder");
    bool seen = false;
    for (int attempt = 0; attempt < 20 && !seen; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const cmac::JournalPosition now = cmac::current_journal_position(root);
        const cmac::JournalReplay replay =
            cmac::replay_journal(root, start.event_id, now.event_id, std::chrono::milliseconds(10000));
        require(replay.complete, "the replay completes");
        const std::vector<std::string>& relist = replay.folders.relist;
        const bool a = std::find(relist.begin(), relist.end(), root + "/a") != relist.end();
        const bool top = std::find(relist.begin(), relist.end(), root) != relist.end();
        seen = a && top;
    }
    require(seen, "the journal reports both changed folders");
}

} // namespace

int main() {
    std::string root{};
    int status = 0;
    try {
        root = build_fixture();
        test_snapshot_file(root);
        test_incremental_equals_full(root);
        test_journal(root);
        std::printf("refresh tests passed\n");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "refresh tests failed: %s\n", error.what());
        status = 1;
    }
    if (!root.empty()) remove_fixture(root);
    return status;
}
