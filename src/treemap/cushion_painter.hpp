#pragma once
// Paints a treemap layout's leaf tiles as lit cushions into a render target,
// and records which tile covers each pixel for picking.

#include "treemap/treemap.hpp"

#include "target.hpp"

#include <cstdint>
#include <span>

namespace cmac {

struct CushionLight final {
    // Direction towards the light (upper left, mostly overhead), normalized
    // by the painter.
    double x{-1.0};
    double y{-1.0};
    double z{10.0};
    double ambient{0.25};
    double diffuse{0.80};
};

// Paints every leaf tile that meets `area` into target and writes its tile
// index into pick (target.width × target.height entries, row-major, no
// padding). Pixels inside `area` that no leaf covers get the background and
// no_tile. Target and pick must not overlap.
void paint_cushions(const TreemapLayout& layout, const CushionLight& light, const render::Target& target,
                    std::span<std::uint32_t> pick, render::Rect area);

} // namespace cmac
