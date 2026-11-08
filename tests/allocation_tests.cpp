// SPDX-FileCopyrightText: 2026 Per Edlund
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/file_tree.h"
#include "core/dir_scanner.h"
#include "core/ntfs/mft_scanner.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{

struct WindowsAllocation
{
    uint64_t m_file_id = 0;
    uint64_t m_logical = 0;
    uint64_t m_allocated = 0;
    DWORD m_link_count = 0;
};

/**
Reports the path and Windows error when a read-only metadata query fails.
**/
void RequireQuery(bool succeeded, const std::wstring& path, const char* operation)
{
    if (!succeeded)
    {
        std::wcerr << path << L": Windows error " << GetLastError() << L'\n';
        throw std::runtime_error(operation);
    }
}

/**
Reads file identity and primary-content allocation through Windows, independently
of the MFT parser. Resident data has no separately allocated data clusters.
**/
WindowsAllocation ReadAllocation(const std::wstring& path, uint64_t cluster_size)
{
    HANDLE raw = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    RequireQuery(raw != INVALID_HANDLE_VALUE, path, "Cannot open file metadata");
    const std::unique_ptr<void, decltype(&CloseHandle)> file(raw, &CloseHandle);
    BY_HANDLE_FILE_INFORMATION identity = {};
    FILE_STANDARD_INFO standard = {};
    RequireQuery(GetFileInformationByHandle(file.get(), &identity) != FALSE &&
        GetFileInformationByHandleEx(file.get(), FileStandardInfo, &standard, sizeof(standard)) != FALSE,
        path, "Cannot read file identity and allocation");

    WindowsAllocation result;
    result.m_file_id = (static_cast<uint64_t>(identity.nFileIndexHigh) << 32) | identity.nFileIndexLow;
    result.m_logical = static_cast<uint64_t>(standard.EndOfFile.QuadPart);
    result.m_link_count = identity.nNumberOfLinks;
    const bool compressed_or_sparse =
        (identity.dwFileAttributes & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE)) != 0;
    if (compressed_or_sparse)
    {
        FILE_COMPRESSION_INFO compression = {};
        RequireQuery(GetFileInformationByHandleEx(file.get(), FileCompressionInfo, &compression, sizeof(compression)) != FALSE,
            path, "Cannot read compressed allocation");
        result.m_allocated = static_cast<uint64_t>(compression.CompressedFileSize.QuadPart);
    }
    else
    {
        result.m_allocated = static_cast<uint64_t>(standard.AllocationSize.QuadPart);
        STARTING_VCN_INPUT_BUFFER input = {};
        RETRIEVAL_POINTERS_BUFFER output = {};
        DWORD returned = 0;
        if (result.m_allocated < cluster_size || (result.m_logical < 4096 &&
            !DeviceIoControl(file.get(), FSCTL_GET_RETRIEVAL_POINTERS, &input, sizeof(input),
                &output, sizeof(output), &returned, nullptr) && GetLastError() == ERROR_HANDLE_EOF))
        {
            result.m_allocated = 0;
        }
    }
    return result;
}

/**
Enumerates every hard-link path Windows lists for a file, including paths outside
the requested folder. The returned names are relative to the volume root.
**/
std::vector<std::wstring> ReadHardLinks(const std::wstring& path)
{
    std::vector<wchar_t> buffer(1024);
    DWORD length = static_cast<DWORD>(buffer.size());
    HANDLE raw = FindFirstFileNameW(path.c_str(), 0, &length, buffer.data());
    if (raw == INVALID_HANDLE_VALUE && GetLastError() == ERROR_MORE_DATA)
    {
        buffer.resize(length);
        raw = FindFirstFileNameW(path.c_str(), 0, &length, buffer.data());
    }
    RequireQuery(raw != INVALID_HANDLE_VALUE, path, "Cannot enumerate hard links");
    const std::unique_ptr<void, decltype(&FindClose)> search(raw, &FindClose);
    std::vector<std::wstring> paths;
    paths.emplace_back(buffer.data());
    for (;;)
    {
        length = static_cast<DWORD>(buffer.size());
        if (FindNextFileNameW(search.get(), &length, buffer.data()))
        {
            paths.emplace_back(buffer.data());
        }
        else if (GetLastError() == ERROR_MORE_DATA)
        {
            buffer.resize(length);
        }
        else
        {
            RequireQuery(GetLastError() == ERROR_HANDLE_EOF, path, "Cannot finish hard-link enumeration");
            return paths;
        }
    }
}

