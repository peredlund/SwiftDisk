#include <windows.h>

#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "common/file_tree.h"
#include "common/stopwatch.h"
#include "core/dir_scanner.h"
#include "core/ntfs/mft_scanner.h"
#include "core/ntfs/validation.h"
namespace space_scanner
{

struct Options
{
    std::wstring path;
    size_t top = 20;
    bool validate = false;
    size_t validate_sample = 128;
    size_t validate_edge_limit = 4096;
    std::wstring json_out;
};

bool TryGetVolumeRoot(const std::wstring& path, std::wstring* volume_root)
{
    if (!volume_root || path.empty())
    {
        return false;
    }

    wchar_t buffer[MAX_PATH] = {};
    DWORD length = GetVolumePathNameW(path.c_str(), buffer,
                                      static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer))
    {
        return false;
    }

    *volume_root = buffer;
    return true;
}

bool IsNtfsPath(const std::wstring& path)
{
    std::wstring volume_root;
    if (!TryGetVolumeRoot(path, &volume_root))
    {
        return false;
    }

    wchar_t file_system_name[MAX_PATH] = {};
    if (!GetVolumeInformationW(volume_root.c_str(), nullptr, 0, nullptr,
                               nullptr, nullptr, file_system_name,
                               static_cast<DWORD>(std::size(file_system_name))))
    {
        return false;
    }

    return _wcsicmp(file_system_name, L"NTFS") == 0;
}

void PrintUsage()
{
    std::wcout << L"SpaceScanner CLI\n";
    std::wcout << L"Usage:\n";
    std::wcout << L"  space_scanner_cli.exe scan --path C:\\\n";
    std::wcout << L"  space_scanner_cli.exe scan --path C:\\\\Users\n";
    std::wcout << L"Options:\n";
    std::wcout
        << L"  --top N          Number of entries to list (default 20)\n";
    std::wcout << L"  --validate       Compare sampled MFT results against "
                  L"Win32 metadata\n";
    std::wcout << L"  --validate-sample N\n";
    std::wcout << L"                   Number of files to sample during "
                  L"validation (default 128)\n";
    std::wcout << L"  --validate-edge-limit N\n";
    std::wcout << L"                   Number of entries to inspect for edge "
                  L"cases (default 4096)\n";
    std::wcout
        << L"  --json-out PATH  Write machine-readable scan report to JSON\n";
}

/**
 Parses CLI arguments for scan mode and options.

 Parameters:
   argc: Argument count.
   argv: Argument array.
   options: Parsed output options.

 Returns: true when required scan options are present.
 **/
bool ParseArgs(int argc, wchar_t* argv[], Options* options)
{
    if (argc < 2 || wcscmp(argv[1], L"scan") != 0)
    {
        return false;
    }

    for (int i = 2; i < argc; ++i)
    {
        if (wcscmp(argv[i], L"--path") == 0 && i + 1 < argc)
        {
            options->path = argv[++i];
        }
        else if (wcscmp(argv[i], L"--top") == 0 && i + 1 < argc)
        {
            options->top = static_cast<size_t>(_wtoi(argv[++i]));
        }
        else if (wcscmp(argv[i], L"--validate") == 0)
        {
            options->validate = true;
        }
        else if (wcscmp(argv[i], L"--validate-sample") == 0 && i + 1 < argc)
        {
            options->validate = true;
            options->validate_sample = static_cast<size_t>(_wtoi(argv[++i]));
        }
        else if (wcscmp(argv[i], L"--validate-edge-limit") == 0 && i + 1 < argc)
        {
            options->validate = true;
            options->validate_edge_limit =
                static_cast<size_t>(_wtoi(argv[++i]));
        }
        else if (wcscmp(argv[i], L"--json-out") == 0 && i + 1 < argc)
        {
            options->json_out = argv[++i];
        }
        else if (wcscmp(argv[i], L"--help") == 0 || wcscmp(argv[i], L"-h") == 0)
        {
            return false;
        }
    }

    return !options->path.empty();
}

/**
 Converts a byte count to a human-readable IEC string.

 Parameters:
   bytes: Raw byte count.

 Returns: Formatted size string with binary units.
 **/
