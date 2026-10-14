#include "treemap/treemap.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <string_view>

namespace cmac {
namespace {

// Tiers R1 to R3 (owner, D4, 2026-10-14), as absolute paths split into
// components; "*" matches any one component. A directory that matches is
// red with everything below it.
inline constexpr std::array<std::string_view, 12> temporary_rules{
    "private/tmp",
    "private/var/tmp",
    "private/var/folders/*/*/T",
    "private/var/folders/*/*/C",
    "Library/Caches",
    "System/Library/Caches",
    "Users/*/Library/Caches",
    "Users/*/Library/Containers/*/Data/Library/Caches",
    "Users/*/.cache",
    "Library/Logs",
    "private/var/log",
    "Users/*/Library/Logs",
};

// One child to place: its weight (allocated bytes) and what it is.
struct Item final {
    double weight{0.0};
    NodeIndex node{no_node};
    TileCategory category{TileCategory::file};
};

struct HeavierFirst final {
    bool operator()(const Item& left, const Item& right) const {
        if (left.weight != right.weight) return left.weight > right.weight;
        return left.node.value < right.node.value;
    }
};

void split_components(const std::string_view path, std::vector<std::string_view>& components) {
    components.clear();
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find('/', start);
        if (end == std::string_view::npos) end = path.size();
        if (end > start) components.push_back(path.substr(start, end - start));
        start = end + 1;
    }
}

struct LayoutContext final {
    const Snapshot& snapshot;
    const TreemapParameters& parameters;
    TreemapLayout& layout;
    // The path of the directory being laid out, below "/".
    std::vector<std::string_view> components{};
    std::vector<std::vector<std::string_view>> rules{};
    // Scratch per depth, so a recursion level never overwrites its caller's.
    std::vector<std::vector<Item>> items_by_depth{};
    std::vector<std::vector<TileRect>> rects_by_depth{};
    std::vector<double> ridge_by_depth{};
};

bool matches_rule(const std::vector<std::string_view>& rule, const std::vector<std::string_view>& components) {
    if (rule.size() != components.size()) return false;
    for (std::size_t i = 0; i < rule.size(); ++i) {
        if (rule[i] == "*") continue;
        if (rule[i] != components[i]) return false;
    }
    return true;
}

bool is_temporary_directory(const LayoutContext& context) {
    for (const std::vector<std::string_view>& rule : context.rules) {
        if (matches_rule(rule, context.components)) return true;
    }
    return false;
}

// FNV-1a over the lower-cased extension, so files of one type share a colour.
std::uint32_t colour_key_of(const std::string_view name) {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || name.size() - dot > 12) return 0;
    std::uint32_t hash = 2166136261u;
    for (std::size_t i = dot + 1; i < name.size(); ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        hash = hash ^ static_cast<std::uint8_t>(c);
        hash = hash * 16777619u;
    }
    return hash;
}

double width_of(const TileRect& rect) {
    const double result = rect.x1 - rect.x0;
    return result;
}

double height_of(const TileRect& rect) {
    const double result = rect.y1 - rect.y0;
    return result;
}

// One parabolic ridge across the rectangle in each axis, of height h at its
// centre and zero at its edges (van Wijk and van de Wetering, AddRidge).
void add_ridges(CushionSurface& surface, const TileRect& rect, const double h) {
    const double w = width_of(rect);
    const double v = height_of(rect);
    if (w > 0.0) {
        surface.a_x += 4.0 * h * (rect.x1 + rect.x0) / w;
        surface.b_x -= 4.0 * h / w;
    }
    if (v > 0.0) {
        surface.a_y += 4.0 * h * (rect.y1 + rect.y0) / v;
        surface.b_y -= 4.0 * h / v;
    }
}

// The worst aspect ratio in a row whose items' areas run from largest to
// smallest, laid along a side of the given length.
double worst_ratio(const double largest_area, const double smallest_area, const double row_area,
                   const double side) {
    const double side_squared = side * side;
    const double row_squared = row_area * row_area;
    const double wide = side_squared * largest_area / row_squared;
    const double narrow = row_squared / (side_squared * smallest_area);
    const double result = std::max(wide, narrow);
    return result;
}

