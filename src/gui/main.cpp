#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gui/direct2d_renderer.h"

#include "common/file_tree.h"
#include "common/stopwatch.h"
#include "core/dir_scanner.h"
#include "core/ntfs/mft_scanner.h"
#include "visualization/treemap.h"

namespace space_scanner::gui
{

    using Microsoft::WRL::ComPtr;

    constexpr UINT WM_APP_SCAN_COMPLETE = WM_APP + 1;
    constexpr UINT WM_APP_SCAN_FAILED = WM_APP + 2;
    constexpr int IDC_PATH_EDIT = 1001;
    constexpr int IDC_SCAN_BUTTON = 1002;
    constexpr int IDC_BROWSE_BUTTON = 1003;
    constexpr int IDC_DEPTH_LABEL = 1004;
    constexpr int IDC_DEPTH_EDIT = 1005;
    constexpr int IDC_SHOW_TOOLTIP_CHECK = 1006;
    constexpr int IDC_SHOW_STATUS_BAR_CHECK = 1007;
    constexpr int IDC_DEPTH_UP_BUTTON = 1008;
    constexpr int IDC_DEPTH_DOWN_BUTTON = 1009;
    constexpr int IDC_RESET_BUTTON = 1010;

    struct GuiOptions
    {
        std::wstring path;
        bool show_help = false;
    };

    struct ScanPayload
    {
        std::unique_ptr<FileTree> tree;
        bool used_mft_scan = false;
        double elapsed_seconds = 0.0;
        std::wstring path;
        std::wstring error;
    };

    struct SharedScanState
    {
        std::mutex mutex;
        std::unique_ptr<ScanPayload> pending_payload;
    };

    bool VolumeExists(wchar_t drive_letter)
    {
        wchar_t root_path[] = { drive_letter, L':', L'\\', L'\0' };
        return GetDriveTypeW(root_path) != DRIVE_NO_ROOT_DIR;
    }

    std::wstring NormalizeScanPath(const std::wstring& path)
    {
        if (path.size() == 2 &&
            ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
            path[1] == L':')
        {
            return path + L'\\';
        }

        return path;
    }

    std::wstring GetDefaultPathFieldValue()
    {
        if (VolumeExists(L'C'))
        {
            return L"C:";
        }

        return {};
    }

    bool TryGetVolumeRoot(const std::wstring& path, std::wstring* volume_root)
    {
        if (!volume_root || path.empty())
        {
            return false;
        }

        wchar_t buffer[MAX_PATH] = {};
        DWORD length = GetVolumePathNameW(path.c_str(), buffer, static_cast<DWORD>(std::size(buffer)));
        if (length == 0 || length >= std::size(buffer))
        {
            return false;
        }

        *volume_root = buffer;
        return true;
    }

    bool IsNtfsPath(const std::wstring& path)
    {
        std::wstring volume_root;
        if (!TryGetVolumeRoot(path, &volume_root))
        {
            return false;
        }

        wchar_t file_system_name[MAX_PATH] = {};
        if (!GetVolumeInformationW(
            volume_root.c_str(),
            nullptr,
            0,
            nullptr,
            nullptr,
            nullptr,
            file_system_name,
            static_cast<DWORD>(std::size(file_system_name))))
        {
            return false;
        }

        return _wcsicmp(file_system_name, L"NTFS") == 0;
    }

    GuiOptions ParseOptions()
    {
        GuiOptions options;

        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
        {
            return options;
        }

        for (int i = 1; i < argc; ++i)
        {
            if (wcscmp(argv[i], L"--path") == 0 && i + 1 < argc)
            {
                options.path = argv[++i];
            }
            else if (wcscmp(argv[i], L"--help") == 0 || wcscmp(argv[i], L"-h") == 0)
            {
                options.show_help = true;
            }
            else if (argv[i][0] != L'-')
            {
                options.path = argv[i];
            }
        }

        LocalFree(argv);
        options.path = NormalizeScanPath(options.path);
        return options;
    }

    std::wstring HumanizeBytes(uint64_t bytes)
    {
        static constexpr const wchar_t* kUnits[] = { L"B", L"KiB", L"MiB", L"GiB", L"TiB", L"PiB" };
        double value = static_cast<double>(bytes);
        size_t unit_index = 0;
        while (value >= 1024.0 && unit_index + 1 < std::size(kUnits))
        {
            value /= 1024.0;
            ++unit_index;
        }

        wchar_t buffer[64] = {};
        swprintf_s(buffer, L"%.2f %s", value, kUnits[unit_index]);
        return buffer;
    }

    COLORREF ColorRefFromRgb(BYTE red, BYTE green, BYTE blue)
    {
        return RGB(red, green, blue);
    }

    bool PointInRect(const RenderRect& rect, float x, float y)
    {
        return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
    }

    RECT RectFToRect(const RenderRect& rect)
    {
        RECT result = {};
        result.left = static_cast<LONG>(std::floor(rect.left));
        result.top = static_cast<LONG>(std::floor(rect.top));
        result.right = static_cast<LONG>(std::ceil(rect.right));
        result.bottom = static_cast<LONG>(std::ceil(rect.bottom));
        return result;
    }

    RenderRect RectToRenderRect(const RECT& rect)
    {
        return MakeRenderRect(
            static_cast<float>(rect.left),
            static_cast<float>(rect.top),
            static_cast<float>(rect.right),
            static_cast<float>(rect.bottom));
    }

    bool IsRenderRectEmpty(const RenderRect& rect)
    {
        return rect.left >= rect.right || rect.top >= rect.bottom;
    }

    RenderRect UnionRenderRect(const RenderRect& left, const RenderRect& right)
    {
        if (IsRenderRectEmpty(left))
        {
            return right;
        }
        if (IsRenderRectEmpty(right))
        {
            return left;
        }

        return MakeRenderRect(
            std::min(left.left, right.left),
            std::min(left.top, right.top),
            std::max(left.right, right.right),
            std::max(left.bottom, right.bottom));
    }

    RenderRect InflateRenderRect(const RenderRect& rect, float amount)
    {
        if (IsRenderRectEmpty(rect))
        {
            return rect;
        }

        return MakeRenderRect(
            rect.left - amount,
            rect.top - amount,
            rect.right + amount,
            rect.bottom + amount);
    }

    float GetWindowDpiScale(HWND hwnd)
    {
        if (!hwnd)
        {
            return 1.0f;
        }

        HDC hdc = GetDC(hwnd);
        if (!hdc)
        {
            return 1.0f;
        }

        const int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(hwnd, hdc);
        return std::clamp(static_cast<float>(dpi) / 96.0f, 1.0f, 4.0f);
    }

    float ScaleForDpi(float value, float dpi_scale)
    {
        return value * dpi_scale;
    }

    class MainWindow
    {
    public:
        explicit MainWindow(GuiOptions options)
            : m_options(std::move(options)),
            m_shared_scan_state(std::make_shared<SharedScanState>()),
            m_requested_path_is_ntfs(IsNtfsPath(m_options.path))
        {
        }

        bool Create(HINSTANCE instance, int show_command)
        {
            WNDCLASSEXW window_class = {};
            window_class.cbSize = sizeof(window_class);
            window_class.style = CS_HREDRAW | CS_VREDRAW;
            window_class.lpfnWndProc = &MainWindow::WindowProc;
            window_class.hInstance = instance;
            window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            window_class.hbrBackground = nullptr;
            window_class.lpszClassName = L"SpaceScannerMainWindow";
            if (!RegisterClassExW(&window_class))
            {
                return false;
            }

            m_hwnd = CreateWindowExW(
                0,
                window_class.lpszClassName,
                L"SpaceScanner",
                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                1440,
                960,
                nullptr,
                nullptr,
                instance,
                this);
            if (!m_hwnd)
            {
                return false;
            }

            ShowWindow(m_hwnd, show_command);
            UpdateWindow(m_hwnd);
            return true;
        }

    private:
        static constexpr float kOuterPadding = 18.0f;
        static constexpr float kHeaderHeight = 108.0f;
        static constexpr float kTreemapTopSliceHeight = 22.0f;
        static constexpr float kTreemapSideSliceWidth = 3.0f;
        static constexpr float kTreemapBottomSliceHeight = 3.0f;
        static constexpr float kStatusBarHeight = 26.0f;
        static constexpr float kStatusBarGap = 7.0f;
        static constexpr float kStatusBarBottomPadding = 4.0f;
        static constexpr float kStatusBarTextVerticalPadding = 4.0f;
        static constexpr float kTooltipOffsetX = 16.0f;
        static constexpr float kTooltipOffsetY = 22.0f;
        static constexpr float kTooltipPaddingX = 10.0f;
        static constexpr float kTooltipPaddingY = 8.0f;
        static constexpr float kTooltipLineGap = 4.0f;
        static constexpr float kTooltipCornerRadius = 4.0f;
        static constexpr float kTooltipInnerMargin = 6.0f;
        static constexpr UINT_PTR kTooltipAnimationTimerId = 1;
        static constexpr UINT kTooltipAnimationIntervalMs = 16;
        static constexpr int kDefaultMaxDepth = 3;
        static constexpr float kTooltipAnimationSmoothing = 0.38f;
        static constexpr float kTooltipAnimationSnapDistance = 1.5f;
        static constexpr size_t kMaxBreadcrumbStrips = 5;
        static constexpr const wchar_t* kControlWindowPropName = L"SpaceScannerMainWindow";
        static constexpr BYTE kWindowBackgroundRed = 23;
        static constexpr BYTE kWindowBackgroundGreen = 27;
        static constexpr BYTE kWindowBackgroundBlue = 32;
        static constexpr BYTE kHeaderPanelRed = 35;
        static constexpr BYTE kHeaderPanelGreen = 40;
        static constexpr BYTE kHeaderPanelBlue = 47;
        static constexpr BYTE kControlPanelRed = 27;
        static constexpr BYTE kControlPanelGreen = 32;
        static constexpr BYTE kControlPanelBlue = 38;
        static constexpr BYTE kControlBackplateRed = 43;
        static constexpr BYTE kControlBackplateGreen = 49;
        static constexpr BYTE kControlBackplateBlue = 58;
        static constexpr BYTE kControlSurfaceRed = 52;
        static constexpr BYTE kControlSurfaceGreen = 60;
        static constexpr BYTE kControlSurfaceBlue = 70;
        static constexpr BYTE kControlTextRed = 240;
        static constexpr BYTE kControlTextGreen = 243;
        static constexpr BYTE kControlTextBlue = 247;
        static constexpr BYTE kControlMutedRed = 169;
        static constexpr BYTE kControlMutedGreen = 176;
        static constexpr BYTE kControlMutedBlue = 185;
        static constexpr BYTE kControlBorderRed = 84;
        static constexpr BYTE kControlBorderGreen = 92;
        static constexpr BYTE kControlBorderBlue = 103;
        static constexpr BYTE kControlActiveRed = 104;
        static constexpr BYTE kControlActiveGreen = 132;
        static constexpr BYTE kControlActiveBlue = 176;
        static constexpr BYTE kControlHoverRed = 66;
        static constexpr BYTE kControlHoverGreen = 76;
        static constexpr BYTE kControlHoverBlue = 90;

