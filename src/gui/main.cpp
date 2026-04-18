#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <d2d1.h>
#include <dwrite.h>
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

    std::wstring FormatSeconds(double seconds)
    {
        wchar_t buffer[64] = {};
        swprintf_s(buffer, L"%.2f s", seconds);
        return buffer;
    }

    D2D1_COLOR_F ColorFromRgb(BYTE red, BYTE green, BYTE blue, float alpha = 1.0f)
    {
        return D2D1::ColorF(
            static_cast<float>(red) / 255.0f,
            static_cast<float>(green) / 255.0f,
            static_cast<float>(blue) / 255.0f,
            alpha);
    }

    D2D1_COLOR_F ParseHexColor(const std::string& value, float alpha = 1.0f)
    {
        if (value.size() != 7 || value[0] != '#')
        {
            return ColorFromRgb(120, 150, 190, alpha);
        }

        auto hex = [](char ch) -> uint8_t
            {
                if (ch >= '0' && ch <= '9')
                {
                    return static_cast<uint8_t>(ch - '0');
                }
                if (ch >= 'a' && ch <= 'f')
                {
                    return static_cast<uint8_t>(10 + ch - 'a');
                }
                if (ch >= 'A' && ch <= 'F')
                {
                    return static_cast<uint8_t>(10 + ch - 'A');
                }
                return 0;
            };

        const BYTE red = static_cast<BYTE>((hex(value[1]) << 4) | hex(value[2]));
        const BYTE green = static_cast<BYTE>((hex(value[3]) << 4) | hex(value[4]));
        const BYTE blue = static_cast<BYTE>((hex(value[5]) << 4) | hex(value[6]));
        return ColorFromRgb(red, green, blue, alpha);
    }

    bool PointInRect(const D2D1_RECT_F& rect, float x, float y)
    {
        return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
    }

    RECT RectFToRect(const D2D1_RECT_F& rect)
    {
        RECT result = {};
        result.left = static_cast<LONG>(std::floor(rect.left));
        result.top = static_cast<LONG>(std::floor(rect.top));
        result.right = static_cast<LONG>(std::ceil(rect.right));
        result.bottom = static_cast<LONG>(std::ceil(rect.bottom));
        return result;
    }

    struct BreadcrumbStrip
    {
        uint64_t node_id = 0;
        D2D1_RECT_F rect = D2D1::RectF(0, 0, 0, 0);
        D2D1_RECT_F frame_rect = D2D1::RectF(0, 0, 0, 0);
        D2D1_RECT_F content_rect = D2D1::RectF(0, 0, 0, 0);
        std::wstring text;
        visualization::TreemapVisualStyle style;
    };

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
        static constexpr float kHeaderHeight = 124.0f;
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
        static constexpr float kTooltipAnimationSmoothing = 0.38f;
        static constexpr float kTooltipAnimationSnapDistance = 1.5f;
        static constexpr size_t kMaxBreadcrumbStrips = 5;

        LRESULT HandleMessage(UINT message, WPARAM w_param, LPARAM l_param)
        {
            switch (message)
            {
            case WM_CREATE:
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
                    m_hovered_index = -1;
                    m_hovered_breadcrumb_index = -1;
                    m_mouse_position = D2D1::Point2F(-1.0f, -1.0f);
                    m_tooltip_position = D2D1::Point2F(-1.0f, -1.0f);
                    m_tooltip_target_position = D2D1::Point2F(-1.0f, -1.0f);
                    StopTooltipAnimation();
                    InvalidateCanvas();
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

        void CreateHeaderControls()
        {
            HFONT gui_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            const std::wstring initial_path = m_options.path.empty() ? GetDefaultPathFieldValue() : m_options.path;

            m_path_edit = CreateWindowExW(
                WS_EX_CLIENTEDGE,
                L"EDIT",
                initial_path.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
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
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
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
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0,
                0,
                0,
                0,
                m_hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BROWSE_BUTTON)),
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
                WS_EX_CLIENTEDGE,
                L"EDIT",
                depth_text.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_CENTER,
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
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
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
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
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
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
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
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
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
                SendMessageW(
                    m_show_tooltip_check,
                    BM_SETCHECK,
                    m_show_tooltip ? BST_CHECKED : BST_UNCHECKED,
                    0);
            }
            if (m_show_status_bar_check)
            {
                SendMessageW(
                    m_show_status_bar_check,
                    WM_SETFONT,
                    reinterpret_cast<WPARAM>(gui_font),
                    TRUE);
                SendMessageW(
                    m_show_status_bar_check,
                    BM_SETCHECK,
                    m_show_status_bar ? BST_CHECKED : BST_UNCHECKED,
                    0);
            }

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

            const int edit_left = 360;
            const int top = 22;
            const int second_row_top = 54;
            const int browse_width = 92;
            const int scan_width = 72;
            const int depth_label_width = 42;
            const int depth_edit_width = 40;
            const int depth_button_width = 18;
            const int depth_button_gap = 4;
            const int show_status_bar_width = 124;
            const int show_tooltip_width = 102;
            const int gap = 8;
            const int right_padding = static_cast<int>(kOuterPadding);
            const int browse_left = width - right_padding - browse_width;
            const int scan_left = browse_left - gap - scan_width;
            const int edit_width = std::max(160, scan_left - gap - edit_left);
            const int depth_button_left = browse_left + browse_width - depth_button_width;
            const int depth_edit_left = depth_button_left - depth_button_gap - depth_edit_width;
            const int depth_label_left = depth_edit_left - gap - depth_label_width;
            const int show_status_bar_left = depth_label_left - gap - show_status_bar_width;
            const int show_tooltip_left = show_status_bar_left - gap - show_tooltip_width;

            if (m_path_edit)
            {
                MoveWindow(m_path_edit, edit_left, top, edit_width, 26, TRUE);
            }
            if (m_scan_button)
            {
                MoveWindow(m_scan_button, scan_left, top, scan_width, 26, TRUE);
            }
            if (m_browse_button)
            {
                MoveWindow(m_browse_button, browse_left, top, browse_width, 26, TRUE);
            }
            if (m_depth_label)
            {
                MoveWindow(m_depth_label, depth_label_left, second_row_top + 4, depth_label_width, 18, TRUE);
            }
            if (m_depth_edit)
            {
                MoveWindow(m_depth_edit, depth_edit_left, second_row_top, depth_edit_width, 26, TRUE);
            }
            if (m_depth_up_button)
            {
                MoveWindow(m_depth_up_button, depth_button_left, second_row_top, depth_button_width, 12, TRUE);
            }
            if (m_depth_down_button)
            {
                MoveWindow(m_depth_down_button, depth_button_left, second_row_top + 13, depth_button_width, 13, TRUE);
            }
            if (m_show_tooltip_check)
            {
                MoveWindow(m_show_tooltip_check, show_tooltip_left, second_row_top + 2, show_tooltip_width, 22, TRUE);
            }
            if (m_show_status_bar_check)
            {
                MoveWindow(m_show_status_bar_check, show_status_bar_left, second_row_top + 2, show_status_bar_width, 22, TRUE);
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
                RebuildLayout();
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
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
            SetWindowTextW(m_depth_edit, depth_text.c_str());
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
            if (buffer[0] == L'\0')
            {
                return false;
            }

            wchar_t* end = nullptr;
            const long parsed = wcstol(buffer, &end, 10);
            if (end == buffer || (end && *end != L'\0'))
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
                m_show_tooltip =
                    SendMessageW(m_show_tooltip_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
                if (!m_show_tooltip)
                {
                    StopTooltipAnimation();
                }
                else if (m_hovered_index >= 0 || m_hovered_breadcrumb_index >= 0)
                {
                    m_tooltip_position = m_mouse_position;
                    m_tooltip_target_position = m_mouse_position;
                    StartTooltipAnimation();
                }
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return;
            }

            if (control_id == IDC_SHOW_STATUS_BAR_CHECK && control == m_show_status_bar_check && notification_code == BN_CLICKED)
            {
                m_show_status_bar =
                    SendMessageW(m_show_status_bar_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
                RebuildLayout();
                InvalidateCanvas();
                return;
            }

            if (control_id == IDC_SCAN_BUTTON && control == m_scan_button)
            {
                BeginScanFromUiPath();
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
            m_hovered_index = -1;
            m_rects.clear();
            m_hovered_breadcrumb_index = -1;
            SetScanControlsEnabled(true);

            if (!payload)
            {
                m_has_error = true;
                m_status_text = L"Scan completed without payload.";
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
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return;
            }

            m_tree = std::move(payload->tree);
            m_tree_stats = m_tree->ComputeStats();
            m_used_mft_scan = payload->used_mft_scan;
            m_view_root_id = m_tree->RootId();
            m_expanded_reveal_nodes.clear();
            m_has_error = false;
            m_error_text.clear();
            m_status_text = L"Ready";
            RebuildLayout();
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }

        HRESULT CreateDeviceResources()
        {
            if (!m_d2d_factory)
            {
                HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2d_factory.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }
            }

            if (!m_dwrite_factory)
            {
                HRESULT hr = DWriteCreateFactory(
                    DWRITE_FACTORY_TYPE_SHARED,
                    __uuidof(IDWriteFactory),
                    reinterpret_cast<IUnknown**>(m_dwrite_factory.GetAddressOf()));
                if (FAILED(hr))
                {
                    return hr;
                }
            }

            if (!m_render_target)
            {
                RECT client = {};
                GetClientRect(m_hwnd, &client);
                const D2D1_SIZE_U size = D2D1::SizeU(
                    static_cast<UINT>(std::max<LONG>(client.right - client.left, 1)),
                    static_cast<UINT>(std::max<LONG>(client.bottom - client.top, 1)));

                HRESULT hr = m_d2d_factory->CreateHwndRenderTarget(
                    D2D1::RenderTargetProperties(),
                    D2D1::HwndRenderTargetProperties(m_hwnd, size),
                    m_render_target.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }

                hr = m_render_target->CreateSolidColorBrush(ColorFromRgb(232, 241, 255), m_text_brush.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }
                hr = m_render_target->CreateSolidColorBrush(ColorFromRgb(150, 171, 204), m_muted_brush.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }
                hr = m_render_target->CreateSolidColorBrush(ColorFromRgb(21, 33, 53, 0.94f), m_panel_brush.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }
                hr = m_render_target->CreateSolidColorBrush(ColorFromRgb(10, 18, 32, 0.97f), m_canvas_brush.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }
                hr = m_render_target->CreateSolidColorBrush(ColorFromRgb(72, 94, 134, 0.55f), m_border_brush.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }
                hr = m_render_target->CreateSolidColorBrush(ColorFromRgb(120, 160, 215), m_tile_brush.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }
                hr = m_render_target->CreateSolidColorBrush(ColorFromRgb(232, 241, 255), m_tile_text_brush.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }
                hr = m_render_target->CreateSolidColorBrush(ColorFromRgb(150, 171, 204), m_tile_secondary_text_brush.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }
                hr = m_render_target->CreateSolidColorBrush(ColorFromRgb(8, 17, 29, 0.72f), m_tile_text_outline_brush.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }
                hr = m_render_target->CreateSolidColorBrush(ColorFromRgb(8, 17, 29, 0.36f), m_tile_text_plate_brush.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }
                hr = m_render_target->CreateSolidColorBrush(ColorFromRgb(250, 252, 255, 0.95f), m_hover_brush.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }
                hr = m_render_target->CreateSolidColorBrush(ColorFromRgb(150, 205, 255, 0.24f), m_active_level_glow_brush.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }
                hr = m_render_target->CreateSolidColorBrush(ColorFromRgb(6, 10, 18, 0.48f), m_active_level_shadow_brush.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }
                hr = m_render_target->CreateSolidColorBrush(ColorFromRgb(255, 140, 140), m_error_brush.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }
                hr = m_render_target->CreateSolidColorBrush(ColorFromRgb(90, 185, 255, 0.16f), m_glow_a_brush.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }
                hr = m_render_target->CreateSolidColorBrush(ColorFromRgb(96, 244, 197, 0.12f), m_glow_b_brush.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }
                hr = m_render_target->CreateSolidColorBrush(ColorFromRgb(18, 31, 49, 0.78f), m_header_action_brush.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }
                hr = m_render_target->CreateSolidColorBrush(ColorFromRgb(130, 184, 255, 0.22f), m_header_action_hover_brush.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }
                hr = m_render_target->CreateSolidColorBrush(ColorFromRgb(255, 255, 255, 0.98f), m_tooltip_background_brush.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }
                hr = m_render_target->CreateSolidColorBrush(ColorFromRgb(166, 172, 181, 0.95f), m_tooltip_border_brush.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }
                hr = m_render_target->CreateSolidColorBrush(ColorFromRgb(42, 42, 42, 0.96f), m_tooltip_text_brush.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }
                hr = m_render_target->CreateSolidColorBrush(ColorFromRgb(86, 86, 86, 0.96f), m_tooltip_secondary_text_brush.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }
                hr = m_render_target->CreateSolidColorBrush(ColorFromRgb(0, 0, 0, 0.18f), m_tooltip_shadow_brush.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }

                hr = m_dwrite_factory->CreateTextFormat(
                    L"Segoe UI Variable Display",
                    nullptr,
                    DWRITE_FONT_WEIGHT_SEMI_BOLD,
                    DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL,
                    28.0f,
                    L"",
                    m_title_format.GetAddressOf());
                if (FAILED(hr))
                {
                    hr = m_dwrite_factory->CreateTextFormat(
                        L"Segoe UI",
                        nullptr,
                        DWRITE_FONT_WEIGHT_SEMI_BOLD,
                        DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL,
                        28.0f,
                        L"",
                        m_title_format.GetAddressOf());
                    if (FAILED(hr))
                    {
                        return hr;
                    }
                }

                hr = m_dwrite_factory->CreateTextFormat(
                    L"Segoe UI",
                    nullptr,
                    DWRITE_FONT_WEIGHT_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL,
                    14.0f,
                    L"",
                    m_body_format.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }

                hr = m_dwrite_factory->CreateTextFormat(
                    L"Segoe UI",
                    nullptr,
                    DWRITE_FONT_WEIGHT_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL,
                    12.0f,
                    L"",
                    m_small_format.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }

                m_body_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                m_small_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

                hr = m_dwrite_factory->CreateEllipsisTrimmingSign(m_small_format.Get(), m_ellipsis_trimming_sign.GetAddressOf());
                if (FAILED(hr))
                {
                    return hr;
                }
            }

            return S_OK;
        }

        void DiscardDeviceResources()
        {
            m_render_target.Reset();
            m_text_brush.Reset();
            m_muted_brush.Reset();
            m_panel_brush.Reset();
            m_canvas_brush.Reset();
            m_border_brush.Reset();
            m_tile_brush.Reset();
            m_tile_text_brush.Reset();
            m_tile_secondary_text_brush.Reset();
            m_tile_text_outline_brush.Reset();
            m_tile_text_plate_brush.Reset();
            m_hover_brush.Reset();
            m_active_level_glow_brush.Reset();
            m_active_level_shadow_brush.Reset();
            m_error_brush.Reset();
            m_glow_a_brush.Reset();
            m_glow_b_brush.Reset();
            m_header_action_brush.Reset();
            m_header_action_hover_brush.Reset();
            m_tooltip_background_brush.Reset();
            m_tooltip_border_brush.Reset();
            m_tooltip_text_brush.Reset();
            m_tooltip_secondary_text_brush.Reset();
            m_tooltip_shadow_brush.Reset();
            m_title_format.Reset();
            m_body_format.Reset();
            m_small_format.Reset();
            m_ellipsis_trimming_sign.Reset();
        }

        void OnSize()
        {
            UpdateHeaderControlsLayout();
            if (m_render_target)
            {
                RECT client = {};
                GetClientRect(m_hwnd, &client);
                const D2D1_SIZE_U size = D2D1::SizeU(
                    static_cast<UINT>(std::max<LONG>(client.right - client.left, 1)),
                    static_cast<UINT>(std::max<LONG>(client.bottom - client.top, 1)));
                m_render_target->Resize(size);
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

        void UpdateCanvasRect()
        {
            RECT client = {};
            GetClientRect(m_hwnd, &client);
            const float width = static_cast<float>(std::max<LONG>(client.right - client.left, 1));
            const float height = static_cast<float>(std::max<LONG>(client.bottom - client.top, 1));
            const float status_bar_top = std::max(
                kOuterPadding + kHeaderHeight + 1.0f,
                height - kStatusBarBottomPadding - kStatusBarHeight);

            if (m_show_status_bar)
            {
                m_status_bar_rect = D2D1::RectF(
                    kOuterPadding,
                    status_bar_top,
                    std::max(width - kOuterPadding, kOuterPadding + 1.0f),
                    std::max(status_bar_top + kStatusBarHeight, status_bar_top + 1.0f));
            }
            else
            {
                m_status_bar_rect = D2D1::RectF(0, 0, 0, 0);
            }

            m_canvas_rect = D2D1::RectF(
                kOuterPadding,
                kOuterPadding + kHeaderHeight,
                std::max(width - kOuterPadding, kOuterPadding + 1.0f),
                std::max(
                    (m_show_status_bar ? status_bar_top - kStatusBarGap : height - kOuterPadding),
                    kOuterPadding + kHeaderHeight + 1.0f));
        }

        void RebuildLayout()
        {
            UpdateCanvasRect();
            m_focus_up_rect = D2D1::RectF(0, 0, 0, 0);
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

            D2D1_RECT_F nested_rect = D2D1::RectF(
                m_canvas_rect.left + 1.0f,
                m_canvas_rect.top + 1.0f,
                m_canvas_rect.right - 1.0f,
                m_canvas_rect.bottom - 1.0f);

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
                strip.rect = D2D1::RectF(
                    nested_rect.left,
                    nested_rect.top,
                    nested_rect.right,
                    std::min(nested_rect.bottom, nested_rect.top + kTreemapTopSliceHeight));
                strip.content_rect = D2D1::RectF(
                    nested_rect.left + kTreemapSideSliceWidth,
                    strip.rect.bottom,
                    nested_rect.right - kTreemapSideSliceWidth,
                    nested_rect.bottom - kTreemapBottomSliceHeight);

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

            const D2D1_POINT_2F new_mouse_position = D2D1::Point2F(static_cast<float>(x), static_cast<float>(y));
            m_mouse_position = new_mouse_position;
            const int hovered_breadcrumb = HitTestBreadcrumb(static_cast<float>(x), static_cast<float>(y));
            const int hovered = HitTestTreemap(static_cast<float>(x), static_cast<float>(y));
            const bool hovered_changed = hovered != m_hovered_index || hovered_breadcrumb != m_hovered_breadcrumb_index;
            if (hovered_changed)
            {
                m_hovered_index = hovered;
                m_hovered_breadcrumb_index = hovered_breadcrumb;
                if (hovered_breadcrumb >= 0)
                {
                    m_tooltip_position = new_mouse_position;
                    m_tooltip_target_position = new_mouse_position;
                    StartTooltipAnimation();
                }
                else if (hovered >= 0)
                {
                    m_tooltip_position = new_mouse_position;
                    m_tooltip_target_position = new_mouse_position;
                    const visualization::TreemapRect& rect = m_rects[static_cast<size_t>(hovered)];
                    if (ShouldFollowTooltip(rect))
                    {
                        StartTooltipAnimation();
                    }
                    else
                    {
                        StopTooltipAnimation();
                    }
                }
                else
                {
                    m_tooltip_position = D2D1::Point2F(-1.0f, -1.0f);
                    m_tooltip_target_position = D2D1::Point2F(-1.0f, -1.0f);
                    StopTooltipAnimation();
                }
                InvalidateCanvas();
            }
            else
            {
                m_hovered_breadcrumb_index = hovered_breadcrumb;
                if (hovered >= 0)
                {
                    const visualization::TreemapRect& rect = m_rects[static_cast<size_t>(hovered)];
                    if (hovered_breadcrumb >= 0)
                    {
                        m_tooltip_target_position = new_mouse_position;
                        StartTooltipAnimation();
                    }
                    else if (ShouldFollowTooltip(rect))
                    {
                        m_tooltip_target_position = new_mouse_position;
                        StartTooltipAnimation();
                    }
                    else
                    {
                        StopTooltipAnimation();
                    }
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
            RebuildLayout();
            InvalidateRect(m_hwnd, nullptr, FALSE);
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
            RebuildLayout();
            InvalidateRect(m_hwnd, nullptr, FALSE);
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
                RebuildLayout();
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
        }

        void DrawTextLine(
            const std::wstring& text,
            IDWriteTextFormat* format,
            const D2D1_RECT_F& bounds,
            ID2D1Brush* brush)
        {
            if (!format || !brush)
            {
                return;
            }
            m_render_target->DrawTextW(
                text.c_str(),
                static_cast<UINT32>(text.size()),
                format,
                bounds,
                brush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        void DrawTileTextLayout(
            const std::wstring& text,
            IDWriteTextFormat* format,
            const D2D1_RECT_F& bounds,
            ID2D1Brush* text_brush,
            ID2D1Brush* outline_brush,
            DWRITE_TEXT_ALIGNMENT text_alignment = DWRITE_TEXT_ALIGNMENT_LEADING,
            DWRITE_PARAGRAPH_ALIGNMENT paragraph_alignment = DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
            DWRITE_WORD_WRAPPING word_wrapping = DWRITE_WORD_WRAPPING_NO_WRAP)
        {
            if (!format || !text_brush || !outline_brush || !m_dwrite_factory)
            {
                return;
            }

            ComPtr<IDWriteTextLayout> layout;
            const float width = std::max(bounds.right - bounds.left, 1.0f);
            const float height = std::max(bounds.bottom - bounds.top, 1.0f);
            HRESULT hr = m_dwrite_factory->CreateTextLayout(
                text.c_str(),
                static_cast<UINT32>(text.size()),
                format,
                width,
                height,
                layout.GetAddressOf());
            if (FAILED(hr) || !layout)
            {
                return;
            }

            DWRITE_TRIMMING trimming = {};
            trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
            layout->SetWordWrapping(word_wrapping);
            layout->SetTextAlignment(text_alignment);
            layout->SetParagraphAlignment(paragraph_alignment);
            if (word_wrapping == DWRITE_WORD_WRAPPING_NO_WRAP)
            {
                layout->SetTrimming(&trimming, m_ellipsis_trimming_sign.Get());
            }

            const D2D1_POINT_2F shadow_origin = D2D1::Point2F(bounds.left + 1.0f, bounds.top + 1.0f);
            const D2D1_POINT_2F origin = D2D1::Point2F(bounds.left, bounds.top);
            m_render_target->DrawTextLayout(shadow_origin, layout.Get(), outline_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            m_render_target->DrawTextLayout(origin, layout.Get(), text_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        void DrawTileTextLine(
            const std::wstring& text,
            IDWriteTextFormat* format,
            const D2D1_RECT_F& bounds,
            ID2D1Brush* text_brush,
            ID2D1Brush* outline_brush,
            DWRITE_TEXT_ALIGNMENT text_alignment = DWRITE_TEXT_ALIGNMENT_LEADING)
        {
            DrawTileTextLayout(
                text,
                format,
                bounds,
                text_brush,
                outline_brush,
                text_alignment,
                DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
                DWRITE_WORD_WRAPPING_NO_WRAP);
        }

        float MeasureTextWidth(
            const std::wstring& text,
            IDWriteTextFormat* format) const
        {
            if (text.empty() || !format || !m_dwrite_factory)
            {
                return 0.0f;
            }

            ComPtr<IDWriteTextLayout> layout;
            HRESULT hr = m_dwrite_factory->CreateTextLayout(
                text.c_str(),
                static_cast<UINT32>(text.size()),
                format,
                4096.0f,
                64.0f,
                layout.GetAddressOf());
            if (FAILED(hr) || !layout)
            {
                return 0.0f;
            }

            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TEXT_METRICS metrics = {};
            if (FAILED(layout->GetMetrics(&metrics)))
            {
                return 0.0f;
            }

            return metrics.widthIncludingTrailingWhitespace;
        }

        std::wstring BuildDirectoryHeaderText(
            const std::wstring& label,
            uint64_t size,
            float available_width) const
        {
            const std::wstring size_suffix = L" - " + HumanizeBytes(size);
            const std::wstring full_text = label + size_suffix;
            if (MeasureTextWidth(full_text, m_small_format.Get()) <= available_width)
            {
                return full_text;
            }

            constexpr size_t kMinimumReservedNameCharacters = 5;
            const float suffix_width = MeasureTextWidth(size_suffix, m_small_format.Get());
            if (suffix_width >= available_width)
            {
                return label;
            }

            size_t low = 0;
            size_t high = label.size();
            size_t best = 0;
            while (low <= high)
            {
                const size_t mid = low + ((high - low) / 2);
                const std::wstring candidate = label.substr(0, mid) + L"..." + size_suffix;
                if (MeasureTextWidth(candidate, m_small_format.Get()) <= available_width)
                {
                    best = mid;
                    low = mid + 1;
                }
                else
                {
                    if (mid == 0)
                    {
                        break;
                    }
                    high = mid - 1;
                }
            }

            if (best < kMinimumReservedNameCharacters)
            {
                return label;
            }

            return label.substr(0, best) + L"..." + size_suffix;
        }

        void DrawTreemap()
        {
            const D2D1_ROUNDED_RECT canvas_panel = D2D1::RoundedRect(m_canvas_rect, 16.0f, 16.0f);
            m_render_target->FillRoundedRectangle(canvas_panel, m_canvas_brush.Get());
            m_render_target->DrawRoundedRectangle(canvas_panel, m_border_brush.Get(), 1.2f);

            if (m_scan_in_progress)
            {
                DrawTextLine(
                    L"Scanning in progress",
                    m_title_format.Get(),
                    D2D1::RectF(m_canvas_rect.left + 24.0f, m_canvas_rect.top + 28.0f, m_canvas_rect.right - 24.0f, m_canvas_rect.top + 70.0f),
                    m_text_brush.Get());
                DrawTextLine(
                    m_status_text,
                    m_body_format.Get(),
                    D2D1::RectF(m_canvas_rect.left + 24.0f, m_canvas_rect.top + 76.0f, m_canvas_rect.right - 24.0f, m_canvas_rect.top + 100.0f),
                    m_muted_brush.Get());
                return;
            }

            if (m_has_error)
            {
                DrawTextLine(
                    L"Scan failed",
                    m_title_format.Get(),
                    D2D1::RectF(m_canvas_rect.left + 24.0f, m_canvas_rect.top + 28.0f, m_canvas_rect.right - 24.0f, m_canvas_rect.top + 70.0f),
                    m_error_brush.Get());
                DrawTextLine(
                    m_error_text,
                    m_body_format.Get(),
                    D2D1::RectF(m_canvas_rect.left + 24.0f, m_canvas_rect.top + 76.0f, m_canvas_rect.right - 24.0f, m_canvas_rect.bottom - 24.0f),
                    m_muted_brush.Get());
                return;
            }

            if (!m_tree)
            {
                DrawTextLine(
                    L"No scan data yet, specify the path by browsing above.",
                    m_title_format.Get(),
                    D2D1::RectF(m_canvas_rect.left + 24.0f, m_canvas_rect.top + 28.0f, m_canvas_rect.right - 24.0f, m_canvas_rect.top + 70.0f),
                    m_text_brush.Get());
                return;
            }

            const Node* focused_node = m_tree->GetNode(m_view_root_id);
            const std::wstring focused_path = focused_node
                ? m_tree->BuildPath(m_view_root_id)
                : (m_options.path.empty() ? L"(no path selected)" : m_options.path);
            const std::wstring focused_name = focused_node
                ? (focused_node->name.empty() ? focused_path : focused_node->name)
                : (m_options.path.empty() ? L"(no path selected)" : m_options.path);
            const std::wstring focused_size = focused_node ? HumanizeBytes(focused_node->size) : L"-";

        const visualization::TreemapVisualStyle frame_style = visualization::BuildTreemapVisualStyle(
            focused_name,
            focused_path,
            true);
            if (m_breadcrumbs.empty())
            {
                const D2D1_RECT_F shadow_rect = D2D1::RectF(
                    m_focus_up_rect.left + 8.0f,
                    m_focus_up_rect.top + 8.0f,
                    m_focus_up_rect.right + 8.0f,
                    m_focus_up_rect.bottom + 8.0f);
                m_render_target->FillRectangle(shadow_rect, m_active_level_shadow_brush.Get());
                const D2D1_RECT_F glow_rect = D2D1::RectF(
                    m_focus_up_rect.left - 2.0f,
                    m_focus_up_rect.top - 2.0f,
                    m_focus_up_rect.right + 2.0f,
                    m_focus_up_rect.bottom + 2.0f);
                m_render_target->DrawRectangle(glow_rect, m_active_level_glow_brush.Get(), 5.5f);
                m_header_action_brush->SetColor(ParseHexColor(frame_style.fill_color));
                m_render_target->FillRectangle(m_focus_up_rect, m_header_action_brush.Get());
                m_render_target->DrawRectangle(m_focus_up_rect, m_hover_brush.Get(), 2.8f);
                m_tile_text_brush->SetColor(ParseHexColor(frame_style.label_text_color));
                m_tile_text_outline_brush->SetColor(ParseHexColor(frame_style.text_outline_color, 0.78f));
                std::wstring frame_text = focused_name + L" - " + focused_size;
                DrawTileTextLine(
                    frame_text,
                    m_small_format.Get(),
                    D2D1::RectF(m_focus_up_rect.left + 8.0f, m_focus_up_rect.top + 3.0f, m_focus_up_rect.right - 8.0f, m_focus_up_rect.bottom - 3.0f),
                    m_tile_text_brush.Get(),
                    m_tile_text_outline_brush.Get());
            }
            else
            {
                for (size_t index = 0; index < m_breadcrumbs.size(); ++index)
                {
                    const BreadcrumbStrip& strip = m_breadcrumbs[index];
                    const bool is_hovered = static_cast<int>(index) == m_hovered_breadcrumb_index;
                    const bool is_active_level = strip.node_id == m_view_root_id;

                    const D2D1_RECT_F left_slice_rect = D2D1::RectF(
                        strip.frame_rect.left,
                        strip.rect.bottom,
                        strip.content_rect.left,
                        strip.frame_rect.bottom - kTreemapBottomSliceHeight);
                    const D2D1_RECT_F right_slice_rect = D2D1::RectF(
                        strip.content_rect.right,
                        strip.rect.bottom,
                        strip.frame_rect.right,
                        strip.frame_rect.bottom - kTreemapBottomSliceHeight);
                    const D2D1_RECT_F bottom_slice_rect = D2D1::RectF(
                        strip.frame_rect.left,
                        strip.frame_rect.bottom - kTreemapBottomSliceHeight,
                        strip.frame_rect.right,
                        strip.frame_rect.bottom);

                    if (is_active_level)
                    {
                        const D2D1_RECT_F shadow_rect = D2D1::RectF(
                            strip.frame_rect.left + 8.0f,
                            strip.frame_rect.top + 8.0f,
                            strip.frame_rect.right + 8.0f,
                            strip.frame_rect.bottom + 8.0f);
                        m_render_target->FillRectangle(shadow_rect, m_active_level_shadow_brush.Get());
                        const D2D1_RECT_F glow_rect = D2D1::RectF(
                            strip.frame_rect.left - 2.0f,
                            strip.frame_rect.top - 2.0f,
                            strip.frame_rect.right + 2.0f,
                            strip.frame_rect.bottom + 2.0f);
                        m_render_target->DrawRectangle(glow_rect, m_active_level_glow_brush.Get(), 5.5f);
                    }

                m_header_action_brush->SetColor(ParseHexColor(strip.style.fill_color, 0.88f));
                m_render_target->FillRectangle(left_slice_rect, m_header_action_brush.Get());
                m_render_target->FillRectangle(right_slice_rect, m_header_action_brush.Get());
                m_render_target->FillRectangle(bottom_slice_rect, m_header_action_brush.Get());

                m_header_action_brush->SetColor(ParseHexColor(strip.style.fill_color));
                m_render_target->FillRectangle(strip.rect, m_header_action_brush.Get());
                m_tile_text_plate_brush->SetColor(ParseHexColor(strip.style.text_plate_color, 0.42f));
                const D2D1_ROUNDED_RECT strip_plate_rect = D2D1::RoundedRect(
                    D2D1::RectF(
                        strip.rect.left + 1.0f,
                        strip.rect.top + 1.0f,
                        std::max(strip.rect.left + 2.0f, strip.rect.right - 1.0f),
                        std::max(strip.rect.top + 2.0f, strip.rect.bottom - 1.0f)),
                    2.0f,
                    2.0f);
                m_render_target->FillRoundedRectangle(strip_plate_rect, m_tile_text_plate_brush.Get());
                m_render_target->DrawRectangle(
                    strip.rect,
                    is_hovered ? m_hover_brush.Get() : m_border_brush.Get(),
                    is_hovered ? 1.6f : 0.9f);
                    if (is_active_level)
                    {
                        m_render_target->DrawRectangle(strip.frame_rect, m_hover_brush.Get(), 2.8f);
                    }

                    m_tile_text_brush->SetColor(ParseHexColor(strip.style.label_text_color));
                    m_tile_text_outline_brush->SetColor(ParseHexColor(strip.style.text_outline_color, 0.78f));
                    DrawTileTextLine(
                        strip.text,
                        m_small_format.Get(),
                        D2D1::RectF(strip.rect.left + 8.0f, strip.rect.top + 3.0f, strip.rect.right - 8.0f, strip.rect.bottom - 3.0f),
                        m_tile_text_brush.Get(),
                        m_tile_text_outline_brush.Get());
                }
            }

            m_tile_brush->SetColor(ParseHexColor(frame_style.fill_color));
            m_render_target->FillRectangle(m_treemap_content_rect, m_tile_brush.Get());

            for (size_t index = 0; index < m_rects.size(); ++index)
            {
                const visualization::TreemapRect& rect = m_rects[index];
                const D2D1_RECT_F draw_rect = D2D1::RectF(
                    m_treemap_content_rect.left + static_cast<float>(rect.x),
                    m_treemap_content_rect.top + static_cast<float>(rect.y),
                    m_treemap_content_rect.left + static_cast<float>(rect.x + rect.width),
                    m_treemap_content_rect.top + static_cast<float>(rect.y + rect.height));

                m_tile_brush->SetColor(ParseHexColor(rect.fill_color));
                m_render_target->FillRectangle(draw_rect, m_tile_brush.Get());

                const bool is_hovered = static_cast<int>(index) == m_hovered_index;
                ID2D1Brush* border_brush = is_hovered ? m_hover_brush.Get() : m_border_brush.Get();
                const float stroke_width = is_hovered ? 2.0f : 0.75f;
                const float tile_width = draw_rect.right - draw_rect.left;
                const float tile_height = draw_rect.bottom - draw_rect.top;
                const bool is_view_root = rect.node_id == m_view_root_id;
                const bool is_directory_header = rect.is_dir && !is_view_root;

                if (is_directory_header && tile_width >= 4.0f && tile_height >= 4.0f)
                {
                    const float header_height = std::min(
                        std::max(3.0f, static_cast<float>(m_layout_options.directory_header_height) - 2.0f),
                        std::max(0.0f, tile_height - 2.0f));
                    if (header_height > 0.0f)
                    {
                        m_tile_text_plate_brush->SetColor(ParseHexColor(rect.text_plate_color, 0.42f));
                        const D2D1_ROUNDED_RECT header_rect = D2D1::RoundedRect(
                            D2D1::RectF(
                                draw_rect.left + 1.0f,
                                draw_rect.top + 1.0f,
                                std::max(draw_rect.left + 2.0f, draw_rect.right - 1.0f),
                                std::min(draw_rect.bottom - 1.0f, draw_rect.top + 1.0f + header_height)),
                            2.0f,
                            2.0f);
                        m_render_target->FillRoundedRectangle(header_rect, m_tile_text_plate_brush.Get());
                    }
                }

                m_render_target->DrawRectangle(draw_rect, border_brush, stroke_width);

                if (tile_width >= 56.0f && tile_height >= 18.0f)
                {
                    m_tile_text_brush->SetColor(ParseHexColor(rect.label_text_color));
                    m_tile_secondary_text_brush->SetColor(ParseHexColor(rect.secondary_text_color));
                    m_tile_text_outline_brush->SetColor(ParseHexColor(rect.text_outline_color, 0.78f));
                    m_tile_text_plate_brush->SetColor(ParseHexColor(rect.text_plate_color, 0.34f));

                    const bool is_leaf_file = !rect.is_dir;
                    const bool show_label = tile_width >= 92.0f && tile_height >= 34.0f && !is_directory_header;
                    if (!is_leaf_file && !is_view_root && !is_directory_header)
                    {
                        const float plate_height = show_label ? 30.0f : 16.0f;
                        const D2D1_ROUNDED_RECT plate_rect = D2D1::RoundedRect(
                            D2D1::RectF(
                                draw_rect.left + 2.0f,
                                draw_rect.top + 2.0f,
                                std::max(draw_rect.left + 4.0f, draw_rect.right - 2.0f),
                                std::min(draw_rect.bottom - 2.0f, draw_rect.top + 2.0f + plate_height)),
                            2.0f,
                            2.0f);
                        m_render_target->FillRoundedRectangle(plate_rect, m_tile_text_plate_brush.Get());
                    }

                    if (is_directory_header)
                    {
                        const D2D1_RECT_F header_rect = D2D1::RectF(
                            draw_rect.left + 6.0f,
                            draw_rect.top + 2.0f,
                            draw_rect.right - 6.0f,
                            draw_rect.top + 16.0f);
                        const float header_width = std::max(0.0f, header_rect.right - header_rect.left);
                        const std::wstring header_text = BuildDirectoryHeaderText(
                            rect.label,
                            rect.size,
                            header_width);

                        DrawTileTextLine(
                            header_text,
                            m_small_format.Get(),
                            header_rect,
                            m_tile_text_brush.Get(),
                            m_tile_text_outline_brush.Get(),
                            DWRITE_TEXT_ALIGNMENT_LEADING);
                    }
                    else if (!is_view_root && tile_width >= 64.0f && tile_height >= 40.0f)
                    {
                        const std::wstring size_text = HumanizeBytes(rect.size);
                        const float margin = 6.0f;
                        const float size_height = 14.0f;
                        const float gap = 2.0f;
                        const float block_height = std::min(tile_height - 12.0f, 42.0f);
                        const float block_top = draw_rect.top + std::max(4.0f, (tile_height - block_height) * 0.42f);
                        const D2D1_RECT_F label_rect = D2D1::RectF(
                            draw_rect.left + margin,
                            block_top,
                            draw_rect.right - margin,
                            block_top + block_height - size_height - gap);
                        const D2D1_RECT_F size_rect = D2D1::RectF(
                            draw_rect.left + margin,
                            block_top + block_height - size_height,
                            draw_rect.right - margin,
                            block_top + block_height);

                        DrawTileTextLayout(
                            rect.label,
                            m_small_format.Get(),
                            label_rect,
                            m_tile_text_brush.Get(),
                            m_tile_text_outline_brush.Get(),
                            DWRITE_TEXT_ALIGNMENT_CENTER,
                            DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                            DWRITE_WORD_WRAPPING_NO_WRAP);
                        DrawTileTextLayout(
                            size_text,
                            m_small_format.Get(),
                            size_rect,
                            m_tile_secondary_text_brush.Get(),
                            m_tile_text_outline_brush.Get(),
                            DWRITE_TEXT_ALIGNMENT_CENTER,
                            DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
                            DWRITE_WORD_WRAPPING_NO_WRAP);
                    }
                    else
                    {
                        const std::wstring size_text = HumanizeBytes(rect.size);
                        const bool can_show_label = tile_width >= 44.0f && tile_height >= 24.0f;
                        const bool can_show_size = tile_width >= 32.0f && tile_height >= 14.0f;
                        if (can_show_label && can_show_size)
                        {
                            const float block_height = std::min(tile_height - 4.0f, 22.0f);
                            const float block_top = draw_rect.top + std::max(1.0f, (tile_height - block_height) * 0.5f);
                            const D2D1_RECT_F label_rect = D2D1::RectF(
                                draw_rect.left + 4.0f,
                                block_top,
                                draw_rect.right - 4.0f,
                                block_top + 10.0f);
                            const D2D1_RECT_F size_rect = D2D1::RectF(
                                draw_rect.left + 4.0f,
                                block_top + 10.0f,
                                draw_rect.right - 4.0f,
                                block_top + block_height);
                            DrawTileTextLayout(
                                rect.label,
                                m_small_format.Get(),
                                label_rect,
                                m_tile_text_brush.Get(),
                                m_tile_text_outline_brush.Get(),
                                DWRITE_TEXT_ALIGNMENT_CENTER,
                                DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                                DWRITE_WORD_WRAPPING_NO_WRAP);
                            DrawTileTextLayout(
                                size_text,
                                m_small_format.Get(),
                                size_rect,
                                m_tile_secondary_text_brush.Get(),
                                m_tile_text_outline_brush.Get(),
                                DWRITE_TEXT_ALIGNMENT_CENTER,
                                DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                                DWRITE_WORD_WRAPPING_NO_WRAP);
                        }
                        else if (can_show_size)
                        {
                            const D2D1_RECT_F size_rect = D2D1::RectF(
                                draw_rect.left + 3.0f,
                                draw_rect.top + 1.0f,
                                draw_rect.right - 3.0f,
                                draw_rect.bottom - 1.0f);
                            DrawTileTextLayout(
                                size_text,
                                m_small_format.Get(),
                                size_rect,
                                m_tile_secondary_text_brush.Get(),
                                m_tile_text_outline_brush.Get(),
                                DWRITE_TEXT_ALIGNMENT_CENTER,
                                DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                                DWRITE_WORD_WRAPPING_NO_WRAP);
                        }
                    }
                }
            }
        }

        void DrawHeader()
        {
            const D2D1_RECT_F header_rect = D2D1::RectF(
                kOuterPadding,
                kOuterPadding,
                m_canvas_rect.right,
                kOuterPadding + kHeaderHeight - 12.0f);
            const D2D1_ROUNDED_RECT panel = D2D1::RoundedRect(header_rect, 20.0f, 20.0f);
            m_render_target->FillRoundedRectangle(panel, m_panel_brush.Get());
            m_render_target->DrawRoundedRectangle(panel, m_border_brush.Get(), 1.2f);

            m_render_target->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(header_rect.left + 140.0f, header_rect.top + 38.0f), 130.0f, 46.0f),
                m_glow_a_brush.Get());
            m_render_target->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(header_rect.right - 140.0f, header_rect.top + 34.0f), 150.0f, 54.0f),
                m_glow_b_brush.Get());

            DrawTextLine(
                L"SpaceScanner",
                m_title_format.Get(),
                D2D1::RectF(header_rect.left + 22.0f, header_rect.top + 12.0f, header_rect.left + 300.0f, header_rect.top + 44.0f),
                m_text_brush.Get());

            const Node* focused_node = m_tree && m_view_root_id != 0
                ? m_tree->GetNode(m_view_root_id)
                : nullptr;
            const std::wstring focused_path = focused_node
                ? m_tree->BuildPath(m_view_root_id)
                : (m_options.path.empty() ? L"(no path selected)" : m_options.path);
            const std::wstring focused_name = focused_node
                ? (focused_node->name.empty() ? focused_path : focused_node->name)
                : (m_options.path.empty() ? L"(no path selected)" : m_options.path);
            const std::wstring focused_size = focused_node
                ? HumanizeBytes(focused_node->size)
                : L"-";

            DrawTextLine(
                focused_path,
                m_small_format.Get(),
                D2D1::RectF(header_rect.left + 24.0f, header_rect.top + 58.0f, header_rect.right - 24.0f, header_rect.top + 74.0f),
                m_muted_brush.Get());

            std::wstringstream summary;
            if (m_scan_in_progress)
            {
                summary << L"Backend: " << (m_requested_path_is_ntfs ? L"NTFS MFT" : L"Directory")
                    << L"  |  Status: scanning";
            }
            else if (m_tree)
            {
                const Node* root = m_tree->GetNode(m_view_root_id);
                summary << L"Backend: " << (m_used_mft_scan ? L"NTFS MFT" : L"Directory")
                    << L"  |  Focus size: " << (root ? HumanizeBytes(root->size) : L"-")
                    << L"  |  Nodes: " << m_tree->NodeCount()
                    << L"  |  Scan: " << FormatSeconds(m_elapsed_seconds);
            }
            else if (m_has_error)
            {
                summary << L"Status: failed";
            }
            else
            {
                summary << L"Status: idle";
            }

            DrawTextLine(
                summary.str(),
                m_body_format.Get(),
                D2D1::RectF(header_rect.left + 24.0f, header_rect.top + 80.0f, header_rect.right - 24.0f, header_rect.top + 100.0f),
                m_muted_brush.Get());

            const std::wstring hint = L"Click a directory to zoom in. Right-click or Backspace to go up. Home returns to the scan root.";
            DrawTextLine(
                hint,
                m_small_format.Get(),
                D2D1::RectF(header_rect.left + 24.0f, header_rect.bottom - 16.0f, header_rect.right - 24.0f, header_rect.bottom - 2.0f),
                m_muted_brush.Get());
        }

        bool ShouldFollowTooltip(const visualization::TreemapRect&) const
        {
            return true;
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

            if (!breadcrumb_hovered)
            {
                const visualization::TreemapRect& rect = m_rects[static_cast<size_t>(m_hovered_index)];
                if (!ShouldFollowTooltip(rect))
                {
                    StopTooltipAnimation();
                    return;
                }
            }

            const float delta_x = m_tooltip_target_position.x - m_tooltip_position.x;
            const float delta_y = m_tooltip_target_position.y - m_tooltip_position.y;
            const float distance_squared = (delta_x * delta_x) + (delta_y * delta_y);
            if (distance_squared <= (kTooltipAnimationSnapDistance * kTooltipAnimationSnapDistance))
            {
                m_tooltip_position = m_tooltip_target_position;
                StopTooltipAnimation();
                InvalidateCanvas();
                return;
            }

            m_tooltip_position.x += delta_x * kTooltipAnimationSmoothing;
            m_tooltip_position.y += delta_y * kTooltipAnimationSmoothing;
            InvalidateCanvas();
        }

        D2D1_RECT_F GetHoveredRectBounds(const visualization::TreemapRect& rect) const
        {
            return D2D1::RectF(
                m_treemap_content_rect.left + static_cast<float>(rect.x),
                m_treemap_content_rect.top + static_cast<float>(rect.y),
                m_treemap_content_rect.left + static_cast<float>(rect.x + rect.width),
                m_treemap_content_rect.top + static_cast<float>(rect.y + rect.height));
        }

        bool TryGetHoverDisplayInfo(
            std::wstring* name_text,
            std::wstring* size_text,
            D2D1_RECT_F* hovered_bounds,
            bool* follow_tooltip) const
        {
            if (!name_text || !size_text || !hovered_bounds || !follow_tooltip)
            {
                return false;
            }

            if (m_hovered_breadcrumb_index >= 0 && m_hovered_breadcrumb_index < static_cast<int>(m_breadcrumbs.size()))
            {
                const BreadcrumbStrip& strip = m_breadcrumbs[static_cast<size_t>(m_hovered_breadcrumb_index)];
                *hovered_bounds = strip.rect;
                *follow_tooltip = true;

                if (m_tree)
                {
                    const Node* node = m_tree->GetNode(strip.node_id);
                    const std::wstring path = m_tree->BuildPath(strip.node_id);
                    *name_text = path.empty() ? strip.text : path;
                    *size_text = L"Size: " + (node ? HumanizeBytes(node->size) : L"-");
                }
                else
                {
                    *name_text = strip.text;
                    *size_text = L"Size: -";
                }
                return true;
            }

            if (m_hovered_index >= 0 && m_hovered_index < static_cast<int>(m_rects.size()))
            {
                const visualization::TreemapRect& rect = m_rects[static_cast<size_t>(m_hovered_index)];
                *hovered_bounds = GetHoveredRectBounds(rect);
                *follow_tooltip = ShouldFollowTooltip(rect);
                *name_text = rect.path.empty() ? rect.label : rect.path;
                *size_text = L"Size: " + HumanizeBytes(rect.size);
                return true;
            }

            return false;
        }

        D2D1_POINT_2F ResolveTooltipPosition(
            const D2D1_POINT_2F& anchor,
            const D2D1_RECT_F& hovered_rect,
            float tooltip_width,
            float tooltip_height,
            bool follow_tooltip) const
        {
            float tooltip_left = anchor.x + kTooltipOffsetX;
            float tooltip_top = anchor.y + kTooltipOffsetY;

            if (tooltip_left + tooltip_width > m_canvas_rect.right - kTooltipInnerMargin)
            {
                tooltip_left = anchor.x - tooltip_width - 10.0f;
            }
            if (tooltip_top + tooltip_height > m_canvas_rect.bottom - kTooltipInnerMargin)
            {
                tooltip_top = anchor.y - tooltip_height - 10.0f;
            }

            if (!follow_tooltip)
            {
                const float hovered_width = hovered_rect.right - hovered_rect.left;
                const float hovered_height = hovered_rect.bottom - hovered_rect.top;
                if (hovered_width >= tooltip_width + (kTooltipInnerMargin * 2.0f) &&
                    hovered_height >= tooltip_height + (kTooltipInnerMargin * 2.0f))
                {
                    const float min_hovered_left = hovered_rect.left + kTooltipInnerMargin;
                    const float max_hovered_left = hovered_rect.right - tooltip_width - kTooltipInnerMargin;
                    const float min_hovered_top = hovered_rect.top + kTooltipInnerMargin;
                    const float max_hovered_top = hovered_rect.bottom - tooltip_height - kTooltipInnerMargin;
                    tooltip_left = std::clamp(tooltip_left, min_hovered_left, max_hovered_left);
                    tooltip_top = std::clamp(tooltip_top, min_hovered_top, max_hovered_top);
                }
            }

            const float min_tooltip_left = m_canvas_rect.left + kTooltipInnerMargin;
            const float max_tooltip_left = std::max(min_tooltip_left, m_canvas_rect.right - tooltip_width - kTooltipInnerMargin);
            const float min_tooltip_top = m_canvas_rect.top + kTooltipInnerMargin;
            const float max_tooltip_top = std::max(min_tooltip_top, m_canvas_rect.bottom - tooltip_height - kTooltipInnerMargin);
            tooltip_left = std::clamp(tooltip_left, min_tooltip_left, max_tooltip_left);
            tooltip_top = std::clamp(tooltip_top, min_tooltip_top, max_tooltip_top);

            return D2D1::Point2F(tooltip_left, tooltip_top);
        }

        void DrawHoverTooltip()
        {
            if (!m_show_tooltip)
            {
                return;
            }
            if (!m_dwrite_factory || !m_small_format || !m_tooltip_background_brush || !m_tooltip_border_brush ||
                !m_tooltip_text_brush || !m_tooltip_secondary_text_brush || !m_tooltip_shadow_brush)
            {
                return;
            }

            std::wstring name_text;
            std::wstring size_text;
            D2D1_RECT_F hovered_bounds = m_canvas_rect;
            bool follow_tooltip = false;
            if (!TryGetHoverDisplayInfo(&name_text, &size_text, &hovered_bounds, &follow_tooltip))
            {
                return;
            }

            const float available_tooltip_width = std::max(48.0f, (m_canvas_rect.right - m_canvas_rect.left) - 12.0f);
            const float max_tooltip_width = std::min(420.0f, std::max(28.0f, available_tooltip_width - (kTooltipPaddingX * 2.0f)));
            const float max_tooltip_height = std::max(48.0f, (m_canvas_rect.bottom - m_canvas_rect.top) - 24.0f);

            ComPtr<IDWriteTextLayout> name_layout;
            HRESULT hr = m_dwrite_factory->CreateTextLayout(
                name_text.c_str(),
                static_cast<UINT32>(name_text.size()),
                m_small_format.Get(),
                max_tooltip_width,
                max_tooltip_height,
                name_layout.GetAddressOf());
            if (FAILED(hr) || !name_layout)
            {
                return;
            }
            name_layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

            ComPtr<IDWriteTextLayout> size_layout;
            hr = m_dwrite_factory->CreateTextLayout(
                size_text.c_str(),
                static_cast<UINT32>(size_text.size()),
                m_small_format.Get(),
                max_tooltip_width,
                22.0f,
                size_layout.GetAddressOf());
            if (FAILED(hr) || !size_layout)
            {
                return;
            }
            size_layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

            DWRITE_TEXT_METRICS name_metrics = {};
            DWRITE_TEXT_METRICS size_metrics = {};
            if (FAILED(name_layout->GetMetrics(&name_metrics)) || FAILED(size_layout->GetMetrics(&size_metrics)))
            {
                return;
            }

            const float text_width = std::max(
                std::max(name_metrics.widthIncludingTrailingWhitespace, size_metrics.widthIncludingTrailingWhitespace),
                96.0f);
            const float name_height = std::max(name_metrics.height, 16.0f);
            const float size_height = std::max(size_metrics.height, 14.0f);
            const float tooltip_width = std::min(available_tooltip_width, text_width + (kTooltipPaddingX * 2.0f));
            const float tooltip_height = (kTooltipPaddingY * 2.0f) + name_height + kTooltipLineGap + size_height;
            const D2D1_POINT_2F tooltip_origin = ResolveTooltipPosition(
                m_tooltip_position,
                hovered_bounds,
                tooltip_width,
                tooltip_height,
                follow_tooltip);
            const float tooltip_left = tooltip_origin.x;
            const float tooltip_top = tooltip_origin.y;

            const D2D1_ROUNDED_RECT shadow_rect = D2D1::RoundedRect(
                D2D1::RectF(
                    tooltip_left + 2.0f,
                    tooltip_top + 3.0f,
                    tooltip_left + tooltip_width + 2.0f,
                    tooltip_top + tooltip_height + 3.0f),
                kTooltipCornerRadius,
                kTooltipCornerRadius);
            const D2D1_ROUNDED_RECT tooltip_rect = D2D1::RoundedRect(
                D2D1::RectF(
                    tooltip_left,
                    tooltip_top,
                    tooltip_left + tooltip_width,
                    tooltip_top + tooltip_height),
                kTooltipCornerRadius,
                kTooltipCornerRadius);
            m_render_target->FillRoundedRectangle(shadow_rect, m_tooltip_shadow_brush.Get());
            m_render_target->FillRoundedRectangle(tooltip_rect, m_tooltip_background_brush.Get());
            m_render_target->DrawRoundedRectangle(tooltip_rect, m_tooltip_border_brush.Get(), 1.0f);

            const D2D1_POINT_2F name_origin = D2D1::Point2F(
                tooltip_left + kTooltipPaddingX,
                tooltip_top + kTooltipPaddingY);
            const D2D1_POINT_2F size_origin = D2D1::Point2F(
                tooltip_left + kTooltipPaddingX,
                tooltip_top + kTooltipPaddingY + name_height + kTooltipLineGap);
            m_render_target->DrawTextLayout(name_origin, name_layout.Get(), m_tooltip_text_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            m_render_target->DrawTextLayout(size_origin, size_layout.Get(), m_tooltip_secondary_text_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        void DrawStatusBar()
        {
            if (!m_show_status_bar)
            {
                return;
            }

            const D2D1_ROUNDED_RECT status_panel = D2D1::RoundedRect(m_status_bar_rect, 12.0f, 12.0f);
            m_render_target->FillRoundedRectangle(status_panel, m_panel_brush.Get());
            m_render_target->DrawRoundedRectangle(status_panel, m_border_brush.Get(), 1.0f);

            std::wstring name_text;
            std::wstring size_text;
            D2D1_RECT_F hovered_bounds = m_status_bar_rect;
            bool follow_tooltip = true;
            if (!TryGetHoverDisplayInfo(&name_text, &size_text, &hovered_bounds, &follow_tooltip))
            {
                return;
            }

            const std::wstring status_text = name_text + L" - " + size_text.substr(6);
            const D2D1_RECT_F text_rect = D2D1::RectF(
                m_status_bar_rect.left + 12.0f,
                m_status_bar_rect.top + kStatusBarTextVerticalPadding,
                m_status_bar_rect.right - 12.0f,
                m_status_bar_rect.bottom - kStatusBarTextVerticalPadding);
            DrawTextLine(status_text, m_small_format.Get(), text_rect, m_text_brush.Get());
        }

        void OnPaint()
        {
            PAINTSTRUCT paint = {};
            BeginPaint(m_hwnd, &paint);

            HRESULT hr = CreateDeviceResources();
            if (SUCCEEDED(hr))
            {
                m_render_target->BeginDraw();
                m_render_target->SetTransform(D2D1::Matrix3x2F::Identity());
                m_render_target->Clear(ColorFromRgb(7, 12, 24));

                UpdateCanvasRect();
                DrawHeader();
                DrawTreemap();
                DrawStatusBar();
                DrawHoverTooltip();

                hr = m_render_target->EndDraw();
                if (hr == D2DERR_RECREATE_TARGET)
                {
                    DiscardDeviceResources();
                }
            }

            EndPaint(m_hwnd, &paint);
        }

        GuiOptions m_options;
        HWND m_hwnd = nullptr;
        HWND m_path_edit = nullptr;
        HWND m_scan_button = nullptr;
        HWND m_browse_button = nullptr;
        HWND m_depth_label = nullptr;
        HWND m_depth_edit = nullptr;
        HWND m_depth_up_button = nullptr;
        HWND m_depth_down_button = nullptr;
        HWND m_show_tooltip_check = nullptr;
        HWND m_show_status_bar_check = nullptr;
        std::shared_ptr<SharedScanState> m_shared_scan_state;
        bool m_requested_path_is_ntfs = false;

        ComPtr<ID2D1Factory> m_d2d_factory;
        ComPtr<IDWriteFactory> m_dwrite_factory;
        ComPtr<ID2D1HwndRenderTarget> m_render_target;
        ComPtr<ID2D1SolidColorBrush> m_text_brush;
        ComPtr<ID2D1SolidColorBrush> m_muted_brush;
        ComPtr<ID2D1SolidColorBrush> m_panel_brush;
        ComPtr<ID2D1SolidColorBrush> m_canvas_brush;
        ComPtr<ID2D1SolidColorBrush> m_border_brush;
        ComPtr<ID2D1SolidColorBrush> m_tile_brush;
        ComPtr<ID2D1SolidColorBrush> m_tile_text_brush;
        ComPtr<ID2D1SolidColorBrush> m_tile_secondary_text_brush;
        ComPtr<ID2D1SolidColorBrush> m_tile_text_outline_brush;
        ComPtr<ID2D1SolidColorBrush> m_tile_text_plate_brush;
        ComPtr<ID2D1SolidColorBrush> m_hover_brush;
        ComPtr<ID2D1SolidColorBrush> m_active_level_glow_brush;
        ComPtr<ID2D1SolidColorBrush> m_active_level_shadow_brush;
        ComPtr<ID2D1SolidColorBrush> m_error_brush;
        ComPtr<ID2D1SolidColorBrush> m_glow_a_brush;
        ComPtr<ID2D1SolidColorBrush> m_glow_b_brush;
        ComPtr<ID2D1SolidColorBrush> m_header_action_brush;
        ComPtr<ID2D1SolidColorBrush> m_header_action_hover_brush;
        ComPtr<ID2D1SolidColorBrush> m_tooltip_background_brush;
        ComPtr<ID2D1SolidColorBrush> m_tooltip_border_brush;
        ComPtr<ID2D1SolidColorBrush> m_tooltip_text_brush;
        ComPtr<ID2D1SolidColorBrush> m_tooltip_secondary_text_brush;
        ComPtr<ID2D1SolidColorBrush> m_tooltip_shadow_brush;
        ComPtr<IDWriteTextFormat> m_title_format;
        ComPtr<IDWriteTextFormat> m_body_format;
        ComPtr<IDWriteTextFormat> m_small_format;
        ComPtr<IDWriteInlineObject> m_ellipsis_trimming_sign;

        bool m_scan_in_progress = false;
        bool m_has_error = false;
        bool m_used_mft_scan = false;
        int m_hovered_index = -1;
        int m_hovered_breadcrumb_index = -1;
        int m_max_depth = 3;
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
        bool m_tooltip_animation_active = false;
        D2D1_POINT_2F m_mouse_position = D2D1::Point2F(-1.0f, -1.0f);
        D2D1_POINT_2F m_tooltip_position = D2D1::Point2F(-1.0f, -1.0f);
        D2D1_POINT_2F m_tooltip_target_position = D2D1::Point2F(-1.0f, -1.0f);
        D2D1_RECT_F m_canvas_rect = D2D1::RectF(0, 0, 0, 0);
        D2D1_RECT_F m_status_bar_rect = D2D1::RectF(0, 0, 0, 0);
        D2D1_RECT_F m_treemap_content_rect = D2D1::RectF(0, 0, 0, 0);
        D2D1_RECT_F m_focus_up_rect = D2D1::RectF(0, 0, 0, 0);
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
