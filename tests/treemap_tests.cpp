// Treemap tests on hand-built snapshots: areas proportional to allocation,
// children inside their parents, the yellow container tiles, the red
// classification, paths, and painting into a target with its pick buffer.

#include "treemap/cushion_painter.hpp"
#include "treemap/treemap.hpp"

#include <cmath>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(const bool condition, const char* const message) {
    if (!condition) throw std::runtime_error(message);
}

// Builds a snapshot breadth first from a description of each directory's
// children, the way the scanner's builder lays it out.
struct SnapshotBuilder final {
    cmac::Snapshot snapshot{};

    cmac::NodeIndex add(const cmac::NodeIndex parent, const std::string_view name, const cmac::NodeKind kind,
                        const std::uint64_t bytes) {
        cmac::Node node{};
        node.parent = parent;
        node.kind = kind;
        node.allocated_bytes = bytes;
        node.directory_state = kind == cmac::NodeKind::directory ? cmac::DirectoryState::listed
                                                                 : cmac::DirectoryState::not_directory;
        node.name.offset = static_cast<std::uint32_t>(snapshot.names.size());
        node.name.length = static_cast<std::uint32_t>(name.size());
        snapshot.names.insert(snapshot.names.end(), name.begin(), name.end());
        const cmac::NodeIndex index{static_cast<std::uint32_t>(snapshot.nodes.size())};
        snapshot.nodes.push_back(node);
        return index;
    }

    // Marks [first, first + count) as the children of parent.
    void adopt(const cmac::NodeIndex parent, const cmac::NodeIndex first, const std::uint32_t count) {
        cmac::Node& node = snapshot.nodes[parent.value];
        node.first_child = first;
        node.child_count = count;
    }

    // Sums sizes upward (children after parents, as breadth first gives).
    void total() {
        for (std::size_t n = snapshot.nodes.size() - 1; n >= 1; --n) {
            const cmac::Node& node = snapshot.nodes[n];
            cmac::Node& parent = snapshot.nodes[node.parent.value];
            parent.descendant_count += 1 + node.descendant_count;
            parent.allocated_bytes += node.allocated_bytes;
        }
    }
};

double area_of(const cmac::TileRect& rect) {
    const double result = (rect.x1 - rect.x0) * (rect.y1 - rect.y0);
    return result;
}

bool inside(const cmac::TileRect& inner, const cmac::TileRect& outer) {
    const double tolerance = 1e-6;
    const bool result = inner.x0 >= outer.x0 - tolerance && inner.y0 >= outer.y0 - tolerance &&
                        inner.x1 <= outer.x1 + tolerance && inner.y1 <= outer.y1 + tolerance;
    return result;
}

// "/" holding five files of sizes 1 to 5, a folder "private/tmp/junk" (red),
// and "Library/Caches" (red).
cmac::Snapshot build_volume() {
    SnapshotBuilder builder{};
    const cmac::NodeIndex root = builder.add(cmac::no_node, "/", cmac::NodeKind::directory, 0);
    const cmac::NodeIndex first = builder.add(root, "a.txt", cmac::NodeKind::file, 1000);
    builder.add(root, "b.jpg", cmac::NodeKind::file, 2000);
    builder.add(root, "c.mov", cmac::NodeKind::file, 3000);
    builder.add(root, "d.bin", cmac::NodeKind::file, 4000);
    builder.add(root, "e", cmac::NodeKind::file, 5000);
    const cmac::NodeIndex private_folder = builder.add(root, "private", cmac::NodeKind::directory, 0);
    const cmac::NodeIndex library = builder.add(root, "Library", cmac::NodeKind::directory, 0);
    builder.adopt(root, first, 7);
    const cmac::NodeIndex tmp = builder.add(private_folder, "tmp", cmac::NodeKind::directory, 0);
    builder.adopt(private_folder, tmp, 1);
    const cmac::NodeIndex caches = builder.add(library, "Caches", cmac::NodeKind::directory, 0);
    builder.adopt(library, caches, 1);
    const cmac::NodeIndex junk = builder.add(tmp, "junk", cmac::NodeKind::file, 6000);
    builder.adopt(tmp, junk, 1);
    const cmac::NodeIndex cached = builder.add(caches, "cached.db", cmac::NodeKind::file, 7000);
    builder.adopt(caches, cached, 1);
    builder.total();
    return builder.snapshot;
}

