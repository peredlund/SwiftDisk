#include <windows.h>

#include <cwchar>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "common/file_tree.h"
#include "common/stopwatch.h"
#include "core/dir_scanner.h"
#include "core/ntfs/mft_scanner.h"

namespace space_scanner {

struct Options {
    wchar_t volume = 0;
    std::wstring path;
    size_t top = 20;
};

void PrintUsage() {
    std::wcout << L"SpaceScanner CLI\n";
    std::wcout << L"Usage:\n";
    std::wcout << L"  space_scanner_cli.exe scan --volume C\n";
    std::wcout << L"  space_scanner_cli.exe scan --path C:\\\\Users\n";
    std::wcout << L"Options:\n";
    std::wcout << L"  --top N          Number of entries to list (default 20)\n";
}

bool ParseArgs(int argc, wchar_t* argv[], Options* options) {
    if (argc < 2 || wcscmp(argv[1], L"scan") != 0) {
        return false;
    }

    for (int i = 2; i < argc; ++i) {
        if (wcscmp(argv[i], L"--volume") == 0 && i + 1 < argc) {
            options->volume = argv[++i][0];
        } else if (wcscmp(argv[i], L"--path") == 0 && i + 1 < argc) {
            options->path = argv[++i];
        } else if (wcscmp(argv[i], L"--top") == 0 && i + 1 < argc) {
            options->top = static_cast<size_t>(_wtoi(argv[++i]));
        } else if (wcscmp(argv[i], L"--help") == 0 || wcscmp(argv[i], L"-h") == 0) {
            return false;
        }
    }

    return options->volume != 0 || !options->path.empty();
}

std::wstring HumanizeBytes(uint64_t bytes) {
    const wchar_t* units[] = {L"B", L"KiB", L"MiB", L"GiB", L"TiB"};
    double value = static_cast<double>(bytes);
    size_t unit_index = 0;
    while (value >= 1024.0 && unit_index < std::size(units) - 1) {
        value /= 1024.0;
        ++unit_index;
    }
    wchar_t buffer[64];
    swprintf_s(buffer, L"%.2f %s", value, units[unit_index]);
    return buffer;
}

} // namespace space_scanner

int wmain(int argc, wchar_t* argv[]) {
    using namespace space_scanner;

    Options options;
    if (!ParseArgs(argc, argv, &options)) {
        PrintUsage();
        return 1;
    }

    FileTree tree;
    Stopwatch timer;
    bool ok = false;
    std::wstring error;
    ntfs::MftScanStats stats;

    if (options.volume != 0) {
        std::wcout << L"Scanning NTFS MFT on volume " << options.volume << L": ...\n";
        ok = ntfs::ScanVolumeMft(options.volume, &tree, &error, &stats);
    } else {
        std::wcout << L"Scanning directory " << options.path << L" ...\n";
        ok = ScanDirectory(options.path, &tree, nullptr);
        tree.Finalize();
    }

    double elapsed = timer.ElapsedSeconds();
    if (!ok) {
        std::wcout << L"Scan failed: " << (error.empty() ? L"Unknown error." : error) << L"\n";
        return 2;
    }

    std::wcout << L"Scan complete in " << elapsed << L" seconds.\n";
    std::wcout << L"Nodes: " << tree.NodeCount() << L"\n";
    if (options.volume != 0) {
        FileTree::TreeStats tree_stats = tree.ComputeStats();
        const Node* root = tree.GetNode(tree.RootId());
        std::wcout << L"MFT records read: " << stats.records_read << L"\n";
        if (stats.bytes_per_file_record > 0 && stats.mft_valid_data_length > 0) {
            uint64_t expected = stats.mft_valid_data_length / stats.bytes_per_file_record;
            std::wcout << L"Expected records (MFT valid length): " << expected << L"\n";
        }
        std::wcout << L"Parsed ok: " << stats.records_parsed << L"\n";
        std::wcout << L"In use: " << stats.records_in_use << L"\n";
        std::wcout << L"Added nodes: " << stats.records_added << L"\n";
        std::wcout << L"Parse failed: " << stats.records_parse_failed << L"\n";
        std::wcout << L"No name: " << stats.records_no_name << L"\n";
        if (stats.records_no_name_with_size > 0) {
            std::wcout << L"No-name files with size: " << stats.records_no_name_with_size
                       << L" (" << HumanizeBytes(stats.records_no_name_size) << L")\n";
        }
        if (stats.extension_size_records > 0) {
            std::wcout << L"Extension records merged: " << stats.extension_size_records
                       << L" (" << HumanizeBytes(stats.extension_size_total) << L")\n";
        }
        if (stats.extension_missing_base_records > 0) {
            std::wcout << L"Extension records missing base: " << stats.extension_missing_base_records
                       << L" (" << HumanizeBytes(stats.extension_missing_base_total) << L")\n";
        }
        std::wcout << L"Files: " << tree_stats.file_count << L"  Dirs: " << tree_stats.dir_count << L"\n";
        std::wcout << L"Total file size: " << HumanizeBytes(tree_stats.total_file_size) << L"\n";
        if (root) {
            std::wcout << L"Root aggregated size: " << HumanizeBytes(root->size) << L"\n";
        }
        std::wcout << L"Reachable file size: " << HumanizeBytes(tree_stats.reachable_file_size) << L"\n";
        std::wcout << L"Unreachable files: " << tree_stats.unreachable_file_count
                   << L" (" << HumanizeBytes(tree_stats.unreachable_file_size) << L")\n";
        std::wcout << L"Zero-size files: " << tree_stats.zero_size_files << L"\n";
    }

    auto top = tree.TopN(options.top);
    std::wcout << L"Top " << top.size() << L" entries:\n";
    for (const Node* node : top) {
        std::wstring path = tree.BuildPath(node->id);
        std::wcout << HumanizeBytes(node->size) << L"  " << path << L"\n";
    }

    return 0;
}
