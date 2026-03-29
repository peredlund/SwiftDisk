#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/file_tree.h"
#include "core/ntfs/mft_scanner.h"

namespace space_scanner::ntfs {

struct ValidationMismatch {
    uint64_t frn = 0;
    std::wstring path;
    std::wstring field;
    std::wstring expected;
    std::wstring actual;
};

struct ValidationEdgeCaseSummary {
    uint64_t entries_inspected = 0;
    uint64_t hard_link_entries = 0;
    uint64_t sparse_entries = 0;
    uint64_t compressed_entries = 0;
    uint64_t reparse_entries = 0;
    uint64_t ads_entries = 0;
    std::vector<std::wstring> hard_link_examples;
    std::vector<std::wstring> sparse_examples;
    std::vector<std::wstring> compressed_examples;
    std::vector<std::wstring> reparse_examples;
    std::vector<std::wstring> ads_examples;
};

struct ValidationReport {
    uint64_t sample_size_requested = 0;
    uint64_t sample_file_candidates = 0;
    uint64_t sampled_files_checked = 0;
    uint64_t sampled_files_with_comparable_ids = 0;
    uint64_t missing_paths = 0;
    uint64_t size_mismatches = 0;
    uint64_t parent_mismatches = 0;
    uint64_t name_mismatches = 0;
    uint64_t type_mismatches = 0;
    uint64_t frn_mismatches = 0;
    uint64_t attribute_list_extension_records = 0;
    uint64_t attribute_list_extension_bytes = 0;
    ValidationEdgeCaseSummary edge_cases;
    std::vector<ValidationMismatch> mismatches;
};

bool ValidateVolumeWithWin32(
    const FileTree& tree,
    const MftScanStats& scan_stats,
    size_t sample_size,
    size_t edge_scan_limit,
    ValidationReport* out_report,
    std::wstring* error);

} // namespace space_scanner::ntfs
