#include "treemap/cushion_painter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace cmac {
namespace {

// Base colours, straight RGB 0..255. Red and yellow are reserved for the
// temporary and system tiles, so the file palette avoids them.
struct BaseColour final {
    double r{0.0};
    double g{0.0};
    double b{0.0};
};

inline constexpr std::array<BaseColour, 10> file_palette{{
    {70.0, 120.0, 220.0},
    {60.0, 170.0, 110.0},
    {150.0, 90.0, 210.0},
    {40.0, 170.0, 190.0},
    {110.0, 150.0, 60.0},
    {200.0, 110.0, 190.0},
    {90.0, 90.0, 180.0},
    {50.0, 140.0, 150.0},
    {160.0, 130.0, 220.0},
    {80.0, 180.0, 160.0},
}};

inline constexpr BaseColour no_extension{140.0, 150.0, 170.0};
inline constexpr BaseColour directory_colour{150.0, 150.0, 150.0};
inline constexpr BaseColour temporary_colour{220.0, 55.0, 45.0};
inline constexpr BaseColour free_space_colour{240.0, 214.0, 90.0};
inline constexpr BaseColour virtual_memory_colour{226.0, 176.0, 30.0};
inline constexpr BaseColour background_colour{40.0, 40.0, 44.0};

BaseColour base_colour_of(const Tile& tile) {
    if (tile.category == TileCategory::temporary) return temporary_colour;
    if (tile.category == TileCategory::free_space) return free_space_colour;
    if (tile.category == TileCategory::virtual_memory) return virtual_memory_colour;
    if (tile.category == TileCategory::directory) return directory_colour;
    if (tile.category == TileCategory::container) return directory_colour;
    if (tile.colour_key == 0) return no_extension;
    const BaseColour result = file_palette[tile.colour_key % file_palette.size()];
    return result;
}

// The whole pixels whose centres lie inside the tile, clipped to area.
render::Rect pixel_span(const TileRect& rect, const render::Rect& area) {
    const int x0 = static_cast<int>(std::lround(rect.x0));
    const int y0 = static_cast<int>(std::lround(rect.y0));
    const int x1 = static_cast<int>(std::lround(rect.x1));
    const int y1 = static_cast<int>(std::lround(rect.y1));
    const render::Rect pixels{x0, y0, x1, y1};
    const render::Rect result = pixels.intersected(area);
    return result;
}

std::uint32_t background_pixel(const render::Order order) {
    const std::uint32_t result = render::pack(order, static_cast<float>(background_colour.r),
                                              static_cast<float>(background_colour.g),
                                              static_cast<float>(background_colour.b));
    return result;
}

// One leaf's cushion: the surface normal at each pixel centre, lit by a
// directional light. Doubles throughout; packed to bytes at the end.
void paint_tile(const Tile& tile, const std::uint32_t tile_index, const CushionLight& light,
                const render::Target& target, const std::span<std::uint32_t> pick, const render::Rect& span) {
    const BaseColour base = base_colour_of(tile);
    const CushionSurface& s = tile.surface;
    const std::size_t width = static_cast<std::size_t>(target.width);
    for (int y = span.y0; y < span.y1; ++y) {
        std::uint32_t* const row = target.row(y);
        std::uint32_t* const pick_row = pick.data() + static_cast<std::size_t>(y) * width;
        const double fy = static_cast<double>(y) + 0.5;
        const double ny = -(2.0 * s.b_y * fy + s.a_y);
        for (int x = span.x0; x < span.x1; ++x) {
            const double fx = static_cast<double>(x) + 0.5;
            const double nx = -(2.0 * s.b_x * fx + s.a_x);
            const double facing = nx * light.x + ny * light.y + light.z;
            const double length = std::sqrt(nx * nx + ny * ny + 1.0);
            double intensity = light.ambient;
            if (facing > 0.0) intensity += light.diffuse * facing / length;
            if (intensity > 1.0) intensity = 1.0;
            const float r = static_cast<float>(base.r * intensity);
            const float g = static_cast<float>(base.g * intensity);
            const float b = static_cast<float>(base.b * intensity);
            row[x] = render::pack(target.order, r, g, b);
            pick_row[x] = tile_index;
        }
    }
}

} // namespace

void paint_cushions(const TreemapLayout& layout, const CushionLight& light, const render::Target& target,
                    const std::span<std::uint32_t> pick, const render::Rect area) {
    const std::size_t pixel_count = static_cast<std::size_t>(target.width) * static_cast<std::size_t>(target.height);
    if (pick.size() != pixel_count) throw std::invalid_argument("pick buffer does not match the target");
    const render::Rect clip = area.intersected(target.bounds());
    if (clip.empty()) return;

    // The light direction is normalized once.
    CushionLight unit = light;
    const double norm = std::sqrt(light.x * light.x + light.y * light.y + light.z * light.z);
    unit.x = light.x / norm;
    unit.y = light.y / norm;
    unit.z = light.z / norm;

    const std::uint32_t background = background_pixel(target.order);
    const std::size_t width = static_cast<std::size_t>(target.width);
    for (int y = clip.y0; y < clip.y1; ++y) {
        std::uint32_t* const row = target.row(y);
        std::uint32_t* const pick_row = pick.data() + static_cast<std::size_t>(y) * width;
        std::fill(row + clip.x0, row + clip.x1, background);
        std::fill(pick_row + clip.x0, pick_row + clip.x1, no_tile);
    }
    for (std::size_t i = 0; i < layout.tiles.size(); ++i) {
        const Tile& tile = layout.tiles[i];
        if (!tile.leaf) continue;
        const render::Rect span = pixel_span(tile.rect, clip);
        if (span.empty()) continue;
        paint_tile(tile, static_cast<std::uint32_t>(i), unit, target, pick, span);
    }
}

} // namespace cmac
