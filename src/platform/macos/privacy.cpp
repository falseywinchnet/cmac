#include "platform/macos/privacy.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <string_view>
#include <vector>

extern char** environ;

namespace cmac {
namespace {

// Readable only with Full Disk Access; without it open fails with EPERM and
// macOS shows nothing.
inline constexpr const char* access_probe = "/Library/Application Support/com.apple.TCC/TCC.db";

// Folders below a home that macOS guards with a consent prompt (or that
// only Full Disk Access opens), relative to the home.
inline constexpr std::array<std::string_view, 17> protected_folders{
    "Desktop",
    "Documents",
    "Downloads",
    "Library/Mobile Documents",
    "Library/CloudStorage",
    "Library/Containers",
    "Library/Group Containers",
    "Library/Mail",
    "Library/Messages",
    "Library/Safari",
    "Library/Calendars",
    "Library/Application Support/AddressBook",
    "Library/Application Support/CallHistoryDB",
    "Library/Application Support/com.apple.TCC",
    "Music/Music",
    "Pictures/Photos Library.photoslibrary",
    "Movies/TV",
};

struct ByFileId final {
    bool operator()(const FileId& left, const FileId& right) const { return left < right; }
};

bool has_full_disk_access() {
    const int descriptor = ::open(access_probe, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) return false;
    ::close(descriptor);
    return true;
}

} // namespace

PrivacyAccess assess_privacy_access() {
    PrivacyAccess result{};
    result.full_disk_access = has_full_disk_access();
    if (result.full_disk_access) return result;
    DIR* const users = ::opendir("/Users");
    if (users == nullptr) return result;
    while (true) {
        const dirent* const entry = ::readdir(users);
        if (entry == nullptr) break;
        const std::string_view name((*entry).d_name);
        if (name == "." || name == ".." || name == "Shared") continue;
        const std::string home = "/Users/" + std::string(name);
        for (const std::string_view folder : protected_folders) {
            const std::string path = home + "/" + std::string(folder);
            struct stat status{};
            if (::lstat(path.c_str(), &status) != 0 || !S_ISDIR(status.st_mode)) continue;
            result.withheld.push_back(FileId{static_cast<std::uint64_t>(status.st_ino)});
            result.withheld_paths.push_back(path);
        }
    }
    ::closedir(users);
    std::sort(result.withheld.begin(), result.withheld.end(), ByFileId{});
    return result;
}

bool open_full_disk_access_settings() {
    std::string program = "/usr/bin/open";
    std::string target = "x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles";
    std::vector<char*> arguments{program.data(), target.data(), nullptr};
    pid_t child = 0;
    if (posix_spawn(&child, program.c_str(), nullptr, nullptr, arguments.data(), environ) != 0) return false;
    int status = 0;
    pid_t waited = -1;
    while (true) {
        waited = waitpid(child, &status, 0);
        if (waited >= 0 || errno != EINTR) break;
    }
    const bool result = waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return result;
}

} // namespace cmac
