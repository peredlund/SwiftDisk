// SPDX-FileCopyrightText: 2026 Per Edlund
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/info_window.h"

#include "gui/version.h"

#include <algorithm>
#include <iterator>
#include <sstream>
#include <string>

#define SWIFT_DISK_WIDE_STRING_IMPL(value) L##value
#define SWIFT_DISK_WIDE_STRING(value) SWIFT_DISK_WIDE_STRING_IMPL(value)

namespace swift_disk::gui
{
namespace
{
    constexpr int IDC_INFO_TEXT = 1101;
    constexpr int IDC_INFO_CLOSE_BUTTON = 1102;
    constexpr BYTE kHeaderPanelRed = 35;
    constexpr BYTE kHeaderPanelGreen = 40;
    constexpr BYTE kHeaderPanelBlue = 47;
    constexpr BYTE kControlPanelRed = 27;
    constexpr BYTE kControlPanelGreen = 32;
    constexpr BYTE kControlPanelBlue = 38;
    constexpr BYTE kControlBackplateRed = 43;
    constexpr BYTE kControlBackplateGreen = 49;
    constexpr BYTE kControlBackplateBlue = 58;
    constexpr BYTE kControlSurfaceRed = 52;
    constexpr BYTE kControlSurfaceGreen = 60;
    constexpr BYTE kControlSurfaceBlue = 70;
    constexpr BYTE kControlTextRed = 240;
    constexpr BYTE kControlTextGreen = 243;
    constexpr BYTE kControlTextBlue = 247;
    constexpr BYTE kControlBorderRed = 84;
    constexpr BYTE kControlBorderGreen = 92;
    constexpr BYTE kControlBorderBlue = 103;
    constexpr BYTE kControlActiveRed = 104;
    constexpr BYTE kControlActiveGreen = 132;
    constexpr BYTE kControlActiveBlue = 176;
    constexpr BYTE kControlHoverRed = 66;
    constexpr BYTE kControlHoverGreen = 76;
    constexpr BYTE kControlHoverBlue = 90;

    struct InfoWindowState
    {
        HBRUSH m_background_brush = nullptr;
        HBRUSH m_text_background_brush = nullptr;
    };

    const wchar_t* InfoWindowClassName()
    {
        return L"SwiftDiskInfoWindow";
    }

    /**
    Builds the complete user-facing guidance shown in the Help window.

    Returns: Help text with Win32 edit-control line endings.
    **/
    std::wstring BuildHelpText()
    {
        return
            L"About SwiftDisk\r\n"
            L"SwiftDisk helps you find what is using disk space by scanning a drive or folder and showing the result as an interactive treemap. It does this very quickly by bypassing the operating system and instead reading the information directly from the drive's Master File Table file.\r\n"
            L"\r\n"
            L"Choose and scan\r\n"
            L"- Use the path field to choose a drive, or type or paste a folder path.\r\n"
            L"- Browse... opens a folder picker and puts the selected folder in the path field.\r\n"
            L"- Scan starts scanning the selected path. You can also press Enter while the path field has focus.\r\n"
            L"- The last scanned path is remembered for the next time you open the app.\r\n"
            L"- Hover over a button, checkbox, or other header control to see what it does. These descriptions remain available when treemap tooltips are turned off.\r\n"
            L"\r\n"
            L"Why the scan is fast\r\n"
            L"- On NTFS drives, SwiftDisk is very fast because it reads the MFT.\r\n"
            L"- NTFS is the default file system for Windows operating systems and the MFT, or Master File Table, is NTFS's own index of files and folders.\r\n"
            L"- Reading the MFT is much faster than scanning each folder one by one.\r\n"
            L"\r\n"
            L"Treemap basics\r\n"
            L"- Each rectangle represents a file or folder. Larger rectangles have a larger size.\r\n"
            L"- Folder tiles can contain smaller child rectangles.\r\n"
            L"- The parent folder bars at the top of the treemap show the folder path you have drilled into.\r\n"
            L"\r\n"
            L"Navigating the treemap\r\n"
            L"- Left-click a folder tile to drill into that folder.\r\n"
            L"- Left-click a file tile to drill into its containing folder.\r\n"
            L"- Left-click a parent folder bar to go back up to that folder.\r\n"
            L"- Press the Backspace or Escape key to go up one folder level.\r\n"
            L"- Press the Home key or Home button to return to the root of the current scan.\r\n"
            L"\r\n"
            L"Right-click actions\r\n"
            L"- Right-click a tile or parent folder bar to open the context menu for that file or folder.\r\n"
            L"- Files can be opened directly by their default application or with Open with....\r\n"
            L"- Files and folders can be opened in File Explorer, copied as a full path, or opened in a Properties window.\r\n"
            L"\r\n"
            L"Treemap depth and expanded tiles\r\n"
            L"- The Depth value limits how many treemap levels SwiftDisk expands automatically.\r\n"
            L"- If you drill into a folder that is deeper than the current depth limit, SwiftDisk remembers that path as intentionally expanded and shows it even if is deeper than the depth limit\r\n"
            L"- Changing Depth, starting a fresh scan, or using Clear removes those remembered expanded folders\r\n"
            L"\r\n"
            L"Treemap controls\r\n"
            L"- Depth controls how many folder levels are visible at once. Lower values keep the view simpler; higher values show more nested folders.\r\n"
            L"- Home returns to the root of the current scan without clearing remembered expanded folders.\r\n"
            L"- Clear removes remembered expanded folders without changing the current scan.\r\n"
            L"\r\n"
            L"Options\r\n"
            L"- Color by file type gives files with the same filename extension the same tile color and adds clearer tile outlines.\r\n"
            L"- Size on disk selects the size used for tile areas, labels, and totals. When checked, it shows allocated storage, including compression and sparse-file savings. When unchecked, it shows Size, the length of file contents.\r\n"
            L"- Tooltips shows the full path and the size for the tile or parent folder bar under the mouse cursor. It shows Size on disk when that option is checked, otherwise Size.\r\n"
            L"- Status bar shows scan and selection details at the bottom of the window.";
    }