        LRESULT HandleMessage(UINT message, WPARAM w_param, LPARAM l_param)
        {
            switch (message)
            {
            case WM_CREATE:
                CreateControlThemeResources();
                CreateHeaderControls();
                if (!m_options.path.empty())
                {
                    StartScan();
                }
                else
                {
                    m_status_text = L"Select a folder path and click Scan.";
                }
                return 0;
            case WM_SIZE:
                OnSize();
                return 0;
            case WM_PAINT:
                OnPaint();
                return 0;
            case WM_MOUSEMOVE:
                OnMouseMove(GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param));
                return 0;
            case WM_MOUSELEAVE:
                if (m_hovered_index != -1 || m_hovered_breadcrumb_index != -1)
                {
                    const int old_hovered_index = m_hovered_index;
                    const int old_hovered_breadcrumb_index = m_hovered_breadcrumb_index;
                    const RenderRect old_tooltip_bounds = m_tooltip_redraw_bounds;
                    ClearHoverState();
                    InvalidateHoverTransition(
                        old_hovered_index,
                        old_hovered_breadcrumb_index,
                        old_tooltip_bounds);
                }
                return 0;
            case WM_LBUTTONDOWN:
                OnLeftButtonDown(GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param));
                return 0;
            case WM_RBUTTONUP:
                NavigateUp();
                return 0;
            case WM_KEYDOWN:
                OnKeyDown(static_cast<UINT>(w_param));
                return 0;
            case WM_COMMAND:
                OnCommand(LOWORD(w_param), HIWORD(w_param), reinterpret_cast<HWND>(l_param));
                return 0;
            case WM_DRAWITEM:
                return HandleDrawItem(reinterpret_cast<const DRAWITEMSTRUCT*>(l_param));
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORBTN:
                return HandleControlColor(
                    reinterpret_cast<HDC>(w_param),
                    reinterpret_cast<HWND>(l_param),
                    message);
            case WM_APP_SCAN_COMPLETE:
                ApplyCompletedScan(false);
                return 0;
            case WM_APP_SCAN_FAILED:
                ApplyCompletedScan(true);
                return 0;
            case WM_DISPLAYCHANGE:
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return 0;
            case WM_TIMER:
                if (w_param == kTooltipAnimationTimerId)
                {
                    OnTooltipAnimationTimer();
                    return 0;
                }
                break;
            case WM_DESTROY:
                StopTooltipAnimation();
                DestroyControlHoverTracking();
                DestroyControlThemeResources();
                DiscardDeviceResources();
                PostQuitMessage(0);
                return 0;
            }

