// SPDX-FileCopyrightText: 2026 Per Edlund
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace swift_disk::gui
{

    /**
    Stores user-configurable GUI preferences that should persist between app
    launches.
    **/
    class AppSettings
    {
    public:
        /**
        Returns whether treemap hover tooltips should be displayed.

        Returns: True when tooltips should be displayed.
        **/
        bool GetShowTooltip() const;

        /**
        Updates whether treemap hover tooltips should be displayed.

        Parameters:
          show_tooltip: True when tooltips should be displayed.
        **/
        void SetShowTooltip(bool show_tooltip);

        /**
        Returns whether the status bar should be displayed.

        Returns: True when the status bar should be displayed.
        **/
        bool GetShowStatusBar() const;

        /**
        Updates whether the status bar should be displayed.

        Parameters:
          show_status_bar: True when the status bar should be displayed.
        **/
        void SetShowStatusBar(bool show_status_bar);

        /**
        Returns the configured treemap depth limit.

        Returns: Treemap depth limit clamped to the supported range.
        **/
        int GetMaxDepth() const;

        /**
        Updates the treemap depth limit.

        Parameters:
          max_depth: Treemap depth limit. Values outside the supported range are
          clamped.
        **/
        void SetMaxDepth(int max_depth);

        /**
        Returns the last path the user scanned.

        Returns: Last scanned path, or an empty string when no path is stored.
        **/
        const std::wstring& GetLastScanPath() const;

        /**
        Updates the last path the user scanned.

        Parameters:
          last_scan_path: Last scanned path to restore on the next app launch.
        **/
        void SetLastScanPath(std::wstring last_scan_path);

    private:
        bool m_show_tooltip = true;
        bool m_show_status_bar = false;
        int m_max_depth = 3;
        std::wstring m_last_scan_path;
    };

    /**
    Loads GUI preferences from the current user's registry hive.

    Returns: Persisted settings, or default settings when values are missing or
    unavailable.
    **/
    AppSettings LoadAppSettings();

    /**
    Saves GUI preferences to the current user's registry hive.

    Parameters:
      settings: Settings to persist.
    **/
    void SaveAppSettings(const AppSettings& settings);

} // namespace swift_disk::gui
