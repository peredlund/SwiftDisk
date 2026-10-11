// SPDX-FileCopyrightText: 2026 Per Edlund
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/ntfs/mft_extents.h"
#include "core/ntfs/ntfs_volume.h"

#include <windows.h>
#include <winioctl.h>

#include <cstring>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace
{

using swift_disk::ntfs::MftExtent;
using swift_disk::ntfs::detail::AppendMftExtentPage;
constexpr size_t kHeaderSize = offsetof(RETRIEVAL_POINTERS_BUFFER, Extents);

/**
Fails the test in Release builds as well as Debug builds.
**/
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

/**
Builds an SDK-format response with (next VCN, physical LCN) extent pairs.
**/
std::vector<uint8_t> BuildPage(int64_t starting_vcn,
    std::initializer_list<std::pair<int64_t, int64_t>> entries)
{
    const DWORD count = static_cast<DWORD>(entries.size());
    std::vector<uint8_t> bytes(kHeaderSize + entries.size() * 2 * sizeof(int64_t));
    std::memcpy(bytes.data() + offsetof(RETRIEVAL_POINTERS_BUFFER, ExtentCount), &count, sizeof(count));
    std::memcpy(bytes.data() + offsetof(RETRIEVAL_POINTERS_BUFFER, StartingVcn), &starting_vcn, sizeof(starting_vcn));
    size_t offset = kHeaderSize;
    for (const auto& entry : entries)
    {
        std::memcpy(bytes.data() + offset, &entry.first, sizeof(entry.first));
        std::memcpy(bytes.data() + offset + sizeof(int64_t), &entry.second, sizeof(entry.second));
        offset += 2 * sizeof(int64_t);
    }
    return bytes;
}

/**
Checks logical record positions across fragmented and paginated mappings.
Physical clusters may move backwards or begin at LCN zero.
**/
void TestFragmentedPages()
{
    const auto first_page = BuildPage(0,
    {
        std::make_pair(2, 100), std::make_pair(5, 20)
    });
    const auto second_page = BuildPage(5,
    {
        std::make_pair(7, 0)
    });
    std::vector<MftExtent> extents;
    uint64_t next_vcn = 0;
    Require(AppendMftExtentPage(first_page.data(), first_page.size(), next_vcn, 4096, &extents, &next_vcn),
        "First fragmented page rejected");
    Require(next_vcn == 5 && extents.size() == 2, "Wrong first-page logical coverage");
    Require(AppendMftExtentPage(second_page.data(), second_page.size(), next_vcn, 4096, &extents, &next_vcn),
        "Continuation page rejected");
    Require(next_vcn == 7 && extents.size() == 3, "Continuation lost logical coverage");
    Require(extents[0].m_start_vcn == 0 && extents[0].m_start_lcn == 100 && extents[0].m_cluster_count == 2,
        "First extent mapping is incorrect");
    Require(extents[1].m_start_vcn == 2 && extents[1].m_start_lcn == 20 && extents[1].m_cluster_count == 3,
        "Fragmented extent mapping is incorrect");
    Require(extents[2].m_start_vcn == 5 && extents[2].m_start_lcn == 0 && extents[2].m_cluster_count == 2,
        "LCN zero was discarded");
}

/**
Ensures malformed pages do not partly mutate a previously accepted map.
**/
void RequireRejected(const std::vector<uint8_t>& page, uint64_t expected_vcn, uint32_t bytes_per_cluster = 4096)
{
    MftExtent existing;
    existing.m_start_lcn = 42;
    existing.m_cluster_count = expected_vcn;
    std::vector<MftExtent> extents;
    extents.push_back(existing);
    uint64_t next_vcn = expected_vcn;
    Require(!AppendMftExtentPage(page.data(), page.size(), expected_vcn, bytes_per_cluster, &extents, &next_vcn),
        "Malformed response accepted");
    Require(extents.size() == 1 && extents[0].m_start_lcn == 42 && next_vcn == expected_vcn,
        "Rejected page mutated the map or cursor");
}

/**
Exercises truncated buffers, invalid counts, gaps, overlaps, sparse runs,
non-progressing pagination and physical-offset overflow.
**/
void TestRejectedPages()
{
    RequireRejected(std::vector<uint8_t>(kHeaderSize - 1), 0);
    auto truncated = BuildPage(0,
    {
        std::make_pair(1, 20)
    });
    truncated.pop_back();
    RequireRejected(truncated, 0);
    const DWORD excessive_count = std::numeric_limits<DWORD>::max();
    auto bad_count = BuildPage(0,
    {
        std::make_pair(1, 20)
    });
    std::memcpy(bad_count.data(), &excessive_count, sizeof(excessive_count));
    RequireRejected(bad_count, 0);
    RequireRejected(BuildPage(0,
    {
    }), 0);
    RequireRejected(BuildPage(-1,
    {
        std::make_pair(1, 20)
    }), 0);
    RequireRejected(BuildPage(6,
    {
        std::make_pair(7, 20)
    }), 5);
    RequireRejected(BuildPage(4,
    {
        std::make_pair(7, 20)
    }), 5);
    RequireRejected(BuildPage(5,
    {
        std::make_pair(5, 20)
    }), 5);
    RequireRejected(BuildPage(5,
    {
        std::make_pair(6, 20), std::make_pair(5, 30)
    }), 5);
    RequireRejected(BuildPage(5,
    {
        std::make_pair(6, 20), std::make_pair(7, -1)
    }), 5);
    RequireRejected(BuildPage(0,
    {
        std::make_pair(1, std::numeric_limits<int64_t>::max())
    }), 0);
    RequireRejected(BuildPage(0,
    {
        std::make_pair(2, std::numeric_limits<int64_t>::max() / 4096)
    }), 0);
    RequireRejected(BuildPage(0,
    {
        std::make_pair(1, 20)
    }), 0, 0);
}

/**
Runs an optional read-only integration scan when explicitly passed --scan X.
This requires administrator access and is not part of the automated test run.
**/
int ScanVolume(char drive_letter)
{
    swift_disk::ntfs::NtfsVolume volume;
    if (!volume.Open(static_cast<wchar_t>(drive_letter)))
    {
        std::cerr << "Volume open failed: Windows error " << GetLastError()
            << " (administrator access is required).\n";
        return 2;
    }
    uint64_t records = 0;
    uint64_t file_records = 0;
    bool root_found = false;
    const bool ok = volume.EnumerateMftRecords(
        [&](uint8_t* record, size_t record_size, uint64_t frn)
        {
            uint32_t signature = 0;
            if (record_size >= sizeof(signature))
            {
                std::memcpy(&signature, record, sizeof(signature));
                if (signature == 0x454C4946)
                {
                    ++file_records;
                    root_found = root_found || frn == 5;
                }
            }
        }, &records);
    const uint64_t expected = volume.Info().mft_valid_data_length / volume.Info().bytes_per_file_record;
    std::cout << "Complete=" << ok << ", records=" << records << ", expected=" << expected
        << ", FILE records=" << file_records << ", root=" << root_found << '\n';
    return ok && records == expected && root_found ? 0 : 1;
}

} // namespace

/**
Runs extent-map regression checks, or an explicitly requested live-volume scan.
**/
int main(int argument_count, char** arguments)
{
    try
    {
        if (argument_count == 3 && std::strcmp(arguments[1], "--scan") == 0 && std::strlen(arguments[2]) == 1)
        {
            return ScanVolume(arguments[2][0]);
        }
        if (argument_count != 1)
        {
            std::cerr << "Usage: ntfs_extent_tests [--scan DRIVE_LETTER]\n";
            return 2;
        }
        TestFragmentedPages();
        TestRejectedPages();
        std::cout << "Extent-map regression checks passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
