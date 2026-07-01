#include "srepilot_app.h"

#include "../../agent_clients.h"
#include "../../agent_registry.h"
#include "../../observability.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <thread>
#include <tuple>

namespace dsn {
namespace rasn {
namespace {

class service_graph_lifecycle_scope
{
public:
    explicit service_graph_lifecycle_scope(rasn_service_graph &services) : _services(services) { _services.acquire(); }
    ~service_graph_lifecycle_scope() { _services.release(); }

    service_graph_lifecycle_scope(const service_graph_lifecycle_scope &) = delete;
    service_graph_lifecycle_scope &operator=(const service_graph_lifecycle_scope &) = delete;

private:
    rasn_service_graph &_services;
};

void append_readiness_error(std::vector<std::string> *errors,
                            const std::string &component,
                            const std::string &detail)
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
    request.key_prefix = "__srepilot_readiness_probe__";
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
    source.workflow_id = "srepilot-readiness";
    source.source_name = "<srepilot-readiness>";
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
        *error = "SREPilot service dependencies not ready";
        if (!last_errors.empty())
        {
            *error += ": " + readiness_errors_summary(last_errors);
        }
    }
    return false;
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

bool parse_uint32(const std::string &text, uint32_t *value)
{
    if (text.empty())
    {
        return false;
    }

    char *end = nullptr;
    const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0')
    {
        return false;
    }

    *value = static_cast<uint32_t>(std::min<unsigned long>(parsed, 1000UL));
    return true;
}

std::string srepilot_system_prompt()
{
    return "You are SREPilot, an rASN incident-response agent. "
           "Produce concise, operationally safe guidance. Separate facts, hypotheses, "
           "checks, mitigation, rollback, and follow-up. Avoid destructive actions unless "
           "the operator explicitly asks for them.";
}

agent_completion_request make_incident_request(const std::string &command,
                                               const std::string &input,
                                               const std::string &user_prompt)
{
    agent_completion_request request;
    request.task.id = "srepilot-" + make_trace_id();
    request.task.name = "srepilot." + command;
    request.task.input = input;
    request.system_prompt = srepilot_system_prompt();
    request.user_prompt = user_prompt;
    request.timeout_ms = 60000;
    request.retry_budget = 1;
    return request;
}

std::string state_line(const state_record &record)
{
    std::ostringstream oss;
    oss << record.key << " kind=" << record.kind << " scope=" << record.scope
        << " sequence=" << record.sequence;
    return oss.str();
}

void print_check(bool ok, const std::string &name, const std::string &detail, bool *all_ok)
{
    if (!ok)
    {
        *all_ok = false;
    }
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name;
    if (!detail.empty())
    {
        std::cout << " - " << detail;
    }
    std::cout << "\n";
}

} // namespace

srepilot_cli::srepilot_cli() : _services(global_rasn_services()) {}

int srepilot_cli::run(const std::vector<std::string> &args)
{
    service_graph_lifecycle_scope lifecycle(_services);
    if (args.empty())
    {
        print_help();
        return 0;
    }
    if (args[0] == "interactive" || args[0] == "repl")
    {
        return repl();
    }
    return run_command(args);
}

int srepilot_cli::repl()
{
    std::cout << "rASN SREPilot prototype\n";
    std::cout << _services.provider_summary() << "\n";
    std::cout << "Type /help for commands. Plain text is treated as diagnose input.\n";

    std::string line;
    while (true)
    {
        if (_shutdown_requested.load())
        {
            return 0;
        }

        std::cout << "srepilot> ";
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
            const int rc = run_command(split_words(line.substr(1)));
            if (rc != 0)
            {
                std::cout << "command failed: " << rc << "\n";
            }
            continue;
        }

        diagnose(std::vector<std::string>{line});
    }
}

