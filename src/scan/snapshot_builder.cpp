#include "scan/snapshot_builder.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace cmac {
namespace {

// Which listing, in which workspace, holds a directory's entries.
struct ListingRef final {
    std::uint32_t workspace{UINT32_MAX};
    std::uint32_t index{0};
};

inline constexpr ListingRef no_listing{UINT32_MAX, 0};

bool is_listing(const ListingRef& reference) {
    const bool result = reference.workspace != UINT32_MAX;
    return result;
}

// A file reached through more than one link, in breadth-first position.
struct LinkedFile final {
    FileId file_id{};
    NodeIndex node{};
};

struct ByFileThenPosition final {
    bool operator()(const LinkedFile& left, const LinkedFile& right) const {
        if (left.file_id != right.file_id) return left.file_id < right.file_id;
        return left.node.value < right.node.value;
    }
};

std::uint16_t with_flag(const std::uint16_t flags, const NodeFlag flag) {
    const std::uint16_t result = static_cast<std::uint16_t>(flags | static_cast<std::uint16_t>(flag));
    return result;
}

void append_name(Snapshot& snapshot, const char* const data, const std::uint32_t length, Node& node) {
    const std::size_t offset = snapshot.names.size();
    if (offset > std::numeric_limits<std::uint32_t>::max() - length)
        throw std::length_error("snapshot name pool exceeds 4 GiB");
    snapshot.names.insert(snapshot.names.end(), data, data + length);
    node.name.offset = static_cast<std::uint32_t>(offset);
    node.name.length = length;
}

} // namespace

void build_snapshot(const std::span<const Workspace> workspaces, const std::string_view root_name,
                    Snapshot& snapshot, ScanStatistics& statistics) {
    snapshot.nodes.clear();
    snapshot.names.clear();

    // 1. For every entry, the listing that read it as a directory.
    std::size_t entry_total = 0;
    std::size_t name_total = root_name.size();
    std::vector<std::vector<ListingRef>> listing_of(workspaces.size());
    for (std::size_t w = 0; w < workspaces.size(); ++w) {
        const Workspace& workspace = workspaces[w];
        entry_total += workspace.entries.size();
        name_total += workspace.names.size();
        listing_of[w].assign(workspace.entries.size(), no_listing);
    }
    if (entry_total >= std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("snapshot exceeds 2^32 nodes");
    ListingRef root_listing = no_listing;
    for (std::size_t w = 0; w < workspaces.size(); ++w) {
        const std::vector<Listing>& listings = workspaces[w].listings;
        for (std::size_t l = 0; l < listings.size(); ++l) {
            const ListingRef reference{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(l)};
            const EntryRef directory = listings[l].directory;
            if (directory == root_entry) {
                root_listing = reference;
            } else {
                listing_of[directory.workspace][directory.index] = reference;
            }
        }
    }

    // 2. Breadth-first copy. Reserving the exact count keeps every node
    // reference below valid while children are appended.
    const std::size_t node_total = entry_total + 1;
    snapshot.nodes.reserve(node_total);
    snapshot.names.reserve(name_total);
    std::vector<ListingRef> node_listing{};
    node_listing.reserve(node_total);

    Node root{};
    root.kind = NodeKind::directory;
    root.directory_state = DirectoryState::failed;
    root.link_count = 1;
    append_name(snapshot, root_name.data(), static_cast<std::uint32_t>(root_name.size()), root);
    snapshot.nodes.push_back(root);
    node_listing.push_back(root_listing);

    std::vector<LinkedFile> linked_files{};
    for (std::size_t n = 0; n < snapshot.nodes.size(); ++n) {
        const ListingRef reference = node_listing[n];
        if (!is_listing(reference)) continue;
        const Workspace& workspace = workspaces[reference.workspace];
        const Listing& listing = workspace.listings[reference.index];
        const std::vector<ListingRef>& workspace_listings = listing_of[reference.workspace];
        Node& directory = snapshot.nodes[n];
        directory.directory_state = listing.state;
        directory.first_child = NodeIndex{static_cast<std::uint32_t>(snapshot.nodes.size())};
        directory.child_count = listing.entry_count;
        for (std::uint32_t k = 0; k < listing.entry_count; ++k) {
            const std::uint32_t entry_index = listing.first_entry + k;
            const ScanEntry& entry = workspace.entries[entry_index];
            Node node{};
            node.parent = NodeIndex{static_cast<std::uint32_t>(n)};
            node.allocated_bytes = entry.allocated_bytes;
            node.logical_bytes = entry.logical_bytes;
            node.file_id = entry.file_id;
            node.link_count = entry.link_count;
            node.flags = entry.flags;
            node.kind = entry.kind;
            node.directory_state = entry.directory_state;
            append_name(snapshot, workspace.names.data() + entry.name.offset, entry.name.length, node);
            const NodeIndex position{static_cast<std::uint32_t>(snapshot.nodes.size())};
            if (entry.kind != NodeKind::directory && entry.link_count > 1)
                linked_files.push_back(LinkedFile{entry.file_id, position});
            snapshot.nodes.push_back(node);
            node_listing.push_back(workspace_listings[entry_index]);
        }
    }

    // 3. Hard links: the first link in breadth-first order is counted; the
    // others keep their own sizes but add nothing to their ancestors.
    std::sort(linked_files.begin(), linked_files.end(), ByFileThenPosition{});
    for (std::size_t i = 1; i < linked_files.size(); ++i) {
        if (linked_files[i].file_id != linked_files[i - 1].file_id) continue;
        Node& repeat = snapshot.nodes[linked_files[i].node.value];
        repeat.flags = with_flag(repeat.flags, NodeFlag::hard_link_repeat);
        statistics.hard_link_repeats = statistics.hard_link_repeats + 1;
    }

    // 4. Totals, children before parents: in breadth-first order every
    // child's index exceeds its parent's, so a descending pass finishes each
    // subtree before adding it to its parent.
    // The root (index 0) is always present, so the last index is valid.
    for (std::size_t n = snapshot.nodes.size() - 1; n >= 1; --n) {
        const Node& node = snapshot.nodes[n];
        Node& parent = snapshot.nodes[node.parent.value];
        parent.descendant_count += 1 + node.descendant_count;
        if (has_flag(node, NodeFlag::hard_link_repeat)) continue;
        parent.allocated_bytes += node.allocated_bytes;
        parent.logical_bytes += node.logical_bytes;
    }

    // 5. Counts.
    for (std::size_t n = 0; n < snapshot.nodes.size(); ++n) {
        const Node& node = snapshot.nodes[n];
        if (has_flag(node, NodeFlag::attributes_failed)) statistics.entries_with_errors += 1;
        if (node.kind == NodeKind::file) statistics.files += 1;
        if (node.kind == NodeKind::symbolic_link) statistics.symbolic_links += 1;
        if (node.kind == NodeKind::other) statistics.others += 1;
        if (node.kind != NodeKind::directory) continue;
        statistics.directories += 1;
        if (node.directory_state == DirectoryState::refused) statistics.refused_directories += 1;
        if (node.directory_state == DirectoryState::failed) statistics.failed_directories += 1;
        if (node.directory_state == DirectoryState::mount_point) statistics.mount_points += 1;
    }
}

} // namespace cmac
