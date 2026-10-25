// SPDX-FileCopyrightText: 2026 Per Edlund
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/dir_scanner.h"

#include <windows.h>
#include <winioctl.h>

#include <cwchar>
#include <vector>

namespace swift_disk
{

/**
 Normalizes the root path used for display and traversal.
 
 Parameters:
   path: Input path.
 
 Returns: Path with one trailing separator removed when longer than a drive
 root form.
 **/
static std::wstring NormalizeRootName(std::wstring path)
{
    if (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/'))
    {
        path.pop_back();
    }
    return path;
}

/**
Reads allocated file storage through Windows. If metadata cannot be queried,
retains a cluster-rounded estimate and marks it for display in the tooltip.
**/
static void ReadSizeOnDisk(const std::wstring& path, DWORD attributes, uint64_t cluster_size, bool is_ntfs, Node* node)
{
    if ((attributes & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE)) != 0)
    {
        DWORD high = 0;
        SetLastError(ERROR_SUCCESS);
        const DWORD low = GetCompressedFileSizeW(path.c_str(), &high);
        if (low != INVALID_FILE_SIZE || GetLastError() == ERROR_SUCCESS)
        {
            node->m_size_on_disk = (static_cast<uint64_t>(high) << 32) | low;
            return;
        }
    }
    else
    {
        HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            FILE_STANDARD_INFO info = {};
            const bool ok = GetFileInformationByHandleEx(file, FileStandardInfo, &info, sizeof(info)) != FALSE;
            bool resident = ok && is_ntfs && cluster_size != 0 && info.AllocationSize.QuadPart >= 0 &&
                static_cast<uint64_t>(info.AllocationSize.QuadPart) < cluster_size;
            if (ok && !resident && info.AllocationSize.QuadPart > 0 && node->size < 4096)
            {
                // NTFS can report resident value bytes as AllocationSize, even
                // though they occupy no separate data clusters.
                STARTING_VCN_INPUT_BUFFER input = {};
                RETRIEVAL_POINTERS_BUFFER output = {};
                DWORD returned = 0;
                resident = !DeviceIoControl(file, FSCTL_GET_RETRIEVAL_POINTERS,
                    &input, sizeof(input), &output, sizeof(output), &returned, nullptr) &&
                    GetLastError() == ERROR_HANDLE_EOF;
            }
            CloseHandle(file);
            if (ok && info.AllocationSize.QuadPart >= 0)
            {
                node->m_size_on_disk = resident ? 0 : static_cast<uint64_t>(info.AllocationSize.QuadPart);
                return;
            }
        }
    }

    node->m_size_on_disk = node->size;
    if (cluster_size != 0 && node->size % cluster_size != 0)
    {
        node->m_size_on_disk += cluster_size - node->size % cluster_size;
    }
    node->m_size_on_disk_estimated = true;
}

/**
 Scans a directory tree using Win32 enumeration APIs.
 Builds a FileTree where file sizes come from directory entry metadata.
 
 Parameters:
   root_path: Root path to scan.
   tree: Destination file tree.
   out_root_id: Optional output for assigned root id.
 
 Returns: false when tree is null or root_path is empty; otherwise true.
 Directories that cannot be enumerated are skipped.
 **/
bool ScanDirectory(const std::wstring& root_path, FileTree* tree, uint64_t* out_root_id)
{
    if (!tree || root_path.empty())
    {
        return false;
    }

    std::wstring normalized_root = NormalizeRootName(root_path);
    wchar_t volume_path[MAX_PATH] = {};
    DWORD sectors_per_cluster = 0;
    DWORD bytes_per_sector = 0;
    DWORD free_clusters = 0;
    DWORD total_clusters = 0;
    uint64_t cluster_size = 0;
    wchar_t file_system[MAX_PATH] = {};
    bool is_ntfs = false;
    if (GetVolumePathNameW(root_path.c_str(), volume_path, MAX_PATH) &&
        GetDiskFreeSpaceW(volume_path, &sectors_per_cluster, &bytes_per_sector, &free_clusters, &total_clusters))
    {
        cluster_size = static_cast<uint64_t>(sectors_per_cluster) * bytes_per_sector;
        is_ntfs = GetVolumeInformationW(volume_path, nullptr, 0, nullptr, nullptr, nullptr, file_system, MAX_PATH) &&
            _wcsicmp(file_system, L"NTFS") == 0;
    }

    uint64_t next_id = 1;
    uint64_t root_id = next_id++;
    Node root_node;
    root_node.id = root_id;
    root_node.parent_id = 0;
    root_node.name = normalized_root;
    root_node.is_dir = true;
    tree->AddNode(root_node);
    tree->SetRootId(root_id);

    struct StackEntry
    {
        std::wstring path;
        uint64_t parent_id;
    };

    // Use an explicit stack so deeply nested directory trees do not consume the
    // process call stack during traversal.
    std::vector<StackEntry> stack;
    stack.push_back({normalized_root, root_id});

    while (!stack.empty())
    {
        StackEntry entry = std::move(stack.back());
        stack.pop_back();

        std::wstring search_path = entry.path;
        if (!search_path.empty() && search_path.back() != L'\\')
        {
            search_path.append(L"\\");
        }
        search_path.append(L"*");

        WIN32_FIND_DATAW find_data = {};
        HANDLE find_handle = FindFirstFileExW(
            search_path.c_str(),
            FindExInfoBasic,
            &find_data,
            FindExSearchNameMatch,
            nullptr,
            FIND_FIRST_EX_LARGE_FETCH);

        if (find_handle == INVALID_HANDLE_VALUE)
        {
            continue;
        }

        do
        {
            const wchar_t* name = find_data.cFileName;
            if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0)
            {
                continue;
            }

            bool is_dir = (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            // WIN32_FIND_DATAW exposes the file size as two 32-bit halves.
            uint64_t size = (static_cast<uint64_t>(find_data.nFileSizeHigh) << 32) |
                            static_cast<uint64_t>(find_data.nFileSizeLow);

            Node node;
            node.id = next_id++;
            node.parent_id = entry.parent_id;
            node.name = name;
            node.is_dir = is_dir;
            node.size = is_dir ? 0 : size;
            if (!is_dir)
            {
                std::wstring file_path = entry.path;
                if (!file_path.empty() && file_path.back() != L'\\')
                {
                    file_path.append(L"\\");
                }
                file_path.append(name);
                ReadSizeOnDisk(file_path, find_data.dwFileAttributes, cluster_size, is_ntfs, &node);
            }
            tree->AddNode(node);

            if (is_dir)
            {
                std::wstring child_path = entry.path;
                if (!child_path.empty() && child_path.back() != L'\\')
                {
                    child_path.append(L"\\");
                }
                child_path.append(name);
                stack.push_back({child_path, node.id});
            }
        } while (FindNextFileW(find_handle, &find_data));

        FindClose(find_handle);
    }

    if (out_root_id)
    {
        *out_root_id = root_id;
    }
    return true;
}

} // namespace swift_disk
