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
        bool GetShowTooltip() const;

        void SetShowTooltip(bool show_tooltip);

        bool GetShowStatusBar() const;

        void SetShowStatusBar(bool show_status_bar);

        bool GetColorByFileType() const;

        void SetColorByFileType(bool color_by_file_type);

        bool GetUseSizeOnDisk() const;

        void SetUseSizeOnDisk(bool use_size_on_disk);

        int GetMaxDepth() const;

        void SetMaxDepth(int max_depth);

        const std::wstring& GetLastScanPath() const;

        void SetLastScanPath(std::wstring last_scan_path);

    private:
        bool m_show_tooltip = true;
        bool m_show_status_bar = false;
        bool m_color_by_file_type = false;
        bool m_use_size_on_disk = false;
        int m_max_depth = 3;
        std::wstring m_last_scan_path;
    };

    AppSettings LoadAppSettings();

    void SaveAppSettings(const AppSettings& settings);

} // namespace swift_disk::gui
