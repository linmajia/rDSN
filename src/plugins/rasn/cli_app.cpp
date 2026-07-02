#include "cli_app.h"

#include "agent_clients.h"
#include "agent_registry.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <thread>
#include <tuple>

#if !defined(_WIN32)
#include <termios.h>
#include <unistd.h>
#endif

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
    return trim(oss.str());
}

bool starts_with(const std::string &value, const std::string &prefix)
{
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool take_flag_value(const std::vector<std::string> &args,
                     size_t *index,
                     const std::string &arg,
                     const std::string &flag,
                     std::string *value,
                     std::string *error)
{
    const std::string equals_prefix = flag + "=";
    if (starts_with(arg, equals_prefix))
    {
        *value = arg.substr(equals_prefix.size());
        return true;
    }
    if (*index + 1 >= args.size())
    {
        if (error != nullptr)
        {
            *error = flag + " requires a value";
        }
        return false;
    }
    ++(*index);
    *value = args[*index];
    return true;
}

bool take_optional_flag_value(const std::vector<std::string> &args,
                              size_t *index,
                              const std::string &arg,
                              const std::string &flag,
                              std::string *value)
{
    const std::string equals_prefix = flag + "=";
    if (starts_with(arg, equals_prefix))
    {
        *value = arg.substr(equals_prefix.size());
        return true;
    }
    if (*index + 1 < args.size() && !starts_with(args[*index + 1], "-"))
    {
        ++(*index);
        *value = args[*index];
        return true;
    }
    return false;
}

bool parse_compat_args(const std::vector<std::string> &args,
                       const std::vector<std::string> &commands,
                       rasn_cli_compat_options *options,
                       std::vector<std::string> *normalized_args,
                       std::string *error)
{
    (void)commands;
    *options = rasn_cli_compat_options();
    normalized_args->clear();
    bool passthrough = false;
    for (size_t i = 0; i < args.size(); ++i)
    {
        const std::string &arg = args[i];
        if (passthrough)
        {
            normalized_args->push_back(arg);
            continue;
        }
        if (arg == "--")
        {
            passthrough = true;
            continue;
        }
        if (arg == "-h" || arg == "--help")
        {
            options->help = true;
            continue;
        }
        if (arg == "--version" || arg == "-V")
        {
            options->version = true;
            continue;
        }
        if (arg == "--print" || arg == "-p")
        {
            options->print = true;
            options->no_interactive = true;
            std::string prompt;
            if (take_optional_flag_value(args, &i, arg, arg == "-p" ? "-p" : "--print", &prompt))
            {
                options->prompt = prompt;
                options->prompt_set = true;
            }
            continue;
        }
        if (starts_with(arg, "--print="))
        {
            options->print = true;
            options->no_interactive = true;
            options->prompt = arg.substr(std::string("--print=").size());
            options->prompt_set = true;
            continue;
        }
        if (arg == "--prompt")
        {
            options->no_interactive = true;
            if (!take_flag_value(args, &i, arg, "--prompt", &options->prompt, error))
            {
                return false;
            }
            options->prompt_set = true;
            continue;
        }
        if (starts_with(arg, "--prompt="))
        {
            options->no_interactive = true;
            options->prompt = arg.substr(std::string("--prompt=").size());
            options->prompt_set = true;
            continue;
        }
        if (arg == "--stream")
        {
            options->stream = true;
            continue;
        }
        if (arg == "--model" || arg == "-m")
        {
            if (!take_flag_value(args, &i, arg, arg == "-m" ? "-m" : "--model", &options->model, error))
            {
                return false;
            }
            options->model_set = true;
            continue;
        }
        if (starts_with(arg, "--model="))
        {
            options->model = arg.substr(std::string("--model=").size());
            options->model_set = true;
            continue;
        }
        if (arg == "--provider")
        {
            if (!take_flag_value(args, &i, arg, "--provider", &options->provider, error))
            {
                return false;
            }
            options->provider_set = true;
            continue;
        }
        if (starts_with(arg, "--provider="))
        {
            options->provider = arg.substr(std::string("--provider=").size());
            options->provider_set = true;
            continue;
        }
        if (arg == "--cwd" || arg == "--workspace" || arg == "--dir" || arg == "-C")
        {
            if (!take_flag_value(args, &i, arg, arg, &options->workspace, error))
            {
                return false;
            }
            options->workspace_set = true;
            continue;
        }
        if (starts_with(arg, "--cwd=") || starts_with(arg, "--workspace=") || starts_with(arg, "--dir="))
        {
            const size_t equals = arg.find('=');
            options->workspace = arg.substr(equals + 1);
            options->workspace_set = true;
            continue;
        }
        if (arg == "--continue" || arg == "-c")
        {
            options->continue_latest = true;
            continue;
        }
        if (arg == "--resume")
        {
            options->resume_set = true;
            (void)take_optional_flag_value(args, &i, arg, "--resume", &options->resume_id);
            continue;
        }
        if (starts_with(arg, "--resume="))
        {
            options->resume_set = true;
            options->resume_id = arg.substr(std::string("--resume=").size());
            continue;
        }
        if (arg == "--yes" || arg == "-y")
        {
            options->yes = true;
            continue;
        }
        if (arg == "--dry-run")
        {
            options->dry_run = true;
            continue;
        }
        if (arg == "--no-interactive" || arg == "--non-interactive")
        {
            options->no_interactive = true;
            continue;
        }
        if (arg == "--approval")
        {
            if (!take_flag_value(args, &i, arg, "--approval", &options->approval, error))
            {
                return false;
            }
            options->approval_set = true;
            continue;
        }
        if (starts_with(arg, "--approval="))
        {
            options->approval = arg.substr(std::string("--approval=").size());
            options->approval_set = true;
            continue;
        }
        if (arg == "--sandbox")
        {
            if (!take_flag_value(args, &i, arg, "--sandbox", &options->sandbox, error))
            {
                return false;
            }
            options->sandbox_set = true;
            continue;
        }
        if (starts_with(arg, "--sandbox="))
        {
            options->sandbox = arg.substr(std::string("--sandbox=").size());
            options->sandbox_set = true;
            continue;
        }
        normalized_args->push_back(arg);
    }

    if ((options->print || options->stream || options->prompt_set) && !normalized_args->empty())
    {
        const std::string remaining_prompt = join_args(*normalized_args, 0);
        if (options->prompt.empty())
        {
            options->prompt = remaining_prompt;
        }
        else if (!remaining_prompt.empty())
        {
            options->prompt += " " + remaining_prompt;
        }
        options->prompt_set = true;
        normalized_args->clear();
    }
    return true;
}

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

bool probe_state_service(const rasn_service_graph &services,
                         const rasn_cli_service_readiness_options &options,
                         std::vector<std::string> *errors)
{
    rasn_state_client state(services.state_address());
    state_query_request request;
    request.key_prefix = options.state_probe_key;
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

bool probe_workflow_service(const rasn_service_graph &services,
                            const rasn_cli_service_readiness_options &options,
                            std::vector<std::string> *errors)
{
    rasn_workflow_client workflow(services.workflow_address());
    workflow_source source;
    source.workflow_id = options.workflow_id;
    source.source_name = options.workflow_source_name;
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

bool probe_service_dependencies_once(const rasn_service_graph &services,
                                     const rasn_cli_service_readiness_options &options,
                                     std::vector<std::string> *errors)
{
    bool ready = true;
    ready = probe_state_service(services, options, errors) && ready;
    ready = probe_registry_service(services, errors) && ready;
    ready = probe_agent_service("coordinator", services.coordinator_address(), "rasn.coordinator", errors) && ready;
    ready = probe_agent_service("model.agent", services.llm_agent_address(), "rasn.llm.agent", errors) && ready;
    ready = probe_model_health(services, errors) && ready;
    ready = probe_agent_service("tool.agent", services.tool_agent_address(), "rasn.tool.agent", errors) && ready;
    ready = probe_workflow_service(services, options, errors) && ready;
    ready = probe_observability_service(services, errors) && ready;
    return ready;
}

class terminal_input_scope
{
public:
    terminal_input_scope()
    {
#if !defined(_WIN32)
        // macOS terminals commonly send DEL for Backspace/Delete. Normalize the
        // erase character while the REPL is active so std::getline stays usable.
        if (!::isatty(STDIN_FILENO))
        {
            return;
        }
        if (::tcgetattr(STDIN_FILENO, &_original) != 0)
        {
            return;
        }

        struct termios updated = _original;
        updated.c_cc[VERASE] = static_cast<cc_t>('\177');
        if (updated.c_cc[VERASE] == _original.c_cc[VERASE])
        {
            return;
        }
        if (::tcsetattr(STDIN_FILENO, TCSANOW, &updated) == 0)
        {
            _restore = true;
        }
#endif
    }

    ~terminal_input_scope()
    {
#if !defined(_WIN32)
        if (_restore)
        {
            (void)::tcsetattr(STDIN_FILENO, TCSANOW, &_original);
        }
#endif
    }

    terminal_input_scope(const terminal_input_scope &) = delete;
    terminal_input_scope &operator=(const terminal_input_scope &) = delete;

private:
#if !defined(_WIN32)
    struct termios _original;
    bool _restore = false;
#endif
};

} // namespace

rasn_service_graph_lifecycle_scope::rasn_service_graph_lifecycle_scope(rasn_service_graph &services)
    : _services(services)
{
    _services.acquire();
}

rasn_service_graph_lifecycle_scope::~rasn_service_graph_lifecycle_scope()
{
    _services.release();
}

std::vector<std::string> cli_args_from_argv(int argc, char **argv, int begin)
{
    std::vector<std::string> args;
    for (int i = (std::max)(0, begin); i < argc; ++i)
    {
        args.push_back(argv[i] == nullptr ? "" : argv[i]);
    }
    return args;
}

void run_dsn_with_cli_args(const std::vector<std::string> &args, bool sleep_after_init)
{
    std::vector<char *> dsn_args;
    dsn_args.reserve(args.size());
    for (const std::string &arg : args)
    {
        dsn_args.push_back(const_cast<char *>(arg.c_str()));
    }
    ::dsn_run(static_cast<int>(dsn_args.size()), dsn_args.data(), sleep_after_init);
}

bool wait_for_cli_service_dependencies(const rasn_service_graph &services,
                                       const rasn_cli_service_readiness_options &options,
                                       std::string *error)
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
        if (probe_service_dependencies_once(services, options, &errors))
        {
            return true;
        }
        last_errors.swap(errors);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (error != nullptr)
    {
        *error = options.dependency_error;
        if (!last_errors.empty())
        {
            *error += ": " + readiness_errors_summary(last_errors);
        }
    }
    return false;
}

rasn_cli_app_base::rasn_cli_app_base(rasn_service_graph &services) : _services(services) {}

rasn_cli_app_base::~rasn_cli_app_base() {}

int rasn_cli_app_base::run(const std::vector<std::string> &args)
{
    const std::vector<std::string> app_commands = commands();
    rasn_cli_compat_options compat_options;
    std::vector<std::string> normalized_args;
    std::string parse_error;
    if (!parse_compat_args(args, app_commands, &compat_options, &normalized_args, &parse_error))
    {
        std::cout << parse_error << "\n";
        return 1;
    }
    if (compat_options.help || compat_options.version)
    {
        int exit_code = 0;
        if (handle_compat_options(compat_options, &exit_code))
        {
            return exit_code;
        }
    }
    if (compat_options.workspace_set)
    {
        std::string workspace_error;
        if (!switch_cli_workspace(compat_options.workspace, &workspace_error))
        {
            std::cout << workspace_error << "\n";
            return 1;
        }
    }

    cli_startup_context startup;
    const cli_workspace_context_options workspace_options = workspace_context_options();
    if (!bootstrap_single_path_argument(normalized_args, app_commands, &startup, max_context_bytes(), &workspace_options))
    {
        std::cout << startup.error << "\n";
        return 1;
    }
    if (startup.matched)
    {
        on_startup_context(startup);
    }

    rasn_service_graph_lifecycle_scope lifecycle(_services);
    int compat_exit_code = 0;
    if (handle_compat_options(compat_options, &compat_exit_code))
    {
        return compat_exit_code;
    }
    if (startup.matched)
    {
        std::cout << startup.message << "\n";
        return repl();
    }
    if (normalized_args.empty())
    {
        return handle_empty_args();
    }
    if (normalized_args[0] == "interactive" || normalized_args[0] == "repl")
    {
        return repl();
    }
    if (normalized_args[0] == "/exit" || normalized_args[0] == "/quit")
    {
        return 0;
    }
    if (normalized_args[0].size() > 1 && normalized_args[0][0] == '/')
    {
        const std::string command = normalized_args[0].substr(1);
        if (cli_argument_is_command(command, app_commands))
        {
            std::vector<std::string> slash_args = normalized_args;
            slash_args[0] = command;
            return run_command(slash_args);
        }
    }
    return run_command(normalized_args);
}

int rasn_cli_app_base::repl()
{
    terminal_input_scope terminal_input;
    std::cout << repl_title() << "\n";
    std::cout << provider_summary() << "\n";
    std::cout << "Type /help for commands. Plain text is " << repl_plain_text_behavior() << ".\n";

    std::string line;
    while (true)
    {
        if (_shutdown_requested.load())
        {
            return 0;
        }

        std::cout << repl_prompt();
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
            const int rc = run_command(split_words(line.substr(1)), true);
            if (rc != 0)
            {
                std::cout << "command failed: " << rc << "\n";
            }
            continue;
        }

        handle_plain_text(line);
    }
}

