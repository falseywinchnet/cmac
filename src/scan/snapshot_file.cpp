#include "scan/snapshot_file.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <bit>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

namespace cmac {
namespace {

static_assert(std::endian::native == std::endian::little, "the snapshot file is written in host order");

inline constexpr std::array<char, 8> magic{'C', 'M', 'A', 'C', 'S', 'N', 'A', 'P'};
inline constexpr std::uint32_t format_version = 1;
inline constexpr std::uint32_t record_bytes = 56;
// Records are encoded into this many at a time before each write.
inline constexpr std::size_t records_per_chunk = 4096;

// Appends a value's bytes to a byte buffer.
template <typename Value>
void append_value(std::vector<unsigned char>& bytes, const Value value) {
    static_assert(std::is_trivially_copyable_v<Value>);
    unsigned char raw[sizeof(Value)] = {};
    std::memcpy(raw, &value, sizeof(Value));
    bytes.insert(bytes.end(), raw, raw + sizeof(Value));
}

void append_text(std::vector<unsigned char>& bytes, const std::string& text) {
    append_value<std::uint32_t>(bytes, static_cast<std::uint32_t>(text.size()));
    bytes.insert(bytes.end(), text.begin(), text.end());
}

void encode_record(const Node& node, unsigned char* const out) {
    unsigned char* at = out;
    const std::uint32_t name_offset = node.name.offset;
    const std::uint32_t name_length = node.name.length;
    const std::uint32_t parent = node.parent.value;
    const std::uint32_t first_child = node.first_child.value;
    const std::uint8_t kind = static_cast<std::uint8_t>(node.kind);
    const std::uint8_t state = static_cast<std::uint8_t>(node.directory_state);
    std::memcpy(at, &name_offset, 4);
    at += 4;
    std::memcpy(at, &name_length, 4);
    at += 4;
    std::memcpy(at, &parent, 4);
    at += 4;
    std::memcpy(at, &first_child, 4);
    at += 4;
    std::memcpy(at, &node.child_count, 4);
    at += 4;
    std::memcpy(at, &node.descendant_count, 4);
    at += 4;
    std::memcpy(at, &node.allocated_bytes, 8);
    at += 8;
    std::memcpy(at, &node.logical_bytes, 8);
    at += 8;
    std::memcpy(at, &node.file_id.value, 8);
    at += 8;
    std::memcpy(at, &node.link_count, 4);
    at += 4;
    std::memcpy(at, &node.flags, 2);
    at += 2;
    std::memcpy(at, &kind, 1);
    at += 1;
    std::memcpy(at, &state, 1);
}

// Reads bytes from a validated buffer. `offset` advances; reading past the
// end sets `overrun` and returns zeros.
struct ByteReader final {
    const std::vector<unsigned char>& bytes;
    std::size_t offset{0};
    bool overrun{false};
};

template <typename Value>
Value take_value(ByteReader& reader) {
    Value value{};
    if (reader.offset > reader.bytes.size() || reader.bytes.size() - reader.offset < sizeof(Value)) {
        reader.overrun = true;
        return value;
    }
    std::memcpy(&value, reader.bytes.data() + reader.offset, sizeof(Value));
    reader.offset += sizeof(Value);
    return value;
}

std::string take_text(ByteReader& reader) {
    const std::uint32_t length = take_value<std::uint32_t>(reader);
    std::string result{};
    if (reader.overrun || reader.bytes.size() - reader.offset < length) {
        reader.overrun = true;
        return result;
    }
    const char* const start = reinterpret_cast<const char*>(reader.bytes.data() + reader.offset);
    result.assign(start, length);
    reader.offset += length;
    return result;
}

Node decode_record(const unsigned char* const in) {
    Node node{};
    const unsigned char* at = in;
    std::uint32_t parent = 0;
    std::uint32_t first_child = 0;
    std::uint8_t kind = 0;
    std::uint8_t state = 0;
    std::memcpy(&node.name.offset, at, 4);
    at += 4;
    std::memcpy(&node.name.length, at, 4);
    at += 4;
    std::memcpy(&parent, at, 4);
    at += 4;
    std::memcpy(&first_child, at, 4);
    at += 4;
    std::memcpy(&node.child_count, at, 4);
    at += 4;
    std::memcpy(&node.descendant_count, at, 4);
    at += 4;
    std::memcpy(&node.allocated_bytes, at, 8);
    at += 8;
    std::memcpy(&node.logical_bytes, at, 8);
    at += 8;
    std::memcpy(&node.file_id.value, at, 8);
    at += 8;
    std::memcpy(&node.link_count, at, 4);
    at += 4;
    std::memcpy(&node.flags, at, 2);
    at += 2;
    std::memcpy(&kind, at, 1);
    at += 1;
    std::memcpy(&state, at, 1);
    node.parent = NodeIndex{parent};
    node.first_child = NodeIndex{first_child};
    node.kind = static_cast<NodeKind>(kind);
    node.directory_state = static_cast<DirectoryState>(state);
    return node;
}

// Every link of a decoded snapshot points where the builder would have put
// it: names inside the pool, the root first, parents before children,
// children contiguous and pointing back at their parent.
bool validate(const Snapshot& snapshot, std::string& error) {
    const std::size_t count = snapshot.nodes.size();
    const std::size_t name_bytes = snapshot.names.size();
    for (std::size_t n = 0; n < count; ++n) {
        const Node& node = snapshot.nodes[n];
        if (node.name.offset > name_bytes || node.name.length > name_bytes - node.name.offset) {
            error = "a name lies outside the name pool";
            return false;
        }
        if (static_cast<std::uint8_t>(node.kind) > static_cast<std::uint8_t>(NodeKind::other) ||
            static_cast<std::uint8_t>(node.directory_state) > static_cast<std::uint8_t>(DirectoryState::failed)) {
            error = "a record has an unknown kind or state";
            return false;
        }
        const bool root = n == 0;
        if (root && node.parent != no_node) {
            error = "the root has a parent";
            return false;
        }
        if (!root && node.parent.value >= n) {
            error = "a parent does not precede its child";
            return false;
        }
        if (node.child_count == 0) continue;
        const std::uint64_t first = node.first_child.value;
        const std::uint64_t end = first + node.child_count;
        if (first <= n || end > count) {
            error = "a child range lies outside the records";
            return false;
        }
        for (std::uint64_t c = first; c < end; ++c) {
            if (snapshot.nodes[c].parent.value != n) {
                error = "a child does not point back at its parent";
                return false;
            }
        }
    }
    return true;
}

std::uint64_t fnv1a(const std::string& text) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const char c : text) {
        hash = hash ^ static_cast<unsigned char>(c);
        hash = hash * 1099511628211ull;
    }
    return hash;
}

} // namespace

