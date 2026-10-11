// SPDX-FileCopyrightText: 2026 Per Edlund
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/ntfs/ntfs_volume.h"

#include "core/ntfs/mft_extents.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <memory>

namespace swift_disk::ntfs
{

namespace
{

/**
 Builds a raw volume device path from a drive letter.
 
 Parameters:
   drive_letter: Drive letter, e.g. 'C'.
 
 Returns: Device path in \\.\X: form.
 **/
std::wstring BuildVolumePath(wchar_t drive_letter)
{
    std::wstring path = L"\\\\.\\";
    path.push_back(drive_letter);
    path.push_back(L':');
    return path;
}

/**
 Builds a root directory path from a drive letter.
 
 Parameters:
   drive_letter: Drive letter, e.g. 'C'.
 
 Returns: Root path in X:\ form.
 **/
std::wstring BuildRootPath(wchar_t drive_letter)
{
    std::wstring path;
    path.push_back(drive_letter);
    path.append(L":\\");
    return path;
}

} // namespace

NtfsVolume::NtfsVolume() = default;

NtfsVolume::~NtfsVolume()
{
    Close();
}

/**
 Transfers ownership of an open raw-volume handle and its cached metadata.

 Parameters:
   other: Volume object to move from. It is left closed.
 **/
NtfsVolume::NtfsVolume(NtfsVolume&& other) noexcept
{
    m_handle = other.m_handle;
    m_info = other.m_info;
    m_is_ntfs = other.m_is_ntfs;
    m_volume_path = std::move(other.m_volume_path);
    other.m_handle = nullptr;
    other.m_is_ntfs = false;
}

/**
 Closes the current handle before taking ownership of another volume's handle and
 metadata.

 Parameters:
   other: Volume object to move from. It is left closed.

 Returns: This volume after the transfer.
 **/
NtfsVolume& NtfsVolume::operator=(NtfsVolume&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }
    Close();
    m_handle = other.m_handle;
    m_info = other.m_info;
    m_is_ntfs = other.m_is_ntfs;
    m_volume_path = std::move(other.m_volume_path);
    other.m_handle = nullptr;
    other.m_is_ntfs = false;
    return *this;
}

/**
 Opens the volume in read-only mode and reads NTFS geometry metadata.
 
 Parameters:
   drive_letter: Volume letter to open.
 
 Returns: true when the raw handle opens and the NTFS geometry query succeeds.
 Filesystem-name detection is best effort and is reported by IsNtfs().
 **/
bool NtfsVolume::Open(wchar_t drive_letter)
{
    Close();

    m_volume_path = BuildVolumePath(drive_letter);
    m_open_access = GENERIC_READ;
    // Read-only raw volume handle; allows direct MFT access without file enumeration.
    m_handle = CreateFileW(
        m_volume_path.c_str(),
        m_open_access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);

    if (m_handle == INVALID_HANDLE_VALUE)
    {
        m_handle = nullptr;
        return false;
    }

    NTFS_VOLUME_DATA_BUFFER volume_data = {};
    DWORD bytes_returned = 0;
    // Pull low-level NTFS geometry needed for MFT parsing.
    if (!DeviceIoControl(
            m_handle,
            FSCTL_GET_NTFS_VOLUME_DATA,
            nullptr,
            0,
            &volume_data,
            sizeof(volume_data),
            &bytes_returned,
            nullptr) || bytes_returned < sizeof(volume_data) || volume_data.BytesPerSector == 0 ||
        volume_data.BytesPerCluster == 0 || volume_data.BytesPerFileRecordSegment == 0 ||
        volume_data.MftValidDataLength.QuadPart <= 0)
    {
        Close();
        return false;
    }

    m_info.bytes_per_sector = volume_data.BytesPerSector;
    m_info.bytes_per_cluster = volume_data.BytesPerCluster;
    m_info.bytes_per_file_record = volume_data.BytesPerFileRecordSegment;
    m_info.mft_valid_data_length = static_cast<uint64_t>(volume_data.MftValidDataLength.QuadPart);

    wchar_t fs_name[MAX_PATH] = {};
    if (GetVolumeInformationW(
            BuildRootPath(drive_letter).c_str(),
            nullptr,
            0,
            nullptr,
            nullptr,
            nullptr,
            fs_name,
            static_cast<DWORD>(std::size(fs_name))))
    {
        m_info.fs_name = fs_name;
        m_is_ntfs = (_wcsicmp(fs_name, L"NTFS") == 0);
    }
    else
    {
        m_is_ntfs = false;
    }

    return true;
}

/**
 Closes the raw-volume handle and resets all metadata and access state. The method
 is safe to call on an already closed volume.
 **/
void NtfsVolume::Close()
{
    if (m_handle)
    {
        CloseHandle(static_cast<HANDLE>(m_handle));
        m_handle = nullptr;
    }
    m_info = {};
    m_is_ntfs = false;
    m_open_access = 0;
    m_volume_path.clear();
}

bool NtfsVolume::IsOpen() const
{
    return m_handle != nullptr;
}

bool NtfsVolume::IsNtfs() const
{
    return m_is_ntfs;
}

bool NtfsVolume::IsReadOnlyAccess() const
{
    return m_open_access == GENERIC_READ;
}

const VolumeInfo& NtfsVolume::Info() const
{
    return m_info;
}

