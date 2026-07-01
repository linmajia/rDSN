#include "srepilot_app.h"
#include "../../agent_registry.h"
#include "../../observability.h"
#include "../../state_service.h"
#include "../../workflow_service.h"

#include <dsn/cpp/utils.h>

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
    dassert(::dsn::register_app<::dsn::rasn::srepilot_app>("rasn.srepilot"),
            "register rasn.srepilot app failed");
}

std::string find_config_path(const std::string &program)
{
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

    if (::dsn::utils::filesystem::file_exists("config.ini"))
    {
        return "config.ini";
    }

    if (::dsn::utils::filesystem::file_exists("..\\config.ini"))
    {
        return "..\\config.ini";
    }
    return "config.ini";
}

} // namespace

int main(int argc, char **argv)
{
    register_rasn_apps();
    const std::string program = (argc > 0 && argv[0] != nullptr) ? argv[0] : "";
    const bool dsn_mode = argc > 1 && argv[1] != nullptr && std::string(argv[1]) == "--dsn";
    const std::string explicit_dsn_config = (dsn_mode && argc > 2 && argv[2] != nullptr) ? argv[2] : "";

    if (!dsn_mode)
    {
        const std::string config_path = find_config_path(program);
        const std::string empty_app_list = "__rasn_direct_cli__";
        std::vector<char *> dsn_args;
        dsn_args.push_back(argv[0]);
        dsn_args.push_back(const_cast<char *>(config_path.c_str()));
        dsn_args.push_back(const_cast<char *>("-app_list"));
        dsn_args.push_back(const_cast<char *>(empty_app_list.c_str()));
        ::dsn_run(static_cast<int>(dsn_args.size()), dsn_args.data(), false);

        ::dsn::rasn::srepilot_cli cli;
        std::vector<std::string> command_args;
        for (int i = 1; i < argc; ++i)
        {
            command_args.push_back(argv[i] == nullptr ? "" : argv[i]);
        }
        return cli.run(command_args);
    }

    const std::string config_path = explicit_dsn_config.empty() ? find_config_path(program) : explicit_dsn_config;
    const std::string srepilot_app_list =
        "rasn.registry;rasn.llm.agent;rasn.tool.agent;rasn.state;rasn.coordinator;rasn.workflow;"
        "rasn.observability;rasn.srepilot";
    std::vector<char *> dsn_args;
    dsn_args.push_back(argv[0]);
    dsn_args.push_back(const_cast<char *>(config_path.c_str()));
    dsn_args.push_back(const_cast<char *>("-app_list"));
    dsn_args.push_back(const_cast<char *>(srepilot_app_list.c_str()));
    ::dsn_run(static_cast<int>(dsn_args.size()), dsn_args.data(), true);
    return 0;
}