int rasn_cli_app_base::handle_empty_args()
{
    return repl();
}

bool rasn_cli_app_base::handle_compat_options(const rasn_cli_compat_options &options, int *exit_code)
{
    if (exit_code != nullptr)
    {
        *exit_code = 0;
    }
    if (options.version)
    {
        std::cout << version_string() << "\n";
        return true;
    }
    if (options.help)
    {
        print_compat_help();
        return true;
    }

    const bool quiet_provider = options.prompt_set || options.print || options.stream;
    if (options.dry_run)
    {
        std::cout << compat_dry_run_message() << "\n";
        return true;
    }
    if (options.provider_set || options.model_set)
    {
        std::string provider_name = options.provider;
        if (!options.provider_set)
        {
            const model_gateway_response current_provider = _services.model_provider();
            provider_name = current_provider.provider.provider.empty() ? "simulator" : current_provider.provider.provider;
        }

        const model_gateway_response response = _services.set_provider(provider_name, options.model);
        if (!response.ok)
        {
            std::cout << response.error << "\n";
            if (exit_code != nullptr)
            {
                *exit_code = 1;
            }
            return true;
        }
        if (!quiet_provider)
        {
            print_compat_provider(response);
        }
    }
    if ((options.resume_set || options.continue_latest) && handle_compat_resume(options, exit_code))
    {
        return true;
    }
    if (supports_compat_safety_options() && (options.approval_set || options.sandbox_set || options.yes))
    {
        if (options.yes)
        {
            std::cout << "approval: yes\n";
        }
        if (options.approval_set)
        {
            std::cout << "approval policy: " << options.approval << "\n";
        }
        if (options.sandbox_set)
        {
            std::cout << "sandbox: " << options.sandbox << "\n";
        }
    }
    if (options.prompt_set)
    {
        on_compat_prompt_start(options);
        const int rc = run_compat_prompt(options.prompt, options.stream);
        on_compat_prompt_finish(options);
        if (exit_code != nullptr)
        {
            *exit_code = rc;
        }
        return true;
    }
    if (options.print || options.stream)
    {
        std::cout << compat_prompt_usage() << "\n";
        if (exit_code != nullptr)
        {
            *exit_code = 1;
        }
        return true;
    }
    if (options.no_interactive)
    {
        return true;
    }
    return false;
}