/**
Loads the MFT extent map through the Windows filesystem driver. The metadata
handle is synchronous and requests attributes only; raw data uses m_handle.

Parameters:
  extents: Receives all extents in logical order after a complete query.

Returns: True when the map covers the cached valid MFT data length.
**/
bool NtfsVolume::GetMftExtents(std::vector<MftExtent>* extents)
{
    if (!m_handle || !extents)
    {
        return false;
    }

    const std::wstring stream_path = m_volume_path + L"\\$MFT::$DATA";
    HANDLE stream_handle = CreateFileW(stream_path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (stream_handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    const std::unique_ptr<void, decltype(&CloseHandle)> stream(stream_handle, &CloseHandle);

    // A bounded page avoids repeatedly reallocating a potentially large map.
    std::vector<uint8_t> buffer(offsetof(RETRIEVAL_POINTERS_BUFFER, Extents) +
        64 * sizeof(RETRIEVAL_POINTERS_BUFFER::Extents[0]));
    std::vector<MftExtent> discovered;
    STARTING_VCN_INPUT_BUFFER input;
    input.StartingVcn.QuadPart = 0;
    uint64_t next_vcn = 0;
    bool more_data;
    do
    {
        DWORD bytes_returned = 0;
        const BOOL complete = DeviceIoControl(stream.get(), FSCTL_GET_RETRIEVAL_POINTERS,
            &input, sizeof(input), buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_returned, nullptr);
        more_data = !complete && GetLastError() == ERROR_MORE_DATA;
        if ((!complete && !more_data) || bytes_returned > buffer.size() ||
            !detail::AppendMftExtentPage(buffer.data(), bytes_returned, next_vcn,
                m_info.bytes_per_cluster, &discovered, &next_vcn))
        {
            return false;
        }
        input.StartingVcn.QuadPart = static_cast<LONGLONG>(next_vcn);
    }
    while (more_data);

    const uint64_t required_clusters = (m_info.mft_valid_data_length - 1) / m_info.bytes_per_cluster + 1;
    if (next_vcn < required_clusters)
    {
        return false;
    }
    *extents = std::move(discovered);
    return true;
}

/**
 Streams MFT bytes via the queried extent map and emits fixed-size raw record slots.
 Reads run extents sequentially and preserves partial record tails
 between chunk reads.
 
 Parameters:
   callback: Record callback invoked for each full raw record slot read.
   out_records: Optional output for the number of full raw record slots read.
 
 Returns: false on setup, seek, or read failure; otherwise true after the
 entire valid MFT length has been processed without an incomplete record tail.
 **/
bool NtfsVolume::EnumerateMftRecords(const RecordCallback& callback, uint64_t* out_records)
{
    if (!m_handle || !callback)
    {
        return false;
    }

    std::vector<MftExtent> extents;
    if (!GetMftExtents(&extents))
    {
        return false;
    }

    const uint32_t record_size = m_info.bytes_per_file_record;
    if (record_size == 0 || m_info.mft_valid_data_length % record_size != 0)
    {
        return false;
    }

    const size_t base_chunk = 8 * 1024 * 1024;
    // Keep reads aligned to full file-record boundaries.
    size_t chunk_size = (base_chunk / record_size) * record_size;
    if (chunk_size == 0)
    {
        chunk_size = record_size;
    }

    // Extra record-sized tail stores carry-over bytes between chunk reads.
    std::vector<uint8_t> buffer(chunk_size + record_size);
    size_t carry = 0;
    uint64_t record_index = 0;

    uint64_t remaining_mft_bytes = m_info.mft_valid_data_length;
    for (const auto& extent : extents)
    {
        uint64_t run_bytes = std::min(extent.m_cluster_count * m_info.bytes_per_cluster, remaining_mft_bytes);
        uint64_t run_offset = extent.m_start_lcn * m_info.bytes_per_cluster;
        uint64_t remaining = run_bytes;

        while (remaining > 0)
        {
            // Read bounded by chunk size and by remaining bytes in this run extent.
            size_t to_read = static_cast<size_t>(std::min<uint64_t>(chunk_size, remaining));
            LARGE_INTEGER offset;
            offset.QuadPart = static_cast<LONGLONG>(run_offset + (run_bytes - remaining));
            if (!SetFilePointerEx(static_cast<HANDLE>(m_handle), offset, nullptr, FILE_BEGIN))
            {
                return false;
            }

            DWORD bytes_read = 0;
            if (!ReadFile(static_cast<HANDLE>(m_handle), buffer.data() + carry, static_cast<DWORD>(to_read), &bytes_read, nullptr) ||
                bytes_read == 0 || bytes_read > to_read)
            {
                return false;
            }

            size_t total = carry + bytes_read;
            size_t records = total / record_size;
            for (size_t i = 0; i < records; ++i)
            {
                uint8_t* record_ptr = buffer.data() + i * record_size;
                callback(record_ptr, record_size, record_index++);
            }
            // Keep only trailing partial record bytes for the next iteration.
            carry = total % record_size;
            if (carry > 0)
            {
                // Preserve incomplete record bytes for the next read.
                std::memmove(buffer.data(), buffer.data() + records * record_size, carry);
            }

            remaining -= bytes_read;
            remaining_mft_bytes -= bytes_read;
        }
        if (remaining_mft_bytes == 0)
        {
            break;
        }
    }

    if (remaining_mft_bytes != 0 || carry != 0)
    {
        return false;
    }
    if (out_records)
    {
        *out_records = record_index;
    }
    return true;
}

} // namespace swift_disk::ntfs
