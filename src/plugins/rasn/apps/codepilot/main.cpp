#include "codepilot_app.h"
#include "../../agent_registry.h"
#include "../../observability.h"
#include "../../schema_manifest.h"
#include "../../state_service.h"
#include "../../workflow_service.h"

#include <dsn/cpp/utils.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

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

std::string trim_ascii(const std::string &value)
{
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])))
    {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
    {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string lower_ascii(std::string value)
{
    for (char &ch : value)
    {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

bool starts_with(const std::string &value, const std::string &prefix)
{
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool direct_cli_skip_config_section(const std::string &section)
{
    const std::string lower = lower_ascii(trim_ascii(section));
    return starts_with(lower, "apps.");
}

std::string current_process_id()
{
#if defined(_WIN32)
    return std::to_string(static_cast<unsigned int>(::_getpid()));
#else
    return std::to_string(static_cast<unsigned int>(::getpid()));
#endif
}

std::string direct_cli_config_path()
{
    const char *tmp = std::getenv("TMPDIR");
    if (tmp == nullptr || tmp[0] == '\0')
    {
        tmp = std::getenv("TEMP");
    }
    if (tmp == nullptr || tmp[0] == '\0')
    {
        tmp = std::getenv("TMP");
    }
    const std::string temp_dir = (tmp == nullptr || tmp[0] == '\0') ? "." : tmp;
    return ::dsn::utils::filesystem::path_combine(
        temp_dir, "rasn-codepilot-direct-" + current_process_id() + ".ini");
}

bool write_direct_cli_config(const std::string &source_path, std::string *direct_path, std::string *error)
{
    std::ifstream input(source_path.c_str(), std::ios::binary);
    if (!input)
    {
        if (error != nullptr)
        {
            *error = "cannot open config file: " + source_path;
        }
        return false;
    }

    const std::string output_path = direct_cli_config_path();
    std::ofstream output(output_path.c_str(), std::ios::binary | std::ios::trunc);
    if (!output)
    {
        if (error != nullptr)
        {
            *error = "cannot create direct CLI config: " + output_path;
        }
        return false;
    }

    bool skipping = false;
    std::string line;
    while (std::getline(input, line))
    {
        const std::string trimmed = trim_ascii(line);
        if (trimmed.size() >= 2 && trimmed[0] == '[' && trimmed[trimmed.size() - 1] == ']')
        {
            const std::string section = trimmed.substr(1, trimmed.size() - 2);
            skipping = direct_cli_skip_config_section(section);
        }
        if (!skipping)
        {
            output << line << "\n";
        }
    }

    if (!output.good())
    {
        if (error != nullptr)
        {
            *error = "failed to write direct CLI config: " + output_path;
        }
        ::dsn::utils::filesystem::remove_path(output_path);
        return false;
    }

    *direct_path = output_path;
    return true;
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
    ::dsn::rasn::debug_log("main entered");

    const int schema_result = maybe_run_direct_schema_command(argc, argv);
    if (schema_result != -1)
    {
        ::dsn::rasn::debug_log("schema command handled before rDSN init");
        return schema_result;
    }

    ::dsn::rasn::debug_log("register rASN apps");
    register_rasn_apps();
    const std::string program = (argc > 0 && argv[0] != nullptr) ? argv[0] : "";
    const bool dsn_mode = argc > 1 && argv[1] != nullptr && std::string(argv[1]) == "--dsn";
    const std::string explicit_dsn_config = (dsn_mode && argc > 2 && argv[2] != nullptr) ? argv[2] : "";

    if (!dsn_mode)
    {
        const std::string config_path = find_config_path(program);
        ::dsn::rasn::debug_log("direct CLI config source: " + config_path);
        std::string direct_config_path_value;
        std::string direct_config_error;
        if (!write_direct_cli_config(config_path, &direct_config_path_value, &direct_config_error))
        {
            std::cerr << direct_config_error << "\n";
            return 1;
        }
        ::dsn::rasn::debug_log("direct CLI filtered config: " + direct_config_path_value);
        const std::string empty_app_list = "__rasn_direct_cli__";
        std::vector<char *> dsn_args;
        dsn_args.push_back(argv[0]);
        dsn_args.push_back(const_cast<char *>(direct_config_path_value.c_str()));
        dsn_args.push_back(const_cast<char *>("-app_list"));
        dsn_args.push_back(const_cast<char *>(empty_app_list.c_str()));
        ::dsn::rasn::debug_log("calling dsn_run for direct CLI runtime");
        ::dsn_run(static_cast<int>(dsn_args.size()), dsn_args.data(), false);
        ::dsn::rasn::debug_log("dsn_run returned for direct CLI runtime");
        if (!::dsn::utils::filesystem::remove_path(direct_config_path_value))
        {
            dwarn("failed to remove direct CLI config file: %s", direct_config_path_value.c_str());
        }

        ::dsn::rasn::debug_log("construct CodePilot CLI");
        ::dsn::rasn::codepilot_cli cli;
        std::vector<std::string> command_args;
        for (int i = 1; i < argc; ++i)
        {
            command_args.push_back(argv[i] == nullptr ? "" : argv[i]);
        }
        ::dsn::rasn::debug_log("enter CodePilot CLI");
        return cli.run(command_args);
    }

    const std::string config_path = explicit_dsn_config.empty() ? find_config_path(program) : explicit_dsn_config;
    ::dsn::rasn::debug_log("calling dsn_run in service mode with config: " + config_path);
    const std::string codepilot_app_list =
        "rasn.registry;rasn.llm.agent;rasn.tool.agent;rasn.state;rasn.coordinator;rasn.workflow;"
        "rasn.observability;rasn.codepilot";
    std::vector<char *> dsn_args;
    dsn_args.push_back(argv[0]);
    dsn_args.push_back(const_cast<char *>(config_path.c_str()));
    dsn_args.push_back(const_cast<char *>("-app_list"));
    dsn_args.push_back(const_cast<char *>(codepilot_app_list.c_str()));
    ::dsn_run(static_cast<int>(dsn_args.size()), dsn_args.data(), true);
    return 0;
}
