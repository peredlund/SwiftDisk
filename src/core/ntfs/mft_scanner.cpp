#include "core/ntfs/mft_scanner.h"

#include "core/ntfs/mft_parser.h"
#include "core/ntfs/ntfs_volume.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace space_scanner::ntfs {

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
bool ScanVolumeMft(wchar_t drive_letter, FileTree* tree, std::wstring* error, MftScanStats* out_stats) {
    if (!tree) {
        if (error) {
            *error = L"FileTree is null.";
        }
        return false;
    }

    MftScanStats stats;

    NtfsVolume volume;
    if (!volume.Open(drive_letter)) {
        if (error) {
            *error = L"Failed to open volume.";
        }
        return false;
    }

    if (!volume.IsReadOnlyAccess()) {
        if (error) {
            *error = L"Refusing to scan: volume opened with write access.";
        }
        return false;
    }

    if (!volume.IsNtfs()) {
        if (error) {
            *error = L"Volume is not NTFS.";
        }
        return false;
    }

    stats.mft_valid_data_length = volume.Info().mft_valid_data_length;
    stats.bytes_per_file_record = volume.Info().bytes_per_file_record;

    constexpr uint64_t kRootFrn = 5;
    Node root_node;
    root_node.id = kRootFrn;
    root_node.parent_id = 0;
    root_node.is_dir = true;
    root_node.name = std::wstring(1, drive_letter) + L":";
    tree->AddNode(root_node);
    tree->SetRootId(kRootFrn);

    const uint32_t bytes_per_sector = volume.Info().bytes_per_sector;
    std::unordered_map<uint64_t, uint64_t> pending_sizes;
    bool ok = volume.EnumerateMftRecords(
        [&](uint8_t* record, size_t record_size, uint64_t frn) {
            stats.records_read++;
            ParsedRecord parsed;
            if (!ParseFileRecord(record, record_size, bytes_per_sector, frn, &parsed)) {
                stats.records_parse_failed++;
                return;
            }
            stats.records_parsed++;
            if (!parsed.in_use) {
                return;
            }
            stats.records_in_use++;
            if (parsed.name.empty()) {
                stats.records_no_name++;
                if (!parsed.is_dir && parsed.size > 0) {
                    if (parsed.base_frn != 0 && parsed.base_frn != parsed.frn) {
                        // Extension record for a base file: merge using max to avoid
                        // double counting overlapping size metadata across segments.
                        if (Node* existing = tree->GetNodeMutable(parsed.base_frn)) {
                            existing->size = std::max(existing->size, parsed.size);
                        } else {
                            // Base record may appear later in scan order; store pending merge.
                            auto& pending = pending_sizes[parsed.base_frn];
                            pending = std::max(pending, parsed.size);
                        }
                        stats.extension_size_records++;
                        stats.extension_size_total += parsed.size;
                    } else {
                        stats.records_no_name_with_size++;
                        stats.records_no_name_size += parsed.size;
                    }
                }
                return;
            }

            if (frn == kRootFrn) {
                Node* root = tree->GetNodeMutable(kRootFrn);
                if (root) {
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
            tree->AddNode(node);
            stats.records_added++;

            auto pending = pending_sizes.find(node.id);
            if (pending != pending_sizes.end()) {
                Node* existing = tree->GetNodeMutable(node.id);
                if (existing) {
                    existing->size = std::max(existing->size, pending->second);
                }
                pending_sizes.erase(pending);
            }
        },
        nullptr);

    if (!ok) {
        if (error) {
            *error = L"Failed while enumerating the MFT.";
        }
        return false;
    }

    if (!pending_sizes.empty()) {
        for (const auto& entry : pending_sizes) {
            stats.extension_missing_base_records++;
            stats.extension_missing_base_total += entry.second;
        }
    }

    tree->Finalize();
    if (out_stats) {
        *out_stats = stats;
    }
    return true;
}

} // namespace space_scanner::ntfs
