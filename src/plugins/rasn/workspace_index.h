#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

struct workspace_index_options
{
    uint64_t max_files = 256;
    uint64_t max_sampled_files = 24;
    uint64_t max_file_bytes = 4096;
    uint64_t max_total_bytes = 64ull * 1024ull;
};

struct workspace_file_entry
{
    std::string absolute_path;
    std::string relative_path;
    uint64_t size_bytes = 0;
    int priority = 0;
};

struct workspace_index_result
{
    std::string workspace_root;
    std::vector<workspace_file_entry> files;
    uint64_t matched_files = 0;
    uint64_t entries_seen = 0;
    bool truncated = false;
};

struct workspace_context_snapshot
{
    workspace_index_result index;
    std::string context_text;
    bool truncated = false;
};

bool is_sensitive_workspace_file(const std::string &path);

bool build_workspace_index(const std::string &workspace_root,
                           const workspace_index_options &options,
                           workspace_index_result *index,
                           std::string *error);

bool build_workspace_context_snapshot(const std::string &workspace_root,
                                      const workspace_index_options &options,
                                      workspace_context_snapshot *snapshot,
                                      std::string *error);

bool build_workspace_source_context(const std::string &workspace_root,
                                    const workspace_index_options &options,
                                    std::string *context_text,
                                    bool *truncated,
                                    std::string *error);

} // namespace rasn
} // namespace dsn
