#include "app/treemap_view.hpp"

#include "r2d.hpp"

#include "gui_forms/live_surface.hpp"
#include "gui_forms/window.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <span>

namespace cmac {
namespace {

double milliseconds_since(const std::chrono::steady_clock::time_point start) {
    const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - start;
    const double result = elapsed.count();
    return result;
}

void put_u16(std::FILE* const file, const std::uint16_t value) {
    const unsigned char bytes[2] = {static_cast<unsigned char>(value & 0xFFu),
                                    static_cast<unsigned char>((value >> 8) & 0xFFu)};
    std::fwrite(bytes, 1, sizeof bytes, file);
}

void put_u32(std::FILE* const file, const std::uint32_t value) {
    const unsigned char bytes[4] = {static_cast<unsigned char>(value & 0xFFu),
                                    static_cast<unsigned char>((value >> 8) & 0xFFu),
                                    static_cast<unsigned char>((value >> 16) & 0xFFu),
                                    static_cast<unsigned char>((value >> 24) & 0xFFu)};
    std::fwrite(bytes, 1, sizeof bytes, file);
}

} // namespace

TreemapView::TreemapView(gf::StableId id) : gf::Control(std::move(id)) {
    set_focusable(true);
}

TreemapView::~TreemapView() = default;

void TreemapView::set_snapshot(std::shared_ptr<const Snapshot> snapshot, const ContainerSpace space) {
    snapshot_ = std::move(snapshot);
    space_ = space;
    selected_tile_ = no_tile;
    shown_selection_ = no_tile;
    layout_dirty_ = true;
    cushions_dirty_ = true;
    picture_announced_ = false;
    selection_changed_.emit();
    request_frame();
}

TreemapSelection TreemapView::selection() const {
    TreemapSelection result{};
    if (selected_tile_ == no_tile || selected_tile_ >= layout_.tiles.size()) return result;
    const Tile& tile = layout_.tiles[selected_tile_];
    result.node = tile.node;
    result.category = tile.category;
    result.valid = true;
    return result;
}

void TreemapView::select_parent() {
    if (selected_tile_ == no_tile || selected_tile_ >= layout_.tiles.size()) return;
    const std::uint32_t parent = layout_.tiles[selected_tile_].parent;
    // Tile 0 is the container; it is never selected.
    if (parent == no_tile || parent == 0) return;
    select_tile(parent);
}

void TreemapView::select_tile(const std::uint32_t tile) {
    if (tile == selected_tile_) return;
    selected_tile_ = tile;
    selection_changed_.emit();
    request_frame();
}

std::uint32_t TreemapView::tile_at(const gf::Point local) const {
    const int x = static_cast<int>(std::floor(local.x * scale_));
    const int y = static_cast<int>(std::floor(local.y * scale_));
    if (x < 0 || y < 0 || x >= device_width_ || y >= device_height_) return no_tile;
    const std::size_t index = static_cast<std::size_t>(y) * static_cast<std::size_t>(device_width_) +
                              static_cast<std::size_t>(x);
    if (index >= pick_.size()) return no_tile;
    const std::uint32_t result = pick_[index];
    return result;
}

void TreemapView::arrange(const gf::Rect bounds) {
    arrange_self(bounds);
    gf::Window* const window = attached_window();
    scale_ = window != nullptr ? (*window).scale() : 1.0;
    const int width = std::max(1, static_cast<int>(std::lround(bounds.width * scale_)));
    const int height = std::max(1, static_cast<int>(std::lround(bounds.height * scale_)));
    if (width != device_width_ || height != device_height_) {
        device_width_ = width;
        device_height_ = height;
        layout_dirty_ = true;
        cushions_dirty_ = true;
        if (surface_.live()) static_cast<void>(surface_.configure(device_width_, device_height_));
    }
    request_frame();
}

void TreemapView::on_paint(gf::Painter& painter, gf::Rect) {
    const gf::Rect bounds = client_rectangle();
    if (surface_.live()) {
        painter.draw_live_surface(surface_.live(), bounds);
    } else {
        painter.fill_rect(bounds, gf::Color::rgba(40, 40, 44));
    }
}

void TreemapView::on_pointer(gf::PointerEvent& event) {
    if (event.action != gf::PointerAction::down || event.button != gf::PointerButton::primary) return;
    const gf::Point local = point_from_window(event.position);
    gf::Window* const window = attached_window();
    if (window != nullptr) static_cast<void>((*window).request_focus(shared_from_this()));
    event.handled = true;
    if (event.click_count >= 2) {
        if (selected_tile_ != no_tile) reveal_requested_.emit();
        return;
    }
    const bool same_spot = std::fabs(local.x - last_press_.x) < 1.0 && std::fabs(local.y - last_press_.y) < 1.0;
    last_press_ = local;
    if (same_spot && selected_tile_ != no_tile) {
        select_parent();
        return;
    }
    const std::uint32_t tile = tile_at(local);
    if (tile != no_tile) select_tile(tile);
}

void TreemapView::on_key(gf::KeyEvent& event) {
    if (event.action != gf::KeyAction::down) return;
    if (event.physical_key == gf::PhysicalKey::backspace) {
        select_parent();
        event.handled = true;
    } else if (event.physical_key == gf::PhysicalKey::enter) {
        if (selected_tile_ != no_tile) reveal_requested_.emit();
        event.handled = true;
    }
}

void TreemapView::on_attached_to_window() {
    gf::Control::on_attached_to_window();
    timer_ = std::make_unique<gf::Timer>(*attached_window(), std::chrono::milliseconds(16));
    gf::on((*timer_).tick(), *this, &TreemapView::tick);
    cushions_dirty_ = true;
    request_frame();
}

void TreemapView::on_detaching_from_window(gf::Window& former_window) noexcept {
    if (timer_) (*timer_).stop();
    timer_.reset();
    surface_.reset();
    surface_attached_ = false;
    cushions_dirty_ = true;
    gf::Control::on_detaching_from_window(former_window);
}

void TreemapView::request_frame() {
    if (!timer_) return;
    if (!(*timer_).enabled()) (*timer_).start();
}

void TreemapView::tick() {
    if (timer_) (*timer_).stop();
    render_frame();
}

render::Rect TreemapView::outline_bounds(const std::uint32_t tile) const {
    if (tile == no_tile || tile >= layout_.tiles.size()) return render::Rect{};
    const TileRect& rect = layout_.tiles[tile].rect;
    const render::Rect pixels{static_cast<int>(std::lround(rect.x0)), static_cast<int>(std::lround(rect.y0)),
                              static_cast<int>(std::lround(rect.x1)), static_cast<int>(std::lround(rect.y1))};
    const render::Rect result = pixels.intersected(render::Rect{0, 0, device_width_, device_height_});
    return result;
}

// A white frame inside the selected tile, two logical pixels thick.
void TreemapView::draw_outline(const render::Target& target, const render::Rect& repair) const {
    const render::Rect box = outline_bounds(selected_tile_);
    if (box.empty()) return;
    const int thickness = std::max(1, static_cast<int>(std::lround(2.0 * scale_)));
    const render::Color white{255.0F, 255.0F, 255.0F, 255.0F};
    const render::Rect top{box.x0, box.y0, box.x1, std::min(box.y1, box.y0 + thickness)};
    const render::Rect bottom{box.x0, std::max(box.y0, box.y1 - thickness), box.x1, box.y1};
    const render::Rect left{box.x0, box.y0, std::min(box.x1, box.x0 + thickness), box.y1};
    const render::Rect right{std::max(box.x0, box.x1 - thickness), box.y0, box.x1, box.y1};
    render::r2d::fill(target, top.intersected(repair), white);
    render::r2d::fill(target, bottom.intersected(repair), white);
    render::r2d::fill(target, left.intersected(repair), white);
    render::r2d::fill(target, right.intersected(repair), white);
}

void TreemapView::render_frame() {
    if (device_width_ <= 0 || device_height_ <= 0) return;
    if (!surface_.configure(device_width_, device_height_)) return;
    gf::Window* const window = attached_window();
    if (!surface_attached_ && window != nullptr) {
        static_cast<void>(surface_.attach(*window, shared_from_this()));
        surface_attached_ = true;
        invalidate(gf::Dirty::paint);
    }
    const render::Order order = surface_.order();
    const std::size_t pixel_count = static_cast<std::size_t>(device_width_) * static_cast<std::size_t>(device_height_);

    if (layout_dirty_) {
        const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        if (snapshot_) {
            lay_out_treemap(*snapshot_, space_, device_width_, device_height_, parameters_, layout_);
        } else {
            layout_ = TreemapLayout{};
        }
        layout_milliseconds_ = milliseconds_since(started);
        selected_tile_ = no_tile;
        shown_selection_ = no_tile;
        layout_dirty_ = false;
        cushions_dirty_ = true;
        selection_changed_.emit();
    }
    if (cushions_dirty_ || cushions_.size() != pixel_count) {
        const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        cushions_.assign(pixel_count, 0);
        pick_.assign(pixel_count, no_tile);
        const render::Target cushions{cushions_.data(), device_width_, device_width_, device_height_, order};
        paint_cushions(layout_, light_, cushions, pick_, cushions.bounds());
        paint_milliseconds_ = milliseconds_since(started);
        cushions_dirty_ = false;
        pending_damage_ = cushions.bounds();
        shown_selection_ = no_tile;
    }
    if (selected_tile_ != shown_selection_) {
        pending_damage_ = pending_damage_.united(outline_bounds(shown_selection_));
        pending_damage_ = pending_damage_.united(outline_bounds(selected_tile_));
    }
    if (pending_damage_.empty()) return;

    std::optional<render::Surface::Frame> frame = surface_.begin(pending_damage_);
    if (!frame) {
        // Every buffer is still on screen; try again next tick.
        request_frame();
        return;
    }
    const render::Target& target = (*frame).target;
    const render::Rect repair = (*frame).repair;
    const render::Target cushions{cushions_.data(), device_width_, device_width_, device_height_, order};
    render::r2d::restore(target, cushions, repair);
    draw_outline(target, repair);
    surface_.publish();
    if (!surface_.direct()) invalidate(gf::Dirty::paint);
    shown_selection_ = selected_tile_;
    pending_damage_ = render::Rect{};
    if (!picture_announced_ && snapshot_) {
        picture_announced_ = true;
        picture_ready_.emit();
    }
}

bool TreemapView::write_bitmap(const std::string& path) const {
    const std::size_t pixel_count = static_cast<std::size_t>(device_width_) * static_cast<std::size_t>(device_height_);
    if (pixel_count == 0 || cushions_.size() != pixel_count) return false;
    std::FILE* const file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return false;
    // BITMAPFILEHEADER, then BITMAPINFOHEADER with a negative height (rows
    // top to bottom), 32 bits per pixel as B, G, R, X.
    const std::uint32_t image_bytes = static_cast<std::uint32_t>(pixel_count * 4);
    put_u16(file, 0x4D42);
    put_u32(file, 54 + image_bytes);
    put_u32(file, 0);
    put_u32(file, 54);
    put_u32(file, 40);
    put_u32(file, static_cast<std::uint32_t>(device_width_));
    put_u32(file, static_cast<std::uint32_t>(-device_height_));
    put_u16(file, 1);
    put_u16(file, 32);
    put_u32(file, 0);
    put_u32(file, image_bytes);
    put_u32(file, 2835);
    put_u32(file, 2835);
    put_u32(file, 0);
    put_u32(file, 0);
    const bool rgba = surface_.order() == render::Order::rgba;
    std::vector<unsigned char> row(static_cast<std::size_t>(device_width_) * 4);
    for (int y = 0; y < device_height_; ++y) {
        const std::uint32_t* const source = cushions_.data() + static_cast<std::size_t>(y) * device_width_;
        for (int x = 0; x < device_width_; ++x) {
            const std::uint32_t pixel = source[x];
            const unsigned char low = static_cast<unsigned char>(pixel & 0xFFu);
            const unsigned char green = static_cast<unsigned char>((pixel >> 8) & 0xFFu);
            const unsigned char high = static_cast<unsigned char>((pixel >> 16) & 0xFFu);
            const std::size_t at = static_cast<std::size_t>(x) * 4;
            row[at + 0] = rgba ? high : low;
            row[at + 1] = green;
            row[at + 2] = rgba ? low : high;
            row[at + 3] = 255;
        }
        std::fwrite(row.data(), 1, row.size(), file);
    }
    const bool closed = std::fclose(file) == 0;
    return closed;
}

} // namespace cmac
