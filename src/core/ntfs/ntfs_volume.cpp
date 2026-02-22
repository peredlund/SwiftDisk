#include "core/ntfs/ntfs_volume.h"

#include "core/ntfs/mft_parser.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <iterator>

namespace space_scanner::ntfs {

namespace {

/**
 Builds a raw volume device path from a drive letter.
 
 Parameters:
   drive_letter: Drive letter, e.g. 'C'.
 
 Returns: Device path in \\.\X: form.
 **/
std::wstring BuildVolumePath(wchar_t drive_letter) {
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
std::wstring BuildRootPath(wchar_t drive_letter) {
    std::wstring path;
    path.push_back(drive_letter);
    path.append(L":\\");
    return path;
}

} // namespace

NtfsVolume::NtfsVolume() = default;

NtfsVolume::~NtfsVolume() {
    Close();
}

NtfsVolume::NtfsVolume(NtfsVolume&& other) noexcept {
    m_handle = other.m_handle;
    m_info = other.m_info;
    m_is_ntfs = other.m_is_ntfs;
    m_volume_path = std::move(other.m_volume_path);
    other.m_handle = nullptr;
    other.m_is_ntfs = false;
}

NtfsVolume& NtfsVolume::operator=(NtfsVolume&& other) noexcept {
    if (this == &other) {
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
 
 Returns: true when open and metadata queries succeed.
 **/
bool NtfsVolume::Open(wchar_t drive_letter) {
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

    if (m_handle == INVALID_HANDLE_VALUE) {
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
            nullptr)) {
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
            static_cast<DWORD>(std::size(fs_name)))) {
        m_info.fs_name = fs_name;
        m_is_ntfs = (_wcsicmp(fs_name, L"NTFS") == 0);
    } else {
        m_is_ntfs = false;
    }

    return true;
}

void NtfsVolume::Close() {
    if (m_handle) {
        CloseHandle(static_cast<HANDLE>(m_handle));
        m_handle = nullptr;
    }
    m_info = {};
    m_is_ntfs = false;
    m_open_access = 0;
    m_volume_path.clear();
}

bool NtfsVolume::IsOpen() const {
    return m_handle != nullptr;
}

bool NtfsVolume::IsNtfs() const {
    return m_is_ntfs;
}

bool NtfsVolume::IsReadOnlyAccess() const {
    return m_open_access == GENERIC_READ;
}

const VolumeInfo& NtfsVolume::Info() const {
    return m_info;
}

/**
 Reads a single FILE record by FRN using FSCTL_GET_NTFS_FILE_RECORD.
 
 Parameters:
   frn: File record number.
   out_record: Destination buffer.
 
 Returns: true when a non-empty record is returned.
 **/
bool NtfsVolume::ReadFileRecord(uint64_t frn, std::vector<uint8_t>* out_record) {
    if (!m_handle || !out_record) {
        return false;
    }

    // FSCTL_GET_NTFS_FILE_RECORD fetches one FILE record by reference number.
    NTFS_FILE_RECORD_INPUT_BUFFER input = {};
    input.FileReferenceNumber.QuadPart = static_cast<LONGLONG>(frn);

    std::vector<uint8_t> buffer(sizeof(NTFS_FILE_RECORD_OUTPUT_BUFFER) + m_info.bytes_per_file_record);
    DWORD bytes_returned = 0;
    if (!DeviceIoControl(
            m_handle,
            FSCTL_GET_NTFS_FILE_RECORD,
            &input,
            sizeof(input),
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytes_returned,
            nullptr)) {
        return false;
    }

    auto* output = reinterpret_cast<NTFS_FILE_RECORD_OUTPUT_BUFFER*>(buffer.data());
    if (output->FileRecordLength == 0) {
        return false;
    }

    out_record->assign(output->FileRecordBuffer, output->FileRecordBuffer + output->FileRecordLength);
    return true;
}

/**
 Loads MFT data runs by parsing MFT record 0.
 
 Parameters:
   runs: Destination data-run list.
   data_size: Optional MFT logical size output.
 
 Returns: true on successful run extraction.
 **/
bool NtfsVolume::GetMftDataRuns(std::vector<DataRun>* runs, uint64_t* data_size) {
    if (!runs) {
        return false;
    }

    std::vector<uint8_t> record;
    if (!ReadFileRecord(0, &record)) {
        return false;
    }

    return ExtractMftDataRuns(record.data(), record.size(), m_info.bytes_per_sector, runs, data_size);
}

/**
 Streams MFT bytes via run list and emits fixed-size FILE records.
 Reads run extents sequentially and preserves partial record tails
 between chunk reads.
 
 Parameters:
   callback: Record callback invoked per decoded record.
   out_records: Optional output for logical record count.
 
 Returns: true on successful full enumeration.
 **/
bool NtfsVolume::EnumerateMftRecords(const RecordCallback& callback, uint64_t* out_records) {
    if (!m_handle || !callback) {
        return false;
    }

    std::vector<DataRun> runs;
    uint64_t mft_data_size = 0;
    if (!GetMftDataRuns(&runs, &mft_data_size)) {
        return false;
    }

    const uint32_t record_size = m_info.bytes_per_file_record;
    if (record_size == 0) {
        return false;
    }

    const size_t base_chunk = 8 * 1024 * 1024;
    // Keep reads aligned to full file-record boundaries.
    size_t chunk_size = (base_chunk / record_size) * record_size;
    if (chunk_size == 0) {
        chunk_size = record_size;
    }

    // Extra record-sized tail stores carry-over bytes between chunk reads.
    std::vector<uint8_t> buffer(chunk_size + record_size);
    size_t carry = 0;
    uint64_t record_index = 0;

    uint64_t mft_limit = m_info.mft_valid_data_length;
    if (mft_limit == 0 && mft_data_size > 0) {
        mft_limit = mft_data_size;
    }
    uint64_t remaining_mft_bytes = mft_limit;
    for (const auto& run : runs) {
        if (run.length == 0 || run.lcn <= 0) {
            // Sparse/invalid run: skip physical read but keep logical record index aligned.
            uint64_t run_bytes = run.length * static_cast<uint64_t>(m_info.bytes_per_cluster);
            record_index += run_bytes / record_size;
            if (remaining_mft_bytes > 0) {
                if (run_bytes >= remaining_mft_bytes) {
                    remaining_mft_bytes = 0;
                    break;
                }
                remaining_mft_bytes -= run_bytes;
            }
            continue;
        }

        uint64_t run_bytes = run.length * static_cast<uint64_t>(m_info.bytes_per_cluster);
        if (remaining_mft_bytes > 0) {
            run_bytes = std::min(run_bytes, remaining_mft_bytes);
        }
        uint64_t run_offset = static_cast<uint64_t>(run.lcn) * m_info.bytes_per_cluster;
        uint64_t remaining = run_bytes;

        while (remaining > 0) {
            // Read bounded by chunk size and by remaining bytes in this run extent.
            size_t to_read = static_cast<size_t>(std::min<uint64_t>(chunk_size, remaining));
            LARGE_INTEGER offset;
            offset.QuadPart = static_cast<LONGLONG>(run_offset + (run_bytes - remaining));
            if (!SetFilePointerEx(static_cast<HANDLE>(m_handle), offset, nullptr, FILE_BEGIN)) {
                return false;
            }

            DWORD bytes_read = 0;
            if (!ReadFile(static_cast<HANDLE>(m_handle), buffer.data() + carry, static_cast<DWORD>(to_read), &bytes_read, nullptr)) {
                return false;
            }

            size_t total = carry + bytes_read;
            size_t records = total / record_size;
            for (size_t i = 0; i < records; ++i) {
                uint8_t* record_ptr = buffer.data() + i * record_size;
                callback(record_ptr, record_size, record_index++);
            }
            // Keep only trailing partial record bytes for the next iteration.
            carry = total % record_size;
            if (carry > 0) {
                // Preserve incomplete record bytes for the next read.
                std::memmove(buffer.data(), buffer.data() + records * record_size, carry);
            }

            remaining -= bytes_read;
            if (remaining_mft_bytes > 0) {
                remaining_mft_bytes -= bytes_read;
                if (remaining_mft_bytes == 0) {
                    remaining = 0;
                }
            }
            if (bytes_read == 0) {
                break;
            }
        }
        if (remaining_mft_bytes == 0) {
            break;
        }
    }

    if (out_records) {
        *out_records = record_index;
    }
    return true;
}

} // namespace space_scanner::ntfs
