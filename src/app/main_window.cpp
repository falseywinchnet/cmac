#include "app/main_window.hpp"

#include "platform/macos/show_in_finder.hpp"
#include "treemap/treemap.hpp"

#include "gui_forms/event.hpp"

#include <cstdio>
#include <ctime>
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

void LoadTask::run(const gf::CancellationFlag&, void* const context) {
    LoadTask& task = *static_cast<LoadTask*>(context);
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    if (!task.path.empty()) task.ok = read_snapshot_file(task.path, *task.loaded, task.error);
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;
    task.seconds = elapsed.count();
    task.done.store(true, std::memory_order_release);
}

void PlanTask::run(const gf::CancellationFlag&, void* const context) {
    PlanTask& task = *static_cast<PlanTask*>(context);
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    task.plan = plan_refresh(task.saved.get(), task.root, RefreshPolicy{});
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;
    task.seconds = elapsed.count();
    task.done.store(true, std::memory_order_release);
}

void SaveTask::run(const gf::CancellationFlag&, void* const context) {
    SaveTask& task = *static_cast<SaveTask*>(context);
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    task.ok = write_snapshot_file(task.path, *task.snapshot, task.info, task.error);
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;
    task.seconds = elapsed.count();
    task.done.store(true, std::memory_order_release);
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
    // The container's free space and virtual memory belong beside the whole
    // volume group, not beside one folder.
    if (options_.root == "/") space_ = measure_container_space(options_.root);
    cache_path_ = default_snapshot_path(options_.root);
    poll_timer_ = std::make_unique<gf::Timer>(window, std::chrono::milliseconds(50));
    gf::on((*poll_timer_).tick(), *this, &MainWindow::poll);
    load_task_ = std::make_unique<LoadTask>();
    (*load_task_).path = cache_path_;
    (*load_task_).loaded = std::make_shared<LoadedSnapshot>();
    load_worker_ = std::make_unique<gf::Worker>(LoadTask::run, load_task_.get());
    phase_ = Phase::loading;
    (*poll_timer_).start();
    show_status("Opening the saved scan of " + options_.root + " ...");
}

void MainWindow::attach_host(const gf::ApplicationWindowHandle handle) {
    host_ = handle;
}

void MainWindow::show_status(const std::string& text) {
    (*status_).set_text(text);
}

void MainWindow::poll() {
    if (phase_ == Phase::loading) {
        if ((*load_task_).done.load(std::memory_order_acquire)) finish_load();
        return;
    }
    if (phase_ == Phase::planning) {
        if ((*plan_task_).done.load(std::memory_order_acquire)) finish_plan();
        return;
    }
    if (phase_ == Phase::scanning) {
        if (!(*scan_).finished()) {
            const ScanProgress& progress = (*scan_).progress();
            const std::uint64_t entries = progress.entries_seen.load(std::memory_order_relaxed);
            const std::uint64_t bytes = progress.allocated_bytes.load(std::memory_order_relaxed);
            const std::string what = refresh_reason_.empty() ? "Scanning " : "Updating ";
            show_status(what + options_.root + ": " + format_count(entries) + " objects, " + format_bytes(bytes) +
                        " (" + elapsed_seconds(started_) + ")");
            return;
        }
        finish_scan();
        return;
    }
    if (phase_ == Phase::saving) {
        if ((*save_task_).done.load(std::memory_order_acquire)) finish_save();
        return;
    }
    (*poll_timer_).stop();
}

// The saved scan is shown at once; the refresh is planned behind it.
void MainWindow::finish_load() {
    (*load_worker_).join();
    load_worker_.reset();
    std::unique_ptr<LoadTask> task = std::move(load_task_);
    plan_task_ = std::make_unique<PlanTask>();
    (*plan_task_).root = options_.root;
    if ((*task).ok) {
        saved_ = (*task).loaded;
        // The snapshot is shared from the loaded record it lives in.
        snapshot_ = std::shared_ptr<const Snapshot>(saved_, &(*saved_).snapshot);
        (*plan_task_).saved = saved_;
        const std::int64_t age = static_cast<std::int64_t>(std::time(nullptr)) - (*saved_).info.written_unix_seconds;
        char text[160] = {};
        std::snprintf(text, sizeof text, "Showing the scan saved %lld min ago (opened in %.2f s); checking for changes ...",
                      static_cast<long long>(age / 60), (*task).seconds);
        show_status(text);
        (*treemap_).set_snapshot(snapshot_, space_);
    } else {
        show_status("Scanning " + options_.root + " ...");
    }
    plan_worker_ = std::make_unique<gf::Worker>(PlanTask::run, plan_task_.get());
    phase_ = Phase::planning;
}

