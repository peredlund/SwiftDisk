#pragma once

#include "gui/drawing_state.h"

#include "common/file_tree.h"
#include "visualization/treemap.h"

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace swift_disk::gui
{

    inline constexpr float kOuterPadding = 18.0f;
    inline constexpr float kHeaderHeight = 116.0f;
    inline constexpr float kTreemapTopSliceHeight = 22.0f;
    inline constexpr float kTreemapSideSliceWidth = 3.0f;
    inline constexpr float kTreemapBottomSliceHeight = 3.0f;
    inline constexpr float kStatusBarHeight = 26.0f;
    inline constexpr float kStatusBarGap = 7.0f;
    inline constexpr float kStatusBarBottomPadding = 4.0f;
    inline constexpr float kStatusBarTextVerticalPadding = 4.0f;
    inline constexpr float kTooltipOffsetX = 16.0f;
    inline constexpr float kTooltipOffsetY = 22.0f;
    inline constexpr float kTooltipPaddingX = 10.0f;
    inline constexpr float kTooltipPaddingY = 8.0f;
    inline constexpr float kTooltipLineGap = 4.0f;
    inline constexpr float kTooltipCornerRadius = 4.0f;
    inline constexpr float kTooltipInnerMargin = 6.0f;

    struct BreadcrumbStrip
    {
        std::uint64_t node_id = 0;
        RenderRect rect = MakeRenderRect(0, 0, 0, 0);
        RenderRect frame_rect = MakeRenderRect(0, 0, 0, 0);
        RenderRect content_rect = MakeRenderRect(0, 0, 0, 0);
        std::wstring text;
        visualization::TreemapVisualStyle style;
    };

    struct MainWindowDrawingState
    {
        RenderVisualPalette palette;
        bool scan_in_progress = false;
        bool has_error = false;
        bool requested_path_is_ntfs = false;
        bool used_mft_scan = false;
        bool show_tooltip = true;
        bool show_status_bar = false;
        int hovered_index = -1;
        int hovered_breadcrumb_index = -1;
        double elapsed_seconds = 0.0;
        std::uint64_t view_root_id = 0;
        std::wstring selected_path;
        std::wstring status_text;
        std::wstring error_text;
        const FileTree* tree = nullptr;
        visualization::TreemapLayoutOptions layout_options;
        const std::vector<visualization::TreemapRect>* rects = nullptr;
        const std::vector<BreadcrumbStrip>* breadcrumbs = nullptr;
        RenderPoint tooltip_position = MakeRenderPoint(-1.0f, -1.0f);
        RenderRect canvas_rect = MakeRenderRect(0, 0, 0, 0);
        RenderRect status_bar_rect = MakeRenderRect(0, 0, 0, 0);
        RenderRect treemap_content_rect = MakeRenderRect(0, 0, 0, 0);
        RenderRect focus_up_rect = MakeRenderRect(0, 0, 0, 0);
        RenderRect header_info_rect = MakeRenderRect(0, 0, 0, 0);
        RenderRect scan_group_rect = MakeRenderRect(0, 0, 0, 0);
        RenderRect treemap_group_rect = MakeRenderRect(0, 0, 0, 0);
        RenderRect display_group_rect = MakeRenderRect(0, 0, 0, 0);
        RenderRect paint_rect = MakeRenderRect(0, 0, 0, 0);
    };

    class Direct2DRenderer;

    /**
    Draws the SwiftDisk main window using Direct2D.
    **/
    class MainWindowPainter
    {
    public:
        MainWindowPainter();
        ~MainWindowPainter();

        MainWindowPainter(const MainWindowPainter&) = delete;
        MainWindowPainter& operator=(const MainWindowPainter&) = delete;

        HRESULT CreateDeviceResources(HWND hwnd, const RenderVisualPalette& palette);
        void DiscardDeviceResources();
        bool HasRenderTarget() const;
        HRESULT Resize(HWND hwnd);
        void Paint(HWND hwnd, const MainWindowDrawingState& state);
        RenderRect GetHoverTooltipBounds(HWND hwnd, const MainWindowDrawingState& state);

    private:
        std::unique_ptr<Direct2DRenderer> m_renderer;
    };

} // namespace swift_disk::gui