cmac::NodeIndex find(const cmac::Snapshot& snapshot, const std::string_view name) {
    for (std::size_t n = 0; n < snapshot.nodes.size(); ++n) {
        if (snapshot.name_of(snapshot.nodes[n]) == name) return cmac::NodeIndex{static_cast<std::uint32_t>(n)};
    }
    throw std::runtime_error("node not found");
}

void test_layout() {
    const cmac::Snapshot snapshot = build_volume();
    const cmac::ContainerSpace space{14000, 7000};
    cmac::TreemapLayout layout{};
    const cmac::TreemapParameters parameters{};
    cmac::lay_out_treemap(snapshot, space, 400, 300, parameters, layout);
    require(!layout.tiles.empty() && layout.tiles[0].category == cmac::TileCategory::container, "container first");

    // Every tile lies inside its parent.
    for (const cmac::Tile& tile : layout.tiles) {
        if (tile.parent == cmac::no_tile) continue;
        require(inside(tile.rect, layout.tiles[tile.parent].rect), "a tile lies inside its parent");
    }

    // Areas are proportional to bytes: the volume is 28000 of 49000 bytes.
    const double total_area = 400.0 * 300.0;
    const std::uint32_t volume_tile = layout.tile_of_node[cmac::root_node.value];
    require(volume_tile != cmac::no_tile, "the volume has a tile");
    const double volume_share = area_of(layout.tiles[volume_tile].rect) / total_area;
    require(std::fabs(volume_share - 28000.0 / 49000.0) < 1e-9, "the volume's area is its share");
    const std::uint32_t e_tile = layout.tile_of_node[find(snapshot, "e").value];
    const std::uint32_t a_tile = layout.tile_of_node[find(snapshot, "a.txt").value];
    const double ratio = area_of(layout.tiles[e_tile].rect) / area_of(layout.tiles[a_tile].rect);
    require(std::fabs(ratio - 5.0) < 1e-9, "a file's area is proportional to its size");

    // The yellow tiles.
    bool free_found = false;
    bool memory_found = false;
    for (const cmac::Tile& tile : layout.tiles) {
        if (tile.category == cmac::TileCategory::free_space) free_found = true;
        if (tile.category == cmac::TileCategory::virtual_memory) memory_found = true;
    }
    require(free_found && memory_found, "free space and virtual memory are tiles");

    // Red: private/tmp and Library/Caches with what is inside them.
    const std::uint32_t junk_tile = layout.tile_of_node[find(snapshot, "junk").value];
    const std::uint32_t cached_tile = layout.tile_of_node[find(snapshot, "cached.db").value];
    require(layout.tiles[junk_tile].category == cmac::TileCategory::temporary, "private/tmp is temporary");
    require(layout.tiles[cached_tile].category == cmac::TileCategory::temporary, "Library/Caches is temporary");
    require(layout.tiles[a_tile].category == cmac::TileCategory::file, "an ordinary file is not");

    require(cmac::path_of(snapshot, find(snapshot, "junk")) == "/private/tmp/junk", "path of a nested file");
    require(cmac::path_of(snapshot, cmac::root_node) == "/", "path of the root");
}

