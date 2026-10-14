#include "scan/directory_reader.hpp"

#include <sys/attr.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace cmac {
namespace {

// The attributes asked for every entry. Each costs about the same; the
// kernel's per-entry work dominates (SCANNER_STUDY.md §5a). The private size
// is not asked for: it doubles kernel time and is measured later only for
// files that may share blocks.
attrlist requested_attributes() {
    attrlist list{};
    list.bitmapcount = ATTR_BIT_MAP_COUNT;
    list.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_ERROR | ATTR_CMN_OBJTYPE |
                      ATTR_CMN_FLAGS | ATTR_CMN_FILEID;
    list.dirattr = ATTR_DIR_ENTRYCOUNT | ATTR_DIR_MOUNTSTATUS;
    list.fileattr = ATTR_FILE_LINKCOUNT | ATTR_FILE_ALLOCSIZE | ATTR_FILE_DATALENGTH;
    // With FSOPT_ATTR_CMN_EXTENDED the fork group carries ATTR_CMNEXT_*.
    list.forkattr = ATTR_CMNEXT_EXT_FLAGS;
    return list;
}

// Reads one record's fields in the order the kernel packs them: the
// returned set, then common, directory, file and extended-common groups,
// each in ascending bit order (ATTR_CMN_ERROR directly after the set).
// Fields are 4-byte aligned, so 8-byte values are copied, not cast.
struct RecordCursor final {
    const std::byte* position{nullptr};
    const std::byte* end{nullptr};
    bool overrun{false};
};

std::uint32_t take_u32(RecordCursor& cursor) {
    std::uint32_t value = 0;
    if (cursor.end - cursor.position < static_cast<std::ptrdiff_t>(sizeof value)) {
        cursor.overrun = true;
        return value;
    }
    std::memcpy(&value, cursor.position, sizeof value);
    cursor.position += sizeof value;
    return value;
}

std::uint64_t take_u64(RecordCursor& cursor) {
    std::uint64_t value = 0;
    if (cursor.end - cursor.position < static_cast<std::ptrdiff_t>(sizeof value)) {
        cursor.overrun = true;
        return value;
    }
    std::memcpy(&value, cursor.position, sizeof value);
    cursor.position += sizeof value;
    return value;
}

attribute_set_t take_attribute_set(RecordCursor& cursor) {
    attribute_set_t value{};
    if (cursor.end - cursor.position < static_cast<std::ptrdiff_t>(sizeof value)) {
        cursor.overrun = true;
        return value;
    }
    std::memcpy(&value, cursor.position, sizeof value);
    cursor.position += sizeof value;
    return value;
}

// ATTR_CMN_NAME is an attrreference_t whose offset is relative to the
// reference itself; the length counts a terminating NUL.
struct NameBytes final {
    const char* data{nullptr};
    std::uint32_t length{0};
    bool valid{false};
};

NameBytes take_name(RecordCursor& cursor, const std::byte* const record_start) {
    NameBytes result{};
    const std::byte* const reference = cursor.position;
    const std::uint32_t raw_offset = take_u32(cursor);
    const std::uint32_t raw_length = take_u32(cursor);
    if (cursor.overrun) return result;
    std::int32_t offset = 0;
    std::memcpy(&offset, &raw_offset, sizeof offset);
    const std::ptrdiff_t start = (reference - record_start) + offset;
    const std::ptrdiff_t record_length = cursor.end - record_start;
    if (start < 0 || raw_length == 0 || start > record_length) return result;
    if (static_cast<std::ptrdiff_t>(raw_length) > record_length - start) return result;
    const char* const bytes = reinterpret_cast<const char*>(record_start + start);
    std::uint32_t length = raw_length;
    if (bytes[length - 1] == '\0') length = length - 1;
    result.data = bytes;
    result.length = length;
    result.valid = true;
    return result;
}

NodeKind kind_of(const std::uint32_t object_type) {
    if (object_type == VREG) return NodeKind::file;
    if (object_type == VDIR) return NodeKind::directory;
    if (object_type == VLNK) return NodeKind::symbolic_link;
    return NodeKind::other;
}

std::uint16_t flags_of(const std::uint32_t bsd_flags, const std::uint64_t extended_flags) {
    std::uint16_t result = 0;
    if ((extended_flags & EF_IS_PURGEABLE) != 0) result |= static_cast<std::uint16_t>(NodeFlag::purgeable);
    if ((extended_flags & EF_IS_SPARSE) != 0) result |= static_cast<std::uint16_t>(NodeFlag::sparse);
    if ((extended_flags & EF_MAY_SHARE_BLOCKS) != 0) result |= static_cast<std::uint16_t>(NodeFlag::may_share_blocks);
    if ((bsd_flags & SF_DATALESS) != 0) result |= static_cast<std::uint16_t>(NodeFlag::dataless);
    if ((bsd_flags & UF_COMPRESSED) != 0) result |= static_cast<std::uint16_t>(NodeFlag::compressed);
    if ((bsd_flags & SF_FIRMLINK) != 0) result |= static_cast<std::uint16_t>(NodeFlag::firmlink);
    return result;
}

void append_name(Workspace& workspace, const NameBytes& name, ScanEntry& entry) {
    const std::size_t offset = workspace.names.size();
    if (offset > std::numeric_limits<std::uint32_t>::max() - name.length)
        throw std::length_error("scan name pool exceeds 4 GiB");
    workspace.names.insert(workspace.names.end(), name.data, name.data + name.length);
    entry.name.offset = static_cast<std::uint32_t>(offset);
    entry.name.length = name.length;
}

// Decodes one record into a new entry. False when the record is malformed;
// nothing is appended then.
bool append_record(const std::byte* const record, const std::uint32_t record_length,
                   const DirectoryReadRequest& request, Workspace& workspace) {
    RecordCursor cursor{record + sizeof(std::uint32_t), record + record_length, false};
    const attribute_set_t returned = take_attribute_set(cursor);
    std::uint32_t entry_error = 0;
    if ((returned.commonattr & ATTR_CMN_ERROR) != 0) entry_error = take_u32(cursor);
    if ((returned.commonattr & ATTR_CMN_NAME) == 0) return false;
    const NameBytes name = take_name(cursor, record);
    if (!name.valid) return false;
    std::uint32_t object_type = VNON;
    if ((returned.commonattr & ATTR_CMN_OBJTYPE) != 0) object_type = take_u32(cursor);
    std::uint32_t bsd_flags = 0;
    if ((returned.commonattr & ATTR_CMN_FLAGS) != 0) bsd_flags = take_u32(cursor);
    std::uint64_t file_id = 0;
    if ((returned.commonattr & ATTR_CMN_FILEID) != 0) file_id = take_u64(cursor);
    std::uint32_t entry_count = UINT32_MAX;
    if ((returned.dirattr & ATTR_DIR_ENTRYCOUNT) != 0) entry_count = take_u32(cursor);
    std::uint32_t mount_status = 0;
    if ((returned.dirattr & ATTR_DIR_MOUNTSTATUS) != 0) mount_status = take_u32(cursor);
    std::uint32_t link_count = 1;
    if ((returned.fileattr & ATTR_FILE_LINKCOUNT) != 0) link_count = take_u32(cursor);
    std::uint64_t allocated = 0;
    if ((returned.fileattr & ATTR_FILE_ALLOCSIZE) != 0) allocated = take_u64(cursor);
    std::uint64_t logical = 0;
    if ((returned.fileattr & ATTR_FILE_DATALENGTH) != 0) logical = take_u64(cursor);
    std::uint64_t extended_flags = 0;
    if ((returned.forkattr & ATTR_CMNEXT_EXT_FLAGS) != 0) extended_flags = take_u64(cursor);
    if (cursor.overrun) return false;

    ScanEntry entry{};
    entry.kind = kind_of(object_type);
    entry.file_id = FileId{file_id};
    entry.link_count = link_count;
    entry.allocated_bytes = allocated;
    entry.logical_bytes = logical;
    entry.flags = flags_of(bsd_flags, extended_flags);
    if (entry_error != 0) {
        entry.flags |= static_cast<std::uint16_t>(NodeFlag::attributes_failed);
        entry.allocated_bytes = 0;
        entry.logical_bytes = 0;
    }
    if (entry.kind == NodeKind::directory) {
        // A firmlink's count is its System-volume stub's (often 0), not its
        // target's, so its count is unknown.
        const bool firmlink = (bsd_flags & SF_FIRMLINK) != 0;
        entry.expected_entries = firmlink ? UINT32_MAX : entry_count;
        entry.directory_state = DirectoryState::failed;
        // A trigger (an automount point) is pruned with mount points:
        // opening it would mount, possibly over a network.
        const std::uint32_t crossing = DIR_MNTSTATUS_MNTPOINT | DIR_MNTSTATUS_TRIGGER;
        if (request.prune_mount_points && (mount_status & crossing) != 0)
            entry.directory_state = DirectoryState::mount_point;
    }
    append_name(workspace, name, entry);
    workspace.entries.push_back(entry);
    return true;
}

} // namespace

