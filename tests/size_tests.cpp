// SPDX-FileCopyrightText: 2026 Per Edlund
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/file_tree.h"
#include "core/dir_scanner.h"
#include "core/ntfs/mft_parser.h"
#include "core/ntfs/mft_scanner.h"
#include "visualization/treemap.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
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

/**
Appends a FILE_NAME attribute to a synthetic MFT record, including its namespace
and cached logical size. All bytes remain in memory.
**/
void AddFileName(std::vector<uint8_t>& record, const std::wstring& name,
    uint8_t name_type, uint64_t cached_size, uint64_t parent_frn = 5)
{
    size_t offset = 56;
    uint32_t type = 0;
    std::memcpy(&type, record.data() + offset, sizeof(type));
    while (type != 0xFFFFFFFF)
    {
        uint32_t length = 0;
        std::memcpy(&length, record.data() + offset + 4, sizeof(length));
        Require(length > 0 && offset + length + 4 <= record.size(), "Invalid synthetic attribute");
        offset += length;
        std::memcpy(&type, record.data() + offset, sizeof(type));
    }
    const uint32_t value_size = static_cast<uint32_t>(66 + name.size() * sizeof(wchar_t));
    const uint32_t length = (24 + value_size + 7) & ~7U;
    Require(offset + length + 4 <= record.size(), "Synthetic FILE_NAME does not fit");
    Put<uint32_t>(record, offset, 0x30);
    Put<uint32_t>(record, offset + 4, length);
    Put<uint32_t>(record, offset + 16, value_size);
    Put<uint16_t>(record, offset + 20, 24);
    Put<uint64_t>(record, offset + 24, parent_frn);
    Put<uint64_t>(record, offset + 24 + 48, cached_size);
    Put<uint8_t>(record, offset + 24 + 64, static_cast<uint8_t>(name.size()));
    Put<uint8_t>(record, offset + 24 + 65, name_type);
    std::memcpy(record.data() + offset + 24 + 66, name.data(), name.size() * sizeof(wchar_t));
    Put<uint32_t>(record, offset + length, 0xFFFFFFFF);
}

/**
Reproduces a long filename and DOS alias split across MFT records. Verifies one
file, correct names/parents and both size totals, regardless of scan order.
**/
void TestSplitMftRecords()
{
    using namespace swift_disk;
    using namespace swift_disk::ntfs;
    constexpr uint64_t logical = 11054304;
    constexpr uint64_t allocated = 11055104;
    const auto name_record = [](const std::wstring& name, uint8_t name_type, uint64_t cached_size,
        uint64_t parent_frn = 5)
    {
        auto record = BuildRecord(true, 0, 0, 0, 0);
        Put<uint32_t>(record, 56, 0xFFFFFFFF);
        AddFileName(record, name, name_type, cached_size, parent_frn);
        return record;
    };
    const auto check = [&](std::vector<uint8_t> base, std::vector<uint8_t> extension,
        uint64_t expected_logical, uint64_t expected_allocated, uint64_t expected_parent = 5)
    {
        Put<uint64_t>(extension, 32, 42);
        for (bool extension_first : {false, true})
        {
            FileTree tree;
            Node root;
            root.id = 5;
            root.name = L"C:";
            root.is_dir = true;
            tree.AddNode(root);
            tree.SetRootId(root.id);
            Node folder;
            folder.id = 7;
            folder.parent_id = 5;
            folder.name = L"folder";
            folder.is_dir = true;
            tree.AddNode(folder);
            MftScanStats stats;
            MftRecordMerger merger(tree, stats);
            const auto add = [&](std::vector<uint8_t> bytes, uint64_t frn)
            {
                ParsedRecord record;
                Require(ParseFileRecord(bytes.data(), bytes.size(), 512, frn, &record), "Split record rejected");
                merger.AddRecord(std::move(record));
            };
            if (extension_first)
            {
                add(extension, 99);
                add(base, 42);
            }
            else
            {
                add(base, 42);
                add(extension, 99);
            }
            merger.Finalize();
            const Node* file = tree.GetNode(42);
            Require(file && !tree.GetNode(99) && tree.ComputeStats().file_count == 1,
                "Extension became a duplicate file");
            Require(file->name == L"VoiceClarity.dll" && file->parent_id == expected_parent,
                "Long filename or parent was lost across records");
            Require(file->size == expected_logical && file->m_size_on_disk == expected_allocated,
                "Split DATA metadata lost authoritative sizes");
            Require(tree.GetNode(5)->size == expected_logical && tree.GetNode(5)->m_size_on_disk == expected_allocated,
                "Split record was counted twice in directory totals");
            const auto* children = tree.GetChildIds(expected_parent);
            Require(children && std::count(children->begin(), children->end(), 42) == 1,
                "Merged file appeared twice in its parent index");
            for (SizeMetric metric : {SizeMetric::Logical, SizeMetric::OnDisk})
            {
                visualization::TreemapLayoutOptions options;
                options.max_depth = 0;
                options.m_size_metric = metric;
                const auto rects = visualization::BuildTreemapLayout(tree, options);
                const uint64_t expected = metric == SizeMetric::Logical ? expected_logical : expected_allocated;
                Require(rects.size() == 1 && rects[0].size == expected,
                    "Duplicate tile or incorrect area after split record merge");
            }
        }
    };

    auto base = BuildRecord(false, 0, logical, allocated, 0);
    AddFileName(base, L"VoiceClarity.dll", 1, logical);
    check(base, name_record(L"VOICEC~1.DLL", 2, logical), logical, allocated);

    base = BuildRecord(false, 0, logical, allocated, 0);
    AddFileName(base, L"VOICEC~1.DLL", 2, logical);
    check(base, name_record(L"VoiceClarity.dll", 1, logical, 7), logical, allocated, 7);
    check(BuildRecord(false, 0, logical, allocated, 0),
        name_record(L"VoiceClarity.dll", 1, logical), logical, allocated);

    base = BuildRecord(true, 0, 0, 0, 0);
    AddFileName(base, L"VoiceClarity.dll", 1, logical);
    check(base, name_record(L"VOICEC~1.DLL", 2, logical), 0, 0);
    check(name_record(L"VoiceClarity.dll", 1, logical), BuildRecord(true, 0, 0, 0, 0), 0, 0);
    check(name_record(L"VoiceClarity.dll", 1, logical),
        BuildRecord(false, 0, logical, allocated, 0), logical, allocated);
}

