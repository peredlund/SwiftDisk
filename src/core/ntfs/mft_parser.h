// SPDX-FileCopyrightText: 2026 Per Edlund
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace swift_disk::ntfs
{

struct DataRun
{
    int64_t lcn = 0;
    uint64_t length = 0;
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
    uint64_t size = 0;
};

bool ParseFileRecord(uint8_t* record, size_t record_size, uint32_t bytes_per_sector, uint64_t frn, ParsedRecord* out);
bool ExtractMftDataRuns(uint8_t* record, size_t record_size, uint32_t bytes_per_sector, std::vector<DataRun>* runs, uint64_t* data_size);

} // namespace swift_disk::ntfs
