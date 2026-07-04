#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gui/app_settings.h"
#include "gui/direct2d_renderer.h"
#include "gui/resource.h"

#include "common/file_tree.h"
#include "common/stopwatch.h"
#include "core/dir_scanner.h"
#include "core/ntfs/mft_scanner.h"
#include "visualization/treemap.h"

namespace swift_disk::gui
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
    constexpr int IDC_HOME_BUTTON = 1010;
    constexpr int IDC_HELP_BUTTON = 1011;
    constexpr int IDC_CLEAR_BUTTON = 1012;
    constexpr int IDC_HELP_TEXT = 1101;
    constexpr int IDC_HELP_CLOSE_BUTTON = 1102;
    constexpr int IDM_TREEMAP_OPEN_IN_EXPLORER = 2001;
    constexpr int IDM_TREEMAP_COPY_FULL_PATH = 2002;
    constexpr int IDM_TREEMAP_PROPERTIES = 2003;
    constexpr int IDM_TREEMAP_OPEN = 2004;
    constexpr int IDM_TREEMAP_OPEN_WITH = 2005;

    struct GuiOptions
    {
        std::wstring path;
        bool show_help = false;
    };

    struct ScanPayload
    {
        std::unique_ptr<FileTree> tree;
        bool has_volume_space_info = false;
        bool scanned_path_is_volume_root = false;
        double elapsed_seconds = 0.0;
        std::uint64_t volume_free_bytes = 0;
        std::uint64_t volume_total_bytes = 0;
        std::wstring path;
        std::wstring volume_root_path;
        std::wstring error;
    };

    struct SharedScanState
    {
        std::mutex mutex;
        std::unique_ptr<ScanPayload> pending_payload;
    };

    struct HelpWindowState
    {
        HBRUSH background_brush = nullptr;
        HBRUSH text_background_brush = nullptr;
    };

    struct DriveListEntry
    {
        std::wstring root_path;
        std::wstring display_text;
    };

    struct VolumeSpaceInfo
    {
        std::wstring root_path;
        std::uint64_t free_bytes = 0;
        std::uint64_t total_bytes = 0;
        bool path_is_volume_root = false;
    };

    std::wstring TrimPathQuoteCharacters(std::wstring path)
    {
        while (!path.empty() && path.front() == L'"')
        {
            path.erase(path.begin());
        }
        while (!path.empty() && path.back() == L'"')
        {
            path.pop_back();
        }
        return path;
    }

    std::wstring NormalizeScanPath(const std::wstring& path)
    {
        std::wstring normalized_path = TrimPathQuoteCharacters(path);
        if (normalized_path.size() == 2 &&
            ((normalized_path[0] >= L'A' && normalized_path[0] <= L'Z') ||
                (normalized_path[0] >= L'a' && normalized_path[0] <= L'z')) &&
            normalized_path[1] == L':')
        {
            return normalized_path + L'\\';
        }

        return normalized_path;
    }

    wchar_t ToUpperDriveLetter(wchar_t drive_letter)
    {
        if (drive_letter >= L'a' && drive_letter <= L'z')
        {
            return static_cast<wchar_t>(drive_letter - L'a' + L'A');
        }

        return drive_letter;
    }

    std::wstring GetDriveTypeDisplayName(UINT drive_type)
    {
        switch (drive_type)
        {
        case DRIVE_REMOVABLE:
            return L"Removable Drive";
        case DRIVE_FIXED:
            return L"Local Disk";
        case DRIVE_REMOTE:
            return L"Network Drive";
        case DRIVE_CDROM:
            return L"Optical Drive";
        case DRIVE_RAMDISK:
            return L"RAM Disk";
        default:
            return L"Drive";
        }
    }

    std::vector<DriveListEntry> EnumerateNtfsDriveEntries()
    {
        const DWORD required_length = GetLogicalDriveStringsW(0, nullptr);
        if (required_length == 0)
        {
            return {};
        }

        std::wstring buffer(required_length, L'\0');
        const DWORD written_length = GetLogicalDriveStringsW(
            required_length,
            buffer.data());
        if (written_length == 0 || written_length >= required_length)
        {
            return {};
        }

        std::vector<DriveListEntry> entries;
        const wchar_t* root_path = buffer.c_str();
        while (*root_path != L'\0')
        {
            const std::wstring root(root_path);
            const UINT drive_type = GetDriveTypeW(root.c_str());

            wchar_t volume_name[MAX_PATH] = {};
            wchar_t file_system_name[MAX_PATH] = {};
            if (GetVolumeInformationW(
                root.c_str(),
                volume_name,
                static_cast<DWORD>(std::size(volume_name)),
                nullptr,
                nullptr,
                nullptr,
                file_system_name,
                static_cast<DWORD>(std::size(file_system_name))) &&
                _wcsicmp(file_system_name, L"NTFS") == 0)
            {
                std::wstring normalized_root = root;
                if (!normalized_root.empty())
                {
                    normalized_root[0] = ToUpperDriveLetter(normalized_root[0]);
                }

                std::wstring display_name = volume_name[0] != L'\0'
                    ? volume_name
                    : GetDriveTypeDisplayName(drive_type);
                std::wstring display_text = L"[";
                display_text.push_back(normalized_root.empty() ? L'?' : normalized_root[0]);
                display_text += L":] ";
                display_text += display_name;

                entries.push_back({ std::move(normalized_root), std::move(display_text) });
            }

            root_path += wcslen(root_path) + 1;
        }

        return entries;
    }

    std::wstring GetDefaultPathFieldValue()
    {
        const std::vector<DriveListEntry> entries = EnumerateNtfsDriveEntries();
        for (const DriveListEntry& entry : entries)
        {
            if (entry.root_path.size() >= 2 && entry.root_path[0] == L'C')
            {
                return entry.display_text;
            }
        }

        return entries.empty() ? std::wstring() : entries.front().display_text;
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

    std::wstring NormalizePathForComparison(std::wstring path)
    {
        std::replace(path.begin(), path.end(), L'/', L'\\');
        if (path.size() == 2 && path[1] == L':')
        {
            path.push_back(L'\\');
        }

        while (path.size() > 3 && path.back() == L'\\')
        {
            path.pop_back();
        }

        return path;
    }

    bool AreSameFilesystemPath(const std::wstring& left, const std::wstring& right)
    {
        const std::wstring normalized_left = NormalizePathForComparison(left);
        const std::wstring normalized_right = NormalizePathForComparison(right);
        return _wcsicmp(normalized_left.c_str(), normalized_right.c_str()) == 0;
    }

    bool TryGetVolumeSpaceInfo(const std::wstring& path, VolumeSpaceInfo* info)
    {
        if (!info)
        {
            return false;
        }

        std::wstring volume_root;
        if (!TryGetVolumeRoot(path, &volume_root))
        {
            return false;
        }

        ULARGE_INTEGER free_bytes_available = {};
        ULARGE_INTEGER total_bytes = {};
        ULARGE_INTEGER total_free_bytes = {};
        if (!GetDiskFreeSpaceExW(
            volume_root.c_str(),
            &free_bytes_available,
            &total_bytes,
            &total_free_bytes))
        {
            return false;
        }

        info->root_path = std::move(volume_root);
        info->free_bytes = total_free_bytes.QuadPart;
        info->total_bytes = total_bytes.QuadPart;
        info->path_is_volume_root = AreSameFilesystemPath(path, info->root_path);
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

    bool IsExistingDirectoryPath(const std::wstring& path)
    {
        if (path.empty())
        {
            return false;
        }

        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
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

    int ScaleIntForDpi(float value, float dpi_scale)
    {
        return static_cast<int>(std::lround(ScaleForDpi(value, dpi_scale)));
    }

    class MainWindow
    {
    public:
        explicit MainWindow(GuiOptions options)
            : m_options(std::move(options)),
            m_shared_scan_state(std::make_shared<SharedScanState>())
        {
            const AppSettings settings = LoadAppSettings();
            m_max_depth = settings.GetMaxDepth();
            m_show_tooltip = settings.GetShowTooltip();
            m_show_status_bar = settings.GetShowStatusBar();
            m_last_scan_path = NormalizeScanPath(settings.GetLastScanPath());
        }

        bool Create(HINSTANCE instance, int show_command)
        {
            WNDCLASSEXW window_class = {};
            window_class.cbSize = sizeof(window_class);
            window_class.style = CS_HREDRAW | CS_VREDRAW;
            window_class.lpfnWndProc = &MainWindow::WindowProc;
            window_class.hInstance = instance;
            window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SWIFT_DISK_APP));
            if (!window_class.hIcon)
            {
                window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
            }
            window_class.hIconSm = static_cast<HICON>(LoadImageW(
                instance,
                MAKEINTRESOURCEW(IDI_SWIFT_DISK_APP),
                IMAGE_ICON,
                GetSystemMetrics(SM_CXSMICON),
                GetSystemMetrics(SM_CYSMICON),
                LR_DEFAULTCOLOR | LR_SHARED));
            if (!window_class.hIconSm)
            {
                window_class.hIconSm = window_class.hIcon;
            }
            window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            window_class.hbrBackground = nullptr;
            window_class.lpszClassName = L"SwiftDiskMainWindow";
            if (!RegisterClassExW(&window_class))
            {
                return false;
            }

            m_hwnd = CreateWindowExW(
                0,
                window_class.lpszClassName,
                L"SwiftDisk",
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
        static constexpr float kHeaderHeight = 116.0f;
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
        static constexpr const wchar_t* kControlWindowPropName = L"SwiftDiskMainWindow";
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
                    m_last_scan_path = m_options.path;
                    SaveCurrentAppSettings();
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
            case WM_GETMINMAXINFO:
                OnGetMinMaxInfo(reinterpret_cast<MINMAXINFO*>(l_param));
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
            case WM_RBUTTONDOWN:
                OnRightButtonDown();
                return 0;
            case WM_RBUTTONUP:
                OnRightButtonUp(GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param));
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
            case WM_CTLCOLORLISTBOX:
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
                OnSize();
                return 0;
            case WM_DPICHANGED:
                if (l_param)
                {
                    const RECT* suggested_rect = reinterpret_cast<const RECT*>(l_param);
                    SetWindowPos(
                        m_hwnd,
                        nullptr,
                        suggested_rect->left,
                        suggested_rect->top,
                        suggested_rect->right - suggested_rect->left,
                        suggested_rect->bottom - suggested_rect->top,
                        SWP_NOZORDER | SWP_NOACTIVATE);
                }
                OnSize();
                return 0;
            case WM_TIMER:
                if (w_param == kTooltipAnimationTimerId)
                {
                    OnTooltipAnimationTimer();
                    return 0;
                }
                break;
            case WM_DESTROY:
                SaveCurrentAppSettings();
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

            if (message == WM_CTLCOLORLISTBOX)
            {
                SetBkColor(
                    hdc,
                    ColorRefFromRgb(kControlSurfaceRed, kControlSurfaceGreen, kControlSurfaceBlue));
                SetBkMode(hdc, OPAQUE);
                return reinterpret_cast<LRESULT>(m_control_surface_brush);
            }

            if ((message == WM_CTLCOLOREDIT || message == WM_CTLCOLORSTATIC) &&
                (IsPathFieldControl(control) || control == m_depth_edit))
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
            const float dpi_scale = GetWindowDpiScale(m_hwnd);
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
            checkbox_rect.left += ScaleIntForDpi(2.0f, dpi_scale);
            checkbox_rect.top += ScaleIntForDpi(3.0f, dpi_scale);
            checkbox_rect.right = checkbox_rect.left + ScaleIntForDpi(14.0f, dpi_scale);
            checkbox_rect.bottom = checkbox_rect.top + ScaleIntForDpi(14.0f, dpi_scale);

            FillGdiRect(draw_item.hDC, checkbox_rect, GetButtonFillColor(pressed, disabled, hovered, checked));
            DrawGdiFrame(
                draw_item.hDC,
                checkbox_rect,
                GetButtonBorderColor(disabled, hovered, checked));

            if (checked)
            {
                HPEN pen = CreatePen(
                    PS_SOLID,
                    std::max(1, ScaleIntForDpi(2.0f, dpi_scale)),
                    disabled
                        ? ColorRefFromRgb(129, 142, 170)
                        : ColorRefFromRgb(kControlTextRed, kControlTextGreen, kControlTextBlue));
                if (pen)
                {
                    const HGDIOBJ old_pen = SelectObject(draw_item.hDC, pen);
                    MoveToEx(
                        draw_item.hDC,
                        checkbox_rect.left + ScaleIntForDpi(3.0f, dpi_scale),
                        checkbox_rect.top + ScaleIntForDpi(8.0f, dpi_scale),
                        nullptr);
                    LineTo(
                        draw_item.hDC,
                        checkbox_rect.left + ScaleIntForDpi(6.0f, dpi_scale),
                        checkbox_rect.bottom - ScaleIntForDpi(4.0f, dpi_scale));
                    LineTo(
                        draw_item.hDC,
                        checkbox_rect.right - ScaleIntForDpi(3.0f, dpi_scale),
                        checkbox_rect.top + ScaleIntForDpi(4.0f, dpi_scale));
                    SelectObject(draw_item.hDC, old_pen);
                    DeleteObject(pen);
                }
            }

            RECT text_rect = draw_item.rcItem;
            text_rect.left = checkbox_rect.right + ScaleIntForDpi(8.0f, dpi_scale);

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
                const int focus_inset = ScaleIntForDpi(1.0f, dpi_scale);
                InflateRect(&focus_rect, -focus_inset, -focus_inset);
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
            case IDC_HOME_BUTTON:
            case IDC_HELP_BUTTON:
            case IDC_CLEAR_BUTTON:
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
                control == m_path_combo_edit ||
                control == m_scan_button ||
                control == m_browse_button ||
                control == m_home_button ||
                control == m_help_button ||
                control == m_clear_button ||
                control == m_depth_edit ||
                control == m_depth_up_button ||
                control == m_depth_down_button ||
                control == m_show_tooltip_check ||
                control == m_show_status_bar_check;
        }

        bool IsEditControl(HWND control) const
        {
            return control == m_path_combo_edit || control == m_depth_edit;
        }

        bool IsPathFieldControl(HWND control) const
        {
            return control == m_path_edit || control == m_path_combo_edit;
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
            case WM_LBUTTONDOWN:
                if (control == m_path_combo_edit)
                {
                    const LRESULT result = CallWindowProcW(original_proc, control, message, w_param, l_param);
                    ShowPathDropdown();
                    return result;
                }
                break;
            case WM_KEYDOWN:
                if (IsPathFieldControl(control) && w_param == VK_RETURN)
                {
                    BeginScanFromPathShortcut();
                    return 0;
                }
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
            AddControlHoverTracking(m_path_combo_edit);
            AddControlHoverTracking(m_scan_button);
            AddControlHoverTracking(m_browse_button);
            AddControlHoverTracking(m_home_button);
            AddControlHoverTracking(m_help_button);
            AddControlHoverTracking(m_clear_button);
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

        int MeasureControlTextWidth(HWND control) const
        {
            if (!control)
            {
                return 0;
            }

            wchar_t text[128] = {};
            GetWindowTextW(control, text, static_cast<int>(std::size(text)));
            const int text_length = static_cast<int>(wcslen(text));
            if (text_length <= 0)
            {
                return 0;
            }

            HDC hdc = GetDC(control);
            if (!hdc)
            {
                return 0;
            }

            const HFONT font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
            const HGDIOBJ old_font = font ? SelectObject(hdc, font) : nullptr;
            SIZE size = {};
            GetTextExtentPoint32W(hdc, text, text_length, &size);
            if (old_font)
            {
                SelectObject(hdc, old_font);
            }
            ReleaseDC(control, hdc);

            return std::max(size.cx, 0L);
        }

        int GetCheckboxControlWidth(HWND checkbox, int minimum_width, float dpi_scale) const
        {
            const int checkbox_left_padding = ScaleIntForDpi(2.0f, dpi_scale);
            const int checkbox_box_size = ScaleIntForDpi(14.0f, dpi_scale);
            const int checkbox_text_gap = ScaleIntForDpi(8.0f, dpi_scale);
            const int checkbox_trailing_padding = ScaleIntForDpi(8.0f, dpi_scale);
            const int measured_width =
                checkbox_left_padding +
                checkbox_box_size +
                checkbox_text_gap +
                MeasureControlTextWidth(checkbox) +
                checkbox_trailing_padding;
            return std::max(minimum_width, measured_width);
        }

        int GetButtonControlWidth(HWND button, int minimum_width, int horizontal_padding) const
        {
            const int measured_width = MeasureControlTextWidth(button) + (horizontal_padding * 2);
            return std::max(minimum_width, measured_width);
        }

        std::wstring GetInitialPathFieldValue() const
        {
            if (!m_options.path.empty())
            {
                return m_options.path;
            }

            if (IsExistingDirectoryPath(m_last_scan_path))
            {
                return m_last_scan_path;
            }

            return GetDefaultPathFieldValue();
        }

        void CreateHeaderControls()
        {
            HFONT gui_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            const std::wstring initial_path = GetInitialPathFieldValue();

            m_path_edit = CreateWindowExW(
                0,
                L"COMBOBOX",
                initial_path.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL,
                0,
                0,
                0,
                0,
                m_hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PATH_EDIT)),
                nullptr,
                nullptr);
            if (m_path_edit)
            {
                m_path_combo_edit = FindWindowExW(m_path_edit, nullptr, L"Edit", nullptr);
                SetWindowTextW(m_path_edit, initial_path.c_str());
                RefreshNtfsDriveDropdown();
            }

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

            m_home_button = CreateWindowExW(
                0,
                L"BUTTON",
                L"Home",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0,
                0,
                0,
                0,
                m_hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_HOME_BUTTON)),
                nullptr,
                nullptr);

            m_clear_button = CreateWindowExW(
                0,
                L"BUTTON",
                L"Clear",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0,
                0,
                0,
                0,
                m_hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CLEAR_BUTTON)),
                nullptr,
                nullptr);

            m_help_button = CreateWindowExW(
                0,
                L"BUTTON",
                L"Help",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0,
                0,
                0,
                0,
                m_hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_HELP_BUTTON)),
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
                L"Tooltip",
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
                L"Status bar",
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
            if (m_path_combo_edit)
            {
                SendMessageW(m_path_combo_edit, WM_SETFONT, reinterpret_cast<WPARAM>(gui_font), TRUE);
            }
            if (m_scan_button)
            {
                SendMessageW(m_scan_button, WM_SETFONT, reinterpret_cast<WPARAM>(gui_font), TRUE);
            }
            if (m_browse_button)
            {
                SendMessageW(m_browse_button, WM_SETFONT, reinterpret_cast<WPARAM>(gui_font), TRUE);
            }
            if (m_home_button)
            {
                SendMessageW(m_home_button, WM_SETFONT, reinterpret_cast<WPARAM>(gui_font), TRUE);
            }
            if (m_clear_button)
            {
                SendMessageW(m_clear_button, WM_SETFONT, reinterpret_cast<WPARAM>(gui_font), TRUE);
            }
            if (m_help_button)
            {
                SendMessageW(m_help_button, WM_SETFONT, reinterpret_cast<WPARAM>(gui_font), TRUE);
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

        int GetRequiredHeaderControlAreaWidth(float dpi_scale) const
        {
            const int group_box_horizontal_padding = ScaleIntForDpi(14.0f, dpi_scale);
            const int action_button_horizontal_padding = ScaleIntForDpi(14.0f, dpi_scale);
            const int action_button_width = std::max({
                GetButtonControlWidth(m_browse_button, 0, action_button_horizontal_padding),
                GetButtonControlWidth(m_scan_button, 0, action_button_horizontal_padding),
                GetButtonControlWidth(m_home_button, 0, action_button_horizontal_padding),
                GetButtonControlWidth(m_help_button, 0, action_button_horizontal_padding),
                GetButtonControlWidth(m_clear_button, 0, action_button_horizontal_padding)
            });
            const int button_gap = group_box_horizontal_padding;
            const int scan_row_width =
                ScaleIntForDpi(2.0f, dpi_scale) +
                ScaleIntForDpi(180.0f, dpi_scale) +
                button_gap +
                action_button_width +
                button_gap +
                action_button_width +
                ScaleIntForDpi(12.0f, dpi_scale);

            const int display_checkbox_gap = ScaleIntForDpi(4.0f, dpi_scale);
            const int checkbox_trailing_padding = ScaleIntForDpi(8.0f, dpi_scale);
            const int show_group_right_padding = group_box_horizontal_padding - checkbox_trailing_padding;
            const int show_tooltip_width = GetCheckboxControlWidth(m_show_tooltip_check, 0, dpi_scale);
            const int show_status_bar_width = GetCheckboxControlWidth(m_show_status_bar_check, 0, dpi_scale);
            const int checkbox_group_width =
                show_tooltip_width + display_checkbox_gap + show_status_bar_width;
            const int display_group_width =
                group_box_horizontal_padding + checkbox_group_width + show_group_right_padding;

            const int depth_label_width =
                std::max(MeasureControlTextWidth(m_depth_label), ScaleIntForDpi(32.0f, dpi_scale));
            const int depth_edit_width = ScaleIntForDpi(42.0f, dpi_scale);
            const int depth_button_width = ScaleIntForDpi(18.0f, dpi_scale);
            const int depth_button_gap = ScaleIntForDpi(4.0f, dpi_scale);
            const int depth_label_gap = ScaleIntForDpi(2.0f, dpi_scale);
            const int depth_group_width =
                depth_label_width + depth_label_gap + depth_edit_width + depth_button_gap + depth_button_width;
            const int treemap_control_gap = group_box_horizontal_padding;
            const int treemap_group_width = (group_box_horizontal_padding * 2) +
                depth_group_width + treemap_control_gap + action_button_width + button_gap + action_button_width;
            const int lower_row_width =
                ScaleIntForDpi(2.0f, dpi_scale) +
                display_group_width +
                group_box_horizontal_padding +
                treemap_group_width +
                group_box_horizontal_padding +
                ScaleIntForDpi(12.0f, dpi_scale) +
                action_button_width;

            return std::max(scan_row_width, lower_row_width);
        }

        int GetMinimumHeaderClientWidth(float dpi_scale) const
        {
            const int outer_padding = ScaleIntForDpi(kOuterPadding, dpi_scale);
            const int inner_padding = ScaleIntForDpi(22.0f, dpi_scale);
            const int header_info_min_width = ScaleIntForDpi(120.0f, dpi_scale);
            const int header_info_gap = ScaleIntForDpi(18.0f, dpi_scale);
            return std::max(
                ScaleIntForDpi(640.0f, dpi_scale),
                GetRequiredHeaderControlAreaWidth(dpi_scale) +
                    ((outer_padding + inner_padding) * 2) +
                    header_info_min_width +
                    header_info_gap);
        }

        void OnGetMinMaxInfo(MINMAXINFO* info) const
        {
            if (!info || !m_hwnd)
            {
                return;
            }

            const float dpi_scale = GetWindowDpiScale(m_hwnd);
            RECT min_rect = {
                0,
                0,
                GetMinimumHeaderClientWidth(dpi_scale),
                1
            };
            const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(m_hwnd, GWL_STYLE));
            const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE));
            AdjustWindowRectEx(&min_rect, style, FALSE, ex_style);
            info->ptMinTrackSize.x = std::max<LONG>(
                info->ptMinTrackSize.x,
                min_rect.right - min_rect.left);
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
            const float dpi_scale = GetWindowDpiScale(m_hwnd);

            const int outer_padding = ScaleIntForDpi(kOuterPadding, dpi_scale);
            const int inner_padding = ScaleIntForDpi(22.0f, dpi_scale);
            const int group_gap = ScaleIntForDpi(6.0f, dpi_scale);
            const int group_top = outer_padding + ScaleIntForDpi(4.0f, dpi_scale);
            const int control_area_right = width - outer_padding - inner_padding;
            const int max_control_area_width = std::max(
                ScaleIntForDpi(340.0f, dpi_scale),
                width - (outer_padding * 2) - (inner_padding * 2));
            const int min_control_area_width = std::min(ScaleIntForDpi(560.0f, dpi_scale), max_control_area_width);
            const int nominal_control_area_width = std::clamp(
                static_cast<int>((width - (outer_padding * 2) - (inner_padding * 2)) * 0.48f),
                min_control_area_width,
                std::min(ScaleIntForDpi(680.0f, dpi_scale), max_control_area_width));
            const int scan_group_height = ScaleIntForDpi(40.0f, dpi_scale);
            const int lower_row_vertical_offset = ScaleIntForDpi(10.0f, dpi_scale);
            const int lower_group_top = group_top + scan_group_height + group_gap + lower_row_vertical_offset;
            const int lower_group_height = ScaleIntForDpi(30.0f, dpi_scale);
            const int header_text_vertical_offset = ScaleIntForDpi(-5.0f, dpi_scale);
            const int header_text_top = group_top + header_text_vertical_offset;
            const int group_box_horizontal_padding = ScaleIntForDpi(14.0f, dpi_scale);
            const int group_box_top_padding = ScaleIntForDpi(10.0f, dpi_scale);
            const int group_box_bottom_padding = ScaleIntForDpi(9.0f, dpi_scale);
            const int action_button_horizontal_padding = ScaleIntForDpi(14.0f, dpi_scale);
            const int action_button_width = std::max({
                GetButtonControlWidth(m_browse_button, 0, action_button_horizontal_padding),
                GetButtonControlWidth(m_scan_button, 0, action_button_horizontal_padding),
                GetButtonControlWidth(m_home_button, 0, action_button_horizontal_padding),
                GetButtonControlWidth(m_help_button, 0, action_button_horizontal_padding),
                GetButtonControlWidth(m_clear_button, 0, action_button_horizontal_padding)
            });
            const int browse_width = action_button_width;
            const int scan_width = action_button_width;
            const int home_width = action_button_width;
            const int help_width = action_button_width;
            const int clear_width = action_button_width;
            const int button_gap = group_box_horizontal_padding;

            const int display_checkbox_gap = ScaleIntForDpi(4.0f, dpi_scale);
            const int lower_group_gap = group_box_horizontal_padding;
            const int checkbox_trailing_padding = ScaleIntForDpi(8.0f, dpi_scale);
            const int show_group_right_padding = group_box_horizontal_padding - checkbox_trailing_padding;
            const int show_tooltip_width = GetCheckboxControlWidth(m_show_tooltip_check, 0, dpi_scale);
            const int show_status_bar_width = GetCheckboxControlWidth(m_show_status_bar_check, 0, dpi_scale);
            const int checkbox_group_width =
                show_tooltip_width + display_checkbox_gap + show_status_bar_width;
            const int display_group_width =
                group_box_horizontal_padding + checkbox_group_width + show_group_right_padding;

            const int depth_label_width =
                std::max(MeasureControlTextWidth(m_depth_label), ScaleIntForDpi(32.0f, dpi_scale));
            const int depth_edit_width = ScaleIntForDpi(42.0f, dpi_scale);
            const int depth_button_width = ScaleIntForDpi(18.0f, dpi_scale);
            const int depth_button_gap = ScaleIntForDpi(4.0f, dpi_scale);
            const int depth_label_gap = ScaleIntForDpi(2.0f, dpi_scale);
            const int depth_group_width =
                depth_label_width + depth_label_gap + depth_edit_width + depth_button_gap + depth_button_width;
            const int treemap_control_gap = group_box_horizontal_padding;
            const int desired_treemap_group_width = (group_box_horizontal_padding * 2) +
                depth_group_width + treemap_control_gap + home_width + button_gap + clear_width;
            const int required_control_area_width = GetRequiredHeaderControlAreaWidth(dpi_scale);
            const int control_area_width = std::clamp(
                std::max(nominal_control_area_width, required_control_area_width),
                min_control_area_width,
                max_control_area_width);
            const int control_left = control_area_right - control_area_width;
            const int scan_group_left = control_left + ScaleIntForDpi(2.0f, dpi_scale);
            const int scan_group_top_inner = group_top + ScaleIntForDpi(10.0f, dpi_scale);
            const int browse_left = control_left + control_area_width - ScaleIntForDpi(12.0f, dpi_scale) - browse_width;
            const int scan_left = browse_left - button_gap - scan_width;
            const int edit_width = std::max(
                ScaleIntForDpi(180.0f, dpi_scale),
                scan_left - button_gap - scan_group_left);
            const int lower_row_left = control_left + ScaleIntForDpi(2.0f, dpi_scale);
            const int display_group_left = lower_row_left;
            const int treemap_group_left = display_group_left + display_group_width + lower_group_gap;
            const int help_left = browse_left;
            const int treemap_group_width = desired_treemap_group_width;
            const int depth_controls_left = treemap_group_left + group_box_horizontal_padding;
            const int home_left = depth_controls_left + depth_group_width + treemap_control_gap;
            const int clear_left = home_left + home_width + button_gap;
            const int header_info_left = outer_padding + inner_padding;
            const int header_info_right =
                std::max(header_info_left + ScaleIntForDpi(1.0f, dpi_scale), control_left - ScaleIntForDpi(18.0f, dpi_scale));

            m_header_info_rect = MakeRenderRect(
                static_cast<float>(header_info_left),
                static_cast<float>(header_text_top),
                static_cast<float>(header_info_right),
                static_cast<float>(outer_padding + ScaleIntForDpi(kHeaderHeight, dpi_scale) - ScaleIntForDpi(16.0f, dpi_scale)));
            m_scan_group_rect = MakeRenderRect(
                static_cast<float>(control_left),
                static_cast<float>(group_top),
                static_cast<float>(control_left + control_area_width),
                static_cast<float>(group_top + scan_group_height));
            m_treemap_group_rect = MakeRenderRect(
                static_cast<float>(treemap_group_left),
                static_cast<float>(lower_group_top - group_box_top_padding),
                static_cast<float>(treemap_group_left + treemap_group_width),
                static_cast<float>(lower_group_top + lower_group_height + group_box_bottom_padding));
            m_display_group_rect = MakeRenderRect(
                static_cast<float>(display_group_left),
                static_cast<float>(lower_group_top - group_box_top_padding),
                static_cast<float>(display_group_left + display_group_width),
                static_cast<float>(lower_group_top + lower_group_height + group_box_bottom_padding));

            const int depth_controls_top = lower_group_top + ScaleIntForDpi(1.0f, dpi_scale);
            const int depth_label_left = depth_controls_left;
            const int depth_edit_left = depth_label_left + depth_label_width + depth_label_gap;
            const int depth_button_left = depth_edit_left + depth_edit_width + depth_button_gap;

            const int show_tooltip_top = lower_group_top + ScaleIntForDpi(5.0f, dpi_scale);
            const int show_status_bar_top = show_tooltip_top;
            const int show_tooltip_left = display_group_left + group_box_horizontal_padding;
            const int show_status_bar_left = show_tooltip_left + show_tooltip_width + display_checkbox_gap;

            if (m_path_edit)
            {
                const int path_dropdown_height = ScaleIntForDpi(190.0f, dpi_scale);
                MoveWindow(m_path_edit, scan_group_left, scan_group_top_inner, edit_width, path_dropdown_height, TRUE);
                SendMessageW(m_path_edit, CB_SETDROPPEDWIDTH, static_cast<WPARAM>(edit_width), 0);
                ApplyEditTextInsets(
                    m_path_combo_edit,
                    ScaleIntForDpi(8.0f, dpi_scale),
                    ScaleIntForDpi(6.0f, dpi_scale),
                    ScaleIntForDpi(8.0f, dpi_scale),
                    ScaleIntForDpi(4.0f, dpi_scale));
            }
            if (m_scan_button)
            {
                MoveWindow(m_scan_button, scan_left, scan_group_top_inner, scan_width, ScaleIntForDpi(28.0f, dpi_scale), TRUE);
            }
            if (m_home_button)
            {
                MoveWindow(m_home_button, home_left, depth_controls_top, home_width, ScaleIntForDpi(28.0f, dpi_scale), TRUE);
            }
            if (m_clear_button)
            {
                MoveWindow(m_clear_button, clear_left, depth_controls_top, clear_width, ScaleIntForDpi(28.0f, dpi_scale), TRUE);
            }
            if (m_browse_button)
            {
                MoveWindow(m_browse_button, browse_left, scan_group_top_inner, browse_width, ScaleIntForDpi(28.0f, dpi_scale), TRUE);
            }
            if (m_help_button)
            {
                MoveWindow(m_help_button, help_left, depth_controls_top, help_width, ScaleIntForDpi(28.0f, dpi_scale), TRUE);
            }
            if (m_depth_label)
            {
                MoveWindow(
                    m_depth_label,
                    depth_label_left,
                    depth_controls_top + ScaleIntForDpi(6.0f, dpi_scale),
                    depth_label_width,
                    ScaleIntForDpi(18.0f, dpi_scale),
                    TRUE);
            }
            if (m_depth_edit)
            {
                MoveWindow(
                    m_depth_edit,
                    depth_edit_left,
                    depth_controls_top,
                    depth_edit_width,
                    ScaleIntForDpi(28.0f, dpi_scale),
                    TRUE);
                ApplyEditTextInsets(
                    m_depth_edit,
                    ScaleIntForDpi(2.0f, dpi_scale),
                    ScaleIntForDpi(5.0f, dpi_scale),
                    ScaleIntForDpi(2.0f, dpi_scale),
                    ScaleIntForDpi(4.0f, dpi_scale));
            }
            if (m_depth_up_button)
            {
                MoveWindow(
                    m_depth_up_button,
                    depth_button_left,
                    depth_controls_top,
                    depth_button_width,
                    ScaleIntForDpi(13.0f, dpi_scale),
                    TRUE);
            }
            if (m_depth_down_button)
            {
                MoveWindow(
                    m_depth_down_button,
                    depth_button_left,
                    depth_controls_top + ScaleIntForDpi(14.0f, dpi_scale),
                    depth_button_width,
                    ScaleIntForDpi(14.0f, dpi_scale),
                    TRUE);
            }
            if (m_show_tooltip_check)
            {
                MoveWindow(
                    m_show_tooltip_check,
                    show_tooltip_left,
                    show_tooltip_top,
                    show_tooltip_width,
                    ScaleIntForDpi(18.0f, dpi_scale),
                    TRUE);
            }
            if (m_show_status_bar_check)
            {
                MoveWindow(
                    m_show_status_bar_check,
                    show_status_bar_left,
                    show_status_bar_top,
                    show_status_bar_width,
                    ScaleIntForDpi(18.0f, dpi_scale),
                    TRUE);
            }
        }

        std::wstring GetRawPathFieldText() const
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

        std::wstring GetPathFromEdit() const
        {
            const std::wstring value = GetRawPathFieldText();
            for (const DriveListEntry& entry : m_ntfs_drive_entries)
            {
                if (value == entry.display_text)
                {
                    return entry.root_path;
                }
            }

            return value;
        }

        void RefreshNtfsDriveDropdown()
        {
            if (!m_path_edit)
            {
                return;
            }

            const std::wstring current_text = GetRawPathFieldText();
            m_ntfs_drive_entries = EnumerateNtfsDriveEntries();

            SendMessageW(m_path_edit, WM_SETREDRAW, FALSE, 0);
            SendMessageW(m_path_edit, CB_RESETCONTENT, 0, 0);
            int selected_index = -1;
            std::wstring display_text = current_text;
            for (const DriveListEntry& entry : m_ntfs_drive_entries)
            {
                const int item_index = static_cast<int>(SendMessageW(
                    m_path_edit,
                    CB_ADDSTRING,
                    0,
                    reinterpret_cast<LPARAM>(entry.display_text.c_str())));
                if (current_text == entry.display_text || current_text == entry.root_path)
                {
                    selected_index = item_index;
                    display_text = entry.display_text;
                }
            }
            if (selected_index >= 0)
            {
                SendMessageW(
                    m_path_edit,
                    CB_SETCURSEL,
                    static_cast<WPARAM>(selected_index),
                    0);
            }
            SetWindowTextW(m_path_edit, display_text.c_str());
            SendMessageW(m_path_edit, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(m_path_edit, nullptr, TRUE);
        }

        void ShowPathDropdown()
        {
            if (!m_path_edit || !IsWindowEnabled(m_path_edit))
            {
                return;
            }

            RefreshNtfsDriveDropdown();
            SendMessageW(m_path_edit, CB_SHOWDROPDOWN, TRUE, 0);
        }

        void HidePathDropdown()
        {
            if (m_path_edit)
            {
                SendMessageW(m_path_edit, CB_SHOWDROPDOWN, FALSE, 0);
            }
        }

        void CommitVisiblePathDropdownSelection()
        {
            if (!m_path_edit || !SendMessageW(m_path_edit, CB_GETDROPPEDSTATE, 0, 0))
            {
                return;
            }

            const LRESULT selected_index = SendMessageW(m_path_edit, CB_GETCURSEL, 0, 0);
            if (selected_index == CB_ERR)
            {
                return;
            }

            const LRESULT text_length = SendMessageW(m_path_edit, CB_GETLBTEXTLEN, selected_index, 0);
            if (text_length == CB_ERR || text_length < 0)
            {
                return;
            }

            std::wstring selected_text(static_cast<size_t>(text_length) + 1, L'\0');
            SendMessageW(
                m_path_edit,
                CB_GETLBTEXT,
                static_cast<WPARAM>(selected_index),
                reinterpret_cast<LPARAM>(selected_text.data()));
            selected_text.resize(static_cast<size_t>(text_length));
            SetWindowTextW(m_path_edit, selected_text.c_str());
        }

        void UpdateTreemapActionControlsEnabled(bool controls_enabled = true)
        {
            const bool enabled = controls_enabled && !m_scan_in_progress && m_tree != nullptr;
            if (m_home_button)
            {
                EnableWindow(m_home_button, enabled);
            }
            if (m_clear_button)
            {
                EnableWindow(m_clear_button, enabled && !m_expanded_reveal_nodes.empty());
            }
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
            UpdateTreemapActionControlsEnabled(enabled);
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
            if (m_help_button)
            {
                EnableWindow(m_help_button, TRUE);
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

        void ClearHoverStateAndInvalidate()
        {
            if (m_hovered_index == -1 &&
                m_hovered_breadcrumb_index == -1 &&
                IsRenderRectEmpty(m_tooltip_redraw_bounds))
            {
                return;
            }

            const int old_hovered_index = m_hovered_index;
            const int old_hovered_breadcrumb_index = m_hovered_breadcrumb_index;
            const RenderRect old_tooltip_bounds = m_tooltip_redraw_bounds;
            ClearHoverState();
            InvalidateHoverTransition(
                old_hovered_index,
                old_hovered_breadcrumb_index,
                old_tooltip_bounds);
        }

        void RebuildAndRedraw()
        {
            RebuildLayout();
            UpdateTreemapActionControlsEnabled();
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }

        void SaveCurrentAppSettings() const
        {
            AppSettings settings;
            settings.SetMaxDepth(m_max_depth);
            settings.SetShowTooltip(m_show_tooltip);
            settings.SetShowStatusBar(m_show_status_bar);
            settings.SetLastScanPath(m_last_scan_path);
            SaveAppSettings(settings);
        }

        void ApplyDepthLimit(int depth_limit)
        {
            const int clamped_depth_limit = std::clamp(depth_limit, 1, 32);
            if (clamped_depth_limit == m_max_depth)
            {
                return;
            }

            m_max_depth = clamped_depth_limit;
            SaveCurrentAppSettings();
            m_expanded_reveal_nodes.clear();
            if (m_tree && !m_scan_in_progress)
            {
                RebuildAndRedraw();
            }
        }

        void GoToScanRoot()
        {
            if (!m_tree || m_scan_in_progress)
            {
                return;
            }

            m_view_root_id = m_tree->RootId();
            RebuildAndRedraw();
        }

        void ClearRememberedExpandedFolders()
        {
            if (!m_tree || m_scan_in_progress || m_expanded_reveal_nodes.empty())
            {
                return;
            }

            m_expanded_reveal_nodes.clear();
            RebuildAndRedraw();
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

            const std::wstring path = NormalizeScanPath(TrimWhitespace(GetPathFromEdit()));
            if (path.empty())
            {
                MessageBoxW(m_hwnd, L"Please enter a folder path to scan.", L"SwiftDisk", MB_OK | MB_ICONINFORMATION);
                return;
            }

            m_options.path = path;
            m_last_scan_path = m_options.path;
            SaveCurrentAppSettings();
            StartScan();
        }

        static const wchar_t* HelpWindowClassName()
        {
            return L"SwiftDiskHelpWindow";
        }

        static std::wstring BuildHelpText()
        {
            return
                L"About SwiftDisk\r\n"
                L"SwiftDisk helps you find what is using disk space by scanning a drive or folder and showing the result as an interactive treemap. It does this very quickly by bypassing the operating system and instead reading the information directly from the drive's Master File Table file.\r\n"
                L"\r\n"
                L"Choose and scan\r\n"
                L"- Use the path field to choose a drive, or type or paste a folder path.\r\n"
                L"- Browse... opens a folder picker and puts the selected folder in the path field.\r\n"
                L"- Scan starts scanning the selected path. You can also press Enter while the path field has focus.\r\n"
                L"- The last scanned path is remembered for the next time you open the app, but it is not scanned automatically.\r\n"
                L"\r\n"
                L"Why the scan is fast\r\n"
                L"- On NTFS drives, SwiftDisk is very fast because it reads the NTFS MFT instead of walking every folder one by one.\r\n"
                L"- The MFT, or Master File Table, is NTFS's own index of files and folders. Reading it lets SwiftDisk collect the file tree with far less directory enumeration work.\r\n"
                L"- Non-NTFS paths use normal directory scanning.\r\n"
                L"\r\n"
                L"Treemap basics\r\n"
                L"- Each rectangle represents a file or folder. Larger rectangles use more disk space.\r\n"
                L"- Folder tiles can contain smaller child rectangles. File tiles are leaves and do not contain children.\r\n"
                L"- The colored parent folder bars at the top of the treemap show the folder path you have drilled into.\r\n"
                L"\r\n"
                L"Navigating the treemap\r\n"
                L"- Left-click a folder tile to drill into that folder.\r\n"
                L"- Left-click a file tile to drill into its containing folder when that file is inside a deeper visible folder.\r\n"
                L"- Left-click a parent folder bar to go back up to that folder.\r\n"
                L"- Press Backspace or Escape to go up one folder level.\r\n"
                L"- Press Home to return to the root of the current scan.\r\n"
                L"\r\n"
                L"Right-click actions\r\n"
                L"- Right-click a tile or parent folder bar to open the context menu for that path.\r\n"
                L"- Files can be opened directly or with Open with....\r\n"
                L"- Files and folders can be opened in File Explorer, copied as a full path, or shown in Properties.\r\n"
                L"\r\n"
                L"Depth and expanded tiles\r\n"
                L"- The Depth value limits how many levels SwiftDisk expands automatically.\r\n"
                L"- If you click a folder that is deeper than the current depth limit, SwiftDisk remembers that path as intentionally expanded.\r\n"
                L"- Remembered expanded folders get one extra reveal level when they appear inside an ancestor view, so the path you drilled into remains easier to follow.\r\n"
                L"- Changing Depth, starting a fresh scan, or using Clear removes those remembered expanded folders.\r\n"
                L"\r\n"
                L"Display options\r\n"
                L"- Tooltip shows the full path and size for the tile or parent folder bar under the mouse.\r\n"
                L"- Status bar shows scan and selection details at the bottom of the window.\r\n"
                L"- Depth controls how many folder levels are visible at once. Lower values keep the view simpler; higher values show more nested folders.\r\n"
                L"- Home returns to the root of the current scan without clearing remembered expanded folders.\r\n"
                L"- Clear removes remembered expanded folders without changing the current scan.";
        }

        static void LayoutHelpWindowControls(HWND hwnd)
        {
            RECT client = {};
            GetClientRect(hwnd, &client);
            const int width = std::max<LONG>(client.right - client.left, 1);
            const int height = std::max<LONG>(client.bottom - client.top, 1);
            const int padding = 14;
            const int button_width = 92;
            const int button_height = 28;
            const int button_top = std::max(padding, height - padding - button_height);
            const int edit_bottom = std::max(padding, button_top - padding);

            HWND help_text = GetDlgItem(hwnd, IDC_HELP_TEXT);
            if (help_text)
            {
                MoveWindow(
                    help_text,
                    padding,
                    padding,
                    std::max(1, width - (padding * 2)),
                    std::max(1, edit_bottom - padding),
                    TRUE);
            }

            HWND close_button = GetDlgItem(hwnd, IDC_HELP_CLOSE_BUTTON);
            if (close_button)
            {
                MoveWindow(
                    close_button,
                    std::max(padding, width - padding - button_width),
                    button_top,
                    button_width,
                    button_height,
                    TRUE);
            }
        }

        static HelpWindowState* GetHelpWindowState(HWND hwnd)
        {
            return reinterpret_cast<HelpWindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        static void FillHelpWindowRect(HDC hdc, const RECT& rect, COLORREF color)
        {
            HBRUSH brush = CreateSolidBrush(color);
            if (!brush)
            {
                return;
            }

            FillRect(hdc, &rect, brush);
            DeleteObject(brush);
        }

        static void DrawHelpWindowFrame(HDC hdc, const RECT& rect, COLORREF color)
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

        static void DrawHelpWindowButton(const DRAWITEMSTRUCT& draw_item)
        {
            if (!draw_item.hDC || !draw_item.hwndItem)
            {
                return;
            }

            const bool disabled = (draw_item.itemState & ODS_DISABLED) != 0;
            const bool pressed = (draw_item.itemState & ODS_SELECTED) != 0;
            const bool focused = (draw_item.itemState & ODS_FOCUS) != 0;
            const bool hovered = (draw_item.itemState & ODS_HOTLIGHT) != 0;
            RECT rect = draw_item.rcItem;

            COLORREF fill_color = ColorRefFromRgb(kControlSurfaceRed, kControlSurfaceGreen, kControlSurfaceBlue);
            if (pressed)
            {
                fill_color = ColorRefFromRgb(kControlBackplateRed, kControlBackplateGreen, kControlBackplateBlue);
            }
            else if (hovered)
            {
                fill_color = ColorRefFromRgb(kControlHoverRed, kControlHoverGreen, kControlHoverBlue);
            }
            FillHelpWindowRect(draw_item.hDC, rect, fill_color);
            DrawHelpWindowFrame(
                draw_item.hDC,
                rect,
                focused
                    ? ColorRefFromRgb(kControlActiveRed, kControlActiveGreen, kControlActiveBlue)
                    : ColorRefFromRgb(kControlBorderRed, kControlBorderGreen, kControlBorderBlue));

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
            if (pressed)
            {
                OffsetRect(&rect, 1, 1);
            }
            DrawTextW(
                draw_item.hDC,
                text,
                -1,
                &rect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            if (old_font)
            {
                SelectObject(draw_item.hDC, old_font);
            }
        }

        static LRESULT CALLBACK HelpWindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param)
        {
            switch (message)
            {
            case WM_CREATE:
            {
                const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
                const wchar_t* help_text_value = create
                    ? static_cast<const wchar_t*>(create->lpCreateParams)
                    : L"";
                HFONT gui_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
                auto* state = new HelpWindowState();
                state->background_brush = CreateSolidBrush(
                    ColorRefFromRgb(kHeaderPanelRed, kHeaderPanelGreen, kHeaderPanelBlue));
                state->text_background_brush = CreateSolidBrush(
                    ColorRefFromRgb(kControlPanelRed, kControlPanelGreen, kControlPanelBlue));
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

                HWND help_text = CreateWindowExW(
                    0,
                    L"EDIT",
                    help_text_value,
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                    0,
                    0,
                    0,
                    0,
                    hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_HELP_TEXT)),
                    nullptr,
                    nullptr);
                if (help_text)
                {
                    SendMessageW(help_text, WM_SETFONT, reinterpret_cast<WPARAM>(gui_font), TRUE);
                    SendMessageW(help_text, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
                    SendMessageW(help_text, EM_SETSEL, 0, 0);
                }

                HWND close_button = CreateWindowExW(
                    0,
                    L"BUTTON",
                    L"Close",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
                    0,
                    0,
                    0,
                    0,
                    hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_HELP_CLOSE_BUTTON)),
                    nullptr,
                    nullptr);
                if (close_button)
                {
                    SendMessageW(close_button, WM_SETFONT, reinterpret_cast<WPARAM>(gui_font), TRUE);
                }

                LayoutHelpWindowControls(hwnd);
                return 0;
            }
            case WM_ERASEBKGND:
            {
                HelpWindowState* state = GetHelpWindowState(hwnd);
                RECT client = {};
                GetClientRect(hwnd, &client);
                FillRect(
                    reinterpret_cast<HDC>(w_param),
                    &client,
                    state && state->background_brush
                        ? state->background_brush
                        : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
                return 1;
            }
            case WM_SIZE:
                LayoutHelpWindowControls(hwnd);
                return 0;
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLOREDIT:
            {
                HDC hdc = reinterpret_cast<HDC>(w_param);
                HWND control = reinterpret_cast<HWND>(l_param);
                if (control == GetDlgItem(hwnd, IDC_HELP_TEXT))
                {
                    HelpWindowState* state = GetHelpWindowState(hwnd);
                    SetTextColor(hdc, ColorRefFromRgb(kControlTextRed, kControlTextGreen, kControlTextBlue));
                    SetBkColor(hdc, ColorRefFromRgb(kControlPanelRed, kControlPanelGreen, kControlPanelBlue));
                    SetBkMode(hdc, OPAQUE);
                    return reinterpret_cast<LRESULT>(
                        state && state->text_background_brush
                            ? state->text_background_brush
                            : GetStockObject(BLACK_BRUSH));
                }
                break;
            }
            case WM_DRAWITEM:
            {
                const auto* draw_item = reinterpret_cast<const DRAWITEMSTRUCT*>(l_param);
                if (draw_item && draw_item->CtlID == IDC_HELP_CLOSE_BUTTON)
                {
                    DrawHelpWindowButton(*draw_item);
                    return TRUE;
                }
                break;
            }
            case WM_COMMAND:
                if (LOWORD(w_param) == IDC_HELP_CLOSE_BUTTON ||
                    LOWORD(w_param) == IDOK ||
                    LOWORD(w_param) == IDCANCEL)
                {
                    DestroyWindow(hwnd);
                    return 0;
                }
                break;
            case WM_CLOSE:
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
            {
                HelpWindowState* state = GetHelpWindowState(hwnd);
                if (state)
                {
                    if (state->background_brush)
                    {
                        DeleteObject(state->background_brush);
                    }
                    if (state->text_background_brush)
                    {
                        DeleteObject(state->text_background_brush);
                    }
                    delete state;
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                }
                return 0;
            }
            default:
                break;
            }

            return DefWindowProcW(hwnd, message, w_param, l_param);
        }

        static bool RegisterHelpWindowClass(HINSTANCE instance)
        {
            WNDCLASSW window_class = {};
            window_class.lpfnWndProc = &MainWindow::HelpWindowProc;
            window_class.hInstance = instance;
            window_class.lpszClassName = HelpWindowClassName();
            window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            window_class.hbrBackground = nullptr;

            if (RegisterClassW(&window_class))
            {
                return true;
            }

            return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }

        void ShowHelpWindow() const
        {
            HINSTANCE instance = GetModuleHandleW(nullptr);
            if (!RegisterHelpWindowClass(instance))
            {
                return;
            }

            const std::wstring help_text = BuildHelpText();
            HWND help_window = CreateWindowExW(
                WS_EX_DLGMODALFRAME,
                HelpWindowClassName(),
                L"SwiftDisk Help",
                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                680,
                560,
                m_hwnd,
                nullptr,
                instance,
                const_cast<wchar_t*>(help_text.c_str()));
            if (!help_window)
            {
                return;
            }

            EnableWindow(m_hwnd, FALSE);
            ShowWindow(help_window, SW_SHOW);
            UpdateWindow(help_window);

            MSG message = {};
            BOOL get_message_result = 0;
            while (IsWindow(help_window) && (get_message_result = GetMessageW(&message, nullptr, 0, 0)) > 0)
            {
                if (!IsDialogMessageW(help_window, &message))
                {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }

            if (IsWindow(m_hwnd))
            {
                EnableWindow(m_hwnd, TRUE);
                SetActiveWindow(m_hwnd);
            }

            if (get_message_result == 0)
            {
                PostQuitMessage(static_cast<int>(message.wParam));
            }
        }

        void BeginScanFromPathShortcut()
        {
            CommitVisiblePathDropdownSelection();
            HidePathDropdown();
            BeginScanFromUiPath();
        }

        void OnCommand(int control_id, int notification_code, HWND control)
        {
            if (control_id == IDC_PATH_EDIT && control == m_path_edit)
            {
                if (notification_code == CBN_DROPDOWN)
                {
                    return;
                }

                if (notification_code == CBN_SETFOCUS)
                {
                    return;
                }

                if (notification_code == CBN_SELCHANGE)
                {
                    return;
                }
            }

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
                SaveCurrentAppSettings();
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
                SaveCurrentAppSettings();
                RebuildLayout();
                InvalidateCanvas();
                return;
            }

            if (control_id == IDC_SCAN_BUTTON && control == m_scan_button)
            {
                HidePathDropdown();
                BeginScanFromUiPath();
                return;
            }

            if (control_id == IDC_HOME_BUTTON && control == m_home_button)
            {
                HidePathDropdown();
                GoToScanRoot();
                SetFocus(m_hwnd);
                return;
            }

            if (control_id == IDC_CLEAR_BUTTON && control == m_clear_button)
            {
                HidePathDropdown();
                ClearRememberedExpandedFolders();
                SetFocus(m_hwnd);
                return;
            }

            if (control_id == IDC_HELP_BUTTON && control == m_help_button)
            {
                HidePathDropdown();
                ShowHelpWindow();
                return;
            }

            if (control_id == IDC_BROWSE_BUTTON && control == m_browse_button)
            {
                HidePathDropdown();
                if (m_scan_in_progress)
                {
                    return;
                }

                std::wstring selected_path;
                if (BrowseForFolder(&selected_path) && m_path_edit)
                {
                    SendMessageW(m_path_edit, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
                    SetWindowTextW(m_path_edit, selected_path.c_str());
                    SetFocus(m_path_combo_edit ? m_path_combo_edit : m_path_edit);
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
            m_has_volume_space_info = false;
            m_scanned_path_is_volume_root = false;
            m_volume_free_bytes = 0;
            m_volume_total_bytes = 0;
            m_volume_root_path.clear();
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

                    VolumeSpaceInfo volume_info;
                    if (TryGetVolumeSpaceInfo(path, &volume_info))
                    {
                        payload->has_volume_space_info = true;
                        payload->scanned_path_is_volume_root = volume_info.path_is_volume_root;
                        payload->volume_free_bytes = volume_info.free_bytes;
                        payload->volume_total_bytes = volume_info.total_bytes;
                        payload->volume_root_path = std::move(volume_info.root_path);
                    }

                    Stopwatch timer;
                    std::wstring error;
                    ntfs::MftScanStats stats;
                    bool ok = false;

                    if (IsNtfsPath(path))
                    {
                        ok = ntfs::ScanPathMft(path, payload->tree.get(), &error, &stats);
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
            m_has_volume_space_info = payload->has_volume_space_info;
            m_scanned_path_is_volume_root = payload->scanned_path_is_volume_root;
            m_volume_free_bytes = payload->volume_free_bytes;
            m_volume_total_bytes = payload->volume_total_bytes;
            m_volume_root_path = std::move(payload->volume_root_path);
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
            const float outer_padding = ScaleForDpi(kOuterPadding, dpi_scale);
            const float header_height = ScaleForDpi(kHeaderHeight, dpi_scale);
            const float status_bar_height = ScaleForDpi(kStatusBarHeight, dpi_scale);
            const float status_bar_gap = ScaleForDpi(kStatusBarGap, dpi_scale);
            const float status_bar_bottom_padding = ScaleForDpi(kStatusBarBottomPadding, dpi_scale);
            const float status_bar_top = std::max(
                outer_padding + header_height + ScaleForDpi(1.0f, dpi_scale),
                height - status_bar_bottom_padding - status_bar_height);

            if (m_show_status_bar)
            {
                m_status_bar_rect = MakeRenderRect(
                    outer_padding,
                    status_bar_top,
                    std::max(width - outer_padding, outer_padding + ScaleForDpi(1.0f, dpi_scale)),
                    std::max(status_bar_top + status_bar_height, status_bar_top + ScaleForDpi(1.0f, dpi_scale)));
            }
            else
            {
                m_status_bar_rect = MakeRenderRect(0, 0, 0, 0);
            }

            m_canvas_rect = MakeRenderRect(
                outer_padding,
                outer_padding + header_height,
                std::max(width - outer_padding, outer_padding + ScaleForDpi(1.0f, dpi_scale)),
                std::max(
                    (m_show_status_bar ? status_bar_top - status_bar_gap : height - outer_padding),
                    outer_padding + header_height + ScaleForDpi(1.0f, dpi_scale)));
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
            m_layout_options.max_rectangles = 100000;
            m_layout_options.max_children_per_directory = 0;
            m_layout_options.minimum_child_ratio = 0.0;
            m_layout_options.expanded_node_ids.assign(m_expanded_reveal_nodes.begin(), m_expanded_reveal_nodes.end());
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
            if ((GetKeyState(VK_RBUTTON) & 0x8000) != 0)
            {
                ClearHoverStateAndInvalidate();
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return;
            }

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
            HidePathDropdown();
            SetFocus(m_hwnd);
            if (m_suppress_next_left_button_down)
            {
                m_suppress_next_left_button_down = false;
                return;
            }

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
            if (rect.node_id == 0 || rect.node_id == m_view_root_id)
            {
                return;
            }

            const Node* node = m_tree->GetNode(rect.node_id);
            if (!node)
            {
                return;
            }

            if (node->is_dir)
            {
                const size_t clicked_depth = GetAbsoluteNodeDepth(rect.node_id);
                const size_t expanded_ancestor_count = CountExpandedRevealAncestorsExclusive(rect.node_id);
                if (clicked_depth >= static_cast<size_t>(m_max_depth) + expanded_ancestor_count)
                {
                    m_expanded_reveal_nodes.insert(rect.node_id);
                }
                m_view_root_id = rect.node_id;
                RebuildAndRedraw();
                return;
            }

            const Node* parent_node = m_tree->GetNode(node->parent_id);
            if (!parent_node || !parent_node->is_dir || parent_node->id == m_view_root_id)
            {
                return;
            }

            m_view_root_id = parent_node->id;
            RebuildAndRedraw();
        }

        void OnRightButtonDown()
        {
            HidePathDropdown();
            SetFocus(m_hwnd);
            ClearHoverStateAndInvalidate();
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        }

        void OnRightButtonUp(int x, int y)
        {
            HidePathDropdown();
            SetFocus(m_hwnd);

            const int breadcrumb_index = HitTestBreadcrumb(static_cast<float>(x), static_cast<float>(y));
            if (breadcrumb_index >= 0)
            {
                ShowNodeContextMenu(
                    m_breadcrumbs[static_cast<size_t>(breadcrumb_index)].node_id,
                    x,
                    y);
                return;
            }

            const int hit_index = HitTestTreemap(static_cast<float>(x), static_cast<float>(y));
            if (hit_index < 0 || !m_tree)
            {
                return;
            }

            const visualization::TreemapRect& rect = m_rects[static_cast<size_t>(hit_index)];
            ShowNodeContextMenu(rect.node_id, x, y);
        }

        void ShowNodeContextMenu(uint64_t node_id, int x, int y)
        {
            if (!m_tree || node_id == 0)
            {
                return;
            }

            const Node* node = m_tree->GetNode(node_id);
            if (!node)
            {
                return;
            }

            std::wstring path = m_tree->BuildPath(node_id);
            if (path.empty())
            {
                return;
            }

            POINT menu_position = { x, y };
            ClientToScreen(m_hwnd, &menu_position);
            ShowTreemapContextMenu(menu_position, path, node->is_dir);
        }

        void ShowTreemapContextMenu(const POINT& menu_position, const std::wstring& path, bool is_dir)
        {
            HMENU menu = CreatePopupMenu();
            if (!menu)
            {
                return;
            }

            AppendMenuW(menu, MF_STRING, IDM_TREEMAP_OPEN_IN_EXPLORER, L"Open in File Explorer");
            AppendMenuW(menu, MF_STRING, IDM_TREEMAP_COPY_FULL_PATH, L"Copy full path");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            if (!is_dir)
            {
                AppendMenuW(menu, MF_STRING, IDM_TREEMAP_OPEN, L"Open");
                AppendMenuW(menu, MF_STRING, IDM_TREEMAP_OPEN_WITH, L"Open with...");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            }
            AppendMenuW(menu, MF_STRING, IDM_TREEMAP_PROPERTIES, L"Properties");

            SetForegroundWindow(m_hwnd);
            const UINT command = TrackPopupMenu(
                menu,
                TPM_RIGHTBUTTON | TPM_RETURNCMD,
                menu_position.x,
                menu_position.y,
                0,
                m_hwnd,
                nullptr);
            DestroyMenu(menu);

            if (command == 0)
            {
                const bool cancelled_with_escape = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
                if (!cancelled_with_escape)
                {
                    m_suppress_next_left_button_down = true;
                }
            }
            else if (command == IDM_TREEMAP_OPEN)
            {
                OpenFile(path);
            }
            else if (command == IDM_TREEMAP_OPEN_WITH)
            {
                OpenFileWithDialog(path);
            }
            else if (command == IDM_TREEMAP_OPEN_IN_EXPLORER)
            {
                OpenPathInFileExplorer(path, is_dir);
            }
            else if (command == IDM_TREEMAP_COPY_FULL_PATH)
            {
                if (!CopyTextToClipboard(path))
                {
                    MessageBoxW(
                        m_hwnd,
                        L"The selected path could not be copied to the clipboard.",
                        L"SwiftDisk",
                        MB_OK | MB_ICONERROR);
                }
            }
            else if (command == IDM_TREEMAP_PROPERTIES)
            {
                OpenPathProperties(path);
            }

            PostMessageW(m_hwnd, WM_NULL, 0, 0);
        }

        void OpenFile(const std::wstring& path) const
        {
            HINSTANCE result = ShellExecuteW(m_hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(result) <= 32)
            {
                MessageBoxW(
                    m_hwnd,
                    L"Windows could not open this file. Try Open with...",
                    L"SwiftDisk",
                    MB_OK | MB_ICONERROR);
            }
        }

        void OpenFileWithDialog(const std::wstring& path) const
        {
            OPENASINFO open_as_info = {};
            open_as_info.pcszFile = path.c_str();
            open_as_info.oaifInFlags = OAIF_EXEC;

            const HRESULT result = SHOpenWithDialog(m_hwnd, &open_as_info);
            if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
            {
                return;
            }

            if (FAILED(result))
            {
                MessageBoxW(
                    m_hwnd,
                    L"Windows could not show Open with for this file.",
                    L"SwiftDisk",
                    MB_OK | MB_ICONERROR);
            }
        }

        void OpenPathInFileExplorer(const std::wstring& path, bool is_dir) const
        {
            HINSTANCE result = nullptr;
            if (is_dir)
            {
                result = ShellExecuteW(m_hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            else
            {
                const std::wstring parameters = L"/select,\"" + path + L"\"";
                result = ShellExecuteW(m_hwnd, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL);
            }

            if (reinterpret_cast<INT_PTR>(result) <= 32)
            {
                MessageBoxW(
                    m_hwnd,
                    L"File Explorer could not open the selected path.",
                    L"SwiftDisk",
                    MB_OK | MB_ICONERROR);
            }
        }

        void OpenPathProperties(const std::wstring& path) const
        {
            SHELLEXECUTEINFOW execute_info = {};
            execute_info.cbSize = sizeof(execute_info);
            execute_info.fMask = SEE_MASK_INVOKEIDLIST;
            execute_info.hwnd = m_hwnd;
            execute_info.lpVerb = L"properties";
            execute_info.lpFile = path.c_str();
            execute_info.nShow = SW_SHOWNORMAL;

            if (!ShellExecuteExW(&execute_info))
            {
                MessageBoxW(
                    m_hwnd,
                    L"Windows could not open the selected path properties.",
                    L"SwiftDisk",
                    MB_OK | MB_ICONERROR);
            }
        }

        bool CopyTextToClipboard(const std::wstring& text) const
        {
            if (!OpenClipboard(m_hwnd))
            {
                return false;
            }

            EmptyClipboard();
            const size_t byte_count = (text.size() + 1) * sizeof(wchar_t);
            HGLOBAL clipboard_memory = GlobalAlloc(GMEM_MOVEABLE, byte_count);
            if (!clipboard_memory)
            {
                CloseClipboard();
                return false;
            }

            void* clipboard_text = GlobalLock(clipboard_memory);
            if (!clipboard_text)
            {
                GlobalFree(clipboard_memory);
                CloseClipboard();
                return false;
            }

            memcpy(clipboard_text, text.c_str(), byte_count);
            GlobalUnlock(clipboard_memory);

            if (!SetClipboardData(CF_UNICODETEXT, clipboard_memory))
            {
                GlobalFree(clipboard_memory);
                CloseClipboard();
                return false;
            }

            CloseClipboard();
            return true;
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
            if (key_code == VK_BACK || key_code == VK_ESCAPE)
            {
                NavigateUp();
            }
            else if (key_code == VK_RETURN)
            {
                BeginScanFromPathShortcut();
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
            state.has_volume_space_info = m_has_volume_space_info;
            state.scanned_path_is_volume_root = m_scanned_path_is_volume_root;
            state.show_tooltip = m_show_tooltip;
            state.show_status_bar = m_show_status_bar;
            state.hovered_index = m_hovered_index;
            state.hovered_breadcrumb_index = m_hovered_breadcrumb_index;
            state.elapsed_seconds = m_elapsed_seconds;
            state.volume_free_bytes = m_volume_free_bytes;
            state.volume_total_bytes = m_volume_total_bytes;
            state.view_root_id = m_view_root_id;
            state.selected_path = m_options.path;
            state.volume_root_path = m_volume_root_path;
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
            state.treemap_group_rect = m_treemap_group_rect;
            state.display_group_rect = m_display_group_rect;
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
        HWND m_path_combo_edit = nullptr;
        HWND m_scan_button = nullptr;
        HWND m_browse_button = nullptr;
        HWND m_home_button = nullptr;
        HWND m_clear_button = nullptr;
        HWND m_help_button = nullptr;
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
        MainWindowPainter m_painter;

        bool m_scan_in_progress = false;
        bool m_has_error = false;
        bool m_has_volume_space_info = false;
        bool m_scanned_path_is_volume_root = false;
        HWND m_hovered_control = nullptr;
        int m_hovered_index = -1;
        int m_hovered_breadcrumb_index = -1;
        int m_max_depth = kDefaultMaxDepth;
        double m_elapsed_seconds = 0.0;
        std::uint64_t m_volume_free_bytes = 0;
        std::uint64_t m_volume_total_bytes = 0;
        uint64_t m_view_root_id = 0;
        std::wstring m_scan_path;
        std::wstring m_volume_root_path;
        std::wstring m_status_text;
        std::wstring m_error_text;
        FileTree::TreeStats m_tree_stats;
        std::unique_ptr<FileTree> m_tree;
        visualization::TreemapLayoutOptions m_layout_options;
        std::vector<visualization::TreemapRect> m_rects;
        std::vector<DriveListEntry> m_ntfs_drive_entries;
        std::unordered_set<uint64_t> m_expanded_reveal_nodes;
        std::vector<BreadcrumbStrip> m_breadcrumbs;
        bool m_show_tooltip = true;
        bool m_show_status_bar = false;
        std::wstring m_last_scan_path;
        bool m_updating_depth_edit = false;
        bool m_tooltip_animation_active = false;
        bool m_suppress_next_left_button_down = false;
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
        RenderRect m_treemap_group_rect = MakeRenderRect(0, 0, 0, 0);
        RenderRect m_display_group_rect = MakeRenderRect(0, 0, 0, 0);
        std::vector<std::pair<HWND, WNDPROC>> m_subclassed_controls;
    };

} // namespace swift_disk::gui

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command)
{
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    swift_disk::gui::GuiOptions options = swift_disk::gui::ParseOptions();
    if (options.show_help)
    {
        MessageBoxW(
            nullptr,
            L"SwiftDisk GUI\n\nUsage:\n  swift_disk_gui.exe --path C:\\\n",
            L"SwiftDisk",
            MB_OK | MB_ICONINFORMATION);
        CoUninitialize();
        return 0;
    }

    swift_disk::gui::MainWindow window(std::move(options));
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
