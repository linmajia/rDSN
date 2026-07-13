#include "codepilot_app.h"
#include <rasn/agent_registry.h>
#include <rasn/runtime_provider.h>
#include <rasn/observability.h>
#include <rasn/schema_manifest.h>
#include <rasn/state_service.h>
#include <rasn/workflow_service.h>

#include <dsn/cpp/utils.h>

#include <iostream>
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
    dassert(::dsn::register_app< ::dsn::rasn::codepilot_app>("rasn.codepilot"),
            "register rasn.codepilot app failed");
}

int maybe_run_direct_schema_command(const std::vector<std::string> &args)
{
    if (args.empty() || args[0] != "schema")
    {
        return -1;
    }
    if (args.size() > 2)
    {
        std::cout << "usage: schema [text|json|idl|cpp|clients-cpp|ts|clients-ts|py|clients-py]\n";
        return 1;
    }

    const std::string format = args.size() == 2 ? args[1] : "text";
    if (format == "json" || format == "--json")
    {
        std::cout << ::dsn::rasn::rasn_schema_manifest_json();
        return 0;
    }
    if (format == "idl" || format == "--idl")
    {
        std::cout << ::dsn::rasn::rasn_schema_manifest_idl();
        return 0;
    }
    if (format == "cpp" || format == "c++" || format == "--cpp")
    {
        std::cout << ::dsn::rasn::rasn_schema_manifest_cpp_header();
        return 0;
    }
    if (format == "clients-cpp" || format == "client-cpp" || format == "cpp-clients" || format == "--clients-cpp")
    {
        std::cout << ::dsn::rasn::rasn_schema_manifest_cpp_clients();
        return 0;
    }
    if (format == "ts" || format == "typescript" || format == "--ts")
    {
        std::cout << ::dsn::rasn::rasn_schema_manifest_typescript();
        return 0;
    }
    if (format == "clients-ts" || format == "client-ts" || format == "ts-clients" || format == "--clients-ts")
    {
        std::cout << ::dsn::rasn::rasn_schema_manifest_typescript_clients();
        return 0;
    }
    if (format == "py" || format == "python" || format == "--py")
    {
        std::cout << ::dsn::rasn::rasn_schema_manifest_python();
        return 0;
    }
    if (format == "clients-py" || format == "client-py" || format == "py-clients" ||
        format == "python-clients" || format == "--clients-py")
    {
        std::cout << ::dsn::rasn::rasn_schema_manifest_python_clients();
        return 0;
    }
    if (format == "text" || format == "--text")
    {
        std::cout << ::dsn::rasn::rasn_schema_manifest_text();
        return 0;
    }

    std::cout << "usage: schema [text|json|idl|cpp|clients-cpp|ts|clients-ts|py|clients-py]\n";
    return 1;
}

} // namespace

int main(int argc, char **argv)
{
    ::dsn::rasn::install_rasn_cli_out_of_memory_handler();
    const std::vector<std::string> process_args = ::dsn::rasn::cli_args_from_argv(argc, argv);
    const std::vector<std::string> command_args = ::dsn::rasn::cli_args_from_argv(argc, argv, 1);
    const int schema_result = maybe_run_direct_schema_command(command_args);
    if (schema_result != -1)
    {
        return schema_result;
    }

    register_rasn_apps();
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
            std::cerr << "rasn: app config.ini was not found beside the CodePilot executable\n";
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

        ::dsn::rasn::codepilot_cli cli;
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
        std::cerr << "rasn: runtime host config '"
                  << (explicit_config.empty() ? "config.rasn.ini" : explicit_config)
                  << "' was not found; deploy config.rasn.ini with "
                     "config.rasn.defaults.ini or pass 'serve <config>'\n";
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
            std::cerr << "rasn: serve app_list contains invalid app instance selectors:";
            for (const std::string &selector : check.invalid)
            {
                std::cerr << " " << selector;
            }
            std::cerr << "\n";
            return 1;
        }
        if (check.config_loaded && check.matched == 0)
        {
            std::cerr << "rasn: serve app_list '" << explicit_app_list
                      << "' selects no runnable [apps.*] instance in '" << runtime_config_path
                      << "'; the runtime host would start no services.\n"
                      << "      runnable apps:";
            bool any_runnable = false;
            for (const ::dsn::rasn::rasn_runtime_host_app_spec &app : check.apps)
            {
                if (app.run && app.count > 0)
                {
                    std::cerr << " " << app.name << "(count=" << app.count << ")";
                    any_runnable = true;
                }
            }
            if (!any_runnable)
            {
                std::cerr << " <none>";
            }
            std::cerr << "\n";
            return 1;
        }
        if (check.config_loaded && !check.unstartable.empty())
        {
            std::cerr << "rasn: warning: serve app_list entries select no runnable [apps.*] "
                         "instance and will be ignored:";
            for (const std::string &selector : check.unstartable)
            {
                std::cerr << " " << selector;
            }
            std::cerr << "\n";
        }
    }
    ::dsn::rasn::run_dsn_with_cli_args(
        std::vector<std::string>{program, runtime_config_path, "-app_list", host_app_list}, true);
    return 0;
}