/**
Verifies logical sizes for hard links in the same or different folders, with
names split across records in any scan order. DOS aliases and repeated names
must not add entries, and late DATA metadata must reach every real path.
**/
void TestHardLinkSizes()
{
    using namespace swift_disk;
    using namespace swift_disk::ntfs;
    constexpr uint64_t logical = 945024;
    constexpr uint64_t allocated = 946176;
    for (bool names_in_base : {false, true})
    {
        for (bool data_in_base : {false, true})
        {
            for (uint8_t other_namespace : {uint8_t{0}, uint8_t{1}, uint8_t{3}})
            {
                auto names = BuildRecord(true, 0, 0, 0, 0);
                Put<uint32_t>(names, 56, 0xFFFFFFFF);
                AddFileName(names, L"CATALO~1.CAT", 2, logical, 7);
                AddFileName(names, L"catalog.cat", 1, logical, 7);
                AddFileName(names, L"catalog.cat", 1, logical, 7);
                AddFileName(names, L"catalog.cat", 1, logical, 8);
                AddFileName(names, L"another.cat", other_namespace, logical, 8);
                auto data = BuildRecord(false, 0, logical, allocated, 0);
                // Cached filename sizes can be stale; DATA is authoritative.
                AddFileName(data, L"catalog.cat", 1, logical + 4096, 7);
                std::vector<std::vector<uint8_t>> records(3, BuildRecord(true, 0, 0, 0, 0));
                for (auto& record : records)
                {
                    Put<uint32_t>(record, 56, 0xFFFFFFFF);
                }
                records[names_in_base ? 0 : 1] = names;
                if (data_in_base && names_in_base)
                {
                    records[0] = data;
                    // Exercise multiple names in the same record as DATA too.
                    AddFileName(records[0], L"CATALO~1.CAT", 2, logical, 7);
                    AddFileName(records[0], L"catalog.cat", 1, logical, 8);
                    AddFileName(records[0], L"another.cat", other_namespace, logical, 8);
                    records[1] = names;
                }
                else
                {
                    records[data_in_base ? 0 : 2] = data;
                }
                Put<uint64_t>(records[1], 32, 42);
                Put<uint64_t>(records[2], 32, 42);
                std::vector<size_t> order = {0, 1, 2};
                do
                {
                    FileTree tree;
                    Node root;
                    root.id = 5;
                    root.is_dir = true;
                    root.name = L"C:";
                    tree.AddNode(root);
                    tree.SetRootId(5);
                    for (uint64_t folder_id : {7ULL, 8ULL})
                    {
                        Node folder;
                        folder.id = folder_id;
                        folder.parent_id = 5;
                        folder.is_dir = true;
                        folder.name = folder_id == 7 ? L"Catalogs" : L"Packages";
                        tree.AddNode(folder);
                    }
                    MftScanStats stats;
                    MftRecordMerger merger(tree, stats);
                    for (size_t index : order)
                    {
                        auto bytes = records[index];
                        for (size_t sector = 1; sector <= 2; ++sector)
                        {
                            uint16_t trailer = 0;
                            std::memcpy(&trailer, bytes.data() + sector * 512 - 2, sizeof(trailer));
                            Put<uint16_t>(bytes, 48 + sector * 2, trailer);
                            Put<uint16_t>(bytes, sector * 512 - 2, 0x1234);
                        }
                        ParsedRecord parsed;
                        Require(ParseFileRecord(bytes.data(), bytes.size(), 512,
                            index == 0 ? 42 : 98 + index, &parsed), "Hard-link record rejected");
                        merger.AddRecord(std::move(parsed));
                    }
                    merger.Finalize();
                    Require(tree.ComputeStats().file_count == 3 && !tree.GetNode(99) && !tree.GetNode(100),
                        "Hard link missing or DOS/extension entry counted twice");
                    Require(tree.GetNode(7)->size == logical && tree.GetNode(8)->size == logical * 2,
                        "Folder did not receive every hard link's full logical size");
                    Require(tree.GetNode(5)->size == logical * 3 && tree.GetNode(5)->m_size_on_disk == allocated,
                        "Hard-link totals lost logical content or duplicated allocation");
                    std::vector<std::wstring> paths;
                    size_t allocated_paths = 0;
                    for (const Node* file : tree.GetAllNodes())
                    {
                        if (file->is_dir)
                        {
                            continue;
                        }
                        Require(file->size == logical, "Hard link retained a stale FILE_NAME size");
                        paths.push_back(tree.BuildPath(file->id));
                        allocated_paths += file->m_size_on_disk != 0 ? 1 : 0;
                    }
                    std::sort(paths.begin(), paths.end());
                    Require(paths == std::vector<std::wstring>
                    {
                        L"C:\\Catalogs\\catalog.cat", L"C:\\Packages\\another.cat", L"C:\\Packages\\catalog.cat"
                    } && allocated_paths == 1, "Hard-link paths or allocation owner are incorrect");
                    for (SizeMetric metric : {SizeMetric::Logical, SizeMetric::OnDisk})
                    {
                        visualization::TreemapLayoutOptions options;
                        options.max_depth = 1;
                        options.m_size_metric = metric;
                        const auto rects = visualization::BuildTreemapLayout(tree, options);
                        const auto file_tiles = std::count_if(rects.begin(), rects.end(),
                            [](const visualization::TreemapRect& rect)
                            {
                                return !rect.is_dir;
                            });
                        Require(file_tiles == (metric == SizeMetric::Logical ? 3 : 1),
                            "Treemap lost a logical hard link or duplicated allocated storage");
                    }
                }
                while (std::next_permutation(order.begin(), order.end()));
            }
        }
    }
}

