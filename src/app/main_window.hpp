#pragma once
// cmac's window: a header with the scan status, the selection and a "Show in
// Finder" button, docked above the treemap. The controller owns the scan and
// polls it on the UI thread; nothing on a scan worker touches a control.

#include "app/treemap_view.hpp"
#include "scan/scanner.hpp"
#include "scan/snapshot.hpp"
#include "space/container_space.hpp"

#include "gui_forms/application.hpp"
#include "gui_forms/basic_controls.hpp"
#include "gui_forms/component.hpp"
#include "gui_forms/timer.hpp"

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
    void poll();
    void finish_scan();
    void on_selection_changed();
    void on_reveal();
    void on_picture_ready();
    void show_status(const std::string& text);

    AppOptions options_{};
    std::shared_ptr<gf::Panel> header_{};
    std::shared_ptr<gf::Label> status_{};
    std::shared_ptr<gf::Label> path_{};
    std::shared_ptr<gf::Label> detail_{};
    std::shared_ptr<gf::Button> show_in_finder_{};
    std::shared_ptr<TreemapView> treemap_{};
    std::shared_ptr<const Snapshot> snapshot_{};
    ContainerSpace space_{};
    ScanStatistics statistics_{};
    std::unique_ptr<Scan> scan_{};
    std::unique_ptr<gf::Timer> poll_timer_{};
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
