// SPDX-FileCopyrightText: 2026 Per Edlund
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/ntfs/mft_parser.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <utility>

namespace swift_disk::ntfs
{

namespace
{

constexpr uint32_t kFileSignature = 0x454C4946; // "FILE"
constexpr uint32_t kAttrTypeEnd = 0xFFFFFFFF;
constexpr uint32_t kAttrTypeFileName = 0x30;
constexpr uint32_t kAttrTypeData = 0x80;

#pragma pack(push, 1)
struct FileRecordHeader
{
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

struct AttrHeaderCommon
{
    uint32_t type;
    uint32_t length;
    uint8_t non_resident;
    uint8_t name_length;
    uint16_t name_offset;
    uint16_t flags;
    uint16_t attr_id;
};

struct AttrHeaderResident
{
    AttrHeaderCommon common;
    uint32_t value_length;
    uint16_t value_offset;
    uint8_t indexed_flag;
    uint8_t padding;
};

struct AttrHeaderNonResident
{
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

struct FileNameAttr
{
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
bool ApplyFixup(uint8_t* record, size_t record_size, uint16_t usa_offset, uint16_t usa_count, uint32_t bytes_per_sector)
{
    if (usa_offset + usa_count * sizeof(uint16_t) > record_size)
    {
        return false;
    }

    // USA layout: entry 0 is the expected sector trailer signature,
    // entries 1..N are the original trailer words to restore.
    auto* usa = reinterpret_cast<uint16_t*>(record + usa_offset);
    uint16_t usa_value = usa[0];

    bool ok = true;
    for (uint16_t i = 1; i < usa_count; ++i)
    {
        size_t sector_offset = (static_cast<size_t>(i) * bytes_per_sector) - sizeof(uint16_t);
        if (sector_offset + sizeof(uint16_t) > record_size)
        {
            return false;
        }
        auto* sector_last = reinterpret_cast<uint16_t*>(record + sector_offset);
        if (*sector_last != usa_value && *sector_last != usa[i])
        {
            ok = false;
            break;
        }
    }
    if (!ok)
    {
        return false;
    }

    for (uint16_t i = 1; i < usa_count; ++i)
    {
        size_t sector_offset = (static_cast<size_t>(i) * bytes_per_sector) - sizeof(uint16_t);
        auto* sector_last = reinterpret_cast<uint16_t*>(record + sector_offset);
        if (*sector_last == usa_value)
        {
            *sector_last = usa[i];
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
uint8_t NamePreference(uint8_t name_type)
{
    switch (name_type)
    {
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
Retains distinct hard-link paths while choosing the same preferred display name
as before. DOS-only names are fallbacks, not additional directory entries.

Returns: True when the preferred name changes.
**/
bool ParsedRecord::AddFileName(std::wstring file_name, uint64_t file_parent_frn, uint8_t name_preference)
{
    if (file_name.empty() || name_preference == 0)
    {
        return false;
    }
    const bool same_path = name == file_name && parent_frn == file_parent_frn;
    const auto additional = std::find_if(m_additional_names.begin(), m_additional_names.end(),
        [&](const ParsedFileName& entry)
        {
            return entry.m_parent_frn == file_parent_frn && entry.m_name == file_name;
        });
    if (name_preference > m_name_preference)
    {
        if (additional != m_additional_names.end())
        {
            m_additional_names.erase(additional);
        }
        if (m_name_preference > 1 && !same_path)
        {
            m_additional_names.push_back(
            {
                std::move(name), parent_frn, m_name_preference
            });
        }
        name = std::move(file_name);
        parent_frn = file_parent_frn;
        m_name_preference = name_preference;
        return true;
    }
    if (name_preference > 1 && !same_path && additional == m_additional_names.end())
    {
        m_additional_names.push_back(
        {
            std::move(file_name), file_parent_frn, name_preference
        });
    }
    return false;
}

/**
 Parses one FILE record and extracts core node metadata.
 Pulls name, parent FRN, base FRN, in-use flags, and primary stream size.
 
 Parameters:
   record: Mutable record bytes.
   record_size: Record byte count.
   bytes_per_sector: Sector size used for fixup validation.
   frn: File record number associated with this record.
   out: Destination parsed record.
 
 Returns: true when the minimum buffer size and FILE signature are accepted.
 Attribute parsing may stop early and leave partial metadata.
 **/
bool ParseFileRecord(uint8_t* record, size_t record_size, uint32_t bytes_per_sector, uint64_t frn, ParsedRecord* out)
{
    if (!record || record_size < sizeof(FileRecordHeader) || !out)
    {
        return false;
    }

    auto* header = reinterpret_cast<FileRecordHeader*>(record);
    if (header->signature != kFileSignature)
    {
        return false;
    }

    if (!ApplyFixup(record, record_size, header->usa_offset, header->usa_count, bytes_per_sector))
    {
        // Fixup validation failed. Continue parsing the original record bytes
        // without modifying sector trailers.
    }
    else
    {
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
    out->m_size_on_disk = 0;
    out->m_wof_size_on_disk = 0;
    out->has_data_attribute = false;
    out->m_name_preference = 0;
    out->m_additional_names.clear();
    if (!in_use)
    {
        return true;
    }

    uint16_t attr_offset = header->first_attr_offset;
    uint64_t data_size = 0;
    uint64_t size_on_disk = 0;
    uint64_t wof_size_on_disk = 0;
    uint64_t name_real_size = 0;

    while (attr_offset + sizeof(AttrHeaderCommon) <= record_size)
    {
        auto* attr = reinterpret_cast<AttrHeaderCommon*>(record + attr_offset);
        if (attr->type == kAttrTypeEnd)
        {
            break;
        }
        if (attr->length == 0 || attr_offset + attr->length > record_size)
        {
            break;
        }

        if (attr->type == kAttrTypeFileName && attr->non_resident == 0)
        {
            auto* resident = reinterpret_cast<AttrHeaderResident*>(attr);
            // Validate that resident payload lies fully inside this attribute.
            if (resident->value_offset + resident->value_length <= attr->length)
            {
                auto* name_attr = reinterpret_cast<FileNameAttr*>(
                    reinterpret_cast<uint8_t*>(attr) + resident->value_offset);
                // FILE_NAME uses a flexible trailing UTF-16 name buffer.
                size_t name_bytes = static_cast<size_t>(name_attr->name_length) * sizeof(wchar_t);
                size_t name_available = resident->value_length;
                // `FileNameAttr` declares `name[1]`; subtract one wchar to get fixed part size.
                if (sizeof(FileNameAttr) - sizeof(wchar_t) + name_bytes <= name_available)
                {
                    // Keep all real paths, including names stored in extensions.
                    if (out->AddFileName(std::wstring(name_attr->name, name_attr->name_length),
                        name_attr->parent_ref & 0x0000FFFFFFFFFFFFULL, NamePreference(name_attr->name_type)))
                    {
                        // Keep FILE_NAME real size as fallback when DATA size is missing here.
                        name_real_size = name_attr->real_size;
                    }
                }
            }
        }
        else if (attr->type == kAttrTypeData)
        {
            const bool unnamed = attr->name_length == 0;
            const size_t name_bytes = static_cast<size_t>(attr->name_length) * sizeof(wchar_t);
            const bool wof_backing = !unnamed && attr->name_offset >= sizeof(AttrHeaderCommon) &&
                attr->name_offset + name_bytes <= attr->length &&
                std::wstring_view(reinterpret_cast<const wchar_t*>(
                    reinterpret_cast<const uint8_t*>(attr) + attr->name_offset), attr->name_length) == L"WofCompressedData";
            if (unnamed || wof_backing)
            {
                // WOF stores file content in this backing stream. Other named
                // streams, including NTFS's virtual $Bad stream, are not content
                // allocation for the primary file-size view.
                if (attr->non_resident == 0)
                {
                    if (attr->length < sizeof(AttrHeaderResident))
                    {
                        break;
                    }
                    auto* resident = reinterpret_cast<AttrHeaderResident*>(attr);
                    if (unnamed)
                    {
                        out->has_data_attribute = true;
                        data_size = std::max<uint64_t>(data_size, resident->value_length);
                    }
                    // Resident content shares its MFT record; no data clusters are allocated.
                }
                else
                {
                    if (attr->length < offsetof(AttrHeaderNonResident, compressed_size))
                    {
                        break;
                    }
                    auto* non_res = reinterpret_cast<AttrHeaderNonResident*>(attr);
                    // Stream-wide sizes are valid only in the first segment.
                    if (non_res->start_vcn != 0)
                    {
                        attr_offset = static_cast<uint16_t>(attr_offset + attr->length);
                        continue;
                    }
                    if (unnamed)
                    {
                        out->has_data_attribute = true;
                        data_size = std::max<uint64_t>(data_size, non_res->data_size);
                    }
                    uint64_t allocated = non_res->allocated_size;
                    if ((attr->flags & 0x80FF) != 0)
                    {
                        // Sparse/compressed allocation excludes holes and compression savings.
                        if (attr->length < sizeof(AttrHeaderNonResident) ||
                            non_res->data_run_offset < sizeof(AttrHeaderNonResident))
                        {
                            break;
                        }
                        allocated = non_res->compressed_size;
                    }
                    if (unnamed)
                    {
                        size_on_disk = std::max(size_on_disk, allocated);
                    }
                    else
                    {
                        wof_size_on_disk += allocated;
                    }
                }
            }
        }

        attr_offset = static_cast<uint16_t>(attr_offset + attr->length);
    }

    if (is_dir)
    {
        out->size = 0;
    }
    else
    {
        if (!out->has_data_attribute && data_size == 0 && name_real_size > 0)
        {
            data_size = name_real_size;
        }
        out->size = data_size;
        out->m_wof_size_on_disk = wof_size_on_disk;
        out->m_size_on_disk = size_on_disk + wof_size_on_disk;
    }
    return true;
}

} // namespace swift_disk::ntfs