/**
Appends a named DATA attribute to an in-memory record. Header sizes and name
offsets cover ordinary, compressed, sparse, resident, and continuation streams.
**/
void AddNamedData(std::vector<uint8_t>& record, const std::wstring& name,
    uint64_t logical_size, uint64_t allocated_size, uint16_t flags = 0,
    uint64_t physical_size = 0, uint64_t start_vcn = 0, bool resident = false)
{
    size_t offset = 56;
    uint32_t type = 0;
    std::memcpy(&type, record.data() + offset, sizeof(type));
    while (type != 0xFFFFFFFF)
    {
        uint32_t length = 0;
        std::memcpy(&length, record.data() + offset + 4, sizeof(length));
        Require(length > 0 && offset + length + 4 <= record.size(), "Invalid synthetic DATA attribute");
        offset += length;
        std::memcpy(&type, record.data() + offset, sizeof(type));
    }
    const uint16_t header_size = resident ? 24 : (flags == 0 ? 64 : 72);
    const uint16_t payload_offset = static_cast<uint16_t>((header_size + name.size() * sizeof(wchar_t) + 7) & ~7ULL);
    const uint32_t length = static_cast<uint32_t>((payload_offset + (resident ? logical_size : 8) + 7) & ~7ULL);
    Require(offset + length + 4 <= record.size(), "Synthetic named DATA does not fit");
    std::fill(record.begin() + offset, record.begin() + offset + length + 4, uint8_t{0});
    Put<uint32_t>(record, offset, 0x80);
    Put<uint32_t>(record, offset + 4, length);
    Put<uint8_t>(record, offset + 8, resident ? 0 : 1);
    Put<uint8_t>(record, offset + 9, static_cast<uint8_t>(name.size()));
    Put<uint16_t>(record, offset + 10, header_size);
    Put<uint16_t>(record, offset + 12, flags);
    std::memcpy(record.data() + offset + header_size, name.data(), name.size() * sizeof(wchar_t));
    if (resident)
    {
        Put<uint32_t>(record, offset + 16, static_cast<uint32_t>(logical_size));
        Put<uint16_t>(record, offset + 20, payload_offset);
    }
    else
    {
        Put<uint64_t>(record, offset + 16, start_vcn);
        Put<uint16_t>(record, offset + 32, payload_offset);
        Put<uint64_t>(record, offset + 40, allocated_size);
        Put<uint64_t>(record, offset + 48, logical_size);
        if (flags != 0)
        {
            Put<uint64_t>(record, offset + 64, physical_size);
        }
    }
    Put<uint32_t>(record, offset + length, 0xFFFFFFFF);
}

