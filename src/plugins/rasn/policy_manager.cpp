#include <rasn/policy_manager.h>

#include <rasn/redaction.h>
#include <rasn/state_service.h>

#include <dsn/cpp/zlocks.h>
#include <dsn/cpp/utils.h>
#include <dsn/service_api_cpp.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>

#if defined(_WIN32)
#include <process.h>
#else
#include <climits>
#include <unistd.h>
#endif

namespace dsn {
namespace rasn {

namespace {

tool_result make_tool_result(bool ok, const std::string &output, const std::string &error)
{
    tool_result result;
    result.ok = ok;
    result.output = output;
    result.error = error;
    return result;
}

bool config_bool_compat(const std::string &key, bool fallback)
{
    const bool compat_value =
        ::dsn_config_get_value_bool("rasn.codepilot.tools", key.c_str(), fallback, "CodePilot compatibility policy setting");
    return ::dsn_config_get_value_bool("rasn.policy", key.c_str(), compat_value, "rASN policy setting");
}

size_t config_size_compat(const std::string &key, size_t fallback)
{
    const uint64_t compat_value =
        ::dsn_config_get_value_uint64("rasn.codepilot.tools", key.c_str(), fallback, "CodePilot compatibility policy size setting");
    const uint64_t value =
        ::dsn_config_get_value_uint64("rasn.policy", key.c_str(), compat_value, "rASN policy size setting");
    const uint64_t max_size = static_cast<uint64_t>((std::numeric_limits<size_t>::max)());
    if (value > max_size)
    {
        dwarn("rasn.policy.%s=%llu exceeds size_t range; using size_t max",
              key.c_str(),
              static_cast<unsigned long long>(value));
        return (std::numeric_limits<size_t>::max)();
    }
    return static_cast<size_t>(value);
}

std::string config_string_compat(const std::string &key, const std::string &fallback)
{
    const char *compat_value = ::dsn_config_get_value_string(
        "rasn.codepilot.tools", key.c_str(), fallback.c_str(), "CodePilot compatibility policy string setting");
    const std::string compat = compat_value == nullptr ? "" : compat_value;
    const char *policy_value =
        ::dsn_config_get_value_string("rasn.policy", key.c_str(), compat.empty() ? fallback.c_str() : compat.c_str(), "rASN policy string setting");
    const std::string policy = policy_value == nullptr ? "" : policy_value;
    if (policy.empty())
    {
        return fallback;
    }
    return policy;
}

bool existing_read_target(const std::string &tool_name, const std::string &target)
{
    const std::string normalized_target = normalize_platform_path(target);
    if (normalized_target.empty())
    {
        return tool_name == "list";
    }
    if (tool_name == "read")
    {
        return ::dsn::utils::filesystem::file_exists(normalized_target);
    }
    if (tool_name == "list")
    {
        return ::dsn::utils::filesystem::directory_exists(normalized_target);
    }
    if (tool_name == "search")
    {
        return ::dsn::utils::filesystem::file_exists(normalized_target) ||
               ::dsn::utils::filesystem::directory_exists(normalized_target);
    }
    return true;
}

std::string policy_path_for_compare(std::string path)
{
    path = normalize_platform_path(path);
    std::string absolute;
    if (::dsn::utils::filesystem::get_absolute_path(path, absolute))
    {
        path = absolute;
    }

    std::string normalized;
    if (::dsn::utils::filesystem::get_normalized_path(path, normalized) == 0 && !normalized.empty())
    {
        path = normalized;
    }
    path = normalize_platform_path(path);

#if !defined(_WIN32)
    // Resolve symbolic links against the real filesystem so an in-workspace
    // symlink that points outside the workspace root cannot slip past the
    // lexical prefix check below. realpath() also collapses ".." segments.
    if (!path.empty())
    {
        char resolved[PATH_MAX];
        if (::realpath(path.c_str(), resolved) != nullptr)
        {
            path = resolved;
        }
        else
        {
            // The leaf may not exist yet (e.g. a write target). Canonicalize
            // the nearest existing parent and re-append the trailing name so a
            // symlinked parent directory is still forced back to its real
            // location.
            const std::string parent = ::dsn::utils::filesystem::remove_file_name(path);
            const std::string leaf = ::dsn::utils::filesystem::get_file_name(path);
            if (!parent.empty() && parent != path && ::realpath(parent.c_str(), resolved) != nullptr)
            {
                path = resolved;
                if (!leaf.empty())
                {
                    path += "/";
                    path += leaf;
                }
            }
        }
        path = normalize_platform_path(path);
    }
#endif

#if defined(_WIN32)
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif

    while (path.size() > 1 && (path.back() == '\\' || path.back() == '/'))
    {
#if defined(_WIN32)
        if (path.size() == 3 && path[1] == ':')
        {
            break;
        }
#endif
        path.pop_back();
    }
    return path;
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

std::string safe_task_id(const agent_task &task)
{
    if (task.id.empty())
    {
        return "tool";
    }
    std::string safe = task.id;
    for (char &c : safe)
    {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
        {
            c = '_';
        }
    }
    return safe;
}

std::string current_process_id()
{
    std::ostringstream oss;
#if defined(_WIN32)
    oss << _getpid();
#else
    oss << getpid();
#endif
    return oss.str();
}

std::string spill_suffix()
{
    // Use rDSN's environment-provider RNG (dsn_random64) so the suffix is
    // virtualizable under deterministic-replay tooling, instead of the
    // process-global std::rand() generator.
    std::ostringstream oss;
    oss << current_process_id() << "-" << std::hex << ::dsn_random64(0, 0xffffffffffffffffULL);
    return oss.str();
}

state_response default_policy_state_writer(const state_record &record)
{
    return global_state_store().put(record);
}

::dsn::service::zlock &policy_state_writer_lock()
{
    static ::dsn::service::zlock lock;
    return lock;
}

policy_state_writer &policy_state_writer_slot()
{
    static policy_state_writer writer = &default_policy_state_writer;
    return writer;
}

state_response write_policy_state_record(const state_record &record)
{
    policy_state_writer writer = nullptr;
    {
        ::dsn::service::zauto_lock guard(policy_state_writer_lock());
        writer = policy_state_writer_slot();
    }
    return writer(record);
}

} // namespace

std::string to_string(tool_side_effect side_effect)
{
    switch (side_effect)
    {
    case tool_side_effect::read_only:
        return "read_only";
    case tool_side_effect::write:
        return "write";
    case tool_side_effect::shell:
        return "shell";
    case tool_side_effect::network:
        return "network";
    default:
        return "unknown";
    }
}

tool_side_effect classify_tool_side_effect(const std::string &tool_name)
{
    if (tool_name == "list" || tool_name == "read" || tool_name == "search")
    {
        return tool_side_effect::read_only;
    }
    if (tool_name == "write" || tool_name == "replace")
    {
        return tool_side_effect::write;
    }
    if (tool_name == "shell")
    {
        return tool_side_effect::shell;
    }
    return tool_side_effect::unknown;
}

std::string human_approval_policy_label(tool_side_effect side_effect)
{
    return "human_approved:" + to_string(side_effect);
}

bool policy_labels_include_human_approval(const std::vector<std::string> &policy_labels,
                                          tool_side_effect side_effect)
{
    const std::string side_effect_label = human_approval_policy_label(side_effect);
    return std::find(policy_labels.begin(), policy_labels.end(), "human_approved") != policy_labels.end() ||
           std::find(policy_labels.begin(), policy_labels.end(), side_effect_label) != policy_labels.end();
}

bool policy_target_within_workspace(const std::string &target,
                                    const std::string &workspace_root,
                                    std::string *normalized_target,
                                    std::string *normalized_root)
{
    if (workspace_root.empty())
    {
        if (normalized_target != nullptr)
        {
            *normalized_target = normalize_platform_path(target);
        }
        if (normalized_root != nullptr)
        {
            normalized_root->clear();
        }
        return true;
    }

    const std::string target_path = policy_path_for_compare(target.empty() ? "." : target);
    const std::string root_path = policy_path_for_compare(workspace_root);
    if (normalized_target != nullptr)
    {
        *normalized_target = target_path;
    }
    if (normalized_root != nullptr)
    {
        *normalized_root = root_path;
    }
    return !root_path.empty() && path_has_prefix_boundary(target_path, root_path);
}

policy_request make_policy_request(const std::string &tool_name,
                                   const std::vector<std::string> &args,
                                   const agent_task &task,
                                   const std::vector<std::string> &policy_labels)
{
    policy_request request;
    request.tool_name = tool_name;
    request.args = args;
    request.side_effect = to_string(classify_tool_side_effect(tool_name));
    request.target = args.empty() ? "" : args[0];
    request.actor = task.name.empty() ? "unknown" : task.name;
    request.policy_labels = policy_labels;
    return request;
}

void set_policy_state_writer(policy_state_writer writer)
{
    ::dsn::service::zauto_lock guard(policy_state_writer_lock());
    policy_state_writer_slot() = writer == nullptr ? &default_policy_state_writer : writer;
}

void reset_policy_state_writer()
{
    set_policy_state_writer(nullptr);
}

policy_decision policy_manager::evaluate(const policy_request &request) const
{
    policy_decision decision;
    decision.policy_name = "config";

    if (request.schema_version != RASN_AGENT_SCHEMA_VERSION)
    {
        decision.side_effect = request.side_effect;
        decision.reason = "policy request has unsupported schema version";
        return decision;
    }

    // Re-derive the side-effect class from the tool name rather than trusting a
    // caller-supplied request.side_effect. Otherwise a request that arrives over
    // RPC could claim tool_name="shell" while labeling side_effect="read_only"
    // and take the read path, bypassing the shell opt-in.
    const std::string side_effect = to_string(classify_tool_side_effect(request.tool_name));
    decision.side_effect = side_effect;

    if (side_effect == "read_only" || side_effect == "write")
    {
        std::string normalized_target;
        std::string normalized_root;
        if (!policy_target_within_workspace(request.target, effective_workspace_root(), &normalized_target, &normalized_root))
        {
            decision.reason = "tool target is outside workspace root: target=" + normalized_target +
                              " root=" + normalized_root;
            return decision;
        }
    }

    if (side_effect == "read_only")
    {
        decision.allowed = existing_read_target(request.tool_name, request.target);
        decision.reason = decision.allowed ? "read-only tool allowed" : "read-only tool target is invalid";
        return decision;
    }

    if (side_effect == "write")
    {
        if (!config_bool("allow_write", false))
        {
            decision.reason = "write tool denied by default policy";
            return decision;
        }
        const bool require_approval = config_bool("require_write_approval", true);
        if (require_approval && !policy_labels_include_human_approval(request.policy_labels, tool_side_effect::write))
        {
            decision.reason = "write tool requires human approval";
            return decision;
        }
        decision.allowed = true;
        decision.reason = require_approval ? "write tool explicitly enabled with human approval"
                                           : "write tool explicitly enabled";
        return decision;
    }

    if (side_effect == "shell")
    {
        if (!config_bool("allow_shell", false))
        {
            decision.reason = "shell tool denied by default policy";
            return decision;
        }
        const bool require_approval = config_bool("require_shell_approval", true);
        if (require_approval && !policy_labels_include_human_approval(request.policy_labels, tool_side_effect::shell))
        {
            decision.reason = "shell tool requires human approval";
            return decision;
        }
        decision.allowed = true;
        decision.reason = require_approval ? "shell tool explicitly enabled with human approval"
                                           : "shell tool explicitly enabled";
        return decision;
    }

    if (side_effect == "network")
    {
        decision.allowed = config_bool("allow_network", false);
        decision.reason = decision.allowed ? "network tool explicitly enabled" : "network tool denied by default policy";
        return decision;
    }

    decision.reason = "unknown tool side-effect class";
    return decision;
}

tool_result policy_manager::apply_tool_output_bounds(const std::string &tool_name,
                                                     const agent_task &task,
                                                     const tool_result &result) const
{
    tool_result redacted = result;
    redacted.output = redact_sensitive_text(result.output);
    redacted.error = redact_sensitive_text(result.error);

    const size_t max_bytes = config_size("max_tool_output_bytes", 65536);
    if (max_bytes == 0 || redacted.output.size() <= max_bytes)
    {
        return redacted;
    }

    const std::string dir = artifact_dir();
    if (!::dsn::utils::filesystem::directory_exists(dir) && !::dsn::utils::filesystem::create_directory(dir))
    {
        return make_tool_result(false,
                                redacted.output.substr(0, max_bytes),
                                "tool output exceeded limit and artifact directory could not be created: " + dir);
    }

    const std::string suffix = spill_suffix();
    const std::string file_name = "tool-output-" + safe_task_id(task) + "-" + suffix + ".txt";
    const std::string path = ::dsn::utils::filesystem::path_combine(dir, file_name);
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!output)
    {
        return make_tool_result(false,
                                redacted.output.substr(0, max_bytes),
                                "tool output exceeded limit and artifact could not be written: " + path);
    }

