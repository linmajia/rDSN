#include "cli_app.h"

#include "agent_clients.h"
#include "agent_registry.h"

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
    cli_startup_context startup;
    const cli_workspace_context_options workspace_options = workspace_context_options();
    if (!bootstrap_single_path_argument(args, commands(), &startup, max_context_bytes(), &workspace_options))
    {
        std::cout << startup.error << "\n";
        return 1;
    }
    if (startup.matched)
    {
        on_startup_context(startup);
    }

    rasn_service_graph_lifecycle_scope lifecycle(_services);
    if (startup.matched)
    {
        std::cout << startup.message << "\n";
        return repl();
    }
    if (args.empty())
    {
        return handle_empty_args();
    }
    if (args[0] == "interactive" || args[0] == "repl")
    {
        return repl();
    }
    return run_command(args);
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

} // namespace rasn
} // namespace dsn
