#pragma once

#include <string>

#include "common/file_tree.h"

namespace space_scanner
{

bool ScanDirectory(const std::wstring& root_path, FileTree* tree, uint64_t* out_root_id);

} // namespace space_scanner