            return DefWindowProcW(m_hwnd, message, w_param, l_param);
        }

        static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param)
        {
            MainWindow* window = nullptr;
            if (message == WM_NCCREATE)
            {
                auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
                window = static_cast<MainWindow*>(create->lpCreateParams);
                window->m_hwnd = hwnd;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
            }
            else
            {
                window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            }

            if (!window)
            {
                return DefWindowProcW(hwnd, message, w_param, l_param);
            }

            return window->HandleMessage(message, w_param, l_param);
        }

        void CreateControlThemeResources()
        {
            if (!m_header_background_brush)
            {
                m_header_background_brush = CreateSolidBrush(
                    ColorRefFromRgb(
                        kHeaderPanelRed,
                        kHeaderPanelGreen,
                        kHeaderPanelBlue));
            }
            if (!m_control_panel_brush)
            {
                m_control_panel_brush = CreateSolidBrush(
                    ColorRefFromRgb(
                        kControlBackplateRed,
                        kControlBackplateGreen,
                        kControlBackplateBlue));
            }
            if (!m_control_surface_brush)
            {
                m_control_surface_brush = CreateSolidBrush(
                    ColorRefFromRgb(
                        kControlSurfaceRed,
                        kControlSurfaceGreen,
                        kControlSurfaceBlue));
            }
            if (!m_control_hover_brush)
            {
                m_control_hover_brush = CreateSolidBrush(
                    ColorRefFromRgb(
                        kControlHoverRed,
                        kControlHoverGreen,
                        kControlHoverBlue));
            }
        }

        void DestroyControlThemeResources()
        {
            if (m_header_background_brush)
            {
                DeleteObject(m_header_background_brush);
                m_header_background_brush = nullptr;
            }
            if (m_control_panel_brush)
            {
                DeleteObject(m_control_panel_brush);
                m_control_panel_brush = nullptr;
            }
            if (m_control_surface_brush)
            {
                DeleteObject(m_control_surface_brush);
                m_control_surface_brush = nullptr;
            }
            if (m_control_hover_brush)
            {
                DeleteObject(m_control_hover_brush);
                m_control_hover_brush = nullptr;
            }
        }

        LRESULT HandleControlColor(HDC hdc, HWND control, UINT message)
        {
            if (!hdc || !control)
            {
                return 0;
            }

            SetTextColor(hdc, ColorRefFromRgb(kControlTextRed, kControlTextGreen, kControlTextBlue));

            if ((message == WM_CTLCOLOREDIT || message == WM_CTLCOLORSTATIC) &&
                (control == m_path_edit || control == m_depth_edit))
            {
                const bool hovered = IsControlHovered(control) && IsWindowEnabled(control);
                SetBkColor(
                    hdc,
                    hovered
                        ? ColorRefFromRgb(kControlHoverRed, kControlHoverGreen, kControlHoverBlue)
                        : ColorRefFromRgb(kControlSurfaceRed, kControlSurfaceGreen, kControlSurfaceBlue));
                SetBkMode(hdc, OPAQUE);
                return reinterpret_cast<LRESULT>(hovered ? m_control_hover_brush : m_control_surface_brush);
            }

            if (message == WM_CTLCOLORSTATIC && control == m_depth_label)
            {
                SetTextColor(hdc, ColorRefFromRgb(kControlMutedRed, kControlMutedGreen, kControlMutedBlue));
                SetBkColor(
                    hdc,
                    ColorRefFromRgb(
                        kHeaderPanelRed,
                        kHeaderPanelGreen,
                        kHeaderPanelBlue));
                SetBkMode(hdc, OPAQUE);
                return reinterpret_cast<LRESULT>(m_header_background_brush);
            }

            SetBkMode(hdc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetStockObject(HOLLOW_BRUSH));
        }

        void FillGdiRect(HDC hdc, const RECT& rect, COLORREF color) const
        {
            HBRUSH brush = CreateSolidBrush(color);
            if (!brush)
            {
                return;
            }

            FillRect(hdc, &rect, brush);
            DeleteObject(brush);
        }

        void DrawGdiFrame(HDC hdc, const RECT& rect, COLORREF color) const
        {
            HPEN pen = CreatePen(PS_SOLID, 1, color);
            if (!pen)
            {
                return;
            }

            const HGDIOBJ old_pen = SelectObject(hdc, pen);
            const HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);
            SelectObject(hdc, old_brush);
            SelectObject(hdc, old_pen);
            DeleteObject(pen);
        }

        COLORREF GetButtonFillColor(bool pressed, bool disabled, bool hovered, bool checked = false) const
        {
            if (disabled)
            {
                return ColorRefFromRgb(48, 53, 61);
            }
            if (pressed)
            {
                return ColorRefFromRgb(59, 68, 80);
            }
            if (checked)
            {
                return hovered ? ColorRefFromRgb(74, 88, 108) : ColorRefFromRgb(62, 72, 86);
            }
            if (hovered)
            {
                return ColorRefFromRgb(kControlHoverRed, kControlHoverGreen, kControlHoverBlue);
            }
            return ColorRefFromRgb(kControlSurfaceRed, kControlSurfaceGreen, kControlSurfaceBlue);
        }

        COLORREF GetButtonBorderColor(bool disabled, bool hovered, bool checked = false) const
        {
            if (disabled)
            {
                return ColorRefFromRgb(50, 60, 81);
            }
            if (hovered || checked)
            {
                return ColorRefFromRgb(kControlActiveRed, kControlActiveGreen, kControlActiveBlue);
            }
            return ColorRefFromRgb(kControlBorderRed, kControlBorderGreen, kControlBorderBlue);
        }

        void DrawCustomButton(const DRAWITEMSTRUCT& draw_item) const
        {
            if (!draw_item.hDC || !draw_item.hwndItem)
            {
                return;
            }

            const bool disabled = (draw_item.itemState & ODS_DISABLED) != 0;
            const bool pressed = (draw_item.itemState & ODS_SELECTED) != 0;
            const bool hovered = IsControlHovered(draw_item.hwndItem) && !disabled;
            const bool is_depth_step_button =
                draw_item.hwndItem == m_depth_up_button ||
                draw_item.hwndItem == m_depth_down_button;
            RECT rect = draw_item.rcItem;
            FillGdiRect(draw_item.hDC, rect, GetButtonFillColor(pressed, disabled, hovered));
            DrawGdiFrame(
                draw_item.hDC,
                rect,
                GetButtonBorderColor(disabled, hovered));

            wchar_t text[64] = {};
            GetWindowTextW(draw_item.hwndItem, text, static_cast<int>(std::size(text)));

            const HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw_item.hwndItem, WM_GETFONT, 0, 0));
            const HGDIOBJ old_font = font ? SelectObject(draw_item.hDC, font) : nullptr;
            SetBkMode(draw_item.hDC, TRANSPARENT);
            SetTextColor(
                draw_item.hDC,
                disabled
                    ? ColorRefFromRgb(112, 125, 151)
                    : ColorRefFromRgb(kControlTextRed, kControlTextGreen, kControlTextBlue));

            if (pressed && !is_depth_step_button)
            {
                OffsetRect(&rect, 1, 1);
            }

            DrawTextW(
                draw_item.hDC,
                text,
                -1,
                &rect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            if ((draw_item.itemState & ODS_FOCUS) != 0 && !is_depth_step_button)
            {
                RECT focus_rect = draw_item.rcItem;
                InflateRect(&focus_rect, -3, -3);
                DrawFocusRect(draw_item.hDC, &focus_rect);
            }

            if (old_font)
            {
                SelectObject(draw_item.hDC, old_font);
            }
        }

        void DrawCustomCheckbox(const DRAWITEMSTRUCT& draw_item) const
        {
            if (!draw_item.hDC || !draw_item.hwndItem)
            {
                return;
            }

            const bool disabled = (draw_item.itemState & ODS_DISABLED) != 0;
            const bool pressed = (draw_item.itemState & ODS_SELECTED) != 0;
            const bool hovered = IsControlHovered(draw_item.hwndItem) && !disabled;
            bool checked = false;
            if (draw_item.hwndItem == m_show_tooltip_check)
            {
                checked = m_show_tooltip;
            }
            else if (draw_item.hwndItem == m_show_status_bar_check)
            {
                checked = m_show_status_bar;
            }

            FillGdiRect(
                draw_item.hDC,
                draw_item.rcItem,
                ColorRefFromRgb(
                    kHeaderPanelRed,
                    kHeaderPanelGreen,
                    kHeaderPanelBlue));
            if (hovered)
            {
                FillGdiRect(
                    draw_item.hDC,
                    draw_item.rcItem,
                    ColorRefFromRgb(kControlBackplateRed, kControlBackplateGreen, kControlBackplateBlue));
            }

            RECT checkbox_rect = draw_item.rcItem;
            checkbox_rect.left += 2;
            checkbox_rect.top += 3;
            checkbox_rect.right = checkbox_rect.left + 14;
            checkbox_rect.bottom = checkbox_rect.top + 14;

            FillGdiRect(draw_item.hDC, checkbox_rect, GetButtonFillColor(pressed, disabled, hovered, checked));
            DrawGdiFrame(
                draw_item.hDC,
                checkbox_rect,
                GetButtonBorderColor(disabled, hovered, checked));

            if (checked)
            {
                HPEN pen = CreatePen(
                    PS_SOLID,
                    2,
                    disabled
                        ? ColorRefFromRgb(129, 142, 170)
                        : ColorRefFromRgb(kControlTextRed, kControlTextGreen, kControlTextBlue));
                if (pen)
                {
                    const HGDIOBJ old_pen = SelectObject(draw_item.hDC, pen);
                    MoveToEx(draw_item.hDC, checkbox_rect.left + 3, checkbox_rect.top + 8, nullptr);
                    LineTo(draw_item.hDC, checkbox_rect.left + 6, checkbox_rect.bottom - 4);
                    LineTo(draw_item.hDC, checkbox_rect.right - 3, checkbox_rect.top + 4);
                    SelectObject(draw_item.hDC, old_pen);
                    DeleteObject(pen);
                }
            }

            RECT text_rect = draw_item.rcItem;
            text_rect.left = checkbox_rect.right + 8;

            wchar_t text[96] = {};
            GetWindowTextW(draw_item.hwndItem, text, static_cast<int>(std::size(text)));

            const HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw_item.hwndItem, WM_GETFONT, 0, 0));
            const HGDIOBJ old_font = font ? SelectObject(draw_item.hDC, font) : nullptr;
            SetBkMode(draw_item.hDC, TRANSPARENT);
            SetTextColor(
                draw_item.hDC,
                disabled
                    ? ColorRefFromRgb(112, 125, 151)
                    : ColorRefFromRgb(kControlTextRed, kControlTextGreen, kControlTextBlue));
            DrawTextW(
                draw_item.hDC,
                text,
                -1,
                &text_rect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            if ((draw_item.itemState & ODS_FOCUS) != 0)
            {
                RECT focus_rect = draw_item.rcItem;
                InflateRect(&focus_rect, -1, -1);
                DrawFocusRect(draw_item.hDC, &focus_rect);
            }

            if (old_font)
            {
                SelectObject(draw_item.hDC, old_font);
            }
        }

        LRESULT HandleDrawItem(const DRAWITEMSTRUCT* draw_item) const
        {
            if (!draw_item)
            {
                return FALSE;
            }

            switch (draw_item->CtlID)
            {
            case IDC_SCAN_BUTTON:
            case IDC_BROWSE_BUTTON:
            case IDC_RESET_BUTTON:
            case IDC_DEPTH_UP_BUTTON:
            case IDC_DEPTH_DOWN_BUTTON:
                DrawCustomButton(*draw_item);
                return TRUE;
            case IDC_SHOW_TOOLTIP_CHECK:
            case IDC_SHOW_STATUS_BAR_CHECK:
                DrawCustomCheckbox(*draw_item);
                return TRUE;
            default:
                return FALSE;
            }
        }

        void SetCheckboxState(HWND checkbox, bool checked) const
        {
            if (!checkbox)
            {
                return;
            }

            SendMessageW(
                checkbox,
                BM_SETCHECK,
                checked ? BST_CHECKED : BST_UNCHECKED,
                0);
            InvalidateRect(checkbox, nullptr, TRUE);
        }

        bool IsControlHovered(HWND control) const
        {
            return control && control == m_hovered_control;
        }

        bool IsTrackedControl(HWND control) const
        {
            return control == m_path_edit ||
                control == m_scan_button ||
                control == m_browse_button ||
                control == m_reset_button ||
                control == m_depth_edit ||
                control == m_depth_up_button ||
                control == m_depth_down_button ||
                control == m_show_tooltip_check ||
                control == m_show_status_bar_check;
        }

        bool IsEditControl(HWND control) const
        {
            return control == m_path_edit || control == m_depth_edit;
        }

        void InvalidateTrackedControl(HWND control) const
        {
            if (!control || !IsTrackedControl(control))
            {
                return;
            }

            if (IsEditControl(control))
            {
                RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
                return;
            }

            InvalidateRect(control, nullptr, TRUE);
        }

        void SetHoveredControl(HWND control)
        {
            if (control && (!IsTrackedControl(control) || !IsWindowEnabled(control)))
            {
                control = nullptr;
            }

            if (m_hovered_control == control)
            {
                return;
            }

            HWND old_hovered_control = m_hovered_control;
            m_hovered_control = control;
            InvalidateTrackedControl(old_hovered_control);
            InvalidateTrackedControl(m_hovered_control);
        }

        void OnControlMouseMove(HWND control)
        {
            SetHoveredControl(control);

            TRACKMOUSEEVENT track = {};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = control;
            TrackMouseEvent(&track);
        }

        void OnControlMouseLeave(HWND control)
        {
            if (m_hovered_control == control)
            {
                SetHoveredControl(nullptr);
            }
        }

        void FillEditPadding(HDC hdc, const RECT& client, const RECT& text_rect, COLORREF color) const
        {
            RECT top_rect = client;
            top_rect.bottom = std::clamp(text_rect.top, client.top, client.bottom);
            FillGdiRect(hdc, top_rect, color);

            RECT bottom_rect = client;
            bottom_rect.top = std::clamp(text_rect.bottom, client.top, client.bottom);
            FillGdiRect(hdc, bottom_rect, color);

            RECT left_rect = client;
            left_rect.top = top_rect.bottom;
            left_rect.right = std::clamp(text_rect.left, client.left, client.right);
            left_rect.bottom = bottom_rect.top;
            FillGdiRect(hdc, left_rect, color);

            RECT right_rect = client;
            right_rect.left = std::clamp(text_rect.right, client.left, client.right);
            right_rect.top = top_rect.bottom;
            right_rect.bottom = bottom_rect.top;
            FillGdiRect(hdc, right_rect, color);
        }

        void PaintEditPadding(HWND control) const
        {
            if (!IsEditControl(control))
            {
                return;
            }

            RECT client = {};
            GetClientRect(control, &client);
            RECT text_rect = {};
            SendMessageW(control, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&text_rect));

            const bool hovered = IsControlHovered(control) && IsWindowEnabled(control);
            const COLORREF color = hovered
                ? ColorRefFromRgb(kControlHoverRed, kControlHoverGreen, kControlHoverBlue)
                : ColorRefFromRgb(kControlSurfaceRed, kControlSurfaceGreen, kControlSurfaceBlue);

            HDC hdc = GetDC(control);
            if (!hdc)
            {
                return;
            }

            FillEditPadding(hdc, client, text_rect, color);
            ReleaseDC(control, hdc);
        }

        WNDPROC GetOriginalControlProc(HWND control) const
        {
            for (const auto& subclassed_control : m_subclassed_controls)
            {
                if (subclassed_control.first == control)
                {
                    return subclassed_control.second;
                }
            }

            return nullptr;
        }

        static LRESULT CALLBACK ControlWindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param)
        {
            auto* window = reinterpret_cast<MainWindow*>(GetPropW(hwnd, kControlWindowPropName));
            if (!window)
            {
                return DefWindowProcW(hwnd, message, w_param, l_param);
            }

            return window->HandleControlMessage(hwnd, message, w_param, l_param);
        }

        LRESULT HandleControlMessage(HWND control, UINT message, WPARAM w_param, LPARAM l_param)
        {
            WNDPROC original_proc = GetOriginalControlProc(control);
            if (!original_proc)
            {
                return DefWindowProcW(control, message, w_param, l_param);
            }

            switch (message)
            {
            case WM_MOUSEMOVE:
                OnControlMouseMove(control);
                break;
            case WM_MOUSELEAVE:
                OnControlMouseLeave(control);
                break;
            case WM_PAINT:
            {
                const LRESULT result = CallWindowProcW(original_proc, control, message, w_param, l_param);
                PaintEditPadding(control);
                return result;
            }
            case WM_NCDESTROY:
                OnControlMouseLeave(control);
                break;
            default:
                break;
            }

            return CallWindowProcW(original_proc, control, message, w_param, l_param);
        }

        void AddControlHoverTracking(HWND control)
        {
            if (!control || GetPropW(control, kControlWindowPropName))
            {
                return;
            }

            SetPropW(control, kControlWindowPropName, this);
            WNDPROC original_proc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(
                    control,
                    GWLP_WNDPROC,
                    reinterpret_cast<LONG_PTR>(&MainWindow::ControlWindowProc)));
            if (!original_proc)
            {
                RemovePropW(control, kControlWindowPropName);
                return;
            }

            m_subclassed_controls.emplace_back(control, original_proc);
        }

        void CreateControlHoverTracking()
        {
            AddControlHoverTracking(m_path_edit);
            AddControlHoverTracking(m_scan_button);
            AddControlHoverTracking(m_browse_button);
            AddControlHoverTracking(m_reset_button);
            AddControlHoverTracking(m_depth_edit);
            AddControlHoverTracking(m_depth_up_button);
            AddControlHoverTracking(m_depth_down_button);
            AddControlHoverTracking(m_show_tooltip_check);
            AddControlHoverTracking(m_show_status_bar_check);
        }

        void DestroyControlHoverTracking()
        {
            SetHoveredControl(nullptr);
            for (const auto& subclassed_control : m_subclassed_controls)
            {
                HWND control = subclassed_control.first;
                if (!control || !IsWindow(control))
                {
                    continue;
                }

                if (GetWindowLongPtrW(control, GWLP_WNDPROC) ==
                    reinterpret_cast<LONG_PTR>(&MainWindow::ControlWindowProc))
                {
                    SetWindowLongPtrW(
                        control,
                        GWLP_WNDPROC,
                        reinterpret_cast<LONG_PTR>(subclassed_control.second));
                }
                RemovePropW(control, kControlWindowPropName);
            }
            m_subclassed_controls.clear();
        }

        void ApplyEditTextInsets(
            HWND edit,
            int left_padding,
            int top_padding,
            int right_padding,
            int bottom_padding) const
        {
            if (!edit)
            {
                return;
            }

            RECT client = {};
            GetClientRect(edit, &client);
            client.left += left_padding;
            client.top += top_padding;
            client.right = std::max(client.left + 1, client.right - right_padding);
            client.bottom = std::max(client.top + 1, client.bottom - bottom_padding);

            SendMessageW(edit, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&client));
            SendMessageW(
                edit,
                EM_SETMARGINS,
                EC_LEFTMARGIN | EC_RIGHTMARGIN,
                MAKELPARAM(left_padding, right_padding));
        }

        void CreateHeaderControls()
        {
            HFONT gui_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            const std::wstring initial_path = m_options.path.empty() ? GetDefaultPathFieldValue() : m_options.path;

            m_path_edit = CreateWindowExW(
                0,
                L"EDIT",
                initial_path.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_MULTILINE | ES_AUTOHSCROLL,
                0,
                0,
                0,
                0,
                m_hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PATH_EDIT)),
                nullptr,
                nullptr);

            m_scan_button = CreateWindowExW(
                0,
                L"BUTTON",
                L"Scan",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0,
                0,
                0,
                0,
                m_hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SCAN_BUTTON)),
                nullptr,
                nullptr);

            m_browse_button = CreateWindowExW(
                0,
                L"BUTTON",
                L"Browse...",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0,
                0,
                0,
                0,
                m_hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BROWSE_BUTTON)),
                nullptr,
                nullptr);

            m_reset_button = CreateWindowExW(
                0,
                L"BUTTON",
                L"Reset treemap",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0,
                0,
                0,
                0,
                m_hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RESET_BUTTON)),
                nullptr,
                nullptr);

            m_depth_label = CreateWindowExW(
                0,
                L"STATIC",
                L"Depth",
                WS_CHILD | WS_VISIBLE,
                0,
                0,
                0,
                0,
                m_hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DEPTH_LABEL)),
                nullptr,
                nullptr);

            const std::wstring depth_text = std::to_wstring(m_max_depth);
            m_depth_edit = CreateWindowExW(
                0,
                L"EDIT",
                depth_text.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_MULTILINE | ES_AUTOHSCROLL | ES_CENTER,
                0,
                0,
                0,
                0,
                m_hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DEPTH_EDIT)),
                nullptr,
                nullptr);

            m_depth_up_button = CreateWindowExW(
                0,
                L"BUTTON",
                L"\x25B2",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0,
                0,
                0,
                0,
                m_hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DEPTH_UP_BUTTON)),
                nullptr,
                nullptr);

            m_depth_down_button = CreateWindowExW(
                0,
                L"BUTTON",
                L"\x25BC",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0,
                0,
                0,
                0,
                m_hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DEPTH_DOWN_BUTTON)),
                nullptr,
                nullptr);

            m_show_tooltip_check = CreateWindowExW(
                0,
                L"BUTTON",
                L"Show tooltip",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_CHECKBOX | BS_OWNERDRAW,
                0,
                0,
                0,
                0,
                m_hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SHOW_TOOLTIP_CHECK)),
                nullptr,
                nullptr);

            m_show_status_bar_check = CreateWindowExW(
                0,
                L"BUTTON",
                L"Show status bar",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_CHECKBOX | BS_OWNERDRAW,
                0,
                0,
                0,
                0,
                m_hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SHOW_STATUS_BAR_CHECK)),
                nullptr,
                nullptr);

            if (m_path_edit)
            {
                SendMessageW(m_path_edit, WM_SETFONT, reinterpret_cast<WPARAM>(gui_font), TRUE);
            }
            if (m_scan_button)
            {
                SendMessageW(m_scan_button, WM_SETFONT, reinterpret_cast<WPARAM>(gui_font), TRUE);
            }
            if (m_browse_button)
            {
                SendMessageW(m_browse_button, WM_SETFONT, reinterpret_cast<WPARAM>(gui_font), TRUE);
            }
            if (m_reset_button)
            {
                SendMessageW(m_reset_button, WM_SETFONT, reinterpret_cast<WPARAM>(gui_font), TRUE);
            }
            if (m_depth_label)
            {
                SendMessageW(m_depth_label, WM_SETFONT, reinterpret_cast<WPARAM>(gui_font), TRUE);
            }
            if (m_depth_edit)
            {
                SendMessageW(m_depth_edit, WM_SETFONT, reinterpret_cast<WPARAM>(gui_font), TRUE);
            }
            if (m_depth_up_button)
            {
                SendMessageW(m_depth_up_button, WM_SETFONT, reinterpret_cast<WPARAM>(gui_font), TRUE);
            }
            if (m_depth_down_button)
            {
                SendMessageW(m_depth_down_button, WM_SETFONT, reinterpret_cast<WPARAM>(gui_font), TRUE);
            }
            if (m_show_tooltip_check)
            {
                SendMessageW(
                    m_show_tooltip_check,
                    WM_SETFONT,
                    reinterpret_cast<WPARAM>(gui_font),
                    TRUE);
                SetCheckboxState(m_show_tooltip_check, m_show_tooltip);
            }
            if (m_show_status_bar_check)
            {
                SendMessageW(
                    m_show_status_bar_check,
                    WM_SETFONT,
                    reinterpret_cast<WPARAM>(gui_font),
                    TRUE);
                SetCheckboxState(m_show_status_bar_check, m_show_status_bar);
            }

            CreateControlHoverTracking();
            UpdateHeaderControlsLayout();
        }

        void UpdateHeaderControlsLayout()
        {
            if (!m_hwnd)
            {
                return;
            }

            RECT client = {};
            GetClientRect(m_hwnd, &client);
            const int width = std::max<LONG>(client.right - client.left, 1);

            const int outer_padding = static_cast<int>(kOuterPadding);
            const int inner_padding = 22;
            const int group_gap = 6;
            const int group_top = outer_padding + 4;
            const int max_control_area_width = std::max(
                340,
                width - (outer_padding * 2) - (inner_padding * 2) - 140);
            const int min_control_area_width = std::min(560, max_control_area_width);
            const int control_area_width = std::clamp(
                static_cast<int>((width - (outer_padding * 2) - (inner_padding * 2)) * 0.48f),
                min_control_area_width,
                std::min(680, max_control_area_width));
            const int control_left = width - outer_padding - inner_padding - control_area_width;
            const int scan_group_height = 40;
            const int lower_row_vertical_offset = 3;
            const int lower_group_top = group_top + scan_group_height + group_gap + lower_row_vertical_offset;
            const int lower_group_height = 30;
            const int header_info_left = outer_padding + inner_padding;
            const int header_info_right = std::max(header_info_left + 120, control_left - 18);
            const int header_text_vertical_offset = -5;
            const int header_text_top = group_top + header_text_vertical_offset;

            const int scan_group_left = control_left + 2;
            const int scan_group_top_inner = group_top + 10;
            const int browse_width = 94;
            const int scan_width = 74;
            const int reset_width = 112;
            const int button_gap = 8;
            const int browse_left = control_left + control_area_width - 12 - browse_width;
            const int scan_left = browse_left - button_gap - scan_width;
            const int edit_width = std::max(180, scan_left - button_gap - scan_group_left);

            const int lower_row_left = control_left + 2;
            const int display_checkbox_gap = 4;
            const int lower_separator_leading_gap = 10;
            const int lower_separator_trailing_gap = 10;
            const int show_tooltip_width = 84;
            const int show_status_bar_width = 108;
            const int checkbox_group_width =
                show_tooltip_width + display_checkbox_gap + show_status_bar_width;
            const int display_group_left = lower_row_left;
            const int display_group_width = checkbox_group_width;

            const int depth_label_width = 38;
            const int depth_edit_width = 42;
            const int depth_button_width = 18;
            const int depth_button_gap = 4;
            const int depth_label_gap = 4;
            const int depth_group_width =
                depth_label_width + depth_label_gap + depth_edit_width + depth_button_gap + depth_button_width;
            m_checkbox_depth_separator_x = static_cast<float>(
                display_group_left + display_group_width + lower_separator_leading_gap);
            const int depth_controls_left =
                static_cast<int>(m_checkbox_depth_separator_x) + lower_separator_trailing_gap;
            m_depth_reset_separator_x = static_cast<float>(
                depth_controls_left + depth_group_width + lower_separator_leading_gap);
            const int reset_left =
                static_cast<int>(m_depth_reset_separator_x) + lower_separator_trailing_gap;

            m_header_info_rect = MakeRenderRect(
                static_cast<float>(header_info_left),
                static_cast<float>(header_text_top),
                static_cast<float>(header_info_right),
                static_cast<float>(outer_padding + static_cast<int>(kHeaderHeight) - 16));
            m_scan_group_rect = MakeRenderRect(
                static_cast<float>(control_left),
                static_cast<float>(group_top),
                static_cast<float>(control_left + control_area_width),
                static_cast<float>(group_top + scan_group_height));
            m_depth_group_rect = MakeRenderRect(
                static_cast<float>(depth_controls_left),
                static_cast<float>(lower_group_top),
                static_cast<float>(depth_controls_left + depth_group_width),
                static_cast<float>(lower_group_top + lower_group_height));
            m_display_group_rect = MakeRenderRect(
                static_cast<float>(display_group_left),
                static_cast<float>(lower_group_top),
                static_cast<float>(display_group_left + display_group_width),
                static_cast<float>(lower_group_top + lower_group_height));

            const int depth_controls_top = lower_group_top + 1;
            const int depth_label_left = depth_controls_left;
            const int depth_edit_left = depth_label_left + depth_label_width + depth_label_gap;
            const int depth_button_left = depth_edit_left + depth_edit_width + depth_button_gap;

            const int show_tooltip_top = lower_group_top + 5;
            const int show_status_bar_top = show_tooltip_top;
            const int show_tooltip_left = display_group_left + 4;
            const int show_status_bar_left = show_tooltip_left + show_tooltip_width + display_checkbox_gap;

            if (m_path_edit)
            {
                MoveWindow(m_path_edit, scan_group_left, scan_group_top_inner, edit_width, 28, TRUE);
                ApplyEditTextInsets(m_path_edit, 8, 6, 8, 4);
            }
            if (m_scan_button)
            {
                MoveWindow(m_scan_button, scan_left, scan_group_top_inner, scan_width, 28, TRUE);
            }
            if (m_reset_button)
            {
                MoveWindow(m_reset_button, reset_left, depth_controls_top, reset_width, 28, TRUE);
            }
            if (m_browse_button)
            {
                MoveWindow(m_browse_button, browse_left, scan_group_top_inner, browse_width, 28, TRUE);
            }
            if (m_depth_label)
            {
                MoveWindow(m_depth_label, depth_label_left, depth_controls_top + 6, depth_label_width, 18, TRUE);
            }
            if (m_depth_edit)
            {
                MoveWindow(m_depth_edit, depth_edit_left, depth_controls_top, depth_edit_width, 28, TRUE);
                ApplyEditTextInsets(m_depth_edit, 2, 5, 2, 4);
            }
            if (m_depth_up_button)
            {
                MoveWindow(m_depth_up_button, depth_button_left, depth_controls_top, depth_button_width, 13, TRUE);
            }
            if (m_depth_down_button)
            {
                MoveWindow(m_depth_down_button, depth_button_left, depth_controls_top + 14, depth_button_width, 14, TRUE);
            }
            if (m_show_tooltip_check)
            {
                MoveWindow(
                    m_show_tooltip_check,
                    show_tooltip_left,
                    show_tooltip_top,
                    show_tooltip_width,
                    18,
                    TRUE);
            }
            if (m_show_status_bar_check)
            {
                MoveWindow(
                    m_show_status_bar_check,
                    show_status_bar_left,
                    show_status_bar_top,
                    show_status_bar_width,
                    18,
                    TRUE);
            }
        }

        std::wstring GetPathFromEdit() const
        {
            if (!m_path_edit)
            {
                return m_options.path;
            }

            const int length = GetWindowTextLengthW(m_path_edit);
            std::wstring value(static_cast<size_t>(std::max(length, 0)), L'\0');
            if (length > 0)
            {
                GetWindowTextW(m_path_edit, value.data(), length + 1);
            }
            return value;
        }

        void SetScanControlsEnabled(bool enabled)
        {
            if (m_path_edit)
            {
                EnableWindow(m_path_edit, enabled);
            }
            if (m_scan_button)
            {
                EnableWindow(m_scan_button, enabled);
            }
            if (m_reset_button)
            {
                EnableWindow(m_reset_button, enabled && m_tree != nullptr);
            }
            if (m_depth_edit)
            {
                EnableWindow(m_depth_edit, enabled);
            }
            if (m_depth_up_button)
            {
                EnableWindow(m_depth_up_button, enabled);
            }
            if (m_depth_down_button)
            {
                EnableWindow(m_depth_down_button, enabled);
            }
            if (m_browse_button)
            {
                EnableWindow(m_browse_button, enabled);
            }
            if (m_show_tooltip_check)
            {
                EnableWindow(m_show_tooltip_check, enabled);
            }
            if (m_show_status_bar_check)
            {
                EnableWindow(m_show_status_bar_check, enabled);
            }
            if (m_hovered_control && !IsWindowEnabled(m_hovered_control))
            {
                SetHoveredControl(nullptr);
            }
        }

        void ClearHoverState()
        {
            m_hovered_index = -1;
            m_hovered_breadcrumb_index = -1;
            m_mouse_position = MakeRenderPoint(-1.0f, -1.0f);
            m_tooltip_position = MakeRenderPoint(-1.0f, -1.0f);
            m_tooltip_target_position = MakeRenderPoint(-1.0f, -1.0f);
            ResetTooltipRedrawBounds();
            StopTooltipAnimation();
        }

        void RebuildAndRedraw()
        {
            RebuildLayout();
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }

        void ApplyDepthLimit(int depth_limit)
        {
            const int clamped_depth_limit = std::clamp(depth_limit, 1, 32);
            if (clamped_depth_limit == m_max_depth)
            {
                return;
            }

            m_max_depth = clamped_depth_limit;
            m_expanded_reveal_nodes.clear();
            if (m_tree && !m_scan_in_progress)
            {
                RebuildAndRedraw();
            }
        }

        void ResetTreemapView()
        {
            if (!m_tree || m_scan_in_progress)
            {
                return;
            }

            ApplyFreshScanResultView();
        }

        void ApplyFreshScanResultView()
        {
            m_has_error = false;
            m_error_text.clear();
            m_status_text = L"Ready";
            m_rects.clear();
            ResetToFreshScanView();
            SetScanControlsEnabled(true);
            RebuildAndRedraw();
        }

        void ResetToFreshScanView()
        {
            if (!m_tree)
            {
                return;
            }

            m_view_root_id = m_tree->RootId();
            m_expanded_reveal_nodes.clear();
            m_layout_options = visualization::TreemapLayoutOptions();
            m_rects.clear();
            m_breadcrumbs.clear();
            ClearHoverState();
        }

        void SetDepthEditValue(int depth_limit)
        {
            if (!m_depth_edit)
            {
                ApplyDepthLimit(depth_limit);
                return;
            }

            const int clamped_depth_limit = std::clamp(depth_limit, 1, 32);
            const std::wstring depth_text = std::to_wstring(clamped_depth_limit);
            m_updating_depth_edit = true;
            SetWindowTextW(m_depth_edit, depth_text.c_str());
            m_updating_depth_edit = false;
            ApplyDepthLimit(clamped_depth_limit);
        }

        void AdjustDepthLimit(int delta)
        {
            int depth_limit = m_max_depth;
            if (!TryGetDepthLimitFromEdit(&depth_limit))
            {
                depth_limit = m_max_depth;
            }

            SetDepthEditValue(depth_limit + delta);
        }

        bool TryGetDepthLimitFromEdit(int* depth_limit) const
        {
            if (!depth_limit)
            {
                return false;
            }

            if (!m_depth_edit)
            {
                *depth_limit = m_max_depth;
                return true;
            }

            wchar_t buffer[32] = {};
            GetWindowTextW(m_depth_edit, buffer, static_cast<int>(std::size(buffer)));
            const std::wstring text = TrimWhitespace(buffer);
            if (text.empty())
            {
                return false;
            }

            wchar_t* end = nullptr;
            const long parsed = wcstol(text.c_str(), &end, 10);
            if (end == text.c_str() || (end && *end != L'\0'))
            {
                return false;
            }

            *depth_limit = static_cast<int>(std::clamp<long>(parsed, 1, 32));
            return true;
        }

        bool BrowseForFolder(std::wstring* selected_path)
        {
            if (!selected_path)
            {
                return false;
            }

            ComPtr<IFileDialog> dialog;
            HRESULT hr = CoCreateInstance(
                CLSID_FileOpenDialog,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(dialog.GetAddressOf()));
            if (FAILED(hr) || !dialog)
            {
                return false;
            }

            DWORD options = 0;
            hr = dialog->GetOptions(&options);
            if (FAILED(hr))
            {
                return false;
            }

            hr = dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
            if (FAILED(hr))
            {
                return false;
            }

            const std::wstring current_path = GetPathFromEdit();
            if (!current_path.empty())
            {
                ComPtr<IShellItem> folder_item;
                hr = SHCreateItemFromParsingName(current_path.c_str(), nullptr, IID_PPV_ARGS(folder_item.GetAddressOf()));
                if (SUCCEEDED(hr) && folder_item)
                {
                    dialog->SetFolder(folder_item.Get());
                }
            }

            hr = dialog->Show(m_hwnd);
            if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
            {
                return false;
            }
            if (FAILED(hr))
            {
                return false;
            }

            ComPtr<IShellItem> result;
            hr = dialog->GetResult(result.GetAddressOf());
            if (FAILED(hr) || !result)
            {
                return false;
            }

            PWSTR path = nullptr;
            hr = result->GetDisplayName(SIGDN_FILESYSPATH, &path);
            if (FAILED(hr) || !path)
            {
                return false;
            }

            *selected_path = path;
            CoTaskMemFree(path);
            return true;
        }

        static std::wstring TrimWhitespace(std::wstring value)
        {
            const size_t first = value.find_first_not_of(L" \t\r\n");
            if (first == std::wstring::npos)
            {
                return {};
            }
            const size_t last = value.find_last_not_of(L" \t\r\n");
            return value.substr(first, last - first + 1);
        }

        void BeginScanFromUiPath()
        {
            if (m_scan_in_progress)
            {
                return;
            }

            const std::wstring path = TrimWhitespace(GetPathFromEdit());
            if (path.empty())
            {
                MessageBoxW(m_hwnd, L"Please enter a folder path to scan.", L"SpaceScanner", MB_OK | MB_ICONINFORMATION);
                return;
            }

            m_options.path = NormalizeScanPath(path);
            m_requested_path_is_ntfs = IsNtfsPath(m_options.path);
            StartScan();
        }

        void OnCommand(int control_id, int notification_code, HWND control)
        {
            if (control_id == IDC_DEPTH_EDIT && control == m_depth_edit && notification_code == EN_CHANGE)
            {
                if (m_updating_depth_edit)
                {
                    return;
                }

                int depth_limit = 0;
                if (TryGetDepthLimitFromEdit(&depth_limit))
                {
                    ApplyDepthLimit(depth_limit);
                }
                return;
            }

            if (control_id == IDC_DEPTH_UP_BUTTON && control == m_depth_up_button && notification_code == BN_CLICKED)
            {
                AdjustDepthLimit(1);
                return;
            }

            if (control_id == IDC_DEPTH_DOWN_BUTTON && control == m_depth_down_button && notification_code == BN_CLICKED)
            {
                AdjustDepthLimit(-1);
                return;
            }

            if (control_id == IDC_SHOW_TOOLTIP_CHECK && control == m_show_tooltip_check && notification_code == BN_CLICKED)
            {
                m_show_tooltip = !m_show_tooltip;
                SetCheckboxState(m_show_tooltip_check, m_show_tooltip);
                if (!m_show_tooltip)
                {
                    StopTooltipAnimation();
                    ResetTooltipRedrawBounds();
                }
                else if (m_hovered_index >= 0 || m_hovered_breadcrumb_index >= 0)
                {
                    m_tooltip_position = m_mouse_position;
                    m_tooltip_target_position = m_mouse_position;
                    m_tooltip_redraw_bounds = MeasureTooltipBoundsAt(m_tooltip_position);
                    StartTooltipAnimation();
                }
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return;
            }

            if (control_id == IDC_SHOW_STATUS_BAR_CHECK && control == m_show_status_bar_check && notification_code == BN_CLICKED)
            {
                m_show_status_bar = !m_show_status_bar;
                SetCheckboxState(m_show_status_bar_check, m_show_status_bar);
                RebuildLayout();
                InvalidateCanvas();
                return;
            }

            if (control_id == IDC_SCAN_BUTTON && control == m_scan_button)
            {
                BeginScanFromUiPath();
                return;
            }

            if (control_id == IDC_RESET_BUTTON && notification_code == BN_CLICKED)
            {
                ResetTreemapView();
                return;
            }

            if (control_id == IDC_BROWSE_BUTTON && control == m_browse_button)
            {
                if (m_scan_in_progress)
                {
                    return;
                }

                std::wstring selected_path;
                if (BrowseForFolder(&selected_path) && m_path_edit)
                {
                    SetWindowTextW(m_path_edit, selected_path.c_str());
                    BeginScanFromUiPath();
                }
            }
        }

        void StartScan()
        {
            m_scan_in_progress = true;
            m_has_error = false;
            ClearHoverState();
            m_rects.clear();
            m_breadcrumbs.clear();
            m_status_text = L"Scanning " + m_options.path + L" ...";
            SetScanControlsEnabled(false);
            InvalidateRect(m_hwnd, nullptr, FALSE);

            const HWND hwnd = m_hwnd;
            const std::wstring path = m_options.path;
            const std::shared_ptr<SharedScanState> shared_state = m_shared_scan_state;
            std::thread(
                [hwnd, path, shared_state]()
                {
                    auto payload = std::make_unique<ScanPayload>();
                    payload->path = path;
                    payload->tree = std::make_unique<FileTree>();

                    Stopwatch timer;
                    std::wstring error;
                    ntfs::MftScanStats stats;
                    bool ok = false;
                    bool used_mft_scan = false;

                    if (IsNtfsPath(path))
                    {
                        ok = ntfs::ScanPathMft(path, payload->tree.get(), &error, &stats);
                        used_mft_scan = ok;
                    }
                    else
                    {
                        ok = ScanDirectory(path, payload->tree.get(), nullptr);
                        if (ok)
                        {
                            payload->tree->Finalize();
                        }
                    }

                    payload->elapsed_seconds = timer.ElapsedSeconds();
                    payload->used_mft_scan = used_mft_scan;
                    if (!ok)
                    {
                        payload->error = error.empty() ? L"Unknown scan error." : error;
                        payload->tree.reset();
                    }

                    {
                        std::lock_guard<std::mutex> lock(shared_state->mutex);
                        shared_state->pending_payload = std::move(payload);
                    }

                    if (IsWindow(hwnd))
                    {
                        PostMessageW(hwnd, ok ? WM_APP_SCAN_COMPLETE : WM_APP_SCAN_FAILED, 0, 0);
                    }
                }
            ).detach();
        }

        void ApplyCompletedScan(bool failed)
        {
            std::unique_ptr<ScanPayload> payload;
            {
                std::lock_guard<std::mutex> lock(m_shared_scan_state->mutex);
                payload = std::move(m_shared_scan_state->pending_payload);
            }

            m_scan_in_progress = false;
            ClearHoverState();
            m_rects.clear();

            if (!payload)
            {
                m_has_error = true;
                m_status_text = L"Scan completed without payload.";
                SetScanControlsEnabled(true);
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return;
            }

            m_scan_path = payload->path;
            m_elapsed_seconds = payload->elapsed_seconds;

            if (failed || !payload->tree)
            {
                m_has_error = true;
                m_error_text = payload->error.empty() ? L"Scan failed." : payload->error;
                m_status_text = L"Scan failed";
                SetScanControlsEnabled(true);
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return;
            }

            m_tree = std::move(payload->tree);
            m_tree_stats = m_tree->ComputeStats();
            m_used_mft_scan = payload->used_mft_scan;
            ApplyFreshScanResultView();
        }

        void DiscardDeviceResources()
        {
            m_painter.DiscardDeviceResources();
        }
        void OnSize()
        {
            UpdateHeaderControlsLayout();
            if (m_painter.HasRenderTarget())
            {
                m_painter.Resize(m_hwnd);
            }
            RebuildLayout();
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }

        void InvalidateCanvas()
        {
            RECT redraw_rect = RectFToRect(m_canvas_rect);
            if (m_show_status_bar)
            {
                const RECT status_rect = RectFToRect(m_status_bar_rect);
                redraw_rect.left = std::min(redraw_rect.left, status_rect.left);
                redraw_rect.top = std::min(redraw_rect.top, status_rect.top);
                redraw_rect.right = std::max(redraw_rect.right, status_rect.right);
                redraw_rect.bottom = std::max(redraw_rect.bottom, status_rect.bottom);
            }
            InvalidateRect(m_hwnd, &redraw_rect, FALSE);
        }

        RenderRect MeasureTooltipBoundsAt(const RenderPoint& tooltip_position)
        {
            if (!m_hwnd || !m_show_tooltip)
            {
                return MakeRenderRect(0, 0, 0, 0);
            }

            MainWindowDrawingState state = BuildDrawingState();
            state.tooltip_position = tooltip_position;
            return m_painter.GetHoverTooltipBounds(m_hwnd, state);
        }

        void InvalidateTooltipTransition(const RenderRect& old_bounds, const RenderPoint& new_position)
        {
            RenderRect new_bounds = MeasureTooltipBoundsAt(new_position);
            RenderRect redraw_bounds = UnionRenderRect(old_bounds, new_bounds);
            if (IsRenderRectEmpty(redraw_bounds))
            {
                m_tooltip_redraw_bounds = new_bounds;
                return;
            }

            RECT redraw_rect = RectFToRect(redraw_bounds);
            InvalidateRect(m_hwnd, &redraw_rect, FALSE);
            m_tooltip_redraw_bounds = new_bounds;
        }

        RenderRect GetTreemapRectBounds(int rect_index) const
        {
            if (rect_index < 0 || rect_index >= static_cast<int>(m_rects.size()))
            {
                return MakeRenderRect(0, 0, 0, 0);
            }

            const visualization::TreemapRect& rect = m_rects[static_cast<size_t>(rect_index)];
            return MakeRenderRect(
                m_treemap_content_rect.left + static_cast<float>(rect.x),
                m_treemap_content_rect.top + static_cast<float>(rect.y),
                m_treemap_content_rect.left + static_cast<float>(rect.x + rect.width),
                m_treemap_content_rect.top + static_cast<float>(rect.y + rect.height));
        }

        RenderRect GetHoverVisualBounds(int rect_index, int breadcrumb_index) const
        {
            const float dpi_scale = GetWindowDpiScale(m_hwnd);
            if (breadcrumb_index >= 0 && breadcrumb_index < static_cast<int>(m_breadcrumbs.size()))
            {
                return InflateRenderRect(
                    m_breadcrumbs[static_cast<size_t>(breadcrumb_index)].frame_rect,
                    ScaleForDpi(12.0f, dpi_scale));
            }

            return InflateRenderRect(GetTreemapRectBounds(rect_index), ScaleForDpi(4.0f, dpi_scale));
        }

        void InvalidateHoverTransition(
            int old_hovered_index,
            int old_hovered_breadcrumb_index,
            const RenderRect& old_tooltip_bounds)
        {
            RenderRect redraw_bounds = UnionRenderRect(
                old_tooltip_bounds,
                m_tooltip_redraw_bounds);
            redraw_bounds = UnionRenderRect(
                redraw_bounds,
                GetHoverVisualBounds(old_hovered_index, old_hovered_breadcrumb_index));
            redraw_bounds = UnionRenderRect(
                redraw_bounds,
                GetHoverVisualBounds(m_hovered_index, m_hovered_breadcrumb_index));
            if (m_show_status_bar)
            {
                redraw_bounds = UnionRenderRect(redraw_bounds, m_status_bar_rect);
            }

            if (IsRenderRectEmpty(redraw_bounds))
            {
                return;
            }

            RECT redraw_rect = RectFToRect(redraw_bounds);
            InvalidateRect(m_hwnd, &redraw_rect, FALSE);
        }

        void ResetTooltipRedrawBounds()
        {
            m_tooltip_redraw_bounds = MakeRenderRect(0, 0, 0, 0);
        }

        void UpdateCanvasRect()
        {
            RECT client = {};
            GetClientRect(m_hwnd, &client);
            const float width = static_cast<float>(std::max<LONG>(client.right - client.left, 1));
            const float height = static_cast<float>(std::max<LONG>(client.bottom - client.top, 1));
            const float dpi_scale = GetWindowDpiScale(m_hwnd);
            const float status_bar_height = ScaleForDpi(kStatusBarHeight, dpi_scale);
            const float status_bar_gap = ScaleForDpi(kStatusBarGap, dpi_scale);
            const float status_bar_bottom_padding = ScaleForDpi(kStatusBarBottomPadding, dpi_scale);
            const float status_bar_top = std::max(
                kOuterPadding + kHeaderHeight + 1.0f,
                height - status_bar_bottom_padding - status_bar_height);

            if (m_show_status_bar)
            {
                m_status_bar_rect = MakeRenderRect(
                    kOuterPadding,
                    status_bar_top,
                    std::max(width - kOuterPadding, kOuterPadding + 1.0f),
                    std::max(status_bar_top + status_bar_height, status_bar_top + 1.0f));
            }
            else
            {
                m_status_bar_rect = MakeRenderRect(0, 0, 0, 0);
            }

            m_canvas_rect = MakeRenderRect(
                kOuterPadding,
                kOuterPadding + kHeaderHeight,
                std::max(width - kOuterPadding, kOuterPadding + 1.0f),
                std::max(
                    (m_show_status_bar ? status_bar_top - status_bar_gap : height - kOuterPadding),
                    kOuterPadding + kHeaderHeight + 1.0f));
        }

        void RebuildLayout()
        {
            UpdateCanvasRect();
            m_focus_up_rect = MakeRenderRect(0, 0, 0, 0);
            m_treemap_content_rect = m_canvas_rect;
            m_breadcrumbs.clear();
            if (!m_tree)
            {
                m_rects.clear();
                return;
            }

            std::vector<uint64_t> ancestors;
            uint64_t current = m_view_root_id;
            size_t guard = 0;
            while (current != 0 && guard < 128)
            {
                ancestors.push_back(current);
                if (current == m_tree->RootId())
                {
                    break;
                }
                const Node* node = m_tree->GetNode(current);
                if (!node || node->parent_id == 0 || node->parent_id == current)
                {
                    break;
                }
                current = node->parent_id;
                ++guard;
            }
            std::reverse(ancestors.begin(), ancestors.end());
            size_t start_index = 0;
            bool truncated = false;
            if (ancestors.size() > kMaxBreadcrumbStrips)
            {
                start_index = ancestors.size() - kMaxBreadcrumbStrips;
                truncated = true;
            }

            RenderRect nested_rect = MakeRenderRect(
                m_canvas_rect.left + 1.0f,
                m_canvas_rect.top + 1.0f,
                m_canvas_rect.right - 1.0f,
                m_canvas_rect.bottom - 1.0f);
            const float dpi_scale = GetWindowDpiScale(m_hwnd);
            const float treemap_top_slice_height = ScaleForDpi(kTreemapTopSliceHeight, dpi_scale);
            const float treemap_side_slice_width = ScaleForDpi(kTreemapSideSliceWidth, dpi_scale);
            const float treemap_bottom_slice_height = ScaleForDpi(kTreemapBottomSliceHeight, dpi_scale);

            const size_t visible_count = ancestors.size() - start_index;
            for (size_t i = 0; i < visible_count; ++i)
            {
                const uint64_t node_id = ancestors[start_index + i];
                const Node* node = m_tree->GetNode(node_id);
                if (!node)
                {
                    continue;
                }

                BreadcrumbStrip strip;
                strip.node_id = node_id;
                strip.frame_rect = nested_rect;
                strip.rect = MakeRenderRect(
                    nested_rect.left,
                    nested_rect.top,
                    nested_rect.right,
                    std::min(nested_rect.bottom, nested_rect.top + treemap_top_slice_height));
                strip.content_rect = MakeRenderRect(
                    nested_rect.left + treemap_side_slice_width,
                    strip.rect.bottom,
                    nested_rect.right - treemap_side_slice_width,
                    nested_rect.bottom - treemap_bottom_slice_height);

                std::wstring text = node->name.empty() ? m_tree->BuildPath(node_id) : node->name;
                if (i == 0 && truncated)
                {
                    text = L"...\\" + text;
            }
            strip.text = text + L" - " + HumanizeBytes(node->size);

            const std::wstring path = m_tree->BuildPath(node_id);
            strip.style = visualization::BuildTreemapVisualStyle(
                node->name.empty() ? path : node->name,
                path,
                true);
                m_breadcrumbs.push_back(std::move(strip));
                nested_rect = m_breadcrumbs.back().content_rect;
            }

            if (!m_breadcrumbs.empty())
            {
                m_focus_up_rect = m_breadcrumbs.back().rect;
                m_treemap_content_rect = m_breadcrumbs.back().content_rect;
            }
            else
            {
                m_focus_up_rect = nested_rect;
                m_treemap_content_rect = nested_rect;
            }

            const float width = std::max(m_treemap_content_rect.right - m_treemap_content_rect.left, 1.0f);
            const float height = std::max(m_treemap_content_rect.bottom - m_treemap_content_rect.top, 1.0f);
            const size_t current_root_depth = GetAbsoluteNodeDepth(m_view_root_id);
            const size_t base_visible_depth = static_cast<size_t>(std::max(m_max_depth, 0));
            const size_t local_depth_limit =
                current_root_depth < base_visible_depth
                    ? (base_visible_depth - current_root_depth - 1)
                    : 0;
            m_layout_options.width = width;
            m_layout_options.height = height;
            m_layout_options.tile_padding = 2.0 * dpi_scale;
            m_layout_options.directory_header_height = 18.0 * dpi_scale;
            m_layout_options.minimum_child_side = 12.0 * dpi_scale;
            m_layout_options.minimum_label_width = 72.0 * dpi_scale;
            m_layout_options.minimum_label_height = 28.0 * dpi_scale;
            m_layout_options.max_depth = local_depth_limit;
            m_layout_options.max_rectangles = 20000;
            m_layout_options.max_children_per_directory = 0;
            m_layout_options.minimum_child_ratio = 0.0;
            m_layout_options.expanded_node_ids.assign(m_expanded_reveal_nodes.begin(), m_expanded_reveal_nodes.end());
            //m_layout_options.max_rectangles = 5000;
            //m_layout_options.max_children_per_directory = 56;
            //m_layout_options.minimum_child_ratio = 0.0015;
            m_rects = visualization::BuildTreemapLayout(*m_tree, m_view_root_id, m_layout_options);
        }

        int HitTestTreemap(float x, float y) const
        {
            if (m_scan_in_progress || !m_tree)
            {
                return -1;
            }

            if (x < m_treemap_content_rect.left || x > m_treemap_content_rect.right ||
                y < m_treemap_content_rect.top || y > m_treemap_content_rect.bottom)
            {
                return -1;
            }

            const float local_x = x - m_treemap_content_rect.left;
            const float local_y = y - m_treemap_content_rect.top;
            for (int index = static_cast<int>(m_rects.size()) - 1; index >= 0; --index)
            {
                const visualization::TreemapRect& rect = m_rects[static_cast<size_t>(index)];
                if (local_x >= rect.x && local_x <= rect.x + rect.width &&
                    local_y >= rect.y && local_y <= rect.y + rect.height)
                {
                    return index;
                }
            }
            return -1;
        }

        int HitTestBreadcrumb(float x, float y) const
        {
            if (m_scan_in_progress || !m_tree)
            {
                return -1;
            }

            for (int index = 0; index < static_cast<int>(m_breadcrumbs.size()); ++index)
            {
                if (PointInRect(m_breadcrumbs[static_cast<size_t>(index)].rect, x, y))
                {
                    return index;
                }
            }
            return -1;
        }

        void OnMouseMove(int x, int y)
        {
            TRACKMOUSEEVENT track = {};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = m_hwnd;
            TrackMouseEvent(&track);

            const RenderPoint new_mouse_position = MakeRenderPoint(static_cast<float>(x), static_cast<float>(y));
            m_mouse_position = new_mouse_position;
            const int hovered_breadcrumb = HitTestBreadcrumb(static_cast<float>(x), static_cast<float>(y));
            const int hovered = HitTestTreemap(static_cast<float>(x), static_cast<float>(y));
            const bool hovered_changed = hovered != m_hovered_index || hovered_breadcrumb != m_hovered_breadcrumb_index;
            if (hovered_changed)
            {
                const int old_hovered_index = m_hovered_index;
                const int old_hovered_breadcrumb_index = m_hovered_breadcrumb_index;
                const RenderRect old_tooltip_bounds = m_tooltip_redraw_bounds;
                m_hovered_index = hovered;
                m_hovered_breadcrumb_index = hovered_breadcrumb;
                if (hovered_breadcrumb >= 0)
                {
                    m_tooltip_position = new_mouse_position;
                    m_tooltip_target_position = new_mouse_position;
                    m_tooltip_redraw_bounds = MeasureTooltipBoundsAt(m_tooltip_position);
                    StartTooltipAnimation();
                }
                else if (hovered >= 0)
                {
                    m_tooltip_position = new_mouse_position;
                    m_tooltip_target_position = new_mouse_position;
                    m_tooltip_redraw_bounds = MeasureTooltipBoundsAt(m_tooltip_position);
                    StartTooltipAnimation();
                }
                else
                {
                    m_tooltip_position = MakeRenderPoint(-1.0f, -1.0f);
                    m_tooltip_target_position = MakeRenderPoint(-1.0f, -1.0f);
                    ResetTooltipRedrawBounds();
                    StopTooltipAnimation();
                }
                InvalidateHoverTransition(
                    old_hovered_index,
                    old_hovered_breadcrumb_index,
                    old_tooltip_bounds);
            }
            else
            {
                m_hovered_breadcrumb_index = hovered_breadcrumb;
                if (hovered >= 0)
                {
                    m_tooltip_target_position = new_mouse_position;
                    StartTooltipAnimation();
                }
                else if (hovered_breadcrumb >= 0)
                {
                    m_tooltip_target_position = new_mouse_position;
                    StartTooltipAnimation();
                }
                else
                {
                    StopTooltipAnimation();
                }
            }

            const bool breadcrumb_is_clickable =
                hovered_breadcrumb >= 0 &&
                m_breadcrumbs[static_cast<size_t>(hovered_breadcrumb)].node_id != m_view_root_id;
            SetCursor(LoadCursorW(nullptr, breadcrumb_is_clickable ? IDC_HAND : IDC_ARROW));
        }

        void OnLeftButtonDown(int x, int y)
        {
            SetFocus(m_hwnd);
            const int breadcrumb_index = HitTestBreadcrumb(static_cast<float>(x), static_cast<float>(y));
            if (breadcrumb_index >= 0)
            {
                NavigateToNode(m_breadcrumbs[static_cast<size_t>(breadcrumb_index)].node_id);
                return;
            }

            const int hit_index = HitTestTreemap(static_cast<float>(x), static_cast<float>(y));
            if (hit_index < 0 || !m_tree)
            {
                return;
            }

            const visualization::TreemapRect& rect = m_rects[static_cast<size_t>(hit_index)];
            if (!rect.is_dir || rect.node_id == 0 || rect.node_id == m_view_root_id)
            {
                return;
            }

            const Node* node = m_tree->GetNode(rect.node_id);
            if (!node || !node->is_dir)
            {
                return;
            }

            const size_t clicked_depth = GetAbsoluteNodeDepth(rect.node_id);
            const size_t expanded_ancestor_count = CountExpandedRevealAncestorsExclusive(rect.node_id);
            if (clicked_depth >= static_cast<size_t>(m_max_depth) + expanded_ancestor_count)
            {
                m_expanded_reveal_nodes.insert(rect.node_id);
            }
            m_view_root_id = rect.node_id;
            RebuildAndRedraw();
        }

        void NavigateToNode(uint64_t node_id)
        {
            if (!m_tree || node_id == 0 || node_id == m_view_root_id)
            {
                return;
            }
            if (!m_tree->GetNode(node_id))
            {
                return;
            }
            m_view_root_id = node_id;
            RebuildAndRedraw();
        }

        void NavigateUp()
        {
            if (!CanNavigateUp())
            {
                return;
            }

            const Node* current = m_tree->GetNode(m_view_root_id);
            if (!current || current->parent_id == 0)
            {
                return;
            }

            NavigateToNode(current->parent_id);
        }

        bool CanNavigateUp() const
        {
            return m_tree && m_view_root_id != 0 && m_view_root_id != m_tree->RootId();
        }

        size_t GetAbsoluteNodeDepth(uint64_t node_id) const
        {
            if (!m_tree || node_id == 0)
            {
                return 0;
            }

            const uint64_t root_id = m_tree->RootId();
            size_t depth = 0;
            uint64_t current_id = node_id;
            size_t guard = 0;
            while (current_id != 0 && current_id != root_id && guard < 1024)
            {
                const Node* current = m_tree->GetNode(current_id);
                if (!current || current->parent_id == 0 || current->parent_id == current_id)
                {
                    break;
                }

                ++depth;
                current_id = current->parent_id;
                ++guard;
            }

            return depth;
        }

        size_t CountExpandedRevealAncestorsExclusive(uint64_t node_id) const
        {
            if (!m_tree || node_id == 0)
            {
                return 0;
            }

            size_t count = 0;
            const uint64_t root_id = m_tree->RootId();
            const Node* current = m_tree->GetNode(node_id);
            size_t guard = 0;
            while (current && current->parent_id != 0 && current->parent_id != current->id && guard < 1024)
            {
                const uint64_t parent_id = current->parent_id;
                if (m_expanded_reveal_nodes.find(parent_id) != m_expanded_reveal_nodes.end())
                {
                    ++count;
                }
                if (parent_id == root_id)
                {
                    break;
                }

                current = m_tree->GetNode(parent_id);
                ++guard;
            }

            return count;
        }

        void OnKeyDown(UINT key_code)
        {
            if (key_code == VK_BACK)
            {
                NavigateUp();
            }
            else if (key_code == VK_HOME && m_tree)
            {
                m_view_root_id = m_tree->RootId();
                RebuildAndRedraw();
            }
        }

        void StartTooltipAnimation()
        {
            if (!m_show_tooltip || m_tooltip_animation_active || !m_hwnd)
            {
                return;
            }

            SetTimer(m_hwnd, kTooltipAnimationTimerId, kTooltipAnimationIntervalMs, nullptr);
            m_tooltip_animation_active = true;
        }

        void StopTooltipAnimation()
        {
            if (!m_tooltip_animation_active || !m_hwnd)
            {
                return;
            }

            KillTimer(m_hwnd, kTooltipAnimationTimerId);
            m_tooltip_animation_active = false;
        }

        void OnTooltipAnimationTimer()
        {
            if (!m_show_tooltip)
            {
                StopTooltipAnimation();
                return;
            }

            const bool breadcrumb_hovered =
                m_hovered_breadcrumb_index >= 0 &&
                m_hovered_breadcrumb_index < static_cast<int>(m_breadcrumbs.size());
            const bool rect_hovered =
                m_hovered_index >= 0 &&
                m_hovered_index < static_cast<int>(m_rects.size());
            if (!breadcrumb_hovered && !rect_hovered)
            {
                StopTooltipAnimation();
                return;
            }

            const float delta_x = m_tooltip_target_position.x - m_tooltip_position.x;
            const float delta_y = m_tooltip_target_position.y - m_tooltip_position.y;
            const float distance_squared = (delta_x * delta_x) + (delta_y * delta_y);
            const RenderRect old_tooltip_bounds = m_tooltip_redraw_bounds;
            if (distance_squared <= (kTooltipAnimationSnapDistance * kTooltipAnimationSnapDistance))
            {
                m_tooltip_position = m_tooltip_target_position;
                StopTooltipAnimation();
                InvalidateTooltipTransition(old_tooltip_bounds, m_tooltip_position);
                return;
            }

            m_tooltip_position.x += delta_x * kTooltipAnimationSmoothing;
            m_tooltip_position.y += delta_y * kTooltipAnimationSmoothing;
            InvalidateTooltipTransition(old_tooltip_bounds, m_tooltip_position);
        }

        MainWindowDrawingState BuildDrawingState() const
        {
            MainWindowDrawingState state;
            state.palette = GetRenderPalette();
            state.scan_in_progress = m_scan_in_progress;
            state.has_error = m_has_error;
            state.requested_path_is_ntfs = m_requested_path_is_ntfs;
            state.used_mft_scan = m_used_mft_scan;
            state.show_tooltip = m_show_tooltip;
            state.show_status_bar = m_show_status_bar;
            state.hovered_index = m_hovered_index;
            state.hovered_breadcrumb_index = m_hovered_breadcrumb_index;
            state.elapsed_seconds = m_elapsed_seconds;
            state.view_root_id = m_view_root_id;
            state.selected_path = m_options.path;
            state.status_text = m_status_text;
            state.error_text = m_error_text;
            state.tree = m_tree.get();
            state.layout_options = m_layout_options;
            state.rects = &m_rects;
            state.breadcrumbs = &m_breadcrumbs;
            state.tooltip_position = m_tooltip_position;
            state.canvas_rect = m_canvas_rect;
            state.status_bar_rect = m_status_bar_rect;
            state.treemap_content_rect = m_treemap_content_rect;
            state.focus_up_rect = m_focus_up_rect;
            state.header_info_rect = m_header_info_rect;
            state.scan_group_rect = m_scan_group_rect;
            state.display_group_rect = m_display_group_rect;
            state.checkbox_depth_separator_x = m_checkbox_depth_separator_x;
            state.depth_reset_separator_x = m_depth_reset_separator_x;
            return state;
        }

        RenderVisualPalette GetRenderPalette() const
        {
            return RenderVisualPalette{
                kWindowBackgroundRed,
                kWindowBackgroundGreen,
                kWindowBackgroundBlue,
                kHeaderPanelRed,
                kHeaderPanelGreen,
                kHeaderPanelBlue,
                kControlPanelRed,
                kControlPanelGreen,
                kControlPanelBlue,
                kControlSurfaceRed,
                kControlSurfaceGreen,
                kControlSurfaceBlue,
                kControlMutedRed,
                kControlMutedGreen,
                kControlMutedBlue,
                kControlBorderRed,
                kControlBorderGreen,
                kControlBorderBlue
            };
        }

        void OnPaint()
        {
            PAINTSTRUCT paint = {};
            BeginPaint(m_hwnd, &paint);
            UpdateCanvasRect();
            MainWindowDrawingState state = BuildDrawingState();
            state.paint_rect = RectToRenderRect(paint.rcPaint);
            m_painter.Paint(m_hwnd, state);
            if (m_show_tooltip && (m_hovered_index >= 0 || m_hovered_breadcrumb_index >= 0))
            {
                m_tooltip_redraw_bounds = m_painter.GetHoverTooltipBounds(m_hwnd, state);
            }
            else
            {
                ResetTooltipRedrawBounds();
            }
            EndPaint(m_hwnd, &paint);
        }

        GuiOptions m_options;
        HWND m_hwnd = nullptr;
        HWND m_path_edit = nullptr;
        HWND m_scan_button = nullptr;
        HWND m_browse_button = nullptr;
        HWND m_reset_button = nullptr;
        HWND m_depth_label = nullptr;
        HWND m_depth_edit = nullptr;
        HWND m_depth_up_button = nullptr;
        HWND m_depth_down_button = nullptr;
        HWND m_show_tooltip_check = nullptr;
        HWND m_show_status_bar_check = nullptr;
        HBRUSH m_header_background_brush = nullptr;
        HBRUSH m_control_panel_brush = nullptr;
        HBRUSH m_control_surface_brush = nullptr;
        HBRUSH m_control_hover_brush = nullptr;
        std::shared_ptr<SharedScanState> m_shared_scan_state;
        bool m_requested_path_is_ntfs = false;
        MainWindowPainter m_painter;

        bool m_scan_in_progress = false;
        bool m_has_error = false;
        bool m_used_mft_scan = false;
        HWND m_hovered_control = nullptr;
        int m_hovered_index = -1;
        int m_hovered_breadcrumb_index = -1;
        int m_max_depth = kDefaultMaxDepth;
        double m_elapsed_seconds = 0.0;
        uint64_t m_view_root_id = 0;
        std::wstring m_scan_path;
        std::wstring m_status_text;
        std::wstring m_error_text;
        FileTree::TreeStats m_tree_stats;
        std::unique_ptr<FileTree> m_tree;
        visualization::TreemapLayoutOptions m_layout_options;
        std::vector<visualization::TreemapRect> m_rects;
        std::unordered_set<uint64_t> m_expanded_reveal_nodes;
        std::vector<BreadcrumbStrip> m_breadcrumbs;
        bool m_show_tooltip = true;
        bool m_show_status_bar = false;
        bool m_updating_depth_edit = false;
        bool m_tooltip_animation_active = false;
        RenderPoint m_mouse_position = MakeRenderPoint(-1.0f, -1.0f);
        RenderPoint m_tooltip_position = MakeRenderPoint(-1.0f, -1.0f);
        RenderPoint m_tooltip_target_position = MakeRenderPoint(-1.0f, -1.0f);
        RenderRect m_tooltip_redraw_bounds = MakeRenderRect(0, 0, 0, 0);
        RenderRect m_canvas_rect = MakeRenderRect(0, 0, 0, 0);
        RenderRect m_status_bar_rect = MakeRenderRect(0, 0, 0, 0);
        RenderRect m_treemap_content_rect = MakeRenderRect(0, 0, 0, 0);
        RenderRect m_focus_up_rect = MakeRenderRect(0, 0, 0, 0);
        RenderRect m_header_info_rect = MakeRenderRect(0, 0, 0, 0);
        RenderRect m_scan_group_rect = MakeRenderRect(0, 0, 0, 0);
        RenderRect m_depth_group_rect = MakeRenderRect(0, 0, 0, 0);
        RenderRect m_display_group_rect = MakeRenderRect(0, 0, 0, 0);
        float m_checkbox_depth_separator_x = 0.0f;
        float m_depth_reset_separator_x = 0.0f;
        std::vector<std::pair<HWND, WNDPROC>> m_subclassed_controls;
    };

} // namespace space_scanner::gui

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command)
{
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    space_scanner::gui::GuiOptions options = space_scanner::gui::ParseOptions();
    if (options.show_help)
    {
        MessageBoxW(
            nullptr,
            L"SpaceScanner GUI\n\nUsage:\n  space_scanner_gui.exe --path C:\\\n",
            L"SpaceScanner",
            MB_OK | MB_ICONINFORMATION);
        CoUninitialize();
        return 0;
    }

    space_scanner::gui::MainWindow window(std::move(options));
    if (!window.Create(instance, show_command))
    {
        CoUninitialize();
        return 1;
    }

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    CoUninitialize();
    return static_cast<int>(message.wParam);
}
