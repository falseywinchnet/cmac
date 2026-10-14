cmake_minimum_required(VERSION 3.28)
# Configure with GUI.Forms' toolchain file and runtime, as every GUI.Forms
# consumer does (docs/BUILDING.md):
#   cmake -S . -B .build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=.build/deps/gui_forms/cmake/llvm22.cmake
project(cmac VERSION 0.1.0 LANGUAGES C CXX)

include(cmake/pins.cmake)

set(CMAC_GUI_FORMS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.build/deps/gui_forms"
    CACHE PATH "Pinned GUI.Forms source (tools/fetch_dependencies.sh)")
if(NOT EXISTS "${CMAC_GUI_FORMS_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "GUI.Forms not found at ${CMAC_GUI_FORMS_SOURCE_DIR}; run tools/fetch_dependencies.sh")
endif()
execute_process(COMMAND git -C "${CMAC_GUI_FORMS_SOURCE_DIR}" rev-parse HEAD
                OUTPUT_VARIABLE _cmac_gui_forms_head OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE _cmac_gui_forms_git)
if(NOT _cmac_gui_forms_git EQUAL 0 OR NOT _cmac_gui_forms_head STREQUAL CMAC_GUI_FORMS_REVISION)
    message(FATAL_ERROR "GUI.Forms at ${CMAC_GUI_FORMS_SOURCE_DIR} is not the pinned revision "
                        "${CMAC_GUI_FORMS_REVISION}; run tools/fetch_dependencies.sh")
endif()

# The window needs GUI.Forms' macOS host, its CPU Skia renderer and the text
# engine (pass GUI_FORMS_SKIA_PREBUILT and GUI_FORMS_SKIA_OUT, as PlaySuite
# does). Without it only the scanner, cmac-scan and their tests are built,
# and GUI.Forms contributes only GUIForms::Threading.
option(CMAC_BUILD_APPLICATION "Build the cmac window" ON)
set(GUI_FORMS_BUILD_GALLERY OFF CACHE BOOL "" FORCE)
set(GUI_FORMS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GUI_FORMS_ENABLE_SKIA ${CMAC_BUILD_APPLICATION} CACHE BOOL "" FORCE)
set(GUI_FORMS_ENABLE_MACOS_HOST ${CMAC_BUILD_APPLICATION} CACHE BOOL "" FORCE)
set(GUI_FORMS_ENABLE_HARFBUZZ_TEXT ${CMAC_BUILD_APPLICATION} CACHE BOOL "" FORCE)
add_subdirectory("${CMAC_GUI_FORMS_SOURCE_DIR}" gui_forms EXCLUDE_FROM_ALL)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(cmac_scan STATIC
    src/scan/directory_reader.cpp
    src/scan/reuse_plan.cpp
    src/scan/scanner.cpp
    src/scan/snapshot.cpp
    src/scan/snapshot_builder.cpp
    src/scan/snapshot_file.cpp
    src/scan/system_tree.cpp)
target_include_directories(cmac_scan PUBLIC src)
target_compile_features(cmac_scan PUBLIC cxx_std_20)
target_compile_options(cmac_scan PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off)
target_link_libraries(cmac_scan PUBLIC GUIForms::Threading)

# The change journal (FSEvents) and the refresh decision built on it.
add_library(cmac_refresh STATIC
    src/platform/macos/change_journal.cpp
    src/platform/macos/privacy.cpp
    src/refresh/refresh.cpp)
target_compile_options(cmac_refresh PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(cmac_refresh PUBLIC cmac_scan "-framework CoreServices")

add_executable(cmac-scan src/tools/cmac_scan.cpp)
target_compile_options(cmac-scan PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(cmac-scan PRIVATE cmac_refresh)

include(CTest)

if(CMAC_BUILD_APPLICATION)
    set(CMAC_GAMES_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.build/deps/games"
        CACHE PATH "Pinned games repository, for its render engine (tools/fetch_dependencies.sh)")
    set(ENGINE_DIR "${CMAC_GAMES_SOURCE_DIR}/shared/render")
    if(NOT EXISTS "${ENGINE_DIR}/build.cmake")
        message(FATAL_ERROR "The render engine is not at ${ENGINE_DIR}; run tools/fetch_dependencies.sh")
    endif()
    # The engine's own build: render_core, render_ui, and its render_engine test.
    include("${ENGINE_DIR}/build.cmake")

    add_library(cmac_treemap STATIC
        src/space/container_space.cpp
        src/treemap/cushion_painter.cpp
        src/treemap/treemap.cpp)
    target_include_directories(cmac_treemap PUBLIC src)
    target_compile_options(cmac_treemap PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off)
    target_link_libraries(cmac_treemap PUBLIC cmac_scan render_core)

    add_executable(cmac MACOSX_BUNDLE
        src/app/main.cpp
        src/app/main_window.cpp
        src/app/treemap_view.cpp
        src/platform/macos/show_in_finder.cpp
        "${ENGINE_DIR}/ui/surface.cpp")
    target_compile_options(cmac PRIVATE -Wall -Wextra)
    target_link_libraries(cmac PRIVATE cmac_treemap cmac_refresh render_ui GUIForms::Application)
    set_target_properties(cmac PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "cmac"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.falseywinchnet.cmac"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
    # GUI.Forms draws only its bundled faces, read from Resources/fonts.
    add_custom_command(TARGET cmac POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAC_GUI_FORMS_SOURCE_DIR}/assets/fonts"
                "$<TARGET_BUNDLE_CONTENT_DIR:cmac>/Resources/fonts")

    if(BUILD_TESTING)
        add_executable(cmac_treemap_tests tests/treemap_tests.cpp)
        target_compile_options(cmac_treemap_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(cmac_treemap_tests PRIVATE cmac_treemap)
        add_test(NAME cmac_treemap_tests COMMAND cmac_treemap_tests)
    endif()
endif()

if(BUILD_TESTING)
    add_executable(cmac_scan_tests tests/scan_tests.cpp)
    target_compile_options(cmac_scan_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(cmac_scan_tests PRIVATE cmac_scan)
    add_test(NAME cmac_scan_tests COMMAND cmac_scan_tests)
    set_tests_properties(cmac_scan_tests PROPERTIES TIMEOUT 120)
    add_executable(cmac_refresh_tests tests/refresh_tests.cpp)
    target_compile_options(cmac_refresh_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(cmac_refresh_tests PRIVATE cmac_refresh)
    add_test(NAME cmac_refresh_tests COMMAND cmac_refresh_tests)
    set_tests_properties(cmac_refresh_tests PROPERTIES TIMEOUT 120)
    add_test(NAME cmac_house_style
             COMMAND python3 -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_house_style.py"
                     "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/tests")
endif()
