// SPDX-FileCopyrightText: 2026 Per Edlund
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace swift_disk
{

struct Node
{
    uint64_t id = 0;
    uint64_t parent_id = 0;
    std::wstring name;
    bool is_dir = false;
    uint64_t size = 0;
    uint64_t path_hash = 0;
};

/**
 Stores and aggregates file system nodes by id.
 Maintains parent-child relationships, computes aggregate sizes,
 and provides path/stat/top-N queries.
 **/
class FileTree
{
  public:
    struct TreeStats
    {
        uint64_t file_count = 0;
        uint64_t dir_count = 0;
        uint64_t total_file_size = 0;
        uint64_t reachable_file_count = 0;
        uint64_t reachable_file_size = 0;
        uint64_t zero_size_files = 0;
        uint64_t unreachable_file_count = 0;
        uint64_t unreachable_file_size = 0;
    };

    uint64_t AddNode(Node node);
    const Node* GetNode(uint64_t id) const;
    Node* GetNodeMutable(uint64_t id);
    size_t NodeCount() const;
    void Reserve(size_t node_count);

    void SetRootId(uint64_t id);
    uint64_t RootId() const;

    void Finalize();
    void RefreshPathHashes();
    TreeStats ComputeStats() const;
    std::vector<const Node*> GetAllNodes() const;
    const std::vector<uint64_t>* GetChildIds(uint64_t id) const;
    std::vector<const Node*> TopN(size_t n) const;
    std::wstring BuildPath(uint64_t id) const;

  private:
    size_t GetDepth(uint64_t id, std::unordered_map<uint64_t, size_t>& cache) const;
    void RefreshPathHashesFromOrderedIds(const std::vector<uint64_t>& ids, bool deepest_first);

    uint64_t m_root_id = 0;
    std::unordered_map<uint64_t, Node> m_nodes;
    std::unordered_map<uint64_t, std::vector<uint64_t>> m_children;
};

} // namespace swift_disk
