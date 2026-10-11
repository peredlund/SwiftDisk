// SPDX-FileCopyrightText: 2026 Per Edlund
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace swift_disk::ntfs
{

/**
Describes one contiguous physical extent of the MFT in cluster units.
**/
struct MftExtent
{
    uint64_t m_start_vcn = 0;
    uint64_t m_start_lcn = 0;
    uint64_t m_cluster_count = 0;
};

namespace detail
{

bool AppendMftExtentPage(const void* data, size_t data_size, uint64_t expected_vcn,
    uint32_t bytes_per_cluster, std::vector<MftExtent>* extents, uint64_t* next_vcn);

} // namespace detail
} // namespace swift_disk::ntfs
