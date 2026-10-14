#include "scan/system_tree.hpp"

#include <sys/attr.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <string_view>

namespace cmac {
namespace {

// searchfs' match criteria, packed as getattrlist packs them: a length,
// then the value. Every object's file id lies in [0, UINT64_MAX]; APFS
// ignores the criteria anyway and returns every object (SCANNER_STUDY.md
// §3.3).
struct FileIdBound final {
    std::uint32_t length{sizeof(std::uint32_t) + sizeof(std::uint64_t)};
    std::uint64_t value{0};
} __attribute__((packed));

inline constexpr std::size_t search_buffer_bytes = 4 * 1024 * 1024;
// UTF-8 for searchfs' script code, as its manual requires.
inline constexpr unsigned int utf8_script = 0x08000103;

struct VolumeCapabilities final {
    std::uint32_t length{0};
    vol_capabilities_attr_t capabilities{};
} __attribute__((packed));

struct ByParent final {
    bool operator()(const SystemEntry& left, const SystemEntry& right) const { return left.parent < right.parent; }
};

// Parent, then id, then name: equal records are adjacent.
struct ByParentIdName final {
    const std::vector<char>* names{nullptr};
    bool operator()(const SystemEntry& left, const SystemEntry& right) const {
        if (left.parent != right.parent) return left.parent < right.parent;
        if (left.id != right.id) return left.id < right.id;
        const std::string_view a((*names).data() + left.name.offset, left.name.length);
        const std::string_view b((*names).data() + right.name.offset, right.name.length);
        return a < b;
    }
};

struct SameRecord final {
    const std::vector<char>* names{nullptr};
    bool operator()(const SystemEntry& left, const SystemEntry& right) const {
        if (left.parent != right.parent || left.id != right.id) return false;
        const std::string_view a((*names).data() + left.name.offset, left.name.length);
        const std::string_view b((*names).data() + right.name.offset, right.name.length);
        return a == b;
    }
};

// Bounded reads of one returned record. searchfs cannot return
// ATTR_CMN_RETURNED_ATTRS (EINVAL), so the layout is fixed: name, type,
// flags, file id, parent id, then for non-directories link count,
// allocation and length.
struct Cursor final {
    const char* position{nullptr};
    const char* end{nullptr};
    bool overrun{false};
};

template <typename Value>
Value take(Cursor& cursor) {
    Value value{};
    if (cursor.end - cursor.position < static_cast<std::ptrdiff_t>(sizeof(Value))) {
        cursor.overrun = true;
        return value;
    }
    std::memcpy(&value, cursor.position, sizeof(Value));
    cursor.position += sizeof(Value);
    return value;
}

NodeKind kind_of(const std::uint32_t object_type) {
    if (object_type == VREG) return NodeKind::file;
    if (object_type == VDIR) return NodeKind::directory;
    if (object_type == VLNK) return NodeKind::symbolic_link;
    return NodeKind::other;
}

// Appends one record; false when it is malformed.
bool append_record(const char* const record, const std::uint32_t length, SystemTree& tree) {
    Cursor cursor{record + sizeof(std::uint32_t), record + length, false};
    const char* const reference = cursor.position;
    const attrreference_t name = take<attrreference_t>(cursor);
    const std::uint32_t object_type = take<std::uint32_t>(cursor);
    const std::uint32_t bsd_flags = take<std::uint32_t>(cursor);
    const std::uint64_t id = take<std::uint64_t>(cursor);
    const std::uint64_t parent = take<std::uint64_t>(cursor);
    if (cursor.overrun) return false;
    const std::ptrdiff_t name_start = (reference - record) + name.attr_dataoffset;
    if (name.attr_length == 0 || name_start < 0 || name_start > static_cast<std::ptrdiff_t>(length) ||
        static_cast<std::ptrdiff_t>(name.attr_length) > static_cast<std::ptrdiff_t>(length) - name_start)
        return false;
    const char* const bytes = record + name_start;
    std::uint32_t name_length = name.attr_length;
    if (bytes[name_length - 1] == '\0') name_length = name_length - 1;

    SystemEntry entry{};
    entry.kind = kind_of(object_type);
    entry.bsd_flags = bsd_flags;
    entry.id = FileId{id};
    entry.parent = FileId{parent};
    if (entry.kind != NodeKind::directory) {
        entry.link_count = take<std::uint32_t>(cursor);
        entry.allocated_bytes = take<std::uint64_t>(cursor);
        entry.logical_bytes = take<std::uint64_t>(cursor);
        if (cursor.overrun) return false;
    }
    // searchfs on APFS reports symbolic links as regular files. A symbolic
    // link keeps its target inline: no allocation, a nonzero length. A
    // regular file looks like that only when compressed or dataless, and
    // both carry a flag. Checked against the walk's own kinds on the M4.
    const std::uint32_t different = UF_COMPRESSED | SF_DATALESS;
    if (entry.kind == NodeKind::file && entry.allocated_bytes == 0 && entry.logical_bytes > 0 &&
        (bsd_flags & different) == 0)
        entry.kind = NodeKind::symbolic_link;
    const std::size_t offset = tree.names.size();
    if (offset > std::numeric_limits<std::uint32_t>::max() - name_length) return false;
    tree.names.insert(tree.names.end(), bytes, bytes + name_length);
    entry.name.offset = static_cast<std::uint32_t>(offset);
    entry.name.length = name_length;
    tree.entries.push_back(entry);
    return true;
}

} // namespace

std::span<const SystemEntry> SystemTree::children_of(const FileId parent) const {
    SystemEntry probe{};
    probe.parent = parent;
    const std::vector<SystemEntry>::const_iterator first =
        std::lower_bound(entries.begin(), entries.end(), probe, ByParent{});
    const std::vector<SystemEntry>::const_iterator last = std::upper_bound(first, entries.end(), probe, ByParent{});
    const std::span<const SystemEntry> result(first, last);
    return result;
}

bool sealed_and_searchable(const std::string& root) {
    attrlist list{};
    list.bitmapcount = ATTR_BIT_MAP_COUNT;
    list.volattr = ATTR_VOL_INFO | ATTR_VOL_CAPABILITIES;
    VolumeCapabilities answer{};
    if (getattrlist(root.c_str(), &list, &answer, sizeof answer, 0) != 0) return false;
    const vol_capabilities_attr_t& capabilities = answer.capabilities;
    const std::uint32_t format = capabilities.capabilities[VOL_CAPABILITIES_FORMAT] &
                                 capabilities.valid[VOL_CAPABILITIES_FORMAT];
    const std::uint32_t interfaces = capabilities.capabilities[VOL_CAPABILITIES_INTERFACES] &
                                     capabilities.valid[VOL_CAPABILITIES_INTERFACES];
    const bool result = (format & VOL_CAP_FMT_SEALED) != 0 && (interfaces & VOL_CAP_INT_SEARCHFS) != 0;
    return result;
}

bool read_system_tree(const std::string& root, const gui_forms::CancellationFlag& cancellation, SystemTree& tree,
                      std::string& error) {
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    tree = SystemTree{};
    attrlist returned{};
    returned.bitmapcount = ATTR_BIT_MAP_COUNT;
    returned.commonattr = ATTR_CMN_NAME | ATTR_CMN_OBJTYPE | ATTR_CMN_FLAGS | ATTR_CMN_FILEID | ATTR_CMN_PARENTID;
    returned.fileattr = ATTR_FILE_LINKCOUNT | ATTR_FILE_ALLOCSIZE | ATTR_FILE_DATALENGTH;
    FileIdBound low{};
    FileIdBound high{};
    high.value = std::numeric_limits<std::uint64_t>::max();
    std::vector<char> buffer(search_buffer_bytes);
    fssearchblock block{};
    block.returnattrs = &returned;
    block.returnbuffer = buffer.data();
    block.returnbuffersize = buffer.size();
    block.maxmatches = 1u << 20;
    block.timelimit.tv_sec = 2;
    block.searchparams1 = &low;
    block.sizeofsearchparams1 = sizeof low;
    block.searchparams2 = &high;
    block.sizeofsearchparams2 = sizeof high;
    block.searchattrs.bitmapcount = ATTR_BIT_MAP_COUNT;
    block.searchattrs.commonattr = ATTR_CMN_FILEID;
    searchstate state{};
    unsigned int options = SRCHFS_START | SRCHFS_MATCHFILES | SRCHFS_MATCHDIRS;
    while (true) {
        if (cancellation.requested()) {
            error = "cancelled";
            tree = SystemTree{};
            return false;
        }
        unsigned long matches = 0;
        const int status = searchfs(root.c_str(), &block, &matches, utf8_script, options, &state);
        const int failure = errno;
        if (status != 0 && failure != EAGAIN) {
            error = std::string("searchfs: ") + std::strerror(failure);
            tree = SystemTree{};
            return false;
        }
        const char* record = buffer.data();
        const char* const buffer_end = buffer.data() + buffer.size();
        for (unsigned long i = 0; i < matches; ++i) {
            std::uint32_t length = 0;
            if (buffer_end - record < static_cast<std::ptrdiff_t>(sizeof length)) break;
            std::memcpy(&length, record, sizeof length);
            if (length < sizeof length || length > static_cast<std::size_t>(buffer_end - record) ||
                !append_record(record, length, tree)) {
                error = "searchfs returned a malformed record";
                tree = SystemTree{};
                return false;
            }
            record += length;
        }
        if (status == 0) break;
        options = options & ~static_cast<unsigned int>(SRCHFS_START);
    }
    // A search resumed after EAGAIN can return a hard-linked file's links
    // again (the M4, 2026-10-14: CodeResources, 5,795 links, twice in one
    // run in two). A directory cannot hold one name twice, so exact repeats
    // are dropped.
    std::sort(tree.entries.begin(), tree.entries.end(), ByParentIdName{&tree.names});
    const std::vector<SystemEntry>::iterator end =
        std::unique(tree.entries.begin(), tree.entries.end(), SameRecord{&tree.names});
    tree.entries.erase(end, tree.entries.end());
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;
    tree.seconds = elapsed.count();
    return true;
}

} // namespace cmac