// "/" holding folder A, whose only content is a chain twelve folders deep,
// and folder B with four files. Laying out A grows the layout's per-depth
// scratch while the root's level is still being placed: the bug the M4's
// first capture showed (2026-10-14), where B and the rest went missing.
cmac::Snapshot build_deep_volume() {
    SnapshotBuilder builder{};
    const cmac::NodeIndex root = builder.add(cmac::no_node, "/", cmac::NodeKind::directory, 0);
    const cmac::NodeIndex a = builder.add(root, "A", cmac::NodeKind::directory, 0);
    const cmac::NodeIndex b = builder.add(root, "B", cmac::NodeKind::directory, 0);
    builder.adopt(root, a, 2);
    cmac::NodeIndex chain = builder.add(a, "a1", cmac::NodeKind::directory, 0);
    builder.adopt(a, chain, 1);
    const cmac::NodeIndex first_file = builder.add(b, "b1", cmac::NodeKind::file, 100);
    builder.add(b, "b2", cmac::NodeKind::file, 100);
    builder.add(b, "b3", cmac::NodeKind::file, 100);
    builder.add(b, "b4", cmac::NodeKind::file, 100);
    builder.adopt(b, first_file, 4);
    for (int level = 2; level <= 12; ++level) {
        const std::string name = "a" + std::to_string(level);
        const cmac::NodeIndex next = builder.add(chain, name, cmac::NodeKind::directory, 0);
        builder.adopt(chain, next, 1);
        chain = next;
    }
    const cmac::NodeIndex bottom = builder.add(chain, "bottom", cmac::NodeKind::file, 400);
    builder.adopt(chain, bottom, 1);
    builder.total();
    return builder.snapshot;
}

void test_deep_coverage() {
    const cmac::Snapshot snapshot = build_deep_volume();
    cmac::TreemapLayout layout{};
    cmac::lay_out_treemap(snapshot, cmac::ContainerSpace{}, 300, 200, cmac::TreemapParameters{}, layout);
    std::vector<std::uint32_t> pixels(300 * 200, 0);
    std::vector<std::uint32_t> pick(300 * 200, 0);
    const render::Target target{pixels.data(), 300, 300, 200, render::Order::rgba};
    cmac::paint_cushions(layout, cmac::CushionLight{}, target, pick, target.bounds());
    for (const std::uint32_t tile : pick) require(tile != cmac::no_tile, "every pixel belongs to a leaf");
    require(layout.tile_of_node[find(snapshot, "b4").value] != cmac::no_tile, "B's files are laid out");
    require(layout.tile_of_node[find(snapshot, "bottom").value] != cmac::no_tile, "the deep chain is laid out");
}

void test_paint() {
    const cmac::Snapshot snapshot = build_volume();
    cmac::TreemapLayout layout{};
    cmac::lay_out_treemap(snapshot, cmac::ContainerSpace{14000, 7000}, 200, 100, cmac::TreemapParameters{}, layout);
    std::vector<std::uint32_t> pixels(200 * 100, 0);
    std::vector<std::uint32_t> pick(200 * 100, 0);
    const render::Target target{pixels.data(), 200, 200, 100, render::Order::rgba};
    cmac::paint_cushions(layout, cmac::CushionLight{}, target, pick, target.bounds());
    // Every pixel is covered by a leaf: the leaves tile the container.
    std::size_t uncovered = 0;
    for (const std::uint32_t tile : pick) {
        if (tile == cmac::no_tile) {
            uncovered = uncovered + 1;
            continue;
        }
        require(layout.tiles[tile].leaf, "only leaves are picked");
    }
    require(uncovered == 0, "every pixel belongs to a leaf");
    // The cushion is lit: a tile is brighter towards its upper left than at
    // its lower right.
    const std::uint32_t e_tile = layout.tile_of_node[find(snapshot, "e").value];
    const cmac::TileRect rect = layout.tiles[e_tile].rect;
    const int left = static_cast<int>(rect.x0) + 2;
    const int top = static_cast<int>(rect.y0) + 2;
    const int right = static_cast<int>(rect.x1) - 3;
    const int bottom = static_cast<int>(rect.y1) - 3;
    const std::uint32_t lit = pixels[static_cast<std::size_t>(top) * 200 + static_cast<std::size_t>(left)];
    const std::uint32_t shaded = pixels[static_cast<std::size_t>(bottom) * 200 + static_cast<std::size_t>(right)];
    const std::uint32_t lit_green = (lit >> 8) & 0xFFu;
    const std::uint32_t shaded_green = (shaded >> 8) & 0xFFu;
    require(lit_green > shaded_green, "the upper left of a cushion is brighter");
}

} // namespace

int main() {
    try {
        test_layout();
        test_deep_coverage();
        test_paint();
        std::printf("treemap tests passed\n");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "treemap tests failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
