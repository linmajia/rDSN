#include <rasn/cli_app.h>

#include <rasn/agent_clients.h>
#include <rasn/agent_registry.h>

#include <algorithm>
#include <cctype>
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

std::string sanitize_agent_id_component(const std::string &value)
{
    std::string result;
    bool previous_separator = true;
    for (unsigned char c : value)
    {
        if (std::isalnum(c))
        {
            result.push_back(static_cast<char>(std::tolower(c)));
            previous_separator = false;
        }
        else if (!previous_separator)
        {
            result.push_back('.');
            previous_separator = true;
        }
    }
    while (!result.empty() && result[result.size() - 1] == '.')
    {
        result.resize(result.size() - 1);
    }
    return result.empty() ? "cli" : result;
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

rasn_cli_app_base::rasn_cli_app_base(rasn_service_graph &services)
    : _services(services), _sandbox_profile(default_read_only_sandbox_profile())
{
}

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
        on_cli_workspace_changed(compat_options.workspace);
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
    initialize_runtime_modules();
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
            return run_tracked_command(slash_args, false);
        }
    }
    return run_tracked_command(normalized_args, false);
}

int rasn_cli_app_base::repl()
{
    initialize_runtime_modules();
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
            const int rc = run_tracked_command(split_words(line.substr(1)), true);
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
        const int rc = run_tracked_compat_prompt(options);
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

void rasn_cli_app_base::on_cli_workspace_changed(const std::string &workspace)
{
    (void)workspace;
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

std::string rasn_cli_app_base::cli_agent_id() const
{
    return "rasn." + sanitize_agent_id_component(repl_prompt()) + ".cli";
}

std::string rasn_cli_app_base::cli_agent_role() const
{
    return "cli";
}

std::string rasn_cli_app_base::cli_agent_app_name() const
{
    return repl_title();
}

std::vector<agent_capability> rasn_cli_app_base::cli_agent_capabilities() const
{
    std::vector<agent_capability> capabilities;
    agent_capability prompt;
    prompt.name = "cli.prompt";
    prompt.input_type = "text";
    prompt.output_type = "text";
    prompt.side_effect_class = "model";
    capabilities.push_back(prompt);

    const std::vector<std::string> app_commands = commands();
    for (const std::string &command : app_commands)
    {
        if (command.empty() || command[0] == '-')
        {
            continue;
        }
        agent_capability capability;
        capability.name = "cli.command." + command;
        capability.input_type = "argv";
        capability.output_type = "exit_code";
        capability.side_effect_class = "mixed";
        capabilities.push_back(capability);
    }
    return capabilities;
}

void rasn_cli_app_base::initialize_runtime_modules()
{
    if (_runtime_modules_initialized)
    {
        heartbeat_runtime_modules();
        return;
    }

    agent_control_record record;
    record.descriptor.agent_id = cli_agent_id();
    record.descriptor.role = cli_agent_role();
    record.descriptor.app_name = cli_agent_app_name();
    record.descriptor.version = version_string();
    record.descriptor.health = "healthy";
    record.descriptor.capabilities = cli_agent_capabilities();
    record.state = "starting";
    record.placement = "local";
    record.restart_policy = "manual";

    std::string error;
    if (!_agent_control.upsert_agent(record, &error))
    {
        warn_runtime_module_failure("agent_control_plane", error);
    }
    else
    {
        const uint64_t now_ms = ::dsn_now_ms();
        const std::string owner = cli_agent_id() + ".process";
        const agent_control_lease lease = _agent_control.acquire_lease(record.descriptor.agent_id, owner, now_ms, 0);
        if (!lease.ok)
        {
            warn_runtime_module_failure("agent_control_plane", lease.error);
        }
        if (!_agent_control.heartbeat(record.descriptor.agent_id, now_ms, &error))
        {
            warn_runtime_module_failure("agent_control_plane", error);
        }
    }

    capability_provider provider;
    provider.descriptor = record.descriptor;
    provider.state = "running";
    provider.placement = "local";
    provider.labels.push_back("cli");
    provider.labels.push_back(cli_agent_role());
    if (!_capability_directory.upsert_provider(provider, &error))
    {
        warn_runtime_module_failure("capability_directory", error);
    }

    resource_quota quota;
    quota.scope = cli_agent_id();
    quota.max_tool_calls = 100000;
    if (!_budget_manager.configure(quota, &error))
    {
        warn_runtime_module_failure("resource_budget", error);
    }

    recovery_policy fallback_policy;
    fallback_policy.failure_class = "*";
    fallback_policy.max_attempts = 1;
    fallback_policy.retryable = false;
    if (!_recovery.set_policy(fallback_policy, &error))
    {
        warn_runtime_module_failure("recovery_supervisor", error);
    }
    recovery_policy transient_policy;
    transient_policy.failure_class = "transient";
    transient_policy.max_attempts = 3;
    transient_policy.retry_delay_ms = 100;
    transient_policy.escalate_after_attempts = 3;
    transient_policy.retryable = true;
    if (!_recovery.set_policy(transient_policy, &error))
    {
        warn_runtime_module_failure("recovery_supervisor", error);
    }

    agent_contract command_contract;
    command_contract.contract_id = "cli.command";
    command_contract.require_input_non_empty = false;
    command_contract.require_output_non_empty = false;
    if (!_contracts.register_contract(command_contract, &error))
    {
        warn_runtime_module_failure("contract_verifier", error);
    }

    _runtime_modules_initialized = true;
}

void rasn_cli_app_base::heartbeat_runtime_modules()
{
    _agent_control.expire_leases(::dsn_now_ms());
    std::string error;
    if (!_agent_control.heartbeat(cli_agent_id(), ::dsn_now_ms(), &error))
    {
        warn_runtime_module_failure("agent_control_plane", error);
    }
}

int rasn_cli_app_base::run_tracked_command(const std::vector<std::string> &args, bool interactive_mode)
{
    initialize_runtime_modules();
    const std::string input = join_args(args, 0);
    const std::string name = args.empty() ? "empty" : args[0];
    const runtime_execution execution =
        begin_runtime_execution("command", name, input, cli_agent_id(), "cli.command");
    const int rc = run_command(args, interactive_mode);
    finish_runtime_execution(execution, rc, input);
    return rc;
}

int rasn_cli_app_base::run_tracked_compat_prompt(const rasn_cli_compat_options &options)
{
    initialize_runtime_modules();
    const runtime_execution execution =
        begin_runtime_execution("prompt", options.stream ? "stream" : "prompt", options.prompt, cli_agent_id(), "cli.prompt");
    const int rc = run_compat_prompt(options.prompt, options.stream);
    finish_runtime_execution(execution, rc, options.prompt);
    return rc;
}

rasn_cli_app_base::runtime_execution rasn_cli_app_base::begin_runtime_execution(const std::string &kind,
                                                                               const std::string &name,
                                                                               const std::string &input,
                                                                               const std::string &receiver,
                                                                               const std::string &message_type)
{
    runtime_execution execution;
    execution.active = true;
    execution.task_id = make_trace_id();
    execution.budget.scope = cli_agent_id();
    execution.budget.tool_calls = 1;
    execution.budget.reason = kind + ":" + name;
    const resource_budget_decision budget = _budget_manager.reserve(execution.budget);
    if (!budget.allowed)
    {
        warn_runtime_module_failure("resource_budget", budget.reason);
    }
    else
    {
        execution.budget_reserved = true;
    }

    orchestration_task task;
    task.task_id = execution.task_id;
    task.owner_agent = cli_agent_id();
    task.input = input;
    std::string error;
    if (!_orchestration.add_task(task, &error))
    {
        warn_runtime_module_failure("task_orchestration", error);
    }
    else if (!_orchestration.start(task.task_id, cli_agent_id(), &error))
    {
        warn_runtime_module_failure("task_orchestration", error);
    }

    agent_message message;
    message.message_id = execution.task_id + "/message";
    message.correlation_id = execution.task_id;
    message.sender = cli_agent_id();
    message.receiver = receiver.empty() ? cli_agent_id() : receiver;
    message.type = message_type;
    message.payload = kind + ":" + name + "\n" + input;
    message.deadline_ms = ::dsn_now_ms() + 10 * 60 * 1000;
    agent_message stored;
    if (!_message_bus.publish(message, &stored, &error))
    {
        warn_runtime_module_failure("agent_message_bus", error);
    }
    else
    {
        execution.message_id = stored.message_id;
    }

    deterministic_choice choice;
    if (!_determinism.record(execution.task_id, "route", "rasn.cli", kind + ":" + name, &choice, &error))
    {
        warn_runtime_module_failure("determinism_ledger", error);
    }

    blackboard_entry input_entry;
    input_entry.key = "task/" + execution.task_id + "/input";
    input_entry.kind = kind + ".input";
    input_entry.owner = cli_agent_id();
    input_entry.value = input;
    input_entry.tags.push_back(kind);
    if (!_blackboard.put(input_entry, nullptr, &error))
    {
        warn_runtime_module_failure("blackboard", error);
    }

    const contract_evaluation input_contract = _contracts.evaluate_input("cli.command", input);
    if (!input_contract.ok)
    {
        warn_runtime_module_failure("contract_verifier", "input contract violation for " + execution.task_id);
    }
    return execution;
}

void rasn_cli_app_base::finish_runtime_execution(const runtime_execution &execution,
                                                 int exit_code,
                                                 const std::string &detail)
{
    if (!execution.active)
    {
        return;
    }

    std::string error;
    deterministic_choice choice;
    if (!_determinism.record(
            execution.task_id, "exit_code", "rasn.cli", std::to_string(exit_code), &choice, &error))
    {
        warn_runtime_module_failure("determinism_ledger", error);
    }

    if (exit_code == 0)
    {
        if (!_orchestration.complete(execution.task_id, detail, &error))
        {
            warn_runtime_module_failure("task_orchestration", error);
        }
        if (!execution.message_id.empty() && !_message_bus.ack(execution.message_id, &error))
        {
            warn_runtime_module_failure("agent_message_bus", error);
        }
    }
    else
    {
        if (!_orchestration.fail(execution.task_id, detail, false, &error))
        {
            warn_runtime_module_failure("task_orchestration", error);
        }
        if (!execution.message_id.empty() && !_message_bus.dead_letter(execution.message_id, detail, &error))
        {
            warn_runtime_module_failure("agent_message_bus", error);
        }
        failure_observation failure;
        failure.task_id = execution.task_id;
        failure.component = cli_agent_id();
        failure.failure_class = "cli";
        failure.code = "non_zero_exit";
        failure.message = detail;
        failure.attempt = 1;
        failure.retryable = false;
        (void)_recovery.observe(failure);
    }

    blackboard_entry output_entry;
    output_entry.key = "task/" + execution.task_id + "/output";
    output_entry.kind = "cli.output";
    output_entry.owner = cli_agent_id();
    output_entry.value = detail;
    output_entry.tags.push_back(exit_code == 0 ? "ok" : "failed");
    if (!_blackboard.put(output_entry, nullptr, &error))
    {
        warn_runtime_module_failure("blackboard", error);
    }

    const contract_evaluation output_contract = _contracts.evaluate_output("cli.command", detail, std::vector<std::string>());
    if (!output_contract.ok)
    {
        warn_runtime_module_failure("contract_verifier", "output contract violation for " + execution.task_id);
    }
    if (execution.budget_reserved && !_budget_manager.release(execution.budget, &error))
    {
        warn_runtime_module_failure("resource_budget", error);
    }
    heartbeat_runtime_modules();
}

std::string rasn_cli_app_base::runtime_modules_summary() const
{
    std::ostringstream output;
    const std::vector<agent_control_record> agents = _agent_control.list(false, ::dsn_now_ms());
    const std::vector<agent_message> messages = _message_bus.snapshot();
    const std::vector<orchestration_task> tasks = _orchestration.snapshot();
    const std::vector<deterministic_choice> choices = _determinism.snapshot();
    const std::vector<blackboard_entry> blackboard = _blackboard.snapshot(false, ::dsn_now_ms());
    const std::vector<human_interaction_request> human = _human_interactions.snapshot();
    output << "rASN runtime modules\n"
           << "agent_control_plane: agents=" << agents.size() << "\n"
           << _agent_control.describe(::dsn_now_ms()) << "\n"
           << "agent_message_bus: messages=" << messages.size() << "\n"
           << "task_orchestration_kernel: tasks=" << tasks.size()
           << " ready=" << _orchestration.ready_tasks(::dsn_now_ms()).size()
           << " blocked=" << _orchestration.blocked_tasks().size() << "\n"
           << "determinism_ledger: choices=" << choices.size() << "\n"
           << "sandbox_runtime: profile=" << _sandbox_profile.name
           << " fs_read=" << (_sandbox_profile.allow_filesystem_read ? "yes" : "no")
           << " fs_write=" << (_sandbox_profile.allow_filesystem_write ? "yes" : "no")
           << " network=" << (_sandbox_profile.allow_network ? "yes" : "no")
           << " process=" << (_sandbox_profile.allow_process_spawn ? "yes" : "no") << "\n"
           << "capability_directory: " << _capability_directory.describe() << "\n"
           << "resource_budget: " << _budget_manager.describe() << "\n"
           << "recovery_supervisor: " << _recovery.describe() << "\n"
           << "blackboard: entries=" << blackboard.size() << "\n"
           << "contract_verifier: " << _contracts.describe() << "\n"
           << "human_interaction: requests=" << human.size()
           << " pending=" << _human_interactions.pending().size() << "\n";
    return output.str();
}

bool rasn_cli_app_base::runtime_modules_ready(std::string *detail) const
{
    const std::string summary = runtime_modules_summary();
    static const char *const required_modules[] = {
        "agent_control_plane",
        "agent_message_bus",
        "task_orchestration_kernel",
        "determinism_ledger",
        "sandbox_runtime",
        "capability_directory",
        "resource_budget",
        "recovery_supervisor",
        "blackboard",
        "contract_verifier",
        "human_interaction",
    };

    std::vector<std::string> missing;
    for (const char *module : required_modules)
    {
        if (summary.find(module) == std::string::npos)
        {
            missing.push_back(module);
        }
    }
    if (missing.empty())
    {
        if (detail != nullptr)
        {
            *detail = "all general runtime modules wired";
        }
        return true;
    }

    if (detail != nullptr)
    {
        std::ostringstream oss;
        oss << "missing:";
        for (const std::string &module : missing)
        {
            oss << " " << module;
        }
        *detail = oss.str();
    }
    return false;
}

deterministic_replay_result rasn_cli_app_base::record_runtime_choice(const std::string &task_id,
                                                                    const std::string &key,
                                                                    const std::string &source,
                                                                    const std::string &value)
{
    deterministic_replay_result result;
    std::string error;
    deterministic_choice choice;
    if (!_determinism.record(task_id, key, source, value, &choice, &error))
    {
        result.error = error;
        return result;
    }
    result.ok = true;
    result.choice = choice;
    return result;
}

sandbox_decision rasn_cli_app_base::evaluate_cli_sandbox_request(const sandbox_request &request) const
{
    return evaluate_sandbox_request(_sandbox_profile, request);
}

void rasn_cli_app_base::set_cli_sandbox_profile(const sandbox_profile &profile)
{
    _sandbox_profile = profile;
}

void rasn_cli_app_base::warn_runtime_module_failure(const std::string &module, const std::string &error) const
{
    if (!error.empty())
    {
        std::cerr << module << " warning: " << error << "\n";
    }
}

int rasn_cli_app_base::run_agent_plan(const rasn_cli_agent_plan &plan,
                                      const agent_plan_executor::model_callback &model,
                                      const agent_plan_executor::approval_callback &approve,
                                      const agent_plan_executor::tool_callback &tool)
{
    _services.runtime().begin_task(plan.task);
    const runtime_execution plan_execution =
        begin_runtime_execution("agent-plan", plan.task.name, plan.prompt, "rasn.agent.executor", "agent.plan");
    (void)record_runtime_choice(plan.task.id, "agent.plan.prompt", "rasn.agent_executor", plan.prompt);

    agent_executor_request request;
    request.task = plan.task;
    request.prompt = plan.prompt;
    request.system_prompt = plan.system_prompt;
    request.context = plan.context;

    agent_plan_executor executor;
    agent_plan_executor::model_callback tracked_model =
        [this, &model, &plan](const agent_executor_model_request &model_request) {
            const runtime_execution execution =
                begin_runtime_execution("model", model_request.request_id, model_request.conversation, "rasn.llm.agent", "model.complete");
            const agent_response response = model(model_request);
            const std::string value = response.ok ? response.output : response.error.message;
            (void)record_runtime_choice(plan.task.id, "model:" + model_request.request_id, "rasn.model", value);
            finish_runtime_execution(execution, response.ok ? 0 : 1, value);
            return response;
        };
    agent_plan_executor::tool_callback tracked_tool =
        [this, &tool, &plan](const agent_executor_tool_request &tool_request) {
            const runtime_execution execution = begin_runtime_execution("tool",
                                                                        tool_request.request_id,
                                                                        tool_request.tool.name,
                                                                        "rasn.tool.agent",
                                                                        "tool.run");
            const tool_result result = tool(tool_request);
            const std::string value = result.ok ? result.output : result.error;
            (void)record_runtime_choice(plan.task.id, "tool:" + tool_request.request_id, "rasn.tool", value);
            finish_runtime_execution(execution, result.ok ? 0 : 1, value);
            return result;
        };
    const agent_executor_result result = executor.execute(request, plan.executor_options, tracked_model, approve, tracked_tool);
    const std::string status = result.status.empty() ? (result.ok ? "ok" : "failed") : result.status;
    const std::string error = result.error.empty() ? "agent plan execution failed" : result.error;
    if (status == "approval-denied" && !plan.approval_failure_source.empty())
    {
        _services.runtime().record_failure(
            plan.task, plan.approval_failure_category, plan.approval_failure_code, error, false, plan.approval_failure_source);
    }
    _services.runtime().finish_task(plan.task, status);
    finish_runtime_execution(plan_execution, result.ok ? 0 : 1, result.ok ? result.output : error);

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