std::wstring HumanizeBytes(uint64_t bytes)
{
    const wchar_t* units[] = {L"B", L"KiB", L"MiB", L"GiB", L"TiB"};
    double value = static_cast<double>(bytes);
    size_t unit_index = 0;
    while (value >= 1024.0 && unit_index < std::size(units) - 1)
    {
        value /= 1024.0;
        ++unit_index;
    }
    wchar_t buffer[64];
    swprintf_s(buffer, L"%.2f %s", value, units[unit_index]);
    return buffer;
}

std::string Utf8FromWide(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }

    int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                                   static_cast<int>(value.size()), nullptr, 0,
                                   nullptr, nullptr);
    if (size <= 0)
    {
        return {};
    }

    std::string utf8(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                        static_cast<int>(value.size()), utf8.data(), size,
                        nullptr, nullptr);
    return utf8;
}

std::string EscapeJson(const std::string& value)
{
    std::ostringstream escaped;
    for (unsigned char ch : value)
    {
        switch (ch)
        {
        case '\"':
            escaped << "\\\"";
            break;
        case '\\':
            escaped << "\\\\";
            break;
        case '\b':
            escaped << "\\b";
            break;
        case '\f':
            escaped << "\\f";
            break;
        case '\n':
            escaped << "\\n";
            break;
        case '\r':
            escaped << "\\r";
            break;
        case '\t':
            escaped << "\\t";
            break;
        default:
            if (ch < 0x20)
            {
                escaped << "\\u";
                constexpr char kHex[] = "0123456789abcdef";
                escaped << "00" << kHex[(ch >> 4) & 0x0F] << kHex[ch & 0x0F];
            }
            else
            {
                escaped << static_cast<char>(ch);
            }
            break;
        }
    }
    return escaped.str();
}

void AppendJsonString(std::ostringstream& json, const std::wstring& value)
{
    json << '"' << EscapeJson(Utf8FromWide(value)) << '"';
}

void AppendJsonString(std::ostringstream& json, const std::string& value)
{
    json << '"' << EscapeJson(value) << '"';
}

void AppendJsonTopEntries(std::ostringstream& json, const FileTree& tree,
                          size_t top_count)
{
    auto top = tree.TopN(top_count);
    json << "[\n";
    for (size_t i = 0; i < top.size(); ++i)
    {
        const Node* node = top[i];
        json << "    {\n";
        json << "      \"id\": " << node->id << ",\n";
        json << "      \"path\": ";
        AppendJsonString(json, tree.BuildPath(node->id));
        json << ",\n";
        json << "      \"size\": " << node->size << ",\n";
        json << "      \"is_dir\": " << (node->is_dir ? "true" : "false")
             << "\n";
        json << "    }";
        if (i + 1 != top.size())
        {
            json << ",";
        }
        json << "\n";
    }
    json << "  ]";
}

void AppendJsonMftStats(std::ostringstream& json,
                        const ntfs::MftScanStats& stats)
{
    json << "  \"mft_stats\": {\n";
    json << "    \"records_read\": " << stats.records_read << ",\n";
    json << "    \"records_parsed\": " << stats.records_parsed << ",\n";
    json << "    \"records_in_use\": " << stats.records_in_use << ",\n";
    json << "    \"records_added\": " << stats.records_added << ",\n";
    json << "    \"records_parse_failed\": " << stats.records_parse_failed
         << ",\n";
    json << "    \"records_no_name\": " << stats.records_no_name << ",\n";
    json << "    \"records_no_name_with_size\": "
         << stats.records_no_name_with_size << ",\n";
    json << "    \"records_no_name_size\": " << stats.records_no_name_size
         << ",\n";
    json << "    \"extension_size_records\": " << stats.extension_size_records
         << ",\n";
    json << "    \"extension_size_total\": " << stats.extension_size_total
         << ",\n";
    json << "    \"extension_missing_base_records\": "
         << stats.extension_missing_base_records << ",\n";
    json << "    \"extension_missing_base_total\": "
         << stats.extension_missing_base_total << ",\n";
    json << "    \"mft_valid_data_length\": " << stats.mft_valid_data_length
         << ",\n";
    json << "    \"bytes_per_file_record\": " << stats.bytes_per_file_record
         << "\n";
    json << "  }";
}