// Fills rects[i] for items (heaviest first, all weights positive) inside
// rect: rows are laid along the shorter side and grown while their worst
// aspect ratio improves.
void squarify(const std::span<const Item> items, const TileRect& rect, std::vector<TileRect>& rects) {
    rects.assign(items.size(), TileRect{});
    double remaining = 0.0;
    for (const Item& item : items) remaining += item.weight;
    TileRect free_area = rect;
    std::size_t start = 0;
    while (start < items.size()) {
        const double w = width_of(free_area);
        const double h = height_of(free_area);
        if (w <= 0.0 || h <= 0.0 || remaining <= 0.0) break;
        const double area_per_weight = (w * h) / remaining;
        // A wide area takes a column at its left; a tall one a row at its top.
        const bool column = w >= h;
        const double side = column ? h : w;
        std::size_t end = start;
        double row_weight = 0.0;
        double best = std::numeric_limits<double>::infinity();
        while (end < items.size()) {
            const double candidate = row_weight + items[end].weight;
            const double largest = items[start].weight * area_per_weight;
            const double smallest = items[end].weight * area_per_weight;
            const double ratio = worst_ratio(largest, smallest, candidate * area_per_weight, side);
            if (end > start && ratio > best) break;
            best = ratio;
            row_weight = candidate;
            end = end + 1;
        }
        const double thickness = row_weight * area_per_weight / side;
        double position = column ? free_area.y0 : free_area.x0;
        for (std::size_t i = start; i < end; ++i) {
            const double length = items[i].weight * area_per_weight / thickness;
            if (column) {
                rects[i] = TileRect{free_area.x0, position, free_area.x0 + thickness, position + length};
            } else {
                rects[i] = TileRect{position, free_area.y0, position + length, free_area.y0 + thickness};
            }
            position += length;
        }
        if (column) {
            free_area.x0 += thickness;
        } else {
            free_area.y0 += thickness;
        }
        remaining -= row_weight;
        start = end;
    }
}

double weight_of(const Node& node) {
    if (has_flag(node, NodeFlag::hard_link_repeat)) return 0.0;
    const double result = static_cast<double>(node.allocated_bytes);
    return result;
}

TileCategory category_of(const LayoutContext& context, const TileCategory parent, const Node& node) {
    if (parent == TileCategory::temporary) return TileCategory::temporary;
    if (node.kind == NodeKind::directory) {
        if (is_temporary_directory(context)) return TileCategory::temporary;
        return TileCategory::directory;
    }
    if (has_flag(node, NodeFlag::purgeable)) return TileCategory::temporary;
    return TileCategory::file;
}

void ensure_depth(LayoutContext& context, const std::size_t depth) {
    if (context.items_by_depth.size() > depth) return;
    context.items_by_depth.resize(depth + 1);
    context.rects_by_depth.resize(depth + 1);
    while (context.ridge_by_depth.size() <= depth) {
        const double previous = context.ridge_by_depth.back();
        context.ridge_by_depth.push_back(previous * context.parameters.ridge_scale);
    }
}

bool too_small(const LayoutContext& context, const TileRect& rect) {
    const double minimum = context.parameters.minimum_side;
    const bool result = width_of(rect) < minimum || height_of(rect) < minimum;
    return result;
}

// Subdivides tile `index` (a directory's) into its children, depth first.
// The tile vector grows here, so tiles are re-read by index, never held.
void lay_out_directory(LayoutContext& context, const std::uint32_t index) {
    const Tile parent = context.layout.tiles[index];
    const Node& directory = context.snapshot.nodes[parent.node.value];
    const std::size_t depth = static_cast<std::size_t>(parent.depth) + 1;
    ensure_depth(context, depth);
    // These scratch vectors are re-read by index below: the recursion can
    // grow the per-depth tables and move them.
    std::vector<Item>& items = context.items_by_depth[depth];
    items.clear();
    for (std::uint32_t k = 0; k < directory.child_count; ++k) {
        const NodeIndex child{directory.first_child.value + k};
        const double weight = weight_of(context.snapshot.nodes[child.value]);
        if (weight > 0.0) items.push_back(Item{weight, child, TileCategory::file});
    }
    if (items.empty()) {
        context.layout.tiles[index].leaf = true;
        return;
    }
    std::sort(items.begin(), items.end(), HeavierFirst{});
    squarify(items, parent.rect, context.rects_by_depth[depth]);
    const std::size_t count = items.size();
    const double ridge = context.ridge_by_depth[depth];
    for (std::size_t i = 0; i < count; ++i) {
        const Item item = context.items_by_depth[depth][i];
        const TileRect rect = context.rects_by_depth[depth][i];
        const Node& node = context.snapshot.nodes[item.node.value];
        const std::string_view name = context.snapshot.name_of(node);
        const bool directory_child = node.kind == NodeKind::directory;
        if (directory_child) context.components.push_back(name);
        Tile tile{};
        tile.rect = rect;
        tile.surface = parent.surface;
        add_ridges(tile.surface, tile.rect, ridge);
        tile.node = item.node;
        tile.parent = index;
        tile.depth = static_cast<std::uint16_t>(depth);
        tile.category = category_of(context, parent.category, node);
        tile.colour_key = colour_key_of(name);
        tile.leaf = !directory_child || too_small(context, tile.rect);
        const std::uint32_t tile_index = static_cast<std::uint32_t>(context.layout.tiles.size());
        context.layout.tiles.push_back(tile);
        context.layout.tile_of_node[tile.node.value] = tile_index;
        if (!tile.leaf) lay_out_directory(context, tile_index);
        if (directory_child) context.components.pop_back();
    }
}

} // namespace