void MainWindow::finish_plan() {
    (*plan_worker_).join();
    plan_worker_.reset();
    std::unique_ptr<PlanTask> task = std::move(plan_task_);
    RefreshPlan& plan = (*task).plan;
    scan_position_ = plan.position;
    refresh_reason_ = plan.incremental ? plan.reason : std::string{};
    ScanOptions scan_options{};
    scan_options.root = options_.root;
    if (plan.incremental) {
        scan_options.previous = std::shared_ptr<const Snapshot>(saved_, &(*saved_).snapshot);
        scan_options.marks = std::move(plan.reuse.marks);
    } else if (saved_) {
        show_status("Scanning " + options_.root + " again: " + plan.reason);
    }
    started_ = std::chrono::steady_clock::now();
    scan_ = std::make_unique<Scan>(std::move(scan_options));
    phase_ = Phase::scanning;
}

void MainWindow::finish_scan() {
    (*scan_).wait();
    const ScanOutcome outcome = (*scan_).outcome();
    statistics_ = (*scan_).statistics();
    if (outcome != ScanOutcome::complete) {
        show_status("The scan of " + options_.root + " did not complete: " + (*scan_).failure());
        scan_.reset();
        phase_ = Phase::idle;
        return;
    }
    snapshot_ = std::make_shared<const Snapshot>((*scan_).take_snapshot());
    scan_.reset();
    const Node& volume = (*snapshot_).nodes[root_node.value];
    char walk[64] = {};
    std::snprintf(walk, sizeof walk, "%.1f s", statistics_.walk_seconds + statistics_.build_seconds);
    summary_ = options_.root + ": " + format_count(volume.descendant_count) + " objects, " +
               format_bytes(volume.allocated_bytes) + "; free " + format_bytes(space_.free_bytes) +
               ", virtual memory " + format_bytes(space_.virtual_memory_bytes);
    if (refresh_reason_.empty()) {
        summary_ += "; scanned in " + std::string(walk);
    } else {
        summary_ += "; updated in " + std::string(walk) + " (" + format_count(statistics_.directories_listed) +
                    " folders reread, " + format_count(statistics_.directories_reused) + " unchanged)";
    }
    if (statistics_.refused_directories > 0)
        summary_ += "; " + format_count(statistics_.refused_directories) + " protected folders not counted";
    show_status(summary_);
    refreshed_shown_ = true;
    (*treemap_).set_snapshot(snapshot_, space_);
    start_save();
}

void MainWindow::start_save() {
    if (cache_path_.empty()) {
        phase_ = Phase::idle;
        return;
    }
    save_task_ = std::make_unique<SaveTask>();
    (*save_task_).path = cache_path_;
    (*save_task_).snapshot = snapshot_;
    (*save_task_).info.root = options_.root;
    (*save_task_).info.journal_event_id = scan_position_.event_id;
    (*save_task_).info.journal_store = scan_position_.store;
    (*save_task_).info.system_build = current_system_build();
    (*save_task_).info.written_unix_seconds = static_cast<std::int64_t>(std::time(nullptr));
    save_worker_ = std::make_unique<gf::Worker>(SaveTask::run, save_task_.get());
    phase_ = Phase::saving;
}

void MainWindow::finish_save() {
    (*save_worker_).join();
    save_worker_.reset();
    std::unique_ptr<SaveTask> task = std::move(save_task_);
    if (!(*task).ok) show_status(summary_ + "; the scan could not be saved: " + (*task).error);
    phase_ = Phase::idle;
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
    // The saved picture keeps the status the load wrote; only the refreshed
    // one reports its layout and paint.
    if (!refreshed_shown_) return;
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
