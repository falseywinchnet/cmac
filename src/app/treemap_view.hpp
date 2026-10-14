#pragma once
// The lower panel: the treemap of the last scan, drawn into the window's live
// buffer through the PlaySuite render engine's Surface. Demand-driven: a
// frame is drawn only when the layout, the size or the selection changed,
// and a selection change repairs only the old and new outlines.
//
// Pointer: a click selects the tile under it; a second click on the same
// spot selects its parent; a double click asks to show it in Finder.
// Keys: Backspace selects the parent, Return asks to show it in Finder.

#include "scan/snapshot.hpp"
#include "space/container_space.hpp"
#include "treemap/cushion_painter.hpp"
#include "treemap/treemap.hpp"

#include "surface.hpp"
#include "target.hpp"

#include "gui_forms/basic_controls.hpp"
#include "gui_forms/timer.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cmac {
namespace gf = gui_forms;

struct TreemapSelection final {
    // The selected node, or no_node for a synthetic tile or no selection.
    NodeIndex node{no_node};
    TileCategory category{TileCategory::container};
    bool valid{false};
};

class TreemapView final : public gf::Control {
public:
    explicit TreemapView(gf::StableId id);
    ~TreemapView() override;

    // Shows a finished scan; laid out at the current size on the next frame.
    void set_snapshot(std::shared_ptr<const Snapshot> snapshot, ContainerSpace space);
    [[nodiscard]] TreemapSelection selection() const;
    void select_parent();

    [[nodiscard]] gf::Event<>& selection_changed() noexcept { return selection_changed_; }
    [[nodiscard]] gf::Event<>& reveal_requested() noexcept { return reveal_requested_; }
    // Emitted once after the first complete picture of each snapshot.
    [[nodiscard]] gf::Event<>& picture_ready() noexcept { return picture_ready_; }

    // Writes the current picture (without the selection outline) as a
    // 32-bit BMP. False when there is no picture or the file cannot be written.
    [[nodiscard]] bool write_bitmap(const std::string& path) const;
    // The last layout and full paint, in milliseconds, for the status line.
    [[nodiscard]] double layout_milliseconds() const noexcept { return layout_milliseconds_; }
    [[nodiscard]] double paint_milliseconds() const noexcept { return paint_milliseconds_; }
    [[nodiscard]] std::size_t tile_count() const noexcept { return layout_.tiles.size(); }

    void arrange(gf::Rect bounds) override;
    void on_paint(gf::Painter& painter, gf::Rect damage) override;
    void on_pointer(gf::PointerEvent& event) override;
    void on_key(gf::KeyEvent& event) override;

protected:
    void on_attached_to_window() override;
    void on_detaching_from_window(gf::Window& former_window) noexcept override;

private:
    void request_frame();
    void tick();
    void render_frame();
    void select_tile(std::uint32_t tile);
    [[nodiscard]] render::Rect outline_bounds(std::uint32_t tile) const;
    void draw_outline(const render::Target& target, const render::Rect& repair) const;
    [[nodiscard]] std::uint32_t tile_at(gf::Point local) const;

    std::shared_ptr<const Snapshot> snapshot_{};
    ContainerSpace space_{};
    TreemapParameters parameters_{};
    CushionLight light_{};
    TreemapLayout layout_{};
    // The painted cushions in the surface's byte order, and the tile under
    // each pixel; both device_width_ × device_height_.
    std::vector<std::uint32_t> cushions_{};
    std::vector<std::uint32_t> pick_{};
    render::Surface surface_{};
    bool surface_attached_{false};
    std::unique_ptr<gf::Timer> timer_{};
    double scale_{1.0};
    int device_width_{0};
    int device_height_{0};
    bool layout_dirty_{true};
    bool cushions_dirty_{true};
    bool picture_announced_{false};
    render::Rect pending_damage_{};
    std::uint32_t selected_tile_{no_tile};
    std::uint32_t shown_selection_{no_tile};
    // The pixel of the last primary press, for "click again for the parent".
    gf::Point last_press_{-1.0, -1.0};
    double layout_milliseconds_{0.0};
    double paint_milliseconds_{0.0};
    gf::Event<> selection_changed_{};
    gf::Event<> reveal_requested_{};
    gf::Event<> picture_ready_{};
};

} // namespace cmac