/**
Finds a scanned path by walking parent-child indexes. Only directories touched
by this targeted check are indexed, avoiding a second full-volume path table.
**/
class PathLookup
{
  public:
    explicit PathLookup(const swift_disk::FileTree& tree);
    const swift_disk::Node* Find(const std::wstring& path);

  private:
    const swift_disk::FileTree& m_tree;
    std::unordered_map<std::wstring, uint64_t> m_paths;
    std::unordered_set<uint64_t> m_indexed_directories;
};

/**
Initializes path lookup at the scanned volume root.
**/
PathLookup::PathLookup(const swift_disk::FileTree& tree) : m_tree(tree)
{
    m_paths.emplace(tree.BuildPath(tree.RootId()), tree.RootId());
}

/**
Returns the matching node, or null when a Windows-listed path is missing.
Paths supplied by Windows retain their actual stored filename spelling.
**/
const swift_disk::Node* PathLookup::Find(const std::wstring& path)
{
    auto found = m_paths.find(path);
    if (found != m_paths.end())
    {
        return m_tree.GetNode(found->second);
    }
    const size_t separator = path.find_last_of(L'\\');
    if (separator == std::wstring::npos)
    {
        return nullptr;
    }
    const std::wstring parent_path = path.substr(0, separator);
    const swift_disk::Node* parent = Find(parent_path);
    if (!parent || !parent->is_dir || !m_indexed_directories.insert(parent->id).second)
    {
        return nullptr;
    }
    const auto* children = m_tree.GetChildIds(parent->id);
    if (children)
    {
        for (uint64_t child_id : *children)
        {
            const swift_disk::Node* child = m_tree.GetNode(child_id);
            if (child)
            {
                if (!m_paths.emplace(parent_path + L"\\" + child->name, child_id).second)
                {
                    throw std::runtime_error("MFT scan returned a duplicate path");
                }
            }
        }
    }
    found = m_paths.find(path);
    return found == m_paths.end() ? nullptr : m_tree.GetNode(found->second);
}

} // namespace

