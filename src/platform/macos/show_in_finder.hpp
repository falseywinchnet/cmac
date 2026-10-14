#pragma once
// Reveals a file system object in Finder: the only outward action cmac has
// (AGENTS.md). Runs /usr/bin/open -R, which returns once Finder has the
// request.

#include <string>

namespace cmac {

// True when open(1) accepted the request.
[[nodiscard]] bool show_in_finder(const std::string& path);

} // namespace cmac
