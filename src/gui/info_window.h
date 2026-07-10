// SPDX-FileCopyrightText: 2026 Per Edlund
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <windows.h>

namespace swift_disk::gui
{

    /**
    Shows the modal SwiftDisk Help window.

    Parameters:
      owner: Handle of the window that owns the Help window.
    **/
    void ShowHelpWindow(HWND owner);

    /**
    Shows the modal SwiftDisk About window.

    Parameters:
      owner: Handle of the window that owns the About window.
    **/
    void ShowAboutWindow(HWND owner);

} // namespace swift_disk::gui