int srepilot_cli::run_command(const std::vector<std::string> &args)
{
    if (args.empty() || args[0] == "help" || args[0] == "-h" || args[0] == "--help")
    {
        print_help();
        return 0;
    }
    if (args[0] == "diagnose")
    {
        return diagnose(std::vector<std::string>(args.begin() + 1, args.end()));
    }
    if (args[0] == "runbook")
    {
        return runbook(std::vector<std::string>(args.begin() + 1, args.end()));
    }
    if (args[0] == "status")
    {
        return status();
    }
    if (args[0] == "observe")
    {
        return observe(std::vector<std::string>(args.begin() + 1, args.end()));
    }
    if (args[0] == "selftest")
    {
        return selftest();
    }
    if (args[0] == "provider")
    {
        return set_provider(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    std::cout << "unknown command: " << args[0] << "\n";
    print_help();
    return 1;
}

int srepilot_cli::diagnose(const std::vector<std::string> &args)
{
    const std::string incident = join_args(args, 0);
    if (incident.empty())
    {
        std::cout << "usage: diagnose <incident summary, alerts, logs, or symptoms>\n";
        return 1;
    }

    const std::string prompt =
        "Triage this production incident. Return: severity, blast radius, likely causes, "
        "safe validation checks, immediate mitigation, rollback criteria, and follow-up tasks.\n\nIncident:\n" +
        incident;
    agent_completion_request request = make_incident_request("diagnose", incident, prompt);
    const llm_response response = _services.complete(request);
    if (!response.ok)
    {
        std::cout << "diagnosis failed: " << response.error << "\n";
        return 1;
    }

    std::cout << response.text << "\n";
    std::string key;
    if (!persist_response("diagnosis", request.task, incident, response.text, &key))
    {
        return 1;
    }
    std::cout << "\nstored " << key << "\n";
    return 0;
}

int srepilot_cli::runbook(const std::vector<std::string> &args)
{
    const std::string symptom = join_args(args, 0);
    if (symptom.empty())
    {
        std::cout << "usage: runbook <service, symptom, or incident class>\n";
        return 1;
    }

    const std::string prompt =
        "Generate an executable SRE runbook for this service or symptom. Include preflight "
        "checks, observability queries, mitigation steps, rollback, escalation triggers, "
        "and post-incident cleanup. Mark destructive actions as operator-approved only.\n\nTarget:\n" +
        symptom;
    agent_completion_request request = make_incident_request("runbook", symptom, prompt);
    const llm_response response = _services.complete(request);
    if (!response.ok)
    {
        std::cout << "runbook generation failed: " << response.error << "\n";
        return 1;
    }

    std::cout << response.text << "\n";
    std::string key;
    if (!persist_response("runbook", request.task, symptom, response.text, &key))
    {
        return 1;
    }
    std::cout << "\nstored " << key << "\n";
    return 0;
}

int srepilot_cli::status()
{
    std::cout << "SREPilot status\n";
    std::cout << _services.provider_summary() << "\n";

    const model_gateway_response health = _services.model_health();
    std::cout << "model: " << (health.ok ? "ok" : "failed");
    if (health.ok)
    {
        std::cout << " provider=" << health.provider.provider << " model=" << health.provider.model;
    }
    else
    {
        std::cout << " error=" << health.error;
    }
    std::cout << "\n";

    const observability_response snapshot = _services.observability_snapshot();
    if (snapshot.ok)
    {
        std::cout << "observability: events=" << snapshot.events.size()
                  << " failures=" << snapshot.failures.size()
                  << " last_sequence=" << snapshot.last_sequence
                  << (snapshot.truncated ? " truncated" : "") << "\n";
    }
    else
    {
        std::cout << "observability: failed " << snapshot.error << "\n";
    }

    state_query_request query;
    query.key_prefix = "srepilot/";
    const state_checkpoint_request recover = configured_state_recovery_request();
    if (configured_state_recovery_available(recover))
    {
        const state_response recovered = _services.recover_state(recover);
        if (!recovered.ok)
        {
            std::cout << "incident recovery: failed " << recovered.error << "\n";
            return 1;
        }
        std::cout << "incident recovery: records=" << recovered.records.size()
                  << " last_sequence=" << recovered.last_sequence << "\n";
    }
    const state_response state = _services.query_state(query);
    if (state.ok)
    {
        std::cout << "incident records: " << state.records.size()
                  << " last_sequence=" << state.last_sequence << "\n";
    }
    else
    {
        std::cout << "incident records: query failed " << state.error << "\n";
    }
    return health.ok && snapshot.ok ? 0 : 1;
}

int srepilot_cli::observe(const std::vector<std::string> &args)
{
    const std::string mode = args.empty() ? "snapshot" : args[0];
    if (mode == "snapshot")
    {
        const observability_response snapshot = _services.observability_snapshot();
        if (!snapshot.ok)
        {
            std::cout << "snapshot failed: " << snapshot.error << "\n";
            return 1;
        }
        std::cout << "events=" << snapshot.events.size() << " failures=" << snapshot.failures.size()
                  << " last_sequence=" << snapshot.last_sequence
                  << (snapshot.truncated ? " truncated" : "") << "\n";
        return 0;
    }
    if (mode == "events")
    {
        observability_query_request query;
        query.limit = 20;
        if (args.size() > 1)
        {
            query.kind = args[1];
        }
        if (args.size() > 2 && !parse_uint32(args[2], &query.limit))
        {
            std::cout << "usage: observe events [kind] [limit]\n";
            return 1;
        }
        const observability_response response = _services.query_events(query);
        if (!response.ok)
        {
            std::cout << "event query failed: " << response.error << "\n";
            return 1;
        }
        for (const runtime_event &event : response.events)
        {
            std::cout << format_observability_event(event) << "\n";
        }
        return 0;
    }
    if (mode == "failures")
    {
        observability_query_request query;
        query.limit = 20;
        if (args.size() > 1 && !parse_uint32(args[1], &query.limit))
        {
            std::cout << "usage: observe failures [limit]\n";
            return 1;
        }
        const observability_response response = _services.query_failures(query);
        if (!response.ok)
        {
            std::cout << "failure query failed: " << response.error << "\n";
            return 1;
        }
        for (const failure_record &failure : response.failures)
        {
            std::cout << format_failure_record(failure) << "\n";
        }
        return 0;
    }
    if (mode == "metrics")
    {
        const std::string format = args.size() > 1 ? args[1] : "text";
        const metrics_snapshot snapshot = _services.runtime_metrics();
        if (format == "prometheus")
        {
            std::cout << snapshot.to_prometheus();
        }
        else if (format == "json")
        {
            std::cout << snapshot.to_json();
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
    if (mode == "resilience")
    {
        std::cout << _services.resilience_report() << "\n";
        return 0;
    }

    std::cout << "usage: observe [snapshot|events [kind] [limit]|failures [limit]|metrics [format]|resilience]\n";
    return 1;
}

int srepilot_cli::selftest()
{
    bool ok = true;
    std::cout << "SREPilot self-test\n";

    const model_gateway_response health = _services.model_health();
    print_check(health.ok,
                "model gateway health",
                health.ok ? health.provider.provider + "/" + health.provider.model : health.error,
                &ok);

    agent_completion_request request = make_incident_request(
        "selftest", "checkout latency is elevated", "Return a one-line incident triage self-test response.");
    const llm_response model = _services.complete(request);
    print_check(model.ok && !model.text.empty(),
                "incident model invoke",
                model.ok ? model.text.substr(0, 120) : model.error,
                &ok);

    state_record record;
    record.key = "srepilot/selftest/" + request.task.id;
    record.kind = "srepilot.selftest";
    record.scope = "srepilot.incidents";
    record.value = model.ok ? model.text : "model failed";
    const state_response put = _services.put_state(record);
    print_check(put.ok, "state put", put.ok ? state_line(put.record) : put.error, &ok);

    state_key_request get_request;
    get_request.key = record.key;
    const state_response get = _services.get_state(get_request);
    print_check(get.ok && get.record.value == record.value,
                "state get",
                get.ok ? state_line(get.record) : get.error,
                &ok);

    const observability_response snapshot = _services.observability_snapshot();
    std::ostringstream observed;
    if (snapshot.ok)
    {
        observed << "events=" << snapshot.events.size() << " failures=" << snapshot.failures.size();
    }
    print_check(snapshot.ok, "observability snapshot", snapshot.ok ? observed.str() : snapshot.error, &ok);

    const std::string resilience = _services.resilience_report();
    print_check(!resilience.empty(), "resilience report", resilience.empty() ? "" : "available", &ok);

    if (ok)
    {
        std::cout << "SREPilot self-test passed\n";
    }
    return ok ? 0 : 1;
}

int srepilot_cli::set_provider(const std::vector<std::string> &args)
{
    if (args.empty())
    {
        std::cout << _services.provider_summary() << "\n";
        return 0;
    }
    if (args.size() > 1)
    {
        std::cout << "usage: provider [name]\n";
        return 1;
    }

    const model_gateway_response response = _services.set_provider(args[0]);
    if (!response.ok)
    {
        std::cout << response.error << "\n";
        return 1;
    }
    std::cout << "provider=" << response.provider.provider << " model=" << response.provider.model << "\n";
    return 0;
}

bool srepilot_cli::recover_state_for_persist(std::string *error)
{
    if (_state_recovered_for_persist)
    {
        return true;
    }

    const state_checkpoint_request recover = configured_state_recovery_request();
    if (configured_state_recovery_available(recover))
    {
        const state_response recovered = _services.recover_state(recover);
        if (!recovered.ok)
        {
            if (error != nullptr)
            {
                *error = recovered.error;
            }
            return false;
        }
    }

    _state_recovered_for_persist = true;
    return true;
}

bool srepilot_cli::persist_response(const std::string &kind,
                                    const agent_task &task,
                                    const std::string &input,
                                    const std::string &output,
                                    std::string *stored_key)
{
    std::string recovery_error;
    if (!recover_state_for_persist(&recovery_error))
    {
        std::cout << "state recovery failed: " << recovery_error << "\n";
        return false;
    }

    state_record record;
    record.key = "srepilot/" + kind + "/" + task.id;
    record.kind = "srepilot." + kind;
    record.scope = "srepilot.incidents";
    record.value = "input:\n" + input + "\n\noutput:\n" + output;

    const state_response stored = _services.put_state(record);
    if (!stored.ok)
    {
        std::cout << "state persistence failed: " << stored.error << "\n";
        return false;
    }

    state_checkpoint_request checkpoint;
    const state_response checkpointed = _services.checkpoint_state(checkpoint);
    if (!checkpointed.ok)
    {
        std::cout << "state checkpoint failed: " << checkpointed.error << "\n";
        return false;
    }
    if (stored_key != nullptr)
    {
        std::ostringstream detail;
        detail << state_line(stored.record) << " checkpoint_records=" << checkpointed.records.size();
        *stored_key = detail.str();
    }
    return true;
}

void srepilot_cli::print_help() const
{
    std::cout << "rASN SREPilot commands:\n"
              << "  diagnose <incident>      triage an incident and persist the diagnosis\n"
              << "  runbook <symptom>        generate and persist an SRE runbook\n"
              << "  status                   summarize provider, state, and observability health\n"
              << "  observe snapshot         show observability counts\n"
              << "  observe events [kind] [limit] show recent runtime events\n"
              << "  observe failures [limit] show classified failures\n"
              << "  observe metrics [format] dump runtime metrics (text|prometheus|json)\n"
              << "  observe resilience       dump overload/model/tool/remote-agent guards\n"
              << "  provider [name]          show or switch model provider\n"
              << "  selftest                 run model/state/observability checks\n"
              << "  interactive              start REPL mode\n";
}

::dsn::error_code srepilot_app::start(int argc, char **argv)
{
    global_rasn_services().acquire();
    _args.clear();
    int begin = 0;
    if (argc > 0 && argv[0] != nullptr && std::string(argv[0]) == "rasn.srepilot")
    {
        begin = 1;
    }
    for (int i = begin; i < argc; ++i)
    {
        _args.push_back(argv[i] == nullptr ? "" : argv[i]);
    }

    _cli_task = ::dsn::tasking::enqueue(
        LPC_RASN_SREPILOT_START, nullptr, [this] { run_cli_task(); }, 0, std::chrono::milliseconds(100));
    if (_cli_task == nullptr)
    {
        global_rasn_services().release();
        return ::dsn::ERR_UNKNOWN;
    }
    return ::dsn::ERR_OK;
}

::dsn::error_code srepilot_app::stop(bool cleanup)
{
    if (_cli_task != nullptr)
    {
        _cli.request_shutdown();
        _cli_task->cancel(false);
        _cli_task = nullptr;
    }
    global_rasn_services().release();
    return ::dsn::ERR_OK;
}

void srepilot_app::run_cli_task()
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
        derror("SREPilot CLI exited with code %d", rc);
    }
}

} // namespace rasn
} // namespace dsn