void AppendJsonValidation(std::ostringstream& json,
                          const ntfs::ValidationReport& report)
{
    json << "  \"validation\": {\n";
    json << "    \"sample_size_requested\": " << report.sample_size_requested
         << ",\n";
    json << "    \"sample_file_candidates\": " << report.sample_file_candidates
         << ",\n";
    json << "    \"sampled_files_checked\": " << report.sampled_files_checked
         << ",\n";
    json << "    \"sampled_files_with_comparable_ids\": "
         << report.sampled_files_with_comparable_ids << ",\n";
    json << "    \"missing_paths\": " << report.missing_paths << ",\n";
    json << "    \"size_mismatches\": " << report.size_mismatches << ",\n";
    json << "    \"parent_mismatches\": " << report.parent_mismatches << ",\n";
    json << "    \"name_mismatches\": " << report.name_mismatches << ",\n";
    json << "    \"type_mismatches\": " << report.type_mismatches << ",\n";
    json << "    \"frn_mismatches\": " << report.frn_mismatches << ",\n";
    json << "    \"attribute_list_extension_records\": "
         << report.attribute_list_extension_records << ",\n";
    json << "    \"attribute_list_extension_bytes\": "
         << report.attribute_list_extension_bytes << ",\n";
    json << "    \"edge_cases\": {\n";
    json << "      \"entries_inspected\": "
         << report.edge_cases.entries_inspected << ",\n";
    json << "      \"hard_link_entries\": "
         << report.edge_cases.hard_link_entries << ",\n";
    json << "      \"sparse_entries\": " << report.edge_cases.sparse_entries
         << ",\n";
    json << "      \"compressed_entries\": "
         << report.edge_cases.compressed_entries << ",\n";
    json << "      \"reparse_entries\": " << report.edge_cases.reparse_entries
         << ",\n";
    json << "      \"ads_entries\": " << report.edge_cases.ads_entries << ",\n";

    auto append_examples = [&](const char* name,
                               const std::vector<std::wstring>& examples,
                               bool trailing_comma)
    {
        json << "      \"" << name << "\": [";
        for (size_t i = 0; i < examples.size(); ++i)
        {
            if (i != 0)
            {
                json << ", ";
            }
            AppendJsonString(json, examples[i]);
        }
        json << "]";
        if (trailing_comma)
        {
            json << ",";
        }
        json << "\n";
    };

    append_examples("hard_link_examples", report.edge_cases.hard_link_examples,
                    true);
    append_examples("sparse_examples", report.edge_cases.sparse_examples, true);
    append_examples("compressed_examples",
                    report.edge_cases.compressed_examples, true);
    append_examples("reparse_examples", report.edge_cases.reparse_examples,
                    true);
    append_examples("ads_examples", report.edge_cases.ads_examples, false);
    json << "    },\n";
    json << "    \"mismatches\": [\n";
    for (size_t i = 0; i < report.mismatches.size(); ++i)
    {
        const ntfs::ValidationMismatch& mismatch = report.mismatches[i];
        json << "      {\n";
        json << "        \"frn\": " << mismatch.frn << ",\n";
        json << "        \"path\": ";
        AppendJsonString(json, mismatch.path);
        json << ",\n";
        json << "        \"field\": ";
        AppendJsonString(json, mismatch.field);
        json << ",\n";
        json << "        \"expected\": ";
        AppendJsonString(json, mismatch.expected);
        json << ",\n";
        json << "        \"actual\": ";
        AppendJsonString(json, mismatch.actual);
        json << "\n";
        json << "      }";
        if (i + 1 != report.mismatches.size())
        {
            json << ",";
        }
        json << "\n";
    }
    json << "    ]\n";
    json << "  }";
}

