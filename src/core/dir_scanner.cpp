#include "core/dir_scanner.h"

#include <windows.h>

#include <cwchar>
#include <vector>

namespace space_scanner {

/**
 Normalizes the root path used for display and traversal.
 
 Parameters:
   path: Input path.
 
 Returns: Path without a trailing separator (except drive root forms).
 **/
static std::wstring NormalizeRootName(std::wstring path) {
    if (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    return path;
}

/**
 Scans a directory tree using Win32 enumeration APIs.
 Builds a FileTree where file sizes come from directory entry metadata.
 
 Parameters:
   root_path: Root path to scan.
   tree: Destination file tree.
   out_root_id: Optional output for assigned root id.
 
 Returns: true on success, otherwise false.
 **/
bool ScanDirectory(const std::wstring& root_path, FileTree* tree, uint64_t* out_root_id) {
    if (!tree || root_path.empty()) {
        return false;
    }

    std::wstring normalized_root = NormalizeRootName(root_path);

    uint64_t next_id = 1;
    uint64_t root_id = next_id++;
    Node root_node;
    root_node.id = root_id;
    root_node.parent_id = 0;
    root_node.name = normalized_root;
    root_node.is_dir = true;
    tree->AddNode(root_node);
    tree->SetRootId(root_id);

    struct StackEntry {
        std::wstring path;
        uint64_t parent_id;
    };

    std::vector<StackEntry> stack;
    stack.push_back({normalized_root, root_id});

    while (!stack.empty()) {
        StackEntry entry = std::move(stack.back());
        stack.pop_back();

        std::wstring search_path = entry.path;
        if (!search_path.empty() && search_path.back() != L'\\') {
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

        if (find_handle == INVALID_HANDLE_VALUE) {
            continue;
        }

        do {
            const wchar_t* name = find_data.cFileName;
            if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) {
                continue;
            }

            bool is_dir = (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            uint64_t size = (static_cast<uint64_t>(find_data.nFileSizeHigh) << 32) |
                static_cast<uint64_t>(find_data.nFileSizeLow);

            Node node;
            node.id = next_id++;
            node.parent_id = entry.parent_id;
            node.name = name;
            node.is_dir = is_dir;
            node.size = is_dir ? 0 : size;
            tree->AddNode(node);

            if (is_dir) {
                std::wstring child_path = entry.path;
                if (!child_path.empty() && child_path.back() != L'\\') {
                    child_path.append(L"\\");
                }
                child_path.append(name);
                stack.push_back({child_path, node.id});
            }
        } while (FindNextFileW(find_handle, &find_data));

        FindClose(find_handle);
    }

    if (out_root_id) {
        *out_root_id = root_id;
    }
    return true;
}

} // namespace space_scanner
