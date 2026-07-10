// SPDX-FileCopyrightText: 2026 Per Edlund
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/app_settings.h"

#include <windows.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace swift_disk::gui
{

    namespace
    {
        constexpr const wchar_t* kSettingsSubkey = L"Software\\SwiftDisk";
        constexpr const wchar_t* kShowTooltipValueName = L"ShowTooltip";
        constexpr const wchar_t* kShowStatusBarValueName = L"ShowStatusBar";
        constexpr const wchar_t* kMaxDepthValueName = L"MaxDepth";
        constexpr const wchar_t* kLastScanPathValueName = L"LastScanPath";
        constexpr int kMinimumMaxDepth = 1;
        constexpr int kMaximumMaxDepth = 32;

        /**
        Reads and validates a DWORD setting from an already opened registry key.

        Parameters:
          key: Registry key containing the setting.
          value_name: Name of the registry value to read.
          value: Receives the DWORD on success.

        Returns: True when the value exists with the expected type and size.
        **/
        bool TryReadDwordValue(HKEY key, const wchar_t* value_name, DWORD* value)
        {
            if (!value)
            {
                return false;
            }

            DWORD type = REG_DWORD;
            DWORD data = 0;
            DWORD data_size = sizeof(data);
            const LSTATUS status = RegGetValueW(
                key,
                nullptr,
                value_name,
                RRF_RT_REG_DWORD,
                &type,
                &data,
                &data_size);
            if (status != ERROR_SUCCESS || type != REG_DWORD || data_size != sizeof(data))
            {
                return false;
            }

            *value = data;
            return true;
        }

        void WriteDwordValue(HKEY key, const wchar_t* value_name, DWORD value)
        {
            RegSetValueExW(
                key,
                value_name,
                0,
                REG_DWORD,
                reinterpret_cast<const BYTE*>(&value),
                sizeof(value));
        }

        /**
        Reads a REG_SZ setting with a size query followed by a bounded data read.

        Parameters:
          key: Registry key containing the setting.
          value_name: Name of the registry value to read.
          value: Receives the string on success.

        Returns: True when the value exists and can be read as REG_SZ.
        **/
        bool TryReadStringValue(HKEY key, const wchar_t* value_name, std::wstring* value)
        {
            if (!value)
            {
                return false;
            }

            DWORD type = REG_SZ;
            DWORD data_size = 0;
            LSTATUS status = RegGetValueW(
                key,
                nullptr,
                value_name,
                RRF_RT_REG_SZ,
                &type,
                nullptr,
                &data_size);
            if (status != ERROR_SUCCESS || type != REG_SZ || data_size == 0)
            {
                return false;
            }

            // Registry sizes are reported in bytes. Round up defensively before
            // adding a guaranteed terminator for malformed or legacy values.
            const size_t character_count =
                (data_size + sizeof(wchar_t) - 1) / sizeof(wchar_t);
            std::vector<wchar_t> buffer(character_count + 1, L'\0');
            status = RegGetValueW(
                key,
                nullptr,
                value_name,
                RRF_RT_REG_SZ,
                &type,
                buffer.data(),
                &data_size);
            if (status != ERROR_SUCCESS || type != REG_SZ)
            {
                return false;
            }

            *value = buffer.data();
            return true;
        }

        void WriteStringValue(HKEY key, const wchar_t* value_name, const std::wstring& value)
        {
            const DWORD data_size = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
            RegSetValueExW(
                key,
                value_name,
                0,
                REG_SZ,
                reinterpret_cast<const BYTE*>(value.c_str()),
                data_size);
        }
    }

    /**
    Returns whether treemap hover tooltips should be displayed.

    Returns: True when tooltips should be displayed.
    **/
    bool AppSettings::GetShowTooltip() const
    {
        return m_show_tooltip;
    }

    /**
    Updates whether treemap hover tooltips should be displayed.

    Parameters:
      show_tooltip: True when tooltips should be displayed.
    **/
    void AppSettings::SetShowTooltip(bool show_tooltip)
    {
        m_show_tooltip = show_tooltip;
    }

    /**
    Returns whether the status bar should be displayed.

    Returns: True when the status bar should be displayed.
    **/
    bool AppSettings::GetShowStatusBar() const
    {
        return m_show_status_bar;
    }

    /**
    Updates whether the status bar should be displayed.

    Parameters:
      show_status_bar: True when the status bar should be displayed.
    **/
    void AppSettings::SetShowStatusBar(bool show_status_bar)
    {
        m_show_status_bar = show_status_bar;
    }

    /**
    Returns the configured treemap depth limit.

    Returns: Treemap depth limit clamped to the supported range.
    **/
    int AppSettings::GetMaxDepth() const
    {
        return m_max_depth;
    }

    /**
    Updates the treemap depth limit.

    Parameters:
      max_depth: Treemap depth limit. Values outside the supported range are
      clamped.
    **/
    void AppSettings::SetMaxDepth(int max_depth)
    {
        m_max_depth = std::clamp(max_depth, kMinimumMaxDepth, kMaximumMaxDepth);
    }

    /**
    Returns the last path the user scanned.

    Returns: Last scanned path, or an empty string when no path is stored.
    **/
    const std::wstring& AppSettings::GetLastScanPath() const
    {
        return m_last_scan_path;
    }

    /**
    Updates the last path the user scanned.

    Parameters:
      last_scan_path: Last scanned path to restore on the next app launch.
    **/
    void AppSettings::SetLastScanPath(std::wstring last_scan_path)
    {
        m_last_scan_path = std::move(last_scan_path);
    }

    /**
    Loads GUI preferences from the current user's registry hive.

    Returns: Persisted settings, or default settings when values are missing or
    unavailable.
    **/
    AppSettings LoadAppSettings()
    {
        AppSettings settings;

        HKEY key = nullptr;
        const LSTATUS open_status = RegOpenKeyExW(
            HKEY_CURRENT_USER,
            kSettingsSubkey,
            0,
            KEY_READ,
            &key);
        if (open_status != ERROR_SUCCESS)
        {
            return settings;
        }

        DWORD value = 0;
        if (TryReadDwordValue(key, kShowTooltipValueName, &value))
        {
            settings.SetShowTooltip(value != 0);
        }
        if (TryReadDwordValue(key, kShowStatusBarValueName, &value))
        {
            settings.SetShowStatusBar(value != 0);
        }
        if (TryReadDwordValue(key, kMaxDepthValueName, &value))
        {
            settings.SetMaxDepth(static_cast<int>(value));
        }
        std::wstring last_scan_path;
        if (TryReadStringValue(key, kLastScanPathValueName, &last_scan_path))
        {
            settings.SetLastScanPath(std::move(last_scan_path));
        }

        RegCloseKey(key);
        return settings;
    }

    /**
    Saves GUI preferences to the current user's registry hive.

    Parameters:
      settings: Settings to persist.
    **/
    void SaveAppSettings(const AppSettings& settings)
    {
        HKEY key = nullptr;
        const LSTATUS create_status = RegCreateKeyExW(
            HKEY_CURRENT_USER,
            kSettingsSubkey,
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE,
            nullptr,
            &key,
            nullptr);
        if (create_status != ERROR_SUCCESS)
        {
            return;
        }

        WriteDwordValue(key, kShowTooltipValueName, settings.GetShowTooltip() ? 1 : 0);
        WriteDwordValue(key, kShowStatusBarValueName, settings.GetShowStatusBar() ? 1 : 0);
        WriteDwordValue(key, kMaxDepthValueName, static_cast<DWORD>(settings.GetMaxDepth()));
        WriteStringValue(key, kLastScanPathValueName, settings.GetLastScanPath());
        RegCloseKey(key);
    }

} // namespace swift_disk::gui