/**
Checks WOF backing storage and exclusion of unrelated named streams. The
reported DLL sizes reproduce a sparse primary stream backed by compressed data.
**/
void TestWofBackingSizes()
{
    using namespace swift_disk;
    using namespace swift_disk::ntfs;
    constexpr uint64_t logical = 146376504;
    constexpr uint64_t backing = 48447488;
    const auto check = [&](std::vector<uint8_t> base, std::vector<uint8_t> extension,
        uint64_t expected_logical, uint64_t expected_disk)
    {
        AddFileName(base, L"Microsoft.Graph.Beta.dll", 1, expected_logical);
        Put<uint64_t>(extension, 32, 42);
        for (bool extension_first : {false, true})
        {
            FileTree tree;
            Node root;
            root.id = 5;
            root.is_dir = true;
            root.name = L"C:";
            tree.AddNode(root);
            tree.SetRootId(5);
            MftScanStats stats;
            MftRecordMerger merger(tree, stats);
            const auto add = [&](std::vector<uint8_t> bytes, uint64_t frn)
            {
                // Protect sector trailers even if an appended attribute crosses one.
                for (size_t sector = 1; sector <= 2; ++sector)
                {
                    uint16_t trailer = 0;
                    std::memcpy(&trailer, bytes.data() + sector * 512 - 2, sizeof(trailer));
                    Put<uint16_t>(bytes, 48 + sector * 2, trailer);
                    Put<uint16_t>(bytes, sector * 512 - 2, 0x1234);
                }
                ParsedRecord parsed;
                Require(ParseFileRecord(bytes.data(), bytes.size(), 512, frn, &parsed), "Named stream record rejected");
                merger.AddRecord(std::move(parsed));
            };
            if (extension_first)
            {
                add(extension, 99);
                add(base, 42);
            }
            else
            {
                add(base, 42);
                add(extension, 99);
            }
            merger.Finalize();
            const Node* file = tree.GetNode(42);
            Require(file && !tree.GetNode(99) && file->size == expected_logical && file->m_size_on_disk == expected_disk,
                "WOF storage was lost, duplicated, or added to logical size");
            Require(tree.GetNode(5)->size == expected_logical && tree.GetNode(5)->m_size_on_disk == expected_disk,
                "Directory did not aggregate WOF backing allocation");
            visualization::TreemapLayoutOptions options;
            options.m_size_metric = SizeMetric::OnDisk;
            const auto rects = visualization::BuildTreemapLayout(tree, options);
            Require(rects.size() == 1 && rects[0].size == expected_disk &&
                rects[0].node_id == (expected_disk > 0 ? 42 : 5),
                "Incorrect tile after WOF backing allocation");
        }
    };
    auto empty_extension = BuildRecord(true, 0, 0, 0, 0);
    Put<uint32_t>(empty_extension, 56, 0xFFFFFFFF);
    auto base = BuildRecord(false, 0x8000, logical, logical, 0);
    AddNamedData(base, L"WofCompressedData", backing, backing);
    check(base, empty_extension, logical, backing);

    auto extension = empty_extension;
    AddNamedData(extension, L"WofCompressedData", backing, backing);
    check(BuildRecord(false, 0x8000, logical, logical, 0), extension, logical, backing);

    // Unrelated named streams and noninitial WOF segments do not add allocation.
    base = BuildRecord(false, 0, 5120, 8192, 0);
    AddNamedData(base, L"extra", 1000, 4096);
    extension = empty_extension;
    AddNamedData(extension, L"compressed", 1048576, 1048576, 1, 16384);
    AddNamedData(extension, L"sparse", 1048576, 1048576, 0x8000, 4096);
    AddNamedData(extension, L"extra", 999999, 999999, 0, 0, 16);
    AddNamedData(extension, L"WofCompressedData", 999999, 999999, 0, 0, 16);
    AddNamedData(extension, L"resident", 42, 0, 0, 0, 0, true);
    check(base, extension, 5120, 8192);

    // A real empty primary stream stays empty while WOF backing still counts.
    base = BuildRecord(true, 0, 0, 0, 0);
    extension = empty_extension;
    AddNamedData(extension, L"WofCompressedData", 4096, 4096);
    check(base, extension, 0, 4096);
}

