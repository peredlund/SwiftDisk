// SPDX-FileCopyrightText: 2026 Per Edlund
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/file_tree.h"
#include "core/dir_scanner.h"
#include "core/ntfs/mft_parser.h"
#include "core/ntfs/mft_scanner.h"
#include "visualization/treemap.h"

#include <windows.h>
#include <winioctl.h>

#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{

/**
Fails a regression check in Debug and Release configurations.
**/
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

/**
Normalizes test-folder arguments, including a quote left by Windows command-line
parsing when a quoted path ends in a single backslash. A bare drive designator
means its root for these tests, rather than the drive's current directory.
**/
std::wstring NormalizeSizeTestPath(std::wstring path)
{
    while (!path.empty() && path.front() == L'"')
    {
        path.erase(path.begin());
    }
    while (!path.empty() && path.back() == L'"')
    {
        path.pop_back();
    }
    if (path.size() == 2 && path[1] == L':' &&
        ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')))
    {
        path.push_back(L'\\');
    }
    return path;
}

/**
Writes a typed field at a documented NTFS byte offset in a synthetic record.
**/
template <typename T>
void Put(std::vector<uint8_t>& record, size_t offset, T value)
{
    std::memcpy(record.data() + offset, &value, sizeof(value));
}

/**
Builds a raw FILE record with a single unnamed DATA attribute. This is entirely
in memory; it neither opens a volume nor creates a file.
**/
std::vector<uint8_t> BuildRecord(bool resident, uint16_t flags, uint64_t logical_size,
    uint64_t allocated_size, uint64_t physical_size, uint64_t start_vcn = 0)
{
    std::vector<uint8_t> record(1024);
    Put<uint32_t>(record, 0, 0x454C4946);
    Put<uint16_t>(record, 4, 48);
    Put<uint16_t>(record, 6, 3);
    Put<uint16_t>(record, 20, 56);
    Put<uint16_t>(record, 22, 1);
    Put<uint16_t>(record, 48, 0x1234);
    Put<uint16_t>(record, 510, 0x1234);
    Put<uint16_t>(record, 1022, 0x1234);

    constexpr size_t attr = 56;
    const uint32_t length = resident ? static_cast<uint32_t>((24 + logical_size + 7) & ~7ULL) : 80;
    Put<uint32_t>(record, attr, 0x80);
    Put<uint32_t>(record, attr + 4, length);
    Put<uint8_t>(record, attr + 8, resident ? 0 : 1);
    Put<uint16_t>(record, attr + 12, flags);
    if (resident)
    {
        Put<uint32_t>(record, attr + 16, static_cast<uint32_t>(logical_size));
        Put<uint16_t>(record, attr + 20, 24);
    }
    else
    {
        Put<uint64_t>(record, attr + 16, start_vcn);
        Put<uint16_t>(record, attr + 32, flags != 0 ? 72 : 64);
        Put<uint64_t>(record, attr + 40, allocated_size);
        Put<uint64_t>(record, attr + 48, logical_size);
        Put<uint64_t>(record, attr + 64, physical_size);
    }
    Put<uint32_t>(record, attr + length, 0xFFFFFFFF);
    return record;
}

/**
Verifies that each storage form yields independent logical and physical sizes,
and that continuation segments do not contribute invalid stream-wide totals.
**/
void TestParsedSizes()
{
    const auto check = [](std::vector<uint8_t> record, uint64_t logical, uint64_t physical)
    {
        swift_disk::ntfs::ParsedRecord parsed;
        Require(swift_disk::ntfs::ParseFileRecord(record.data(), record.size(), 512, 42, &parsed),
            "Synthetic size record rejected");
        Require(parsed.size == logical && parsed.m_size_on_disk == physical, "Incorrect NTFS size pair");
    };
    check(BuildRecord(true, 0, 42, 0, 0), 42, 0);
    check(BuildRecord(true, 0, 0, 0, 0), 0, 0);
    check(BuildRecord(false, 0, 5120, 8192, 999999), 5120, 8192);
    check(BuildRecord(false, 1, 1048576, 1048576, 16384), 1048576, 16384);
    check(BuildRecord(false, 0x8000, 1073741824, 1073741824, 4096), 1073741824, 4096);
    check(BuildRecord(false, 0x8000, 1073741824, 1073741824, 0), 1073741824, 0);
    check(BuildRecord(false, 0, 0, 4096, 0), 0, 4096);
    check(BuildRecord(false, 0x8000, 999999, 999999, 999999, 16), 0, 0);

    // A regular nonresident header needs only 64 bytes, not the optional field.
    auto short_header = BuildRecord(false, 0, 5120, 8192, 0);
    Put<uint32_t>(short_header, 56 + 4, 64);
    Put<uint32_t>(short_header, 56 + 64, 0xFFFFFFFF);
    check(short_header, 5120, 8192);

    // A valid empty DATA stream must stay empty even if FILE_NAME is stale.
    auto empty_with_name = BuildRecord(true, 0, 0, 0, 0);
    constexpr size_t name_attr = 80;
    Put<uint32_t>(empty_with_name, name_attr, 0x30);
    Put<uint32_t>(empty_with_name, name_attr + 4, 96);
    Put<uint32_t>(empty_with_name, name_attr + 16, 68);
    Put<uint16_t>(empty_with_name, name_attr + 20, 24);
    Put<uint64_t>(empty_with_name, name_attr + 24 + 48, 5242880);
    Put<uint8_t>(empty_with_name, name_attr + 24 + 64, 1);
    Put<uint8_t>(empty_with_name, name_attr + 24 + 65, 1);
    Put<wchar_t>(empty_with_name, name_attr + 24 + 66, L'x');
    Put<uint32_t>(empty_with_name, name_attr + 96, 0xFFFFFFFF);
    check(empty_with_name, 0, 0);
}

/**
Checks nested aggregation, mode-dependent ordering and areas, zero-allocation
files, and switching back without modifying the stored sizes.
**/
void TestTreeAndLayoutSizes()
{
    using namespace swift_disk;
    FileTree tree;
    Node root;
    root.id = 1;
    root.is_dir = true;
    tree.AddNode(root);
    tree.SetRootId(root.id);

    Node plain;
    plain.id = 2;
    plain.parent_id = 1;
    plain.name = L"plain";
    plain.size = 5120;
    plain.m_size_on_disk = 8192;
    tree.AddNode(plain);

    Node folder;
    folder.id = 3;
    folder.parent_id = 1;
    folder.is_dir = true;
    folder.name = L"folder";
    tree.AddNode(folder);

    Node compressed;
    compressed.id = 4;
    compressed.parent_id = 3;
    compressed.size = 1048576;
    compressed.m_size_on_disk = 4096;
    tree.AddNode(compressed);

    Node resident;
    resident.id = 5;
    resident.parent_id = 1;
    resident.size = 42;
    tree.AddNode(resident);
    tree.Finalize();
    Require(tree.GetNode(1)->size == 1053738 && tree.GetNode(1)->m_size_on_disk == 12288,
        "Root did not aggregate both size metrics");
    Require(tree.GetNode(3)->size == 1048576 && tree.GetNode(3)->m_size_on_disk == 4096,
        "Nested folder lost size totals");

    visualization::TreemapLayoutOptions options;
    options.max_rectangles = 1;
    auto rects = visualization::BuildTreemapLayout(tree, options);
    Require(rects.size() == 1 && rects[0].node_id == 3 && rects[0].size == 1048576,
        "Logical mode did not select the largest logical node");
    options.m_size_metric = SizeMetric::OnDisk;
    rects = visualization::BuildTreemapLayout(tree, options);
    Require(rects.size() == 1 && rects[0].node_id == 2 && rects[0].size == 8192,
        "Disk mode did not select the largest allocation");
    options.max_depth = 0;
    options.max_rectangles = 10;
    rects = visualization::BuildTreemapLayout(tree, options);
    Require(rects.size() == 2, "Zero-allocation file consumed disk-mode area");
    Require(rects[0].size == 8192 && rects[1].size == 4096, "Disk mode retained logical sizes");
    Require(std::abs(rects[0].width * rects[0].height / (rects[1].width * rects[1].height) - 2.0) < 0.000001,
        "Tile areas did not follow allocation sizes");
    options.m_size_metric = SizeMetric::Logical;
    rects = visualization::BuildTreemapLayout(tree, options);
    Require(rects.size() == 3 && tree.GetNode(1)->m_size_on_disk == 12288,
        "Switching metrics changed stored totals or lost resident content");
}

} // namespace

