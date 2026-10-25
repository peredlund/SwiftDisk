cmake_minimum_required(VERSION 3.20)
project(SwiftDisk VERSION 1.2.0 LANGUAGES CXX)

if (NOT WIN32)
    message(FATAL_ERROR
        "SwiftDisk only supports Windows."
    )
endif()

if (NOT MSVC)
    message(FATAL_ERROR
        "SwiftDisk requires a Microsoft Visual C++ compiler (MSVC)."
    )
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(swift_disk_core
    src/common/file_tree.cpp
    src/common/file_tree.h
    src/common/stopwatch.cpp
    src/common/stopwatch.h
    src/core/dir_scanner.cpp
    src/core/dir_scanner.h
    src/core/ntfs/mft_parser.cpp
    src/core/ntfs/mft_parser.h
    src/core/ntfs/mft_extents.cpp
    src/core/ntfs/mft_extents.h
    src/core/ntfs/mft_scanner.cpp
    src/core/ntfs/mft_scanner.h
    src/core/ntfs/ntfs_volume.cpp
    src/core/ntfs/ntfs_volume.h
    src/visualization/treemap.cpp
    src/visualization/treemap.h
)

target_include_directories(swift_disk_core PUBLIC
    src
)

target_compile_options(swift_disk_core PRIVATE
    /W4
    /permissive-
    /EHsc
)

target_compile_definitions(swift_disk_core PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

set(SWIFT_DISK_GUI_RESOURCES)
if (WIN32)
    enable_language(RC)
    list(APPEND SWIFT_DISK_GUI_RESOURCES
        src/gui/swift_disk_gui.rc
    )
endif()

configure_file(
    src/gui/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/gui/version.h
    @ONLY
)

add_executable(swift_disk_gui WIN32
    src/gui/app_settings.cpp
    src/gui/app_settings.h
    src/gui/direct2d_renderer.cpp
    src/gui/direct2d_renderer.h
    src/gui/drawing_state.h
    src/gui/info_window.cpp
    src/gui/info_window.h
    src/gui/main.cpp
    src/gui/resource.h
    ${SWIFT_DISK_GUI_RESOURCES}
)

set_property(DIRECTORY PROPERTY
    VS_STARTUP_PROJECT swift_disk_gui
)

set_target_properties(swift_disk_gui PROPERTIES
    OUTPUT_NAME SwiftDisk
)

target_link_libraries(swift_disk_gui PRIVATE
    swift_disk_core
    advapi32
    d2d1
    dwrite
    shell32
)

target_include_directories(swift_disk_gui PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_link_options(swift_disk_gui PRIVATE
    "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'"
)

target_compile_options(swift_disk_gui PRIVATE
    /W4
    /permissive-
    /EHsc
)

target_compile_definitions(swift_disk_gui PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

option(BUILD_TESTING "Build SwiftDisk regression tests" ON)
if (BUILD_TESTING)
    enable_testing()
    add_executable(ntfs_extent_tests tests/ntfs_extent_tests.cpp tests/size_tests.cpp)
    target_link_libraries(ntfs_extent_tests PRIVATE swift_disk_core)
    target_compile_options(ntfs_extent_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_definitions(ntfs_extent_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    add_test(NAME ntfs_extent_tests COMMAND ntfs_extent_tests)
endif()
