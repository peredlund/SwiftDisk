// SPDX-FileCopyrightText: 2026 Per Edlund
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace swift_disk::ntfs
{

struct ParsedFileName
{
    std::wstring m_name;
    uint64_t m_parent_frn = 0;
    uint8_t m_preference = 0;
};

struct ParsedRecord
{
    uint64_t frn = 0;
    uint64_t parent_frn = 0;
    uint64_t base_frn = 0;
    std::wstring name;
    bool is_dir = false;
    bool in_use = false;
    bool has_data_attribute = false;
    uint8_t m_name_preference = 0;
    uint64_t size = 0;
    uint64_t m_size_on_disk = 0;
    uint64_t m_wof_size_on_disk = 0;
    // Only additional non-DOS paths need storage beyond the preferred name.
    std::vector<ParsedFileName> m_additional_names;

    bool AddFileName(std::wstring file_name, uint64_t file_parent_frn, uint8_t name_preference);
};

bool ParseFileRecord(uint8_t* record, size_t record_size, uint32_t bytes_per_sector, uint64_t frn, ParsedRecord* out);

} // namespace swift_disk::ntfs
