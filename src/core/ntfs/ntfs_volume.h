// SPDX-FileCopyrightText: 2026 Per Edlund
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace swift_disk::ntfs
{

struct DataRun;

struct VolumeInfo
{
    uint32_t bytes_per_sector = 0;
    uint32_t bytes_per_cluster = 0;
    uint32_t bytes_per_file_record = 0;
    uint64_t mft_valid_data_length = 0;
    std::wstring fs_name;
};

/**
 Provides low-level NTFS volume access for MFT scanning.
 **/
class NtfsVolume
{
  public:
    NtfsVolume();
    ~NtfsVolume();

    NtfsVolume(const NtfsVolume&) = delete;
    NtfsVolume& operator=(const NtfsVolume&) = delete;
    NtfsVolume(NtfsVolume&& other) noexcept;
    NtfsVolume& operator=(NtfsVolume&& other) noexcept;

    bool Open(wchar_t drive_letter);
    void Close();

    bool IsOpen() const;
    bool IsNtfs() const;
    bool IsReadOnlyAccess() const;
    const VolumeInfo& Info() const;

    using RecordCallback = std::function<void(uint8_t* record, size_t record_size, uint64_t frn)>;
    bool EnumerateMftRecords(const RecordCallback& callback, uint64_t* out_records);

  private:
    bool ReadFileRecord(uint64_t frn, std::vector<uint8_t>* out_record);
    bool GetMftDataRuns(std::vector<DataRun>* runs, uint64_t* data_size);

    void* m_handle = nullptr;
    VolumeInfo m_info;
    bool m_is_ntfs = false;
    uint32_t m_open_access = 0;
    std::wstring m_volume_path;
};

} // namespace swift_disk::ntfs