    output << redacted.output;
    output.close();
    if (!output)
    {
        return make_tool_result(false,
                                redacted.output.substr(0, max_bytes),
                                "tool output exceeded limit and artifact could not be flushed: " + path);
    }

    const std::string state_key = "artifact/" + safe_task_id(task) + "-" + suffix;
    state_record record;
    record.key = state_key;
    record.kind = "artifact";
    record.scope = "rasn.policy";
    record.value = "tool=" + tool_name + "\npath=" + path + "\nbytes=" + std::to_string(redacted.output.size());
    const state_response stored = write_policy_state_record(record);
    if (!stored.ok)
    {
        dwarn("failed to index oversized tool output artifact in state: %s", stored.error.c_str());
    }

    std::ostringstream bounded;
    bounded << redacted.output.substr(0, max_bytes)
            << "\n\n[tool output truncated at " << max_bytes
            << " bytes; full output stored at " << path
            << "; state_key=" << state_key << "]";
    return make_tool_result(redacted.ok, bounded.str(), redacted.error);
}

bool policy_manager::config_bool(const std::string &key, bool fallback) const
{
    return config_bool_compat(key, fallback);
}

size_t policy_manager::config_size(const std::string &key, size_t fallback) const
{
    return config_size_compat(key, fallback);
}

std::string policy_manager::config_string(const std::string &key, const std::string &fallback) const
{
    return config_string_compat(key, fallback);
}

std::string policy_manager::artifact_dir() const
{
    return config_string_compat("artifact_dir", "rasn/artifacts");
}

std::string policy_manager::effective_workspace_root() const
{
    const std::string configured = config_string("workspace_root", "");
    if (!configured.empty())
    {
        return configured;
    }

    // An explicit, opt-in escape hatch preserves the unconfined dev-CLI
    // behavior for operators who knowingly want it.
    if (config_bool("allow_unconfined_paths", false))
    {
        return "";
    }

    // Default to the process working directory so read/list/search tools are
    // confined out of the box instead of accepting arbitrary absolute paths.
    std::string cwd;
    if (::dsn::utils::filesystem::get_current_directory(cwd) && !cwd.empty())
    {
        return cwd;
    }
    return configured;
}

policy_manager &global_policy_manager()
{
    static policy_manager manager;
    return manager;
}

} // namespace rasn
} // namespace dsn