bool WriteJsonReport(const std::wstring& path, const Options& options,
                     bool used_mft_scan, double elapsed_seconds,
                     const FileTree& tree,
                     const FileTree::TreeStats& tree_stats,
                     const ntfs::MftScanStats& scan_stats,
                     const ntfs::ValidationReport* validation,
                     std::wstring* error)
{
    std::ostringstream json;
    json.setf(std::ios::fixed);
    json.precision(6);

    json << "{\n";
    json << "  \"scan_mode\": ";
    AppendJsonString(json, "path");
    json << ",\n";
    json << "  \"scan_backend\": ";
    AppendJsonString(json, used_mft_scan ? "ntfs_mft" : "directory");
    json << ",\n";
    json << "  \"path\": ";
    AppendJsonString(json, options.path);
    json << ",\n";
    json << "  \"elapsed_seconds\": " << elapsed_seconds << ",\n";
    json << "  \"node_count\": " << tree.NodeCount() << ",\n";
    json << "  \"tree_stats\": {\n";
    json << "    \"file_count\": " << tree_stats.file_count << ",\n";
    json << "    \"dir_count\": " << tree_stats.dir_count << ",\n";
    json << "    \"total_file_size\": " << tree_stats.total_file_size << ",\n";
    json << "    \"reachable_file_count\": " << tree_stats.reachable_file_count
         << ",\n";
    json << "    \"reachable_file_size\": " << tree_stats.reachable_file_size
         << ",\n";
    json << "    \"zero_size_files\": " << tree_stats.zero_size_files << ",\n";
    json << "    \"unreachable_file_count\": "
         << tree_stats.unreachable_file_count << ",\n";
    json << "    \"unreachable_file_size\": "
         << tree_stats.unreachable_file_size << "\n";
    json << "  },\n";
    if (used_mft_scan)
    {
        AppendJsonMftStats(json, scan_stats);
        json << ",\n";
    }
    if (validation)
    {
        AppendJsonValidation(json, *validation);
        json << ",\n";
    }
    json << "  \"top_entries\": ";
    AppendJsonTopEntries(json, tree, options.top);
    json << "\n";
    json << "}\n";

    std::ofstream output(std::filesystem::path(path), std::ios::binary);
    if (!output)
    {
        if (error)
        {
            *error = L"Failed to open JSON output file.";
        }
        return false;
    }

    std::string text = json.str();
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output.good())
    {
        if (error)
        {
            *error = L"Failed while writing JSON report.";
        }
        return false;
    }

    return true;
}

} // namespace space_scanner

/**
 CLI entry point that runs NTFS or directory scan and prints summary.

 Parameters:
   argc: Argument count.
   argv: Argument array.

 Returns: Process exit code.
 **/
