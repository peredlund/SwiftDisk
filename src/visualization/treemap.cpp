#include "visualization/treemap.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <queue>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace space_scanner::visualization
{

namespace
{

struct Bounds
{
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

struct LayoutItem
{
    uint64_t node_id = 0;
    uint64_t size = 0;
    bool is_dir = false;
    bool is_hidden = false;
    std::wstring label;
    std::wstring path;
};

struct ChildCollection
{
    std::vector<LayoutItem> items;
    uint64_t total_size = 0;
};

struct PositionedItem
{
    const LayoutItem* item = nullptr;
    Bounds bounds;
    size_t depth = 0;
};

struct SelectionState
{
    uint64_t parent_size = 0;
    size_t child_depth = 0;
    size_t next_child_index = 0;
    size_t visible_child_count = 0;
};

struct ChildCandidate
{
    uint64_t parent_id = 0;
    uint64_t size = 0;
    size_t child_index = 0;
};

struct ChildCandidateLess
{
    bool operator()(const ChildCandidate& left, const ChildCandidate& right) const
    {
        if (left.size != right.size)
        {
            return left.size < right.size;
        }
        if (left.parent_id != right.parent_id)
        {
            return left.parent_id > right.parent_id;
        }
        return left.child_index > right.child_index;
    }
};

std::wstring HumanizeBytes(uint64_t bytes)
{
    static constexpr std::wstring_view kUnits[] = {L"B", L"KiB", L"MiB", L"GiB", L"TiB", L"PiB"};
    double value = static_cast<double>(bytes);
    size_t unit_index = 0;
    while (value >= 1024.0 && unit_index + 1 < std::size(kUnits))
    {
        value /= 1024.0;
        ++unit_index;
    }

    wchar_t buffer[64] = {};
    swprintf_s(buffer, L"%.2f %ls", value, kUnits[unit_index].data());
    return buffer;
}

std::string Utf8FromWide(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }

    int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
    {
        return {};
    }

    std::string utf8(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), utf8.data(), size, nullptr, nullptr);
    return utf8;
}

std::string EscapeHtml(const std::string& value)
{
    std::ostringstream escaped;
    for (unsigned char ch : value)
    {
        switch (ch)
        {
        case '&':
            escaped << "&amp;";
            break;
        case '<':
            escaped << "&lt;";
            break;
        case '>':
            escaped << "&gt;";
            break;
        case '"':
            escaped << "&quot;";
            break;
        case '\'':
            escaped << "&#39;";
            break;
        default:
            escaped << static_cast<char>(ch);
            break;
        }
    }
    return escaped.str();
}

std::string EscapeAttribute(const std::wstring& value)
{
    return EscapeHtml(Utf8FromWide(value));
}

std::string FormatDouble(double value)
{
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream << std::setprecision(2) << value;
    return stream.str();
}

std::string FormatPercent(double ratio)
{
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream << std::setprecision(2) << ratio * 100.0;
    return stream.str();
}

uint64_t SumSizes(const std::vector<LayoutItem>& items, size_t begin, size_t end)
{
    uint64_t total = 0;
    for (size_t i = begin; i < end; ++i)
    {
        total += items[i].size;
    }
    return total;
}

uint64_t HashPath(const std::wstring& value)
{
    uint64_t hash = 1469598103934665603ull;
    for (wchar_t ch : value)
    {
        hash ^= static_cast<uint64_t>(static_cast<uint16_t>(ch));
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string HexColor(uint32_t rgb)
{
    std::ostringstream stream;
    stream << '#'
           << std::hex << std::setfill('0') << std::setw(6)
           << std::nouppercase << rgb;
    return stream.str();
}

uint32_t ParseHexColor(std::string_view value)
{
    if (value.size() != 7 || value[0] != '#')
    {
        return 0x7A96BEu;
    }

    auto hex = [](char ch) -> uint32_t
    {
        if (ch >= '0' && ch <= '9')
        {
            return static_cast<uint32_t>(ch - '0');
        }
        if (ch >= 'a' && ch <= 'f')
        {
            return static_cast<uint32_t>(10 + ch - 'a');
        }
        if (ch >= 'A' && ch <= 'F')
        {
            return static_cast<uint32_t>(10 + ch - 'A');
        }
        return 0;
    };

    return (hex(value[1]) << 20) | (hex(value[2]) << 16) |
           (hex(value[3]) << 12) | (hex(value[4]) << 8) |
           (hex(value[5]) << 4) | hex(value[6]);
}

double SrgbToLinear(double channel)
{
    const double normalized = channel / 255.0;
    if (normalized <= 0.04045)
    {
        return normalized / 12.92;
    }
    return std::pow((normalized + 0.055) / 1.055, 2.4);
}

double RelativeLuminance(uint32_t rgb)
{
    const double red = SrgbToLinear(static_cast<double>((rgb >> 16) & 0xFFu));
    const double green = SrgbToLinear(static_cast<double>((rgb >> 8) & 0xFFu));
    const double blue = SrgbToLinear(static_cast<double>(rgb & 0xFFu));
    return 0.2126 * red + 0.7152 * green + 0.0722 * blue;
}

double ContrastRatio(uint32_t left_rgb, uint32_t right_rgb)
{
    const double left = RelativeLuminance(left_rgb);
    const double right = RelativeLuminance(right_rgb);
    const double lighter = std::max(left, right);
    const double darker = std::min(left, right);
    return (lighter + 0.05) / (darker + 0.05);
}

struct TextPalette
{
    std::string primary;
    std::string secondary;
    std::string outline;
    std::string plate;
};

TextPalette TextColorsForFill(std::string_view fill_color)
{
    const uint32_t rgb = ParseHexColor(fill_color);
    constexpr uint32_t kLightPrimary = 0xF8FBFFu;
    constexpr uint32_t kDarkPrimary = 0x102033u;

    const double light_contrast = ContrastRatio(rgb, kLightPrimary);
    const double dark_contrast = ContrastRatio(rgb, kDarkPrimary);
    if (light_contrast >= dark_contrast)
    {
        return {"#f8fbff", "#f8fbff", "#08111d", "#08111d"};
    }
    return {"#102033", "#102033", "#f8fbff", "#f8fbff"};
}

std::string ColorForItem(const LayoutItem& item, size_t depth)
{
    const uint64_t hash = HashPath(item.path.empty() ? item.label : item.path);
    const uint32_t base = static_cast<uint32_t>(hash & 0x00FFFFFFu);

    uint32_t red = 64u + ((base >> 16) & 0x7Fu);
    uint32_t green = 64u + ((base >> 8) & 0x7Fu);
    uint32_t blue = 64u + (base & 0x7Fu);

    const uint32_t depth_boost = static_cast<uint32_t>(std::min<size_t>(depth * 8, 48));
    if (item.is_dir)
    {
        red = std::min<uint32_t>(red + 18u + depth_boost / 2u, 220u);
        green = std::min<uint32_t>(green + 14u + depth_boost / 2u, 220u);
        blue = std::min<uint32_t>(blue + 10u + depth_boost / 3u, 220u);
    }
    else
    {
        red = std::min<uint32_t>(red + 8u, 210u);
        green = std::min<uint32_t>(green + 8u, 210u);
        blue = std::min<uint32_t>(blue + 8u, 210u);
    }

    return HexColor((red << 16) | (green << 8) | blue);
}

/**
 Collects a directory's real children, drops duplicate ids, ignores zero-sized
 nodes, and sorts the result by descending size.

 The returned order is important for the budgeted treemap strategy: each
 directory exposes its children to the global selection heap one at a time,
 largest first.
 **/
ChildCollection CollectChildren(
    const FileTree& tree,
    const Node& parent)
{
    ChildCollection collection;
    const std::vector<uint64_t>* child_ids = tree.GetChildIds(parent.id);
    if (!child_ids)
    {
        return collection;
    }

    std::unordered_set<uint64_t> seen;
    seen.reserve(child_ids->size());

    for (uint64_t child_id : *child_ids)
    {
        if (!seen.insert(child_id).second)
        {
            continue;
        }

        const Node* child = tree.GetNode(child_id);
        if (!child || child->size == 0)
        {
            continue;
        }

        LayoutItem item;
        item.node_id = child->id;
        item.size = child->size;
        item.is_dir = child->is_dir;
        item.label = child->name.empty() ? L"(unnamed)" : child->name;
        item.path = tree.BuildPath(child->id);
        collection.total_size += item.size;
        collection.items.push_back(std::move(item));
    }

    std::sort(collection.items.begin(), collection.items.end(), [](const LayoutItem& left, const LayoutItem& right)
              {
                  if (left.size != right.size)
                  {
                      return left.size > right.size;
                  }
                  return left.label < right.label;
              });
    return collection;
}

/**
 Chooses a split point that balances cumulative size as evenly as possible
 between the left and right partitions.

 This is the core partition step for the binary treemap layout. The function
 assumes items are already sorted by size, then walks forward until the running
 total reaches roughly half of the partition's total size.
 **/
size_t ChooseSplitIndex(const std::vector<LayoutItem>& items, size_t begin, size_t end)
{
    const uint64_t total = SumSizes(items, begin, end);
    uint64_t running = 0;
    size_t split = begin + 1;
    for (size_t index = begin; index + 1 < end; ++index)
    {
        running += items[index].size;
        split = index + 1;
        if (running * 2 >= total)
        {
            break;
        }
    }
    return split;
}

Bounds InsetBounds(const Bounds& bounds, const TreemapLayoutOptions& options)
{
    Bounds inner;
    inner.x = bounds.x + options.tile_padding;
    inner.y = bounds.y + options.directory_header_height;
    inner.width = std::max(0.0, bounds.width - (options.tile_padding * 2.0));
    inner.height = std::max(0.0, bounds.height - options.directory_header_height - options.tile_padding);
    return inner;
}

void EmitNodeRect(
    const LayoutItem& item,
    size_t depth,
    const Bounds& bounds,
    std::vector<TreemapRect>* rects)
{
    if (item.is_hidden)
    {
        return;
    }

    TreemapRect rect;
    rect.node_id = item.node_id;
    rect.size = item.size;
    rect.depth = depth;
    rect.x = bounds.x;
    rect.y = bounds.y;
    rect.width = bounds.width;
    rect.height = bounds.height;
    rect.is_dir = item.is_dir;
    rect.label = item.label;
    rect.path = item.path;
    const TreemapVisualStyle style = BuildTreemapVisualStyle(item.label, item.path, item.is_dir, depth);
    rect.fill_color = style.fill_color;
    rect.label_text_color = style.label_text_color;
    rect.secondary_text_color = style.secondary_text_color;
    rect.text_outline_color = style.text_outline_color;
    rect.text_plate_color = style.text_plate_color;
    rects->push_back(std::move(rect));
}

/**
 Recursively assigns bounds to a list of items using a binary split treemap.

 This helper is geometry-only: it does not decide which nodes are visible under
 the rectangle budget. It simply turns a chosen item list into rectangles by
 repeatedly splitting the current bounds along the longer axis.
 **/
void CollectItemBounds(
    const std::vector<LayoutItem>& items,
    size_t begin,
    size_t end,
    size_t depth,
    const Bounds& bounds,
    std::vector<PositionedItem>* positioned)
{
    if (begin >= end)
    {
        return;
    }

    if (bounds.width <= 0.0 || bounds.height <= 0.0)
    {
        return;
    }

    if (begin + 1 == end)
    {
        PositionedItem entry;
        entry.item = &items[begin];
        entry.bounds = bounds;
        entry.depth = depth;
        positioned->push_back(entry);
        return;
    }

    const uint64_t total = SumSizes(items, begin, end);
    if (total == 0)
    {
        return;
    }

    const size_t split = ChooseSplitIndex(items, begin, end);
    const uint64_t first_total = SumSizes(items, begin, split);
    const double ratio = static_cast<double>(first_total) / static_cast<double>(total);

    Bounds first = bounds;
    Bounds second = bounds;
    if (bounds.width >= bounds.height)
    {
        const double first_width = bounds.width * ratio;
        first.width = first_width;
        second.x = bounds.x + first_width;
        second.width = std::max(0.0, bounds.width - first_width);
    }
    else
    {
        const double first_height = bounds.height * ratio;
        first.height = first_height;
        second.y = bounds.y + first_height;
        second.height = std::max(0.0, bounds.height - first_height);
    }

    CollectItemBounds(items, begin, split, depth, first, positioned);
    CollectItemBounds(items, split, end, depth, second, positioned);
}

const ChildCollection& GetOrCreateChildren(
    const FileTree& tree,
    uint64_t parent_id,
    std::unordered_map<uint64_t, ChildCollection>* cache)
{
    auto it = cache->find(parent_id);
    if (it != cache->end())
    {
        return it->second;
    }

    ChildCollection children;
    const Node* parent = tree.GetNode(parent_id);
    if (parent)
    {
        children = CollectChildren(tree, *parent);
    }

    auto inserted = cache->emplace(parent_id, std::move(children)).first;
    return inserted->second;
}

/**
 Pushes the next eligible child for a directory into the global candidate heap.

 Each directory contributes at most one pending candidate at a time: its
 largest remaining child that passes the minimum ratio filter and any optional
 per-directory child cap. When that candidate is selected, the caller advances
 the directory state and asks this helper for the next one.
 **/
void PushNextCandidate(
    const FileTree& tree,
    uint64_t parent_id,
    const TreemapLayoutOptions& options,
    std::unordered_map<uint64_t, SelectionState>* states,
    std::unordered_map<uint64_t, ChildCollection>* children_cache,
    std::priority_queue<ChildCandidate, std::vector<ChildCandidate>, ChildCandidateLess>* candidates)
{
    auto state_it = states->find(parent_id);
    if (state_it == states->end())
    {
        return;
    }

    SelectionState& state = state_it->second;
    if (options.max_children_per_directory > 0 &&
        state.visible_child_count >= options.max_children_per_directory)
    {
        return;
    }

    const ChildCollection& children = GetOrCreateChildren(tree, parent_id, children_cache);
    const double parent_size = static_cast<double>(std::max<uint64_t>(state.parent_size, 1));

    while (state.next_child_index < children.items.size())
    {
        const LayoutItem& child = children.items[state.next_child_index];
        const double child_ratio = static_cast<double>(child.size) / parent_size;
        if (child_ratio < options.minimum_child_ratio)
        {
            ++state.next_child_index;
            continue;
        }

        ChildCandidate candidate;
        candidate.parent_id = parent_id;
        candidate.size = child.size;
        candidate.child_index = state.next_child_index;
        candidates->push(candidate);
        return;
    }
}

/**
 Emits rectangles for the visible portion of a subtree after the budget
 selection pass has decided how many children each directory may show.

 The method emits the current node first, then lays out only the selected
 leading children for that directory. Any unselected remainder is represented
 by a hidden placeholder that occupies space during layout but is never drawn,
 causing omitted descendants to stay absorbed into the nearest real directory.
 **/
void EmitVisibleSubtree(
    const FileTree& tree,
    uint64_t root_id,
    uint64_t node_id,
    size_t depth,
    const Bounds& bounds,
    const TreemapLayoutOptions& options,
    const std::unordered_map<uint64_t, size_t>& visible_child_counts,
    const std::unordered_map<uint64_t, ChildCollection>& children_cache,
    std::vector<TreemapRect>* rects)
{
    const Node* node = tree.GetNode(node_id);
    if (!node)
    {
        return;
    }

    LayoutItem item;
    item.node_id = node->id;
    item.size = node->size;
    item.is_dir = node->is_dir;
    item.label = node->name.empty() ? (node->id == root_id ? L"(root)" : L"(unnamed)") : node->name;
    item.path = tree.BuildPath(node->id);
    EmitNodeRect(item, depth, bounds, rects);

    if (!node->is_dir)
    {
        return;
    }

    if (depth >= options.max_depth)
    {
        return;
    }

    if (bounds.width < options.minimum_child_side ||
        bounds.height < options.minimum_child_side + options.directory_header_height)
    {
        return;
    }

    auto visible_it = visible_child_counts.find(node_id);
    if (visible_it == visible_child_counts.end() || visible_it->second == 0)
    {
        return;
    }

    auto children_it = children_cache.find(node_id);
    if (children_it == children_cache.end())
    {
        return;
    }

    const ChildCollection& children = children_it->second;
    const size_t visible_count = std::min(visible_it->second, children.items.size());
    if (visible_count == 0)
    {
        return;
    }

    std::vector<LayoutItem> layout_items;
    layout_items.reserve(visible_count + 1);

    uint64_t visible_size = 0;
    for (size_t index = 0; index < visible_count; ++index)
    {
        visible_size += children.items[index].size;
        layout_items.push_back(children.items[index]);
    }

    if (children.total_size > visible_size)
    {
        LayoutItem hidden;
        hidden.size = children.total_size - visible_size;
        hidden.is_hidden = true;
        layout_items.push_back(std::move(hidden));
    }

    Bounds inner = InsetBounds(bounds, options);
    if (inner.width <= 0.0 || inner.height <= 0.0)
    {
        return;
    }

    const size_t child_depth = (node_id == root_id) ? 0 : depth + 1;
    std::vector<PositionedItem> positioned;
    positioned.reserve(layout_items.size());
    CollectItemBounds(layout_items, 0, layout_items.size(), child_depth, inner, &positioned);

    for (const PositionedItem& positioned_item : positioned)
    {
        if (!positioned_item.item || positioned_item.item->is_hidden)
        {
            continue;
        }

        EmitVisibleSubtree(
            tree,
            root_id,
            positioned_item.item->node_id,
            positioned_item.depth,
            positioned_item.bounds,
            options,
            visible_child_counts,
            children_cache,
            rects);
    }
}

/**
 Emits the selected top-level children of the current view root directly into
 the available treemap bounds.

 Unlike deeper directories, the current root is not drawn as an enclosing tile
 in the GUI because that level is already represented by the surrounding header
 and breadcrumb chrome. This helper therefore lays out only the root's visible
 children, while still reserving space for omitted descendants through the same
 hidden-placeholder mechanism used deeper in the tree.
 **/
void EmitVisibleChildren(
    const FileTree& tree,
    uint64_t root_id,
    size_t depth,
    const Bounds& bounds,
    const TreemapLayoutOptions& options,
    const std::unordered_map<uint64_t, size_t>& visible_child_counts,
    const std::unordered_map<uint64_t, ChildCollection>& children_cache,
    std::vector<TreemapRect>* rects)
{
    auto visible_it = visible_child_counts.find(root_id);
    if (visible_it == visible_child_counts.end() || visible_it->second == 0)
    {
        return;
    }

    auto children_it = children_cache.find(root_id);
    if (children_it == children_cache.end())
    {
        return;
    }

    const ChildCollection& children = children_it->second;
    const size_t visible_count = std::min(visible_it->second, children.items.size());
    if (visible_count == 0)
    {
        return;
    }

    std::vector<LayoutItem> layout_items;
    layout_items.reserve(visible_count + 1);

    uint64_t visible_size = 0;
    for (size_t index = 0; index < visible_count; ++index)
    {
        visible_size += children.items[index].size;
        layout_items.push_back(children.items[index]);
    }

    if (children.total_size > visible_size)
    {
        LayoutItem hidden;
        hidden.size = children.total_size - visible_size;
        hidden.is_hidden = true;
        layout_items.push_back(std::move(hidden));
    }

    std::vector<PositionedItem> positioned;
    positioned.reserve(layout_items.size());
    CollectItemBounds(layout_items, 0, layout_items.size(), depth, bounds, &positioned);

    for (const PositionedItem& positioned_item : positioned)
    {
        if (!positioned_item.item || positioned_item.item->is_hidden)
        {
            continue;
        }

        EmitVisibleSubtree(
            tree,
            root_id,
            positioned_item.item->node_id,
            positioned_item.depth,
            positioned_item.bounds,
            options,
            visible_child_counts,
            children_cache,
            rects);
    }
}

void AppendStatsCard(std::ostringstream& html, const std::string& label, const std::string& value)
{
    html << "        <div class=\"stat-card\">\n";
    html << "          <div class=\"stat-label\">" << label << "</div>\n";
    html << "          <div class=\"stat-value\">" << value << "</div>\n";
    html << "        </div>\n";
}

std::string TooltipText(const TreemapRect& rect, uint64_t root_size)
{
    std::ostringstream tooltip;
    tooltip << EscapeHtml(Utf8FromWide(rect.path.empty() ? rect.label : rect.path))
            << "&#10;Size: " << EscapeHtml(Utf8FromWide(HumanizeBytes(rect.size)))
            << " (" << FormatPercent(root_size == 0 ? 0.0 : static_cast<double>(rect.size) / static_cast<double>(root_size)) << "%)"
            << "&#10;Type: " << (rect.is_dir ? "Directory" : "File");
    return tooltip.str();
}

} // namespace

TreemapVisualStyle BuildTreemapVisualStyle(
    const std::wstring& label,
    const std::wstring& path,
    bool is_dir,
    size_t depth)
{
    LayoutItem item;
    item.label = label;
    item.path = path;
    item.is_dir = is_dir;

    TreemapVisualStyle style;
    style.fill_color = ColorForItem(item, depth);
    const TextPalette palette = TextColorsForFill(style.fill_color);
    style.label_text_color = palette.primary;
    style.secondary_text_color = palette.secondary;
    style.text_outline_color = palette.outline;
    style.text_plate_color = palette.plate;
    return style;
}

/**
 Builds a treemap layout for the requested root using a global rectangle budget.

 Strategy:
   1. Keep the underlying FileTree exact. No nodes are merged into the tree itself.
   2. Build the initial global candidate heap from the current view root's
      children rather than drawing the root itself as an enclosing tile.
   3. For each visible directory, consider only its next-largest undisplayed child
      as an expansion candidate. All candidates across the current visible tree are
      stored in a max-heap ordered by size.
   4. Repeatedly take the largest candidate from that heap until the rectangle
      budget is exhausted. Selecting a child spends one rectangle from the budget.
      If that child is a directory and depth permits, it starts contributing its
      own children as future candidates.
   5. After the visible child counts are chosen, emit the actual rectangles in a
      second pass. The current view root emits only its visible children directly
      into the treemap bounds, while deeper directories emit themselves first and
      then their selected children. Any children that were not selected remain
      absorbed into the nearest real ancestor directory's area instead of becoming
      a synthetic "Other" rectangle.

 This yields a best-first expansion of the tree: large directories/files near the
 top tend to appear first, and smaller descendants only become visible if there is
 enough remaining budget to justify expanding deeper.
 **/
std::vector<TreemapRect> BuildTreemapLayout(const FileTree& tree, uint64_t root_id, const TreemapLayoutOptions& options)
{
    std::vector<TreemapRect> rects;
    if (options.max_rectangles == 0)
    {
        return rects;
    }

    const Node* root = tree.GetNode(root_id);
    if (!root)
    {
        return rects;
    }

    Bounds root_bounds;
    root_bounds.width = options.width;
    root_bounds.height = options.height;

    std::unordered_map<uint64_t, ChildCollection> children_cache;
    children_cache.reserve(std::min<size_t>(options.max_rectangles, 512));

    std::unordered_map<uint64_t, SelectionState> selection_states;
    selection_states.reserve(std::min<size_t>(options.max_rectangles, 512));

    std::unordered_map<uint64_t, size_t> visible_child_counts;
    visible_child_counts.reserve(std::min<size_t>(options.max_rectangles, 512));

    std::priority_queue<ChildCandidate, std::vector<ChildCandidate>, ChildCandidateLess> candidates;

    if (root->is_dir && root->size > 0)
    {
        SelectionState root_state;
        root_state.parent_size = root->size;
        root_state.child_depth = 0;
        selection_states.emplace(root_id, root_state);
        PushNextCandidate(tree, root_id, options, &selection_states, &children_cache, &candidates);

        // The current view root is not emitted as a tile in the GUI treemap, so
        // the full rectangle budget is available for visible descendants.
        size_t visible_rectangles = 0;
        while (!candidates.empty() && visible_rectangles < options.max_rectangles)
        {
            const ChildCandidate candidate = candidates.top();
            candidates.pop();

            auto state_it = selection_states.find(candidate.parent_id);
            if (state_it == selection_states.end())
            {
                continue;
            }

            SelectionState& state = state_it->second;
            if (candidate.child_index != state.next_child_index)
            {
                continue;
            }

            const ChildCollection& children = GetOrCreateChildren(tree, candidate.parent_id, &children_cache);
            if (candidate.child_index >= children.items.size())
            {
                continue;
            }

            const LayoutItem& child = children.items[candidate.child_index];
            ++state.visible_child_count;
            visible_child_counts[candidate.parent_id] = state.visible_child_count;
            ++state.next_child_index;
            ++visible_rectangles;

            // Advance this directory to its next-largest child so it can keep
            // competing globally for more visible rectangles.
            PushNextCandidate(tree, candidate.parent_id, options, &selection_states, &children_cache, &candidates);

            if (!child.is_dir || state.child_depth >= options.max_depth)
            {
                continue;
            }

            // Newly selected directories join the competition by exposing their
            // own largest child as the next possible expansion.
            SelectionState child_state;
            child_state.parent_size = child.size;
            child_state.child_depth = state.child_depth + 1;
            selection_states.emplace(child.node_id, child_state);
            PushNextCandidate(tree, child.node_id, options, &selection_states, &children_cache, &candidates);
        }
    }

    if (!root->is_dir || root->size == 0)
    {
        LayoutItem item;
        item.node_id = root->id;
        item.size = root->size;
        item.is_dir = root->is_dir;
        item.label = root->name.empty() ? L"(root)" : root->name;
        item.path = tree.BuildPath(root->id);
        EmitNodeRect(item, 0, root_bounds, &rects);
        return rects;
    }

    EmitVisibleChildren(
        tree,
        root_id,
        0,
        root_bounds,
        options,
        visible_child_counts,
        children_cache,
        &rects);
    return rects;
}

std::vector<TreemapRect> BuildTreemapLayout(const FileTree& tree, const TreemapLayoutOptions& options)
{
    return BuildTreemapLayout(tree, tree.RootId(), options);
}

bool WriteTreemapHtml(
    const std::wstring& output_path,
    const FileTree& tree,
    const TreemapDocumentInfo& info,
    const TreemapLayoutOptions& options,
    std::wstring* error)
{
    const Node* root = tree.GetNode(tree.RootId());
    if (!root)
    {
        if (error)
        {
            *error = L"Treemap output requires a valid root node.";
        }
        return false;
    }

    const std::vector<TreemapRect> rects = BuildTreemapLayout(tree, options);
    std::ostringstream html;
    html.setf(std::ios::fixed);
    html << std::setprecision(2);

    const std::string scan_path = EscapeHtml(Utf8FromWide(info.scan_path.empty() ? tree.BuildPath(root->id) : info.scan_path));
    const std::string backend_name = EscapeHtml(Utf8FromWide(info.backend_name));
    const std::string root_size = EscapeHtml(Utf8FromWide(HumanizeBytes(root->size)));

    html << "<!DOCTYPE html>\n";
    html << "<html lang=\"en\">\n";
    html << "<head>\n";
    html << "  <meta charset=\"utf-8\">\n";
    html << "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    html << "  <title>SpaceScanner Treemap</title>\n";
    html << "  <style>\n";
    html << "    :root {\n";
    html << "      --bg-a: #081120;\n";
    html << "      --bg-b: #12263a;\n";
    html << "      --panel: rgba(8, 15, 28, 0.78);\n";
    html << "      --panel-border: rgba(196, 214, 255, 0.16);\n";
    html << "      --text: #edf4ff;\n";
    html << "      --muted: #9fb2d1;\n";
    html << "      --accent: #8fd7ff;\n";
    html << "    }\n";
    html << "    * { box-sizing: border-box; }\n";
    html << "    body {\n";
    html << "      margin: 0;\n";
    html << "      min-height: 100vh;\n";
    html << "      font-family: \"Segoe UI\", \"Trebuchet MS\", sans-serif;\n";
    html << "      color: var(--text);\n";
    html << "      background:\n";
    html << "        radial-gradient(circle at top left, rgba(82, 175, 255, 0.22), transparent 36%),\n";
    html << "        radial-gradient(circle at bottom right, rgba(97, 235, 194, 0.16), transparent 30%),\n";
    html << "        linear-gradient(140deg, var(--bg-a), var(--bg-b));\n";
    html << "    }\n";
    html << "    main {\n";
    html << "      max-width: 1800px;\n";
    html << "      margin: 0 auto;\n";
    html << "      padding: 24px;\n";
    html << "    }\n";
    html << "    .hero {\n";
    html << "      display: grid;\n";
    html << "      gap: 18px;\n";
    html << "      margin-bottom: 18px;\n";
    html << "    }\n";
    html << "    .hero-panel,\n";
    html << "    .canvas-panel {\n";
    html << "      background: var(--panel);\n";
    html << "      border: 1px solid var(--panel-border);\n";
    html << "      border-radius: 18px;\n";
    html << "      backdrop-filter: blur(12px);\n";
    html << "      box-shadow: 0 20px 70px rgba(0, 0, 0, 0.35);\n";
    html << "    }\n";
    html << "    .hero-panel {\n";
    html << "      padding: 24px;\n";
    html << "    }\n";
    html << "    h1 {\n";
    html << "      margin: 0 0 10px;\n";
    html << "      font-size: clamp(26px, 3vw, 42px);\n";
    html << "      letter-spacing: 0.02em;\n";
    html << "    }\n";
    html << "    .subhead {\n";
    html << "      margin: 0;\n";
    html << "      color: var(--muted);\n";
    html << "      font-size: 15px;\n";
    html << "      line-height: 1.5;\n";
    html << "      word-break: break-all;\n";
    html << "    }\n";
    html << "    .stats {\n";
    html << "      display: grid;\n";
    html << "      grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));\n";
    html << "      gap: 12px;\n";
    html << "      margin-top: 18px;\n";
    html << "    }\n";
    html << "    .stat-card {\n";
    html << "      padding: 14px 16px;\n";
    html << "      border-radius: 14px;\n";
    html << "      background: rgba(255, 255, 255, 0.04);\n";
    html << "      border: 1px solid rgba(255, 255, 255, 0.06);\n";
    html << "    }\n";
    html << "    .stat-label {\n";
    html << "      color: var(--muted);\n";
    html << "      font-size: 12px;\n";
    html << "      text-transform: uppercase;\n";
    html << "      letter-spacing: 0.08em;\n";
    html << "    }\n";
    html << "    .stat-value {\n";
    html << "      margin-top: 6px;\n";
    html << "      font-size: 20px;\n";
    html << "      font-weight: 600;\n";
    html << "    }\n";
    html << "    .hint {\n";
    html << "      margin-top: 14px;\n";
    html << "      color: var(--muted);\n";
    html << "      font-size: 13px;\n";
    html << "    }\n";
    html << "    .canvas-panel {\n";
    html << "      padding: 12px;\n";
    html << "    }\n";
    html << "    svg {\n";
    html << "      display: block;\n";
    html << "      width: 100%;\n";
    html << "      height: auto;\n";
    html << "      border-radius: 12px;\n";
    html << "      background: linear-gradient(180deg, rgba(5, 10, 18, 0.92), rgba(10, 18, 32, 0.86));\n";
    html << "    }\n";
    html << "    .tile-rect {\n";
    html << "      stroke: rgba(4, 10, 20, 0.65);\n";
    html << "      stroke-width: 1.1;\n";
    html << "      transition: filter 120ms ease, stroke 120ms ease;\n";
    html << "    }\n";
    html << "    .tile:hover .tile-rect {\n";
    html << "      filter: brightness(1.08);\n";
    html << "      stroke: rgba(255, 255, 255, 0.85);\n";
    html << "    }\n";
    html << "    .tile-label {\n";
    html << "      font-size: 12px;\n";
    html << "      font-weight: 600;\n";
    html << "      pointer-events: none;\n";
    html << "    }\n";
    html << "    .tile-size {\n";
    html << "      font-size: 11px;\n";
    html << "      pointer-events: none;\n";
    html << "    }\n";
    html << "    @media (max-width: 720px) {\n";
    html << "      main { padding: 16px; }\n";
    html << "      .hero-panel { padding: 18px; }\n";
    html << "    }\n";
    html << "  </style>\n";
    html << "</head>\n";
    html << "<body>\n";
    html << "  <main>\n";
    html << "    <section class=\"hero\">\n";
    html << "      <div class=\"hero-panel\">\n";
    html << "        <h1>SpaceScanner Treemap</h1>\n";
    html << "        <p class=\"subhead\">" << scan_path << "</p>\n";
    html << "        <div class=\"stats\">\n";
    AppendStatsCard(html, "Backend", backend_name.empty() ? std::string("directory") : backend_name);
    AppendStatsCard(html, "Root Size", root_size);
    AppendStatsCard(html, "Rendered Tiles", std::to_string(rects.size()));
    AppendStatsCard(html, "Nodes in Tree", std::to_string(tree.NodeCount()));
    AppendStatsCard(html, "Elapsed", FormatDouble(info.elapsed_seconds) + "s");
    AppendStatsCard(html, "Depth Limit", std::to_string(options.max_depth));
    html << "        </div>\n";
    html << "        <p class=\"hint\">Hover any rectangle for exact path and size. Rendering is capped to keep large scans responsive without drawing synthetic summary boxes.</p>\n";
    html << "      </div>\n";
    html << "      <div class=\"canvas-panel\">\n";
    html << "        <svg viewBox=\"0 0 " << FormatDouble(options.width) << ' ' << FormatDouble(options.height)
         << "\" role=\"img\" aria-label=\"Treemap of scanned disk usage\">\n";

    for (size_t index = 0; index < rects.size(); ++index)
    {
        const TreemapRect& rect = rects[index];
        const std::string label = EscapeHtml(Utf8FromWide(rect.label));
        const std::string size_label = EscapeHtml(Utf8FromWide(HumanizeBytes(rect.size)));
        const std::string title = TooltipText(rect, root->size);
        const std::string path_class = rect.is_dir ? " dir" : " file";

        html << "          <g class=\"tile" << path_class << "\">\n";
        html << "            <title>" << title << "</title>\n";
        html << "            <clipPath id=\"clip-" << index << "\">\n";
        html << "              <rect x=\"" << FormatDouble(rect.x) << "\" y=\"" << FormatDouble(rect.y)
             << "\" width=\"" << FormatDouble(rect.width) << "\" height=\"" << FormatDouble(rect.height) << "\" />\n";
        html << "            </clipPath>\n";
        html << "            <rect class=\"tile-rect\" x=\"" << FormatDouble(rect.x) << "\" y=\"" << FormatDouble(rect.y)
             << "\" width=\"" << FormatDouble(rect.width) << "\" height=\"" << FormatDouble(rect.height)
             << "\" rx=\"2\" ry=\"2\" fill=\"" << rect.fill_color << "\" />\n";

        if (rect.width >= 56.0 && rect.height >= 18.0)
        {
            const double text_x = rect.x + 6.0;
            const bool show_label = rect.width >= options.minimum_label_width && rect.height >= options.minimum_label_height;
            const double plate_height = show_label ? 30.0 : 16.0;
            const double label_y = rect.y + 14.0;
            const double size_y = show_label ? rect.y + 28.0 : rect.y + 14.0;
            html << "            <g clip-path=\"url(#clip-" << index << ")\">\n";
            html << "              <rect x=\"" << FormatDouble(rect.x + 2.0) << "\" y=\"" << FormatDouble(rect.y + 2.0)
                 << "\" width=\"" << FormatDouble(std::max(0.0, rect.width - 4.0)) << "\" height=\"" << FormatDouble(plate_height)
                 << "\" rx=\"2\" ry=\"2\" fill=\"" << rect.text_plate_color << "\" opacity=\"0.34\" />\n";
            if (show_label)
            {
                html << "              <text class=\"tile-label\" fill=\"" << rect.label_text_color
                     << "\" stroke=\"" << rect.text_outline_color << "\" stroke-width=\"2.2\" paint-order=\"stroke\" x=\"" << FormatDouble(text_x)
                     << "\" y=\"" << FormatDouble(label_y) << "\">" << label << "</text>\n";
            }
            html << "              <text class=\"tile-size\" fill=\"" << rect.secondary_text_color
                 << "\" stroke=\"" << rect.text_outline_color << "\" stroke-width=\"1.8\" paint-order=\"stroke\" x=\"" << FormatDouble(text_x)
                 << "\" y=\"" << FormatDouble(size_y) << "\">" << size_label << "</text>\n";
            html << "            </g>\n";
        }

        html << "          </g>\n";
    }

    html << "        </svg>\n";
    html << "      </div>\n";
    html << "    </section>\n";
    html << "  </main>\n";
    html << "</body>\n";
    html << "</html>\n";

    std::ofstream output(std::filesystem::path(output_path), std::ios::binary);
    if (!output)
    {
        if (error)
        {
            *error = L"Failed to open treemap output file.";
        }
        return false;
    }

    const std::string text = html.str();
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output.good())
    {
        if (error)
        {
            *error = L"Failed while writing treemap output.";
        }
        return false;
    }

    return true;
}

} // namespace space_scanner::visualization
