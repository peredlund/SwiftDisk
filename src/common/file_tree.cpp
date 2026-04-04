#include "common/file_tree.h"

#include <algorithm>
#include <unordered_set>

namespace space_scanner
{

/**
 Inserts or replaces a node and updates parent-child indices.
 
 Parameters:
   node: Node to insert.
 
 Returns: Inserted node id.
 **/
uint64_t FileTree::AddNode(Node node)
{
    const uint64_t node_id = node.id;
    const uint64_t parent_id = node.parent_id;
    m_nodes.insert_or_assign(node_id, std::move(node));
    if (parent_id != 0 && parent_id != node_id)
    {
        m_children[parent_id].push_back(node_id);
    }
    return node_id;
}

const Node* FileTree::GetNode(uint64_t id) const
{
    auto it = m_nodes.find(id);
    if (it == m_nodes.end())
    {
        return nullptr;
    }
    return &it->second;
}

Node* FileTree::GetNodeMutable(uint64_t id)
{
    auto it = m_nodes.find(id);
    if (it == m_nodes.end())
    {
        return nullptr;
    }
    return &it->second;
}

size_t FileTree::NodeCount() const
{
    return m_nodes.size();
}

void FileTree::Reserve(size_t node_count)
{
    m_nodes.reserve(node_count);
    m_children.reserve(node_count);
    m_path_cache.reserve(node_count);
}

void FileTree::SetRootId(uint64_t id)
{
    m_root_id = id;
}

uint64_t FileTree::RootId() const
{
    return m_root_id;
}

/**
 Computes node depth relative to the configured root.
 Uses memoization to avoid repeated parent-chain traversals during
 depth-based sorting and aggregation.
 
 Parameters:
   id: Node id to evaluate.
   cache: Depth memoization map.
 
 Returns: Depth value, where root-like nodes are depth 0.
 **/
size_t FileTree::GetDepth(uint64_t id, std::unordered_map<uint64_t, size_t>& cache) const
{
    auto cached = cache.find(id);
    if (cached != cache.end())
    {
        return cached->second;
    }

    auto it = m_nodes.find(id);
    if (it == m_nodes.end())
    {
        cache[id] = 0;
        return 0;
    }

    const Node& node = it->second;
    if (node.parent_id == 0 || node.parent_id == node.id || node.id == m_root_id)
    {
        cache[id] = 0;
        return 0;
    }

    size_t depth = 1 + GetDepth(node.parent_id, cache);
    cache[id] = depth;
    return depth;
}

/**
 Finalizes the tree by aggregating child sizes into parents.
 Nodes are processed deepest-first so each parent receives already
 aggregated child totals.
 **/
void FileTree::Finalize()
{
    std::unordered_map<uint64_t, size_t> depth_cache;
    std::vector<uint64_t> ids;
    ids.reserve(m_nodes.size());
    for (const auto& entry : m_nodes)
    {
        ids.push_back(entry.first);
    }

    std::sort(ids.begin(), ids.end(), [&](uint64_t a, uint64_t b)
              { return GetDepth(a, depth_cache) > GetDepth(b, depth_cache); });

    for (uint64_t id : ids)
    {
        auto it = m_nodes.find(id);
        if (it == m_nodes.end())
        {
            continue;
        }
        const Node& node = it->second;
        if (node.parent_id == 0 || node.parent_id == node.id)
        {
            continue;
        }
        auto parent_it = m_nodes.find(node.parent_id);
        if (parent_it == m_nodes.end())
        {
            continue;
        }
        parent_it->second.size += node.size;
    }

    m_path_cache.clear();
}

/**
 Computes global and root-reachable statistics for the current tree.
 
 Returns: Filled TreeStats structure.
 **/
FileTree::TreeStats FileTree::ComputeStats() const
{
    TreeStats stats;

    for (const auto& entry : m_nodes)
    {
        const Node& node = entry.second;
        if (node.is_dir)
        {
            stats.dir_count++;
        }
        else
        {
            stats.file_count++;
            stats.total_file_size += node.size;
            if (node.size == 0)
            {
                stats.zero_size_files++;
            }
        }
    }

    if (m_root_id != 0 && m_nodes.find(m_root_id) != m_nodes.end())
    {
        std::vector<uint64_t> stack;
        stack.push_back(m_root_id);
        std::unordered_set<uint64_t> visited;
        visited.reserve(m_nodes.size());

        while (!stack.empty())
        {
            uint64_t current = stack.back();
            stack.pop_back();
            if (!visited.insert(current).second)
            {
                continue;
            }
            auto it = m_nodes.find(current);
            if (it == m_nodes.end())
            {
                continue;
            }
            const Node& node = it->second;
            if (!node.is_dir)
            {
                stats.reachable_file_count++;
                stats.reachable_file_size += node.size;
            }
            else
            {
                auto child_it = m_children.find(current);
                if (child_it != m_children.end())
                {
                    for (uint64_t child : child_it->second)
                    {
                        if (visited.find(child) == visited.end())
                        {
                            stack.push_back(child);
                        }
                    }
                }
            }
        }
    }

    if (stats.reachable_file_count <= stats.file_count)
    {
        stats.unreachable_file_count = stats.file_count - stats.reachable_file_count;
    }
    if (stats.reachable_file_size <= stats.total_file_size)
    {
        stats.unreachable_file_size = stats.total_file_size - stats.reachable_file_size;
    }

    return stats;
}

std::vector<const Node*> FileTree::GetAllNodes() const
{
    std::vector<const Node*> nodes;
    nodes.reserve(m_nodes.size());
    for (const auto& entry : m_nodes)
    {
        nodes.push_back(&entry.second);
    }
    return nodes;
}

const std::vector<uint64_t>* FileTree::GetChildIds(uint64_t id) const
{
    auto it = m_children.find(id);
    if (it == m_children.end())
    {
        return nullptr;
    }
    return &it->second;
}

/**
Returns the largest nodes by current size value.
 
Parameters:
   n: Maximum number of nodes to return.
 
 Returns: Nodes sorted by descending size.
 **/
std::vector<const Node*> FileTree::TopN(size_t n) const
{
    std::vector<const Node*> nodes;
    nodes.reserve(m_nodes.size());
    for (const auto& entry : m_nodes)
    {
        nodes.push_back(&entry.second);
    }

    std::sort(nodes.begin(), nodes.end(), [](const Node* a, const Node* b)
              { return a->size > b->size; });

    if (nodes.size() > n)
    {
        nodes.resize(n);
    }
    return nodes;
}

/**
 Builds and caches a path by walking parent links to the root.
 
 Parameters:
   id: Node id to render.
 
 Returns: Path string for the node, if ancestry can be resolved.
 **/
std::wstring FileTree::BuildPath(uint64_t id) const
{
    auto cached = m_path_cache.find(id);
    if (cached != m_path_cache.end())
    {
        return cached->second;
    }

    std::vector<std::wstring> parts;
    uint64_t current = id;
    size_t guard = 0;
    while (current != 0 && guard < 1024)
    {
        auto it = m_nodes.find(current);
        if (it == m_nodes.end())
        {
            break;
        }
        const Node& node = it->second;
        if (!node.name.empty())
        {
            parts.push_back(node.name);
        }
        if (node.parent_id == 0 || node.parent_id == node.id)
        {
            break;
        }
        current = node.parent_id;
        if (current == m_root_id)
        {
            auto root_it = m_nodes.find(current);
            if (root_it != m_nodes.end() && !root_it->second.name.empty())
            {
                parts.push_back(root_it->second.name);
            }
            break;
        }
        ++guard;
    }

    std::wstring path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it)
    {
        if (!path.empty() && path.back() != L'\\')
        {
            path.append(L"\\");
        }
        path.append(*it);
    }

    m_path_cache[id] = path;
    return path;
}

} // namespace space_scanner
