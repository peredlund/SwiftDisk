#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/file_tree.h"

namespace space_scanner::visualization {

struct TreemapLayoutOptions {
    double width = 1600.0;
    double height = 900.0;
    double tile_padding = 2.0;
    double directory_header_height = 18.0;
    double minimum_child_side = 12.0;
    double minimum_label_width = 72.0;
    double minimum_label_height = 28.0;
    double minimum_child_ratio = 0.0025;
    size_t max_depth = 6;
    size_t max_children_per_directory = 48;
    size_t max_rectangles = 4000;
};

struct TreemapDocumentInfo {
    std::wstring scan_path;
    std::wstring backend_name;
    double elapsed_seconds = 0.0;
};

struct TreemapRect {
    uint64_t node_id = 0;
    uint64_t size = 0;
    size_t depth = 0;
    size_t aggregated_child_count = 0;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    bool is_dir = false;
    bool is_aggregate = false;
    std::wstring label;
    std::wstring path;
    std::string fill_color;
    std::string label_text_color;
    std::string secondary_text_color;
    std::string text_outline_color;
    std::string text_plate_color;
};

std::vector<TreemapRect> BuildTreemapLayout(const FileTree& tree, const TreemapLayoutOptions& options);
std::vector<TreemapRect> BuildTreemapLayout(const FileTree& tree, uint64_t root_id, const TreemapLayoutOptions& options);

bool WriteTreemapHtml(
    const std::wstring& output_path,
    const FileTree& tree,
    const TreemapDocumentInfo& info,
    const TreemapLayoutOptions& options,
    std::wstring* error);

} // namespace space_scanner::visualization
