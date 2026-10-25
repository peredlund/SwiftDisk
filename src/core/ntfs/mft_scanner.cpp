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

} // namespace

/**
 Scans an NTFS volume by enumerating and parsing MFT records.
 Builds a FileTree keyed by FRN and merges extension-record sizes back
 to base records using non-additive max semantics.
 
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
    std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> pending_sizes;
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
            if (parsed.name.empty())
            {
                stats.records_no_name++;
                if (!parsed.is_dir && (parsed.size > 0 || parsed.m_size_on_disk > 0))
                {
                    if (parsed.base_frn != 0 && parsed.base_frn != parsed.frn)
                    {
                        // Extension record for a base file: merge using max to avoid
                        // double counting overlapping size metadata across segments.
                        if (Node* existing = tree->GetNodeMutable(parsed.base_frn))
                        {
                            existing->size = std::max(existing->size, parsed.size);
                            existing->m_size_on_disk = std::max(existing->m_size_on_disk, parsed.m_size_on_disk);
                        }
                        else
                        {
                            // Base record may appear later in scan order; store pending merge.
                            auto& pending = pending_sizes[parsed.base_frn];
                            pending.first = std::max(pending.first, parsed.size);
                            pending.second = std::max(pending.second, parsed.m_size_on_disk);
                        }
                        stats.extension_size_records++;
                        stats.extension_size_total += parsed.size;
                    }
                    else
                    {
                        stats.records_no_name_with_size++;
                        stats.records_no_name_size += parsed.size;
                    }
                }
                return;
            }

            if (frn == kRootFrn)
            {
                Node* root = tree->GetNodeMutable(kRootFrn);
                if (root)
                {
                    root->name = std::wstring(1, drive_letter) + L":";
                }
                return;
            }

            Node node;
            node.id = parsed.frn;
            node.parent_id = parsed.parent_frn;
            node.name = std::move(parsed.name);
            node.is_dir = parsed.is_dir;
            node.size = parsed.size;
            node.m_size_on_disk = parsed.m_size_on_disk;
            tree->AddNode(std::move(node));
            stats.records_added++;

            auto pending = pending_sizes.find(node.id);
            if (pending != pending_sizes.end())
            {
                Node* existing = tree->GetNodeMutable(node.id);
                if (existing)
                {
                    existing->size = std::max(existing->size, pending->second.first);
                    existing->m_size_on_disk = std::max(existing->m_size_on_disk, pending->second.second);
                }
                pending_sizes.erase(pending);
            }
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

    if (!pending_sizes.empty())
    {
        for (const auto& entry : pending_sizes)
        {
            stats.extension_missing_base_records++;
            stats.extension_missing_base_total += entry.second.first;
        }
    }

    tree->Finalize();
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
