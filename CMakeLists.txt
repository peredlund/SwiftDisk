cmake_minimum_required(VERSION 3.20)
project(SpaceScanner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(space_scanner_core
    src/common/file_tree.cpp
    src/common/stopwatch.cpp
    src/core/dir_scanner.cpp
    src/core/ntfs/mft_parser.cpp
    src/core/ntfs/mft_scanner.cpp
    src/core/ntfs/validation.cpp
    src/core/ntfs/ntfs_volume.cpp
    src/visualization/treemap.cpp
)

target_include_directories(space_scanner_core PUBLIC
    src
)

if (MSVC)
    target_compile_options(space_scanner_core PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(space_scanner_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_compile_definitions(space_scanner_core PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

add_executable(space_scanner_cli
    src/cli/main.cpp
)

target_link_libraries(space_scanner_cli PRIVATE space_scanner_core)

if (MSVC)
    target_compile_options(space_scanner_cli PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(space_scanner_cli PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(space_scanner_gui WIN32
    src/gui/main.cpp
)

target_link_libraries(space_scanner_gui PRIVATE
    space_scanner_core
    d2d1
    dwrite
    shell32
)

if (MSVC)
    target_link_options(space_scanner_gui PRIVATE
        "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'"
    )
endif()

if (MSVC)
    target_compile_options(space_scanner_gui PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(space_scanner_gui PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_compile_definitions(space_scanner_gui PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)
