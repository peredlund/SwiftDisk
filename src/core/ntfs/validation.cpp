#include "core/ntfs/validation.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <sstream>
#include <utility>

namespace swift_disk::ntfs
{

namespace
{

constexpr size_t kMaxMismatchExamples = 64;
constexpr size_t kMaxEdgeExamplesPerKind = 8;
constexpr uint64_t kNtfsRecordNumberMask = 0x0000FFFFFFFFFFFFULL;

struct HandleCloser
{
    void operator()(HANDLE handle) const
    {
        if (handle && handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle);
        }
    }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct Win32EntryInfo
{
    bool is_dir = false;
    bool is_sparse = false;
    bool is_compressed = false;
    bool is_reparse = false;
    bool has_ads = false;
    bool has_comparable_ids = false;
    uint64_t frn = 0;
    uint64_t parent_frn = 0;
    uint64_t size = 0;
    uint32_t hard_link_count = 0;
    std::wstring name;
};

uint64_t FileId128Low64(const FILE_ID_128& file_id)
{
    uint64_t value = 0;
    std::memcpy(&value, &file_id, sizeof(value));
    return value;
}

uint64_t NormalizeNtfsFileReference(uint64_t file_reference)
{
    // MFT parser stores the 48-bit record number, not the 16-bit sequence.
    return file_reference & kNtfsRecordNumberMask;
}

bool IsComparableNtfsFileId(const FILE_ID_128& file_id)
{
    for (size_t i = sizeof(uint64_t); i < sizeof(file_id.Identifier); ++i)
    {
        if (file_id.Identifier[i] != 0)
        {
            return false;
        }
    }
    return true;
}

std::wstring UInt64ToWide(uint64_t value)
{
    return std::to_wstring(value);
}

std::wstring BoolToWide(bool value)
{
    return value ? L"true" : L"false";
}

std::wstring BaseNameFromPath(const std::wstring& path)
{
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos)
    {
        return path;
    }
    return path.substr(pos + 1);
}

HANDLE OpenMetadataHandle(const std::wstring& path)
{
    return CreateFileW(
        path.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
}

bool QueryComparableNtfsIds(HANDLE handle, uint64_t* out_frn, uint64_t* out_parent_frn)
{
    if (!handle || handle == INVALID_HANDLE_VALUE || !out_frn || !out_parent_frn)
    {
        return false;
    }

    READ_FILE_USN_DATA input = {};
    input.MinMajorVersion = 2;
    input.MaxMajorVersion = 3;

    std::array<uint8_t, 4096> buffer = {};
    DWORD bytes_returned = 0;
    if (!DeviceIoControl(
            handle,
            FSCTL_READ_FILE_USN_DATA,
            &input,
            sizeof(input),
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytes_returned,
            nullptr))
    {
        return false;
    }

    if (bytes_returned < sizeof(USN_RECORD_COMMON_HEADER))
    {
        return false;
    }

    auto* header = reinterpret_cast<const USN_RECORD_COMMON_HEADER*>(buffer.data());
    if (header->MajorVersion == 2 && bytes_returned >= sizeof(USN_RECORD_V2))
    {
        auto* record = reinterpret_cast<const USN_RECORD_V2*>(buffer.data());
        *out_frn = NormalizeNtfsFileReference(record->FileReferenceNumber);
        *out_parent_frn = NormalizeNtfsFileReference(record->ParentFileReferenceNumber);
        return true;
    }

    if (header->MajorVersion == 3 && bytes_returned >= sizeof(USN_RECORD_V3))
    {
        auto* record = reinterpret_cast<const USN_RECORD_V3*>(buffer.data());
        if (!IsComparableNtfsFileId(record->FileReferenceNumber) ||
            !IsComparableNtfsFileId(record->ParentFileReferenceNumber))
        {
            return false;
        }
        *out_frn = NormalizeNtfsFileReference(FileId128Low64(record->FileReferenceNumber));
        *out_parent_frn = NormalizeNtfsFileReference(FileId128Low64(record->ParentFileReferenceNumber));
        return true;
    }

    return false;
}

bool DetectNamedStreams(const std::wstring& path, bool is_dir)
{
    if (is_dir)
    {
        return false;
    }

    WIN32_FIND_STREAM_DATA stream_data = {};
    HANDLE stream_handle = FindFirstStreamW(path.c_str(), FindStreamInfoStandard, &stream_data, 0);
    if (stream_handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    bool has_ads = false;
    do
    {
        if (wcscmp(stream_data.cStreamName, L"::$DATA") != 0)
        {
            has_ads = true;
            break;
        }
    } while (FindNextStreamW(stream_handle, &stream_data));

    FindClose(stream_handle);
    return has_ads;
}

bool QueryPathInfo(const std::wstring& path, Win32EntryInfo* out_info)
{
    if (!out_info)
    {
        return false;
    }

    UniqueHandle handle(OpenMetadataHandle(path));
    if (!handle || handle.get() == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    BY_HANDLE_FILE_INFORMATION info = {};
    if (!GetFileInformationByHandle(static_cast<HANDLE>(handle.get()), &info))
    {
        return false;
    }

    out_info->is_dir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    out_info->is_sparse = (info.dwFileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0;
    out_info->is_compressed = (info.dwFileAttributes & FILE_ATTRIBUTE_COMPRESSED) != 0;
    out_info->is_reparse = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    out_info->hard_link_count = info.nNumberOfLinks;
    out_info->size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | static_cast<uint64_t>(info.nFileSizeLow);
    out_info->name = BaseNameFromPath(path);
    out_info->has_ads = DetectNamedStreams(path, out_info->is_dir);
    out_info->has_comparable_ids = QueryComparableNtfsIds(
        static_cast<HANDLE>(handle.get()),
        &out_info->frn,
        &out_info->parent_frn);

    return true;
}

void AddMismatch(
    ValidationReport* report,
    const Node& node,
    const std::wstring& path,
    const std::wstring& field,
    const std::wstring& expected,
    const std::wstring& actual)
{
    if (!report)
    {
        return;
    }
    if (report->mismatches.size() >= kMaxMismatchExamples)
    {
        return;
    }

    ValidationMismatch mismatch;
    mismatch.frn = node.id;
    mismatch.path = path;
    mismatch.field = field;
    mismatch.expected = expected;
    mismatch.actual = actual;
    report->mismatches.push_back(std::move(mismatch));
}

void AddEdgeExample(std::vector<std::wstring>* examples, const std::wstring& path, const std::wstring& detail)
{
    if (!examples || examples->size() >= kMaxEdgeExamplesPerKind)
    {
        return;
    }
    if (detail.empty())
    {
        examples->push_back(path);
    }
    else
    {
        examples->push_back(path + L" [" + detail + L"]");
    }
}

std::vector<const Node*> CollectSortedNodes(const FileTree& tree, bool files_only)
{
    std::vector<const Node*> nodes = tree.GetAllNodes();
    nodes.erase(
        std::remove_if(
            nodes.begin(),
            nodes.end(),
            [&](const Node* node)
            {
                if (!node || node->id == tree.RootId())
                {
                    return true;
                }
                return files_only && node->is_dir;
            }),
        nodes.end());

    std::sort(nodes.begin(), nodes.end(),
        [](const Node* left, const Node* right) { return left->id < right->id; });

    return nodes;
}

std::vector<const Node*> SampleEvenly(const std::vector<const Node*>& nodes, size_t sample_size)
{
    if (nodes.empty() || sample_size == 0)
    {
        return {};
    }
    if (nodes.size() <= sample_size)
    {
        return nodes;
    }
    if (sample_size == 1)
    {
        return {nodes[nodes.size() / 2]};
    }

    std::vector<const Node*> sample;
    sample.reserve(sample_size);
    for (size_t i = 0; i < sample_size; ++i)
    {
        size_t index = (i * (nodes.size() - 1)) / (sample_size - 1);
        sample.push_back(nodes[index]);
    }
    return sample;
}

} // namespace

bool ValidateVolumeWithWin32(
    const FileTree& tree,
    const MftScanStats& scan_stats,
    size_t sample_size,
    size_t edge_scan_limit,
    ValidationReport* out_report,
    std::wstring* error)
{
    if (!out_report)
    {
        if (error)
        {
            *error = L"ValidationReport is null.";
        }
        return false;
    }

    ValidationReport report;
    report.sample_size_requested = sample_size;
    report.attribute_list_extension_records = scan_stats.extension_size_records;
    report.attribute_list_extension_bytes = scan_stats.extension_size_total;

    std::vector<const Node*> files = CollectSortedNodes(tree, true);
    report.sample_file_candidates = files.size();

    std::vector<const Node*> sample = SampleEvenly(files, sample_size);
    for (const Node* node : sample)
    {
        if (!node)
        {
            continue;
        }

        std::wstring path = tree.BuildPath(node->id);
        Win32EntryInfo win32_info;
        if (!QueryPathInfo(path, &win32_info))
        {
            report.missing_paths++;
            AddMismatch(&report, *node, path, L"path", L"accessible", L"missing_or_unopenable");
            continue;
        }

        report.sampled_files_checked++;
        if (win32_info.has_comparable_ids)
        {
            report.sampled_files_with_comparable_ids++;
        }

        if (win32_info.size != node->size)
        {
            report.size_mismatches++;
            AddMismatch(&report, *node, path, L"size", UInt64ToWide(node->size), UInt64ToWide(win32_info.size));
        }
        if (win32_info.has_comparable_ids && win32_info.parent_frn != node->parent_id)
        {
            report.parent_mismatches++;
            AddMismatch(&report, *node, path, L"parent_frn", UInt64ToWide(node->parent_id), UInt64ToWide(win32_info.parent_frn));
        }
        if (win32_info.name != node->name)
        {
            report.name_mismatches++;
            AddMismatch(&report, *node, path, L"name", node->name, win32_info.name);
        }
        if (win32_info.is_dir != node->is_dir)
        {
            report.type_mismatches++;
            AddMismatch(&report, *node, path, L"is_dir", BoolToWide(node->is_dir), BoolToWide(win32_info.is_dir));
        }
        if (win32_info.has_comparable_ids && win32_info.frn != node->id)
        {
            report.frn_mismatches++;
            AddMismatch(&report, *node, path, L"frn", UInt64ToWide(node->id), UInt64ToWide(win32_info.frn));
        }
    }

    std::vector<const Node*> entries = CollectSortedNodes(tree, false);
    size_t inspect_limit = std::min(edge_scan_limit, entries.size());
    for (size_t i = 0; i < inspect_limit; ++i)
    {
        const Node* node = entries[i];
        if (!node)
        {
            continue;
        }

        std::wstring path = tree.BuildPath(node->id);
        Win32EntryInfo win32_info;
        if (!QueryPathInfo(path, &win32_info))
        {
            continue;
        }

        report.edge_cases.entries_inspected++;

        if (win32_info.hard_link_count > 1)
        {
            report.edge_cases.hard_link_entries++;
            AddEdgeExample(&report.edge_cases.hard_link_examples, path, L"links=" + UInt64ToWide(win32_info.hard_link_count));
        }
        if (win32_info.is_sparse)
        {
            report.edge_cases.sparse_entries++;
            AddEdgeExample(&report.edge_cases.sparse_examples, path, L"sparse");
        }
        if (win32_info.is_compressed)
        {
            report.edge_cases.compressed_entries++;
            AddEdgeExample(&report.edge_cases.compressed_examples, path, L"compressed");
        }
        if (win32_info.is_reparse)
        {
            report.edge_cases.reparse_entries++;
            AddEdgeExample(&report.edge_cases.reparse_examples, path, L"reparse");
        }
        if (win32_info.has_ads)
        {
            report.edge_cases.ads_entries++;
            AddEdgeExample(&report.edge_cases.ads_examples, path, L"ads");
        }
    }

    *out_report = std::move(report);
    return true;
}

} // namespace swift_disk::ntfs
