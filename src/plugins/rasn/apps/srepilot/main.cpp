#include "srepilot_app.h"
#include <rasn/agent_registry.h>
#include <rasn/runtime_provider.h>
#include <rasn/observability.h>
#include <rasn/state_service.h>
#include <rasn/workflow_service.h>

#include <dsn/cpp/utils.h>

#include <string>
#include <vector>

namespace {

void register_rasn_apps()
{
    dassert(::dsn::register_app< ::dsn::rasn::rasn_registry_app>("rasn.registry"),
            "register rasn.registry app failed");
    dassert(::dsn::register_app< ::dsn::rasn::rasn_state_app>("rasn.state"),
            "register rasn.state app failed");
    dassert(::dsn::register_app< ::dsn::rasn::rasn_workflow_app>("rasn.workflow"),
            "register rasn.workflow app failed");
    dassert(::dsn::register_app< ::dsn::rasn::rasn_observability_app>("rasn.observability"),
            "register rasn.observability app failed");
    ::dsn::rasn::register_rasn_runtime_apps();
    dassert(::dsn::register_app< ::dsn::rasn::rasn_llm_agent_app>("rasn.llm.agent"),
            "register rasn.llm.agent app failed");
    dassert(::dsn::register_app< ::dsn::rasn::rasn_tool_agent_app>("rasn.tool.agent"),
            "register rasn.tool.agent app failed");
    dassert(::dsn::register_app< ::dsn::rasn::rasn_coordinator_app>("rasn.coordinator"),
            "register rasn.coordinator app failed");
    dassert(::dsn::register_app< ::dsn::rasn::srepilot_app>("rasn.srepilot"),
            "register rasn.srepilot app failed");
}

std::string find_config_file(const std::string &program, const char *filename)
{
    if (!program.empty())
    {
        const std::string exe_dir = ::dsn::utils::filesystem::remove_file_name(program);
        if (!exe_dir.empty())
        {
            const std::string beside_exe = ::dsn::utils::filesystem::path_combine(exe_dir, filename);
            if (::dsn::utils::filesystem::file_exists(beside_exe))
            {
                return beside_exe;
            }

            const std::string parent_dir = ::dsn::utils::filesystem::remove_file_name(exe_dir);
            if (!parent_dir.empty())
            {
                const std::string beside_target = ::dsn::utils::filesystem::path_combine(parent_dir, filename);
                if (::dsn::utils::filesystem::file_exists(beside_target))
                {
                    return beside_target;
                }
            }
        }
    }

    if (::dsn::utils::filesystem::file_exists(filename))
    {
        return filename;
    }

    const std::string parent_rel = std::string("..\\") + filename;
    if (::dsn::utils::filesystem::file_exists(parent_rel))
    {
        return parent_rel;
    }
    return filename;
}

std::string find_config_path(const std::string &program)
{
    return find_config_file(program, "config.ini");
}

// In `--dsn` service mode, prefer the shared runtime config.rasn.ini when it is
// deployed next to the binary: it launches the full service fleet and, via a
// trailing `@include config.ini`, co-hosts this binary's own app gateway. When
// the runtime config is absent, fall back to the thin config.ini so `--dsn` still
// starts on built-in default configuration values.
std::string find_runtime_config_path(const std::string &program)
{
    const std::string runtime_config = find_config_file(program, "config.rasn.ini");
    if (::dsn::utils::filesystem::file_exists(runtime_config))
    {
        return runtime_config;
    }
    return find_config_path(program);
}

} // namespace

int main(int argc, char **argv)
{
    register_rasn_apps();
    const std::vector<std::string> process_args = ::dsn::rasn::cli_args_from_argv(argc, argv);
    const std::vector<std::string> command_args = ::dsn::rasn::cli_args_from_argv(argc, argv, 1);
    const std::string program = process_args.empty() ? "" : process_args[0];
    const bool dsn_mode = !command_args.empty() && command_args[0] == "--dsn";
    const std::string explicit_dsn_config = (dsn_mode && command_args.size() > 1) ? command_args[1] : "";
    const std::string explicit_dsn_app_list = (dsn_mode && command_args.size() > 2) ? command_args[2] : "";

    if (!dsn_mode)
    {
        const std::string config_path = find_config_path(program);
        const std::string empty_app_list = "__rasn_direct_cli__";
        ::dsn::rasn::run_dsn_with_cli_args(
            std::vector<std::string>{program, config_path, "-app_list", empty_app_list}, false);

        ::dsn::rasn::srepilot_cli cli;
        return cli.run(command_args);
    }

    // In `--dsn` service mode, prefer the shared runtime config.rasn.ini deployed
    // next to the binary (it launches the full service fleet and, via its trailing
    // `@include config.ini`, co-hosts this binary's own gateway). When that runtime
    // config is absent, fall back to the thin config.ini so `--dsn` still starts on
    // built-in default configuration values. An explicit `--dsn <path>` wins.
    const std::string config_path = explicit_dsn_config.empty() ? find_runtime_config_path(program) : explicit_dsn_config;
    const std::string default_srepilot_app_list =
        "rasn.registry;rasn.llm.agent;rasn.tool.agent;rasn.state;rasn.coordinator;rasn.workflow;"
        "rasn.observability;rasn.runtime;rasn.srepilot";
    const std::string srepilot_app_list =
        explicit_dsn_app_list.empty() ? default_srepilot_app_list
                                      : ::dsn::rasn::normalize_rasn_runtime_app_list(explicit_dsn_app_list);
    // Ensure config.rasn.ini's trailing `@include config.ini` resolves beside the
    // selected runtime config even when the binary is launched from another
    // directory (rDSN resolves includes relative to the working directory).
    const std::string runtime_config_path = ::dsn::rasn::align_working_directory_to_runtime_config(config_path);
    ::dsn::rasn::run_dsn_with_cli_args(
        std::vector<std::string>{program, runtime_config_path, "-app_list", srepilot_app_list}, true);
    return 0;
}
