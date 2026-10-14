#pragma once
// The result of one scan: every file system object reached from the root, as
// one flat array in breadth-first order, with names in one pool. A snapshot
// is immutable once published; a rescan builds a new one.

#include <cstdint>
#include <string_view>
#include <vector>

namespace cmac {

// A position in Snapshot::nodes. Distinct from a file id, which names an
// object on the volume and survives between scans.
struct NodeIndex final {
    std::uint32_t value{0};
    bool operator==(const NodeIndex& other) const { return value == other.value; }
    bool operator!=(const NodeIndex& other) const { return value != other.value; }
};

inline constexpr NodeIndex root_node{0};
// The root's parent, and any link that names no node.
inline constexpr NodeIndex no_node{UINT32_MAX};

// The volume's identity for an object (ATTR_CMN_FILEID). Hard links share it.
struct FileId final {
    std::uint64_t value{0};
    bool operator==(const FileId& other) const { return value == other.value; }
    bool operator!=(const FileId& other) const { return value != other.value; }
    bool operator<(const FileId& other) const { return value < other.value; }
};

enum class NodeKind : std::uint8_t { file, directory, symbolic_link, other };

// What happened when the scan tried to list a directory. Every other kind
// of node is `not_directory`.
enum class DirectoryState : std::uint8_t {
    not_directory,
    listed,
    // Privacy protection or permissions refused it (EPERM, EACCES). Its size
    // is unknown, not zero.
    refused,
    // Another volume is mounted here; the scan did not cross into it.
    mount_point,
    // Opening or reading failed for another reason, or the scan was
    // cancelled before reaching it.
    failed,
};

// Bits of Node::flags.
enum class NodeFlag : std::uint16_t {
    purgeable = 1u << 0,         // EF_IS_PURGEABLE: the system may delete it
    sparse = 1u << 1,            // EF_IS_SPARSE: has holes; length overstates use
    may_share_blocks = 1u << 2,  // EF_MAY_SHARE_BLOCKS: possibly an APFS clone
    dataless = 1u << 3,          // SF_DATALESS: contents evicted to iCloud
    compressed = 1u << 4,        // UF_COMPRESSED: decmpfs
    // A second or later link to a file already counted; adds nothing to totals.
    hard_link_repeat = 1u << 5,
    // The kernel returned an error for this entry; its sizes are unknown.
    attributes_failed = 1u << 6,
};

// A name in Snapshot::names: UTF-8 bytes, not terminated.
struct NameRef final {
    std::uint32_t offset{0};
    std::uint32_t length{0};
};

struct Node final {
    NameRef name{};
    NodeIndex parent{no_node};
    // Children are contiguous: [first_child, first_child + child_count).
    NodeIndex first_child{no_node};
    std::uint32_t child_count{0};
    // Objects below a directory, at any depth (zero for other kinds).
    std::uint32_t descendant_count{0};
    // For a file: its allocation on disk, all forks. For a directory: the sum
    // over its descendants, hard-link repeats excluded. Clones are counted
    // once per copy here; the private-size pass of S3 resizes them (D5).
    std::uint64_t allocated_bytes{0};
    // Same rule, for logical length (the data fork's length).
    std::uint64_t logical_bytes{0};
    FileId file_id{};
    std::uint32_t link_count{0};
    std::uint16_t flags{0};
    NodeKind kind{NodeKind::other};
    DirectoryState directory_state{DirectoryState::not_directory};
};

[[nodiscard]] inline bool has_flag(const Node& node, const NodeFlag flag) {
    const std::uint16_t bit = static_cast<std::uint16_t>(flag);
    const bool result = (node.flags & bit) != 0;
    return result;
}

struct Snapshot final {
    // nodes[0] is the root; its name is the scanned path. Breadth-first, so
    // a parent's index is below its children's.
    std::vector<Node> nodes{};
    std::vector<char> names{};

    [[nodiscard]] std::string_view name_of(const Node& node) const {
        const std::string_view result(names.data() + node.name.offset, node.name.length);
        return result;
    }
};

} // namespace cmac