void rasn_cli_app_base::print_compat_help() const
{
    std::cout << repl_title() << "\n";
    std::cout << "Common options:\n"
              << "  -h, --help                 Show help\n"
              << "  --version                  Show version\n"
              << "  -p, --print [prompt]       Run one prompt and print the answer\n"
              << "  --prompt <prompt>          Run one prompt without entering the REPL\n"
              << "  -m, --model <model>        Select model for providers that support models\n"
              << "  --provider <provider>      Select LLM provider\n"
              << "  --cwd, --workspace, --dir  Use a workspace directory\n"
              << "  --resume [id], --continue  Continue prior trace/replay state when available\n";
}

std::string rasn_cli_app_base::version_string() const
{
    return "rASN CLI prototype";
}

std::string rasn_cli_app_base::compat_prompt_usage() const
{
    return "usage: --print <prompt>";
}

std::string rasn_cli_app_base::compat_dry_run_message() const
{
    return "dry-run: no request executed";
}

std::string rasn_cli_app_base::compat_resume_continue_message() const
{
    return "--resume/--continue are accepted for compatibility; no default session store is configured yet";
}

bool rasn_cli_app_base::handle_compat_resume(const rasn_cli_compat_options &options, int *exit_code)
{
    (void)exit_code;
    if (options.resume_set || options.continue_latest)
    {
        std::cout << compat_resume_continue_message() << "\n";
    }
    return false;
}

