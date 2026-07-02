#include "codepilot_app.h"

#include "../../agent_clients.h"
#include "../../agent_registry.h"
#include "../../cli_support.h"
#include "../../metrics.h"
#include "../../observability.h"
#include "../../policy_manager.h"
#include "../../schema_manifest.h"
#include "local_tools.h"

#include <dsn/c/app_model.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>

namespace dsn {
namespace rasn {

namespace {

std::string join_args(const std::vector<std::string> &args, size_t begin)
{
    std::ostringstream oss;
    for (size_t i = begin; i < args.size(); ++i)
    {
        if (i != begin)
        {
            oss << " ";
        }
        oss << args[i];
    }
    return oss.str();
}

std::string quote_command_argument(const std::string &value)
{
    std::string quoted = "\"";
    for (const char c : value)
    {
        if (c == '"' || c == '\\')
        {
            quoted.push_back('\\');
        }
        quoted.push_back(c);
    }
    quoted.push_back('"');
    return quoted;
}

void replace_all(std::string *text, const std::string &needle, const std::string &replacement)
{
    if (needle.empty())
    {
        return;
    }
    size_t pos = 0;
    while ((pos = text->find(needle, pos)) != std::string::npos)
    {
        text->replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
}

std::vector<std::string> default_eval_tasks()
{
    std::vector<std::string> tasks;
    tasks.push_back("Explain the rASN runtime architecture in three concise bullets.");
    tasks.push_back("Plan a safe code change that adds a bounded local tool.");
    tasks.push_back("Diagnose a failed replay trace with one missing tool result.");
    return tasks;
}

bool load_eval_tasks(const std::string &path, std::vector<std::string> *tasks, std::string *error)
{
    if (path.empty())
    {
        *tasks = default_eval_tasks();
        return true;
    }

    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input)
    {
        if (error != nullptr)
        {
            *error = "cannot open eval suite: " + path;
        }
        return false;
    }

    tasks->clear();
    std::string line;
    while (std::getline(input, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        tasks->push_back(line);
    }
    if (tasks->empty())
    {
        if (error != nullptr)
        {
            *error = "eval suite has no tasks: " + path;
        }
        return false;
    }
    return true;
}

std::string capability_names(const std::vector<agent_capability> &capabilities)
{
    if (capabilities.empty())
    {
        return "<none>";
    }

    std::ostringstream oss;
    for (size_t i = 0; i < capabilities.size(); ++i)
    {
        if (i != 0)
        {
            oss << ",";
        }
        oss << capabilities[i].name;
    }
    return oss.str();
}

std::string cli_agent_descriptor_line(const agent_descriptor &descriptor)
{
    std::ostringstream oss;
    oss << descriptor.agent_id
        << " role=" << descriptor.role
        << " app=" << descriptor.app_name
        << " health=" << descriptor.health;
    if (!descriptor.endpoint_uri.empty())
    {
        oss << " endpoint=" << descriptor.endpoint_uri;
    }
    else if (!descriptor.host.empty() || descriptor.port != 0)
    {
        oss << " endpoint=" << descriptor.host << ":" << descriptor.port;
    }
    oss << " capabilities=" << capability_names(descriptor.capabilities);
    return oss.str();
}

void print_agent_descriptors(const std::vector<agent_descriptor> &agents)
{
    for (const agent_descriptor &descriptor : agents)
    {
        std::cout << cli_agent_descriptor_line(descriptor) << "\n";
    }
    std::cout << "agents=" << agents.size() << "\n";
}

class service_graph_lifecycle_scope
{
public:
    explicit service_graph_lifecycle_scope(rasn_service_graph &services) : _services(services) { _services.acquire(); }
    ~service_graph_lifecycle_scope() { _services.release(); }

private:
    rasn_service_graph &_services;
};

void append_readiness_error(std::vector<std::string> *errors, const std::string &component, const std::string &detail)
{
    if (errors != nullptr)
    {
        errors->push_back(component + "=" + detail);
    }
}

std::string readiness_errors_summary(const std::vector<std::string> &errors)
{
    std::ostringstream oss;
    for (size_t i = 0; i < errors.size(); ++i)
    {
        if (i != 0)
        {
            oss << "; ";
        }
        oss << errors[i];
    }
    return oss.str();
}

bool probe_state_service(const rasn_service_graph &services, std::vector<std::string> *errors)
{
    rasn_state_client state(services.state_address());
    state_query_request request;
    request.key_prefix = "__rasn_readiness_probe__";
    ::dsn::error_code err;
    state_response response;
    std::tie(err, response) = state.query_sync(request, std::chrono::milliseconds(500));
    if (err != ::dsn::ERR_OK)
    {
        append_readiness_error(errors, "state", err.to_string());
        return false;
    }
    if (!response.ok)
    {
        append_readiness_error(errors, "state", response.error);
        return false;
    }
    return true;
}

bool probe_registry_service(const rasn_service_graph &services, std::vector<std::string> *errors)
{
    rasn_registry_client registry(services.registry_address());
    ::dsn::error_code err;
    registry_query_response response;
    std::tie(err, response) = registry.list_sync("", std::chrono::milliseconds(500));
    if (err != ::dsn::ERR_OK)
    {
        append_readiness_error(errors, "registry", err.to_string());
        return false;
    }
    if (!response.ok)
    {
        append_readiness_error(errors, "registry", response.error);
        return false;
    }
    return true;
}

bool probe_agent_service(const std::string &label,
                         const ::dsn::rpc_address &address,
                         const std::string &expected_agent_id,
                         std::vector<std::string> *errors)
{
    rasn_agent_client client(address);
    ::dsn::error_code err;
    agent_descriptor descriptor;
    std::tie(err, descriptor) = client.describe_sync("readiness", std::chrono::milliseconds(500));
    if (err != ::dsn::ERR_OK)
    {
        append_readiness_error(errors, label, err.to_string());
        return false;
    }
    if (descriptor.agent_id != expected_agent_id)
    {
        append_readiness_error(errors, label, "unexpected agent id: " + descriptor.agent_id);
        return false;
    }
    return true;
}

bool probe_model_health(const rasn_service_graph &services, std::vector<std::string> *errors)
{
    rasn_llm_agent_client model(services.llm_agent_address());
    ::dsn::error_code err;
    model_gateway_response response;
    std::tie(err, response) = model.health_sync("readiness", std::chrono::milliseconds(500));
    if (err != ::dsn::ERR_OK)
    {
        append_readiness_error(errors, "model.health", err.to_string());
        return false;
    }
    if (!response.ok)
    {
        append_readiness_error(errors, "model.health", response.error);
        return false;
    }
    return true;
}

bool probe_workflow_service(const rasn_service_graph &services, std::vector<std::string> *errors)
{
    rasn_workflow_client workflow(services.workflow_address());
    workflow_source source;
    source.workflow_id = "readiness";
    source.source_name = "<readiness>";
    source.source_text = "task readiness ask \"ping\"\n";
    ::dsn::error_code err;
    workflow_response response;
    std::tie(err, response) = workflow.validate_sync(source, std::chrono::milliseconds(500));
    if (err != ::dsn::ERR_OK)
    {
        append_readiness_error(errors, "workflow", err.to_string());
        return false;
    }
    if (!response.ok)
    {
        append_readiness_error(errors, "workflow", response.error);
        return false;
    }
    return true;
}

bool probe_observability_service(const rasn_service_graph &services, std::vector<std::string> *errors)
{
    rasn_observability_client observability(services.observability_address());
    observability_query_request request;
    request.limit = 1;
    ::dsn::error_code err;
    observability_response response;
    std::tie(err, response) = observability.query_sync(request, std::chrono::milliseconds(500));
    if (err != ::dsn::ERR_OK)
    {
        append_readiness_error(errors, "observability", err.to_string());
        return false;
    }
    if (!response.ok)
    {
        append_readiness_error(errors, "observability", response.error);
        return false;
    }
    return true;
}

bool probe_service_dependencies_once(const rasn_service_graph &services, std::vector<std::string> *errors)
{
    bool ready = true;
    ready = probe_state_service(services, errors) && ready;
    ready = probe_registry_service(services, errors) && ready;
    ready = probe_agent_service("coordinator", services.coordinator_address(), "rasn.coordinator", errors) && ready;
    ready = probe_agent_service("model.agent", services.llm_agent_address(), "rasn.llm.agent", errors) && ready;
    ready = probe_model_health(services, errors) && ready;
    ready = probe_agent_service("tool.agent", services.tool_agent_address(), "rasn.tool.agent", errors) && ready;
    ready = probe_workflow_service(services, errors) && ready;
    ready = probe_observability_service(services, errors) && ready;
    return ready;
}

bool wait_for_service_dependencies(const rasn_service_graph &services, std::string *error)
{
    if (!services.rpc_clients_enabled())
    {
        return true;
    }

    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    std::vector<std::string> last_errors;
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::vector<std::string> errors;
        if (probe_service_dependencies_once(services, &errors))
        {
            return true;
        }
        last_errors.swap(errors);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (error != nullptr)
    {
        *error = "rASN service dependencies are not ready";
        if (!last_errors.empty())
        {
            *error += ": " + readiness_errors_summary(last_errors);
        }
    }
    return false;
}

bool agent_target_id(const std::string &target, std::string *agent_id)
{
    static const std::map<std::string, std::string> targets = {
        {"coordinator", "rasn.coordinator"},
        {"model", "rasn.llm.agent"},
        {"llm", "rasn.llm.agent"},
        {"tool", "rasn.tool.agent"},
    };

    const std::map<std::string, std::string>::const_iterator it = targets.find(target);
    if (it == targets.end())
    {
        return false;
    }
    if (agent_id != nullptr)
    {
        *agent_id = it->second;
    }
    return true;
}

bool agent_target_address(const rasn_service_graph &services,
                          const std::string &target,
                          ::dsn::rpc_address *address)
{
    if (target == "coordinator")
    {
        if (address != nullptr)
        {
            *address = services.coordinator_address();
        }
        return true;
    }
    if (target == "model" || target == "llm")
    {
        if (address != nullptr)
        {
            *address = services.llm_agent_address();
        }
        return true;
    }
    if (target == "tool")
    {
        if (address != nullptr)
        {
            *address = services.tool_agent_address();
        }
        return true;
    }
    return false;
}

bool config_bool_or_default(const std::string &section,
                            const std::string &key,
                            bool default_value,
                            const std::string &description)
{
    return ::dsn_config_get_value_bool(section.c_str(), key.c_str(), default_value, description.c_str());
}

bool policy_config_bool_or_default(const std::string &key, bool default_value, const std::string &description)
{
    const bool compat_value = config_bool_or_default("rasn.codepilot.tools", key, default_value, description);
    return config_bool_or_default("rasn.policy", key, compat_value, description);
}

std::vector<std::string> codepilot_commands()
{
    static const std::vector<std::string> commands = {
        "help",        "-h",       "--help",    "interactive", "repl",     "providers",
        "tools",       "schema",   "topology",  "selftest",    "tool",     "state",
        "registry",    "agentctl", "observe",   "skills",      "skill",    "provider",
        "trace",       "context",  "replay",    "workflow",    "plan",     "agent",
        "eval",        "ask",      "stream",    "simulate",
    };
    return commands;
}

std::string side_effect_approval_config_key(tool_side_effect side_effect)
{
    if (side_effect == tool_side_effect::write)
    {
        return "require_write_approval";
    }
    if (side_effect == tool_side_effect::shell)
    {
        return "require_shell_approval";
    }
    return "";
}

bool parse_tool_request(const std::string &text, std::string *tool_name, std::vector<std::string> *tool_args)
{
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line))
    {
        line = trim(line);
        if (line.find("RASN_TOOL ") != 0)
        {
            continue;
        }

        std::vector<std::string> words = split_words(line.substr(10));
        if (words.empty())
        {
            return false;
        }

        *tool_name = words[0];
        tool_args->assign(words.begin() + 1, words.end());
        return true;
    }
    return false;
}

bool codepilot_approval_answer_is_yes(const std::string &answer)
{
    std::string normalized = trim(answer);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return normalized == "y" || normalized == "yes" || normalized == "approve";
}

std::string state_record_line(const state_record &record)
{
    std::ostringstream oss;
    oss << record.key << " kind=" << record.kind
        << " scope=" << record.scope
        << " sequence=" << record.sequence
        << " value=" << record.value;
    return oss.str();
}

bool read_text_file(const std::string &path, std::string *text, std::string *error)
{
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input)
    {
        if (error != nullptr)
        {
            *error = "cannot open file: " + path;
        }
        return false;
    }

