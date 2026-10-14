// Scanner tests on a fixture tree built in a temporary directory: hard links,
// a clone, a sparse file, a symbolic link loop, a refused directory, a
// directory too large for one bulk call, and a deep chain.

#include "scan/directory_reader.hpp"
#include "scan/scan_workspace.hpp"
#include "scan/scanner.hpp"
#include "scan/snapshot.hpp"

#include <fcntl.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

void require(const bool condition, const char* const message) {
    if (!condition) throw std::runtime_error(message);
}

inline constexpr int chain_depth = 300;
inline constexpr int many_files = 5000;

void make_directory(const std::string& path) {
    require(::mkdir(path.c_str(), 0755) == 0, "mkdir failed");
}

// Writes `size` bytes of a repeating pattern, so APFS stores them.
void write_file(const std::string& path, const std::size_t size) {
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    require(descriptor >= 0, "create failed");
    std::vector<char> bytes(size, 'x');
    std::size_t written = 0;
    while (written < size) {
        const ssize_t count = ::write(descriptor, bytes.data() + written, size - written);
        require(count > 0, "write failed");
        written += static_cast<std::size_t>(count);
    }
    ::close(descriptor);
}

struct Fixture final {
    std::string root{};
    std::string refused{};
};

Fixture build_fixture() {
    const char* const temporary = std::getenv("TMPDIR");
    std::string pattern = std::string(temporary != nullptr ? temporary : "/tmp/") + "/cmac-scan-tests-XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    require(::mkdtemp(buffer.data()) != nullptr, "mkdtemp failed");
    Fixture fixture{};
    fixture.root = buffer.data();
    const std::string& root = fixture.root;

    make_directory(root + "/a");
    write_file(root + "/a/f1", 1000);
    write_file(root + "/a/f2", 5000);

    std::string chain = root + "/a/deep";
    make_directory(chain);
    for (int level = 1; level < chain_depth; ++level) {
        chain += "/d";
        make_directory(chain);
    }
    write_file(chain + "/bottom", 1);

    make_directory(root + "/many");
    for (int i = 0; i < many_files; ++i) {
        const std::string name = root + "/many/entry-with-a-longer-name-" + std::to_string(i);
        write_file(name, 0);
    }

    make_directory(root + "/links");
    write_file(root + "/links/h1", 16384);
    require(::link((root + "/links/h1").c_str(), (root + "/links/h2").c_str()) == 0, "link failed");

    require(::symlink(".", (root + "/loop").c_str()) == 0, "symlink failed");

    const std::string sparse = root + "/sparse";
    const int descriptor = ::open(sparse.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    require(descriptor >= 0, "create sparse failed");
    const off_t gibibyte = 1024LL * 1024LL * 1024LL;
    require(::pwrite(descriptor, "z", 1, gibibyte) == 1, "sparse write failed");
    ::close(descriptor);

    require(::clonefile((root + "/a/f2").c_str(), (root + "/clone").c_str(), 0) == 0, "clonefile failed");

    fixture.refused = root + "/refused";
    make_directory(fixture.refused);
    write_file(fixture.refused + "/hidden", 4096);
    require(::chmod(fixture.refused.c_str(), 0) == 0, "chmod failed");
    return fixture;
}

void remove_fixture(const Fixture& fixture) {
    (void)::chmod(fixture.refused.c_str(), 0755);
    const std::string command = "/bin/rm -rf '" + fixture.root + "'";
    (void)std::system(command.c_str());
}

cmac::Snapshot scan_fixture(const Fixture& fixture, const std::uint32_t threads, const bool stop_at_count,
                            cmac::ScanStatistics& statistics) {
    cmac::ScanOptions options{};
    options.root = fixture.root;
    options.thread_count = threads;
    options.stop_at_entry_count = stop_at_count;
    cmac::Scan scan(options);
    scan.wait();
    require(scan.outcome() == cmac::ScanOutcome::complete, "scan did not complete");
    statistics = scan.statistics();
    cmac::Snapshot result = scan.take_snapshot();
    return result;
}

cmac::NodeIndex child_named(const cmac::Snapshot& snapshot, const cmac::NodeIndex parent,
                            const std::string_view name) {
    const cmac::Node& directory = snapshot.nodes[parent.value];
    for (std::uint32_t k = 0; k < directory.child_count; ++k) {
        const cmac::NodeIndex index{directory.first_child.value + k};
        if (snapshot.name_of(snapshot.nodes[index.value]) == name) return index;
    }
    throw std::runtime_error("child not found: " + std::string(name));
}

const cmac::Node& node_at(const cmac::Snapshot& snapshot, const cmac::NodeIndex index) {
    return snapshot.nodes[index.value];
}

void check_snapshot(const cmac::Snapshot& snapshot, const cmac::ScanStatistics& statistics) {
    const cmac::NodeIndex root = cmac::root_node;
    require(node_at(snapshot, root).descendant_count == snapshot.nodes.size() - 1, "descendant count");

    const cmac::NodeIndex a = child_named(snapshot, root, "a");
    require(node_at(snapshot, a).child_count == 3, "a has f1, f2, deep");
    require(node_at(snapshot, child_named(snapshot, a, "f1")).logical_bytes == 1000, "f1 length");
    require(node_at(snapshot, child_named(snapshot, a, "f2")).logical_bytes == 5000, "f2 length");

    cmac::NodeIndex level = child_named(snapshot, a, "deep");
    for (int i = 1; i < chain_depth; ++i) level = child_named(snapshot, level, "d");
    require(node_at(snapshot, child_named(snapshot, level, "bottom")).logical_bytes == 1, "deep chain bottom");

    require(node_at(snapshot, child_named(snapshot, root, "many")).child_count == static_cast<std::uint32_t>(many_files),
            "many: every entry across several bulk calls");

    const cmac::NodeIndex links = child_named(snapshot, root, "links");
    const cmac::Node& h1 = node_at(snapshot, child_named(snapshot, links, "h1"));
    const cmac::Node& h2 = node_at(snapshot, child_named(snapshot, links, "h2"));
    require(h1.file_id == h2.file_id && h1.link_count == 2, "hard links share a file id");
    const bool one_repeat = cmac::has_flag(h1, cmac::NodeFlag::hard_link_repeat) !=
                            cmac::has_flag(h2, cmac::NodeFlag::hard_link_repeat);
    require(one_repeat, "exactly one hard link is a repeat");
    require(node_at(snapshot, links).allocated_bytes == h1.allocated_bytes, "hard-linked file counted once");
    require(statistics.hard_link_repeats == 1, "one hard-link repeat counted");

    require(node_at(snapshot, child_named(snapshot, root, "loop")).kind == cmac::NodeKind::symbolic_link,
            "symbolic link recorded, not followed");

    const cmac::Node& sparse = node_at(snapshot, child_named(snapshot, root, "sparse"));
    require(sparse.logical_bytes == 1024ULL * 1024ULL * 1024ULL + 1, "sparse length");
    require(sparse.allocated_bytes < 1024ULL * 1024ULL, "sparse allocation is small");

    const cmac::Node& clone = node_at(snapshot, child_named(snapshot, root, "clone"));
    require(cmac::has_flag(clone, cmac::NodeFlag::may_share_blocks), "clone may share blocks");

    const cmac::Node& refused = node_at(snapshot, child_named(snapshot, root, "refused"));
    require(refused.directory_state == cmac::DirectoryState::refused, "refused directory");
    require(refused.child_count == 0 && refused.allocated_bytes == 0, "refused contents unknown");
    require(statistics.refused_directories == 1, "one refused directory counted");

    // Totals: the root's allocation is every counted file's allocation.
    std::uint64_t allocated = 0;
    for (const cmac::Node& node : snapshot.nodes) {
        if (node.kind == cmac::NodeKind::directory) continue;
        if (cmac::has_flag(node, cmac::NodeFlag::hard_link_repeat)) continue;
        allocated += node.allocated_bytes;
    }
    require(node_at(snapshot, root).allocated_bytes == allocated, "root total");
}

void test_scan(const Fixture& fixture) {
    cmac::ScanStatistics statistics{};
    const cmac::Snapshot snapshot = scan_fixture(fixture, 6, true, statistics);
    check_snapshot(snapshot, statistics);
}

// One thread, many threads and reading to the end give the same tree.
void test_equivalent_scans(const Fixture& fixture) {
    cmac::ScanStatistics one_statistics{};
    cmac::ScanStatistics many_statistics{};
    cmac::ScanStatistics full_statistics{};
    const cmac::Snapshot one = scan_fixture(fixture, 1, true, one_statistics);
    const cmac::Snapshot many = scan_fixture(fixture, 12, true, many_statistics);
    const cmac::Snapshot full = scan_fixture(fixture, 4, false, full_statistics);
    check_snapshot(one, one_statistics);
    check_snapshot(many, many_statistics);
    check_snapshot(full, full_statistics);
    require(one.nodes.size() == many.nodes.size() && one.nodes.size() == full.nodes.size(), "same node count");
    require(one.nodes[0].allocated_bytes == many.nodes[0].allocated_bytes, "same total");
    require(full_statistics.bulk_calls > many_statistics.bulk_calls, "reading to the end makes more calls");
}

void test_cancel(const Fixture& fixture) {
    cmac::ScanOptions options{};
    options.root = fixture.root;
    options.thread_count = 4;
    cmac::Scan scan(options);
    scan.request_cancel();
    scan.wait();
    const cmac::ScanOutcome outcome = scan.outcome();
    require(outcome == cmac::ScanOutcome::cancelled || outcome == cmac::ScanOutcome::complete,
            "a cancelled scan stops");
}

// On the volume group, /Users is a firmlink whose System-volume stub is
// empty. Its parent's entry count must not stop its read (found on the M4,
// 2026-10-14: a scan of / missed the whole Data volume).
void test_firmlink_count() {
    cmac::Workspace workspace{};
    workspace.attribute_buffer.resize(256 * 1024);
    const int root = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    require(root >= 0, "open / failed");
    cmac::DirectoryReadRequest request{};
    request.directory_descriptor = root;
    const cmac::DirectoryReadResult listing = cmac::read_directory(request, workspace);
    require(listing.state == cmac::DirectoryState::listed, "/ listed");
    std::uint32_t expected = 0;
    bool found = false;
    for (std::uint32_t k = 0; k < listing.entry_count; ++k) {
        const cmac::ScanEntry& entry = workspace.entries[listing.first_entry + k];
        const std::string_view name(workspace.names.data() + entry.name.offset, entry.name.length);
        if (name != "Users") continue;
        found = true;
        expected = entry.expected_entries;
    }
    require(found, "/Users present");
    require(expected == UINT32_MAX, "a firmlink's entry count is unknown");
    const int users = ::openat(root, "Users", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    require(users >= 0, "open /Users failed");
    request.directory_descriptor = users;
    request.expected_entries = expected;
    const cmac::DirectoryReadResult users_listing = cmac::read_directory(request, workspace);
    require(users_listing.entry_count > 0, "/Users lists its Data-volume entries");
    ::close(users);
    ::close(root);
}

void test_bad_root() {
    cmac::ScanOptions options{};
    options.root = "/nonexistent/cmac/root";
    bool threw = false;
    try {
        cmac::Scan scan(options);
    } catch (const std::system_error&) {
        threw = true;
    }
    require(threw, "a missing root is rejected");
}

} // namespace

int main() {
    Fixture fixture{};
    int status = 0;
    try {
        fixture = build_fixture();
        test_scan(fixture);
        test_equivalent_scans(fixture);
        test_cancel(fixture);
        test_firmlink_count();
        test_bad_root();
        std::printf("scan tests passed\n");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "scan tests failed: %s\n", error.what());
        status = 1;
    }
    if (!fixture.root.empty()) remove_fixture(fixture);
    return status;
}
