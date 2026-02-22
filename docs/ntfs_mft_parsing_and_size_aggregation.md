# SpaceScanner NTFS MFT Parsing and Size Aggregation

This document explains how the current codebase scans NTFS by reading the Master File Table (MFT), parses MFT records, builds an in-memory tree, and computes file/folder sizes.

## Table of Contents

- [1. NTFS MFT Basics](#1-ntfs-mft-basics)
- [2. End-to-End Scan Pipeline](#2-end-to-end-scan-pipeline)
- [3. Reading Raw MFT Bytes](#3-reading-raw-mft-bytes)
- [4. Parsing a FILE Record](#4-parsing-a-file-record)
- [5. Extension Record Merge Logic](#5-extension-record-merge-logic)
- [6. In-Memory Representation](#6-in-memory-representation)
- [7. Folder Size Aggregation](#7-folder-size-aggregation)
- [8. Tree Statistics](#8-tree-statistics)
- [9. Size Semantics Used by This Scanner](#9-size-semantics-used-by-this-scanner)
- [10. Important Edge Behavior](#10-important-edge-behavior)
- [11. Current Limitations](#11-current-limitations)
- [12. Summary](#12-summary)

## 1. NTFS MFT Basics

NTFS stores filesystem metadata in a special file named `$MFT`. Each file/directory is represented by one or more fixed-size FILE records (often 1024 bytes, but this code reads the actual size from volume metadata).

### 1.1 FILE record

- Starts with signature `FILE` (`0x454C4946`, little-endian).
- Contains flags such as:
  - in-use,
  - directory.
- Contains a variable-length attribute list.

### 1.2 Attributes used here

- `FILE_NAME` (`0x30`): name, parent reference, and some size fields.
- `DATA` (`0x80`): file stream metadata, including logical size.

### 1.3 Base and extension records

Some objects span multiple MFT records. Extension records reference a base record (`base_file_record`). This scanner merges extension size information into the base node.

### 1.4 USA/fixup

FILE records use Update Sequence Array (USA) fixups to protect sector ends. The parser restores sector trailer words before parsing attributes.

## 2. End-to-End Scan Pipeline

Primary NTFS path:

1. `ntfs::ScanVolumeMft` (`src/core/ntfs/mft_scanner.cpp`)
2. `NtfsVolume::EnumerateMftRecords` (`src/core/ntfs/ntfs_volume.cpp`)
3. `ParseFileRecord` (`src/core/ntfs/mft_parser.cpp`)
4. Insert nodes into `FileTree` (`src/common/file_tree.*`)
5. `FileTree::Finalize` for bottom-up folder aggregation
6. `FileTree::ComputeStats` for totals/reachability

CLI entry (`src/cli/main.cpp`) runs this pipeline and prints scan/stats/top-N output.

## 3. Reading Raw MFT Bytes

### 3.1 Open volume and load geometry

`NtfsVolume::Open`:

- Opens raw volume path like `\\.\E:` in read-only mode.
- Calls `FSCTL_GET_NTFS_VOLUME_DATA` to obtain:
  - bytes per sector,
  - bytes per cluster,
  - bytes per file record,
  - MFT valid data length.
- Verifies filesystem type is NTFS.

### 3.2 Get `$MFT` runlist

- `ReadFileRecord(0, ...)` reads MFT record 0 (`$MFT`).
- `ExtractMftDataRuns` parses unnamed non-resident `DATA` to decode runlist entries:
  - `lcn`: logical cluster number start,
  - `length`: run length in clusters.

### 3.3 Stream records from runs

`EnumerateMftRecords`:

- Iterates runs and reads large aligned chunks (8 MiB base chunk).
- Splits byte stream into full record-size slices.
- Uses a carry buffer to preserve partial records across reads.
- Stops at `mft_valid_data_length` when available.
- For sparse/invalid runs (`lcn <= 0`), skips physical reads but advances logical record index.

Callback receives: `record_ptr`, `record_size`, and logical `frn` index.

## 4. Parsing a FILE Record

`ParseFileRecord` in `src/core/ntfs/mft_parser.cpp`:

### 4.1 Validation and fixup

- Validates size and `FILE` signature.
- Attempts `ApplyFixup` using USA metadata and bytes/sector.
- Continues parsing if fixup fails in cases where source already provided fixed-up data.

### 4.2 Header fields extracted

- `in_use` from `flags & 0x01`.
- `is_dir` from `flags & 0x02`.
- `base_frn` from low 48 bits of `base_file_record`.

### 4.3 Attribute walk

Iterates variable-length attributes from `first_attr_offset` until end marker or bounds failure.

### 4.4 Name extraction

For resident `FILE_NAME` attributes:

- Chooses preferred namespace using `NamePreference`:
  - Win32&DOS (`3`) > Win32 (`1`) > POSIX (`0`) > DOS (`2`).
- Stores selected name and parent FRN.
- Captures `name_real_size` as fallback size.

### 4.5 Size extraction

For unnamed `DATA` (`name_length == 0`):

- resident: use `value_length`,
- non-resident: use `data_size`.

If multiple candidate `DATA` values are seen, parser keeps the maximum value.

### 4.6 Output mapping

- Directories: size `0` at parse stage.
- Files: DATA-derived logical size, with fallback to `FILE_NAME.real_size` if needed.

## 5. Extension Record Merge Logic

`ScanVolumeMft` merges no-name extension records into base records when:

- record has non-zero size,
- record points to a different `base_frn`.

Merge policy is **max**, not sum:

- existing base: `existing->size = max(existing->size, parsed.size)`
- pending base: `pending_sizes[base_frn] = max(...)`

This avoids overcounting when multiple extension records represent overlapping metadata for one file.

## 6. In-Memory Representation

### 6.1 Parsed record model

`ParsedRecord` (`src/core/ntfs/mft_parser.h`):

- `frn`, `parent_frn`, `base_frn`
- `name`
- `is_dir`, `in_use`
- `size`

### 6.2 Tree node model

`Node` (`src/common/file_tree.h`):

- `id` (FRN)
- `parent_id`
- `name`
- `is_dir`
- `size`

### 6.3 `FileTree` internals

- `m_nodes: unordered_map<uint64_t, Node>`
- `m_children: unordered_map<uint64_t, vector<uint64_t>>`
- `m_path_cache: unordered_map<uint64_t, wstring>`
- `m_root_id`

`AddNode` inserts/overwrites node and records parent-child linkage.

## 7. Folder Size Aggregation

Folder sizes are synthesized, not read directly from NTFS directory metadata.

`FileTree::Finalize` algorithm:

1. Compute node depth (memoized parent traversal).
2. Sort node IDs by descending depth (deepest first).
3. Add each node size into its parent.

Result: every directory accumulates all descendant sizes.

## 8. Tree Statistics

`FileTree::ComputeStats` computes:

- global counts and file size totals,
- zero-size file count,
- root-reachable file count/size via DFS from `m_root_id`,
- unreachable file count/size as total minus reachable.

This highlights orphaned/mislinked nodes.

## 9. Size Semantics Used by This Scanner

Current behavior:

- Uses logical size from unnamed `DATA` stream (`data_size`).
- Does not sum named alternate data streams (ADS).
- Directory size is aggregated from descendant nodes.

This generally matches content-size expectations from Explorer/WizTree better than allocated-cluster accounting.

## 10. Important Edge Behavior

- Root FRN is hard-coded as `5` for NTFS and named `<drive>:` in output.
- Parse failures are counted and skipped.
- Name-less records are tracked; extension-like records can still contribute size via base merge.
- Missing base records for pending extensions are counted in scan stats.
- Sparse/invalid MFT runs are handled logically by advancing index without physical read.

## 11. Current Limitations

- Hard-link modeling is simplified (FRN-keyed node model; not expanded as multiple directory entries).
- ADS sizes are ignored.
- Some rare attribute-list/fragmentation edge cases may need deeper handling.
- Finalization uses a depth sort (`O(N log N)`) rather than a linear-time aggregation strategy.

## 12. Summary

The scanner opens an NTFS volume, gets `$MFT` runlist from record 0, streams raw MFT records, parses record metadata (name/parent/size), builds an FRN-keyed tree, merges extension sizes with max semantics, and aggregates sizes bottom-up to produce folder totals and global stats.
