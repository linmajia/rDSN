#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

struct sandbox_profile
{
    std::string name = "read-only";
    bool allow_filesystem_read = true;
    bool allow_filesystem_write = false;
    bool allow_network = false;
    bool allow_process_spawn = false;
    uint64_t max_cpu_ms = 0;
    uint64_t max_memory_bytes = 0;
    std::vector<std::string> allowed_roots;
    std::vector<std::string> denied_paths;
    std::vector<std::string> allowed_network_hosts;
    std::vector<std::string> allowed_commands;
};

struct sandbox_request
{
    std::string operation;
    std::string path;
    std::string network_host;
    std::string command;
};

struct sandbox_decision
{
    bool allowed = false;
    std::string profile;
    std::string reason;
    uint64_t max_cpu_ms = 0;
    uint64_t max_memory_bytes = 0;
};

sandbox_profile default_read_only_sandbox_profile();
sandbox_profile default_workspace_write_sandbox_profile(const std::string &workspace_root);
sandbox_decision evaluate_sandbox_request(const sandbox_profile &profile, const sandbox_request &request);

} // namespace rasn
} // namespace dsn
