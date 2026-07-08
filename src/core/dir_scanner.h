// SPDX-FileCopyrightText: 2026 Per Edlund
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

#include "common/file_tree.h"

namespace swift_disk
{

bool ScanDirectory(const std::wstring& root_path, FileTree* tree, uint64_t* out_root_id);

} // namespace swift_disk
