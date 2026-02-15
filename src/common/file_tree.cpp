#include "common/file_tree.h"

#include <algorithm>
#include <unordered_set>

namespace space_scanner {

uint64_t FileTree::AddNode(const Node& node) {
    nodes_[node.id] = node;
    if (node.parent_id != 0 && node.parent_id != node.id) {
        children_[node.parent_id].push_back(node.id);
    }
    return node.id;
}

const Node* FileTree::GetNode(uint64_t id) const {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) {
        return nullptr;
    }
    return &it->second;
}

Node* FileTree::GetNodeMutable(uint64_t id) {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) {
        return nullptr;
    }
    return &it->second;
}

size_t FileTree::NodeCount() const {
    return nodes_.size();
}

void FileTree::SetRootId(uint64_t id) {
    root_id_ = id;
}

uint64_t FileTree::RootId() const {
    return root_id_;
}

size_t FileTree::GetDepth(uint64_t id, std::unordered_map<uint64_t, size_t>& cache) const {
    auto cached = cache.find(id);
    if (cached != cache.end()) {
        return cached->second;
    }

    auto it = nodes_.find(id);
    if (it == nodes_.end()) {
        cache[id] = 0;
        return 0;
    }

    const Node& node = it->second;
    if (node.parent_id == 0 || node.parent_id == node.id || node.id == root_id_) {
        cache[id] = 0;
        return 0;
    }

    size_t depth = 1 + GetDepth(node.parent_id, cache);
    cache[id] = depth;
    return depth;
}

void FileTree::Finalize() {
    std::unordered_map<uint64_t, size_t> depth_cache;
    std::vector<uint64_t> ids;
    ids.reserve(nodes_.size());
    for (const auto& entry : nodes_) {
        ids.push_back(entry.first);
    }

    std::sort(ids.begin(), ids.end(), [&](uint64_t a, uint64_t b) {
        return GetDepth(a, depth_cache) > GetDepth(b, depth_cache);
    });

    for (uint64_t id : ids) {
        auto it = nodes_.find(id);
        if (it == nodes_.end()) {
            continue;
        }
        const Node& node = it->second;
        if (node.parent_id == 0 || node.parent_id == node.id) {
            continue;
        }
        auto parent_it = nodes_.find(node.parent_id);
        if (parent_it == nodes_.end()) {
            continue;
        }
        parent_it->second.size += node.size;
    }

    path_cache_.clear();
}

FileTree::TreeStats FileTree::ComputeStats() const {
    TreeStats stats;

    for (const auto& entry : nodes_) {
        const Node& node = entry.second;
        if (node.is_dir) {
            stats.dir_count++;
        } else {
            stats.file_count++;
            stats.total_file_size += node.size;
            if (node.size == 0) {
                stats.zero_size_files++;
            }
        }
    }

    if (root_id_ != 0 && nodes_.find(root_id_) != nodes_.end()) {
        std::vector<uint64_t> stack;
        stack.push_back(root_id_);
        std::unordered_set<uint64_t> visited;
        visited.reserve(nodes_.size());

        while (!stack.empty()) {
            uint64_t current = stack.back();
            stack.pop_back();
            if (!visited.insert(current).second) {
                continue;
            }
            auto it = nodes_.find(current);
            if (it == nodes_.end()) {
                continue;
            }
            const Node& node = it->second;
            if (!node.is_dir) {
                stats.reachable_file_count++;
                stats.reachable_file_size += node.size;
            } else {
                auto child_it = children_.find(current);
                if (child_it != children_.end()) {
                    for (uint64_t child : child_it->second) {
                        if (visited.find(child) == visited.end()) {
                            stack.push_back(child);
                        }
                    }
                }
            }
        }
    }

    if (stats.reachable_file_count <= stats.file_count) {
        stats.unreachable_file_count = stats.file_count - stats.reachable_file_count;
    }
    if (stats.reachable_file_size <= stats.total_file_size) {
        stats.unreachable_file_size = stats.total_file_size - stats.reachable_file_size;
    }

    return stats;
}

std::vector<const Node*> FileTree::TopN(size_t n) const {
    std::vector<const Node*> nodes;
    nodes.reserve(nodes_.size());
    for (const auto& entry : nodes_) {
        nodes.push_back(&entry.second);
    }

    std::sort(nodes.begin(), nodes.end(), [](const Node* a, const Node* b) {
        return a->size > b->size;
    });

    if (nodes.size() > n) {
        nodes.resize(n);
    }
    return nodes;
}

std::wstring FileTree::BuildPath(uint64_t id) const {
    auto cached = path_cache_.find(id);
    if (cached != path_cache_.end()) {
        return cached->second;
    }

    std::vector<std::wstring> parts;
    uint64_t current = id;
    size_t guard = 0;
    while (current != 0 && guard < 1024) {
        auto it = nodes_.find(current);
        if (it == nodes_.end()) {
            break;
        }
        const Node& node = it->second;
        if (!node.name.empty()) {
            parts.push_back(node.name);
        }
        if (node.parent_id == 0 || node.parent_id == node.id) {
            break;
        }
        current = node.parent_id;
        if (current == root_id_) {
            auto root_it = nodes_.find(current);
            if (root_it != nodes_.end() && !root_it->second.name.empty()) {
                parts.push_back(root_it->second.name);
            }
            break;
        }
        ++guard;
    }

    std::wstring path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!path.empty() && path.back() != L'\\') {
            path.append(L"\\");
        }
        path.append(*it);
    }

    path_cache_[id] = path;
    return path;
}

} // namespace space_scanner
