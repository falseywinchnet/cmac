#include "space/container_space.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cmac {
namespace {

// The VM volume of the boot volume group, holding the swap files and the
// sleep image.
inline constexpr const char* virtual_memory_volume = "/System/Volumes/VM";

// The allocation of the regular files directly in the VM volume. statfs
// cannot give a volume's own use on APFS: its f_bfree is the container's.
std::uint64_t virtual_memory_allocation() {
    std::uint64_t result = 0;
    DIR* const directory = ::opendir(virtual_memory_volume);
    if (directory == nullptr) return result;
    const int descriptor = ::dirfd(directory);
    while (true) {
        const dirent* const entry = ::readdir(directory);
        if (entry == nullptr) break;
        struct stat status{};
        if (::fstatat(descriptor, (*entry).d_name, &status, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (!S_ISREG(status.st_mode)) continue;
        // st_blocks counts 512-byte units.
        result += static_cast<std::uint64_t>(status.st_blocks) * 512u;
    }
    ::closedir(directory);
    return result;
}

} // namespace

ContainerSpace measure_container_space(const std::string& root) {
    ContainerSpace result{};
    struct statfs volume{};
    if (statfs(root.c_str(), &volume) == 0) {
        result.free_bytes = static_cast<std::uint64_t>(volume.f_bavail) * volume.f_bsize;
    }
    result.virtual_memory_bytes = virtual_memory_allocation();
    return result;
}

} // namespace cmac