bool write_snapshot_file(const std::string& path, const Snapshot& snapshot, const SnapshotInfo& info,
                         std::string& error) {
    if (snapshot.nodes.empty()) {
        error = "empty snapshot";
        return false;
    }
    std::vector<unsigned char> header{};
    header.insert(header.end(), magic.begin(), magic.end());
    append_value<std::uint32_t>(header, format_version);
    append_value<std::uint32_t>(header, record_bytes);
    append_value<std::uint64_t>(header, snapshot.nodes.size());
    append_value<std::uint64_t>(header, snapshot.names.size());
    append_value<std::uint64_t>(header, info.journal_event_id);
    append_value<std::int64_t>(header, info.written_unix_seconds);
    append_text(header, info.root);
    append_text(header, info.journal_store);
    append_text(header, info.system_build);

    const std::string temporary = path + ".writing";
    std::FILE* const file = std::fopen(temporary.c_str(), "wb");
    if (file == nullptr) {
        error = "cannot create " + temporary + ": " + std::strerror(errno);
        return false;
    }
    bool written = std::fwrite(header.data(), 1, header.size(), file) == header.size();
    std::vector<unsigned char> chunk(records_per_chunk * record_bytes);
    std::size_t n = 0;
    while (written && n < snapshot.nodes.size()) {
        const std::size_t remaining = snapshot.nodes.size() - n;
        const std::size_t batch = remaining < records_per_chunk ? remaining : records_per_chunk;
        for (std::size_t i = 0; i < batch; ++i) encode_record(snapshot.nodes[n + i], chunk.data() + i * record_bytes);
        const std::size_t bytes = batch * record_bytes;
        written = std::fwrite(chunk.data(), 1, bytes, file) == bytes;
        n += batch;
    }
    if (written && !snapshot.names.empty())
        written = std::fwrite(snapshot.names.data(), 1, snapshot.names.size(), file) == snapshot.names.size();
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed) {
        error = "cannot write " + temporary;
        ::unlink(temporary.c_str());
        return false;
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        error = "cannot replace " + path + ": " + std::strerror(errno);
        ::unlink(temporary.c_str());
        return false;
    }
    return true;
}

