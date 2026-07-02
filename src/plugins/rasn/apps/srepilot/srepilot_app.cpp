#include "srepilot_app.h"

#include <rasn/cli_support.h>
#include <rasn/observability.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>

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

std::vector<std::string> srepilot_commands()
{
    static const std::vector<std::string> commands = {
        "help", "-h", "--help", "interactive", "repl", "diagnose", "runbook",
        "status", "observe", "selftest", "provider",
    };
    return commands;
}

bool parse_uint32(const std::string &text, uint32_t *value)
{
    if (text.empty())
    {
        return false;
    }

    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0')
    {
        return false;
    }
    *value = static_cast<uint32_t>((std::min)(parsed, 1000ull));
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

srepilot_cli::srepilot_cli() : rasn_cli_app_base(global_rasn_services()) {}

std::vector<std::string> srepilot_cli::commands() const
{
    return srepilot_commands();
}

std::string srepilot_cli::repl_title() const
{
    return "rASN SREPilot prototype";
}

std::string srepilot_cli::repl_prompt() const
{
    return "srepilot> ";
}

std::string srepilot_cli::repl_plain_text_behavior() const
{
    return "treated as diagnose input";
}

void srepilot_cli::on_startup_context(const cli_startup_context &startup)
{
    _startup_context.clear();
    _startup_context.push_back("workspace: " + startup.workspace_root);
    if (!startup.context_text.empty())
    {
        _startup_context.push_back(startup.context_text);
    }
}

int srepilot_cli::handle_empty_args()
{
    print_help(false);
    return 0;
}

void srepilot_cli::handle_plain_text(const std::string &line)
{
    (void)diagnose(std::vector<std::string>{line});
}

int srepilot_cli::run_compat_prompt(const std::string &prompt, bool stream)
{
    (void)stream;
    return diagnose(std::vector<std::string>{prompt});
}

void srepilot_cli::print_compat_help() const
{
    print_help(false);
}

std::string srepilot_cli::version_string() const
{
    return "rASN SREPilot prototype";
}

std::string srepilot_cli::compat_prompt_usage() const
{
    return "usage: --print <incident>";
}

std::string srepilot_cli::compat_dry_run_message() const
{
    return "dry-run: no diagnosis request executed";
}

std::string srepilot_cli::compat_resume_continue_message() const
{
    return "--resume/--continue are accepted for compatibility; SREPilot persists incidents automatically";
}

int srepilot_cli::run_command(const std::vector<std::string> &args, bool interactive_mode)
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
    print_help(interactive_mode);
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

    const std::string context = startup_context_block();
    const std::string prompt =
        "Triage this production incident. Return: severity, blast radius, likely causes, "
        "safe validation checks, immediate mitigation, rollback criteria, and follow-up tasks.\n\nIncident:\n" +
        incident + context;
    agent_completion_request request = make_incident_request("diagnose", incident + context, prompt);
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

    const std::string context = startup_context_block();
    const std::string prompt =
        "Generate an executable SRE runbook for this service or symptom. Include preflight "
        "checks, observability queries, mitigation steps, rollback, escalation triggers, "
        "and post-incident cleanup. Mark destructive actions as operator-approved only.\n\nTarget:\n" +
        symptom + context;
    agent_completion_request request = make_incident_request("runbook", symptom + context, prompt);
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

std::string srepilot_cli::startup_context_block() const
{
    if (_startup_context.empty())
    {
        return "";
    }

    std::ostringstream oss;
    oss << "\n\nStartup context:\n";
    for (size_t i = 0; i < _startup_context.size(); ++i)
    {
        if (i != 0)
        {
            oss << "\n\n";
        }
        oss << _startup_context[i];
    }
    return oss.str();
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

void srepilot_cli::print_help(bool interactive_mode) const
{
    std::cout << "rASN SREPilot commands:\n"
              << cli_help_intro(interactive_mode, "treated as diagnose input")
              << cli_help_item(interactive_mode, "-p, --print [incident]", "run one diagnosis and print the answer")
              << cli_help_item(interactive_mode, "--prompt <incident>", "run one diagnosis without entering the REPL")
              << cli_help_item(interactive_mode, "-m, --model <model>", "select a provider model")
              << cli_help_item(interactive_mode, "--provider <name>", "select an LLM provider")
              << cli_help_item(interactive_mode, "--cwd|--workspace|--dir <path>", "run from a workspace directory")
              << cli_help_item(interactive_mode, "--resume, --continue", "accepted compatibility aliases; incidents are persisted automatically")
              << cli_help_item(interactive_mode, "--dry-run", "parse options without executing a diagnosis")
              << cli_help_item(interactive_mode, "diagnose <incident>", "triage an incident and persist the diagnosis")
              << cli_help_item(interactive_mode, "runbook <symptom>", "generate and persist an SRE runbook")
              << cli_help_item(interactive_mode, "status", "summarize provider, state, and observability health")
              << cli_help_item(interactive_mode, "observe snapshot", "show observability counts")
              << cli_help_item(interactive_mode, "observe events [kind] [limit]", "show recent runtime events")
              << cli_help_item(interactive_mode, "observe failures [limit]", "show classified failures")
              << cli_help_item(interactive_mode, "observe metrics [format]", "dump runtime metrics (text|prometheus|json)")
              << cli_help_item(interactive_mode, "observe resilience", "dump overload/model/tool/remote-agent guards")
              << cli_help_item(interactive_mode, "provider [name]", "show or switch model provider")
              << cli_help_item(interactive_mode, "selftest", "run model/state/observability checks")
              << cli_help_item(interactive_mode, "interactive", "start REPL mode");
}

::dsn::error_code srepilot_app::start(int argc, char **argv)
{
    global_rasn_services().acquire();
    std::vector<std::string> args = cli_args_from_argv(argc, argv);
    size_t begin = 0;
    if (!args.empty() && args[0] == "rasn.srepilot")
    {
        begin = 1;
    }
    _args.assign(args.begin() + begin, args.end());

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
    rasn_cli_service_readiness_options readiness_options;
    readiness_options.dependency_error = "SREPilot service dependencies not ready";
    const int rc =
        wait_for_cli_service_dependencies(global_rasn_services(), readiness_options, &readiness_error)
            ? _cli.run(_args)
            : 1;
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
