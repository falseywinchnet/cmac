#include "platform/macos/show_in_finder.hpp"

#include <spawn.h>
#include <sys/wait.h>

#include <cerrno>
#include <vector>

extern char** environ;

namespace cmac {

bool show_in_finder(const std::string& path) {
    // argv for open -R <path>; the strings outlive the spawn.
    std::string program = "/usr/bin/open";
    std::string reveal = "-R";
    std::string target = path;
    std::vector<char*> arguments{program.data(), reveal.data(), target.data(), nullptr};
    pid_t child = 0;
    const int spawned = posix_spawn(&child, program.c_str(), nullptr, nullptr, arguments.data(), environ);
    if (spawned != 0) return false;
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