    /**
    Builds the About text from generated version metadata, compiler build date,
    copyright, and license information.

    Returns: Formatted text shown in the About window.
    **/
    std::wstring BuildAboutText()
    {
        std::wstringstream stream;
        stream
            << L"SwiftDisk\r\n"
            << L"Version: " << SWIFT_DISK_VERSION << L"\r\n"
            << L"Build date: " << SWIFT_DISK_WIDE_STRING(__DATE__) << L"\r\n"
            << L"\r\n"
            << L"License\r\n"
            << L"GNU General Public License version 3\r\n"
            << L"\r\n"
            << L"Copyright (C) 2026 Per Edlund\r\n"
            << L"\r\n"
            << L"This program is free software: you can redistribute it and/or modify\r\n"
            << L"it under the terms of the GNU General Public License as published by\r\n"
            << L"the Free Software Foundation, either version 3 of the License, or\r\n"
            << L"(at your option) any later version.\r\n"
            << L"\r\n"
            << L"This program is distributed in the hope that it will be useful,\r\n"
            << L"but WITHOUT ANY WARRANTY; without even the implied warranty of\r\n"
            << L"MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the\r\n"
            << L"GNU General Public License for more details.\r\n"
            << L"\r\n"
            << L"You should have received a copy of the GNU General Public License\r\n"
            << L"along with this program. If not, see < https://www.gnu.org/licenses/>.";
        return stream.str();
    }

    COLORREF ColorRefFromRgb(BYTE red, BYTE green, BYTE blue)
    {
        return RGB(red, green, blue);
    }

    /**
    Positions the scrollable text and Close button within the current client area
    while preserving fixed outer padding and valid minimum dimensions.

    Parameters:
      hwnd: Information window whose child controls should be positioned.
    **/
    void LayoutInfoWindowControls(HWND hwnd)
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

        HWND info_text = GetDlgItem(hwnd, IDC_INFO_TEXT);
        if (info_text)
        {
            MoveWindow(
                info_text,
                padding,
                padding,
                std::max(1, width - (padding * 2)),
                std::max(1, edit_bottom - padding),
                TRUE);
        }

        HWND close_button = GetDlgItem(hwnd, IDC_INFO_CLOSE_BUTTON);
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

