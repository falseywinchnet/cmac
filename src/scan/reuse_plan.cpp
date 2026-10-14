#include "scan/reuse_plan.hpp"

#include <algorithm>
#include <string_view>

namespace cmac {
namespace {

// The deepest node of previous on the way to path: the node itself when it
// exists, otherwise its nearest existing ancestor. no_node when path is not
// below the root.
NodeIndex deepest_existing(const Snapshot& previous, const std::string_view path) {
    const std::string_view root = previous.name_of(previous.nodes[root_node.value]);
    if (path.substr(0, root.size()) != root) return no_node;
    std::string_view rest = path.substr(root.size());
    NodeIndex current = root_node;
    while (!rest.empty()) {
        while (!rest.empty() && rest.front() == '/') rest.remove_prefix(1);
        if (rest.empty()) break;
        const std::size_t slash = rest.find('/');
        const std::string_view component = rest.substr(0, slash);
        rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash);
        const Node& directory = previous.nodes[current.value];
        NodeIndex found = no_node;
        for (std::uint32_t k = 0; k < directory.child_count; ++k) {
            const NodeIndex child{directory.first_child.value + k};
            if (previous.name_of(previous.nodes[child.value]) != component) continue;
            found = child;
            break;
        }
        if (found == no_node) break;
        current = found;
    }
    return current;
}

// The directory to mark for a path: the deepest existing node, or its parent
// when that node is not a directory.
NodeIndex directory_for(const Snapshot& previous, const std::string_view path) {
    NodeIndex node = deepest_existing(previous, path);
    if (node == no_node) return no_node;
    if (previous.nodes[node.value].kind != NodeKind::directory) node = previous.nodes[node.value].parent;
    return node;
}

// APFS gives a file being written more blocks than its length needs, and
// releases them lazily after it is closed, with no FSEvents event: on the M4
// (2026-10-14) 628 files written 38 minutes earlier lost 1,044,480 bytes
// each between two scans a minute apart. A file still holding more than
// this beyond its rounded length is unsettled; its folder is reread.
inline constexpr std::uint64_t unsettled_excess_bytes = 64 * 1024;
inline constexpr std::uint64_t allocation_block_bytes = 4096;

bool unsettled(const Node& node) {
    if (node.kind != NodeKind::file) return false;
    const std::uint16_t different = static_cast<std::uint16_t>(NodeFlag::sparse) |
                                    static_cast<std::uint16_t>(NodeFlag::compressed) |
                                    static_cast<std::uint16_t>(NodeFlag::dataless);
    if ((node.flags & different) != 0) return false;
    const std::uint64_t blocks = (node.logical_bytes + allocation_block_bytes - 1) / allocation_block_bytes;
    const std::uint64_t needed = blocks * allocation_block_bytes;
    const bool result = node.allocated_bytes >= needed + unsettled_excess_bytes;
    return result;
}

void sort_unique(std::vector<std::string>& paths) {
    std::sort(paths.begin(), paths.end());
    const std::vector<std::string>::iterator end = std::unique(paths.begin(), paths.end());
    paths.erase(end, paths.end());
}

} // namespace

ReusePlan plan_reuse(const Snapshot& previous, const ChangedFolders& changes) {
    ReusePlan plan{};
    plan.marks.assign(previous.nodes.size(), ReuseMark::reuse);
    for (std::size_t n = 0; n < previous.nodes.size(); ++n) {
        const Node& node = previous.nodes[n];
        if (node.kind != NodeKind::directory) continue;
        const bool unread = node.directory_state == DirectoryState::refused ||
                            node.directory_state == DirectoryState::failed;
        if (unread) plan.marks[n] = ReuseMark::rescan;
    }
    for (std::size_t n = 1; n < previous.nodes.size(); ++n) {
        const Node& node = previous.nodes[n];
        if (!unsettled(node)) continue;
        if (plan.marks[node.parent.value] == ReuseMark::reuse) plan.marks[node.parent.value] = ReuseMark::relist;
    }
    std::vector<std::string> relist = changes.relist;
    std::vector<std::string> rescan = changes.rescan;
    sort_unique(relist);
    sort_unique(rescan);
    for (const std::string& path : rescan) {
        const NodeIndex directory = directory_for(previous, path);
        if (directory == no_node) continue;
        plan.marks[directory.value] = ReuseMark::rescan;
    }
    for (const std::string& path : relist) {
        const NodeIndex directory = directory_for(previous, path);
        if (directory == no_node) continue;
        if (plan.marks[directory.value] == ReuseMark::reuse) plan.marks[directory.value] = ReuseMark::relist;
    }
    for (const ReuseMark mark : plan.marks) {
        if (mark == ReuseMark::relist) plan.relisted += 1;
        if (mark == ReuseMark::rescan) plan.rescanned += 1;
    }
    return plan;
}

} // namespace cmac
