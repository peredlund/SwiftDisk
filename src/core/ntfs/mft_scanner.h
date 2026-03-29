#pragma once

#include <string>

#include "common/file_tree.h"

namespace space_scanner::ntfs {

struct MftScanStats {
    uint64_t records_read = 0;
    uint64_t records_parsed = 0;
    uint64_t records_in_use = 0;
    uint64_t records_added = 0;
    uint64_t records_parse_failed = 0;
    uint64_t records_no_name = 0;
    uint64_t records_no_name_with_size = 0;
    uint64_t records_no_name_size = 0;
    uint64_t extension_size_records = 0;
    uint64_t extension_size_total = 0;
    uint64_t extension_missing_base_records = 0;
    uint64_t extension_missing_base_total = 0;
    uint64_t mft_valid_data_length = 0;
    uint32_t bytes_per_file_record = 0;
};

bool ScanVolumeMft(wchar_t drive_letter, FileTree* tree, std::wstring* error, MftScanStats* out_stats);
bool ScanPathMft(const std::wstring& root_path, FileTree* tree, std::wstring* error, MftScanStats* out_stats);

} // namespace space_scanner::ntfs