/**
Reproduces the special bad-cluster stream: a volume-sized virtual mapping can
have holes without the ordinary sparse-attribute flag. It is not file content
or WOF backing storage and must not inflate displayed disk usage.
**/
void TestBadClusterStream()
{
    using namespace swift_disk::ntfs;
    constexpr uint64_t volume_size = 232ULL * 1024 * 1024 * 1024;
    auto record = BuildRecord(true, 0, 0, 0, 0);
    AddNamedData(record, L"$Bad", volume_size, volume_size);
    // The named attribute follows the empty resident DATA attribute at byte 80.
    constexpr size_t run_offset = 80 + 72;
    Put<uint8_t>(record, run_offset, 0x04);
    Put<uint32_t>(record, run_offset + 1, static_cast<uint32_t>(volume_size / 4096));
    Put<uint8_t>(record, run_offset + 5, 0);
    AddFileName(record, L"$BadClus", 3, 0);
    ParsedRecord parsed;
    Require(ParseFileRecord(record.data(), record.size(), 512, 8, &parsed), "Bad-cluster record rejected");
    Require(parsed.size == 0 && parsed.m_size_on_disk == 0,
        "Virtual $BadClus stream inflated displayed disk usage");
}

} // namespace

/**
Runs all in-memory logical/allocation size regression checks.
**/
void RunSizeTests()
{
    TestBadClusterStream();
    TestParsedSizes();
    TestTreeAndLayoutSizes();
    TestSplitMftRecords();
    TestHardLinkSizes();
    TestWofBackingSizes();
}

/**
Checks a user-selected test folder against Windows file metadata. This optional
integration check only reads existing files; it creates or modifies nothing.
MFT mode requires an administrator terminal and a local NTFS path.
**/
int VerifyFileSizes(const wchar_t* path, bool use_mft, bool logical_only)
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

    if (logical_only)
    {
        // Enumerate independently so missing hard-link paths cannot pass just
        // because every file retained by the MFT scanner has the right size.
        FileTree expected;
        Require(ScanDirectory(normalized_path, &expected, nullptr), "Reference directory scan failed");
        expected.Finalize();
        std::unordered_map<std::wstring, uint64_t> actual_sizes;
        for (const Node* node : tree.GetAllNodes())
        {
            if (!node->is_dir)
            {
                Require(actual_sizes.emplace(tree.BuildPath(node->id), node->size).second,
                    "MFT scan returned a duplicate path");
            }
        }
        uint64_t file_count = 0;
        for (const Node* node : expected.GetAllNodes())
        {
            if (node->is_dir)
            {
                continue;
            }
            const std::wstring file_path = expected.BuildPath(node->id);
            const auto actual = actual_sizes.find(file_path);
            if (actual == actual_sizes.end() || actual->second != node->size)
            {
                std::wcerr << L"Missing path or incorrect logical size: " << file_path << L'\n';
                return 1;
            }
            ++file_count;
        }
        const Node* root = tree.GetNode(tree.RootId());
        const Node* expected_root = expected.GetNode(expected.RootId());
        Require(file_count == actual_sizes.size() && root && expected_root && root->size == expected_root->size,
            "Unexpected MFT entries or incorrect logical folder total");
        std::cout << "Verified " << file_count << " file paths: logical=" << root->size << '\n';
        return 0;
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
