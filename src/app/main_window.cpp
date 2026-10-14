#include "app/main_window.hpp"

#include "platform/macos/show_in_finder.hpp"
#include "treemap/treemap.hpp"

#include "gui_forms/event.hpp"

#include <cstdio>
#include <exception>
#include <utility>

namespace cmac {
namespace {

inline constexpr double header_height = 76.0;

std::string format_bytes(const std::uint64_t bytes) {
    const double value = static_cast<double>(bytes);
    char text[64] = {};
    if (value >= 1024.0 * 1024.0 * 1024.0) {
        std::snprintf(text, sizeof text, "%.2f GiB", value / (1024.0 * 1024.0 * 1024.0));
    } else if (value >= 1024.0 * 1024.0) {
        std::snprintf(text, sizeof text, "%.1f MiB", value / (1024.0 * 1024.0));
    } else if (value >= 1024.0) {
        std::snprintf(text, sizeof text, "%.1f KiB", value / 1024.0);
    } else {
        std::snprintf(text, sizeof text, "%llu bytes", static_cast<unsigned long long>(bytes));
    }
    const std::string result(text);
    return result;
}

std::string format_count(const std::uint64_t count) {
    std::string digits = std::to_string(count);
    std::string result{};
    const std::size_t length = digits.size();
    for (std::size_t i = 0; i < length; ++i) {
        if (i > 0 && (length - i) % 3 == 0) result.push_back(',');
        result.push_back(digits[i]);
    }
    return result;
}

const char* kind_name(const Node& node) {
    if (node.kind == NodeKind::directory) {
        if (node.directory_state == DirectoryState::refused) return "folder, protected (size unknown)";
        if (node.directory_state == DirectoryState::mount_point) return "folder, another volume mounted here";
        if (node.directory_state == DirectoryState::failed) return "folder, could not be read";
        return "folder";
    }
    if (node.kind == NodeKind::symbolic_link) return "symbolic link";
    if (node.kind == NodeKind::file) return "file";
    return "other";
}

std::string flags_text(const Node& node) {
    std::string result{};
    if (has_flag(node, NodeFlag::purgeable)) result += ", purgeable";
    if (has_flag(node, NodeFlag::sparse)) result += ", sparse";
    if (has_flag(node, NodeFlag::may_share_blocks)) result += ", may share blocks with a clone";
    if (has_flag(node, NodeFlag::dataless)) result += ", in iCloud only";
    if (has_flag(node, NodeFlag::compressed)) result += ", compressed";
    if (has_flag(node, NodeFlag::hard_link_repeat)) result += ", another link to a file counted elsewhere";
    return result;
}

std::string elapsed_seconds(const std::chrono::steady_clock::time_point started) {
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;
    char text[32] = {};
    std::snprintf(text, sizeof text, "%.1f s", elapsed.count());
    const std::string result(text);
    return result;
}

} // namespace

void HostReady::operator()(gf::Window&, gf::ApplicationWindowHandle handle) const {
    (*window).attach_host(handle);
}

MainWindow::MainWindow(AppOptions options) : options_(std::move(options)) {
    root = gf::make_control<gf::Panel>(gf::StableId("cmac.root"));
    header_ = gf::make_control<gf::Panel>(gf::StableId("cmac.header"));
    status_ = gf::make_control<gf::Label>(gf::StableId("cmac.status"), "Starting.");
    path_ = gf::make_control<gf::Label>(gf::StableId("cmac.path"), "");
    detail_ = gf::make_control<gf::Label>(gf::StableId("cmac.detail"), "");
    show_in_finder_ = gf::make_control<gf::Button>(gf::StableId("cmac.show_in_finder"), "Show in Finder");
    treemap_ = gf::make_control<TreemapView>(gf::StableId("cmac.treemap"));

    (*status_).set_bounds({12.0, 6.0, 4000.0, 20.0});
    (*path_).set_bounds({12.0, 28.0, 4000.0, 20.0});
    (*detail_).set_bounds({12.0, 50.0, 4000.0, 20.0});
    (*show_in_finder_).set_bounds({12.0, 6.0, 130.0, 28.0});
    (*show_in_finder_).set_dock(gf::DockStyle::right);
    (*header_).set_bounds({0.0, 0.0, 1280.0, header_height});
    (*header_).set_dock(gf::DockStyle::top);
    (*treemap_).set_dock(gf::DockStyle::fill);

    (*header_).add_child(status_);
    (*header_).add_child(path_);
    (*header_).add_child(detail_);
    (*header_).add_child(show_in_finder_);
    // Docking takes children backmost first: the header claims the top
    // before the treemap fills what is left.
    (*root).add_child(header_);
    (*root).add_child(treemap_);

    gf::on((*treemap_).selection_changed(), *this, &MainWindow::on_selection_changed);
    gf::on((*treemap_).reveal_requested(), *this, &MainWindow::on_reveal);
    gf::on((*treemap_).picture_ready(), *this, &MainWindow::on_picture_ready);
    gf::on((*show_in_finder_).clicked(), *this, &MainWindow::on_reveal);
}

MainWindow::~MainWindow() = default;

void MainWindow::start(gf::Window& window) {
    started_ = std::chrono::steady_clock::now();
    ScanOptions scan_options{};
    scan_options.root = options_.root;
    scan_ = std::make_unique<Scan>(scan_options);
    space_ = measure_container_space(options_.root);
    poll_timer_ = std::make_unique<gf::Timer>(window, std::chrono::milliseconds(100));
    gf::on((*poll_timer_).tick(), *this, &MainWindow::poll);
    (*poll_timer_).start();
    show_status("Scanning " + options_.root + " ...");
}

void MainWindow::attach_host(const gf::ApplicationWindowHandle handle) {
    host_ = handle;
}

void MainWindow::show_status(const std::string& text) {
    (*status_).set_text(text);
}

void MainWindow::poll() {
    if (!scan_) return;
    if (!(*scan_).finished()) {
        const ScanProgress& progress = (*scan_).progress();
        const std::uint64_t entries = progress.entries_seen.load(std::memory_order_relaxed);
        const std::uint64_t bytes = progress.allocated_bytes.load(std::memory_order_relaxed);
        show_status("Scanning " + options_.root + ": " + format_count(entries) + " objects, " + format_bytes(bytes) +
                    " (" + elapsed_seconds(started_) + ")");
        return;
    }
    (*poll_timer_).stop();
    finish_scan();
}

void MainWindow::finish_scan() {
    (*scan_).wait();
    const ScanOutcome outcome = (*scan_).outcome();
    statistics_ = (*scan_).statistics();
    if (outcome != ScanOutcome::complete) {
        show_status("The scan of " + options_.root + " did not complete: " + (*scan_).failure());
        scan_.reset();
        return;
    }
    snapshot_ = std::make_shared<const Snapshot>((*scan_).take_snapshot());
    scan_.reset();
    const Node& volume = (*snapshot_).nodes[root_node.value];
    char walk[32] = {};
    std::snprintf(walk, sizeof walk, "%.1f s", statistics_.walk_seconds);
    summary_ = options_.root + ": " + format_count(volume.descendant_count) + " objects, " +
               format_bytes(volume.allocated_bytes) + " in " + walk + "; free " + format_bytes(space_.free_bytes) +
               ", virtual memory " + format_bytes(space_.virtual_memory_bytes);
    if (statistics_.refused_directories > 0)
        summary_ += "; " + format_count(statistics_.refused_directories) + " protected folders not counted";
    show_status(summary_);
    (*treemap_).set_snapshot(snapshot_, space_);
}

void MainWindow::on_selection_changed() {
    const TreemapSelection selection = (*treemap_).selection();
    if (!selection.valid) {
        (*path_).set_text(snapshot_ ? "Click a rectangle to select it; click it again for its folder." : "");
        (*detail_).set_text("");
        return;
    }
    if (selection.category == TileCategory::free_space) {
        (*path_).set_text("Free space in the container");
        (*detail_).set_text(format_bytes(space_.free_bytes) + ", shared by every volume in the container");
        return;
    }
    if (selection.category == TileCategory::virtual_memory) {
        (*path_).set_text("/System/Volumes/VM (virtual memory)");
        (*detail_).set_text(format_bytes(space_.virtual_memory_bytes) + ", swap files and the sleep image");
        return;
    }
    if (!snapshot_ || selection.node == no_node) return;
    const Node& node = (*snapshot_).nodes[selection.node.value];
    (*path_).set_text(path_of(*snapshot_, selection.node));
    std::string detail = format_bytes(node.allocated_bytes) + ", " + kind_name(node);
    if (node.kind == NodeKind::directory)
        detail += ", " + format_count(node.descendant_count) + " objects inside";
    detail += flags_text(node);
    if (selection.category == TileCategory::temporary) detail += "; in a folder cmac believes is temporary";
    (*detail_).set_text(detail);
}

void MainWindow::on_reveal() {
    const TreemapSelection selection = (*treemap_).selection();
    if (!selection.valid) return;
    std::string path{};
    if (selection.category == TileCategory::virtual_memory) path = "/System/Volumes/VM";
    if (snapshot_ && selection.node != no_node) path = path_of(*snapshot_, selection.node);
    if (path.empty()) return;
    if (!show_in_finder(path)) show_status("Finder could not show " + path);
}

void MainWindow::on_picture_ready() {
    char timing[96] = {};
    std::snprintf(timing, sizeof timing, "; layout %.0f ms, paint %.0f ms, %zu tiles",
                  (*treemap_).layout_milliseconds(), (*treemap_).paint_milliseconds(), (*treemap_).tile_count());
    show_status(summary_ + timing);
    if (options_.capture_path.empty()) return;
    const bool written = (*treemap_).write_bitmap(options_.capture_path);
    std::printf("%s%s\ncapture %s: %s\n", summary_.c_str(), timing, options_.capture_path.c_str(),
                written ? "written" : "failed");
    std::fflush(stdout);
    static_cast<void>(host_.request_close());
}

} // namespace cmac
