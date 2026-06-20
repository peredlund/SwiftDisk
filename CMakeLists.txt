cmake_minimum_required(VERSION 3.20)
project(SwiftDisk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(swift_disk_core
    src/common/file_tree.cpp
    src/common/stopwatch.cpp
    src/core/dir_scanner.cpp
    src/core/ntfs/mft_parser.cpp
    src/core/ntfs/mft_scanner.cpp
    src/core/ntfs/validation.cpp
    src/core/ntfs/ntfs_volume.cpp
    src/visualization/treemap.cpp
)

target_include_directories(swift_disk_core PUBLIC
    src
)

if (MSVC)
    target_compile_options(swift_disk_core PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(swift_disk_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_compile_definitions(swift_disk_core PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

add_executable(swift_disk_cli
    src/cli/main.cpp
)

target_link_libraries(swift_disk_cli PRIVATE swift_disk_core)

if (MSVC)
    target_compile_options(swift_disk_cli PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(swift_disk_cli PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(swift_disk_gui WIN32
    src/gui/direct2d_renderer.cpp
    src/gui/direct2d_renderer.h
    src/gui/drawing_state.h
    src/gui/main.cpp
)

target_link_libraries(swift_disk_gui PRIVATE
    swift_disk_core
    d2d1
    dwrite
    shell32
)

if (MSVC)
    target_link_options(swift_disk_gui PRIVATE
        "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'"
    )
endif()

if (MSVC)
    target_compile_options(swift_disk_gui PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(swift_disk_gui PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_compile_definitions(swift_disk_gui PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)