    InfoWindowState* GetInfoWindowState(HWND hwnd)
    {
        return reinterpret_cast<InfoWindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    /**
    Fills a GDI rectangle with a temporary solid brush.

    Parameters:
      hdc: Destination device context.
      rect: Area to fill.
      color: Fill color.
    **/
    void FillInfoWindowRect(HDC hdc, const RECT& rect, COLORREF color)
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
    Draws a one-pixel GDI frame while restoring the device context's previously
    selected pen and brush.

    Parameters:
      hdc: Destination device context.
      rect: Frame bounds.
      color: Frame color.
    **/
    void DrawInfoWindowFrame(HDC hdc, const RECT& rect, COLORREF color)
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
    Draws the owner-drawn Close button with themed disabled, pressed, hover, and
    keyboard-focus feedback.

    Parameters:
      draw_item: Win32 owner-draw state and destination device context.
    **/
    void DrawInfoWindowButton(const DRAWITEMSTRUCT& draw_item)
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
        FillInfoWindowRect(draw_item.hDC, rect, fill_color);
        DrawInfoWindowFrame(
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

    /**
    Handles creation, layout, themed drawing, commands, and resource cleanup for
    Help and About windows.

    Parameters:
      hwnd: Information window receiving the message.
      message: Win32 message identifier.
      w_param: Message-specific word parameter.
      l_param: Message-specific long parameter.

    Returns: The handled result or the default window-procedure result.
    **/
    LRESULT CALLBACK InfoWindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param)
    {
        switch (message)
        {
        case WM_CREATE:
        {
            const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
            const wchar_t* info_text_value = create
                ? static_cast<const wchar_t*>(create->lpCreateParams)
                : L"";
            HFONT gui_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            auto* state = new InfoWindowState();
            state->m_background_brush = CreateSolidBrush(
                ColorRefFromRgb(kHeaderPanelRed, kHeaderPanelGreen, kHeaderPanelBlue));
            state->m_text_background_brush = CreateSolidBrush(
                ColorRefFromRgb(kControlPanelRed, kControlPanelGreen, kControlPanelBlue));
            // Keep the brushes with the HWND because later paint and destroy
            // messages do not carry the creation context.
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

            HWND info_text = CreateWindowExW(
                0,
                L"EDIT",
                info_text_value,
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                0,
                0,
                0,
                0,
                hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_INFO_TEXT)),
                nullptr,
                nullptr);
            if (info_text)
            {
                SendMessageW(info_text, WM_SETFONT, reinterpret_cast<WPARAM>(gui_font), TRUE);
                SendMessageW(info_text, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
                SendMessageW(info_text, EM_SETSEL, 0, 0);
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
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_INFO_CLOSE_BUTTON)),
                nullptr,
                nullptr);
            if (close_button)
            {
                SendMessageW(close_button, WM_SETFONT, reinterpret_cast<WPARAM>(gui_font), TRUE);
            }

            LayoutInfoWindowControls(hwnd);
            return 0;
        }
        case WM_ERASEBKGND:
        {
            InfoWindowState* state = GetInfoWindowState(hwnd);
            RECT client = {};
            GetClientRect(hwnd, &client);
            FillRect(
                reinterpret_cast<HDC>(w_param),
                &client,
                state && state->m_background_brush
                    ? state->m_background_brush
                    : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            return 1;
        }
        case WM_SIZE:
            LayoutInfoWindowControls(hwnd);
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        {
            HDC hdc = reinterpret_cast<HDC>(w_param);
            HWND control = reinterpret_cast<HWND>(l_param);
            if (control == GetDlgItem(hwnd, IDC_INFO_TEXT))
            {
                InfoWindowState* state = GetInfoWindowState(hwnd);
                SetTextColor(hdc, ColorRefFromRgb(kControlTextRed, kControlTextGreen, kControlTextBlue));
                SetBkColor(hdc, ColorRefFromRgb(kControlPanelRed, kControlPanelGreen, kControlPanelBlue));
                SetBkMode(hdc, OPAQUE);
                return reinterpret_cast<LRESULT>(
                    state && state->m_text_background_brush
                        ? state->m_text_background_brush
                        : GetStockObject(BLACK_BRUSH));
            }
            break;
        }
        case WM_DRAWITEM:
        {
            const auto* draw_item = reinterpret_cast<const DRAWITEMSTRUCT*>(l_param);
            if (draw_item && draw_item->CtlID == IDC_INFO_CLOSE_BUTTON)
            {
                DrawInfoWindowButton(*draw_item);
                return TRUE;
            }
            break;
        }
        case WM_COMMAND:
            if (LOWORD(w_param) == IDC_INFO_CLOSE_BUTTON ||
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
            InfoWindowState* state = GetInfoWindowState(hwnd);
            if (state)
            {
                if (state->m_background_brush)
                {
                    DeleteObject(state->m_background_brush);
                }
                if (state->m_text_background_brush)
                {
                    DeleteObject(state->m_text_background_brush);
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

    /**
    Registers the shared native window class used by both Help and About. An
    existing registration is treated as success so either window can open first.

    Parameters:
      instance: Application module that owns the window class.

    Returns: True when the class is available for window creation.
    **/
    bool RegisterInfoWindowClass(HINSTANCE instance)
    {
        WNDCLASSW window_class = {};
        window_class.lpfnWndProc = InfoWindowProc;
        window_class.hInstance = instance;
        window_class.lpszClassName = InfoWindowClassName();
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = nullptr;

        if (RegisterClassW(&window_class))
        {
            return true;
        }

        return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    /**
    Increases an information window's height to fit its wrapped text, limited to
    the monitor's work area so the window and Close button remain accessible.

    Parameters:
      hwnd: Information window with its text control already laid out.
    **/
    void FitInfoWindowHeightToText(HWND hwnd)
    {
        HWND info_text = GetDlgItem(hwnd, IDC_INFO_TEXT);
        if (!info_text)
        {
            return;
        }
        HDC hdc = GetDC(info_text);
        if (!hdc)
        {
            return;
        }

        const HFONT font = reinterpret_cast<HFONT>(SendMessageW(info_text, WM_GETFONT, 0, 0));
        const HGDIOBJ old_font = font ? SelectObject(hdc, font) : nullptr;
        TEXTMETRICW metrics = {};
        const bool measured = GetTextMetricsW(hdc, &metrics) != FALSE;
        if (old_font)
        {
            SelectObject(hdc, old_font);
        }
        ReleaseDC(info_text, hdc);
        if (!measured)
        {
            return;
        }

        RECT window_rect = {};
        RECT text_rect = {};
        GetWindowRect(hwnd, &window_rect);
        SendMessageW(info_text, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&text_rect));
        const int line_count = static_cast<int>(SendMessageW(info_text, EM_GETLINECOUNT, 0, 0));
        const LONG current_height = window_rect.bottom - window_rect.top;
        const LONG extra_height = std::max(0L, line_count * metrics.tmHeight - (text_rect.bottom - text_rect.top));
        LONG height = current_height + extra_height;
        LONG top = window_rect.top;

        MONITORINFO monitor = {};
        monitor.cbSize = sizeof(monitor);
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
        {
            const LONG screen_padding = 14;
            height = std::min(height, std::max(1L,
                monitor.rcWork.bottom - monitor.rcWork.top - screen_padding * 2));
            const LONG min_top = monitor.rcWork.top + screen_padding;
            const LONG max_top = std::max(min_top, monitor.rcWork.bottom - screen_padding - height);
            top = std::clamp(top, min_top, max_top);
        }

        SetWindowPos(hwnd, nullptr, window_rect.left, top,
            window_rect.right - window_rect.left, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    /**
    Creates a modeless native window and runs a local message loop that gives it
    modal behavior relative to its disabled owner.

    Parameters:
      owner: Window to disable while the information window is open.
      title: Information-window title.
      text: Read-only text displayed in the window.
      width: Initial outer window width in pixels.
      height: Initial outer window height in pixels.
      fit_text_height: Whether to grow the window to fit all text when screen space allows.
    **/
    void ShowInfoWindow(
        HWND owner,
        const wchar_t* title,
        const std::wstring& text,
        int width,
        int height,
        bool fit_text_height = false)
    {
        HINSTANCE instance = GetModuleHandleW(nullptr);
        if (!RegisterInfoWindowClass(instance))
        {
            return;
        }

        HWND info_window = CreateWindowExW(
            WS_EX_DLGMODALFRAME,
            InfoWindowClassName(),
            title,
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            width,
            height,
            owner,
            nullptr,
            instance,
            // InfoWindowProc reads this pointer only during synchronous
            // WM_CREATE handling; CreateWindowExW does not take ownership of it.
            const_cast<wchar_t*>(text.c_str()));
        if (!info_window)
        {
            return;
        }
        if (fit_text_height)
        {
            FitInfoWindowHeightToText(info_window);
        }

        // Disabling the owner and running this nested loop provides the modal
        // behavior normally supplied by a dialog manager.
        EnableWindow(owner, FALSE);
        ShowWindow(info_window, SW_SHOW);
        UpdateWindow(info_window);

        MSG message = {};
        BOOL get_message_result = 0;
        while (IsWindow(info_window) && (get_message_result = GetMessageW(&message, nullptr, 0, 0)) > 0)
        {
            // Preserve dialog-style Tab, Enter, and Escape handling even though
            // the window was created as a regular overlapped window.
            if (!IsDialogMessageW(info_window, &message))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        if (IsWindow(owner))
        {
            EnableWindow(owner, TRUE);
            SetActiveWindow(owner);
        }

        if (get_message_result == 0)
        {
            // The nested loop consumed WM_QUIT, so repost it for the application's
            // outer message loop after this function returns.
            PostQuitMessage(static_cast<int>(message.wParam));
        }
    }
} // namespace

    /**
    Shows the modal SwiftDisk Help window.

    Parameters:
      owner: Handle of the window that owns the Help window.
    **/
    void ShowHelpWindow(HWND owner)
    {
        ShowInfoWindow(owner, L"SwiftDisk Help", BuildHelpText(), 680, 740, true);
    }

    /**
    Shows the modal SwiftDisk About window.

    Parameters:
      owner: Handle of the window that owns the About window.
    **/
    void ShowAboutWindow(HWND owner)
    {
        ShowInfoWindow(owner, L"About SwiftDisk", BuildAboutText(), 460, 390);
    }

} // namespace swift_disk::gui