/**
Runs all in-memory logical/allocation size regression checks.
**/
void RunSizeTests()
{
    TestParsedSizes();
    TestTreeAndLayoutSizes();
}

/**
Checks a user-selected test folder against Windows file metadata. This optional
integration check only reads existing files; it creates or modifies nothing.
MFT mode requires an administrator terminal and a local NTFS path.
**/
int VerifyFileSizes(const wchar_t* path, bool use_mft)
{
    using namespace swift_disk;
    const std::wstring normalized_path = NormalizeSizeTestPath(path);
    const DWORD root_attributes = GetFileAttributesW(normalized_path.c_str());
    if (root_attributes == INVALID_FILE_ATTRIBUTES || (root_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        std::wcerr << L"Test folder does not exist or cannot be accessed: " << normalized_path << L'\n';
        return 2;
    }
    FileTree tree;
    std::wstring error;
    if (use_mft)
    {
        if (!ntfs::ScanPathMft(normalized_path, &tree, &error, nullptr))
        {
            std::wcerr << L"MFT scan failed: " << error << L'\n';
            return 1;
        }
    }
    else
    {
        Require(ScanDirectory(normalized_path, &tree, nullptr), "Directory scan failed");
        tree.Finalize();
    }

    uint64_t file_count = 0;
    uint64_t logical_total = 0;
    uint64_t disk_total = 0;
    wchar_t volume_path[MAX_PATH] = {};
    wchar_t file_system[MAX_PATH] = {};
    DWORD sectors = 0;
    DWORD bytes_per_sector = 0;
    DWORD free_clusters = 0;
    DWORD total_clusters = 0;
    Require(GetVolumePathNameW(normalized_path.c_str(), volume_path, MAX_PATH) &&
        GetDiskFreeSpaceW(volume_path, &sectors, &bytes_per_sector, &free_clusters, &total_clusters) &&
        GetVolumeInformationW(volume_path, nullptr, 0, nullptr, nullptr, nullptr, file_system, MAX_PATH),
        "Cannot query test-volume geometry");
    const uint64_t cluster_size = static_cast<uint64_t>(sectors) * bytes_per_sector;
    const bool is_ntfs = _wcsicmp(file_system, L"NTFS") == 0;
    for (const Node* node : tree.GetAllNodes())
    {
        if (node->is_dir)
        {
            continue;
        }
        const std::wstring file_path = tree.BuildPath(node->id);
        HANDLE file = CreateFileW(file_path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        Require(file != INVALID_HANDLE_VALUE, "Cannot open test file metadata");
        FILE_STANDARD_INFO standard = {};
        FILE_COMPRESSION_INFO compressed = {};
        const bool got_standard = GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)) != FALSE;
        const DWORD attributes = GetFileAttributesW(file_path.c_str());
        const bool compressed_or_sparse = (attributes & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE)) != 0;
        const bool got_compression = !compressed_or_sparse ||
            GetFileInformationByHandleEx(file, FileCompressionInfo, &compressed, sizeof(compressed)) != FALSE;
        STARTING_VCN_INPUT_BUFFER input = {};
        RETRIEVAL_POINTERS_BUFFER output = {};
        DWORD returned = 0;
        const bool resident = got_standard && !compressed_or_sparse &&
            ((is_ntfs && static_cast<uint64_t>(standard.AllocationSize.QuadPart) < cluster_size) ||
                (standard.EndOfFile.QuadPart < 4096 &&
                    !DeviceIoControl(file, FSCTL_GET_RETRIEVAL_POINTERS, &input, sizeof(input),
                        &output, sizeof(output), &returned, nullptr) && GetLastError() == ERROR_HANDLE_EOF));
        CloseHandle(file);
        Require(got_standard && got_compression && attributes != INVALID_FILE_ATTRIBUTES,
            "Windows metadata query failed");
        const uint64_t logical = static_cast<uint64_t>(standard.EndOfFile.QuadPart);
        const uint64_t physical = resident ? 0 : static_cast<uint64_t>(compressed_or_sparse
            ? compressed.CompressedFileSize.QuadPart : standard.AllocationSize.QuadPart);
        if (node->size != logical || node->m_size_on_disk != physical || node->m_size_on_disk_estimated)
        {
            std::wcerr << file_path << L": scanner=" << node->size << L'/' << node->m_size_on_disk
                << L", Windows=" << logical << L'/' << physical << L'\n';
            return 1;
        }
        logical_total += logical;
        disk_total += physical;
        ++file_count;
    }
    const Node* root = tree.GetNode(tree.RootId());
    Require(file_count > 0 && root && root->size == logical_total && root->m_size_on_disk == disk_total,
        "Empty test folder or incorrect folder totals");
    std::cout << "Verified " << file_count << " files: logical=" << logical_total << ", disk=" << disk_total << '\n';
    return 0;
}
