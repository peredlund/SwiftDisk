// SPDX-FileCopyrightText: 2026 Per Edlund
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/file_tree.h"

namespace swift_disk::visualization
{

struct TreemapLayoutOptions
{
    double width = 1600.0;
    double height = 900.0;
    double tile_padding = 2.0;
    double directory_header_height = 18.0;
    double minimum_child_side = 12.0;
    double minimum_label_width = 72.0;
    double minimum_label_height = 28.0;
    double minimum_child_ratio = 0.0;
    size_t max_depth = 6;
    size_t max_children_per_directory = 0;
    size_t max_rectangles = 4000;
    std::vector<uint64_t> expanded_node_ids;
};

struct TreemapRect
{
    uint64_t node_id = 0;
    uint64_t size = 0;
    size_t depth = 0;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    bool is_dir = false;
    std::wstring label;
    std::string fill_color;
    std::string label_text_color;
    std::string secondary_text_color;
    std::string text_outline_color;
    std::string text_plate_color;
};

struct TreemapVisualStyle
{
    std::string fill_color;
    std::string label_text_color;
    std::string secondary_text_color;
    std::string text_outline_color;
    std::string text_plate_color;
};

std::vector<TreemapRect> BuildTreemapLayout(const FileTree& tree, const TreemapLayoutOptions& options);
std::vector<TreemapRect> BuildTreemapLayout(const FileTree& tree, uint64_t root_id, const TreemapLayoutOptions& options);
TreemapVisualStyle BuildTreemapVisualStyle(
    const std::wstring& label,
    const std::wstring& path,
    bool is_dir);

} // namespace swift_disk::visualization
