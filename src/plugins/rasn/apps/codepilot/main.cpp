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

int maybe_run_direct_schema_command(int argc, char **argv)
{
    if (argc < 2 || argv[1] == nullptr || std::string(argv[1]) != "schema")
    {
        return -1;
    }
    if (argc > 3)
    {
        std::cout << "usage: schema [text|json|idl|cpp|clients-cpp|ts|clients-ts|py|clients-py]\n";
        return 1;
    }

    const std::string format = argc == 3 && argv[2] != nullptr ? argv[2] : "text";
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
    const int schema_result = maybe_run_direct_schema_command(argc, argv);
    if (schema_result != -1)
    {
        return schema_result;
    }

    register_rasn_apps();
    const std::string program = (argc > 0 && argv[0] != nullptr) ? argv[0] : "";
    const bool dsn_mode = argc > 1 && argv[1] != nullptr && std::string(argv[1]) == "--dsn";
    const std::string explicit_dsn_config = (dsn_mode && argc > 2 && argv[2] != nullptr) ? argv[2] : "";

    if (argc > 1 && !dsn_mode)
    {
        const std::string config_path = find_config_path(program);
        const std::string empty_app_list = "__rasn_direct_cli__";
        std::vector<char *> dsn_args;
        dsn_args.push_back(argv[0]);
        dsn_args.push_back(const_cast<char *>(config_path.c_str()));
        dsn_args.push_back(const_cast<char *>("-app_list"));
        dsn_args.push_back(const_cast<char *>(empty_app_list.c_str()));
        ::dsn_run(static_cast<int>(dsn_args.size()), dsn_args.data(), false);

        ::dsn::rasn::codepilot_cli cli;
        std::vector<std::string> command_args;
        for (int i = 1; i < argc; ++i)
        {
            command_args.push_back(argv[i] == nullptr ? "" : argv[i]);
        }
        return cli.run(command_args);
    }

    if (dsn_mode)
    {
        ++argv;
        --argc;
    }

    const std::string config_path = explicit_dsn_config.empty() ? find_config_path(program) : explicit_dsn_config;
    ::dsn_run_config(config_path.c_str(), true);
    return 0;
}
