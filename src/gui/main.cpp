// SPDX-FileCopyrightText: 2026 Per Edlund
// SPDX-License-Identifier: GPL-3.0-or-later

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
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gui/app_settings.h"
#include "gui/direct2d_renderer.h"
#include "gui/info_window.h"
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
    constexpr int IDC_ABOUT_BUTTON = 1013;
    constexpr int IDC_COLOR_BY_FILE_TYPE_CHECK = 1014;
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
        double elapsed_seconds = 0.0;
        std::wstring path;
        std::wstring error;
    };

    struct SharedScanState
    {
        std::mutex mutex;
        // The detached scan worker publishes exactly one result here before it
        // notifies the UI thread with WM_APP_SCAN_COMPLETE or WM_APP_SCAN_FAILED.
        std::unique_ptr<ScanPayload> pending_payload;
    };

    struct DriveListEntry
    {
        std::wstring root_path;
        std::wstring display_text;
    };

    /**
    Removes quote characters from both ends of path text without changing quotes
    that are part of the path itself.

    Parameters:
      path: Path text to trim.

    Returns: Path text without surrounding quotes.
    **/
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

    /**
    Normalizes user-entered scan paths while preserving all characters except
    surrounding quote characters. Bare drive designators are converted to root
    paths so Windows APIs interpret "C:" as C:\\ rather than a drive-relative path.

    Parameters:
      path: Path text from the command line, settings, or path field.

    Returns: The normalized path.
    **/
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

    /**
    Normalizes an ASCII drive letter for consistent path and dropdown display.

    Parameters:
      drive_letter: Drive letter to normalize.

    Returns: The uppercase letter, or the input when it is not lowercase ASCII.
    **/
    wchar_t ToUpperDriveLetter(wchar_t drive_letter)
    {
        if (drive_letter >= L'a' && drive_letter <= L'z')
        {
            return static_cast<wchar_t>(drive_letter - L'a' + L'A');
        }

        return drive_letter;
    }

    /**
    Maps a Windows drive type to the fallback label used when a volume has no
    user-assigned name.

    Parameters:
      drive_type: Value returned by GetDriveTypeW.

    Returns: User-facing drive type name.
    **/
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

    /**
    Enumerates mounted NTFS volumes and builds the paired display and root-path
    values used by the editable drive dropdown.

    Returns: Available NTFS drives in the order reported by Windows.
    **/
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

            // GetLogicalDriveStringsW returns consecutive null-terminated paths
            // followed by an additional null, so advance past this path's null.
            root_path += wcslen(root_path) + 1;
        }

        return entries;
    }

    /**
    Chooses the initial drive-dropdown value, preferring the system drive letter
    when it is NTFS and otherwise falling back to the first available NTFS drive.

    Returns: Display text for the preferred drive, or an empty string.
    **/
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

    /**
    Resolves an arbitrary filesystem path to the root of its containing volume.

    Parameters:
      path: Filesystem path to resolve.
      volume_root: Receives the volume root on success.

    Returns: True when Windows returned a volume root that fit in the buffer.
    **/
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

    /**
    Determines whether a path belongs to an NTFS volume so scan dispatch can use
    the optimized MFT implementation.

    Parameters:
      path: Filesystem path to inspect.

    Returns: True when the containing volume reports the NTFS filesystem.
    **/
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

    /**
    Checks whether a path currently resolves to a directory.

    Parameters:
      path: Filesystem path to inspect.

    Returns: True when the path exists and has the directory attribute.
    **/
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

    /**
    Parses supported command-line options and accepts a positional path for
    compatibility with launching SwiftDisk directly against a folder.

    Returns: Normalized startup options for the main window.
    **/
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

    /**
    Formats a byte count using binary units and two fractional digits.

    Parameters:
      bytes: Byte count to format.

    Returns: Human-readable size from bytes through pebibytes.
    **/
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

    /**
    Converts floating-point render bounds to an enclosing Win32 rectangle. The
    outward rounding ensures invalidation includes every partially covered pixel.

    Parameters:
      rect: Floating-point bounds to convert.

    Returns: Enclosing integer rectangle.
    **/
    RECT RectFToRect(const RenderRect& rect)
    {
        RECT result = {};
        result.left = static_cast<LONG>(std::floor(rect.left));
        result.top = static_cast<LONG>(std::floor(rect.top));
        result.right = static_cast<LONG>(std::ceil(rect.right));
        result.bottom = static_cast<LONG>(std::ceil(rect.bottom));
        return result;
    }

    /**
    Converts a Win32 integer rectangle to renderer coordinates.

    Parameters:
      rect: Integer rectangle to convert.

    Returns: Equivalent floating-point render rectangle.
    **/
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

    /**
    Combines two render rectangles while treating an empty rectangle as an
    identity value for incremental redraw-bound accumulation.

    Parameters:
      left: First rectangle.
      right: Second rectangle.

    Returns: Bounds containing both non-empty rectangles.
    **/
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

    /**
    Expands non-empty render bounds equally in every direction.

    Parameters:
      rect: Rectangle to expand.
      amount: Expansion applied to each edge.

    Returns: Expanded rectangle, or the original empty rectangle.
    **/
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

    /**
    Reads a window's horizontal DPI and converts it to a bounded scale relative
    to the 96-DPI Win32 baseline.

    Parameters:
      hwnd: Window whose DPI should be queried.

    Returns: DPI scale in the supported range, or 1.0 when unavailable.
    **/
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
        /**
        Creates the in-memory main-window controller and restores persisted UI
        preferences. The native window and controls are created separately by
        Create().

        Parameters:
          options: Command-line options that take precedence over saved paths.
        **/
        explicit MainWindow(GuiOptions options)
            : m_options(std::move(options)),
            m_shared_scan_state(std::make_shared<SharedScanState>())
        {
            const AppSettings settings = LoadAppSettings();
            m_max_depth = settings.GetMaxDepth();
            m_show_tooltip = settings.GetShowTooltip();
            m_show_status_bar = settings.GetShowStatusBar();
            m_color_by_file_type = settings.GetColorByFileType();
            m_last_scan_path = NormalizeScanPath(settings.GetLastScanPath());
        }

        /**
        Registers the native window class, creates the top-level window, and
        makes it visible. Child controls are initialized from WM_CREATE.

        Parameters:
          instance: Application module instance used for window resources.
          show_command: Initial window display state supplied by Windows.

        Returns: True when the top-level window was created successfully.
        **/
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

        /**
        Dispatches top-level Win32 messages into the window's scanning, layout,
        painting, input, and resource-lifecycle handlers.

        Parameters:
          message: Win32 message identifier.
          w_param: Message-specific word parameter.
          l_param: Message-specific long parameter.

        Returns: The result required by the handled message or DefWindowProcW.
        **/
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

        /**
        Bridges the static Win32 callback to the MainWindow instance stored on
        the HWND during WM_NCCREATE.

        Parameters:
          hwnd: Window receiving the message.
          message: Win32 message identifier.
          w_param: Message-specific word parameter.
          l_param: Message-specific long parameter.

        Returns: The instance handler result, or the default window procedure
        result before the instance is available.
        **/
        static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param)
        {
            MainWindow* window = nullptr;
            if (message == WM_NCCREATE)
            {
                auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
                window = static_cast<MainWindow*>(create->lpCreateParams);
                window->m_hwnd = hwnd;
                // WM_NCCREATE is the first callback that carries lpCreateParams;
                // retain the controller pointer for every later window message.
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

        /**
        Lazily creates the GDI brushes shared by standard and owner-drawn header
        controls.
        **/
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

        /**
        Releases every GDI brush owned by the window and clears its handle so the
        cleanup remains safe after partial initialization.
        **/
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

        /**
        Supplies text, background, and brush choices for standard controls that
        must blend into the custom dark header.

        Parameters:
          hdc: Device context Windows will use to draw the control.
          control: Control requesting its colors.
          message: WM_CTLCOLOR message identifying the control category.

        Returns: A brush handle for Windows to use, or zero for invalid input.
        **/
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

        /**
        Fills a GDI rectangle with a temporary solid brush.

        Parameters:
          hdc: Destination device context.
          rect: Area to fill.
          color: Fill color.
        **/
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

        /**
        Draws a one-pixel rectangular frame while preserving the device context's
        previously selected pen and brush.

        Parameters:
          hdc: Destination device context.
          rect: Frame bounds.
          color: Frame color.
        **/
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

        /**
        Selects a button fill color using native interaction precedence so
        disabled and pressed states remain unambiguous.

        Parameters:
          pressed: Whether the control is currently pressed.
          disabled: Whether the control is disabled.
          hovered: Whether the pointer is over the control.
          checked: Whether a checkbox-style control is checked.

        Returns: Fill color for the requested state.
        **/
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

        /**
        Selects the muted, normal, or active border used by an owner-drawn control.

        Parameters:
          disabled: Whether the control is disabled.
          hovered: Whether the pointer is over the control.
          checked: Whether a checkbox-style control is checked.

        Returns: Border color for the requested state.
        **/
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

        /**
        Draws an owner-drawn button with the shared header palette while
        preserving native pressed, disabled, and keyboard-focus feedback.

        Parameters:
          draw_item: Win32 owner-draw state and target device context.
        **/
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

        /**
        Draws an owner-drawn checkbox whose checked value is backed by the
        corresponding MainWindow setting instead of native button state alone.

        Parameters:
          draw_item: Win32 owner-draw state and target device context.
        **/
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
            else if (draw_item.hwndItem == m_color_by_file_type_check)
            {
                checked = m_color_by_file_type;
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

        /**
        Routes WM_DRAWITEM requests to the custom button or checkbox renderer for
        controls owned by MainWindow.

        Parameters:
          draw_item: Owner-draw request from Windows.

        Returns: TRUE when the control was drawn, otherwise FALSE.
        **/
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
            case IDC_ABOUT_BUTTON:
            case IDC_DEPTH_UP_BUTTON:
            case IDC_DEPTH_DOWN_BUTTON:
                DrawCustomButton(*draw_item);
                return TRUE;
            case IDC_SHOW_TOOLTIP_CHECK:
            case IDC_COLOR_BY_FILE_TYPE_CHECK:
            case IDC_SHOW_STATUS_BAR_CHECK:
                DrawCustomCheckbox(*draw_item);
                return TRUE;
            default:
                return FALSE;
            }
        }

        /**
        Synchronizes a checkbox's native accessibility state with the setting
        stored by MainWindow and schedules it for repainting.

        Parameters:
          checkbox: Checkbox control to update.
          checked: New checked state.
        **/
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

        /**
        Determines whether a child control participates in shared hover tracking
        and themed invalidation.

        Parameters:
          control: Child control to classify.

        Returns: True when MainWindow subclasses and tracks the control.
        **/
        bool IsTrackedControl(HWND control) const
        {
            return control == m_path_edit ||
                control == m_path_combo_edit ||
                control == m_scan_button ||
                control == m_browse_button ||
                control == m_home_button ||
                control == m_help_button ||
                control == m_clear_button ||
                control == m_about_button ||
                control == m_depth_edit ||
                control == m_depth_up_button ||
                control == m_depth_down_button ||
                control == m_show_tooltip_check ||
                control == m_show_status_bar_check ||
                control == m_color_by_file_type_check;
        }

        bool IsEditControl(HWND control) const
        {
            return control == m_path_combo_edit || control == m_depth_edit;
        }

        bool IsPathFieldControl(HWND control) const
        {
            return control == m_path_edit || control == m_path_combo_edit;
        }

        /**
        Invalidates a themed control after an interaction-state change, including
        the non-client frame used by edit controls.

        Parameters:
          control: Tracked control to repaint.
        **/
        void InvalidateTrackedControl(HWND control) const
        {
            if (!control || !IsTrackedControl(control))
            {
                return;
            }

            if (IsEditControl(control))
            {
                // Edit controls draw their non-client border separately, so a
                // frame redraw is required for hover changes to reach it.
                RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
                return;
            }

            InvalidateRect(control, nullptr, TRUE);
        }

        /**
        Changes the currently hovered header control and repaints both the old and
        new controls so their themed backgrounds update immediately.

        Parameters:
          control: New hovered control, or null to clear hover state.
        **/
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

        /**
        Marks a child control as hovered and asks Windows for a matching
        WM_MOUSELEAVE notification.

        Parameters:
          control: Child control currently under the pointer.
        **/
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

        /**
        Fills the four bands between an edit control's client bounds and its inset
        text-formatting rectangle.

        Parameters:
          hdc: Destination device context.
          client: Full edit client rectangle.
          text_rect: Rectangle used by native text painting.
          color: Padding background color.
        **/
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

        /**
        Repaints the area outside an edit control's formatting rectangle. Native
        edit painting only fills the text rectangle, which otherwise leaves the
        custom hover background incomplete around the inset text.

        Parameters:
          control: Subclassed edit control whose padding should be painted.
        **/
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

        /**
        Looks up the native procedure saved when a child control was subclassed.

        Parameters:
          control: Subclassed child control.

        Returns: Original procedure, or null when the control is not registered.
        **/
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

        /**
        Bridges a subclassed child-control procedure back to its MainWindow so
        controls can share hover tracking, keyboard shortcuts, and edit painting.

        Parameters:
          hwnd: Child control receiving the message.
          message: Win32 message identifier.
          w_param: Message-specific word parameter.
          l_param: Message-specific long parameter.

        Returns: The result from the instance control handler or default procedure.
        **/
        static LRESULT CALLBACK ControlWindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param)
        {
            auto* window = reinterpret_cast<MainWindow*>(GetPropW(hwnd, kControlWindowPropName));
            if (!window)
            {
                return DefWindowProcW(hwnd, message, w_param, l_param);
            }

            return window->HandleControlMessage(hwnd, message, w_param, l_param);
        }

        /**
        Adds application-specific behavior around a subclassed control and then
        delegates unhandled behavior to that control's original window procedure.

        Parameters:
          control: Child control receiving the message.
          message: Win32 message identifier.
          w_param: Message-specific word parameter.
          l_param: Message-specific long parameter.

        Returns: The handled result or the original control-procedure result.
        **/
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

        /**
        Subclasses one child control and records its original procedure so hover
        behavior can be added without losing the control's native behavior.

        Parameters:
          control: Child control to subclass.
        **/
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

        /**
        Installs the shared child-control subclass on every interactive header
        control that participates in custom hover or keyboard behavior.
        **/
        void CreateControlHoverTracking()
        {
            AddControlHoverTracking(m_path_edit);
            AddControlHoverTracking(m_path_combo_edit);
            AddControlHoverTracking(m_scan_button);
            AddControlHoverTracking(m_browse_button);
            AddControlHoverTracking(m_home_button);
            AddControlHoverTracking(m_help_button);
            AddControlHoverTracking(m_clear_button);
            AddControlHoverTracking(m_about_button);
            AddControlHoverTracking(m_depth_edit);
            AddControlHoverTracking(m_depth_up_button);
            AddControlHoverTracking(m_depth_down_button);
            AddControlHoverTracking(m_show_tooltip_check);
            AddControlHoverTracking(m_show_status_bar_check);
            AddControlHoverTracking(m_color_by_file_type_check);
        }

        /**
        Restores every subclassed control's original procedure before the main
        window releases its controller and associated properties.
        **/
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

        /**
        Applies DPI-scaled text padding to an edit control by updating both its
        formatting rectangle and horizontal margins.

        Parameters:
          edit: Edit control to update.
          left_padding: Left inset in physical pixels.
          top_padding: Top inset in physical pixels.
          right_padding: Right inset in physical pixels.
          bottom_padding: Bottom inset in physical pixels.
        **/
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

        /**
        Measures a control's current label using its assigned font and device
        context.

        Parameters:
          control: Control whose label should be measured.

        Returns: Label width in physical pixels, or zero when unavailable.
        **/
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

        /**
        Calculates a checkbox width from its box, label, DPI-scaled spacing, and
        caller-provided minimum.

        Parameters:
          checkbox: Checkbox whose label should be measured.
          minimum_width: Lower width bound in physical pixels.
          dpi_scale: Current window DPI scale.

        Returns: Required checkbox width in physical pixels.
        **/
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

        /**
        Selects the initial path-field value using command-line path, valid saved
        path, and default NTFS drive precedence.

        Returns: Text to place in the path field at startup.
        **/
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

        /**
        Creates the complete native header control set, applies the shared font
        and persisted state, installs child-control subclassing, and performs the
        initial layout.
        **/
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

            m_about_button = CreateWindowExW(
                0,
                L"BUTTON",
                L"About",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0,
                0,
                0,
                0,
                m_hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ABOUT_BUTTON)),
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
            m_color_by_file_type_check = CreateWindowExW(
                0,
                L"BUTTON",
                L"Color by file type",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_CHECKBOX | BS_OWNERDRAW,
                0,
                0,
                0,
                0,
                m_hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_COLOR_BY_FILE_TYPE_CHECK)),
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
            if (m_about_button)
            {
                SendMessageW(m_about_button, WM_SETFONT, reinterpret_cast<WPARAM>(gui_font), TRUE);
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
            if (m_color_by_file_type_check)
            {
                SendMessageW(
                    m_color_by_file_type_check,
                    WM_SETFONT,
                    reinterpret_cast<WPARAM>(gui_font),
                    TRUE);
                SetCheckboxState(m_color_by_file_type_check, m_color_by_file_type);
            }

            CreateControlHoverTracking();
            UpdateHeaderControlsLayout();
        }

        /**
        Calculates the minimum width needed by both header control rows using
        measured label widths and the same gaps used by the layout routine.

        Parameters:
          dpi_scale: Current window DPI scale.

        Returns: Required control-area width in physical pixels.
        **/
        int GetRequiredHeaderControlAreaWidth(float dpi_scale) const
        {
            const int group_box_horizontal_padding = ScaleIntForDpi(14.0f, dpi_scale);
            const int action_button_horizontal_padding = ScaleIntForDpi(14.0f, dpi_scale);
            const int action_button_width = std::max({
                GetButtonControlWidth(m_browse_button, 0, action_button_horizontal_padding),
                GetButtonControlWidth(m_scan_button, 0, action_button_horizontal_padding),
                GetButtonControlWidth(m_home_button, 0, action_button_horizontal_padding),
                GetButtonControlWidth(m_help_button, 0, action_button_horizontal_padding),
                GetButtonControlWidth(m_clear_button, 0, action_button_horizontal_padding),
                GetButtonControlWidth(m_about_button, 0, action_button_horizontal_padding)
            });
            const int button_gap = group_box_horizontal_padding;
            const int scan_row_width =
                ScaleIntForDpi(2.0f, dpi_scale) +
                ScaleIntForDpi(180.0f, dpi_scale) +
                button_gap +
                action_button_width +
                button_gap +
                action_button_width;

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
            const int color_by_file_type_width = GetCheckboxControlWidth(m_color_by_file_type_check, 0, dpi_scale);
            const int treemap_control_gap = group_box_horizontal_padding;
            const int treemap_group_width = (group_box_horizontal_padding * 2) +
                depth_group_width + treemap_control_gap + action_button_width + button_gap + action_button_width +
                treemap_control_gap + color_by_file_type_width - checkbox_trailing_padding;
            const int lower_row_width =
                ScaleIntForDpi(2.0f, dpi_scale) +
                display_group_width +
                group_box_horizontal_padding +
                treemap_group_width +
                group_box_horizontal_padding +
                action_button_width +
                button_gap +
                action_button_width;

            return std::max(scan_row_width, lower_row_width);
        }

        /**
        Combines the control-area requirement with the summary area and outer
        padding to determine the smallest usable client width.

        Parameters:
          dpi_scale: Current window DPI scale.

        Returns: Minimum main-window client width in physical pixels.
        **/
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

        /**
        Converts the minimum usable client width to a framed window width and
        applies it to native resize tracking.

        Parameters:
          info: WM_GETMINMAXINFO structure to update.
        **/
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

        /**
        Measures and positions all header controls for the current client width
        and DPI while keeping the summary, scan, display, and treemap groups from
        overlapping.
        **/
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
                GetButtonControlWidth(m_clear_button, 0, action_button_horizontal_padding),
                GetButtonControlWidth(m_about_button, 0, action_button_horizontal_padding)
            });
            const int browse_width = action_button_width;
            const int scan_width = action_button_width;
            const int home_width = action_button_width;
            const int help_width = action_button_width;
            const int clear_width = action_button_width;
            const int about_width = action_button_width;
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
            const int color_by_file_type_width = GetCheckboxControlWidth(m_color_by_file_type_check, 0, dpi_scale);
            const int treemap_control_gap = group_box_horizontal_padding;
            const int desired_treemap_group_width = (group_box_horizontal_padding * 2) +
                depth_group_width + treemap_control_gap + home_width + button_gap + clear_width +
                treemap_control_gap + color_by_file_type_width - checkbox_trailing_padding;
            const int required_control_area_width = GetRequiredHeaderControlAreaWidth(dpi_scale);
            const int control_area_width = std::clamp(
                std::max(nominal_control_area_width, required_control_area_width),
                min_control_area_width,
                max_control_area_width);
            const int control_left = control_area_right - control_area_width;
            const int scan_group_left = control_left + ScaleIntForDpi(2.0f, dpi_scale);
            const int scan_group_top_inner = group_top + ScaleIntForDpi(10.0f, dpi_scale);
            const int browse_left = control_left + control_area_width - browse_width;
            const int scan_left = browse_left - button_gap - scan_width;
            const int edit_width = std::max(
                ScaleIntForDpi(180.0f, dpi_scale),
                scan_left - button_gap - scan_group_left);
            const int lower_row_left = control_left + ScaleIntForDpi(2.0f, dpi_scale);
            const int display_group_left = lower_row_left;
            const int treemap_group_left = display_group_left + display_group_width + lower_group_gap;
            const int about_left = browse_left;
            const int help_left = about_left - button_gap - help_width;
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
                // A CBS_DROPDOWN window's height includes its expanded list, not
                // only the visible edit row.
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
            if (m_about_button)
            {
                MoveWindow(m_about_button, about_left, depth_controls_top, about_width, ScaleIntForDpi(28.0f, dpi_scale), TRUE);
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
            if (m_color_by_file_type_check)
            {
                MoveWindow(
                    m_color_by_file_type_check,
                    clear_left + clear_width + treemap_control_gap,
                    show_tooltip_top,
                    color_by_file_type_width,
                    ScaleIntForDpi(18.0f, dpi_scale),
                    TRUE);
            }
        }

        /**
        Reads the visible text from the editable path combo box, falling back to
        startup options before the control exists.

        Returns: Unmapped text currently shown in the path field.
        **/
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

        /**
        Converts drive-dropdown display text back to its filesystem root while
        leaving custom user-entered paths unchanged.

        Returns: Filesystem path represented by the path field.
        **/
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

        /**
        Re-enumerates NTFS drives and repopulates the editable combo box without
        discarding a custom path currently typed by the user.
        **/
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

        /**
        Refreshes the available NTFS drives immediately before opening the path
        combo box so removable and newly mounted volumes are current.
        **/
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

        /**
        Copies the highlighted item from an open dropdown into the combo edit.
        This makes an immediate Enter key scan use the visible selection even
        before the combo box has emitted its normal selection commit.
        **/
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

        /**
        Updates Home and Clear availability from scan state, tree availability,
        and whether any explicit folder reveals can be cleared.

        Parameters:
          controls_enabled: Parent enable state imposed by the current operation.
        **/
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

        /**
        Enables or disables controls that can mutate scan or layout state while
        leaving Help and About available during a background scan.

        Parameters:
          enabled: Whether scan-dependent controls may accept input.
        **/
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
            if (m_about_button)
            {
                EnableWindow(m_about_button, TRUE);
            }
            if (m_show_tooltip_check)
            {
                EnableWindow(m_show_tooltip_check, enabled);
            }
            if (m_show_status_bar_check)
            {
                EnableWindow(m_show_status_bar_check, enabled);
            }
            if (m_color_by_file_type_check)
            {
                EnableWindow(m_color_by_file_type_check, enabled);
            }
            if (m_hovered_control && !IsWindowEnabled(m_hovered_control))
            {
                SetHoveredControl(nullptr);
            }
        }

        /**
        Resets every treemap, breadcrumb, pointer, and tooltip field that belongs
        to the current hover interaction and stops tooltip animation.
        **/
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

        /**
        Clears treemap, breadcrumb, and tooltip hover state and invalidates the
        union of their old and new visual bounds.
        **/
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

        /**
        Takes a snapshot of the current persistent UI preferences and last scan
        path and writes it through the application settings helper.
        **/
        void SaveCurrentAppSettings() const
        {
            AppSettings settings;
            settings.SetMaxDepth(m_max_depth);
            settings.SetShowTooltip(m_show_tooltip);
            settings.SetShowStatusBar(m_show_status_bar);
            settings.SetColorByFileType(m_color_by_file_type);
            settings.SetLastScanPath(m_last_scan_path);
            SaveAppSettings(settings);
        }

        /**
        Applies and persists a clamped treemap depth limit. Any remembered
        one-off folder expansions are cleared because they were derived from the
        previous limit.

        Parameters:
          depth_limit: Requested absolute depth in the range 1 through 32.
        **/
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

        /**
        Returns the treemap to the scanned tree root when navigation is available.
        **/
        void GoToScanRoot()
        {
            if (!m_tree || m_scan_in_progress)
            {
                return;
            }

            m_view_root_id = m_tree->RootId();
            RebuildAndRedraw();
        }

        /**
        Removes all per-folder depth extensions and rebuilds the treemap using
        only the configured global depth.
        **/
        void ClearRememberedExpandedFolders()
        {
            if (!m_tree || m_scan_in_progress || m_expanded_reveal_nodes.empty())
            {
                return;
            }

            m_expanded_reveal_nodes.clear();
            RebuildAndRedraw();
        }

        /**
        Transitions a successful scan result to its ready state, resets view
        navigation, re-enables controls, and builds the initial layout.
        **/
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

        /**
        Resets navigation, explicit depth extensions, layout options, geometry,
        breadcrumbs, and hover state for a newly assigned tree.
        **/
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

        /**
        Updates the depth edit and applies the value while suppressing the
        recursive EN_CHANGE notification caused by SetWindowTextW.

        Parameters:
          depth_limit: Requested depth value.
        **/
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

        /**
        Applies a relative depth change using the current valid edit value or the
        last accepted depth when the edit is temporarily invalid.

        Parameters:
          delta: Signed amount to add to the depth limit.
        **/
        void AdjustDepthLimit(int delta)
        {
            int depth_limit = m_max_depth;
            if (!TryGetDepthLimitFromEdit(&depth_limit))
            {
                depth_limit = m_max_depth;
            }

            SetDepthEditValue(depth_limit + delta);
        }

        /**
        Parses the depth edit as a complete decimal value and clamps it to the
        supported range.

        Parameters:
          depth_limit: Receives the parsed value on success.

        Returns: True when the edit contains a valid integer.
        **/
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

        /**
        Shows the modern Windows folder picker, seeded from the current path when
        possible, and extracts a filesystem path from the selected shell item.

        Parameters:
          selected_path: Receives the selected folder path.

        Returns: True when the user selected a filesystem folder.
        **/
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

        /**
        Removes leading and trailing ASCII whitespace accepted around path and
        numeric edit values.

        Parameters:
          value: Text to trim.

        Returns: Trimmed text, or an empty string for all-whitespace input.
        **/
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

        /**
        Validates and normalizes the current path field, persists the accepted
        path, and starts a scan unless one is already running.
        **/
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

        void BeginScanFromPathShortcut()
        {
            CommitVisiblePathDropdownSelection();
            HidePathDropdown();
            BeginScanFromUiPath();
        }

        /**
        Handles header control notifications and keeps persisted settings,
        control state, treemap layout, and focus behavior synchronized.

        Parameters:
          control_id: Application identifier for the source control.
          notification_code: Control-specific WM_COMMAND notification.
          control: Source control window.
        **/
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

            if (control_id == IDC_COLOR_BY_FILE_TYPE_CHECK && control == m_color_by_file_type_check && notification_code == BN_CLICKED)
            {
                m_color_by_file_type = !m_color_by_file_type;
                SetCheckboxState(m_color_by_file_type_check, m_color_by_file_type);
                SaveCurrentAppSettings();
                RebuildAndRedraw();
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
                ShowHelpWindow(m_hwnd);
                return;
            }

            if (control_id == IDC_ABOUT_BUTTON && control == m_about_button)
            {
                HidePathDropdown();
                ShowAboutWindow(m_hwnd);
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

        /**
        Transitions the UI into its scanning state and starts a detached worker
        that selects the NTFS MFT scanner when possible, then publishes one
        payload back to the UI thread.
        **/
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
            // The shared state outlives MainWindow if the user closes the app
            // while this detached worker is still scanning.
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

                    // Publish the payload before posting the message so the UI
                    // can consume a complete result as soon as it handles it.
                    if (IsWindow(hwnd))
                    {
                        PostMessageW(hwnd, ok ? WM_APP_SCAN_COMPLETE : WM_APP_SCAN_FAILED, 0, 0);
                    }
                }
            ).detach();
        }

        /**
        Consumes the worker's pending payload on the UI thread and transitions
        the window to either an error state or a freshly rooted treemap view.

        Parameters:
          failed: Whether the worker posted the failure completion message.
        **/
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
            ApplyFreshScanResultView();
        }

        void DiscardDeviceResources()
        {
            m_painter.DiscardDeviceResources();
        }

        /**
        Repositions controls, resizes existing device resources, rebuilds render
        geometry, and schedules a repaint after a client-size change.
        **/
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

        /**
        Invalidates the treemap canvas together with the status bar when it is
        visible, without forcing an unnecessary full-window repaint.
        **/
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

        /**
        Builds current renderer state and asks the painter to measure the tooltip
        at a proposed anchor position.

        Parameters:
          tooltip_position: Proposed tooltip anchor in client coordinates.

        Returns: Tooltip bounds, or an empty rectangle when tooltips are disabled.
        **/
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

        /**
        Invalidates the union of a tooltip's previous and newly measured bounds
        so animation never leaves stale pixels behind.

        Parameters:
          old_bounds: Tooltip bounds from the previous frame.
          new_position: Tooltip anchor position for the next frame.
        **/
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

        /**
        Converts a treemap rectangle from layout-local coordinates into absolute
        client-space render bounds.

        Parameters:
          rect_index: Index in the current treemap rectangle list.

        Returns: Client-space bounds, or an empty rectangle for an invalid index.
        **/
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

        /**
        Calculates conservative repaint bounds for either a breadcrumb frame or a
        treemap hover outline, including its DPI-scaled visual overflow.

        Parameters:
          rect_index: Hovered treemap rectangle index.
          breadcrumb_index: Hovered breadcrumb index, which takes precedence.

        Returns: Inflated client-space hover bounds.
        **/
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

        /**
        Invalidates every region affected by a hover change: old and new hover
        visuals, tooltip bounds, and the optional status bar.

        Parameters:
          old_hovered_index: Previously hovered treemap rectangle index.
          old_hovered_breadcrumb_index: Previously hovered breadcrumb index.
          old_tooltip_bounds: Tooltip bounds before the state change.
        **/
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

        /**
        Divides the client area below the header into treemap and optional status
        bar rectangles while retaining valid one-pixel bounds in tiny windows.
        **/
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

        /**
        Rebuilds breadcrumb frames and the treemap for the current view root. The
        user-facing depth limit is converted from an absolute scan-tree depth to
        the local depth expected by the treemap layout engine.
        **/
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
            // BuildTreemapLayout counts descendants below its supplied root;
            // subtract the root's absolute depth and the root level itself.
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
            m_layout_options.color_by_file_type = m_color_by_file_type;
            if (m_color_by_file_type && !m_layout_options.file_type_palette)
            {
                m_layout_options.file_type_palette = std::make_shared<const visualization::FileTypePalette>(
                    visualization::BuildFileTypePalette(*m_tree));
            }
            m_rects = visualization::BuildTreemapLayout(*m_tree, m_view_root_id, m_layout_options);
        }

        /**
        Finds the topmost treemap rectangle at a client-space point.

        Parameters:
          x: Horizontal client coordinate.
          y: Vertical client coordinate.

        Returns: Rectangle index, or -1 when no rectangle is hit.
        **/
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
            // Later rectangles are painted over earlier ones, so test them in
            // reverse order to match the visual stacking order.
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

        /**
        Finds the breadcrumb strip containing a client-space point.

        Parameters:
          x: Horizontal client coordinate.
          y: Vertical client coordinate.

        Returns: Breadcrumb index, or -1 when no strip is hit.
        **/
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

        /**
        Tracks treemap and breadcrumb hover transitions, advances the tooltip
        target, and selects a cursor that reflects breadcrumb clickability.

        Parameters:
          x: Horizontal client coordinate.
          y: Vertical client coordinate.
        **/
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

        /**
        Navigates through breadcrumb or treemap clicks. Directory clicks beyond
        the configured depth are remembered as explicit reveals, while file
        clicks focus their containing directory when useful.

        Parameters:
          x: Horizontal client coordinate.
          y: Vertical client coordinate.
        **/
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
                // Each explicitly revealed ancestor extends the effective depth
                // by one, so only remember this node when it crosses that limit.
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

        /**
        Prepares for context-menu interaction by closing the path dropdown,
        clearing hover visuals, and returning keyboard focus to the main window.
        **/
        void OnRightButtonDown()
        {
            HidePathDropdown();
            SetFocus(m_hwnd);
            ClearHoverStateAndInvalidate();
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        }

        /**
        Resolves a right-click against breadcrumbs before treemap rectangles and
        opens the context menu for the selected node.

        Parameters:
          x: Horizontal client coordinate.
          y: Vertical client coordinate.
        **/
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

        /**
        Resolves a tree node to its full path, converts the click to screen
        coordinates, and opens the appropriate file or directory context menu.

        Parameters:
          node_id: Selected tree node.
          x: Horizontal client coordinate.
          y: Vertical client coordinate.
        **/
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

        /**
        Builds and displays the context menu for a scanned path, dispatches the
        selected shell action, and protects the treemap from the click that can
        follow mouse-based popup cancellation.

        Parameters:
          menu_position: Popup location in screen coordinates.
          path: Full filesystem path represented by the clicked node.
          is_dir: Whether the path is a directory.
        **/
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
                    // A mouse click outside TrackPopupMenu dismisses the menu and
                    // may then arrive as WM_LBUTTONDOWN on the main window.
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

            // Complete the standard foreground-popup sequence so Windows reliably
            // dismisses the menu when focus changes later.
            PostMessageW(m_hwnd, WM_NULL, 0, 0);
        }

        /**
        Opens a file with its registered Windows shell action and reports launch
        failures to the user.

        Parameters:
          path: File path to open.
        **/
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

        /**
        Shows the native Open With dialog for a file, treating user cancellation
        as a normal outcome and reporting other shell failures.

        Parameters:
          path: File path for which an application should be selected.
        **/
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

        /**
        Opens a directory directly in File Explorer or selects a file within its
        containing Explorer window.

        Parameters:
          path: Filesystem path to show.
          is_dir: Whether the path is a directory.
        **/
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

        /**
        Invokes the Windows shell Properties verb for a scanned path.

        Parameters:
          path: File or directory whose properties should be shown.
        **/
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

        /**
        Copies Unicode text to the Windows clipboard using movable global memory
        as required by SetClipboardData.

        Parameters:
          text: Text to place on the clipboard.

        Returns: True when clipboard ownership of the text was transferred.
        **/
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

            // SetClipboardData owns clipboard_memory after a successful call.
            CloseClipboard();
            return true;
        }

        /**
        Displays the selected tree node as the top level of the treemap and rebuilds
        the navigation and layout state.

        Parameters:
          node_id: Tree node to display as the top level of the treemap.
        **/
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

        /**
        Navigates to the parent of the currently displayed node without moving above
        the root of the scanned tree.
        **/
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

        /**
        Walks parent links to measure a node's depth relative to the scanned tree
        root. Invalid or cyclic parent chains terminate defensively.

        Parameters:
          node_id: Node whose depth should be measured.

        Returns: Number of parent edges between the node and scan root.
        **/
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

        /**
        Counts explicitly revealed ancestors of a node without counting the node
        itself. This converts remembered reveals into an effective depth bonus.

        Parameters:
          node_id: Node whose ancestor chain should be inspected.

        Returns: Number of remembered reveal ancestors.
        **/
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

        /**
        Maps main-window navigation and scan shortcuts to their shared command
        paths.

        Parameters:
          key_code: Virtual-key code from WM_KEYDOWN.
        **/
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

        /**
        Starts the shared tooltip timer when tooltips are enabled and no animation
        timer is already active.
        **/
        void StartTooltipAnimation()
        {
            if (!m_show_tooltip || m_tooltip_animation_active || !m_hwnd)
            {
                return;
            }

            SetTimer(m_hwnd, kTooltipAnimationTimerId, kTooltipAnimationIntervalMs, nullptr);
            m_tooltip_animation_active = true;
        }

        /**
        Stops the tooltip timer and synchronizes the cached active-state flag.
        **/
        void StopTooltipAnimation()
        {
            if (!m_tooltip_animation_active || !m_hwnd)
            {
                return;
            }

            KillTimer(m_hwnd, kTooltipAnimationTimerId);
            m_tooltip_animation_active = false;
        }

        /**
        Moves the tooltip toward the latest mouse position using exponential
        smoothing, invalidating only the old and new tooltip regions each frame.
        **/
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

        /**
        Takes a lightweight view of current UI, scan, geometry, and interaction
        state for the renderer. Referenced vectors remain owned by MainWindow.

        Returns: Drawing state valid for the current UI-thread paint operation.
        **/
        MainWindowDrawingState BuildDrawingState() const
        {
            MainWindowDrawingState state;
            state.palette = GetRenderPalette();
            state.scan_in_progress = m_scan_in_progress;
            state.has_error = m_has_error;
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
            state.treemap_group_rect = m_treemap_group_rect;
            state.display_group_rect = m_display_group_rect;
            return state;
        }

        /**
        Collects the main-window theme constants into the renderer's palette
        structure.

        Returns: Palette matching the native header and window controls.
        **/
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

        /**
        Paints the invalidated client region and records the renderer's current
        tooltip bounds for precise invalidation during later hover animation.
        **/
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
        HWND m_about_button = nullptr;
        HWND m_depth_label = nullptr;
        HWND m_depth_edit = nullptr;
        HWND m_depth_up_button = nullptr;
        HWND m_depth_down_button = nullptr;
        HWND m_show_tooltip_check = nullptr;
        HWND m_show_status_bar_check = nullptr;
        HWND m_color_by_file_type_check = nullptr;
        HBRUSH m_header_background_brush = nullptr;
        HBRUSH m_control_panel_brush = nullptr;
        HBRUSH m_control_surface_brush = nullptr;
        HBRUSH m_control_hover_brush = nullptr;
        std::shared_ptr<SharedScanState> m_shared_scan_state;
        MainWindowPainter m_painter;

        bool m_scan_in_progress = false;
        bool m_has_error = false;
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
        std::vector<DriveListEntry> m_ntfs_drive_entries;
        std::unordered_set<uint64_t> m_expanded_reveal_nodes;
        std::vector<BreadcrumbStrip> m_breadcrumbs;
        bool m_show_tooltip = true;
        bool m_show_status_bar = false;
        bool m_color_by_file_type = false;
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

/**
Initializes process-wide Win32 services, creates the main window, and runs the
application message loop.

Parameters:
  instance: Application module instance.
  show_command: Initial window display state supplied by Windows.

Returns: Process exit code from startup or the final WM_QUIT message.
**/
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command)
{
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    swift_disk::gui::GuiOptions options = swift_disk::gui::ParseOptions();
    if (options.show_help)
    {
        MessageBoxW(
            nullptr,
            L"SwiftDisk\n\nUsage:\n  SwiftDisk.exe --path C:\\\n",
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
