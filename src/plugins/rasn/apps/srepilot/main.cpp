#include "srepilot_app.h"
#include <rasn/agent_registry.h>
#include <rasn/runtime_provider.h>
#include <rasn/observability.h>
#include <rasn/state_service.h>
#include <rasn/workflow_service.h>

#include <dsn/cpp/utils.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

void register_rasn_apps()
{
    dassert(::dsn::register_app< ::dsn::rasn::rasn_registry_app>("rasn.registry"),
            "register rasn.registry app failed");
    ::dsn::rasn::register_rasn_state_apps();
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

} // namespace

int main(int argc, char **argv)
{
    ::dsn::rasn::install_rasn_cli_out_of_memory_handler();
    register_rasn_apps();
    const std::vector<std::string> process_args = ::dsn::rasn::cli_args_from_argv(argc, argv);
    const std::vector<std::string> command_args = ::dsn::rasn::cli_args_from_argv(argc, argv, 1);
    const std::string program = process_args.empty() ? "" : process_args[0];

    // Runtime-host mode: `serve` (preferred) or the deprecated `--dsn` alias. This
    // brings up the rASN runtime as standalone services; it is a deployment ROLE,
    // NOT a placement selector -- an app's local/distributed/hybrid placement comes
    // from config ([rasn.runtime]), never the command line.
    const bool host_mode =
        !command_args.empty() && (command_args[0] == "serve" || command_args[0] == "--dsn");

    if (!host_mode)
    {
        // Client/CLI: run one command against the runtime. Placement comes from the
        // app's config.ini [rasn.runtime]. For distributed/hybrid, start the no-op
        // mimic node and attach to it so remote module RPC has a node context; for
        // local, keep the node-less fast path (modules run in-process).
        const std::string config_path =
            ::dsn::rasn::find_rasn_cli_config_file(program, "config.ini");
        if (config_path.empty())
        {
            fprintf(stderr, "rasn: app config.ini was not found beside the SREPilot executable\n");
            return 1;
        }
        const bool remote = ::dsn::rasn::rasn_runtime_config_file_selects_remote(config_path);
        const std::string cli_app_list = remote ? "mimic" : "__rasn_direct_cli__";
        ::dsn::rasn::run_dsn_with_cli_args(
            std::vector<std::string>{program, config_path, "-app_list", cli_app_list}, false);
        if (remote)
        {
            ::dsn::rasn::attach_cli_runtime_client_node();
        }

        ::dsn::rasn::srepilot_cli cli;
        return cli.run(command_args);
    }

    // `serve`/`--dsn`: launch the standalone runtime host from config.rasn.ini.
    // An explicit `serve <path>` wins; a missing host config fails clearly rather
    // than starting a process with no service specs and sleeping forever.
    const std::string explicit_config = command_args.size() > 1 ? command_args[1] : "";
    const std::string explicit_app_list = command_args.size() > 2 ? command_args[2] : "";
    const std::string config_path =
        explicit_config.empty()
            ? ::dsn::rasn::find_rasn_cli_config_file(program, "config.rasn.ini")
            : explicit_config;
    if (config_path.empty() || !::dsn::utils::filesystem::file_exists(config_path))
    {
        fprintf(stderr,
                "rasn: runtime host config '%s' was not found; deploy config.rasn.ini with "
                "config.rasn.defaults.ini or pass 'serve <config>'\n",
                explicit_config.empty() ? "config.rasn.ini" : explicit_config.c_str());
        return 1;
    }
    // The runtime host carries no app gateway of its own -- the fleet is services only.
    const std::string default_host_app_list =
        "rasn.registry;rasn.llm.agent;rasn.tool.agent;rasn.state;rasn.coordinator;rasn.workflow;"
        "rasn.observability;rasn.runtime";
    const std::string host_app_list =
        explicit_app_list.empty() ? default_host_app_list
                                  : ::dsn::rasn::normalize_rasn_runtime_app_list(explicit_app_list);
    // Ensure config.rasn.ini's `@include config.rasn.defaults.ini` resolves beside
    // the selected runtime config even when the binary is launched from another
    // directory (rDSN resolves includes relative to the working directory).
    const std::string runtime_config_path =
        ::dsn::rasn::align_working_directory_to_runtime_config(config_path);
    // Guard an explicit override that would start no runnable app instance and
    // leave the host sleeping without any bound services.
    if (!explicit_app_list.empty())
    {
        const ::dsn::rasn::rasn_runtime_host_app_list_check check =
            ::dsn::rasn::rasn_runtime_check_host_app_list(runtime_config_path, host_app_list);
        if (check.config_loaded && !check.invalid.empty())
        {
            std::string invalid;
            for (const std::string &selector : check.invalid)
            {
                invalid += " " + selector;
            }
            fprintf(stderr,
                    "rasn: serve app_list contains invalid app instance selectors:%s\n",
                    invalid.c_str());
            return 1;
        }
        if (check.config_loaded && check.matched == 0)
        {
            std::string available;
            for (const ::dsn::rasn::rasn_runtime_host_app_spec &app : check.apps)
            {
                if (app.run && app.count > 0)
                {
                    available += " " + app.name + "(count=" + std::to_string(app.count) + ")";
                }
            }
            if (available.empty())
            {
                available = " <none>";
            }
            fprintf(stderr,
                    "rasn: serve app_list '%s' selects no runnable [apps.*] instance in '%s'; the "
                    "runtime host would start no services.\n      runnable apps:%s\n",
                    explicit_app_list.c_str(), runtime_config_path.c_str(), available.c_str());
            return 1;
        }
        if (check.config_loaded && !check.unstartable.empty())
        {
            std::string ignored;
            for (const std::string &selector : check.unstartable)
            {
                ignored += " " + selector;
            }
            fprintf(stderr,
                    "rasn: warning: serve app_list entries select no runnable [apps.*] instance "
                    "and will be ignored:%s\n",
                    ignored.c_str());
        }
    }
    ::dsn::rasn::run_dsn_with_cli_args(
        std::vector<std::string>{program, runtime_config_path, "-app_list", host_app_list}, true);
    return 0;
}
