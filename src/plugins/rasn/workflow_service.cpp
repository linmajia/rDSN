#include "workflow_service.h"

#include "agent_registry.h"
#include "agent_services.h"
#include "state_service.h"

#include <dsn/service_api_cpp.h>
#include <dsn/tool-api/task.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

namespace dsn {
namespace rasn {

namespace {

bool valid_schema(uint32_t schema_version)
{
    return schema_version == RASN_AGENT_SCHEMA_VERSION;
}

std::string workflow_identity(const workflow_source &source)
{
    if (!source.workflow_id.empty())
    {
        return source.workflow_id;
    }
    if (!source.source_name.empty())
    {
        return source.source_name;
    }
    return "inline";
}

uint64_t workflow_execution_lease_ms()
{
    return (std::max)(
        static_cast<uint64_t>(1),
        ::dsn_config_get_value_uint64(
            "rasn.workflow", "execution_lease_ms", 600000, "workflow execution ownership lease in milliseconds"));
}

uint64_t workflow_execution_lease_renew_ms()
{
    const uint64_t lease_ms = workflow_execution_lease_ms();
    const uint64_t configured = ::dsn_config_get_value_uint64(
        "rasn.workflow", "execution_lease_renew_ms", 0, "workflow execution lease renewal interval in milliseconds");
    const uint64_t requested = configured == 0 ? (std::max)(static_cast<uint64_t>(1), lease_ms / 3) : configured;
    return (std::max)(static_cast<uint64_t>(1), (std::min)(requested, (std::max)(static_cast<uint64_t>(1), lease_ms / 2)));
}

bool has_workflow_timer_context()
{
    return ::dsn::task::get_current_node2() != nullptr;
}

uint64_t workflow_cancel_probe_ms()
{
    // How often a running executor re-reads the shared run record to observe a
    // cancellation that was routed to a different instance than the lease
    // holder. 0 disables the cross-instance probe (single-instance deployments
    // do not need it and avoid the extra state read).
    return ::dsn_config_get_value_uint64(
        "rasn.workflow", "cancel_probe_ms", 2000, "cross-instance workflow cancellation probe interval in milliseconds");
}

struct workflow_execution_lease
{
    std::string run_id;
    std::string workflow_id;
    std::string owner;
    std::string status;
    uint64_t expires_ms = 0;
    uint64_t sequence = 0;
};

std::string workflow_lease_key(const std::string &run_id)
{
    return "workflow-lease/" + run_id;
}

state_response default_workflow_state_put(const state_put_request &request)
{
    return global_rasn_services().put_state(request);
}

state_response workflow_state_put(const state_put_request &request);

state_response workflow_state_put(const state_record &record)
{
    state_put_request request;
    request.record = record;
    return workflow_state_put(request);
}

state_response default_workflow_state_get(const state_key_request &request)
{
    return global_rasn_services().get_state(request);
}

state_response default_workflow_state_query(const state_query_request &request)
{
    return global_rasn_services().query_state(request);
}

::dsn::service::zlock &workflow_state_reader_lock()
{
    static ::dsn::service::zlock lock;
    return lock;
}

workflow_state_getter &workflow_state_getter_slot()
{
    static workflow_state_getter getter = &default_workflow_state_get;
    return getter;
}

workflow_state_queryer &workflow_state_queryer_slot()
{
    static workflow_state_queryer queryer = &default_workflow_state_query;
    return queryer;
}

workflow_state_writer &workflow_state_writer_slot()
{
    static workflow_state_writer writer = &default_workflow_state_put;
    return writer;
}

state_response workflow_state_put(const state_put_request &request)
{
    workflow_state_writer writer = nullptr;
    {
        ::dsn::service::zauto_lock guard(workflow_state_reader_lock());
        writer = workflow_state_writer_slot();
    }
    return writer(request);
}

state_response workflow_state_get(const state_key_request &request)
{
    workflow_state_getter getter = nullptr;
    {
        ::dsn::service::zauto_lock guard(workflow_state_reader_lock());
        getter = workflow_state_getter_slot();
    }
    return getter(request);
}

state_response workflow_state_query(const state_query_request &request)
{
    workflow_state_queryer queryer = nullptr;
    {
        ::dsn::service::zauto_lock guard(workflow_state_reader_lock());
        queryer = workflow_state_queryer_slot();
    }
    return queryer(request);
}

bool state_key_not_found(const state_response &response, const std::string &key)
{
    return !response.ok && response.error == "state key not found: " + key;
}

bool workflow_state_service_ready()
{
    if (!global_rasn_services().rpc_clients_enabled())
    {
        return true;
    }

    rasn_state_client state(global_rasn_services().state_address());
    state_query_request request;
    request.key_prefix = "__rasn_workflow_recovery_probe__";
    ::dsn::error_code err;
    state_response response;
    std::tie(err, response) = state.query_sync(request, std::chrono::milliseconds(250));
    return err == ::dsn::ERR_OK && response.ok;
}

void recover_workflow_state_after_start()
{
    for (int attempt = 0; attempt < 40; ++attempt)
    {
        if (workflow_state_service_ready())
        {
            const workflow_response recovered = global_workflow_store().recover_from_state();
            if (!recovered.ok)
            {
                dwarn("workflow startup state recovery skipped: %s", recovered.error.c_str());
            }
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    dwarn("workflow startup state recovery skipped: rasn.state was not ready");
}

void copy_lease_fields(const workflow_run_record &source, workflow_run_record *target)
{
    if (target == nullptr)
    {
        return;
    }
    target->execution_owner = source.execution_owner;
    target->lease_key = source.lease_key;
    target->lease_sequence = source.lease_sequence;
    target->lease_expires_ms = source.lease_expires_ms;
}

// Header fields are serialized one per line as key=value, so any newline in a
// value (LLM/tool/compiler errors routinely carry stack traces and stderr)
// would otherwise split into lines the line-by-line decoder rejects, corrupting
// the record and aborting recovery. Escape newlines (and the escape character)
// on write and reverse it on read so multi-line values survive a round trip on
// a single physical line.
std::string escape_state_field(const std::string &value)
{
    std::string out;
    out.reserve(value.size());
    for (char c : value)
    {
        switch (c)
        {
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

std::string unescape_state_field(const std::string &value)
{
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i)
    {
        if (value[i] == '\\' && i + 1 < value.size())
        {
            const char next = value[i + 1];
            if (next == '\\')
            {
                out += '\\';
                ++i;
                continue;
            }
            if (next == 'n')
            {
                out += '\n';
                ++i;
                continue;
            }
            if (next == 'r')
            {
                out += '\r';
                ++i;
                continue;
            }
        }
        out += value[i];
    }
    return out;
}

std::string record_value(const workflow_run_record &record)
{
    std::ostringstream oss;
    oss << "run_id=" << record.run_id << "\n"
        << "workflow_id=" << escape_state_field(record.workflow_id) << "\n"
        << "source_name=" << escape_state_field(record.source_name) << "\n"
        << "status=" << record.status << "\n"
        << "sequence=" << record.sequence << "\n";
    if (!record.execution_owner.empty())
    {
        oss << "execution_owner=" << record.execution_owner << "\n";
    }
    if (!record.lease_key.empty())
    {
        oss << "lease_key=" << record.lease_key << "\n";
    }
    if (record.lease_sequence != 0)
    {
        oss << "lease_sequence=" << record.lease_sequence << "\n";
    }
    if (record.lease_expires_ms != 0)
    {
        oss << "lease_expires_ms=" << record.lease_expires_ms << "\n";
    }
    if (!record.error.empty())
    {
        oss << "error=" << escape_state_field(record.error) << "\n";
    }
    if (!record.plan.empty())
    {
        oss << "plan=\n" << record.plan << "\n";
    }
    if (!record.result_text.empty())
    {
        oss << "result=\n" << record.result_text;
    }
    return oss.str();
}

std::string lease_value(const workflow_execution_lease &lease)
{
    std::ostringstream oss;
    oss << "run_id=" << lease.run_id << "\n"
        << "workflow_id=" << lease.workflow_id << "\n"
        << "owner=" << lease.owner << "\n"
        << "status=" << lease.status << "\n"
        << "expires_ms=" << lease.expires_ms << "\n";
    return oss.str();
}

std::string node_state_key(const std::string &run_id, const std::string &node_id)
{
    return "workflow-node/" + run_id + "/" + node_id;
}

std::string node_status_value(const std::string &run_id,
                              const std::string &workflow_id,
                              const workflow_node_status &status)
{
    std::ostringstream oss;
    oss << "run_id=" << run_id << "\n"
        << "workflow_id=" << escape_state_field(workflow_id) << "\n"
        << "node_id=" << status.node_id << "\n"
        << "action=" << status.action << "\n"
        << "status=" << status.status << "\n";
    if (!status.error.empty())
    {
        oss << "error=" << escape_state_field(status.error) << "\n";
    }
    if (!status.output.empty())
    {
        oss << "output=\n" << status.output;
    }
    return oss.str();
}

bool terminal_status(const std::string &status)
{
    return status == "completed" || status == "failed" || status == "cancelled";
}

bool consume_prefix(const std::string &line, const std::string &prefix, std::string *value)
{
    if (line.find(prefix) != 0)
    {
        return false;
    }
    if (value != nullptr)
    {
        *value = line.substr(prefix.size());
    }
    return true;
}

bool parse_uint64(const std::string &value, uint64_t *result)
{
    if (value.empty())
    {
        return false;
    }

    errno = 0;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0')
    {
        return false;
    }
    if (result != nullptr)
    {
        *result = static_cast<uint64_t>(parsed);
    }
    return true;
}

bool decode_workflow_execution_lease(const state_record &state, workflow_execution_lease *lease, std::string *error)
{
    if (state.kind != "workflow.lease")
    {
        if (error != nullptr)
        {
            *error = "state record is not a workflow lease: " + state.key;
        }
        return false;
    }

    workflow_execution_lease decoded;
    decoded.sequence = state.sequence;
    std::istringstream input(state.value);
    std::string line;
    while (std::getline(input, line))
    {
        std::string value;
        if (consume_prefix(line, "run_id=", &value))
        {
            decoded.run_id = value;
        }
        else if (consume_prefix(line, "workflow_id=", &value))
        {
            decoded.workflow_id = value;
        }
        else if (consume_prefix(line, "owner=", &value))
        {
            decoded.owner = value;
        }
        else if (consume_prefix(line, "status=", &value))
        {
            decoded.status = value;
        }
        else if (consume_prefix(line, "expires_ms=", &value))
        {
            if (!parse_uint64(value, &decoded.expires_ms))
            {
                if (error != nullptr)
                {
                    *error = "workflow lease has invalid expiration: " + state.key;
                }
                return false;
            }
        }
        else if (!line.empty())
        {
            if (error != nullptr)
            {
                *error = "workflow lease has unknown field in " + state.key + ": " + line;
            }
            return false;
        }
    }

    if (decoded.run_id.empty() || decoded.owner.empty() || decoded.status.empty() || decoded.expires_ms == 0)
    {
        if (error != nullptr)
        {
            *error = "workflow lease is incomplete: " + state.key;
        }
        return false;
    }

    const std::string expected_key = workflow_lease_key(decoded.run_id);
    if (state.key != expected_key)
    {
        if (error != nullptr)
        {
            *error = "workflow lease key/run id mismatch: " + state.key;
        }
        return false;
    }

    if (lease != nullptr)
    {
        *lease = decoded;
    }
    return true;
}

void renew_workflow_execution_lease(const std::shared_ptr<struct workflow_lease_renewal_state> &state);

struct workflow_lease_renewal_state
{
    explicit workflow_lease_renewal_state(const workflow_run_record &source) : record(source) {}

    mutable ::dsn::service::zlock lock;
    workflow_run_record record;
    bool stopped = false;
    bool failed = false;
    std::string error;
};

void renew_workflow_execution_lease(const std::shared_ptr<workflow_lease_renewal_state> &state)
{
    workflow_run_record current;
    {
        ::dsn::service::zauto_lock guard(state->lock);
        if (state->stopped || state->failed)
        {
            return;
        }
        current = state->record;
    }

    if (current.lease_key.empty() || current.execution_owner.empty() || current.lease_sequence == 0)
    {
        return;
    }

    workflow_execution_lease lease;
    lease.run_id = current.run_id;
    lease.workflow_id = current.workflow_id;
    lease.owner = current.execution_owner;
    lease.status = "running";
    lease.expires_ms = ::dsn_now_ms() + workflow_execution_lease_ms();

    state_record lease_record;
    lease_record.key = current.lease_key;
    lease_record.kind = "workflow.lease";
    lease_record.scope = "rasn.workflow";
    lease_record.value = lease_value(lease);

    state_put_request request;
    request.record = lease_record;
    request.check_sequence = true;
    request.expected_sequence = current.lease_sequence;
    const state_response response = workflow_state_put(request);

    ::dsn::service::zauto_lock guard(state->lock);
    if (!response.ok)
    {
        state->failed = true;
        state->error = response.error;
        dwarn("failed to renew workflow lease run=%s owner=%s: %s",
              current.run_id.c_str(),
              current.execution_owner.c_str(),
              response.error.c_str());
        return;
    }
    state->record.lease_sequence = response.record.sequence;
    state->record.lease_expires_ms = lease.expires_ms;
}

class workflow_lease_renewal_guard
{
public:
    explicit workflow_lease_renewal_guard(const workflow_run_record &record)
        : _state(std::make_shared<workflow_lease_renewal_state>(record))
    {
        if (record.lease_key.empty() || record.execution_owner.empty() || record.lease_sequence == 0)
        {
            return;
        }
        const uint64_t interval_ms = workflow_execution_lease_renew_ms();
        if (!has_workflow_timer_context())
        {
            std::shared_ptr<workflow_lease_renewal_state> state = _state;
            _fallback_thread = std::thread([state, interval_ms]() {
                uint64_t elapsed_ms = 0;
                while (true)
                {
                    const uint64_t step_ms = (std::min)(static_cast<uint64_t>(50), interval_ms - elapsed_ms);
                    std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
                    {
                        ::dsn::service::zauto_lock guard(state->lock);
                        if (state->stopped || state->failed)
                        {
                            return;
                        }
                    }
                    elapsed_ms += step_ms;
                    if (elapsed_ms < interval_ms)
                    {
                        continue;
                    }
                    elapsed_ms = 0;
                    renew_workflow_execution_lease(state);
                }
            });
            return;
        }

        const std::chrono::milliseconds interval(interval_ms);
        std::shared_ptr<workflow_lease_renewal_state> state = _state;
        _timer = ::dsn::tasking::enqueue_timer(
            LPC_RASN_WORKFLOW_LEASE_RENEW_TIMER,
            nullptr,
            [state]() { renew_workflow_execution_lease(state); },
            interval,
            0,
            interval);
        if (_timer == nullptr)
        {
            dwarn("failed to start workflow lease renewal timer run=%s", record.run_id.c_str());
        }
    }

    ~workflow_lease_renewal_guard() { stop(); }

    void stop()
    {
        {
            ::dsn::service::zauto_lock guard(_state->lock);
            _state->stopped = true;
        }
        if (_timer != nullptr)
        {
            _timer->cancel(true);
            _timer = nullptr;
        }
        if (_fallback_thread.joinable())
        {
            _fallback_thread.join();
        }
    }

    workflow_run_record snapshot() const
    {
        ::dsn::service::zauto_lock guard(_state->lock);
        return _state->record;
    }

    // True once a lease renewal attempt has failed: the executor has (or is
    // about to) lose ownership, so execution must abort rather than continue as
    // a split-brain writer against a lease another owner may have taken over.
    bool failed() const
    {
        ::dsn::service::zauto_lock guard(_state->lock);
        return _state->failed;
    }

private:
    std::shared_ptr<workflow_lease_renewal_state> _state;
    ::dsn::task_ptr _timer;
    std::thread _fallback_thread;
};

void append_multiline_value(std::string *target, const std::string &line)
{
    if (target == nullptr)
    {
        return;
    }
    if (!target->empty())
    {
        *target += "\n";
    }
    *target += line;
}

bool workflow_run_id_from_state_key(const std::string &key, std::string *run_id)
{
    static const std::string prefix = "workflow/";
    if (key.find(prefix) != 0 || key.size() == prefix.size())
    {
        return false;
    }
    if (run_id != nullptr)
    {
        *run_id = key.substr(prefix.size());
    }
    return true;
}

bool workflow_node_id_from_state_key(const std::string &key, const std::string &run_id, std::string *node_id)
{
    const std::string prefix = "workflow-node/" + run_id + "/";
    if (key.find(prefix) != 0 || key.size() == prefix.size())
    {
        return false;
    }
    if (node_id != nullptr)
    {
        *node_id = key.substr(prefix.size());
    }
    return true;
}

bool decode_workflow_run_record(const state_record &state, workflow_run_record *record, std::string *error)
{
    if (state.kind != "workflow")
    {
        if (error != nullptr)
        {
            *error = "state record is not a workflow run: " + state.key;
        }
        return false;
    }

    std::string key_run_id;
    if (!workflow_run_id_from_state_key(state.key, &key_run_id))
    {
        if (error != nullptr)
        {
            *error = "workflow state key is invalid: " + state.key;
        }
        return false;
    }

    workflow_run_record decoded;
    decoded.state_key = state.key;
    decoded.sequence = state.sequence;

    bool parsed_sequence = false;
    std::string section;
    std::istringstream input(state.value);
    std::string line;
    while (std::getline(input, line))
    {
        if (section == "plan")
        {
            if (line == "result=")
            {
                section = "result";
            }
            else
            {
                append_multiline_value(&decoded.plan, line);
            }
            continue;
        }
        if (section == "result")
        {
            append_multiline_value(&decoded.result_text, line);
            continue;
        }

        if (line == "plan=")
        {
            section = "plan";
            continue;
        }
        if (line == "result=")
        {
            section = "result";
            continue;
        }

        std::string value;
        if (consume_prefix(line, "run_id=", &value))
        {
            decoded.run_id = value;
        }
        else if (consume_prefix(line, "workflow_id=", &value))
        {
            decoded.workflow_id = unescape_state_field(value);
        }
        else if (consume_prefix(line, "source_name=", &value))
        {
            decoded.source_name = unescape_state_field(value);
        }
        else if (consume_prefix(line, "status=", &value))
        {
            decoded.status = value;
        }
        else if (consume_prefix(line, "sequence=", &value))
        {
            if (!parse_uint64(value, &decoded.sequence))
            {
                if (error != nullptr)
                {
                    *error = "workflow state has invalid sequence: " + state.key;
                }
                return false;
            }
            parsed_sequence = true;
        }
        else if (consume_prefix(line, "execution_owner=", &value))
        {
            decoded.execution_owner = value;
        }
        else if (consume_prefix(line, "lease_key=", &value))
        {
            decoded.lease_key = value;
        }
        else if (consume_prefix(line, "lease_sequence=", &value))
        {
            if (!parse_uint64(value, &decoded.lease_sequence))
            {
                if (error != nullptr)
                {
                    *error = "workflow state has invalid lease sequence: " + state.key;
                }
                return false;
            }
        }
        else if (consume_prefix(line, "lease_expires_ms=", &value))
        {
            if (!parse_uint64(value, &decoded.lease_expires_ms))
            {
                if (error != nullptr)
                {
                    *error = "workflow state has invalid lease expiration: " + state.key;
                }
                return false;
            }
        }
        else if (consume_prefix(line, "error=", &value))
        {
            decoded.error = unescape_state_field(value);
        }
        else if (!line.empty())
        {
            if (error != nullptr)
            {
                *error = "workflow state has unknown field in " + state.key + ": " + line;
            }
            return false;
        }
    }

    if (decoded.run_id.empty())
    {
        decoded.run_id = key_run_id;
    }
    if (decoded.run_id != key_run_id)
    {
        if (error != nullptr)
        {
            *error = "workflow state key/run id mismatch: " + state.key;
        }
        return false;
    }
    if (decoded.status.empty())
    {
        if (error != nullptr)
        {
            *error = "workflow state missing status: " + state.key;
        }
        return false;
    }
    if (!parsed_sequence && decoded.sequence == 0)
    {
        if (error != nullptr)
        {
            *error = "workflow state missing sequence: " + state.key;
        }
        return false;
    }

    if (record != nullptr)
    {
        *record = decoded;
    }
    return true;
}

bool decode_workflow_node_status(const state_record &state,
                                 const std::string &expected_run_id,
                                 workflow_node_status *status,
                                 std::string *workflow_id,
                                 std::string *error)
{
    if (state.kind != "workflow.node")
    {
        if (error != nullptr)
        {
            *error = "state record is not a workflow node: " + state.key;
        }
        return false;
    }

    std::string key_node_id;
    if (!workflow_node_id_from_state_key(state.key, expected_run_id, &key_node_id))
    {
        if (error != nullptr)
        {
            *error = "workflow node state key is invalid for run " + expected_run_id + ": " + state.key;
        }
        return false;
    }

    std::string decoded_run_id;
    std::string decoded_workflow_id;
    workflow_node_status decoded;
    std::string section;
    std::istringstream input(state.value);
    std::string line;
    while (std::getline(input, line))
    {
        if (section == "output")
        {
            append_multiline_value(&decoded.output, line);
            continue;
        }

        if (line == "output=")
        {
            section = "output";
            continue;
        }

        std::string value;
        if (consume_prefix(line, "run_id=", &value))
        {
            decoded_run_id = value;
        }
        else if (consume_prefix(line, "workflow_id=", &value))
        {
            decoded_workflow_id = unescape_state_field(value);
        }
        else if (consume_prefix(line, "node_id=", &value))
        {
            decoded.node_id = value;
        }
        else if (consume_prefix(line, "action=", &value))
        {
            decoded.action = value;
        }
        else if (consume_prefix(line, "status=", &value))
        {
            decoded.status = value;
        }
        else if (consume_prefix(line, "error=", &value))
        {
            decoded.error = unescape_state_field(value);
        }
        else if (!line.empty())
        {
            if (error != nullptr)
            {
                *error = "workflow node state has unknown field in " + state.key + ": " + line;
            }
            return false;
        }
    }

    if (decoded_run_id.empty())
    {
        decoded_run_id = expected_run_id;
    }
    if (decoded.node_id.empty())
    {
        decoded.node_id = key_node_id;
    }
    if (decoded_run_id != expected_run_id || decoded.node_id != key_node_id)
    {
        if (error != nullptr)
        {
            *error = "workflow node state key/body mismatch: " + state.key;
        }
        return false;
    }
    if (decoded.status.empty())
    {
        if (error != nullptr)
        {
            *error = "workflow node state missing status: " + state.key;
        }
        return false;
    }

    if (status != nullptr)
    {
        *status = decoded;
    }
    if (workflow_id != nullptr)
    {
        *workflow_id = decoded_workflow_id;
    }
    return true;
}

std::string compiled_plan(const workflow_graph &graph, bool include_registry)
{
    std::string plan = graph.describe_plan();
    if (include_registry)
    {
        plan += "\nRegistry snapshot:\n" + global_agent_registry().describe();
    }
    return plan;
}

class service_graph_provider : public llm_provider
{
public:
    std::string name() const override { return "rasn.coordinator"; }
    std::string model() const override { return "workflow-service"; }
    model_provider_descriptor describe() const override
    {
        model_provider_descriptor descriptor;
        descriptor.provider = name();
        descriptor.model = model();
        descriptor.endpoint = "rasn.coordinator";
        descriptor.payload_format = "agent_request";
        descriptor.local = true;
        descriptor.health = "healthy";
        return descriptor;
    }

    llm_response complete(const llm_request &request, nucleus_runtime &runtime) override
    {
        agent_request generic;
        generic.request_id = request.task_id.empty() ? make_trace_id() : request.task_id;
        generic.trace_id = runtime.trace_id();
        generic.workflow_node_id = request.task_id;
        generic.capability = "model.complete";
        generic.input = request.user_prompt;
        generic.timeout_ms = request.timeout_ms;
        generic.retry_budget = request.retry_budget;
        generic.policy_labels = request.policy_labels;
        generic.task.id = generic.request_id;
        generic.task.name = "workflow.node";
        generic.task.input = request.user_prompt;

        if (!request.system_prompt.empty())
        {
            agent_context_entry entry;
            entry.kind = "system_prompt";
            entry.name = "system";
            entry.value = request.system_prompt;
            generic.context.push_back(entry);
        }
        for (const std::string &context : request.context)
        {
            agent_context_entry entry;
            entry.kind = "text";
            entry.name = "context";
            entry.value = context;
            generic.context.push_back(entry);
        }

        return make_llm_response_from_agent(global_rasn_services().invoke(generic));
    }
};

} // namespace

workflow_response workflow_store::validate(const workflow_source &source)
{
    workflow_graph graph;
    workflow_response response;
    if (!parse_source(source, &graph, &response))
    {
        return response;
    }

    response.run.workflow_id = workflow_identity(source);
    response.run.source_name = source.source_name;
    response.run.status = "validated";
    response.run.plan = compiled_plan(graph, false);
    return response;
}

workflow_response workflow_store::compile(const workflow_source &source)
{
    workflow_graph graph;
    workflow_response response;
    if (!parse_source(source, &graph, &response))
    {
        return response;
    }

    response.run.workflow_id = workflow_identity(source);
    response.run.source_name = source.source_name;
    response.run.status = "compiled";
    response.run.plan = compiled_plan(graph, true);
    return response;
}

workflow_response workflow_store::start(const workflow_start_request &request)
{
    if (!valid_schema(request.schema_version))
    {
        return error_response("workflow start request has unsupported schema version");
    }

    workflow_start_request normalized = request;
    if (normalized.resume && normalized.run_id.empty())
    {
        return error_response("workflow resume missing run id");
    }
    if (normalized.run_id.empty())
    {
        normalized.run_id = make_trace_id();
    }

    workflow_graph graph;
    workflow_response response;
    if (!parse_source(normalized.source, &graph, &response))
    {
        return response;
    }

    workflow_run_record existing;
    bool has_existing = false;
    {
        ::dsn::service::zauto_lock guard(_lock);
        const std::map<std::string, workflow_run_record>::const_iterator it = _runs.find(normalized.run_id);
        if (it != _runs.end())
        {
            existing = it->second;
            has_existing = true;
        }
    }
    if (!has_existing)
    {
        std::string recover_error;
        has_existing = recover_run_from_state(normalized.run_id, &existing, &recover_error);
        if (!has_existing && !recover_error.empty())
        {
            return error_response(recover_error);
        }
    }
    if (has_existing && !normalized.resume)
    {
        return error_response("workflow run already exists: " + normalized.run_id);
    }
    if (!has_existing && normalized.resume)
    {
        return error_response("workflow run not found for resume: " + normalized.run_id);
    }

    const std::string run_id = normalized.run_id;
    const std::string workflow_id = workflow_identity(normalized.source);
    if (has_existing && existing.workflow_id != workflow_id)
    {
        return error_response("workflow resume source mismatch for run " + normalized.run_id +
                              ": expected " + existing.workflow_id + " got " + workflow_id);
    }

    workflow_graph::workflow_resume_state resume_state;
    if (normalized.resume)
    {
        std::string resume_error;
        if (!recover_resume_state_from_state(normalized.run_id, workflow_id, &resume_state, &resume_error))
        {
            return error_response(resume_error);
        }
    }

    workflow_run_record running = make_record(normalized, "running");
    running.plan = compiled_plan(graph, true);
    const workflow_response lease_response = acquire_execution_lease(run_id, workflow_id, &running);
    if (!lease_response.ok)
    {
        return lease_response;
    }
    workflow_response running_response = store_record(running);
    if (!running_response.ok)
    {
        release_execution_lease(running, "failed");
        return running_response;
    }
    running = running_response.run;

    workflow_lease_renewal_guard lease_renewer(running);
    service_graph_provider provider;

    // Best-effort terminal-failed finalization used when execution throws. It
    // stops lease renewal, records a "failed" terminal state under the lease we
    // still hold, and releases the lease. If ownership was already lost (CAS
    // conflict / renewal failure) the persist and release simply no-op, leaving
    // the authoritative owner to finalize, and we surface the failure locally.
    auto finalize_failed = [this, &lease_renewer, running](const std::string &message) -> workflow_response {
        lease_renewer.stop();
        workflow_run_record failed = running;
        copy_lease_fields(lease_renewer.snapshot(), &failed);
        failed.status = "failed";
        failed.error = message;
        const workflow_response persisted = store_record_if_current(failed, running.sequence);
        if (!persisted.run.run_id.empty())
        {
            release_execution_lease(persisted.run, "failed");
            return persisted;
        }
        release_execution_lease(failed, "failed");
        return error_response(message);
    };

    // Cross-instance cancellation probe throttle. A cancel routed to a different
    // instance than the lease holder only lands in shared state, so the running
    // executor periodically re-reads its run record to observe it.
    struct cancel_probe_state
    {
        ::dsn::service::zlock lock;
        uint64_t last_ms = 0;
    };
    auto probe = std::make_shared<cancel_probe_state>();
    const uint64_t probe_interval_ms = workflow_cancel_probe_ms();

    auto should_cancel = [this, run_id, &lease_renewer, probe, probe_interval_ms]() -> bool {
        // Same-instance cancellation (cheap, in-memory).
        if (is_cancelled(run_id, nullptr))
        {
            return true;
        }
        // Lost the execution lease (renewal failed): abort rather than run on as
        // a split-brain writer against a lease another owner may have taken.
        if (lease_renewer.failed())
        {
            return true;
        }
        // Cross-instance cancellation: re-read the shared run record, throttled.
        if (probe_interval_ms == 0)
        {
            return false;
        }
        const uint64_t now = ::dsn_now_ms();
        {
            ::dsn::service::zauto_lock guard(probe->lock);
            if (probe->last_ms != 0 && now - probe->last_ms < probe_interval_ms)
            {
                return false;
            }
            probe->last_ms = now;
        }
        workflow_run_record remote;
        std::string probe_error;
        if (recover_run_from_state(run_id, &remote, &probe_error) && remote.status == "cancelled")
        {
            return true;
        }
        return false;
    };

    workflow_result result;
    try
    {
        result = graph.execute(provider,
                               global_rasn_services().runtime(),
                               [](const std::string &name,
                                  const std::vector<std::string> &args,
                                  nucleus_runtime &runtime,
                                  const agent_task &task,
                                  uint32_t timeout_ms) {
                                 return global_rasn_services().run_tool(name, args, task, timeout_ms);
                               },
                               [this, run_id, workflow_id](const workflow_node_status &status) {
                                  persist_node_status(run_id, workflow_id, status);
                               },
                               should_cancel,
                               resume_state);
    }
    catch (const std::exception &ex)
    {
        return finalize_failed(std::string("workflow execution error: ") + ex.what());
    }
    catch (...)
    {
        return finalize_failed("workflow execution error: unknown exception");
    }

    lease_renewer.stop();
    const workflow_run_record lease_snapshot = lease_renewer.snapshot();
    workflow_run_record cancelled;
    if (is_cancelled(run_id, &cancelled))
    {
        copy_lease_fields(lease_snapshot, &cancelled);
        release_execution_lease(cancelled, "cancelled");
        workflow_response cancelled_response;
        cancelled_response.run = cancelled;
        return cancelled_response;
    }

    workflow_run_record completed = running;
    copy_lease_fields(lease_snapshot, &completed);
    if (result.cancelled)
    {
        completed.status = "cancelled";
        completed.error = result.error.empty() ? "cancelled by request" : result.error;
    }
    else if (result.ok)
    {
        completed.status = "completed";
        completed.result_text = result.text;
    }
    else
    {
        completed.status = "failed";
        completed.error = result.error;
    }

    workflow_response completed_response = store_record_if_current(completed, running.sequence);
    if (!completed_response.run.run_id.empty())
    {
        release_execution_lease(completed_response.run, completed.status);
    }
    else
    {
        // The completion CAS failed (e.g. a cancel on another instance wrote a
        // terminal record first) or we lost ownership. We may still hold the
        // lease, so release it with the fields captured above; the release
        // no-ops if ownership was already lost. This keeps the lease from
        // leaking and blocking resume/restart until it expires.
        release_execution_lease(completed, completed.status);
    }
    return completed_response;
}

workflow_response workflow_store::query(const workflow_run_query &request)
{
    if (!valid_schema(request.schema_version))
    {
        return error_response("workflow query request has unsupported schema version");
    }
    if (request.run_id.empty())
    {
        return error_response("workflow query missing run id");
    }

    {
        ::dsn::service::zauto_lock guard(_lock);
        const std::map<std::string, workflow_run_record>::const_iterator it = _runs.find(request.run_id);
        if (it != _runs.end())
        {
            workflow_response response;
            response.run = it->second;
            return response;
        }
    }

    workflow_run_record recovered;
    std::string recover_error;
    if (recover_run_from_state(request.run_id, &recovered, &recover_error))
    {
        workflow_response response;
        response.run = recovered;
        return response;
    }
    if (!recover_error.empty())
    {
        return error_response(recover_error);
    }
    return error_response("workflow run not found: " + request.run_id);
}

workflow_response workflow_store::cancel(const workflow_run_query &request)
{
    if (!valid_schema(request.schema_version))
    {
        return error_response("workflow cancel request has unsupported schema version");
    }
    if (request.run_id.empty())
    {
        return error_response("workflow cancel missing run id");
    }

    workflow_run_record current;
    bool found = false;
    {
        ::dsn::service::zauto_lock guard(_lock);
        const std::map<std::string, workflow_run_record>::const_iterator it = _runs.find(request.run_id);
        if (it != _runs.end())
        {
            current = it->second;
            found = true;
        }
    }

    if (!found)
    {
        std::string recover_error;
        if (recover_run_from_state(request.run_id, &current, &recover_error))
        {
            found = true;
        }
        else if (!recover_error.empty())
        {
            return error_response(recover_error);
        }
    }
    if (!found)
    {
        return error_response("workflow run not found: " + request.run_id);
    }

    if (terminal_status(current.status))
    {
        return error_response("workflow run is already terminal: " + request.run_id);
    }

    workflow_run_record cancelled = current;
    cancelled.status = "cancelled";
    cancelled.error = "cancelled by request";
    if (cancelled.state_key.empty())
    {
        cancelled.state_key = "workflow/" + cancelled.run_id;
    }
    {
        ::dsn::service::zauto_lock guard(_lock);
        cancelled.sequence = (std::max)(_last_sequence, current.sequence) + 1;
        _last_sequence = (std::max)(_last_sequence, cancelled.sequence);
    }

    const state_response persisted = persist_to_state(cancelled, true, current.sequence);
    if (!persisted.ok)
    {
        return error_response("workflow cancel state persist failed for run " + request.run_id + ": " + persisted.error);
    }

    {
        ::dsn::service::zauto_lock guard(_lock);
        _runs[cancelled.run_id] = cancelled;
        _last_sequence = (std::max)(_last_sequence, cancelled.sequence);
    }

    workflow_response response;
    response.run = cancelled;
    return response;
}

workflow_response workflow_store::error_response(const std::string &error) const
{
    workflow_response response;
    response.ok = false;
    response.error = error;
    response.run.status = "failed";
    response.run.error = error;
    return response;
}

bool workflow_store::parse_source(const workflow_source &source,
                                  workflow_graph *graph,
                                  workflow_response *response) const
{
    if (!valid_schema(source.schema_version))
    {
        if (response != nullptr)
        {
            *response = error_response("workflow source has unsupported schema version");
        }
        return false;
    }
    if (source.source_text.empty())
    {
        if (response != nullptr)
        {
            *response = error_response("workflow source is empty");
        }
        return false;
    }

    std::string error;
    if (graph == nullptr || !graph->load_from_text(source.source_text, source.source_name, &error))
    {
        if (response != nullptr)
        {
            *response = error_response(error.empty() ? "workflow parse failed" : error);
        }
        return false;
    }
    if (graph->nodes().empty())
    {
        if (response != nullptr)
        {
            *response = error_response("workflow has no tasks");
        }
        return false;
    }
    return true;
}

workflow_run_record workflow_store::make_record(const workflow_start_request &request, const std::string &status)
{
    workflow_run_record record;
    record.run_id = request.run_id;
    record.workflow_id = workflow_identity(request.source);
    record.source_name = request.source.source_name;
    record.status = status;
    record.state_key = "workflow/" + request.run_id;
    return record;
}

workflow_response workflow_store::store_record(const workflow_run_record &record)
{
    workflow_run_record stored = record;
    {
        ::dsn::service::zauto_lock guard(_lock);
        stored.sequence = ++_last_sequence;
    }

    const state_response persisted = persist_to_state(stored);
    if (!persisted.ok)
    {
        return error_response("workflow state persist failed for run " + stored.run_id + ": " + persisted.error);
    }

    {
        ::dsn::service::zauto_lock guard(_lock);
        _runs[stored.run_id] = stored;
        _last_sequence = (std::max)(_last_sequence, stored.sequence);
    }

    dinfo("stored rASN workflow run=%s status=%s sequence=%llu",
          stored.run_id.c_str(),
          stored.status.c_str(),
          static_cast<unsigned long long>(stored.sequence));

    workflow_response response;
    response.ok = stored.status != "failed";
    response.error = stored.error;
    response.run = stored;
    return response;
}

workflow_response workflow_store::store_record_if_current(const workflow_run_record &record, uint64_t expected_sequence)
{
    workflow_run_record stored = record;
    std::string lease_error;
    if (!owns_execution_lease(stored, &lease_error))
    {
        return error_response(lease_error);
    }

    {
        ::dsn::service::zauto_lock guard(_lock);
        stored.sequence = ++_last_sequence;
    }

    const state_response persisted = persist_to_state(stored, true, expected_sequence);
    if (!persisted.ok)
    {
        return error_response("workflow state persist conflict for run " + stored.run_id + ": " + persisted.error);
    }

    {
        ::dsn::service::zauto_lock guard(_lock);
        _runs[stored.run_id] = stored;
        _last_sequence = (std::max)(_last_sequence, stored.sequence);
    }

    dinfo("stored rASN workflow run=%s status=%s sequence=%llu",
          stored.run_id.c_str(),
          stored.status.c_str(),
          static_cast<unsigned long long>(stored.sequence));

    workflow_response response;
    response.ok = stored.status != "failed";
    response.error = stored.error;
    response.run = stored;
    return response;
}

workflow_response workflow_store::recover_from_state()
{
    state_query_request request;
    request.key_prefix = "workflow/";
    const state_response state = workflow_state_query(request);
    if (!state.ok)
    {
        return error_response("workflow state recovery failed: " + state.error);
    }

    std::vector<workflow_run_record> recovered;
    recovered.reserve(state.records.size());
    size_t skipped = 0;
    for (const state_record &record : state.records)
    {
        if (record.kind != "workflow")
        {
            continue;
        }
        workflow_run_record run;
        std::string error;
        if (!decode_workflow_run_record(record, &run, &error))
        {
            // Skip and log a single unreadable record instead of aborting the
            // entire recovery batch: one corrupt run must not block startup
            // recovery of every other run.
            ++skipped;
            dwarn("skipping unrecoverable rASN workflow record %s: %s", record.key.c_str(), error.c_str());
            continue;
        }
        recovered.push_back(run);
    }

    {
        ::dsn::service::zauto_lock guard(_lock);
        for (const workflow_run_record &run : recovered)
        {
            _runs[run.run_id] = run;
            _last_sequence = (std::max)(_last_sequence, run.sequence);
        }
    }

    dinfo("recovered rASN workflow runs=%llu skipped=%llu",
          static_cast<unsigned long long>(recovered.size()),
          static_cast<unsigned long long>(skipped));
    workflow_response response;
    response.run.status = "recovered";
    response.run.result_text = "runs=" + std::to_string(recovered.size()) + " skipped=" + std::to_string(skipped);
    return response;
}

bool workflow_store::recover_run_from_state(const std::string &run_id, workflow_run_record *record, std::string *error)
{
    state_key_request request;
    request.key = "workflow/" + run_id;
    const state_response state = workflow_state_get(request);
    if (!state.ok)
    {
        if (!state_key_not_found(state, request.key) && error != nullptr)
        {
            *error = "workflow state lookup failed for run " + run_id + ": " + state.error;
        }
        return false;
    }

    workflow_run_record recovered;
    if (!decode_workflow_run_record(state.record, &recovered, error))
    {
        return false;
    }

    {
        ::dsn::service::zauto_lock guard(_lock);
        _runs[recovered.run_id] = recovered;
        _last_sequence = (std::max)(_last_sequence, recovered.sequence);
    }

    if (record != nullptr)
    {
        *record = recovered;
    }
    return true;
}

workflow_response workflow_store::acquire_execution_lease(const std::string &run_id,
                                                          const std::string &workflow_id,
                                                          workflow_run_record *record) const
{
    workflow_execution_lease lease;
    lease.run_id = run_id;
    lease.workflow_id = workflow_id;
    lease.owner = make_trace_id();
    lease.status = "running";
    lease.expires_ms = ::dsn_now_ms() + workflow_execution_lease_ms();

    state_record lease_record;
    lease_record.key = workflow_lease_key(run_id);
    lease_record.kind = "workflow.lease";
    lease_record.scope = "rasn.workflow";
    lease_record.value = lease_value(lease);

    state_put_request create_request;
    create_request.record = lease_record;
    create_request.create_only = true;
    state_response create_response = workflow_state_put(create_request);
    if (!create_response.ok)
    {
        state_key_request get_request;
        get_request.key = lease_record.key;
        const state_response existing_response = workflow_state_get(get_request);
        if (!existing_response.ok)
        {
            return error_response("workflow lease acquisition failed for run " + run_id + ": " + create_response.error);
        }

        workflow_execution_lease existing;
        std::string decode_error;
        if (!decode_workflow_execution_lease(existing_response.record, &existing, &decode_error))
        {
            return error_response(decode_error);
        }
        if (!existing.workflow_id.empty() && existing.workflow_id != workflow_id)
        {
            return error_response("workflow lease source mismatch for run " + run_id + ": expected " +
                                  existing.workflow_id + " got " + workflow_id);
        }

        const uint64_t now_ms = ::dsn_now_ms();
        if (existing.status == "running" && existing.expires_ms > now_ms)
        {
            return error_response("workflow run already owned: " + run_id + " owner=" + existing.owner +
                                  " lease_expires_ms=" + std::to_string(existing.expires_ms));
        }

        state_put_request takeover_request;
        takeover_request.record = lease_record;
        takeover_request.check_sequence = true;
        takeover_request.expected_sequence = existing.sequence;
        create_response = workflow_state_put(takeover_request);
        if (!create_response.ok)
        {
            return error_response("workflow lease acquisition conflict for run " + run_id + ": " +
                                  create_response.error);
        }
    }

    if (record != nullptr)
    {
        record->execution_owner = lease.owner;
        record->lease_key = lease_record.key;
        record->lease_sequence = create_response.record.sequence;
        record->lease_expires_ms = lease.expires_ms;
    }
    workflow_response response;
    if (record != nullptr)
    {
        response.run = *record;
    }
    return response;
}

bool workflow_store::owns_execution_lease(const workflow_run_record &record, std::string *error) const
{
    if (record.lease_key.empty() || record.execution_owner.empty() || record.lease_sequence == 0)
    {
        return true;
    }

    state_key_request request;
    request.key = record.lease_key;
    const state_response response = workflow_state_get(request);
    if (!response.ok)
    {
        if (error != nullptr)
        {
            *error = "workflow lease ownership check failed for run " + record.run_id + ": " + response.error;
        }
        return false;
    }

    workflow_execution_lease lease;
    std::string decode_error;
    if (!decode_workflow_execution_lease(response.record, &lease, &decode_error))
    {
        if (error != nullptr)
        {
            *error = decode_error;
        }
        return false;
    }

    if (lease.owner != record.execution_owner || lease.sequence != record.lease_sequence || lease.status != "running")
    {
        if (error != nullptr)
        {
            *error = "workflow lease no longer owned by run " + record.run_id + " owner=" + record.execution_owner;
        }
        return false;
    }
    return true;
}

void workflow_store::release_execution_lease(const workflow_run_record &record, const std::string &status) const
{
    if (record.lease_key.empty() || record.execution_owner.empty() || record.lease_sequence == 0)
    {
        return;
    }

    workflow_execution_lease lease;
    lease.run_id = record.run_id;
    lease.workflow_id = record.workflow_id;
    lease.owner = record.execution_owner;
    lease.status = status;
    lease.expires_ms = ::dsn_now_ms();

    state_record lease_record;
    lease_record.key = record.lease_key;
    lease_record.kind = "workflow.lease";
    lease_record.scope = "rasn.workflow";
    lease_record.value = lease_value(lease);

    state_put_request release_request;
    release_request.record = lease_record;
    release_request.check_sequence = true;
    release_request.expected_sequence = record.lease_sequence;
    const state_response response = workflow_state_put(release_request);
    if (!response.ok)
    {
        dwarn("failed to release workflow lease run=%s owner=%s: %s",
              record.run_id.c_str(),
              record.execution_owner.c_str(),
              response.error.c_str());
    }
}

bool workflow_store::recover_resume_state_from_state(const std::string &run_id,
                                                     const std::string &workflow_id,
                                                     workflow_graph::workflow_resume_state *resume_state,
                                                     std::string *error) const
{
    state_query_request request;
    request.key_prefix = "workflow-node/" + run_id + "/";
    const state_response state = workflow_state_query(request);
    if (!state.ok)
    {
        if (error != nullptr)
        {
            *error = "workflow resume state query failed: " + state.error;
        }
        return false;
    }

    workflow_graph::workflow_resume_state recovered;
    for (const state_record &record : state.records)
    {
        workflow_node_status status;
        std::string record_workflow_id;
        if (!decode_workflow_node_status(record, run_id, &status, &record_workflow_id, error))
        {
            return false;
        }
        if (record_workflow_id != workflow_id)
        {
            if (error != nullptr)
            {
                *error = "workflow node state source mismatch for " + record.key;
            }
            return false;
        }
        if (status.status == "completed" || status.status == "resumed")
        {
            recovered[status.node_id] = status;
        }
    }

    if (resume_state != nullptr)
    {
        *resume_state = recovered;
    }
    dinfo("recovered rASN workflow resume nodes run=%s nodes=%llu",
          run_id.c_str(),
          static_cast<unsigned long long>(recovered.size()));
    return true;
}

bool workflow_store::is_cancelled(const std::string &run_id, workflow_run_record *record) const
{
    ::dsn::service::zauto_lock guard(_lock);
    const std::map<std::string, workflow_run_record>::const_iterator it = _runs.find(run_id);
    if (it == _runs.end() || it->second.status != "cancelled")
    {
        return false;
    }
    if (record != nullptr)
    {
        *record = it->second;
    }
    return true;
}

state_response workflow_store::persist_to_state(const workflow_run_record &record,
                                                bool check_sequence,
                                                uint64_t expected_sequence) const
{
    state_record state;
    state.key = record.state_key;
    state.kind = "workflow";
    state.scope = "rasn.workflow";
    state.value = record_value(record);
    state.sequence = record.sequence;
    state_put_request request;
    request.record = state;
    request.check_sequence = check_sequence;
    request.expected_sequence = expected_sequence;
    const state_response response = workflow_state_put(request);
    if (!response.ok)
    {
        dwarn("failed to persist workflow run=%s to state: %s", record.run_id.c_str(), response.error.c_str());
    }
    return response;
}

void workflow_store::persist_node_status(const std::string &run_id,
                                         const std::string &workflow_id,
                                         const workflow_node_status &status) const
{
    state_record state;
    state.key = node_state_key(run_id, status.node_id);
    state.kind = "workflow.node";
    state.scope = "rasn.workflow";
    state.value = node_status_value(run_id, workflow_id, status);
    const state_response response = workflow_state_put(state);
    if (!response.ok)
    {
        dwarn("failed to persist workflow node run=%s node=%s status=%s: %s",
              run_id.c_str(),
              status.node_id.c_str(),
              status.status.c_str(),
              response.error.c_str());
    }
}

workflow_store &global_workflow_store()
{
    static workflow_store store;
    return store;
}

void set_workflow_state_readers(workflow_state_getter getter, workflow_state_queryer queryer)
{
    ::dsn::service::zauto_lock guard(workflow_state_reader_lock());
    workflow_state_getter_slot() = getter == nullptr ? &default_workflow_state_get : getter;
    workflow_state_queryer_slot() = queryer == nullptr ? &default_workflow_state_query : queryer;
}

void reset_workflow_state_readers()
{
    set_workflow_state_readers(nullptr, nullptr);
}

void set_workflow_state_writer(workflow_state_writer writer)
{
    ::dsn::service::zauto_lock guard(workflow_state_reader_lock());
    workflow_state_writer_slot() = writer == nullptr ? &default_workflow_state_put : writer;
}

void reset_workflow_state_writer()
{
    set_workflow_state_writer(nullptr);
}

void rasn_workflow_rpc_service::open_service()
{
    dinfo("opening rasn.workflow serverlet");
    this->register_async_rpc_handler(RPC_RASN_WORKFLOW_VALIDATE, "validate", &rasn_workflow_rpc_service::on_validate);
    this->register_async_rpc_handler(RPC_RASN_WORKFLOW_COMPILE, "compile", &rasn_workflow_rpc_service::on_compile);
    this->register_async_rpc_handler(RPC_RASN_WORKFLOW_START, "start", &rasn_workflow_rpc_service::on_start);
    this->register_async_rpc_handler(RPC_RASN_WORKFLOW_QUERY, "query", &rasn_workflow_rpc_service::on_query);
    this->register_async_rpc_handler(RPC_RASN_WORKFLOW_CANCEL, "cancel", &rasn_workflow_rpc_service::on_cancel);
}

void rasn_workflow_rpc_service::close_service()
{
    dinfo("closing rasn.workflow serverlet");
    this->unregister_rpc_handler(RPC_RASN_WORKFLOW_VALIDATE);
    this->unregister_rpc_handler(RPC_RASN_WORKFLOW_COMPILE);
    this->unregister_rpc_handler(RPC_RASN_WORKFLOW_START);
    this->unregister_rpc_handler(RPC_RASN_WORKFLOW_QUERY);
    this->unregister_rpc_handler(RPC_RASN_WORKFLOW_CANCEL);
}

void rasn_workflow_rpc_service::on_validate(const workflow_source &request,
                                            ::dsn::rpc_replier<workflow_response> &reply)
{
    reply(global_workflow_store().validate(request));
}

void rasn_workflow_rpc_service::on_compile(const workflow_source &request,
                                           ::dsn::rpc_replier<workflow_response> &reply)
{
    reply(global_workflow_store().compile(request));
}

void rasn_workflow_rpc_service::on_start(const workflow_start_request &request,
                                         ::dsn::rpc_replier<workflow_response> &reply)
{
    reply(global_workflow_store().start(request));
}

void rasn_workflow_rpc_service::on_query(const workflow_run_query &request,
                                         ::dsn::rpc_replier<workflow_response> &reply)
{
    reply(global_workflow_store().query(request));
}

void rasn_workflow_rpc_service::on_cancel(const workflow_run_query &request,
                                          ::dsn::rpc_replier<workflow_response> &reply)
{
    reply(global_workflow_store().cancel(request));
}

std::pair<::dsn::error_code, workflow_response>
rasn_workflow_client::validate_sync(const workflow_source &request,
                                    std::chrono::milliseconds timeout,
                                    int thread_hash,
                                    uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<workflow_response>(::dsn::rpc::call(
        _server, RPC_RASN_WORKFLOW_VALIDATE, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

std::pair<::dsn::error_code, workflow_response>
rasn_workflow_client::compile_sync(const workflow_source &request,
                                   std::chrono::milliseconds timeout,
                                   int thread_hash,
                                   uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<workflow_response>(::dsn::rpc::call(
        _server, RPC_RASN_WORKFLOW_COMPILE, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

std::pair<::dsn::error_code, workflow_response>
rasn_workflow_client::start_sync(const workflow_start_request &request,
                                 std::chrono::milliseconds timeout,
                                 int thread_hash,
                                 uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<workflow_response>(::dsn::rpc::call(
        _server, RPC_RASN_WORKFLOW_START, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

std::pair<::dsn::error_code, workflow_response>
rasn_workflow_client::query_sync(const workflow_run_query &request,
                                 std::chrono::milliseconds timeout,
                                 int thread_hash,
                                 uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<workflow_response>(::dsn::rpc::call(
        _server, RPC_RASN_WORKFLOW_QUERY, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

std::pair<::dsn::error_code, workflow_response>
rasn_workflow_client::cancel_sync(const workflow_run_query &request,
                                  std::chrono::milliseconds timeout,
                                  int thread_hash,
                                  uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<workflow_response>(::dsn::rpc::call(
        _server, RPC_RASN_WORKFLOW_CANCEL, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

::dsn::error_code rasn_workflow_app::start(int argc, char **argv)
{
    global_rasn_services().acquire();
    _rpc.open_service();
    _recovery_task = ::dsn::tasking::enqueue(LPC_RASN_WORKFLOW_STARTUP_RECOVERY,
                                             nullptr,
                                             []() { recover_workflow_state_after_start(); },
                                             0,
                                             std::chrono::milliseconds(1500));
    if (_recovery_task == nullptr)
    {
        dwarn("failed to schedule workflow startup state recovery");
    }
    return ::dsn::ERR_OK;
}

::dsn::error_code rasn_workflow_app::stop(bool cleanup)
{
    if (_recovery_task != nullptr)
    {
        _recovery_task->cancel(true);
        _recovery_task = nullptr;
    }
    _rpc.close_service();
    global_rasn_services().release();
    return ::dsn::ERR_OK;
}

} // namespace rasn
} // namespace dsn
