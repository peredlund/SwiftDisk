// SPDX-FileCopyrightText: 2026 Per Edlund
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/ntfs/mft_extents.h"

#include <windows.h>
#include <winioctl.h>

#include <cstring>
#include <limits>
#include <type_traits>

namespace swift_disk::ntfs::detail
{

/**
Validates one FSCTL_GET_RETRIEVAL_POINTERS response for the dense MFT stream.
Pages must join at exact extent boundaries. No output is changed on rejection.

Parameters:
  data: Response buffer, which may contain only a partial page of extents.
  data_size: Number of bytes actually returned by Windows.
  expected_vcn: Logical cluster immediately after the previously accepted page.
  bytes_per_cluster: Volume cluster size used to validate physical byte offsets.
  extents: Receives the validated extents in logical order.
  next_vcn: Receives the starting VCN for the next request.

Returns: True when every returned extent is valid and the page makes progress.
**/
bool AppendMftExtentPage(const void* data, size_t data_size, uint64_t expected_vcn,
    uint32_t bytes_per_cluster, std::vector<MftExtent>* extents, uint64_t* next_vcn)
{
    constexpr size_t header_size = offsetof(RETRIEVAL_POINTERS_BUFFER, Extents);
    using ExtentEntry = std::remove_extent_t<decltype(RETRIEVAL_POINTERS_BUFFER::Extents)>;
    if (!data || !extents || !next_vcn || bytes_per_cluster == 0 || data_size < header_size)
    {
        return false;
    }

    const auto* bytes = static_cast<const uint8_t*>(data);
    DWORD extent_count;
    LARGE_INTEGER starting_vcn;
    std::memcpy(&extent_count, bytes + offsetof(RETRIEVAL_POINTERS_BUFFER, ExtentCount), sizeof(extent_count));
    std::memcpy(&starting_vcn, bytes + offsetof(RETRIEVAL_POINTERS_BUFFER, StartingVcn), sizeof(starting_vcn));
    if (extent_count == 0 || extent_count > (data_size - header_size) / sizeof(ExtentEntry) ||
        starting_vcn.QuadPart < 0 || static_cast<uint64_t>(starting_vcn.QuadPart) != expected_vcn)
    {
        return false;
    }

    const uint64_t max_cluster = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) / bytes_per_cluster;
    std::vector<MftExtent> page;
    page.reserve(extent_count);
    uint64_t current_vcn = expected_vcn;
    for (DWORD index = 0; index < extent_count; ++index)
    {
        ExtentEntry entry;
        std::memcpy(&entry, bytes + header_size + index * sizeof(ExtentEntry), sizeof(entry));
        if (entry.NextVcn.QuadPart <= 0 || static_cast<uint64_t>(entry.NextVcn.QuadPart) <= current_vcn ||
            entry.Lcn.QuadPart < 0)
        {
            return false;
        }

        MftExtent extent;
        extent.m_start_vcn = current_vcn;
        extent.m_start_lcn = static_cast<uint64_t>(entry.Lcn.QuadPart);
        extent.m_cluster_count = static_cast<uint64_t>(entry.NextVcn.QuadPart) - current_vcn;
        if (extent.m_start_lcn > max_cluster || extent.m_cluster_count > max_cluster - extent.m_start_lcn)
        {
            return false;
        }
        page.push_back(extent);
        current_vcn = static_cast<uint64_t>(entry.NextVcn.QuadPart);
    }

    extents->insert(extents->end(), page.begin(), page.end());
    *next_vcn = current_vcn;
    return true;
}

} // namespace swift_disk::ntfs::detail