bool rasn_cli_app_base::supports_compat_safety_options() const
{
    return false;
}

void rasn_cli_app_base::print_compat_provider(const model_gateway_response &response) const
{
    std::cout << "provider=" << response.provider.provider << " model=" << response.provider.model << "\n";
}

void rasn_cli_app_base::on_compat_prompt_start(const rasn_cli_compat_options &options)
{
    (void)options;
}

void rasn_cli_app_base::on_compat_prompt_finish(const rasn_cli_compat_options &options)
{
    (void)options;
}

void rasn_cli_app_base::on_startup_context(const cli_startup_context &startup)
{
    (void)startup;
}

size_t rasn_cli_app_base::max_context_bytes() const
{
    return 1024u * 1024u;
}

cli_workspace_context_options rasn_cli_app_base::workspace_context_options() const
{
    return cli_workspace_context_options();
}

std::string rasn_cli_app_base::provider_summary() const
{
    return _services.provider_summary();
}

int rasn_cli_app_base::run_agent_plan(const rasn_cli_agent_plan &plan,
                                      const agent_plan_executor::model_callback &model,
                                      const agent_plan_executor::approval_callback &approve,
                                      const agent_plan_executor::tool_callback &tool)
{
    _services.runtime().begin_task(plan.task);

    agent_executor_request request;
    request.task = plan.task;
    request.prompt = plan.prompt;
    request.system_prompt = plan.system_prompt;
    request.context = plan.context;

    agent_plan_executor executor;
    const agent_executor_result result = executor.execute(request, plan.executor_options, model, approve, tool);
    const std::string status = result.status.empty() ? (result.ok ? "ok" : "failed") : result.status;
    const std::string error = result.error.empty() ? "agent plan execution failed" : result.error;
    if (status == "approval-denied" && !plan.approval_failure_source.empty())
    {
        _services.runtime().record_failure(
            plan.task, plan.approval_failure_category, plan.approval_failure_code, error, false, plan.approval_failure_source);
    }
    _services.runtime().finish_task(plan.task, status);

    if (result.ok)
    {
        std::cout << result.output << "\n";
        return 0;
    }

    std::cout << error << "\n";
    return 1;
}

} // namespace rasn
} // namespace dsn
