#include "core/ntfs/mft_parser.h"

#include <algorithm>
#include <cstring>

namespace space_scanner::ntfs {

namespace {

constexpr uint32_t kFileSignature = 0x454C4946; // "FILE"
constexpr uint32_t kAttrTypeEnd = 0xFFFFFFFF;
constexpr uint32_t kAttrTypeFileName = 0x30;
constexpr uint32_t kAttrTypeData = 0x80;

#pragma pack(push, 1)
struct FileRecordHeader {
    uint32_t signature;
    uint16_t usa_offset;
    uint16_t usa_count;
    uint64_t lsn;
    uint16_t sequence_number;
    uint16_t hard_link_count;
    uint16_t first_attr_offset;
    uint16_t flags;
    uint32_t bytes_in_use;
    uint32_t bytes_allocated;
    uint64_t base_file_record;
    uint16_t next_attr_id;
    uint16_t align;
    uint32_t record_number;
};

struct AttrHeaderCommon {
    uint32_t type;
    uint32_t length;
    uint8_t non_resident;
    uint8_t name_length;
    uint16_t name_offset;
    uint16_t flags;
    uint16_t attr_id;
};

struct AttrHeaderResident {
    AttrHeaderCommon common;
    uint32_t value_length;
    uint16_t value_offset;
    uint8_t indexed_flag;
    uint8_t padding;
};

struct AttrHeaderNonResident {
    AttrHeaderCommon common;
    uint64_t start_vcn;
    uint64_t last_vcn;
    uint16_t data_run_offset;
    uint16_t compression_unit_size;
    uint32_t padding;
    uint64_t allocated_size;
    uint64_t data_size;
    uint64_t initialized_size;
    uint64_t compressed_size;
};

struct FileNameAttr {
    uint64_t parent_ref;
    uint64_t creation_time;
    uint64_t mod_time;
    uint64_t mft_mod_time;
    uint64_t access_time;
    uint64_t allocated_size;
    uint64_t real_size;
    uint32_t flags;
    uint32_t reparse;
    uint8_t name_length;
    uint8_t name_type;
    wchar_t name[1];
};
#pragma pack(pop)

/**
 Applies NTFS update-sequence fixups for a raw FILE record.
 Restores the trailing word of each sector before attribute parsing.
 
 Parameters:
   record: Mutable record bytes.
   record_size: Record byte count.
   usa_offset: Offset to update sequence array.
   usa_count: Number of entries in update sequence array.
   bytes_per_sector: Sector size used for trailer restoration.
 
 Returns: true if fixup was applied or already present, otherwise false.
 **/
bool ApplyFixup(uint8_t* record, size_t record_size, uint16_t usa_offset, uint16_t usa_count, uint32_t bytes_per_sector) {
    if (usa_offset + usa_count * sizeof(uint16_t) > record_size) {
        return false;
    }

    // USA layout: entry 0 is the expected sector trailer signature,
    // entries 1..N are the original trailer words to restore.
    auto* usa = reinterpret_cast<uint16_t*>(record + usa_offset);
    uint16_t usa_value = usa[0];

    bool ok = true;
    for (uint16_t i = 1; i < usa_count; ++i) {
        size_t sector_offset = (static_cast<size_t>(i) * bytes_per_sector) - sizeof(uint16_t);
        if (sector_offset + sizeof(uint16_t) > record_size) {
            return false;
        }
        auto* sector_last = reinterpret_cast<uint16_t*>(record + sector_offset);
        if (*sector_last == usa_value) {
            *sector_last = usa[i];
        } else if (*sector_last == usa[i]) {
            // Fixup already applied by the source.
            continue;
        } else {
            ok = false;
            break;
        }
    }
    return ok;
}

/**
 Ranks FILE_NAME namespaces for display name selection.
 
 Parameters:
   name_type: NTFS file name namespace value.
 
 Returns: Higher score for preferred namespaces.
 **/
uint8_t NamePreference(uint8_t name_type) {
    switch (name_type) {
        case 3: // Win32 & DOS
            return 4;
        case 1: // Win32
            return 3;
        case 0: // POSIX
            return 2;
        case 2: // DOS
            return 1;
        default:
            return 0;
    }
}

} // namespace

/**
 Parses one FILE record and extracts core node metadata.
 Pulls name, parent FRN, base FRN, in-use flags, and primary stream size.
 
 Parameters:
   record: Mutable record bytes.
   record_size: Record byte count.
   bytes_per_sector: Sector size used for fixup validation.
   frn: File record number associated with this record.
   out: Destination parsed record.
 
 Returns: true if the record signature/shape is parseable, otherwise false.
 **/
bool ParseFileRecord(uint8_t* record, size_t record_size, uint32_t bytes_per_sector, uint64_t frn, ParsedRecord* out) {
    if (!record || record_size < sizeof(FileRecordHeader) || !out) {
        return false;
    }

    auto* header = reinterpret_cast<FileRecordHeader*>(record);
    if (header->signature != kFileSignature) {
        return false;
    }

    if (!ApplyFixup(record, record_size, header->usa_offset, header->usa_count, bytes_per_sector)) {
        // FSCTL_GET_NTFS_FILE_RECORD may return a record with fixups already applied.
        // In that case the USA check will fail, but the record is still parseable.
        // Continue without applying fixups.
    } else {
        header = reinterpret_cast<FileRecordHeader*>(record);
    }
    bool in_use = (header->flags & 0x01) != 0;
    bool is_dir = (header->flags & 0x02) != 0;
    uint64_t base_frn = header->base_file_record & 0x0000FFFFFFFFFFFFULL;

    out->frn = frn;
    out->base_frn = base_frn;
    out->parent_frn = 0;
    out->name.clear();
    out->is_dir = is_dir;
    out->in_use = in_use;
    out->size = 0;
    if (!in_use) {
        return true;
    }

    uint16_t attr_offset = header->first_attr_offset;
    uint8_t best_name_score = 0;
    std::wstring best_name;
    uint64_t parent_frn = 0;
    uint64_t data_size = 0;
    uint64_t name_real_size = 0;

    while (attr_offset + sizeof(AttrHeaderCommon) <= record_size) {
        auto* attr = reinterpret_cast<AttrHeaderCommon*>(record + attr_offset);
        if (attr->type == kAttrTypeEnd) {
            break;
        }
        if (attr->length == 0 || attr_offset + attr->length > record_size) {
            break;
        }

        if (attr->type == kAttrTypeFileName && attr->non_resident == 0) {
            auto* resident = reinterpret_cast<AttrHeaderResident*>(attr);
            // Validate that resident payload lies fully inside this attribute.
            if (resident->value_offset + resident->value_length <= attr->length) {
                auto* name_attr = reinterpret_cast<FileNameAttr*>(
                    reinterpret_cast<uint8_t*>(attr) + resident->value_offset);
                // FILE_NAME uses a flexible trailing UTF-16 name buffer.
                size_t name_bytes = static_cast<size_t>(name_attr->name_length) * sizeof(wchar_t);
                size_t name_available = resident->value_length;
                // `FileNameAttr` declares `name[1]`; subtract one wchar to get fixed part size.
                if (sizeof(FileNameAttr) - sizeof(wchar_t) + name_bytes <= name_available) {
                    // Prefer Win32-style names over DOS/other namespaces.
                    uint8_t score = NamePreference(name_attr->name_type);
                    if (score > best_name_score) {
                        best_name_score = score;
                        best_name.assign(name_attr->name, name_attr->name + name_attr->name_length);
                        // Parent reference packs FRN + sequence; low 48 bits are the FRN.
                        parent_frn = name_attr->parent_ref & 0x0000FFFFFFFFFFFFULL;
                        // Keep FILE_NAME real size as fallback when DATA size is missing here.
                        name_real_size = name_attr->real_size;
                    }
                }
            }
        } else if (attr->type == kAttrTypeData) {
            if (attr->name_length == 0) {
                // Only unnamed $DATA is counted as file primary content size.
                if (attr->non_resident == 0) {
                    auto* resident = reinterpret_cast<AttrHeaderResident*>(attr);
                    data_size = std::max<uint64_t>(data_size, resident->value_length);
                } else {
                    auto* non_res = reinterpret_cast<AttrHeaderNonResident*>(attr);
                    data_size = std::max<uint64_t>(data_size, non_res->data_size);
                }
            }
        }

        attr_offset = static_cast<uint16_t>(attr_offset + attr->length);
    }

    out->parent_frn = parent_frn;
    out->name = std::move(best_name);
    if (is_dir) {
        out->size = 0;
    } else {
        if (data_size == 0 && name_real_size > 0) {
            data_size = name_real_size;
        }
        out->size = data_size;
    }
    return true;
}

/**
 Extracts unnamed non-resident $DATA runs from an MFT record.
 Used for MFT record 0 to stream physical $MFT bytes efficiently.
 
 Parameters:
   record: Mutable record bytes.
   record_size: Record byte count.
   bytes_per_sector: Sector size used for fixup validation.
   runs: Destination run list.
   data_size: Optional logical data size output.
 
 Returns: true when at least one data run is decoded.
 **/
bool ExtractMftDataRuns(uint8_t* record, size_t record_size, uint32_t bytes_per_sector, std::vector<DataRun>* runs, uint64_t* data_size) {
    if (!record || record_size < sizeof(FileRecordHeader) || !runs) {
        return false;
    }

    auto* header = reinterpret_cast<FileRecordHeader*>(record);
    if (header->signature != kFileSignature) {
        return false;
    }

    if (!ApplyFixup(record, record_size, header->usa_offset, header->usa_count, bytes_per_sector)) {
        return false;
    }

    header = reinterpret_cast<FileRecordHeader*>(record);
    uint16_t attr_offset = header->first_attr_offset;

    while (attr_offset + sizeof(AttrHeaderCommon) <= record_size) {
        auto* attr = reinterpret_cast<AttrHeaderCommon*>(record + attr_offset);
        if (attr->type == kAttrTypeEnd) {
            break;
        }
        if (attr->length == 0 || attr_offset + attr->length > record_size) {
            break;
        }

        if (attr->type == kAttrTypeData && attr->non_resident != 0 && attr->name_length == 0) {
            auto* non_res = reinterpret_cast<AttrHeaderNonResident*>(attr);
            if (data_size) {
                *data_size = non_res->data_size;
            }
            if (non_res->data_run_offset < attr->length) {
                const uint8_t* run_ptr = reinterpret_cast<uint8_t*>(attr) + non_res->data_run_offset;
                size_t run_bytes = attr->length - non_res->data_run_offset;

                int64_t current_lcn = 0;
                size_t index = 0;
                while (index < run_bytes) {
                    // Runlist header byte: low nibble = length field size,
                    // high nibble = offset field size.
                    uint8_t header_byte = run_ptr[index++];
                    if (header_byte == 0) {
                        // Zero header marks end-of-runlist terminator.
                        break;
                    }
                    uint8_t len_size = header_byte & 0x0F;
                    uint8_t off_size = (header_byte >> 4) & 0x0F;
                    if (len_size == 0 || index + len_size + off_size > run_bytes) {
                        // Malformed runlist entry: stop parsing this attribute.
                        break;
                    }

                    uint64_t run_length = 0;
                    for (uint8_t i = 0; i < len_size; ++i) {
                        run_length |= static_cast<uint64_t>(run_ptr[index++]) << (i * 8);
                    }

                    int64_t run_offset = 0;
                    if (off_size > 0) {
                        uint64_t raw_offset = 0;
                        for (uint8_t i = 0; i < off_size; ++i) {
                            raw_offset |= static_cast<uint64_t>(run_ptr[index++]) << (i * 8);
                        }
                        // NTFS encodes run offsets as signed deltas from prior LCN.
                        int64_t sign_mask = 1LL << (off_size * 8 - 1);
                        if (raw_offset & static_cast<uint64_t>(sign_mask)) {
                            // Sign-extend variable-width negative offsets to 64-bit.
                            int64_t extend_mask = -1LL << (off_size * 8);
                            run_offset = static_cast<int64_t>(raw_offset) | extend_mask;
                        } else {
                            run_offset = static_cast<int64_t>(raw_offset);
                        }
                    }

                    // Non-resident DATA run offsets are relative to previous run LCN.
                    current_lcn += run_offset;
                    runs->push_back({current_lcn, run_length});
                }
                return !runs->empty();
            }
        }

        attr_offset = static_cast<uint16_t>(attr_offset + attr->length);
    }

    return false;
}

} // namespace space_scanner::ntfs
