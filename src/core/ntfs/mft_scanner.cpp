// SPDX-FileCopyrightText: 2026 Per Edlund
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/ntfs/mft_scanner.h"

#include "core/ntfs/mft_parser.h"
#include "core/ntfs/ntfs_volume.h"

#include <algorithm>
#include <cwchar>
#include <limits>
#include <unordered_map>
#include <utility>

namespace swift_disk::ntfs
{

namespace
{

/**
 Normalizes an NTFS scan path for case-insensitive component traversal and volume
 root comparison.

 Parameters:
   path: Drive-qualified path to normalize.

 Returns: Path with backslash separators, one trailing backslash removed when
 present, and an uppercase drive prefix.
 **/
std::wstring NormalizeScanPath(std::wstring path)
{
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (!path.empty() && path.size() > 3 && path.back() == L'\\')
    {
        path.pop_back();
    }
    if (path.size() == 3 && path[1] == L':' && path[2] == L'\\')
    {
        path.pop_back();
    }
    if (path.size() >= 2 && path[1] == L':')
    {
        path[0] = static_cast<wchar_t>(towupper(path[0]));
    }
    return path;
}

bool PathsEqualIgnoreCase(const std::wstring& left, const std::wstring& right)
{
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

/**
 Splits a relative Windows path into non-empty directory components while
 tolerating repeated separators.

 Parameters:
   path: Relative path to split.

 Returns: Path components in traversal order.
 **/
std::vector<std::wstring> SplitRelativePathComponents(const std::wstring& path)
{
    std::vector<std::wstring> components;
    size_t index = 0;
    while (index < path.size())
    {
        while (index < path.size() && path[index] == L'\\')
        {
            ++index;
        }
        if (index >= path.size())
        {
            break;
        }

        size_t next = path.find(L'\\', index);
        if (next == std::wstring::npos)
        {
            components.push_back(path.substr(index));
            break;
        }

        components.push_back(path.substr(index, next - index));
        index = next + 1;
    }
    return components;
}

/**
 Finds the first indexed child whose name matches case-insensitively.

 Parameters:
   tree: Tree containing the parent and child nodes.
   parent_id: Parent whose children should be searched.
   name: Child name to match.

 Returns: Matching child node, or null when no match exists.
 **/
const Node* FindChildByName(const FileTree& tree, uint64_t parent_id, const std::wstring& name)
{
    const std::vector<uint64_t>* child_ids = tree.GetChildIds(parent_id);
    if (!child_ids)
    {
        return nullptr;
    }

    for (uint64_t child_id : *child_ids)
    {
        const Node* child = tree.GetNode(child_id);
        if (child && PathsEqualIgnoreCase(child->name, name))
        {
            return child;
        }
    }
    return nullptr;
}

/**
 Extracts and uppercases the leading character from a drive-qualified path.

 Parameters:
   path: Path expected to begin with an X: prefix.
   drive_letter: Receives the normalized leading character.

 Returns: True when drive_letter is non-null and the second path character is ':'.
 **/
bool TryGetDriveLetterFromPath(const std::wstring& path, wchar_t* drive_letter)
{
    if (!drive_letter || path.size() < 2 || path[1] != L':')
    {
        return false;
    }
    *drive_letter = static_cast<wchar_t>(towupper(path[0]));
    return true;
}

/**
Combines metadata for one base file, preferring its long filename and actual
unnamed DATA sizes over FILE_NAME size fallbacks, including valid zero sizes.
WOF backing allocations are added independently; only their first segments
contribute, so continuation records do not count the same storage again.
**/
void MergeRecordMetadata(ParsedRecord& target, const ParsedRecord& source)
{
    uint64_t primary_allocation = target.m_size_on_disk - target.m_wof_size_on_disk;
    const uint64_t source_primary_allocation = source.m_size_on_disk - source.m_wof_size_on_disk;
    target.AddFileName(source.name, source.parent_frn, source.m_name_preference);
    for (const ParsedFileName& entry : source.m_additional_names)
    {
        target.AddFileName(entry.m_name, entry.m_parent_frn, entry.m_preference);
    }
    if (source.has_data_attribute && !target.has_data_attribute)
    {
        target.size = source.size;
        primary_allocation = source_primary_allocation;
        target.has_data_attribute = true;
    }
    else if (source.has_data_attribute == target.has_data_attribute)
    {
        target.size = std::max(target.size, source.size);
        primary_allocation = std::max(primary_allocation, source_primary_allocation);
    }
    target.m_wof_size_on_disk += source.m_wof_size_on_disk;
    target.m_size_on_disk = primary_allocation + target.m_wof_size_on_disk;
}

} // namespace

/**
Creates a merger that combines split records by file identity and retains
additional hard-link paths separately until the file's final sizes are known.
**/
MftRecordMerger::MftRecordMerger(FileTree& tree, MftScanStats& stats)
    : m_tree(tree), m_stats(stats)
{
}

/**
Adds parsed base or extension metadata using the base record's identity.
Extension records can supply sizes and legitimate hard-link paths; DOS aliases
do not create additional entries.
**/
void MftRecordMerger::AddRecord(ParsedRecord record)
{
    if (!record.in_use)
    {
        return;
    }
    const bool is_extension = record.base_frn != 0 && record.base_frn != record.frn;
    const uint64_t base_frn = is_extension ? record.base_frn : record.frn;
    if (record.name.empty())
    {
        ++m_stats.records_no_name;
        if (!is_extension && !record.is_dir && record.size > 0)
        {
            ++m_stats.records_no_name_with_size;
            m_stats.records_no_name_size += record.size;
        }
    }
    if (is_extension && !record.is_dir && (record.size > 0 || record.m_size_on_disk > 0))
    {
        ++m_stats.extension_size_records;
        m_stats.extension_size_total += record.size;
    }
    if (base_frn == m_tree.RootId())
    {
        return;
    }

    Node* existing = m_tree.GetNodeMutable(base_frn);
    if (is_extension && !existing)
    {
        auto [pending, inserted] = m_pending_records.try_emplace(base_frn, std::move(record));
        if (!inserted)
        {
            MergeRecordMetadata(pending->second, record);
        }
        return;
    }
    if (existing)
    {
        ParsedRecord merged;
        merged.name = existing->name;
        merged.parent_frn = existing->parent_id;
        merged.m_name_preference = existing->m_mft_name_preference;
        merged.has_data_attribute = existing->m_mft_has_data_attribute;
        merged.size = existing->size;
        merged.m_size_on_disk = existing->m_size_on_disk;
        merged.m_wof_size_on_disk = existing->m_mft_wof_size_on_disk;
        auto links = m_hard_links.find(base_frn);
        if (links != m_hard_links.end())
        {
            merged.m_additional_names = std::move(links->second);
        }
        MergeRecordMetadata(merged, record);
        Node updated = *existing;
        updated.name = std::move(merged.name);
        updated.parent_id = merged.parent_frn;
        updated.m_mft_name_preference = merged.m_name_preference;
        updated.m_mft_has_data_attribute = merged.has_data_attribute;
        updated.size = merged.size;
        updated.m_size_on_disk = merged.m_size_on_disk;
        updated.m_mft_wof_size_on_disk = merged.m_wof_size_on_disk;
        m_tree.AddNode(std::move(updated));
        if (!merged.m_additional_names.empty())
        {
            m_hard_links[base_frn] = std::move(merged.m_additional_names);
        }
        return;
    }

    auto pending = m_pending_records.find(base_frn);
    if (pending != m_pending_records.end())
    {
        MergeRecordMetadata(record, pending->second);
        m_pending_records.erase(pending);
    }
    Node node;
    node.id = base_frn;
    node.parent_id = record.parent_frn;
    node.name = std::move(record.name);
    node.is_dir = record.is_dir;
    node.m_mft_name_preference = record.m_name_preference;
    node.m_mft_has_data_attribute = record.has_data_attribute;
    node.size = record.size;
    node.m_size_on_disk = record.m_size_on_disk;
    node.m_mft_wof_size_on_disk = record.m_wof_size_on_disk;
    m_tree.AddNode(std::move(node));
    if (!record.m_additional_names.empty())
    {
        m_hard_links[base_frn] = std::move(record.m_additional_names);
    }
    ++m_stats.records_added;
}

/**
Discards orphan extension metadata and adds each distinct hard-link path with
the file's full logical size. Allocation stays on the preferred path so shared
storage is counted once. Additional IDs lie outside the 48-bit MFT FRN range.
**/
void MftRecordMerger::Finalize()
{
    for (const auto& entry : m_pending_records)
    {
        ++m_stats.extension_missing_base_records;
        m_stats.extension_missing_base_total += entry.second.size;
    }
    m_pending_records.clear();
    uint64_t link_id = uint64_t{1} << 63;
    for (const auto& [base_frn, names] : m_hard_links)
    {
        const Node* file = m_tree.GetNode(base_frn);
        if (!file || file->is_dir)
        {
            continue;
        }
        const Node original = *file;
        for (const ParsedFileName& entry : names)
        {
            Node link = original;
            link.id = link_id++;
            link.parent_id = entry.m_parent_frn;
            link.name = entry.m_name;
            link.m_mft_name_preference = entry.m_preference;
            link.m_size_on_disk = 0;
            link.m_mft_wof_size_on_disk = 0;
            m_tree.AddNode(std::move(link));
        }
    }
    m_hard_links.clear();
    m_tree.Finalize();
}

/**
 Scans an NTFS volume by enumerating and parsing MFT records.
 Builds a FileTree with each hard-link path, merging extension-record metadata
 without counting DOS aliases or continuation records as separate files.
 
 Parameters:
   drive_letter: Volume letter to scan.
   tree: Destination file tree.
   error: Optional error message output.
   out_stats: Optional scan statistics output.
 
 Returns: true on success, otherwise false.
 **/
bool ScanVolumeMft(wchar_t drive_letter, FileTree* tree, std::wstring* error, MftScanStats* out_stats)
{
    if (!tree)
    {
        if (error)
        {
            *error = L"FileTree is null.";
        }
        return false;
    }

    MftScanStats stats;

    NtfsVolume volume;
    if (!volume.Open(drive_letter))
    {
        if (error)
        {
            *error = L"Failed to open volume.";
        }
        return false;
    }

    if (!volume.IsReadOnlyAccess())
    {
        if (error)
        {
            *error = L"Refusing to scan: volume opened with write access.";
        }
        return false;
    }

    if (!volume.IsNtfs())
    {
        if (error)
        {
            *error = L"Volume is not NTFS.";
        }
        return false;
    }

    stats.mft_valid_data_length = volume.Info().mft_valid_data_length;
    stats.bytes_per_file_record = volume.Info().bytes_per_file_record;
    if (stats.bytes_per_file_record > 0 && stats.mft_valid_data_length > 0)
    {
        const uint64_t estimated_records = stats.mft_valid_data_length / stats.bytes_per_file_record;
        const size_t reserve_count = static_cast<size_t>(std::min<uint64_t>(
            estimated_records + 1,
            static_cast<uint64_t>(std::numeric_limits<size_t>::max())));
        tree->Reserve(reserve_count);
    }

    constexpr uint64_t kRootFrn = 5;
    Node root_node;
    root_node.id = kRootFrn;
    root_node.parent_id = 0;
    root_node.is_dir = true;
    root_node.name = std::wstring(1, drive_letter) + L":";
    tree->AddNode(root_node);
    tree->SetRootId(kRootFrn);

    const uint32_t bytes_per_sector = volume.Info().bytes_per_sector;
    MftRecordMerger merger(*tree, stats);
    bool ok = volume.EnumerateMftRecords(
        [&](uint8_t* record, size_t record_size, uint64_t frn)
        {
            stats.records_read++;
            ParsedRecord parsed;
            if (!ParseFileRecord(record, record_size, bytes_per_sector, frn, &parsed))
            {
                stats.records_parse_failed++;
                return;
            }
            stats.records_parsed++;
            if (!parsed.in_use)
            {
                return;
            }
            stats.records_in_use++;
            merger.AddRecord(std::move(parsed));
        },
        nullptr);

    if (!ok)
    {
        if (error)
        {
            *error = L"Failed while enumerating the MFT.";
        }
        return false;
    }

    merger.Finalize();
    if (out_stats)
    {
        *out_stats = stats;
    }
    return true;
}

/**
 Scans an NTFS path by reading the full volume MFT and then projecting
 the requested subtree into the destination tree.
 
 Parameters:
   root_path: Directory or file path that should become the scan root.
   tree: Destination file tree.
   error: Optional error message output.
   out_stats: Optional scan statistics output for the underlying volume scan.
 
 Returns: true on success, otherwise false.
 **/
bool ScanPathMft(const std::wstring& root_path, FileTree* tree, std::wstring* error, MftScanStats* out_stats)
{
    if (!tree)
    {
        if (error)
        {
            *error = L"FileTree is null.";
        }
        return false;
    }

    std::wstring normalized_root = NormalizeScanPath(root_path);
    wchar_t drive_letter = 0;
    if (!TryGetDriveLetterFromPath(normalized_root, &drive_letter))
    {
        if (error)
        {
            *error = L"Path must start with a drive letter.";
        }
        return false;
    }

    if (normalized_root == std::wstring(1, drive_letter) + L":")
    {
        return ScanVolumeMft(drive_letter, tree, error, out_stats);
    }

    FileTree full_tree;
    MftScanStats stats;
    if (!ScanVolumeMft(drive_letter, &full_tree, error, &stats))
    {
        return false;
    }

    const Node* subtree_root = full_tree.GetNode(full_tree.RootId());
    std::wstring relative_path = normalized_root.substr(2);
    for (const std::wstring& component : SplitRelativePathComponents(relative_path))
    {
        if (!subtree_root)
        {
            break;
        }
        subtree_root = FindChildByName(full_tree, subtree_root->id, component);
    }

    if (!subtree_root)
    {
        if (error)
        {
            *error = L"Requested path was not found in the NTFS tree.";
        }
        return false;
    }

    tree->SetRootId(subtree_root->id);
    std::vector<uint64_t> stack;
    stack.push_back(subtree_root->id);
    while (!stack.empty())
    {
        uint64_t node_id = stack.back();
        stack.pop_back();

        const Node* node = full_tree.GetNode(node_id);
        if (!node)
        {
            continue;
        }

        Node copy = *node;
        if (copy.id == subtree_root->id)
        {
            // Detach the projected subtree from its volume parent and retain the
            // exact requested path as the new root's display name.
            copy.parent_id = 0;
            copy.name = normalized_root;
        }
        tree->AddNode(std::move(copy));

        const std::vector<uint64_t>* child_ids = full_tree.GetChildIds(node_id);
        if (!child_ids)
        {
            continue;
        }
        for (uint64_t child_id : *child_ids)
        {
            stack.push_back(child_id);
        }
    }

    tree->RefreshPathHashes();
    if (out_stats)
    {
        *out_stats = stats;
    }
    return true;
}

} // namespace swift_disk::ntfs