    std::ostringstream content;
    content << input.rdbuf();
    if (text != nullptr)
    {
        *text = content.str();
    }
    return true;
}

bool load_workflow_source(const std::string &path, workflow_source *source, std::string *error)
{
    std::string text;
    if (!read_text_file(path, &text, error))
    {
        return false;
    }
    if (source != nullptr)
    {
        source->workflow_id = path;
        source->source_name = path;
        source->source_text = text;
    }
    return true;
}

void print_workflow_run(const workflow_run_record &run, bool include_result)
{
    std::cout << "run_id=" << (run.run_id.empty() ? "<none>" : run.run_id)
              << " workflow_id=" << run.workflow_id
              << " status=" << run.status
              << " sequence=" << run.sequence << "\n";
    if (!run.plan.empty())
    {
        std::cout << run.plan << "\n";
    }
    if (!run.error.empty())
    {
        std::cout << run.error << "\n";
    }
    if (include_result && !run.result_text.empty())
    {
        std::cout << "\n" << run.result_text;
    }
}

agent_context_entry make_text_context(const std::string &kind,
                                      const std::string &name,
                                      const std::string &value)
{
    agent_context_entry entry;
    entry.kind = kind;
    entry.name = name;
    entry.value = value;
    return entry;
}

std::string codepilot_system_prompt()
{
    return "You are CodePilot, the rASN coding agent CLI. "
           "State assumptions, preserve determinism where possible, and produce actionable output. "
           "When context entries are attached, treat them as source/context supplied by the CLI; "
           "do not claim no source code was provided.";
}

bool codepilot_context_has_workspace_snapshot(const std::vector<std::string> &context)
{
    for (const std::string &entry : context)
    {
        if (entry.find("workspace source snapshot:") != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

std::string codepilot_prompt_with_context_contract(const std::string &prompt,
                                                   const std::vector<std::string> &context)
{
    if (context.empty())
    {
        return prompt;
    }

    std::ostringstream output;
    output << "The CLI has attached " << context.size() << " context "
           << (context.size() == 1 ? "entry" : "entries");
    if (codepilot_context_has_workspace_snapshot(context))
    {
        output << ", including a bounded workspace source snapshot";
    }
    output << ". Treat the attached context as the source/context supplied for this request. "
           << "Base the answer on concrete file names and excerpts from that context. "
           << "Do not say that no source code was provided; if the bounded snapshot is insufficient, "
           << "state what additional files or tool output are needed.\n\nUser request:\n"
           << prompt;
    return output.str();
}

agent_request make_codepilot_model_request(const agent_task &task,
                                           const std::string &request_id,
                                           const std::string &prompt,
                                           const std::string &system_prompt,
                                           const std::vector<std::string> &context,
                                           const std::string &trace_id)
{
    agent_request request;
    request.request_id = request_id;
    request.trace_id = trace_id;
    request.task = task;
    request.capability = "model.complete";
    request.input = codepilot_prompt_with_context_contract(prompt, context);
    if (!system_prompt.empty())
    {
        request.context.push_back(make_text_context("system_prompt", "system", system_prompt));
    }
    for (const std::string &entry : context)
    {
        request.context.push_back(make_text_context("text", "codepilot.context", entry));
    }
    return request;
}

agent_request make_codepilot_tool_request(const agent_task &task,
                                          const std::string &request_id,
                                          const std::string &tool_name,
                                          const std::vector<std::string> &args,
                                          const std::string &trace_id,
                                          const std::vector<std::string> &policy_labels = std::vector<std::string>())
{
    agent_request request;
    request.request_id = request_id;
    request.trace_id = trace_id;
    request.task = task;
    request.capability = "tool.run";
    request.input = tool_name;
    request.policy_labels = policy_labels;
    for (const std::string &arg : args)
    {
        request.context.push_back(make_text_context("argument", "arg", arg));
    }
    return request;
}

std::string agent_error_message(const agent_response &response)
{
    if (!response.error.message.empty())
    {
        return response.error.message;
    }
    if (!response.output.empty())
    {
        return response.output;
    }
    return "agent request failed";
}

} // namespace

codepilot_cli::codepilot_cli() : _services(global_rasn_services())
{
    register_default_tool_provider(&create_codepilot_tool_provider);
    _services.set_tool_provider(create_default_tool_provider());
}

int codepilot_cli::run(const std::vector<std::string> &args)
{
    cli_startup_context startup;
    const cli_workspace_context_options workspace_context_options;
    if (!bootstrap_single_path_argument(args, codepilot_commands(), &startup, 1024u * 1024u, &workspace_context_options))
    {
        std::cout << startup.error << "\n";
        return 1;
    }
    if (startup.matched)
    {
        _context.push_back("workspace: " + startup.workspace_root);
        if (!startup.context_text.empty())
        {
            _context.push_back(startup.context_text);
        }
    }

    service_graph_lifecycle_scope lifecycle(_services);
    if (startup.matched)
    {
        std::cout << startup.message << "\n";
        return repl();
    }

    if (args.empty() || args[0] == "interactive" || args[0] == "repl")
    {
        return repl();
    }

    return run_command(args);
}

int codepilot_cli::repl()
{
    std::cout << "rASN CodePilot prototype\n";
    std::cout << provider_summary() << "\n";
    std::cout << "Type /help for commands. Plain text is sent as an ask prompt.\n";

    std::string line;
    while (true)
    {
        if (_shutdown_requested.load())
        {
            return 0;
        }

        std::cout << "codepilot> ";
        if (!std::getline(std::cin, line))
        {
            return 0;
        }

        line = trim(line);
        if (line.empty())
        {
            continue;
        }

        if (line == "/exit" || line == "/quit")
        {
            return 0;
        }

        if (line[0] == '/')
        {
            std::vector<std::string> args = split_words(line.substr(1));
            const int rc = run_command(args, true);
            if (rc != 0)
            {
                std::cout << "command failed: " << rc << "\n";
            }
            continue;
        }

        ask(line, false);
    }
}

int codepilot_cli::run_command(const std::vector<std::string> &args, bool interactive_mode)
{
    if (args.empty() || args[0] == "help" || args[0] == "-h" || args[0] == "--help")
    {
        print_help(interactive_mode);
        return 0;
    }
    if (args[0] == "interactive" || args[0] == "repl")
    {
        if (interactive_mode)
        {
            std::cout << "already in interactive mode\n";
            return 0;
        }
        return repl();
    }

    if (args[0] == "providers")
    {
        std::cout << describe_provider_environment() << "\n";
        std::cout << provider_summary() << "\n";
        return 0;
    }

    if (args[0] == "tools")
    {
        std::cout << _services.tools_summary() << "\n";
        return 0;
    }

    if (args[0] == "schema")
    {
        if (args.size() > 2)
        {
            std::cout << "usage: schema [text|json|idl|cpp|clients-cpp|ts|clients-ts|py|clients-py]\n";
            return 1;
        }
        if (args.size() == 2)
        {
            if (args[1] == "json" || args[1] == "--json")
            {
                std::cout << rasn_schema_manifest_json();
                return 0;
            }
            if (args[1] == "idl" || args[1] == "--idl")
            {
                std::cout << rasn_schema_manifest_idl();
                return 0;
            }
            if (args[1] == "cpp" || args[1] == "c++" || args[1] == "--cpp")
            {
                std::cout << rasn_schema_manifest_cpp_header();
                return 0;
            }
            if (args[1] == "clients-cpp" || args[1] == "client-cpp" || args[1] == "cpp-clients" ||
                args[1] == "--clients-cpp")
            {
                std::cout << rasn_schema_manifest_cpp_clients();
                return 0;
            }
            if (args[1] == "ts" || args[1] == "typescript" || args[1] == "--ts")
            {
                std::cout << rasn_schema_manifest_typescript();
                return 0;
            }
            if (args[1] == "clients-ts" || args[1] == "client-ts" || args[1] == "ts-clients" ||
                args[1] == "--clients-ts")
            {
                std::cout << rasn_schema_manifest_typescript_clients();
                return 0;
            }
            if (args[1] == "py" || args[1] == "python" || args[1] == "--py")
            {
                std::cout << rasn_schema_manifest_python();
                return 0;
            }
            if (args[1] == "clients-py" || args[1] == "client-py" || args[1] == "py-clients" ||
                args[1] == "python-clients" || args[1] == "--clients-py")
            {
                std::cout << rasn_schema_manifest_python_clients();
                return 0;
            }
            if (args[1] != "text" && args[1] != "--text")
            {
                std::cout << "usage: schema [text|json|idl|cpp|clients-cpp|ts|clients-ts|py|clients-py]\n";
                return 1;
            }
        }
        std::cout << rasn_schema_manifest_text();
        return 0;
    }

    if (args[0] == "topology")
    {
        std::cout << _services.topology() << "\n";
        return 0;
    }

    if (args[0] == "selftest")
    {
        return run_selftest(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "tool")
    {
        return run_tool(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "state")
    {
        return run_state(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "registry")
    {
        return run_registry(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "agentctl")
    {
        return run_agent_control(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "observe")
    {
        return run_observe(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "skills")
    {
        std::cout << describe_codepilot_skills();
        return 0;
    }

    if (args[0] == "skill")
    {
        return run_skill(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "provider")
    {
        if (args.size() < 2)
        {
            std::cout << provider_summary() << "\n";
            return 0;
        }
        const int rc = set_provider(args[1]);
        if (rc != 0 || args.size() == 2)
        {
            return rc;
        }
        return run_command(std::vector<std::string>(args.begin() + 2, args.end()), interactive_mode);
    }

    if (args[0] == "trace")
    {
        if (args.size() < 2)
        {
            std::cout << "trace file: " << (_services.runtime().trace_file().empty() ? "<memory only>" : _services.runtime().trace_file()) << "\n";
            return 0;
        }
        _services.runtime().set_trace_file(args[1]);
        std::cout << "trace file: " << args[1] << "\n";
        if (args.size() == 2)
        {
            return 0;
        }
        return run_command(std::vector<std::string>(args.begin() + 2, args.end()), interactive_mode);
    }

    if (args[0] == "context")
    {
        if (args.size() < 2)
        {
            std::cout << "context files loaded: " << _context.size() << "\n";
            return 0;
        }
        std::string error;
        if (!load_context_file(args[1], &error))
        {
            std::cout << error << "\n";
            return 1;
        }
        std::cout << "loaded context file: " << args[1] << "\n";
        return 0;
    }

    if (args[0] == "replay")
    {
        if (args.size() < 2)
        {
            std::cout << "usage: replay <trace-jsonl>\n";
            return 1;
        }
        const int rc = enable_replay(args[1]);
        if (rc != 0 || args.size() == 2)
        {
            return rc;
        }
        return run_command(std::vector<std::string>(args.begin() + 2, args.end()), interactive_mode);
    }

    if (args[0] == "workflow")
    {
        if (args.size() < 2)
        {
            std::cout << "usage: workflow [validate|compile|start|resume|query|cancel|nodes] <workflow-file-or-run-id>\n";
            return 1;
        }

        if (args[1] == "validate" || args[1] == "compile")
        {
            if (args.size() != 3)
            {
                std::cout << "usage: workflow " << args[1] << " <workflow-file>\n";
                return 1;
            }
            workflow_source source;
            std::string error;
            if (!load_workflow_source(args[2], &source, &error))
            {
                std::cout << error << "\n";
                return 1;
            }
            const workflow_response response =
                args[1] == "validate" ? _services.validate_workflow(source) : _services.compile_workflow(source);
            if (!response.ok)
            {
                std::cout << response.error << "\n";
                return 1;
            }
            print_workflow_run(response.run, false);
            return 0;
        }

        if (args[1] == "query" || args[1] == "cancel")
        {
            if (args.size() != 3)
            {
                std::cout << "usage: workflow " << args[1] << " <run-id>\n";
                return 1;
            }
            workflow_run_query query;
            query.run_id = args[2];
            workflow_response response =
                args[1] == "query" ? _services.query_workflow(query) : _services.cancel_workflow(query);
            if (!response.ok && args[1] == "query")
            {
                state_checkpoint_request recover_request;
                const state_response recovered = _services.recover_state(recover_request);
                if (recovered.ok)
                {
                    response = _services.query_workflow(query);
                }
            }
            if (!response.ok)
            {
                std::cout << response.error << "\n";
                return 1;
            }
            print_workflow_run(response.run, true);
            return 0;
        }

        if (args[1] == "nodes")
        {
            if (args.size() != 3)
            {
                std::cout << "usage: workflow nodes <run-id>\n";
                return 1;
            }
            state_query_request request;
            request.key_prefix = "workflow-node/" + args[2];
            state_response response = _services.query_state(request);
            if (response.ok && response.records.empty())
            {
                state_checkpoint_request recover_request;
                const state_response recovered = _services.recover_state(recover_request);
                if (recovered.ok)
                {
                    response = _services.query_state(request);
                }
            }
            if (!response.ok)
            {
                std::cout << response.error << "\n";
                return 1;
            }
            for (const state_record &record : response.records)
            {
                std::cout << state_record_line(record) << "\n";
            }
            std::cout << "nodes=" << response.records.size()
                      << " last_sequence=" << response.last_sequence << "\n";
            return 0;
        }

        if (args[1] == "start" || args[1] == "run")
        {
            if (args.size() < 3 || args.size() > 4)
            {
                std::cout << "usage: workflow " << args[1] << " <workflow-file> [run-id]\n";
                return 1;
            }
            return run_workflow(args[2], args.size() == 4 ? args[3] : "");
        }

        if (args[1] == "resume")
        {
            if (args.size() != 4)
            {
                std::cout << "usage: workflow resume <workflow-file> <run-id>\n";
                return 1;
            }
            return run_workflow(args[2], args[3], true);
        }

        return run_workflow(args[1]);
    }

    if (args[0] == "plan")
    {
        if (args.size() < 2)
        {
            std::cout << "usage: plan <goal>\n";
            return 1;
        }
        return ask(join_args(args, 1), true);
    }

    if (args[0] == "agent")
    {
        if (args.size() < 2)
        {
            std::cout << "usage: agent <prompt>\n";
            return 1;
        }
        return agent(join_args(args, 1));
    }

    if (args[0] == "eval")
    {
        return run_eval(args);
    }

    if (args[0] == "ask")
    {
        if (args.size() < 2)
        {
            std::cout << "usage: ask <prompt>\n";
            return 1;
        }
        return ask(join_args(args, 1), false);
    }

    if (args[0] == "stream")
    {
        if (args.size() < 2)
        {
            std::cout << "usage: stream <prompt>\n";
            return 1;
        }
        return stream(join_args(args, 1));
    }

    if (args[0] == "simulate")
    {
        set_provider("simulator");
        return ask(args.size() > 1 ? join_args(args, 1) : "Simulate a coding assistant response.", false);
    }

    return ask(join_args(args, 0), false);
}

int codepilot_cli::ask(const std::string &prompt, bool planning_mode)
{
    agent_task task;
    task.id = make_trace_id();
    task.name = planning_mode ? "codepilot.plan" : "codepilot.ask";
    task.input = prompt;

    _services.runtime().begin_task(task);

    const std::string user_prompt = planning_mode ? "Create a concise implementation plan for: " + prompt : prompt;
    const agent_request request = make_codepilot_model_request(task,
                                                               task.id + "/model",
                                                               user_prompt,
                                                               codepilot_system_prompt(),
                                                               _context,
                                                               _services.runtime().trace_id());
    const agent_response response = _services.invoke(request);
    if (!response.ok)
    {
        _services.runtime().finish_task(task, "failed");
        std::cout << agent_error_message(response) << "\n";
        return 1;
    }

    _services.runtime().finish_task(task, "ok");
    std::cout << response.output << "\n";
    return 0;
}

int codepilot_cli::stream(const std::string &prompt)
{
    agent_task task;
    task.id = make_trace_id();
    task.name = "codepilot.stream";
    task.input = prompt;

    _services.runtime().begin_task(task);

    const agent_request generic = make_codepilot_model_request(task,
                                                               task.id + "/model",
                                                               prompt,
                                                               codepilot_system_prompt(),
                                                               _context,
                                                               _services.runtime().trace_id());
    const agent_completion_request completion = make_completion_request_from_agent(generic);
    size_t chunks = 0;
    const llm_response response = _services.complete_streaming(completion, [&chunks](const std::string &chunk) {
        ++chunks;
        std::cout << chunk;
        std::cout.flush();
    });
    if (!response.ok)
    {
        _services.runtime().finish_task(task, "failed");
        std::cout << "\n" << response.error << "\n";
        return 1;
    }

    _services.runtime().finish_task(task, "ok");
    if (chunks == 0)
    {
        std::cout << response.text;
    }
    std::cout << "\n";
    return 0;
}

int codepilot_cli::agent(const std::string &prompt)
{
    agent_task task;
    task.id = make_trace_id();
    task.name = "codepilot.agent";
    task.input = prompt;
    _services.runtime().begin_task(task);

    std::vector<std::string> context = _context;
    std::string conversation = prompt;

    for (int step = 0; step < 4; ++step)
    {
        const std::string system_prompt =
            codepilot_system_prompt() +
            " You may request exactly one tool call by writing a line: RASN_TOOL <tool> <args>. "
            "Use tools only when needed, then produce the final answer without RASN_TOOL. " +
            _services.tools_summary();
        const agent_request request = make_codepilot_model_request(task,
                                                                   task.id + "/model/" + std::to_string(step),
                                                                   conversation,
                                                                   system_prompt,
                                                                   context,
                                                                   _services.runtime().trace_id());
        const agent_response response = _services.invoke(request);
        if (!response.ok)
        {
            _services.runtime().finish_task(task, "failed");
            std::cout << agent_error_message(response) << "\n";
            return 1;
        }

        std::string tool_name;
        std::vector<std::string> tool_args;
        if (!parse_tool_request(response.output, &tool_name, &tool_args))
        {
            _services.runtime().finish_task(task, "ok");
            std::cout << response.output << "\n";
            return 0;
        }

        std::vector<std::string> policy_labels;
        if (!approve_tool_invocation(tool_name, tool_args, false, &policy_labels))
        {
            _services.runtime().record_failure(
                task, "policy", "tool_approval_denied", "tool denied by user approval", false, "codepilot.cli");
            _services.runtime().finish_task(task, "approval-denied");
            std::cout << "tool denied by user approval\n";
            return 1;
        }

        const agent_request tool_request = make_codepilot_tool_request(task,
                                                                       task.id + "/tool/" + std::to_string(step),
                                                                       tool_name,
                                                                       tool_args,
                                                                       _services.runtime().trace_id(),
                                                                       policy_labels);
        const tool_result tool = make_tool_result_from_agent(_services.invoke(tool_request));
        std::ostringstream tool_context;
        tool_context << "Tool " << tool_name << (tool.ok ? " succeeded" : " failed") << ":\n"
                     << (tool.ok ? tool.output : tool.error);
        if (!tool.output.empty() && !tool.ok)
        {
            tool_context << "\nstdout/stderr:\n" << tool.output;
        }
        context.push_back(tool_context.str());
        conversation = prompt + "\n\nThe requested tool was executed. Continue with the answer.";
    }

    _services.runtime().finish_task(task, "tool-limit");
    std::cout << "agent stopped after reaching the tool-call limit\n";
    return 1;
}

int codepilot_cli::run_eval(const std::vector<std::string> &args)
{
    bool external = false;
    std::string command_template;
    std::string suite_path;
    if (args.size() > 1 && args[1] == "external")
    {
        if (args.size() < 3 || args.size() > 4)
        {
            std::cout << "usage: eval [suite-file]\n"
                      << "       eval external <command-template> [suite-file]\n";
            return 1;
        }
        external = true;
        command_template = args[2];
        suite_path = args.size() == 4 ? args[3] : "";
    }
    else
    {
        if (args.size() > 2)
        {
            std::cout << "usage: eval [suite-file]\n";
            return 1;
        }
        suite_path = args.size() == 2 ? args[1] : "";
    }

    std::vector<std::string> tasks;
    std::string error;
    if (!load_eval_tasks(suite_path, &tasks, &error))
    {
        std::cout << error << "\n";
        return 1;
    }

    std::cout << "target=" << (external ? "external" : "codepilot")
              << " tasks=" << tasks.size() << "\n";
    uint64_t total_latency_ms = 0;
    uint32_t failures = 0;
    for (size_t i = 0; i < tasks.size(); ++i)
    {
        const std::string &prompt = tasks[i];
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        bool ok = false;
        std::string output;
        std::string eval_error;

        if (external)
        {
            std::string command = command_template;
            const std::string quoted_prompt = quote_command_argument(prompt);
            if (command.find("{prompt}") != std::string::npos)
            {
                replace_all(&command, "{prompt}", quoted_prompt);
            }
            else
            {
                command += " " + quoted_prompt;
            }
            const tool_result result = codepilot_run_shell_command(command, 300000);
            ok = result.ok;
            output = result.output;
            eval_error = result.error;
        }
        else
        {
            agent_task task;
            task.id = make_trace_id();
            task.name = "codepilot.eval";
            task.input = prompt;
            _services.runtime().begin_task(task);
            const agent_request request = make_codepilot_model_request(task,
                                                                       task.id + "/model",
                                                                       prompt,
                                                                       codepilot_system_prompt(),
                                                                       _context,
                                                                       _services.runtime().trace_id());
            const agent_response response = _services.invoke(request);
            ok = response.ok;
            output = response.output;
            eval_error = response.ok ? "" : agent_error_message(response);
            _services.runtime().finish_task(task, ok ? "ok" : "failed");
        }

        const uint64_t latency_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
        total_latency_ms += latency_ms;
        if (!ok)
        {
            ++failures;
        }
        std::cout << "task=" << (i + 1)
                  << " ok=" << (ok ? "true" : "false")
                  << " latency_ms=" << latency_ms
                  << " output_bytes=" << output.size();
        if (!ok)
        {
            std::cout << " error=\"" << eval_error << "\"";
        }
        std::cout << "\n";
    }
    std::cout << "eval_summary tasks=" << tasks.size()
              << " failures=" << failures
              << " total_latency_ms=" << total_latency_ms << "\n";
    return failures == 0 ? 0 : 1;
}

int codepilot_cli::run_tool(const std::vector<std::string> &args)
{
    if (args.empty())
    {
        std::cout << _services.tools_summary() << "\n";
        return 0;
    }

    std::vector<std::string> tool_args = args;
    bool explicit_approval = false;
    if (!tool_args.empty() && (tool_args[0] == "--yes" || tool_args[0] == "--approve"))
    {
        explicit_approval = true;
        tool_args.erase(tool_args.begin());
    }
    if (tool_args.empty())
    {
        std::cout << "usage: tool [--yes] <name> <args>\n";
        return 1;
    }

    agent_task task;
    task.id = make_trace_id();
    task.name = "codepilot.tool";
    task.input = join_args(tool_args, 0);
    _services.runtime().begin_task(task);

    const std::string tool_name = tool_args[0];
    const std::vector<std::string> invocation_args(tool_args.begin() + 1, tool_args.end());
    std::vector<std::string> policy_labels;
    if (!approve_tool_invocation(tool_name, invocation_args, explicit_approval, &policy_labels))
    {
        _services.runtime().record_failure(
            task, "policy", "tool_approval_denied", "tool denied by user approval", false, "codepilot.cli");
        _services.runtime().finish_task(task, "approval-denied");
        std::cout << "tool denied by user approval\n";
        return 1;
    }

    const agent_request request = make_codepilot_tool_request(task,
                                                              task.id + "/tool",
                                                              tool_name,
                                                              invocation_args,
                                                              _services.runtime().trace_id(),
                                                              policy_labels);
    const tool_result result = make_tool_result_from_agent(_services.invoke(request));
    _services.runtime().finish_task(task, result.ok ? "ok" : "failed");
    std::cout << (result.ok ? result.output : result.error) << "\n";
    if (!result.ok && !result.output.empty())
    {
        std::cout << result.output << "\n";
    }
    return result.ok ? 0 : 1;
}

bool codepilot_cli::approve_tool_invocation(const std::string &tool_name,
                                            const std::vector<std::string> &args,
                                            bool explicit_approval,
                                            std::vector<std::string> *policy_labels) const
{
    const tool_side_effect side_effect = classify_tool_side_effect(tool_name);
    if (side_effect != tool_side_effect::write && side_effect != tool_side_effect::shell)
    {
        return true;
    }

    const std::string config_key = side_effect_approval_config_key(side_effect);
    const bool require_approval = config_key.empty()
                                      ? false
                                      : policy_config_bool_or_default(
                                            config_key, true, "CodePilot side-effect approval setting");
    if (explicit_approval)
    {
        if (policy_labels != nullptr)
        {
            policy_labels->push_back(human_approval_policy_label(side_effect));
        }
        return true;
    }
    if (!require_approval)
    {
        return true;
    }

    std::cout << "Approval required for " << to_string(side_effect) << " tool '" << tool_name << "'";
    if (!args.empty())
    {
        std::cout << " target '" << args[0] << "'";
    }
    std::cout << ". Type 'yes' to continue: ";
    std::string answer;
    if (!std::getline(std::cin, answer) || !codepilot_approval_answer_is_yes(answer))
    {
        return false;
    }

    if (policy_labels != nullptr)
    {
        policy_labels->push_back(human_approval_policy_label(side_effect));
    }
    return true;
}

int codepilot_cli::run_state(const std::vector<std::string> &args)
{
    if (args.empty())
    {
        std::cout << "usage: state <put|get|query|checkpoint|recover> ...\n";
        return 1;
    }

    state_response response;
    if (args[0] == "put")
    {
        if (args.size() < 3)
        {
            std::cout << "usage: state put <key> <value>\n";
            return 1;
        }
        state_record record;
        record.key = args[1];
        record.kind = "memory";
        record.scope = "codepilot";
        record.value = join_args(args, 2);
        response = _services.put_state(record);
        if (response.ok)
        {
            std::cout << "stored " << state_record_line(response.record) << "\n";
        }
    }
    else if (args[0] == "get")
    {
        if (args.size() != 2)
        {
            std::cout << "usage: state get <key>\n";
            return 1;
        }
        state_key_request request;
        request.key = args[1];
        response = _services.get_state(request);
        if (response.ok)
        {
            std::cout << state_record_line(response.record) << "\n";
        }
    }
    else if (args[0] == "query")
    {
        state_query_request request;
        request.key_prefix = args.size() > 1 ? args[1] : "";
        response = _services.query_state(request);
        if (response.ok)
        {
            for (const state_record &record : response.records)
            {
                std::cout << state_record_line(record) << "\n";
            }
            std::cout << "records=" << response.records.size()
                      << " last_sequence=" << response.last_sequence << "\n";
        }
    }
    else if (args[0] == "checkpoint")
    {
        state_checkpoint_request request;
        if (args.size() > 1)
        {
            request.path = args[1];
        }
        response = _services.checkpoint_state(request);
        if (response.ok)
        {
            std::cout << "checkpointed records=" << response.records.size()
                      << " last_sequence=" << response.last_sequence << "\n";
        }
    }
    else if (args[0] == "recover")
    {
        state_checkpoint_request request;
        if (args.size() > 1)
        {
            request.path = args[1];
        }
        response = _services.recover_state(request);
        if (response.ok)
        {
            std::cout << "recovered records=" << response.records.size()
                      << " last_sequence=" << response.last_sequence << "\n";
        }
    }
    else
    {
        std::cout << "unknown state command: " << args[0] << "\n";
        return 1;
    }

    if (!response.ok)
    {
        std::cout << response.error << "\n";
        return 1;
    }
    return 0;
}

int codepilot_cli::run_registry(const std::vector<std::string> &args)
{
    if (_services.rpc_clients_enabled())
    {
        rasn_registry_client client(_services.registry_address());
        if (args.empty() || args[0] == "list")
        {
            const std::pair<::dsn::error_code, registry_query_response> result =
                client.list_sync("registry", default_rpc_timeout());
            if (result.first != ::dsn::ERR_OK)
            {
                std::cout << result.first.to_string() << "\n";
                return 1;
            }
            if (!result.second.ok)
            {
                std::cout << result.second.error << "\n";
                return 1;
            }
            print_agent_descriptors(result.second.agents);
            return 0;
        }

        if (args[0] == "query")
        {
            if (args.size() != 2)
            {
                std::cout << "usage: registry query <capability>\n";
                return 1;
            }
            registry_query_request request;
            request.capability = args[1];
            request.healthy_only = true;
            const std::pair<::dsn::error_code, registry_query_response> result =
                client.query_sync(request, default_rpc_timeout());
            if (result.first != ::dsn::ERR_OK)
            {
                std::cout << result.first.to_string() << "\n";
                return 1;
            }
            if (!result.second.ok)
            {
                std::cout << result.second.error << "\n";
                return 1;
            }
            print_agent_descriptors(result.second.agents);
            return 0;
        }

        if (args[0] == "get")
        {
            if (args.size() != 2)
            {
                std::cout << "usage: registry get <agent-id>\n";
                return 1;
            }
            const std::pair<::dsn::error_code, registry_query_response> result =
                client.list_sync("registry", default_rpc_timeout());
            if (result.first != ::dsn::ERR_OK)
            {
                std::cout << result.first.to_string() << "\n";
                return 1;
            }
            if (!result.second.ok)
            {
                std::cout << result.second.error << "\n";
                return 1;
            }
            for (const agent_descriptor &descriptor : result.second.agents)
            {
                if (descriptor.agent_id == args[1])
                {
                    std::cout << cli_agent_descriptor_line(descriptor) << "\n";
                    return 0;
                }
            }
            std::cout << "agent not found: " << args[1] << "\n";
            return 1;
        }

        std::cout << "usage: registry [list|get|query] ...\n";
        return 1;
    }

    if (args.empty() || args[0] == "list")
    {
        const std::vector<agent_descriptor> agents = global_agent_registry().list_agents(false);
        print_agent_descriptors(agents);
        return 0;
    }

    if (args[0] == "query")
    {
        if (args.size() != 2)
        {
            std::cout << "usage: registry query <capability>\n";
            return 1;
        }
        const std::vector<agent_descriptor> agents = global_agent_registry().query_by_capability(args[1], true);
        print_agent_descriptors(agents);
        return 0;
    }

    if (args[0] == "get")
    {
        if (args.size() != 2)
        {
            std::cout << "usage: registry get <agent-id>\n";
            return 1;
        }
        agent_descriptor descriptor;
        if (!global_agent_registry().find_agent(args[1], &descriptor))
        {
            std::cout << "agent not found: " << args[1] << "\n";
            return 1;
        }
        std::cout << cli_agent_descriptor_line(descriptor) << "\n";
        return 0;
    }

    std::cout << "usage: registry [list|get|query] ...\n";
    return 1;
}

int codepilot_cli::run_agent_control(const std::vector<std::string> &args)
{
    if (args.size() < 2)
    {
        std::cout << "usage: agentctl <describe|heartbeat|query|cancel> <coordinator|model|tool>\n";
        return 1;
    }

    std::string expected_agent_id;
    if (!agent_target_id(args[1], &expected_agent_id))
    {
        std::cout << "unknown agent target: " << args[1] << "\n";
        return 1;
    }

    if (!_services.rpc_clients_enabled())
    {
        if (args[0] == "describe" || args[0] == "heartbeat" || args[0] == "query")
        {
            agent_descriptor descriptor;
            if (!global_agent_registry().find_agent(expected_agent_id, &descriptor))
            {
                std::cout << "agent not found: " << expected_agent_id << "\n";
                return 1;
            }
            std::cout << cli_agent_descriptor_line(descriptor) << "\n";
            return 0;
        }
        std::cout << "agentctl cancel requires rDSN RPC service mode\n";
        return 1;
    }

    ::dsn::rpc_address address;
    if (!agent_target_address(_services, args[1], &address))
    {
        std::cout << "unknown agent target: " << args[1] << "\n";
        return 1;
    }

    rasn_agent_client client(address);
    if (args[0] == "describe")
    {
        const std::pair<::dsn::error_code, agent_descriptor> result =
            client.describe_sync("agentctl", default_rpc_timeout());
        if (result.first != ::dsn::ERR_OK)
        {
            std::cout << result.first.to_string() << "\n";
            return 1;
        }
        std::cout << cli_agent_descriptor_line(result.second) << "\n";
        return 0;
    }
    if (args[0] == "heartbeat")
    {
        const std::pair<::dsn::error_code, agent_descriptor> result =
            client.heartbeat_sync("agentctl", default_rpc_timeout());
        if (result.first != ::dsn::ERR_OK)
        {
            std::cout << result.first.to_string() << "\n";
            return 1;
        }
        std::cout << cli_agent_descriptor_line(result.second) << "\n";
        return 0;
    }
    if (args[0] == "query")
    {
        const std::pair<::dsn::error_code, agent_descriptor> result =
            client.query_sync("agentctl", default_rpc_timeout());
        if (result.first != ::dsn::ERR_OK)
        {
            std::cout << result.first.to_string() << "\n";
            return 1;
        }
        std::cout << cli_agent_descriptor_line(result.second) << "\n";
        return 0;
    }
    if (args[0] == "cancel")
    {
        agent_request request;
        request.request_id = args.size() > 2 ? args[2] : make_trace_id();
        request.trace_id = request.request_id;
        request.task.id = request.request_id;
        request.task.name = "agentctl.cancel";
        request.capability = "agent.cancel";
        const std::pair<::dsn::error_code, agent_response> result =
            client.cancel_sync(request, default_rpc_timeout());
        if (result.first != ::dsn::ERR_OK)
        {
            std::cout << result.first.to_string() << "\n";
            return 1;
        }
        std::cout << "ok=" << (result.second.ok ? "true" : "false");
        if (!result.second.error.code.empty())
        {
            std::cout << " error=" << result.second.error.code
                      << " message=" << result.second.error.message;
        }
        std::cout << "\n";
        return result.second.ok ? 0 : 2;
    }

    std::cout << "usage: agentctl <describe|heartbeat|query|cancel> <coordinator|model|tool>\n";
    return 1;
}

int codepilot_cli::run_observe(const std::vector<std::string> &args)
{
    if (args.empty())
    {
        std::cout << "usage: observe <events|failures|timeline|diagnose|replay|metrics|resilience|snapshot> ...\n";
        return 1;
    }

    if (args[0] == "replay")
    {
        if (args.size() != 2)
        {
            std::cout << "usage: observe replay <trace-jsonl>\n";
            return 1;
        }
        return enable_replay(args[1]);
    }

    if (args[0] == "metrics")
    {
        const std::string format = args.size() > 1 ? args[1] : "text";
        const metrics_snapshot snapshot = _services.runtime_metrics();
        if (format == "prometheus" || format == "prom")
        {
            std::cout << snapshot.to_prometheus();
        }
        else if (format == "json")
        {
            std::cout << snapshot.to_json() << "\n";
        }
        else if (format == "text")
        {
            std::cout << snapshot.to_text();
        }
        else
        {
            std::cout << "usage: observe metrics [text|prometheus|json]\n";
            return 1;
        }
        return 0;
    }

    if (args[0] == "resilience")
    {
        std::cout << _services.resilience_report() << "\n";
        return 0;
    }

    observability_response response;
    if (args[0] == "timeline" || args[0] == "diagnose")
    {
        observability_query_request request;
        request.limit = 0;
        if (args.size() > 1)
        {
            request.trace_id = args[1];
        }
        response = _services.query_events(request);
        if (response.ok)
        {
            std::cout << (args[0] == "timeline" ? format_observability_timeline(response.events, request.trace_id)
                                                : diagnose_observability_events(response.events, request.trace_id));
        }
    }
    else if (args[0] == "events")
    {
        observability_query_request request;
        request.limit = 100;
        if (args.size() > 1)
        {
            request.kind = args[1];
        }
        response = _services.query_events(request);
        if (response.ok)
        {
            std::cout << "events=" << response.events.size()
                      << " last_sequence=" << response.last_sequence
                      << (response.truncated ? " [truncated]" : "") << "\n";
            for (const runtime_event &event : response.events)
            {
                std::cout << format_observability_event(event) << "\n";
            }
        }
    }
    else if (args[0] == "failures")
    {
        observability_query_request request;
        request.limit = 100;
        response = _services.query_failures(request);
        if (response.ok)
        {
            std::cout << "failures=" << response.failures.size()
                      << " last_sequence=" << response.last_sequence
                      << (response.truncated ? " [truncated]" : "") << "\n";
            for (const failure_record &failure : response.failures)
            {
                std::cout << format_failure_record(failure) << "\n";
            }
        }
    }
    else if (args[0] == "snapshot")
    {
        response = _services.observability_snapshot();
        if (response.ok)
        {
            std::cout << "events=" << response.events.size()
                      << " failures=" << response.failures.size()
                      << " last_sequence=" << response.last_sequence
                      << (response.truncated ? " [truncated]" : "") << "\n";
        }
    }
    else
    {
        std::cout << "unknown observe command: " << args[0] << "\n";
        return 1;
    }

    if (!response.ok)
    {
        std::cout << response.error << "\n";
        return 1;
    }
    return 0;
}

int codepilot_cli::run_skill(const std::vector<std::string> &args)
{
    if (args.empty())
    {
        std::cout << describe_codepilot_skills();
        return 0;
    }

    codepilot_skill_metadata skill;
    if (!find_codepilot_skill(args[0], &skill))
    {
        std::cout << "unknown skill: " << args[0] << "\n" << describe_codepilot_skills();
        return 1;
    }

    if (args.size() == 1)
    {
        std::cout << skill.name << ": " << skill.description << "\n\n" << skill.prompt << "\n";
        return 0;
    }

    return ask(skill.prompt + "\n\nUser task: " + join_args(args, 1), false);
}

int codepilot_cli::run_selftest(const std::vector<std::string> &args)
{
    if (args.size() > 1)
    {
        std::cout << "usage: selftest [checkpoint-path]\n";
        return 1;
    }

    const std::string mode = _services.rpc_clients_enabled() ? "rDSN RPC" : "inline";
    const std::string checkpoint_path = args.empty() ? "" : args[0];
    std::cout << "rASN self-test mode=" << mode << "\n";

    bool ok = true;
    const auto check = [&ok](bool condition, const std::string &label, const std::string &detail) {
        if (condition)
        {
            std::cout << "[PASS] " << label;
            if (!detail.empty())
            {
                std::cout << " - " << detail;
            }
            std::cout << "\n";
            return;
        }

        ok = false;
        std::cout << "[FAIL] " << label;
        if (!detail.empty())
        {
            std::cout << " - " << detail;
        }
        std::cout << "\n";
    };

    const model_gateway_response health = _services.model_health();
    check(health.ok, "model gateway health", health.ok ? health.provider.provider + "/" + health.provider.model : health.error);

    agent_completion_request completion;
    completion.task.id = make_trace_id();
    completion.task.name = "selftest.model";
    completion.task.input = "selftest";
    completion.system_prompt = "You are validating the rASN service graph. Reply briefly.";
    completion.user_prompt = "Return a short rASN self-test response.";
    const llm_response model = _services.complete(completion);
    check(model.ok && !model.text.empty(), "generic model invoke", model.ok ? model.text.substr(0, 120) : model.error);

    const std::string tools = _services.tools_summary();
    check(!tools.empty() && tools.find("Available tools") != std::string::npos, "generic tool describe", tools.substr(0, 120));

    state_record record;
    record.key = "selftest/" + completion.task.id;
    record.kind = "selftest";
    record.scope = "rasn.selftest";
    record.value = "mode=" + mode;
    const state_response put = _services.put_state(record);
    check(put.ok, "state put", put.ok ? state_record_line(put.record) : put.error);

    state_key_request get_request;
    get_request.key = record.key;
    const state_response get = _services.get_state(get_request);
    check(get.ok && get.record.value == record.value, "state get", get.ok ? state_record_line(get.record) : get.error);

    if (_services.rpc_clients_enabled())
    {
        rasn_agent_client llm_client(_services.llm_agent_address());
        const std::pair<::dsn::error_code, agent_descriptor> heartbeat =
            llm_client.heartbeat_sync("selftest", std::chrono::milliseconds(5000));
        check(heartbeat.first == ::dsn::ERR_OK && heartbeat.second.agent_id == "rasn.llm.agent",
              "agent heartbeat RPC",
              heartbeat.first == ::dsn::ERR_OK ? heartbeat.second.agent_id : heartbeat.first.to_string());

        const std::pair<::dsn::error_code, agent_descriptor> query =
            llm_client.query_sync("selftest", std::chrono::milliseconds(5000));
        check(query.first == ::dsn::ERR_OK && query.second.agent_id == "rasn.llm.agent",
              "agent query RPC",
              query.first == ::dsn::ERR_OK ? query.second.agent_id : query.first.to_string());

        agent_request cancel_request;
        cancel_request.request_id = "cancel-" + completion.task.id;
        cancel_request.trace_id = completion.task.id;
        cancel_request.task.id = completion.task.id;
        const std::pair<::dsn::error_code, agent_response> cancel =
            llm_client.cancel_sync(cancel_request, std::chrono::milliseconds(5000));
        check(cancel.first == ::dsn::ERR_OK && !cancel.second.ok &&
                  cancel.second.error.code == "cancel_not_found",
              "agent cancel RPC",
              cancel.first == ::dsn::ERR_OK ? cancel.second.error.code : cancel.first.to_string());
    }
    else
    {
        check(true, "agent control RPC", "skipped in inline mode");
    }

    state_checkpoint_request checkpoint;
    checkpoint.path = checkpoint_path;
    const state_response checkpointed = _services.checkpoint_state(checkpoint);
    check(checkpointed.ok, "state checkpoint", checkpointed.ok ? "records=" + std::to_string(checkpointed.records.size()) : checkpointed.error);

    workflow_source source;
    source.workflow_id = "rasn-selftest";
    source.source_name = "<selftest>";
    source.source_text =
        "task inspect tool \"list .\" capability tool.run policy read_only budget_ms 5000 latency_ms 50 cost_hint 1 reliability 99 state selftest/inspect\n"
        "task summarize ask \"Summarize the rASN self-test result\" after inspect capability model.complete policy read_only budget_ms 10000 latency_ms 250 cost_hint 2 reliability 95 state selftest/summary\n";
    const workflow_response compiled = _services.compile_workflow(source);
    check(compiled.ok && !compiled.run.plan.empty(), "workflow compile", compiled.ok ? "plan generated" : compiled.error);

    workflow_start_request start;
    start.run_id = "selftest-" + completion.task.id;
    start.source = source;
    const workflow_response workflow = _services.start_workflow(start);
    check(workflow.ok && workflow.run.status == "completed", "workflow run", workflow.ok ? workflow.run.status : workflow.error);

    workflow_start_request resume;
    resume.run_id = start.run_id;
    resume.source = source;
    resume.resume = true;
    const workflow_response resumed = _services.start_workflow(resume);
    check(resumed.ok && resumed.run.status == "completed" && resumed.run.result_text.find("## inspect") != std::string::npos,
          "workflow resume",
          resumed.ok ? resumed.run.status : resumed.error);

    state_query_request node_query;
    node_query.key_prefix = "workflow-node/" + start.run_id;
    const state_response node_states = _services.query_state(node_query);
    check(node_states.ok && node_states.records.size() >= 2,
          "workflow node state",
          node_states.ok ? "nodes=" + std::to_string(node_states.records.size()) : node_states.error);

    workflow_source cancel_source;
    cancel_source.workflow_id = "rasn-cancel-selftest";
    cancel_source.source_name = "<cancel-selftest>";
    std::ostringstream cancel_text;
    for (size_t i = 0; i < 128; ++i)
    {
        cancel_text << "task cancel" << i
                    << " tool \"list .\" capability tool.run policy read_only budget_ms 1000 latency_ms 10 cost_hint 1 reliability 99 state selftest/cancel"
                    << i << "\n";
    }
    cancel_source.source_text = cancel_text.str();

    workflow_start_request cancel_start;
    cancel_start.run_id = "selftest-cancel-" + completion.task.id;
    cancel_start.source = cancel_source;

    workflow_response started_cancel_run;
    std::thread cancel_thread([this, &cancel_start, &started_cancel_run]() {
        if (_services.rpc_clients_enabled() && !::dsn_mimic_app("rasn.workflow", 1))
        {
            started_cancel_run.ok = false;
            started_cancel_run.error = "failed to mimic rasn.workflow app context";
            started_cancel_run.run.status = "failed";
            started_cancel_run.run.error = started_cancel_run.error;
            return;
        }
        started_cancel_run = _services.start_workflow(cancel_start);
    });

    workflow_run_query cancel_query;
    cancel_query.run_id = cancel_start.run_id;
    workflow_response cancel_response;
    for (size_t attempt = 0; attempt < 5000; ++attempt)
    {
        cancel_response = _services.cancel_workflow(cancel_query);
        if (cancel_response.ok || cancel_response.error.find("not found") == std::string::npos)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    cancel_thread.join();

    state_query_request cancelled_node_query;
    cancelled_node_query.key_prefix = "workflow-node/" + cancel_start.run_id;
    const state_response cancelled_node_states = _services.query_state(cancelled_node_query);
    bool has_cancelled_node = false;
    if (cancelled_node_states.ok)
    {
        for (const state_record &node_state : cancelled_node_states.records)
        {
            if (node_state.value.find("status=cancelled") != std::string::npos)
            {
                has_cancelled_node = true;
                break;
            }
        }
    }

    check(cancel_response.ok && started_cancel_run.run.status == "cancelled" && has_cancelled_node,
          "workflow cancellation",
          cancel_response.ok ? started_cancel_run.run.status : cancel_response.error);

    observability_response observed = _services.observability_snapshot();
    check(observed.ok && !observed.events.empty(),
          "observability snapshot",
          observed.ok ? "events=" + std::to_string(observed.events.size()) +
                            " failures=" + std::to_string(observed.failures.size())
                      : observed.error);

    state_query_request snapshot_query;
    snapshot_query.key_prefix = "observability-snapshot/";
    const state_response snapshot_index = _services.query_state(snapshot_query);
    check(snapshot_index.ok && !snapshot_index.records.empty(),
          "observability snapshot state index",
          snapshot_index.ok ? "records=" + std::to_string(snapshot_index.records.size()) : snapshot_index.error);

    std::cout << (ok ? "rASN self-test passed\n" : "rASN self-test failed\n");
    return ok ? 0 : 1;
}

int codepilot_cli::run_workflow(const std::string &path, const std::string &run_id, bool resume)
{
    std::string error;
    workflow_source source;
    if (!load_workflow_source(path, &source, &error))
    {
        std::cout << error << "\n";
        return 1;
    }

    workflow_start_request request;
    request.run_id = run_id.empty() ? make_trace_id() : run_id;
    request.source = source;
    request.resume = resume;

    workflow_response response = _services.start_workflow(request);
    if (!response.ok)
    {
        std::cout << response.error << "\n";
        return 1;
    }

    print_workflow_run(response.run, true);
    return 0;
}

int codepilot_cli::enable_replay(const std::string &path)
{
    replay_load_request request;
    request.path = path;
    const observability_response response = _services.load_replay(request);
    if (!response.ok)
    {
        std::cout << response.error << "\n";
        return 1;
    }
    std::cout << "replay enabled from " << path << "\n";
    return 0;
}

int codepilot_cli::set_provider(const std::string &provider_name)
{
    const model_gateway_response response = _services.set_provider(provider_name);
    if (!response.ok)
    {
        std::cout << response.error << "\n";
        return 1;
    }
    std::cout << provider_summary() << "\n";
    return 0;
}

bool codepilot_cli::load_context_file(const std::string &path, std::string *error)
{
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input)
    {
        if (error != nullptr)
        {
            *error = "cannot open context file: " + path;
        }
        return false;
    }

    std::ostringstream content;
    content << input.rdbuf();
    _context.push_back("file: " + path + "\n" + content.str());
    return true;
}

void codepilot_cli::print_help(bool interactive_mode) const
{
    std::cout << "rASN CodePilot commands:\n"
              << cli_help_intro(interactive_mode, "sent as an ask prompt")
              << cli_help_item(interactive_mode, "ask <prompt>", "send a coding prompt")
              << cli_help_item(interactive_mode, "stream <prompt>", "stream model-response chunks with trace events")
              << cli_help_item(interactive_mode, "agent <prompt>", "run an agent loop that can request local tools")
              << cli_help_item(interactive_mode, "plan <goal>", "request an implementation plan")
              << cli_help_item(interactive_mode, "eval [suite]", "run CodePilot eval tasks and latency/failure metrics")
              << cli_help_item(interactive_mode, "eval external <template> [suite]", "run an external CLI command template with {prompt}")
              << cli_help_item(interactive_mode, "workflow <file>", "execute a declarative task graph")
              << cli_help_item(interactive_mode, "workflow start <file> [run-id]", "execute a task graph")
              << cli_help_item(interactive_mode, "workflow resume <file> <run-id>", "resume from completed node state")
              << cli_help_item(interactive_mode, "workflow validate <file>", "validate a task graph through rasn.workflow")
              << cli_help_item(interactive_mode, "workflow compile <file>", "compile a task graph into an executable plan")
              << cli_help_item(interactive_mode, "workflow query <run-id>", "query a workflow run")
              << cli_help_item(interactive_mode, "workflow cancel <run-id>", "cancel a non-terminal workflow run")
              << cli_help_item(interactive_mode, "workflow nodes <run-id>", "list latest per-node workflow state")
              << cli_help_item(interactive_mode, "context <file>", "attach a source file to future prompts")
              << cli_help_item(interactive_mode, "schema [text|json|idl|cpp|clients-cpp|ts|clients-ts|py|clients-py]", "export schemas and RPC clients")
              << cli_help_item(interactive_mode, "tools", "list local tools")
              << cli_help_item(interactive_mode, "tool [--yes] <name> <args>", "run a local tool directly")
              << cli_help_item(interactive_mode, "selftest [checkpoint]", "run model/tool/state/workflow/observability checks")
              << cli_help_item(interactive_mode, "state <cmd> [args]", "use rASN state/checkpoint service")
              << cli_help_item(interactive_mode, "registry [cmd] [args]", "inspect rASN agent registry entries")
              << cli_help_item(interactive_mode, "agentctl <cmd> <agent>", "describe, heartbeat, query, or cancel an agent")
              << cli_help_item(interactive_mode, "observe events [kind]", "query structured runtime events")
              << cli_help_item(interactive_mode, "observe timeline [trace]", "show ordered trace events")
              << cli_help_item(interactive_mode, "observe diagnose [trace]", "summarize failures and replay issues")
              << cli_help_item(interactive_mode, "observe failures", "query classified failure records")
              << cli_help_item(interactive_mode, "observe replay <file>", "load replay choices through rasn.observability")
              << cli_help_item(interactive_mode, "observe metrics [format]", "dump runtime metrics (text|prometheus|json)")
              << cli_help_item(interactive_mode, "observe resilience", "dump overload/model/tool/remote-agent resilience state")
              << cli_help_item(interactive_mode, "observe snapshot", "summarize observability state")
              << cli_help_item(interactive_mode, "skills", "list built-in skills")
              << cli_help_item(interactive_mode, "skill <name> [task]", "show or apply a skill prompt")
              << cli_help_item(interactive_mode, "topology", "show the rDSN service graph")
              << cli_help_item(interactive_mode, "provider [name]", "show or switch provider")
              << cli_help_item(interactive_mode, "trace [file]", "show or set JSONL runtime trace file")
              << cli_help_item(interactive_mode, "replay <trace-jsonl>", "replay captured nondeterministic choices")
              << cli_help_item(interactive_mode, "simulate <prompt>", "force the random local simulator")
              << cli_help_item(interactive_mode, "interactive", "start REPL mode")
              << "\n"
              << describe_provider_environment() << "\n";
}

std::string codepilot_cli::provider_summary() const
{
    return _services.provider_summary();
}

::dsn::error_code codepilot_app::start(int argc, char **argv)
{
    global_rasn_services().acquire();
    _args.clear();
    int begin = 0;
    if (argc > 0 && argv[0] != nullptr && std::string(argv[0]) == "rasn.codepilot")
    {
        begin = 1;
    }
    for (int i = begin; i < argc; ++i)
    {
        _args.push_back(argv[i] == nullptr ? "" : argv[i]);
    }

    _cli_task = ::dsn::tasking::enqueue(
        LPC_RASN_CODEPILOT_START, nullptr, [this] { run_cli_task(); }, 0, std::chrono::milliseconds(100));
    if (_cli_task == nullptr)
    {
        global_rasn_services().release();
        return ::dsn::ERR_UNKNOWN;
    }
    return ::dsn::ERR_OK;
}

::dsn::error_code codepilot_app::stop(bool cleanup)
{
    if (_cli_task != nullptr)
    {
        // Ask the interactive loop to stop, then cancel WITHOUT waiting. The CLI
        // task may be parked in repl() on a blocking std::getline(std::cin), and
        // rDSN tasks are not preemptible, so cancel(true) would hang shutdown until
        // the user happened to press enter/EOF.
        _cli.request_shutdown();
        _cli_task->cancel(false);
        _cli_task = nullptr;
    }
    global_rasn_services().release();
    return ::dsn::ERR_OK;
}

void codepilot_app::run_cli_task()
{
    std::string readiness_error;
    const int rc = wait_for_service_dependencies(global_rasn_services(), &readiness_error) ? _cli.run(_args) : 1;
    if (!readiness_error.empty())
    {
        std::cerr << readiness_error << "\n";
    }
    std::cout.flush();
    std::cerr.flush();
    if (rc != 0)
    {
        derror("CodePilot CLI exited with code %d", rc);
    }
}

} // namespace rasn
} // namespace dsn
