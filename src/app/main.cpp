// cmac: a disk-space diagnostic for the Mac. Scans the volume group and shows
// it as a cushion treemap.
//
// usage: cmac [--root PATH] [--capture FILE.bmp]

#include "app/main_window.hpp"

#include "gui_forms/application.hpp"

#include <cstdio>
#include <exception>
#include <memory>
#include <string_view>

namespace {

bool parse_arguments(const int argc, char** const argv, cmac::AppOptions& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        const bool has_value = i + 1 < argc;
        if (argument == "--root" && has_value) {
            i = i + 1;
            options.root = argv[i];
        } else if (argument == "--capture" && has_value) {
            i = i + 1;
            options.capture_path = argv[i];
        } else if (argument.rfind("-psn_", 0) == 0) {
            // A process serial number from Launch Services; ignored.
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

int main(const int argc, char** const argv) {
    cmac::AppOptions options{};
    if (!parse_arguments(argc, argv, options)) {
        std::fprintf(stderr, "usage: cmac [--root PATH] [--capture FILE.bmp]\n");
        return 64;
    }
    try {
        cmac::MainWindow window(options);
        std::unique_ptr<gui_forms::Window> model =
            std::make_unique<gui_forms::Window>(window.root, gui_forms::Size{1280.0, 800.0});
        window.start(*model);
        gui_forms::ApplicationWindowOptions window_options{};
        window_options.title = "cmac";
        window_options.initial_size = {1280.0, 800.0};
        window_options.minimum_size = {600.0, 420.0};
        window_options.ready = cmac::HostReady{&window};
        const gui_forms::ApplicationResult result = gui_forms::Application::run(std::move(model), window_options);
        return result.accepted() ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "cmac: %s\n", error.what());
        return 1;
    }
}
