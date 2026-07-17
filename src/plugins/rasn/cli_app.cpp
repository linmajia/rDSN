#include <rasn/cli_app.h>

#include <rasn/agent_clients.h>
#include <rasn/agent_registry.h>

#include <dsn/cpp/utils.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <sstream>
#include <thread>
#include <tuple>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace dsn {
namespace rasn {

namespace {

[[noreturn]] void exit_on_allocation_failure() noexcept
{
    static const char message[] = "rASN CLI: memory allocation failed\n";
#if defined(_WIN32)
    (void)::_write(2, message, static_cast<unsigned int>(sizeof(message) - 1));
#else
    (void)::write(STDERR_FILENO, message, sizeof(message) - 1);
#endif
    std::_Exit(EXIT_FAILURE);
}

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

bool parse_uint64_cli(const std::string &text, uint64_t *value)
{
    if (text.empty() || text[0] == '-')
    {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0')
    {
        return false;
    }
    if (value != nullptr)
    {
        *value = static_cast<uint64_t>(parsed);
    }
    return true;
}

void print_state_record(const state_record &record)
{
    std::cout << record.key << " kind=" << record.kind << " scope=" << record.scope
              << " sequence=" << record.sequence << " value=" << record.value << "\n";
}

bool starts_with(const std::string &value, const std::string &prefix)
{
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string resolve_cli_executable_path(const std::string &program)
{
    const auto resolve_existing = [](const std::string &candidate) {
        std::string absolute;
        if (candidate.empty() || !::dsn::utils::filesystem::file_exists(candidate))
        {
            return std::string();
        }
#if !defined(_WIN32)
        struct stat file_status;
        if (::stat(candidate.c_str(), &file_status) != 0 ||
            !S_ISREG(file_status.st_mode) ||
            ::access(candidate.c_str(), X_OK) != 0)
        {
            return std::string();
        }
#endif
        if (::dsn::utils::filesystem::get_absolute_path(candidate, absolute))
        {
            return absolute;
        }
        return std::string();
    };

#if defined(_WIN32)
    std::vector<char> module_path(32768);
    const DWORD module_path_size = ::GetModuleFileNameA(
        nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (module_path_size > 0 && module_path_size < module_path.size())
    {
        const std::string resolved =
            resolve_existing(std::string(module_path.data(), module_path_size));
        if (!resolved.empty())
        {
            return resolved;
        }
    }
#endif

    if (program.empty())
    {
        return "";
    }

    const bool has_directory =
        program.find('/') != std::string::npos || program.find('\\') != std::string::npos
#if defined(_WIN32)
        || program.find(':') != std::string::npos
#endif
        ;
    if (has_directory)
    {
        return resolve_existing(program);
    }

    const char *path_value = std::getenv("PATH");
    if (path_value == nullptr)
    {
        return "";
    }

    std::istringstream paths(path_value);
    std::string directory;
#if defined(_WIN32)
    constexpr char path_separator = ';';
#else
    constexpr char path_separator = ':';
#endif
    while (std::getline(paths, directory, path_separator))
    {
        if (directory.empty())
        {
            directory = ".";
        }
        const std::string candidate =
            ::dsn::utils::filesystem::path_combine(directory, program);
#if defined(_WIN32)
        std::string resolved = resolve_existing(candidate + ".exe");
        if (!resolved.empty())
        {
            return resolved;
        }
#else
        std::string resolved;
#endif
        resolved = resolve_existing(candidate);
        if (!resolved.empty())
        {
            return resolved;
        }
    }
    return "";
}

std::string to_lower_ascii(std::string value)
{
    for (char &c : value)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

std::string config_string_or_default(const char *section,
                                     const char *key,
                                     const char *fallback,
                                     const char *description)
{
    const char *value = ::dsn_config_get_value_string(section, key, fallback, description);
    return value == nullptr ? fallback : value;
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

std::vector<std::string> normalize_slash_command_args(std::vector<std::string> args)
{
    if (!args.empty() && args[0] == "?")
    {
        args[0] = "help";
    }
    return args;
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

std::string join_strings(const std::vector<std::string> &values, const std::string &delimiter)
{
    std::ostringstream output;
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (i != 0)
        {
            output << delimiter;
        }
        output << values[i];
    }
    return output.str();
}

} // namespace

int run_rasn_state_command(rasn_service_graph &services,
                           const std::vector<std::string> &args)
{
    if (args.empty())
    {
        std::cout << "usage: state put|get|query|checkpoint|recover|compact|migrate|prune ...\n";
        return 1;
    }

    state_response response;
    if (args[0] == "put")
    {
        if (args.size() < 3)
        {
            std::cout << "usage: state put <scope/key> <value>\n";
            return 1;
        }
        state_record record;
        record.key = args[1];
        record.value = join_args(args, 2);
        record.kind = "memory";
        record.scope = record.key.substr(0, record.key.find('/'));
        response = services.put_state(record);
    }
    else if (args[0] == "get")
    {
        if (args.size() != 2)
        {
            std::cout << "usage: state get <scope/key>\n";
            return 1;
        }
        state_key_request request;
        request.key = args[1];
        response = services.get_state(request);
    }
    else if (args[0] == "query")
    {
        state_query_request request;
        request.key_prefix = args.size() > 1 ? args[1] : "";
        response = services.query_state(request);
    }
    else if (args[0] == "checkpoint")
    {
        state_checkpoint_request request;
        request.path = args.size() > 1 ? args[1] : "";
        response = services.checkpoint_state(request);
    }
    else if (args[0] == "recover")
    {
        state_checkpoint_request request;
        request.path = args.size() > 1 ? args[1] : "";
        response = services.recover_state(request);
    }
    else if (args[0] == "compact")
    {
        std::string checkpoint_path;
        std::string state_prefix;
        for (size_t i = 1; i < args.size(); ++i)
        {
            if (args[i] == "--prefix")
            {
                if (i + 1 >= args.size() || !state_prefix.empty())
                {
                    std::cout << "usage: state compact [--prefix <state-prefix>] [checkpoint-path]\n";
                    return 1;
                }
                state_prefix = args[++i];
            }
            else if (args[i].compare(0, 9, "--prefix=") == 0)
            {
                if (!state_prefix.empty())
                {
                    std::cout << "usage: state compact [--prefix <state-prefix>] [checkpoint-path]\n";
                    return 1;
                }
                state_prefix = args[i].substr(9);
            }
            else if (checkpoint_path.empty())
            {
                checkpoint_path = args[i];
            }
            else
            {
                std::cout << "usage: state compact [--prefix <state-prefix>] [checkpoint-path]\n";
                return 1;
            }
        }
        const rasn_runtime_state_compaction_report report =
            compact_rasn_runtime_state_mirror(
                services, checkpoint_path, state_prefix);
        if (!report.ok)
        {
            std::cout << "state compact failed: " << report.error << "\n";
            return 1;
        }
        std::cout << "state compact ok: prefix=" << report.state_prefix
                  << " runtime_records=" << report.runtime_records
                  << " watermarks=" << report.watermark_records
                  << " queried=" << report.queried_records
                  << " checkpoint_records=" << report.checkpointed_records
                  << " last_sequence=" << report.last_sequence
                  << " recovery_journal_compacted="
                  << (report.compaction_details_available
                          ? (report.recovery_journal_compacted ? "yes" : "no")
                          : "unknown")
                  << " checkpoint="
                  << (report.checkpoint_path.empty()
                          ? "<state-service default>"
                          : report.checkpoint_path)
                  << "\n";
        return 0;
    }
    else if (args[0] == "migrate")
    {
        std::string checkpoint_path;
        std::string state_prefix = load_rasn_runtime_config().state_prefix;
        bool apply = false;
        for (size_t i = 1; i < args.size(); ++i)
        {
            if (args[i] == "--prefix")
            {
                if (i + 1 >= args.size())
                {
                    std::cout << "usage: state migrate <checkpoint-path> [--prefix <state-prefix>] [--apply]\n";
                    return 1;
                }
                state_prefix = args[++i];
            }
            else if (args[i] == "--apply")
            {
                apply = true;
            }
            else if (checkpoint_path.empty() && !args[i].empty() &&
                     args[i][0] != '-')
            {
                checkpoint_path = args[i];
            }
            else
            {
                std::cout << "usage: state migrate <checkpoint-path> [--prefix <state-prefix>] [--apply]\n";
                return 1;
            }
        }
        const state_migration_report report =
            migrate_state_checkpoint(services, checkpoint_path, state_prefix, apply);
        std::cout << "state migrate " << (report.ok ? "ok" : "failed")
                  << (apply ? " (apply)" : " (dry-run)")
                  << ": prefix=" << report.key_prefix
                  << " source_records=" << report.source_records
                  << " target_records=" << report.target_records
                  << " planned=" << report.planned_records
                  << " unchanged=" << report.unchanged_records
                  << " migrated=" << report.migrated_records
                  << " sequence_advance="
                  << (report.sequence_advance_required ? "yes" : "no")
                  << " conflicts=" << report.conflict_keys.size() << "\n";
        if (!report.ok)
        {
            std::cout << report.error << "\n";
            return 1;
        }
        if (!apply &&
            (report.planned_records != 0 ||
             report.sequence_advance_required))
        {
            std::cout << "dry-run only; rerun with --apply after reviewing the target\n";
        }
        return 0;
    }
    else if (args[0] == "prune")
    {
        std::string key_prefix;
        uint64_t max_sequence = 0;
        bool have_max_sequence = false;
        bool apply = false;
        for (size_t i = 1; i < args.size(); ++i)
        {
            if (args[i] == "--prefix" && i + 1 < args.size() &&
                key_prefix.empty())
            {
                key_prefix = args[++i];
            }
            else if (args[i] == "--max-sequence" && i + 1 < args.size() &&
                     !have_max_sequence)
            {
                have_max_sequence =
                    parse_uint64_cli(args[++i], &max_sequence) &&
                    max_sequence != 0;
                if (!have_max_sequence)
                {
                    std::cout << "state prune requires a non-zero uint64 --max-sequence\n";
                    return 1;
                }
            }
            else if (args[i] == "--apply")
            {
                apply = true;
            }
            else
            {
                std::cout << "usage: state prune --prefix <obsolete-prefix> --max-sequence <cutoff> [--apply]\n";
                return 1;
            }
        }
        if (key_prefix.empty() || !have_max_sequence)
        {
            std::cout << "usage: state prune --prefix <obsolete-prefix> --max-sequence <cutoff> [--apply]\n";
            return 1;
        }

        state_query_request query;
        query.key_prefix = key_prefix;
        const state_response existing = services.query_state(query);
        if (!existing.ok)
        {
            std::cout << "state prune failed: " << existing.error << "\n";
            return 1;
        }
        if (max_sequence > existing.last_sequence)
        {
            std::cout << "state prune failed: maximum sequence " << max_sequence
                      << " exceeds current sequence " << existing.last_sequence
                      << "\n";
            return 1;
        }
        size_t eligible = 0;
        for (const state_record &record : existing.records)
        {
            eligible += record.sequence <= max_sequence ? 1 : 0;
        }
        if (!apply)
        {
            std::cout << "state prune dry-run: prefix=" << key_prefix
                      << " eligible=" << eligible
                      << " preserved_newer=" << existing.records.size() - eligible
                      << " max_sequence=" << max_sequence << "\n"
                      << "This is a logical delete, not checkpoint compaction; rerun with --apply only for explicitly obsolete keys.\n";
            return 0;
        }

        state_delete_prefix_request request;
        request.key_prefix = key_prefix;
        request.max_sequence = max_sequence;
        response = services.delete_state_prefix(request);
        if (!response.ok)
        {
            std::cout << "state error: " << response.error << "\n";
            return 1;
        }
        std::cout << "state prune ok (apply): prefix=" << key_prefix
                  << " deleted=" << response.records.size()
                  << " max_sequence=" << max_sequence
                  << " last_sequence=" << response.last_sequence << "\n";
        return 0;
    }
    else
    {
        std::cout << "unknown state command: " << args[0] << "\n";
        return 1;
    }

    if (!response.ok)
    {
        std::cout << "state error: " << response.error << "\n";
        return 1;
    }
    if (args[0] == "put")
    {
        std::cout << "stored ";
        print_state_record(response.record);
    }
    else if (args[0] == "get")
    {
        print_state_record(response.record);
    }
    else if (args[0] == "query")
    {
        for (const state_record &record : response.records)
        {
            print_state_record(record);
        }
        std::cout << "records=" << response.records.size()
                  << " last_sequence=" << response.last_sequence << "\n";
    }
    else if (args[0] == "checkpoint")
    {
        std::cout << "checkpointed records=" << response.records.size()
                  << " last_sequence=" << response.last_sequence << "\n";
    }
    else if (args[0] == "recover")
    {
        std::cout << "recovered records=" << response.records.size()
                  << " last_sequence=" << response.last_sequence << "\n";
    }
    return 0;
}

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

std::string find_rasn_cli_config_file(const std::string &program, const std::string &filename)
{
    const std::string executable = resolve_cli_executable_path(program);
    if (executable.empty() || filename.empty())
    {
        return "";
    }

    const std::string executable_dir =
        ::dsn::utils::filesystem::remove_file_name(executable);
    if (executable_dir.empty())
    {
        return "";
    }

    const std::string beside_executable =
        ::dsn::utils::filesystem::path_combine(executable_dir, filename);
    if (::dsn::utils::filesystem::file_exists(beside_executable))
    {
        return beside_executable;
    }

    const std::string parent_dir =
        ::dsn::utils::filesystem::remove_file_name(executable_dir);
    if (!parent_dir.empty())
    {
        const std::string beside_target =
            ::dsn::utils::filesystem::path_combine(parent_dir, filename);
        if (::dsn::utils::filesystem::file_exists(beside_target))
        {
            return beside_target;
        }
    }
    return "";
}

void install_rasn_cli_out_of_memory_handler()
{
    std::set_new_handler(exit_on_allocation_failure);
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

bool attach_cli_runtime_client_node()
{
    // A one-shot CLI thread has no rDSN service-node context, which remote/hybrid
    // runtime module RPC requires. With [core] enable_default_app_mimic = true (set
    // in every app config.ini) rDSN auto-registers a no-op [apps.mimic] node; when
    // the CLI started it (-app_list mimic), attaching this thread to it gives the
    // CLI a lightweight client node so calls to a remote runtime host resolve.
    // Returns false (with a warning) if the mimic node is unavailable, in which case
    // remote runtime calls will fail closed with a missing-node-context error.
    if (::dsn_mimic_app("mimic", 1))
    {
        return true;
    }
    fprintf(stderr,
            "rasn: warning: could not attach a client runtime node (no [apps.mimic] node "
            "started); distributed/hybrid runtime calls may fail. Ensure [core] "
            "enable_default_app_mimic = true.\n");
    return false;
}

std::string align_working_directory_to_runtime_config(const std::string &config_path)
{
    if (config_path.empty())
    {
        return config_path;
    }

    std::string absolute;
    if (!::dsn::utils::filesystem::get_absolute_path(config_path, absolute) || absolute.empty())
    {
        absolute = config_path;
    }

    const std::string config_dir = ::dsn::utils::filesystem::remove_file_name(absolute);
    if (config_dir.empty())
    {
        return absolute;
    }

    // rDSN resolves a config file's `@include <relative>` against the process
    // working directory, so switch into the runtime config's own directory before
    // handing it to dsn_run. Passing the absolute config path keeps the main file
    // openable after the chdir while its sibling `@include config.rasn.defaults.ini`
    // now resolves beside it. Chdir'ing into the directory that already is the CWD
    // is a harmless no-op, so this only changes behavior for launches from another
    // directory.
    // Windows resolves the include the same CWD-relative way, so chdir there too
    // (via ::_chdir); guarding this block to POSIX previously made runtime-host
    // config alignment a no-op and left the include bound to the caller's directory.
#if defined(_WIN32)
    const int chdir_rc = ::_chdir(config_dir.c_str());
#else
    const int chdir_rc = ::chdir(config_dir.c_str());
#endif
    if (chdir_rc != 0)
    {
        fprintf(stderr,
                "rasn: warning: could not switch to runtime config directory '%s' (%s); an "
                "@include in %s will resolve against the current directory instead\n",
                config_dir.c_str(),
                std::strerror(errno),
                absolute.c_str());
    }
    return absolute;
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
    : _services(services)
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
        std::string command = normalized_args[0].substr(1);
        if (command == "?")
        {
            command = "help";
        }
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
            const int rc = run_tracked_command(normalize_slash_command_args(split_words(line.substr(1))), true);
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
    configure_runtime_module_mode();

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
    if (!_rasn_runtime->upsert_agent(record, &error))
    {
        warn_runtime_module_failure("agent_control_plane", error);
    }
    else
    {
        const uint64_t now_ms = ::dsn_now_ms();
        const std::string owner = cli_agent_id() + ".process";
        const agent_control_lease lease =
            _rasn_runtime->acquire_agent_lease(record.descriptor.agent_id, owner, now_ms, 0);
        if (!lease.ok)
        {
            warn_runtime_module_failure("agent_control_plane", lease.error);
        }
        if (!_rasn_runtime->heartbeat_agent(record.descriptor.agent_id, now_ms, &error))
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
    if (!_rasn_runtime->upsert_capability_provider(provider, &error))
    {
        warn_runtime_module_failure("capability_directory", error);
    }

    resource_quota quota;
    quota.scope = cli_agent_id();
    quota.max_tool_calls = 100000;
    if (!_rasn_runtime->configure_budget(quota, &error))
    {
        warn_runtime_module_failure("resource_budget", error);
    }

    recovery_policy fallback_policy;
    fallback_policy.failure_class = "*";
    fallback_policy.max_attempts = 1;
    fallback_policy.retryable = false;
    if (!_rasn_runtime->set_recovery_policy(fallback_policy, &error))
    {
        warn_runtime_module_failure("recovery_supervisor", error);
    }
    recovery_policy transient_policy;
    transient_policy.failure_class = "transient";
    transient_policy.max_attempts = 3;
    transient_policy.retry_delay_ms = 100;
    transient_policy.escalate_after_attempts = 3;
    transient_policy.retryable = true;
    if (!_rasn_runtime->set_recovery_policy(transient_policy, &error))
    {
        warn_runtime_module_failure("recovery_supervisor", error);
    }

    agent_contract command_contract;
    command_contract.contract_id = "cli.command";
    command_contract.require_input_non_empty = false;
    command_contract.require_output_non_empty = false;
    if (!_rasn_runtime->register_contract(command_contract, &error))
    {
        warn_runtime_module_failure("contract_verifier", error);
    }

    _runtime_modules_initialized = true;
}

void rasn_cli_app_base::heartbeat_runtime_modules()
{
    const uint64_t now_ms = ::dsn_now_ms();
    const size_t expired = _rasn_runtime->expire_agent_leases(now_ms);
    if (expired != 0)
    {
        dinfo("expired %llu rASN runtime agent lease(s)", static_cast<unsigned long long>(expired));
    }
    std::string error;
    if (!_rasn_runtime->heartbeat_agent(cli_agent_id(), now_ms, &error))
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
    const resource_budget_decision budget = _rasn_runtime->reserve_budget(execution.budget);
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
    if (!_rasn_runtime->add_task(task, &error))
    {
        warn_runtime_module_failure("task_orchestration", error);
    }
    else if (!_rasn_runtime->start_task(task.task_id, cli_agent_id(), &error))
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
    if (!_rasn_runtime->publish_message(message, &stored, &error))
    {
        warn_runtime_module_failure("agent_message_bus", error);
    }
    else
    {
        execution.message_id = stored.message_id;
    }

    deterministic_choice choice;
    if (!_rasn_runtime->record_choice(execution.task_id, "route", "rasn.cli", kind + ":" + name, &choice, &error))
    {
        warn_runtime_module_failure("determinism_ledger", error);
    }

    blackboard_entry input_entry;
    input_entry.key = "task/" + execution.task_id + "/input";
    input_entry.kind = kind + ".input";
    input_entry.owner = cli_agent_id();
    input_entry.value = input;
    input_entry.tags.push_back(kind);
    if (!_rasn_runtime->put_blackboard(input_entry, nullptr, &error))
    {
        warn_runtime_module_failure("blackboard", error);
    }

    const contract_evaluation input_contract = _rasn_runtime->evaluate_input("cli.command", input);
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
    if (!_rasn_runtime->record_choice(
            execution.task_id, "exit_code", "rasn.cli", std::to_string(exit_code), &choice, &error))
    {
        warn_runtime_module_failure("determinism_ledger", error);
    }

    if (exit_code == 0)
    {
        if (!_rasn_runtime->complete_task(execution.task_id, detail, &error))
        {
            warn_runtime_module_failure("task_orchestration", error);
        }
        if (!execution.message_id.empty() && !_rasn_runtime->ack_message(execution.message_id, &error))
        {
            warn_runtime_module_failure("agent_message_bus", error);
        }
    }
    else
    {
        if (!_rasn_runtime->fail_task(execution.task_id, detail, false, &error))
        {
            warn_runtime_module_failure("task_orchestration", error);
        }
        if (!execution.message_id.empty() && !_rasn_runtime->dead_letter_message(execution.message_id, detail, &error))
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
        (void)_rasn_runtime->observe_failure(failure);
    }

    blackboard_entry output_entry;
    output_entry.key = "task/" + execution.task_id + "/output";
    output_entry.kind = "cli.output";
    output_entry.owner = cli_agent_id();
    output_entry.value = detail;
    output_entry.tags.push_back(exit_code == 0 ? "ok" : "failed");
    if (!_rasn_runtime->put_blackboard(output_entry, nullptr, &error))
    {
        warn_runtime_module_failure("blackboard", error);
    }

    const contract_evaluation output_contract =
        _rasn_runtime->evaluate_output("cli.command", detail, std::vector<std::string>());
    if (!output_contract.ok)
    {
        warn_runtime_module_failure("contract_verifier", "output contract violation for " + execution.task_id);
    }
    if (execution.budget_reserved && !_rasn_runtime->release_budget(execution.budget, &error))
    {
        warn_runtime_module_failure("resource_budget", error);
    }
    heartbeat_runtime_modules();
}

std::string rasn_cli_app_base::runtime_modules_summary() const
{
    std::ostringstream output;
    const sandbox_profile sandbox = _rasn_runtime->sandbox();
    const std::vector<agent_control_record> agents = _rasn_runtime->list_agents(false, ::dsn_now_ms());
    const std::vector<agent_message> messages = _rasn_runtime->message_snapshot();
    const std::vector<orchestration_task> tasks = _rasn_runtime->task_snapshot();
    const std::vector<deterministic_choice> choices = _rasn_runtime->choice_snapshot();
    const std::vector<blackboard_entry> blackboard = _rasn_runtime->blackboard_snapshot(false, ::dsn_now_ms());
    const std::vector<human_interaction_request> human = _rasn_runtime->human_snapshot();
    output << "rASN runtime modules\n"
           << _rasn_runtime->summary_header() << "\n"
           << "agent_control_plane: agents=" << agents.size() << "\n"
           << _rasn_runtime->describe_agents(::dsn_now_ms()) << "\n"
           << "agent_message_bus: messages=" << messages.size() << "\n"
           << "task_orchestration_kernel: tasks=" << tasks.size()
           << " ready=" << _rasn_runtime->ready_tasks(::dsn_now_ms()).size()
           << " blocked=" << _rasn_runtime->blocked_tasks().size() << "\n"
           << "determinism_ledger: choices=" << choices.size() << "\n"
           << "sandbox_runtime: profile=" << sandbox.name
           << " fs_read=" << (sandbox.allow_filesystem_read ? "yes" : "no")
           << " fs_write=" << (sandbox.allow_filesystem_write ? "yes" : "no")
           << " network=" << (sandbox.allow_network ? "yes" : "no")
           << " process=" << (sandbox.allow_process_spawn ? "yes" : "no") << "\n"
           << "capability_directory: " << _rasn_runtime->describe_capabilities() << "\n"
           << "resource_budget: " << _rasn_runtime->describe_budgets() << "\n"
           << "recovery_supervisor: " << _rasn_runtime->describe_recovery() << "\n"
           << "blackboard: entries=" << blackboard.size() << "\n"
           << "contract_verifier: " << _rasn_runtime->describe_contracts() << "\n"
           << "human_interaction: requests=" << human.size()
           << " pending=" << _rasn_runtime->pending_human().size() << "\n";
    return output.str();
}

std::string rasn_cli_app_base::runtime_modules_topology() const
{
    configure_runtime_module_mode();
    if (_rasn_runtime == nullptr)
    {
        return "runtime_topology: unavailable";
    }
    return _rasn_runtime->describe_topology();
}

bool rasn_cli_app_base::runtime_modules_ready(std::string *detail) const
{
    // Ping every runtime module through the runtime facade. In local mode
    // this resolves in-process; in distributed mode it verifies each module's
    // service endpoint is actually reachable on its (possibly remote) node, so a
    // down module surfaces here instead of being masked by a static summary.
    configure_runtime_module_mode();

    if (_rasn_runtime == nullptr)
    {
        if (detail != nullptr)
        {
            *detail = "rASN runtime is not configured";
        }
        return false;
    }

    const std::vector<std::pair<std::string, bool>> health = _rasn_runtime->module_health();
    std::vector<std::string> unreachable;
    for (const std::pair<std::string, bool> &entry : health)
    {
        if (!entry.second)
        {
            unreachable.push_back(entry.first);
        }
    }
    if (unreachable.empty())
    {
        if (detail != nullptr)
        {
            std::ostringstream oss;
            oss << "all " << health.size() << " rASN runtime modules reachable (provider="
                << _rasn_runtime->provider_name() << ", mode="
                << (_rasn_runtime->distributed() ? "distributed" : "local") << ")";
            *detail = oss.str();
        }
        return true;
    }

    if (detail != nullptr)
    {
        std::ostringstream oss;
        oss << "unreachable:";
        for (const std::string &module : unreachable)
        {
            oss << " " << module;
        }
        *detail = oss.str();
    }
    return false;
}

void rasn_cli_app_base::configure_runtime_module_mode() const
{
    if (_rasn_runtime == nullptr)
    {
        _rasn_runtime = create_rasn_runtime(_services, load_rasn_runtime_config());
    }
}

deterministic_replay_result rasn_cli_app_base::record_runtime_choice(const std::string &task_id,
                                                                    const std::string &key,
                                                                    const std::string &source,
                                                                    const std::string &value)
{
    deterministic_replay_result result;
    std::string error;
    deterministic_choice choice;
    configure_runtime_module_mode();
    if (!_rasn_runtime->record_choice(task_id, key, source, value, &choice, &error))
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
    configure_runtime_module_mode();
    return _rasn_runtime->evaluate_sandbox(request);
}

void rasn_cli_app_base::set_cli_sandbox_profile(const sandbox_profile &profile)
{
    configure_runtime_module_mode();
    _rasn_runtime->set_sandbox_profile(profile);
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
