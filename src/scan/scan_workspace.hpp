#pragma once
// What one scan worker writes while the scan runs. Each worker owns one
// Workspace and nobody else reads it until every worker has drained the
// queue; the snapshot builder then reads them all.

#include "scan/snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace cmac {

// Where an entry was written: which workspace, and its index there.
struct EntryRef final {
    std::uint32_t workspace{0};
    std::uint32_t index{0};
    bool operator==(const EntryRef& other) const {
        return workspace == other.workspace && index == other.index;
    }
    bool operator!=(const EntryRef& other) const { return !(*this == other); }
};

// The root is not an entry of any listing.
inline constexpr EntryRef root_entry{UINT32_MAX, 0};

// One object as the kernel reported it. The name is in the owning
// workspace's pool.
struct ScanEntry final {
    NameRef name{};
    std::uint64_t allocated_bytes{0};
    std::uint64_t logical_bytes{0};
    FileId file_id{};
    std::uint32_t link_count{0};
    // For a directory: the entry count its parent's listing reported
    // (ATTR_DIR_ENTRYCOUNT), or UINT32_MAX when unknown.
    std::uint32_t expected_entries{UINT32_MAX};
    std::uint16_t flags{0};
    NodeKind kind{NodeKind::other};
    // A directory starts as `failed` (not reached) and is settled by its
    // listing; a pruned mount point is `mount_point` here and never listed.
    DirectoryState directory_state{DirectoryState::not_directory};
};

// One directory a worker read: its entries are the contiguous range
// [first_entry, first_entry + entry_count) of the same workspace.
struct Listing final {
    EntryRef directory{};
    std::uint32_t first_entry{0};
    std::uint32_t entry_count{0};
    DirectoryState state{DirectoryState::failed};
};

// A previous snapshot's child, for matching a relisted directory's entries
// by name.
struct PreviousChild final {
    std::string_view name{};
    NodeIndex node{};
};

struct Workspace final {
    std::vector<ScanEntry> entries{};
    std::vector<char> names{};
    std::vector<Listing> listings{};
    // getattrlistbulk's output; sized once before the scan (256 KiB was the
    // measured probe's size, SCANNER_STUDY.md §2).
    std::vector<std::byte> attribute_buffer{};
    std::uint64_t bulk_calls{0};
    std::uint64_t directories_listed{0};
    std::uint64_t directories_reused{0};
    // Scratch for matching one relisted directory's children; reused.
    std::vector<PreviousChild> previous_children{};
};

} // namespace cmac
