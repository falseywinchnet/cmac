#pragma once
// The space in the APFS container that no scanned file accounts for: free
// space and virtual memory (swap files and the sleep image), shown in yellow
// beside the volume's files (owner, D2).

#include <cstdint>
#include <string>

namespace cmac {

struct ContainerSpace final {
    // Available to the volume (statfs f_bavail), shared by every volume in
    // the container.
    std::uint64_t free_bytes{0};
    // Used by the VM volume (/System/Volumes/VM), where the swap files and
    // the sleep image live. Zero when that volume is absent.
    std::uint64_t virtual_memory_bytes{0};
};

// Measures the space around the volume holding `root`. Fields that cannot
// be measured are zero.
[[nodiscard]] ContainerSpace measure_container_space(const std::string& root);

} // namespace cmac
