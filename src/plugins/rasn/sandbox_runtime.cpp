#include <rasn/sandbox_runtime.h>

#include <rasn/rasn_core.h>

#include <dsn/cpp/utils.h>

#include <algorithm>
#include <cctype>

namespace dsn {
namespace rasn {

namespace {

std::string normalized_absolute_path(const std::string &path)
{
    std::string normalized = normalize_platform_path(path.empty() ? "." : path);
    std::string absolute;
    if (::dsn::utils::filesystem::get_absolute_path(normalized, absolute))
    {
        normalized = absolute;
    }
    std::string canonical;
    if (::dsn::utils::filesystem::get_normalized_path(normalized, canonical) == 0 && !canonical.empty())
    {
        normalized = canonical;
    }
    normalized = normalize_platform_path(normalized);
#if defined(_WIN32)
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
#endif
    return normalized;
}

bool path_has_prefix_boundary(const std::string &path, const std::string &prefix)
{
    if (path == prefix)
    {
        return true;
    }
    if (path.size() <= prefix.size() || path.find(prefix) != 0)
    {
        return false;
    }
    const char boundary = path[prefix.size()];
    return boundary == '\\' || boundary == '/';
}

bool list_contains(const std::vector<std::string> &values, const std::string &value)
{
    for (const std::string &candidate : values)
    {
        if (candidate == value)
        {
            return true;
        }
    }
    return false;
}

bool path_allowed_by_roots(const sandbox_profile &profile, const std::string &path)
{
    if (profile.allowed_roots.empty())
    {
        return true;
    }
    const std::string normalized = normalized_absolute_path(path);
    for (const std::string &root : profile.allowed_roots)
    {
        if (path_has_prefix_boundary(normalized, normalized_absolute_path(root)))
        {
            return true;
        }
    }
    return false;
}

bool path_denied(const sandbox_profile &profile, const std::string &path)
{
    const std::string normalized = normalized_absolute_path(path);
    for (const std::string &denied : profile.denied_paths)
    {
        if (path_has_prefix_boundary(normalized, normalized_absolute_path(denied)))
        {
            return true;
        }
    }
    return false;
}

sandbox_decision denied_decision(const sandbox_profile &profile, const std::string &reason)
{
    sandbox_decision decision;
    decision.allowed = false;
    decision.profile = profile.name;
    decision.reason = reason;
    decision.max_cpu_ms = profile.max_cpu_ms;
    decision.max_memory_bytes = profile.max_memory_bytes;
    return decision;
}

sandbox_decision allowed_decision(const sandbox_profile &profile)
{
    sandbox_decision decision;
    decision.allowed = true;
    decision.profile = profile.name;
    decision.max_cpu_ms = profile.max_cpu_ms;
    decision.max_memory_bytes = profile.max_memory_bytes;
    return decision;
}

} // namespace

sandbox_profile default_read_only_sandbox_profile()
{
    sandbox_profile profile;
    profile.name = "read-only";
    profile.allow_filesystem_read = true;
    profile.allow_filesystem_write = false;
    profile.allow_network = false;
    profile.allow_process_spawn = false;
    return profile;
}

sandbox_profile default_workspace_write_sandbox_profile(const std::string &workspace_root)
{
    sandbox_profile profile;
    profile.name = "workspace-write";
    profile.allow_filesystem_read = true;
    profile.allow_filesystem_write = true;
    profile.allow_network = false;
    profile.allow_process_spawn = false;
    if (!workspace_root.empty())
    {
        profile.allowed_roots.push_back(workspace_root);
    }
    return profile;
}

sandbox_decision evaluate_sandbox_request(const sandbox_profile &profile, const sandbox_request &request)
{
    if ((request.operation == "fs.read" || request.operation == "fs.write") && request.path.empty())
    {
        return denied_decision(profile, "filesystem sandbox request missing path");
    }
    if (request.operation == "fs.read")
    {
        if (!profile.allow_filesystem_read)
        {
            return denied_decision(profile, "filesystem reads are denied");
        }
        if (path_denied(profile, request.path) || !path_allowed_by_roots(profile, request.path))
        {
            return denied_decision(profile, "path is outside sandbox roots");
        }
        return allowed_decision(profile);
    }
    if (request.operation == "fs.write")
    {
        if (!profile.allow_filesystem_write)
        {
            return denied_decision(profile, "filesystem writes are denied");
        }
        if (path_denied(profile, request.path) || !path_allowed_by_roots(profile, request.path))
        {
            return denied_decision(profile, "path is outside sandbox roots");
        }
        return allowed_decision(profile);
    }
    if (request.operation == "network")
    {
        if (!profile.allow_network)
        {
            return denied_decision(profile, "network access is denied");
        }
        if (!profile.allowed_network_hosts.empty() && !list_contains(profile.allowed_network_hosts, request.network_host))
        {
            return denied_decision(profile, "network host is not allowed: " + request.network_host);
        }
        return allowed_decision(profile);
    }
    if (request.operation == "process")
    {
        if (!profile.allow_process_spawn)
        {
            return denied_decision(profile, "process spawning is denied");
        }
        if (!profile.allowed_commands.empty() && !list_contains(profile.allowed_commands, request.command))
        {
            return denied_decision(profile, "command is not allowed: " + request.command);
        }
        return allowed_decision(profile);
    }
    return denied_decision(profile, "unknown sandbox operation: " + request.operation);
}

} // namespace rasn
} // namespace dsn
