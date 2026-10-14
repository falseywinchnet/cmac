#pragma once
// The treemap: every object large enough to see, as a rectangle whose area is
// its allocation, laid out by the squarified algorithm (Bruls, Huizing and
// van Wijk, 2000) and shaded as a cushion (van Wijk and van de Wetering,
// 1999). Written from the papers.
//
// Coordinates are device pixels, as doubles; a rectangle is
// [x0, x1) × [y0, y1). The layout is built on the UI thread from an
// immutable snapshot and replaced whole when the snapshot or size changes.

#include "scan/snapshot.hpp"
#include "space/container_space.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cmac {

// What a tile is painted as (planning/CLASSIFICATION_STUDY.md).
enum class TileCategory : std::uint8_t {
    file,
    // A directory too small to subdivide.
    directory,
    // Red: tiers R1 to R3 and purgeable files (owner, D4).
    temporary,
    // Yellow: the container's free space.
    free_space,
    // Yellow: swap files and the sleep image (owner, D2).
    virtual_memory,
    // The synthetic tile holding the volume and the yellow space.
    container,
};

struct TileRect final {
    double x0{0.0};
    double y0{0.0};
    double x1{0.0};
    double y1{0.0};
};

// The cushion over a tile: z(x, y) = a_x x + b_x x² + a_y y + b_y y², the sum of
// one parabolic ridge per level in each axis, from the root down.
struct CushionSurface final {
    double a_x{0.0};
    double b_x{0.0};
    double a_y{0.0};
    double b_y{0.0};
};

inline constexpr std::uint32_t no_tile = UINT32_MAX;

struct Tile final {
    TileRect rect{};
    CushionSurface surface{};
    // The snapshot node, or no_node for the synthetic tiles (container, free
    // space, virtual memory).
    NodeIndex node{no_node};
    std::uint32_t parent{no_tile};
    // For files: which palette entry their extension maps to.
    std::uint32_t colour_key{0};
    std::uint16_t depth{0};
    TileCategory category{TileCategory::file};
    // Painted: not subdivided further.
    bool leaf{false};
};

struct TreemapParameters final {
    // Ridge height at the top level, and the factor applied per level (the
    // paper's H and F).
    // 0.88 per level keeps each file's own bubble visible ten levels down
    // (0.88^10 ≈ 0.28); 0.75 flattened small tiles (M4 capture, 2026-10-14).
    double ridge_height{0.45};
    double ridge_scale{0.88};
    // A rectangle narrower than this, in device pixels, is not subdivided.
    double minimum_side{2.0};
};

struct TreemapLayout final {
    // Parents before children; tiles[0] is the container.
    std::vector<Tile> tiles{};
    // For each snapshot node, its tile, or no_tile when it is too small to
    // have one.
    std::vector<std::uint32_t> tile_of_node{};
    int width{0};
    int height{0};
};

// Replaces layout with the treemap of snapshot in a width × height area,
// the volume's files beside the free space and virtual memory. Reuses the
// layout's capacity.
void lay_out_treemap(const Snapshot& snapshot, const ContainerSpace& space, int width, int height,
                     const TreemapParameters& parameters, TreemapLayout& layout);

// The node's absolute path ("/Users/me/file"), built from its ancestors.
[[nodiscard]] std::string path_of(const Snapshot& snapshot, NodeIndex node);

} // namespace cmac
