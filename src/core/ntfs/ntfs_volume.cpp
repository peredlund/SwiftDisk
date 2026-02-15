#include "core/ntfs/ntfs_volume.h"

#include "core/ntfs/mft_parser.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <iterator>

namespace space_scanner::ntfs {

namespace {

std::wstring BuildVolumePath(wchar_t drive_letter) {
    std::wstring path = L"\\\\.\\";
    path.push_back(drive_letter);
    path.push_back(L':');
    return path;
}

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
    handle_ = other.handle_;
    info_ = other.info_;
    is_ntfs_ = other.is_ntfs_;
    volume_path_ = std::move(other.volume_path_);
    other.handle_ = nullptr;
    other.is_ntfs_ = false;
}

NtfsVolume& NtfsVolume::operator=(NtfsVolume&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    Close();
    handle_ = other.handle_;
    info_ = other.info_;
    is_ntfs_ = other.is_ntfs_;
    volume_path_ = std::move(other.volume_path_);
    other.handle_ = nullptr;
    other.is_ntfs_ = false;
    return *this;
}

bool NtfsVolume::Open(wchar_t drive_letter) {
    Close();

    volume_path_ = BuildVolumePath(drive_letter);
    open_access_ = GENERIC_READ;
    handle_ = CreateFileW(
        volume_path_.c_str(),
        open_access_,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);

    if (handle_ == INVALID_HANDLE_VALUE) {
        handle_ = nullptr;
        return false;
    }

    NTFS_VOLUME_DATA_BUFFER volume_data = {};
    DWORD bytes_returned = 0;
    if (!DeviceIoControl(
            handle_,
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

    info_.bytes_per_sector = volume_data.BytesPerSector;
    info_.bytes_per_cluster = volume_data.BytesPerCluster;
    info_.bytes_per_file_record = volume_data.BytesPerFileRecordSegment;
    info_.mft_valid_data_length = static_cast<uint64_t>(volume_data.MftValidDataLength.QuadPart);

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
        info_.fs_name = fs_name;
        is_ntfs_ = (_wcsicmp(fs_name, L"NTFS") == 0);
    } else {
        is_ntfs_ = false;
    }

    return true;
}

void NtfsVolume::Close() {
    if (handle_) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
    info_ = {};
    is_ntfs_ = false;
    open_access_ = 0;
    volume_path_.clear();
}

bool NtfsVolume::IsOpen() const {
    return handle_ != nullptr;
}

bool NtfsVolume::IsNtfs() const {
    return is_ntfs_;
}

bool NtfsVolume::IsReadOnlyAccess() const {
    return open_access_ == GENERIC_READ;
}

const VolumeInfo& NtfsVolume::Info() const {
    return info_;
}

bool NtfsVolume::ReadFileRecord(uint64_t frn, std::vector<uint8_t>* out_record) {
    if (!handle_ || !out_record) {
        return false;
    }

    NTFS_FILE_RECORD_INPUT_BUFFER input = {};
    input.FileReferenceNumber.QuadPart = static_cast<LONGLONG>(frn);

    std::vector<uint8_t> buffer(sizeof(NTFS_FILE_RECORD_OUTPUT_BUFFER) + info_.bytes_per_file_record);
    DWORD bytes_returned = 0;
    if (!DeviceIoControl(
            handle_,
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

bool NtfsVolume::GetMftDataRuns(std::vector<DataRun>* runs, uint64_t* data_size) {
    if (!runs) {
        return false;
    }

    std::vector<uint8_t> record;
    if (!ReadFileRecord(0, &record)) {
        return false;
    }

    return ExtractMftDataRuns(record.data(), record.size(), info_.bytes_per_sector, runs, data_size);
}

bool NtfsVolume::EnumerateMftRecords(const RecordCallback& callback, uint64_t* out_records) {
    if (!handle_ || !callback) {
        return false;
    }

    std::vector<DataRun> runs;
    uint64_t mft_data_size = 0;
    if (!GetMftDataRuns(&runs, &mft_data_size)) {
        return false;
    }

    const uint32_t record_size = info_.bytes_per_file_record;
    if (record_size == 0) {
        return false;
    }

    const size_t base_chunk = 8 * 1024 * 1024;
    size_t chunk_size = (base_chunk / record_size) * record_size;
    if (chunk_size == 0) {
        chunk_size = record_size;
    }

    std::vector<uint8_t> buffer(chunk_size + record_size);
    size_t carry = 0;
    uint64_t record_index = 0;

    uint64_t mft_limit = info_.mft_valid_data_length;
    if (mft_limit == 0 && mft_data_size > 0) {
        mft_limit = mft_data_size;
    }
    uint64_t remaining_mft_bytes = mft_limit;
    for (const auto& run : runs) {
        if (run.length == 0 || run.lcn <= 0) {
            uint64_t run_bytes = run.length * static_cast<uint64_t>(info_.bytes_per_cluster);
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

        uint64_t run_bytes = run.length * static_cast<uint64_t>(info_.bytes_per_cluster);
        if (remaining_mft_bytes > 0) {
            run_bytes = std::min(run_bytes, remaining_mft_bytes);
        }
        uint64_t run_offset = static_cast<uint64_t>(run.lcn) * info_.bytes_per_cluster;
        uint64_t remaining = run_bytes;

        while (remaining > 0) {
            size_t to_read = static_cast<size_t>(std::min<uint64_t>(chunk_size, remaining));
            LARGE_INTEGER offset;
            offset.QuadPart = static_cast<LONGLONG>(run_offset + (run_bytes - remaining));
            if (!SetFilePointerEx(static_cast<HANDLE>(handle_), offset, nullptr, FILE_BEGIN)) {
                return false;
            }

            DWORD bytes_read = 0;
            if (!ReadFile(static_cast<HANDLE>(handle_), buffer.data() + carry, static_cast<DWORD>(to_read), &bytes_read, nullptr)) {
                return false;
            }

            size_t total = carry + bytes_read;
            size_t records = total / record_size;
            for (size_t i = 0; i < records; ++i) {
                uint8_t* record_ptr = buffer.data() + i * record_size;
                callback(record_ptr, record_size, record_index++);
            }
            carry = total % record_size;
            if (carry > 0) {
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