bool read_snapshot_file(const std::string& path, LoadedSnapshot& loaded, std::string& error) {
    loaded = LoadedSnapshot{};
    std::FILE* const file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        error = "cannot open " + path + ": " + std::strerror(errno);
        return false;
    }
    struct stat status{};
    if (::fstat(::fileno(file), &status) != 0 || status.st_size < 0) {
        std::fclose(file);
        error = "cannot size " + path;
        return false;
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(status.st_size));
    const bool read = std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
    std::fclose(file);
    if (!read) {
        error = "cannot read " + path;
        return false;
    }

    ByteReader reader{bytes};
    std::array<char, 8> found{};
    for (char& c : found) c = static_cast<char>(take_value<unsigned char>(reader));
    const std::uint32_t version = take_value<std::uint32_t>(reader);
    const std::uint32_t size = take_value<std::uint32_t>(reader);
    const std::uint64_t node_count = take_value<std::uint64_t>(reader);
    const std::uint64_t name_bytes = take_value<std::uint64_t>(reader);
    SnapshotInfo info{};
    info.journal_event_id = take_value<std::uint64_t>(reader);
    info.written_unix_seconds = take_value<std::int64_t>(reader);
    info.root = take_text(reader);
    info.journal_store = take_text(reader);
    info.system_build = take_text(reader);
    if (reader.overrun || found != magic || version != format_version || size != record_bytes) {
        error = path + " is not a version 1 cmac snapshot";
        return false;
    }
    const std::uint64_t remaining = bytes.size() - reader.offset;
    if (node_count == 0 || node_count >= UINT32_MAX || name_bytes > std::numeric_limits<std::uint32_t>::max() ||
        node_count > remaining / record_bytes || remaining - node_count * record_bytes != name_bytes) {
        error = path + " has inconsistent sizes";
        return false;
    }

    Snapshot snapshot{};
    snapshot.nodes.resize(static_cast<std::size_t>(node_count));
    const unsigned char* const records = bytes.data() + reader.offset;
    for (std::size_t n = 0; n < snapshot.nodes.size(); ++n) snapshot.nodes[n] = decode_record(records + n * record_bytes);
    const unsigned char* const names = records + node_count * record_bytes;
    snapshot.names.assign(reinterpret_cast<const char*>(names), reinterpret_cast<const char*>(names) + name_bytes);
    if (!validate(snapshot, error)) {
        error = path + ": " + error;
        return false;
    }
    if (snapshot.name_of(snapshot.nodes[root_node.value]) != info.root) {
        error = path + ": the root's name is not the recorded root";
        return false;
    }
    loaded.snapshot = std::move(snapshot);
    loaded.info = std::move(info);
    return true;
}

std::string default_snapshot_path(const std::string& root) {
    const char* const home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') return std::string{};
    const std::string caches = std::string(home) + "/Library/Caches";
    const std::string folder = caches + "/com.falseywinchnet.cmac";
    ::mkdir(caches.c_str(), 0700);
    ::mkdir(folder.c_str(), 0700);
    char name[64] = {};
    std::snprintf(name, sizeof name, "/snapshot-%016llx.cmacsnap", static_cast<unsigned long long>(fnv1a(root)));
    const std::string result = folder + name;
    return result;
}

} // namespace cmac
