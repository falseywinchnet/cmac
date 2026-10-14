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

# Stage S1 needs only GUIForms::Threading. The host, Skia and the text
# engine are switched on in stage S6, with the application.
set(GUI_FORMS_BUILD_GALLERY OFF CACHE BOOL "" FORCE)
set(GUI_FORMS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GUI_FORMS_ENABLE_SKIA OFF CACHE BOOL "" FORCE)
set(GUI_FORMS_ENABLE_MACOS_HOST OFF CACHE BOOL "" FORCE)
set(GUI_FORMS_ENABLE_HARFBUZZ_TEXT OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAC_GUI_FORMS_SOURCE_DIR}" gui_forms EXCLUDE_FROM_ALL)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(cmac_scan STATIC
    src/scan/directory_reader.cpp
    src/scan/scanner.cpp
    src/scan/snapshot_builder.cpp)
target_include_directories(cmac_scan PUBLIC src)
target_compile_features(cmac_scan PUBLIC cxx_std_20)
target_compile_options(cmac_scan PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off)
target_link_libraries(cmac_scan PUBLIC GUIForms::Threading)

add_executable(cmac-scan src/tools/cmac_scan.cpp)
target_compile_options(cmac-scan PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(cmac-scan PRIVATE cmac_scan)

include(CTest)
if(BUILD_TESTING)
    add_executable(cmac_scan_tests tests/scan_tests.cpp)
    target_compile_options(cmac_scan_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(cmac_scan_tests PRIVATE cmac_scan)
    add_test(NAME cmac_scan_tests COMMAND cmac_scan_tests)
    set_tests_properties(cmac_scan_tests PROPERTIES TIMEOUT 120)
    add_test(NAME cmac_house_style
             COMMAND python3 -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_house_style.py"
                     "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/tests")
endif()