/**
Checks allocation for every file in a requested folder against a whole-volume
production scan. Windows file identities and hard-link enumeration independently
verify that allocation appears on exactly one of each file's scanned paths.
Only metadata is read; this command creates, changes and deletes no files.
**/
int VerifyAllocations(const wchar_t* path)
{
    using namespace swift_disk;
    std::wstring root_path(path);
    std::replace(root_path.begin(), root_path.end(), L'/', L'\\');
    while (root_path.size() > 2 && root_path.back() == L'\\')
    {
        root_path.pop_back();
    }
    if (root_path.size() < 2 || root_path[1] != L':')
    {
        throw std::runtime_error("Allocation check requires a drive-qualified NTFS folder");
    }
    root_path[0] = static_cast<wchar_t>(std::towupper(root_path[0]));
    const std::wstring drive = root_path.substr(0, 2);
    const std::wstring volume_root = drive + L"\\";
    wchar_t file_system[MAX_PATH] = {};
    DWORD sectors = 0;
    DWORD bytes_per_sector = 0;
    DWORD free_clusters = 0;
    DWORD total_clusters = 0;
    RequireQuery(GetDiskFreeSpaceW(volume_root.c_str(), &sectors, &bytes_per_sector, &free_clusters, &total_clusters) &&
        GetVolumeInformationW(volume_root.c_str(), nullptr, 0, nullptr, nullptr, nullptr, file_system, MAX_PATH),
        root_path, "Cannot query volume geometry");
    const DWORD attributes = GetFileAttributesW((root_path + L"\\").c_str());
    if (_wcsicmp(file_system, L"NTFS") != 0 || attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        throw std::runtime_error("Allocation check requires an existing NTFS folder");
    }
    FileTree tree;
    std::wstring error;
    std::cout << "Scanning volume read-only for allocation attribution...\n";
    if (!ntfs::ScanVolumeMft(root_path[0], &tree, &error, nullptr))
    {
        std::wcerr << L"MFT scan failed: " << error << L'\n';
        return 1;
    }
    PathLookup lookup(tree);
    const Node* folder = lookup.Find(root_path);
    if (!folder || !folder->is_dir)
    {
        throw std::runtime_error("Requested folder is missing from the MFT tree");
    }
    FileTree expected;
    if (!ScanDirectory(root_path + L"\\", &expected, nullptr))
    {
        throw std::runtime_error("Cannot enumerate reference folder");
    }
    expected.Finalize();
    uint64_t folder_files = 0;
    uint64_t windows_folder_allocation = 0;
    uint64_t unique_allocation = 0;
    uint64_t attributed_inside = 0;
    uint64_t attributed_outside = 0;
    uint64_t checked_links = 0;
    uint64_t outside_owners = 0;
    std::unordered_set<uint64_t> checked_files;
    const uint64_t cluster_size = static_cast<uint64_t>(sectors) * bytes_per_sector;
    for (const Node* reference : expected.GetAllNodes())
    {
        if (reference->is_dir)
        {
            continue;
        }
        const std::wstring file_path = expected.BuildPath(reference->id);
        const WindowsAllocation allocation = ReadAllocation(file_path, cluster_size);
        ++folder_files;
        windows_folder_allocation += allocation.m_allocated;
        const Node* scanned = lookup.Find(file_path);
        if (!scanned || scanned->is_dir || scanned->size != allocation.m_logical)
        {
            std::wcerr << L"Missing path or incorrect logical size: " << file_path << L'\n';
            return 1;
        }
        if (!checked_files.insert(allocation.m_file_id).second)
        {
            continue;
        }
        const auto links = ReadHardLinks(file_path);
        if (links.size() != allocation.m_link_count)
        {
            throw std::runtime_error("Windows link count changed during verification");
        }
        uint64_t scanned_allocation = 0;
        size_t owners = 0;
        for (const std::wstring& relative : links)
        {
            const std::wstring link_path = drive + relative;
            const Node* link = lookup.Find(link_path);
            if (!link || link->is_dir || link->size != allocation.m_logical || link->m_size_on_disk_estimated)
            {
                std::wcerr << L"Missing or incorrect hard-link path: " << link_path << L'\n';
                return 1;
            }
            ++checked_links;
            scanned_allocation += link->m_size_on_disk;
            if (link->m_size_on_disk == 0)
            {
                continue;
            }
            ++owners;
            const WindowsAllocation owner = ReadAllocation(link_path, cluster_size);
            if (owner.m_file_id != allocation.m_file_id || link->m_size_on_disk != allocation.m_allocated)
            {
                std::wcerr << L"Allocation mismatch: " << link_path << L", scanner=" << link->m_size_on_disk
                    << L", Windows=" << allocation.m_allocated << L'\n';
                return 1;
            }
            const bool inside = link_path.size() > root_path.size() &&
                _wcsnicmp(link_path.c_str(), root_path.c_str(), root_path.size()) == 0 &&
                link_path[root_path.size()] == L'\\';
            if (inside)
            {
                attributed_inside += link->m_size_on_disk;
            }
            else
            {
                attributed_outside += link->m_size_on_disk;
                ++outside_owners;
            }
        }
        if (scanned_allocation != allocation.m_allocated || owners != (allocation.m_allocated > 0 ? 1U : 0U))
        {
            std::wcerr << L"Missing or duplicate allocation: " << file_path << L", scanner sum="
                << scanned_allocation << L", Windows=" << allocation.m_allocated << L", owners=" << owners << L'\n';
            return 1;
        }
        unique_allocation += allocation.m_allocated;
    }
    if (folder_files == 0 || folder->size != expected.GetNode(expected.RootId())->size ||
        folder->m_size_on_disk != attributed_inside || attributed_inside + attributed_outside != unique_allocation)
    {
        throw std::runtime_error("Folder totals do not match the checked files and allocation owners");
    }
    std::cout << "Verified " << folder_files << " folder paths, " << checked_files.size()
        << " underlying files, " << checked_links << " hard-link paths across the volume.\n"
        << "Logical bytes: " << folder->size << '\n'
        << "Windows allocation per folder entry: " << windows_folder_allocation << '\n'
        << "Windows allocation once per underlying file: " << unique_allocation << '\n'
        << "SwiftDisk allocation attributed inside folder: " << attributed_inside << '\n'
        << "SwiftDisk allocation attributed outside folder: " << attributed_outside << '\n'
        << "Files with allocation attributed outside folder: " << outside_owners << '\n'
        << "All checked allocation matches Windows and is counted exactly once.\n";
    return 0;
}
