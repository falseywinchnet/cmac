#include "scan/snapshot.hpp"

#include <vector>

namespace cmac {

std::string path_of(const Snapshot& snapshot, const NodeIndex node) {
    std::vector<std::string_view> names{};
    NodeIndex current = node;
    while (current != no_node && current != root_node) {
        const Node& entry = snapshot.nodes[current.value];
        names.push_back(snapshot.name_of(entry));
        current = entry.parent;
    }
    std::string result(snapshot.name_of(snapshot.nodes[root_node.value]));
    for (std::size_t i = names.size(); i > 0; --i) {
        if (result.empty() || result.back() != '/') result.push_back('/');
        result.append(names[i - 1]);
    }
    return result;
}

} // namespace cmac