void lay_out_treemap(const Snapshot& snapshot, const ContainerSpace& space, const int width, const int height,
                     const TreemapParameters& parameters, TreemapLayout& layout) {
    layout.tiles.clear();
    layout.tile_of_node.assign(snapshot.nodes.size(), no_tile);
    layout.width = width;
    layout.height = height;
    if (width <= 0 || height <= 0 || snapshot.nodes.empty()) return;

    LayoutContext context{snapshot, parameters, layout};
    context.ridge_by_depth.push_back(parameters.ridge_height);
    for (const std::string_view rule : temporary_rules) {
        std::vector<std::string_view> components{};
        split_components(rule, components);
        context.rules.push_back(components);
    }

    Tile container{};
    container.rect = TileRect{0.0, 0.0, static_cast<double>(width), static_cast<double>(height)};
    add_ridges(container.surface, container.rect, parameters.ridge_height);
    container.category = TileCategory::container;
    layout.tiles.push_back(container);

    // The volume beside its free space and virtual memory.
    const Node& root = snapshot.nodes[root_node.value];
    std::vector<Item> items{};
    items.push_back(Item{static_cast<double>(root.allocated_bytes), root_node, TileCategory::directory});
    items.push_back(Item{static_cast<double>(space.free_bytes), no_node, TileCategory::free_space});
    items.push_back(Item{static_cast<double>(space.virtual_memory_bytes), no_node, TileCategory::virtual_memory});
    std::vector<Item> placed{};
    for (const Item& item : items) {
        if (item.weight > 0.0) placed.push_back(item);
    }
    std::sort(placed.begin(), placed.end(), HeavierFirst{});
    std::vector<TileRect> rects{};
    squarify(placed, container.rect, rects);
    ensure_depth(context, 1);
    const double ridge = context.ridge_by_depth[1];
    for (std::size_t i = 0; i < placed.size(); ++i) {
        Tile tile{};
        tile.rect = rects[i];
        tile.surface = container.surface;
        add_ridges(tile.surface, tile.rect, ridge);
        tile.node = placed[i].node;
        tile.parent = 0;
        tile.depth = 1;
        tile.category = placed[i].category;
        tile.leaf = placed[i].node == no_node || too_small(context, tile.rect);
        const std::uint32_t tile_index = static_cast<std::uint32_t>(layout.tiles.size());
        layout.tiles.push_back(tile);
        if (placed[i].node == no_node) continue;
        layout.tile_of_node[root_node.value] = tile_index;
        split_components(snapshot.name_of(root), context.components);
        if (!tile.leaf) lay_out_directory(context, tile_index);
    }
}

std::string path_of(const Snapshot& snapshot, const NodeIndex node) {
    std::vector<std::string_view> names{};
    NodeIndex current = node;
    while (current != no_node && current != root_node) {
        const Node& entry = snapshot.nodes[current.value];
        names.push_back(snapshot.name_of(entry));
        current = entry.parent;
    }
    std::string result(snapshot.name_of(snapshot.nodes[root_node.value]));
    for (std::size_t i = names.size(); i > 0; --i) {
        if (result.empty() || result.back() != '/') result.push_back('/');
        result.append(names[i - 1]);
    }
    return result;
}

} // namespace cmac