DirectoryReadResult read_directory(const DirectoryReadRequest& request, Workspace& workspace) {
    DirectoryReadResult result{};
    if (workspace.entries.size() >= std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("scan workspace exceeds 2^32 entries");
    result.first_entry = static_cast<std::uint32_t>(workspace.entries.size());
    attrlist list = requested_attributes();
    std::byte* const buffer = workspace.attribute_buffer.data();
    const std::size_t buffer_size = workspace.attribute_buffer.size();
    std::uint64_t records_seen = 0;
    bool reading = true;
    while (reading) {
        const int count = getattrlistbulk(request.directory_descriptor, &list, buffer, buffer_size,
                                          FSOPT_ATTR_CMN_EXTENDED);
        workspace.bulk_calls = workspace.bulk_calls + 1;
        if (count < 0) {
            result.state = DirectoryState::failed;
            result.error = errno;
            break;
        }
        if (count == 0) break;
        const std::byte* record = buffer;
        const std::byte* const buffer_end = buffer + buffer_size;
        for (int index = 0; index < count; ++index) {
            std::uint32_t record_length = 0;
            if (buffer_end - record < static_cast<std::ptrdiff_t>(sizeof record_length)) {
                reading = false;
                break;
            }
            std::memcpy(&record_length, record, sizeof record_length);
            if (record_length < sizeof record_length || record_length > static_cast<std::size_t>(buffer_end - record)) {
                reading = false;
                break;
            }
            if (!append_record(record, record_length, request, workspace)) {
                reading = false;
                break;
            }
            record += record_length;
        }
        if (!reading) {
            result.state = DirectoryState::failed;
            result.error = EBADMSG;
        }
        records_seen += static_cast<std::uint64_t>(count);
        // Checked after a call, never before the first: the first call is
        // what reveals a count that was wrong.
        if (records_seen >= request.expected_entries) break;
    }
    const std::size_t appended = workspace.entries.size() - result.first_entry;
    result.entry_count = static_cast<std::uint32_t>(appended);
    return result;
}

} // namespace cmac
