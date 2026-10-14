#pragma once
// The sealed System volume, read in one catalog search (searchfs) instead
// of directory by directory. On the M4 the search took 3.2 s and 1.2 s of
// kernel CPU against 3.9 s and 23.7 s for the walk, and it runs beside the
// Data volume's walk: a first scan of "/" fell from 9.7 s to 5.7 s
// (SCANNER_STUDY.md §6c). A sealed volume cannot change during the search,
// so searchfs never has to restart (EBUSY), which ruled it out for Data.

#include "scan/snapshot.hpp"

#include "gui_forms/threading.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace cmac {

// One object of the System volume as searchfs returned it.
struct SystemEntry final {
    FileId parent{};
    FileId id{};
    NameRef name{};
    std::uint64_t allocated_bytes{0};
    std::uint64_t logical_bytes{0};
    std::uint32_t link_count{1};
    // BSD flags (SF_FIRMLINK marks a stub whose target is on the Data
    // volume; UF_COMPRESSED; SF_DATALESS).
    std::uint32_t bsd_flags{0};
    NodeKind kind{NodeKind::other};
};

struct SystemTree final {
    // Sorted by parent id: the children of one directory are contiguous.
    std::vector<SystemEntry> entries{};
    std::vector<char> names{};
    double seconds{0.0};

    [[nodiscard]] std::span<const SystemEntry> children_of(FileId parent) const;
    [[nodiscard]] std::string_view name_of(const SystemEntry& entry) const {
        const std::string_view result(names.data() + entry.name.offset, entry.name.length);
        return result;
    }
};

// True when the volume holding `root` is sealed and supports searchfs: the
// boot System volume.
[[nodiscard]] bool sealed_and_searchable(const std::string& root);

// Reads every object of the volume holding `root` with searchfs. False with a
// reason on failure (tree is then empty); the caller reads the directories
// itself instead. Stops early, returning false, once cancellation is
// requested (checked between searchfs calls).
[[nodiscard]] bool read_system_tree(const std::string& root, const gui_forms::CancellationFlag& cancellation,
                                    SystemTree& tree, std::string& error);

} // namespace cmac