int wmain(int argc, wchar_t* argv[])
{
    using namespace space_scanner;

    Options options;
    if (!ParseArgs(argc, argv, &options))
    {
        PrintUsage();
        return 1;
    }

    FileTree tree;
    FileTree::TreeStats tree_stats;
    Stopwatch timer;
    bool ok = false;
    std::wstring error;
    ntfs::MftScanStats stats;
    ntfs::ValidationReport validation_report;
    bool has_validation_report = false;
    bool used_mft_scan = false;

    if (IsNtfsPath(options.path))
    {
        std::wcout << L"Scanning NTFS MFT subtree rooted at " << options.path
                   << L" ...\n";
        ok = ntfs::ScanPathMft(options.path, &tree, &error, &stats);
        used_mft_scan = ok;
    }
    else
    {
        if (options.validate)
        {
            std::wcout
                << L"Validation mode currently requires an NTFS path scan.\n";
            return 1;
        }
        std::wcout << L"Scanning directory " << options.path << L" ...\n";
        ok = ScanDirectory(options.path, &tree, nullptr);
        tree.Finalize();
    }

    double elapsed = timer.ElapsedSeconds();
    if (!ok)
    {
        std::wcout << L"Scan failed: "
                   << (error.empty() ? L"Unknown error." : error) << L"\n";
        return 2;
    }

    tree_stats = tree.ComputeStats();
    if (options.validate && used_mft_scan)
    {
        std::wcout << L"Running Win32 validation on sampled files ...\n";
        ok = ntfs::ValidateVolumeWithWin32(tree, stats, options.validate_sample,
                                           options.validate_edge_limit,
                                           &validation_report, &error);
        if (!ok)
        {
            std::wcout << L"Validation failed: "
                       << (error.empty() ? L"Unknown error." : error) << L"\n";
            return 3;
        }
        has_validation_report = true;
    }

    std::wcout << L"Scan complete in " << elapsed << L" seconds.\n";
    std::wcout << L"Nodes: " << tree.NodeCount() << L"\n";
    if (used_mft_scan)
    {
        const Node* root = tree.GetNode(tree.RootId());
        std::wcout << L"MFT records read: " << stats.records_read << L"\n";
        if (stats.bytes_per_file_record > 0 && stats.mft_valid_data_length > 0)
        {
            uint64_t expected =
                stats.mft_valid_data_length / stats.bytes_per_file_record;
            std::wcout << L"Expected records (MFT valid length): " << expected
                       << L"\n";
        }
        std::wcout << L"Parsed ok: " << stats.records_parsed << L"\n";
        std::wcout << L"In use: " << stats.records_in_use << L"\n";
        std::wcout << L"Added nodes: " << stats.records_added << L"\n";
        std::wcout << L"Parse failed: " << stats.records_parse_failed << L"\n";
        std::wcout << L"No name: " << stats.records_no_name << L"\n";
        if (stats.records_no_name_with_size > 0)
        {
            std::wcout << L"No-name files with size: "
                       << stats.records_no_name_with_size << L" ("
                       << HumanizeBytes(stats.records_no_name_size) << L")\n";
        }
        if (stats.extension_size_records > 0)
        {
            std::wcout << L"Extension records merged: "
                       << stats.extension_size_records << L" ("
                       << HumanizeBytes(stats.extension_size_total) << L")\n";
        }
        if (stats.extension_missing_base_records > 0)
        {
            std::wcout << L"Extension records missing base: "
                       << stats.extension_missing_base_records << L" ("
                       << HumanizeBytes(stats.extension_missing_base_total)
                       << L")\n";
        }
        std::wcout << L"Files: " << tree_stats.file_count << L"  Dirs: "
                   << tree_stats.dir_count << L"\n";
        std::wcout << L"Total file size: "
                   << HumanizeBytes(tree_stats.total_file_size) << L"\n";
        if (root)
        {
            std::wcout << L"Root aggregated size: " << HumanizeBytes(root->size)
                       << L"\n";
        }
        std::wcout << L"Reachable file size: "
                   << HumanizeBytes(tree_stats.reachable_file_size) << L"\n";
        std::wcout << L"Unreachable files: "
                   << tree_stats.unreachable_file_count << L" ("
                   << HumanizeBytes(tree_stats.unreachable_file_size) << L")\n";
        std::wcout << L"Zero-size files: " << tree_stats.zero_size_files
                   << L"\n";
        if (has_validation_report)
        {
            std::wcout << L"Validation sampled files: "
                       << validation_report.sampled_files_checked << L" / "
                       << validation_report.sample_file_candidates << L"\n";
            std::wcout << L"Validation comparable IDs: "
                       << validation_report.sampled_files_with_comparable_ids
                       << L" / " << validation_report.sampled_files_checked
                       << L"\n";
            std::wcout << L"Validation mismatches: size="
                       << validation_report.size_mismatches << L" parent="
                       << validation_report.parent_mismatches << L" name="
                       << validation_report.name_mismatches << L" type="
                       << validation_report.type_mismatches << L" frn="
                       << validation_report.frn_mismatches << L" missing="
                       << validation_report.missing_paths << L"\n";
            std::wcout << L"Edge cases inspected: "
                       << validation_report.edge_cases.entries_inspected
                       << L" hardlinks="
                       << validation_report.edge_cases.hard_link_entries
                       << L" sparse="
                       << validation_report.edge_cases.sparse_entries
                       << L" compressed="
                       << validation_report.edge_cases.compressed_entries
                       << L" reparse="
                       << validation_report.edge_cases.reparse_entries
                       << L" ads=" << validation_report.edge_cases.ads_entries
                       << L" extension-records="
                       << validation_report.attribute_list_extension_records
                       << L"\n";
        }
    }

    auto top = tree.TopN(options.top);
    std::wcout << L"Top " << top.size() << L" entries:\n";
    for (const Node* node : top)
    {
        std::wstring path = tree.BuildPath(node->id);
        std::wcout << HumanizeBytes(node->size) << L"  " << path << L"\n";
    }

    if (!options.json_out.empty())
    {
        if (!WriteJsonReport(
                options.json_out, options, used_mft_scan, elapsed, tree,
                tree_stats, stats,
                has_validation_report ? &validation_report : nullptr, &error))
        {
            std::wcout << L"Failed to write JSON report: "
                       << (error.empty() ? L"Unknown error." : error) << L"\n";
            return 4;
        }
        std::wcout << L"JSON report written to " << options.json_out << L"\n";
    }

    return 0;
}
