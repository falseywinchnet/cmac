#pragma once
// cmac's window: a header with the scan status, the selection and a "Show in
// Finder" button, docked above the treemap. The controller owns the scan and
// polls it on the UI thread; nothing on a scan worker touches a control.

#include "app/treemap_view.hpp"
#include "platform/macos/privacy.hpp"
#include "refresh/refresh.hpp"
#include "scan/scanner.hpp"
#include "scan/snapshot.hpp"
#include "scan/snapshot_file.hpp"
#include "space/container_space.hpp"

#include "gui_forms/application.hpp"
#include "gui_forms/basic_controls.hpp"
#include "gui_forms/component.hpp"
#include "gui_forms/threading.hpp"
#include "gui_forms/timer.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

namespace cmac {
namespace gf = gui_forms;

struct AppOptions final {
    std::string root{"/"};
    // When set: write the first complete treemap there as a BMP, print the
    // status line, and close.
    std::string capture_path{};
};

// Work done off the UI thread. Each record is owned by MainWindow and
// borrowed by its Worker until joined; the worker writes the results, then
// sets `done` (release), and the UI thread reads them after an acquire.
struct LoadTask final {
    std::string path{};
    std::shared_ptr<LoadedSnapshot> loaded{};
    bool ok{false};
    std::string error{};
    double seconds{0.0};
    std::atomic<bool> done{false};
    static void run(const gf::CancellationFlag& cancellation, void* context);
};

struct PlanTask final {
    // The saved scan, or null.
    std::shared_ptr<const LoadedSnapshot> saved{};
    std::string root{};
    RefreshPlan plan{};
    double seconds{0.0};
    std::atomic<bool> done{false};
    static void run(const gf::CancellationFlag& cancellation, void* context);
};

struct SaveTask final {
    std::string path{};
    std::shared_ptr<const Snapshot> snapshot{};
    SnapshotInfo info{};
    bool ok{false};
    std::string error{};
    double seconds{0.0};
    std::atomic<bool> done{false};
    static void run(const gf::CancellationFlag& cancellation, void* context);
};

class MainWindow final : public gf::Component {
public:
    explicit MainWindow(AppOptions options);
    ~MainWindow() override;
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    // Starts the scan and its poll timer; the window model must be the one
    // that will run.
    void start(gf::Window& window);
    // From the host once the native window exists.
    void attach_host(gf::ApplicationWindowHandle handle);

    std::shared_ptr<gf::Panel> root{};

private:
    // Launch: load the saved scan and show it, plan the refresh, scan
    // (incrementally when the journal allows), show the result, save it.
    enum class Phase : std::uint8_t { loading, planning, scanning, saving, idle };

    void poll();
    void finish_load();
    void finish_plan();
    void start_save();
    void finish_save();
    void finish_scan();
    void on_selection_changed();
    void on_reveal();
    void on_picture_ready();
    void on_full_disk_access();
    void show_status(const std::string& text);

    AppOptions options_{};
    std::shared_ptr<gf::Panel> header_{};
    std::shared_ptr<gf::Label> status_{};
    std::shared_ptr<gf::Label> path_{};
    std::shared_ptr<gf::Label> detail_{};
    std::shared_ptr<gf::Button> show_in_finder_{};
    std::shared_ptr<gf::Button> full_disk_access_{};
    PrivacyAccess privacy_{};
    std::shared_ptr<TreemapView> treemap_{};
    std::shared_ptr<const Snapshot> snapshot_{};
    ContainerSpace space_{};
    ScanStatistics statistics_{};
    std::unique_ptr<Scan> scan_{};
    std::unique_ptr<gf::Timer> poll_timer_{};
    Phase phase_{Phase::idle};
    std::string cache_path_{};
    std::shared_ptr<const LoadedSnapshot> saved_{};
    std::string refresh_reason_{};
    JournalPosition scan_position_{};
    // Set once the refreshed (not the saved) picture has been handed over.
    bool refreshed_shown_{false};
    // Each task before the worker that borrows it, so the worker is joined
    // (destroyed) first.
    std::unique_ptr<LoadTask> load_task_{};
    std::unique_ptr<gf::Worker> load_worker_{};
    std::unique_ptr<PlanTask> plan_task_{};
    std::unique_ptr<gf::Worker> plan_worker_{};
    std::unique_ptr<SaveTask> save_task_{};
    std::unique_ptr<gf::Worker> save_worker_{};
    gf::ApplicationWindowHandle host_{};
    std::chrono::steady_clock::time_point started_{};
    std::string summary_{};
};

// Forwards the host's ready notification to the window (a named target for
// ApplicationWindowOptions::ready).
struct HostReady final {
    MainWindow* window{nullptr};
    void operator()(gf::Window& model, gf::ApplicationWindowHandle handle) const;
};

} // namespace cmac
