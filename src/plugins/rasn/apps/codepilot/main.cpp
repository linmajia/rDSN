#include "codepilot_app.h"
#include "../../agent_registry.h"
#include "../../observability.h"
#include "../../schema_manifest.h"
#include "../../state_service.h"
#include "../../workflow_service.h"

#include <dsn/cpp/utils.h>

#include <iostream>
#include <string>
#include <vector>

namespace {

void register_rasn_apps()
{
    dassert(::dsn::register_app<::dsn::rasn::rasn_registry_app>("rasn.registry"),
            "register rasn.registry app failed");
    dassert(::dsn::register_app<::dsn::rasn::rasn_state_app>("rasn.state"),
            "register rasn.state app failed");
    dassert(::dsn::register_app<::dsn::rasn::rasn_workflow_app>("rasn.workflow"),
            "register rasn.workflow app failed");
    dassert(::dsn::register_app<::dsn::rasn::rasn_observability_app>("rasn.observability"),
            "register rasn.observability app failed");
    dassert(::dsn::register_app<::dsn::rasn::rasn_llm_agent_app>("rasn.llm.agent"),
            "register rasn.llm.agent app failed");
    dassert(::dsn::register_app<::dsn::rasn::rasn_tool_agent_app>("rasn.tool.agent"),
            "register rasn.tool.agent app failed");
    dassert(::dsn::register_app<::dsn::rasn::rasn_coordinator_app>("rasn.coordinator"),
            "register rasn.coordinator app failed");
    dassert(::dsn::register_app<::dsn::rasn::codepilot_app>("rasn.codepilot"),
            "register rasn.codepilot app failed");
}

std::string find_config_path(const std::string &program)
{
    if (::dsn::utils::filesystem::file_exists("config.ini"))
    {
        return "config.ini";
    }

    if (!program.empty())
    {
        const std::string exe_dir = ::dsn::utils::filesystem::remove_file_name(program);
        if (!exe_dir.empty())
        {
            const std::string beside_exe = ::dsn::utils::filesystem::path_combine(exe_dir, "config.ini");
            if (::dsn::utils::filesystem::file_exists(beside_exe))
            {
                return beside_exe;
            }

            const std::string parent_dir = ::dsn::utils::filesystem::remove_file_name(exe_dir);
            if (!parent_dir.empty())
            {
                const std::string beside_target = ::dsn::utils::filesystem::path_combine(parent_dir, "config.ini");
                if (::dsn::utils::filesystem::file_exists(beside_target))
                {
                    return beside_target;
                }
            }
        }
    }

    if (::dsn::utils::filesystem::file_exists("..\\config.ini"))
    {
        return "..\\config.ini";
    }
    return "config.ini";
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
    const std::vector<std::string> process_args = ::dsn::rasn::cli_args_from_argv(argc, argv);
    const std::vector<std::string> command_args = ::dsn::rasn::cli_args_from_argv(argc, argv, 1);
    const int schema_result = maybe_run_direct_schema_command(command_args);
    if (schema_result != -1)
    {
        return schema_result;
    }

    register_rasn_apps();
    const std::string program = process_args.empty() ? "" : process_args[0];
    const bool dsn_mode = !command_args.empty() && command_args[0] == "--dsn";
    const std::string explicit_dsn_config = (dsn_mode && command_args.size() > 1) ? command_args[1] : "";

    if (!dsn_mode)
    {
        const std::string config_path = find_config_path(program);
        const std::string empty_app_list = "__rasn_direct_cli__";
        ::dsn::rasn::run_dsn_with_cli_args(
            std::vector<std::string>{program, config_path, "-app_list", empty_app_list}, false);

        ::dsn::rasn::codepilot_cli cli;
        return cli.run(command_args);
    }

    const std::string config_path = explicit_dsn_config.empty() ? find_config_path(program) : explicit_dsn_config;
    const std::string codepilot_app_list =
        "rasn.registry;rasn.llm.agent;rasn.tool.agent;rasn.state;rasn.coordinator;rasn.workflow;"
        "rasn.observability;rasn.codepilot";
    ::dsn::rasn::run_dsn_with_cli_args(
        std::vector<std::string>{program, config_path, "-app_list", codepilot_app_list}, true);
    return 0;
}
