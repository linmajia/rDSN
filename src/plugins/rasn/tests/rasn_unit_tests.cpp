#include <rasn/agent_types.h>
#include <rasn/agent_control_plane.h>
#include <rasn/agent_executor.h>
#include <rasn/agent_messages.h>
#include <rasn/agent_message_bus.h>
#include <rasn/agent_registry.h>
#include <rasn/agent_runtime.h>
#include <rasn/agent_services.h>
#include <rasn/admission_gate.h>
#include <rasn/approval_sandbox.h>
#include <rasn/blackboard.h>
#include <rasn/capability_directory.h>
#include <rasn/circuit_breaker.h>
#include <rasn/cli_app.h>
#include <rasn/cli_support.h>
#include <rasn/runtime_provider.h>
#include <rasn/contract_verifier.h>
#include <rasn/coordinator_service.h>
#include <rasn/apps/codepilot/local_tools.h>
#include <rasn/determinism_ledger.h>
#include <rasn/human_interaction.h>
#include <rasn/llm_provider.h>
#include <rasn/metrics.h>
#include <rasn/model_cost.h>
#include <rasn/policy_manager.h>
#include <rasn/provider_router.h>
#include <rasn/rate_limiter.h>
#include <rasn/recovery_supervisor.h>
#include <rasn/redaction.h>
#include <rasn/resource_budget.h>
#include <rasn/rpc_resilience.h>
#include <rasn/sandbox_runtime.h>
#include <rasn/state_service.h>
#include <rasn/schema_manifest.h>
#include <rasn/session_store.h>
#include <rasn/task_orchestration.h>
#include <rasn/tool_catalog.h>
#include <rasn/workflow.h>
#include <rasn/workflow_service.h>
#include <rasn/workspace_change.h>
#include <rasn/workspace_index.h>

#include <dsn/cpp/utils.h>
#include <dsn/tool-api/command.h>
#include <dsn/tool-api/task.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace dsn {
namespace rasn {
namespace {

class fixed_llm_provider : public llm_provider
{
public:
    std::string name() const override { return "test.provider"; }
    std::string model() const override { return "test-model"; }

    model_provider_descriptor describe() const override
    {
        model_provider_descriptor descriptor;
        descriptor.provider = name();
        descriptor.model = model();
        descriptor.local = true;
        descriptor.health = "healthy";
        return descriptor;
    }

    llm_response complete(const llm_request &request, nucleus_runtime &runtime) override
    {
        ++calls;
        last_timeout_ms = request.timeout_ms;
        last_retry_budget = request.retry_budget;
        last_user_prompt = request.user_prompt;
        last_policy_labels = request.policy_labels;
        last_context.clear();
        for (const std::string &context : request.context)
        {
            if (!last_context.empty())
            {
                last_context += "\n";
            }
            last_context += context;
        }
        llm_response response;
        response.ok = true;
        response.text = "model:" + request.user_prompt;
        return response;
    }

    int calls = 0;
    uint32_t last_timeout_ms = 0;
    uint32_t last_retry_budget = 0;
    std::string last_user_prompt;
    std::string last_context;
    std::vector<std::string> last_policy_labels;
};

class sleeping_tool_provider : public agent_tool_provider
{
public:
    explicit sleeping_tool_provider(uint32_t sleep_ms) : _sleep_ms(sleep_ms) {}

    std::string describe_tools() const override { return "sleeping test tool provider"; }

    tool_result run(const std::string &name,
                    const std::vector<std::string> &args,
                    nucleus_runtime &runtime,
                    const agent_task &task) const override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(_sleep_ms));
        tool_result result;
        result.ok = true;
        result.output = name + ":slept";
        return result;
    }

private:
    uint32_t _sleep_ms;
};

class blocking_tool_provider : public agent_tool_provider
{
public:
    std::string describe_tools() const override { return "blocking test tool provider"; }

    tool_result run(const std::string &name,
                    const std::vector<std::string> &args,
                    nucleus_runtime &runtime,
                    const agent_task &task) const override
    {
        (void)args;
        (void)runtime;
        (void)task;
        {
            std::unique_lock<std::mutex> guard(_lock);
            ++_running;
            _max_running = (std::max)(_max_running, _running);
            _cv.notify_all();
            while (!_released)
            {
                _cv.wait(guard);
            }
            --_running;
        }
        tool_result result;
        result.ok = true;
        result.output = name + ":unblocked";
        return result;
    }

    bool wait_for_running(uint32_t target, uint32_t timeout_ms) const
    {
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        std::unique_lock<std::mutex> guard(_lock);
        while (_running < target)
        {
            if (_cv.wait_until(guard, deadline) == std::cv_status::timeout)
            {
                return _running >= target;
            }
        }
        return true;
    }

    void release_all() const
    {
        std::lock_guard<std::mutex> guard(_lock);
        _released = true;
        _cv.notify_all();
    }

    uint32_t max_running() const
    {
        std::lock_guard<std::mutex> guard(_lock);
        return _max_running;
    }

private:
    mutable std::mutex _lock;
    mutable std::condition_variable _cv;
    mutable uint32_t _running = 0;
    mutable uint32_t _max_running = 0;
    mutable bool _released = false;
};

std::string temp_file_path(const std::string &name)
{
    const char *tmp = std::getenv("TEMP");
    if (tmp == nullptr || *tmp == '\0')
    {
        tmp = std::getenv("TMPDIR");
    }
    const std::string directory = (tmp == nullptr || *tmp == '\0') ? "." : tmp;
#if defined(_WIN32)
    return directory + "\\" + name;
#else
    return directory + "/" + name;
#endif
}

std::string workspace_temp_file_path(const std::string &name)
{
    return normalize_platform_path(name);
}

agent_descriptor make_unit_agent_descriptor(const std::string &agent_id,
                                            const std::string &role,
                                            const std::string &capability)
{
    agent_descriptor descriptor;
    descriptor.agent_id = agent_id;
    descriptor.role = role;
    descriptor.app_name = "unit";
    descriptor.health = "healthy";
    if (!capability.empty())
    {
        agent_capability cap;
        cap.name = capability;
        cap.input_type = "text";
        cap.output_type = "text";
        cap.side_effect_class = "read_only";
        descriptor.capabilities.push_back(cap);
    }
    return descriptor;
}

void write_text_file(const std::string &path, const std::string &content)
{
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open()) << path;
    output << content;
}

std::string read_text_file(const std::string &path)
{
    std::ifstream input(path.c_str(), std::ios::binary);
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

class scoped_cout_capture
{
public:
    scoped_cout_capture() : _previous(std::cout.rdbuf(_output.rdbuf())) {}
    ~scoped_cout_capture() { std::cout.rdbuf(_previous); }

    std::string str() const { return _output.str(); }

private:
    std::ostringstream _output;
    std::streambuf *_previous;
};

std::string lease_record_value(const std::string &run_id,
                               const std::string &workflow_id,
                               const std::string &owner,
                               const std::string &status,
                               uint64_t expires_ms)
{
    std::ostringstream oss;
    oss << "run_id=" << run_id << "\n"
        << "workflow_id=" << workflow_id << "\n"
        << "owner=" << owner << "\n"
        << "status=" << status << "\n"
        << "expires_ms=" << expires_ms << "\n";
    return oss.str();
}

std::vector<state_record> &captured_policy_state_records()
{
    static std::vector<state_record> records;
    return records;
}

std::vector<state_record> &captured_observability_state_records()
{
    static std::vector<state_record> records;
    return records;
}

state_response capture_policy_state_record(const state_record &record)
{
    captured_policy_state_records().push_back(record);
    state_response response;
    response.record = record;
    response.last_sequence = captured_policy_state_records().size();
    return response;
}

state_response capture_observability_state_record(const state_record &record)
{
    captured_observability_state_records().push_back(record);
    state_response response;
    response.record = record;
    response.last_sequence = captured_observability_state_records().size();
    return response;
}

state_response fail_workflow_state_get(const state_key_request &request)
{
    state_response response;
    response.ok = false;
    response.error = "injected state get failure for " + request.key;
    return response;
}

state_response fail_workflow_state_put(const state_put_request &request)
{
    state_response response;
    response.ok = false;
    response.error = "injected state put failure for " + request.record.key;
    return response;
}

std::string workflow_run_state_value(const std::string &run_id,
                                     const std::string &workflow_id,
                                     const std::string &status,
                                     uint64_t sequence)
{
    std::ostringstream oss;
    oss << "run_id=" << run_id << "\n"
        << "workflow_id=" << workflow_id << "\n"
        << "source_name=unit-recovered-cancel\n"
        << "status=" << status << "\n"
        << "sequence=" << sequence << "\n";
    return oss.str();
}

state_response put_workflow_run_state(const std::string &run_id,
                                      const std::string &workflow_id,
                                      const std::string &status,
                                      uint64_t sequence)
{
    state_record record;
    record.key = "workflow/" + run_id;
    record.kind = "workflow";
    record.scope = "rasn.workflow";
    record.sequence = sequence;
    record.value = workflow_run_state_value(run_id, workflow_id, status, sequence);
    state_put_request request;
    request.record = record;
    request.create_only = true;
    return global_state_store().put(request);
}

class scoped_workflow_state_readers
{
public:
    scoped_workflow_state_readers(workflow_state_getter getter, workflow_state_queryer queryer)
    {
        set_workflow_state_readers(getter, queryer);
    }

    ~scoped_workflow_state_readers() { reset_workflow_state_readers(); }
};

class scoped_workflow_state_writer
{
public:
    explicit scoped_workflow_state_writer(workflow_state_writer writer) { set_workflow_state_writer(writer); }

    ~scoped_workflow_state_writer() { reset_workflow_state_writer(); }
};

std::string artifact_path_from_record(const state_record &record)
{
    std::istringstream input(record.value);
    std::string line;
    while (std::getline(input, line))
    {
        const std::string prefix = "path=";
        if (line.find(prefix) == 0)
        {
            return line.substr(prefix.size());
        }
    }
    return "";
}

TEST(rasn_agent_types, request_and_response_validation_reports_required_fields)
{
    agent_request request;
    std::string error;
    EXPECT_FALSE(validate_agent_request(request, &error));
    EXPECT_EQ("missing agent request id", error);

    request.request_id = "req";
    EXPECT_FALSE(validate_agent_request(request, &error));
    EXPECT_EQ("missing agent request trace id", error);

    request.trace_id = "trace";
    EXPECT_FALSE(validate_agent_request(request, &error));
    EXPECT_EQ("missing agent request capability", error);

    request.capability = "model.complete";
    EXPECT_TRUE(validate_agent_request(request, &error));

    agent_response response;
    response.request_id = "req";
    response.ok = true;
    response.error.message = "unexpected";
    EXPECT_FALSE(validate_agent_response(response, &error));
    EXPECT_EQ("agent response cannot contain both success and error", error);
}

TEST(rasn_agent_types, completion_request_preserves_timeout_and_retry_budget)
{
    agent_completion_request completion;
    completion.task.id = "completion-timeout";
    completion.task.name = "unit.completion";
    completion.user_prompt = "hello";
    completion.timeout_ms = 321;
    completion.retry_budget = 2;

    const agent_request generic = make_model_agent_request(completion, "trace-timeout");
    EXPECT_EQ(321u, generic.timeout_ms);
    EXPECT_EQ(2u, generic.retry_budget);

    const agent_completion_request roundtrip = make_completion_request_from_agent(generic);
    EXPECT_EQ(321u, roundtrip.timeout_ms);
    EXPECT_EQ(2u, roundtrip.retry_budget);
}

TEST(rasn_agent_types, schema_manifest_exposes_core_contracts)
{
    const std::string manifest = rasn_schema_manifest_text();
    EXPECT_NE(std::string::npos, manifest.find("[agent_request]"));
    EXPECT_NE(std::string::npos, manifest.find("policy_labels"));
    EXPECT_NE(std::string::npos, manifest.find("[runtime_event]"));
    EXPECT_NE(std::string::npos, manifest.find("[tool_descriptor]"));
    EXPECT_NE(std::string::npos, manifest.find("[tool_argument_descriptor]"));
    EXPECT_NE(std::string::npos, manifest.find("[agent_descriptor]"));
    EXPECT_NE(std::string::npos, manifest.find("[model_provider_descriptor]"));
    EXPECT_NE(std::string::npos, manifest.find("credential_ref"));
    EXPECT_NE(std::string::npos, manifest.find("[state_response]"));
    EXPECT_NE(std::string::npos, manifest.find("[state_delete_prefix_request]"));
    EXPECT_NE(std::string::npos, manifest.find("[state_sequence_barrier_request]"));
    EXPECT_NE(std::string::npos, manifest.find("[state_checkpoint_result]"));
    EXPECT_NE(std::string::npos, manifest.find("[redaction_policy]"));
    EXPECT_NE(std::string::npos, manifest.find("[workflow_node]"));

    const std::string json = rasn_schema_manifest_json();
    EXPECT_NE(std::string::npos, json.find("\"name\":\"agent_request\""));
    EXPECT_NE(std::string::npos, json.find("\"name\":\"agent_context_entry\""));
    EXPECT_NE(std::string::npos, json.find("\"name\":\"credential_ref\""));
    EXPECT_NE(std::string::npos, json.find("\"required\":true"));

    const std::string idl = rasn_schema_manifest_idl();
    EXPECT_NE(std::string::npos, idl.find("record agent_request version 1"));
    EXPECT_NE(std::string::npos, idl.find("optional list<string> policy_labels;"));

    const std::string cpp = rasn_schema_manifest_cpp_header();
    EXPECT_NE(std::string::npos, cpp.find("namespace generated"));
    EXPECT_NE(std::string::npos, cpp.find("struct agent_request"));
    EXPECT_NE(std::string::npos, cpp.find("struct model_provider_descriptor"));
    EXPECT_NE(std::string::npos, cpp.find("std::string credential_ref;"));
    EXPECT_NE(std::string::npos, cpp.find("std::vector<agent_context_entry> context;"));
    EXPECT_NE(std::string::npos, cpp.find("struct state_response"));
    EXPECT_NE(std::string::npos, cpp.find("std::uint32_t schema_version = schema_version_value;"));

    const std::vector<rpc_operation_descriptor> operations = rasn_rpc_operation_manifest();
    EXPECT_GE(operations.size(), 28u);
    EXPECT_EQ("rasn.agent", operations.front().service);
    const std::string cpp_clients = rasn_schema_manifest_cpp_clients();
    EXPECT_NE(std::string::npos, cpp_clients.find("class workflow_rpc_client"));
    EXPECT_NE(std::string::npos, cpp_clients.find("RPC_RASN_OBSERVABILITY_LOAD_REPLAY"));
    EXPECT_NE(std::string::npos, cpp_clients.find("RPC_RASN_STATE_DELETE_PREFIX"));
    EXPECT_NE(std::string::npos, cpp_clients.find("RPC_RASN_STATE_ADVANCE_SEQUENCE"));
    EXPECT_NE(std::string::npos, cpp_clients.find("RPC_RASN_STATE_CHECKPOINT_DETAILED"));
    EXPECT_NE(std::string::npos, cpp_clients.find("std::pair< ::dsn::error_code, state_response>"));

    const std::string typescript = rasn_schema_manifest_typescript();
    EXPECT_NE(std::string::npos, typescript.find("export interface agent_request"));
    EXPECT_NE(std::string::npos, typescript.find("context?: agent_context_entry[];"));
    EXPECT_NE(std::string::npos, typescript.find("export const state_response_schema_version = 1 as const;"));
    const std::string typescript_clients = rasn_schema_manifest_typescript_clients();
    EXPECT_NE(std::string::npos, typescript_clients.find("export interface RasnRpcTransport"));
    EXPECT_NE(std::string::npos, typescript_clients.find("export class WorkflowRpcClient"));
    EXPECT_NE(std::string::npos, typescript_clients.find("\"RPC_RASN_WORKFLOW_START\""));

    const std::string python = rasn_schema_manifest_python();
    EXPECT_NE(std::string::npos, python.find("class agent_request:"));
    EXPECT_NE(std::string::npos, python.find("context: List[agent_context_entry] = field(default_factory=list)"));
    EXPECT_NE(std::string::npos, python.find("record: Optional[state_record] = None"));
    const std::string python_clients = rasn_schema_manifest_python_clients();
    EXPECT_NE(std::string::npos, python_clients.find("class RasnRpcTransport"));
    EXPECT_NE(std::string::npos, python_clients.find("class WorkflowRpcClient"));
    EXPECT_NE(std::string::npos, python_clients.find("\"RPC_RASN_WORKFLOW_START\""));
}

TEST(rasn_redaction, redacts_exact_values_and_common_secret_patterns)
{
    const std::string exact_secret = "ghp_exactSecretValue123456";
    const std::string input =
        "Authorization: Bearer " + exact_secret + "\n"
        "password=hunter2\n"
        "{\"api_key\":\"sk-json-secret\"}\n"
        "safe token_env=RASN_COPILOT_TOKEN\n";

    const std::string redacted = redact_sensitive_text(input, std::vector<std::string>{exact_secret});
    EXPECT_EQ(std::string::npos, redacted.find(exact_secret));
    EXPECT_EQ(std::string::npos, redacted.find("hunter2"));
    EXPECT_EQ(std::string::npos, redacted.find("sk-json-secret"));
    EXPECT_NE(std::string::npos, redacted.find("Authorization: Bearer <redacted-secret>"));
    EXPECT_NE(std::string::npos, redacted.find("password=<redacted-secret>"));
    EXPECT_NE(std::string::npos, redacted.find("\"api_key\":\"<redacted-secret>\""));
    EXPECT_NE(std::string::npos, redacted.find("token_env=RASN_COPILOT_TOKEN"));
}

TEST(rasn_runtime, redacts_event_values_before_observability_and_trace)
{
    const std::string trace_path = temp_file_path("rasn-redaction-trace.jsonl");
    std::remove(trace_path.c_str());

    nucleus_runtime runtime;
    runtime.set_trace_file(trace_path);
    agent_task task;
    task.id = "redaction-runtime";
    task.name = "unit.redaction";
    task.input = "api_key=sk-runtime-secret";
    runtime.begin_task(task);

    const std::vector<runtime_event> events = runtime.events();
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(std::string::npos, events[0].value.find("sk-runtime-secret"));
    EXPECT_NE(std::string::npos, events[0].value.find("<redacted-secret>"));

    const std::string trace = read_text_file(trace_path);
    EXPECT_EQ(std::string::npos, trace.find("sk-runtime-secret"));
    EXPECT_NE(std::string::npos, trace.find("<redacted-secret>"));
    std::remove(trace_path.c_str());
}

TEST(rasn_model_streaming, default_streaming_records_redacted_chunks)
{
    fixed_llm_provider provider;
    nucleus_runtime runtime;
    llm_request request;
    request.task_id = "stream-task";
    request.user_prompt = "stream password=hunter2 response";

    std::vector<std::string> chunks;
    const llm_response response = provider.complete_streaming(request, runtime, [&chunks](const std::string &chunk) {
        chunks.push_back(chunk);
    });

    ASSERT_TRUE(response.ok);
    ASSERT_FALSE(chunks.empty());
    for (const std::string &chunk : chunks)
    {
        EXPECT_EQ(std::string::npos, chunk.find("hunter2"));
    }

    const std::vector<runtime_event> events = runtime.events();
    bool saw_chunk = false;
    for (const runtime_event &event : events)
    {
        if (event.kind == "llm.response.chunk")
        {
            saw_chunk = true;
            EXPECT_NE(std::string::npos, event.name.find("test.provider:"));
            EXPECT_EQ(std::string::npos, event.value.find("hunter2"));
        }
    }
    EXPECT_TRUE(saw_chunk);
}

TEST(rasn_agent_runtime, replays_recorded_llm_responses_by_provider_order)
{
    const std::string trace_path = temp_file_path("rasn-llm-replay-trace.jsonl");
    write_text_file(trace_path,
                    "{\"schema_version\":1,\"sequence\":1,\"trace_id\":\"trace\",\"task_id\":\"task\","
                    "\"kind\":\"llm.response\",\"name\":\"test.provider\",\"value\":\"first\","
                    "\"timestamp\":\"now\"}\n"
                    "{\"schema_version\":1,\"sequence\":2,\"trace_id\":\"trace\",\"task_id\":\"task\","
                    "\"kind\":\"llm.response\",\"name\":\"other.provider\",\"value\":\"other\","
                    "\"timestamp\":\"now\"}\n"
                    "{\"schema_version\":1,\"sequence\":3,\"trace_id\":\"trace\",\"task_id\":\"task\","
                    "\"kind\":\"llm.response\",\"name\":\"test.provider\",\"value\":\"second\","
                    "\"timestamp\":\"now\"}\n");

    nucleus_runtime runtime;
    std::string error;
    ASSERT_TRUE(runtime.enable_replay(trace_path, &error)) << error;

    agent_task task;
    task.id = "llm-replay";
    task.name = "llm.replay.test";

    std::string response;
    ASSERT_TRUE(runtime.replay_llm_response(task, "test.provider", &response));
    EXPECT_EQ("first", response);
    ASSERT_TRUE(runtime.replay_llm_response(task, "test.provider", &response));
    EXPECT_EQ("second", response);
    EXPECT_FALSE(runtime.replay_llm_response(task, "test.provider", &response));

    std::remove(trace_path.c_str());
}

TEST(rasn_agent_runtime, stores_uri_and_host_port_endpoints)
{
    agent_runtime runtime("model", "unit.model");
    runtime.set_endpoint("127.0.0.1", 27102, "dsn://rasn/llm.agent");
    agent_descriptor descriptor = runtime.descriptor();
    EXPECT_EQ("127.0.0.1", descriptor.host);
    EXPECT_EQ(27102u, descriptor.port);
    EXPECT_EQ("dsn://rasn/llm.agent", descriptor.endpoint_uri);

    runtime.set_endpoint("localhost", 27103);
    descriptor = runtime.descriptor();
    EXPECT_EQ("localhost", descriptor.host);
    EXPECT_EQ(27103u, descriptor.port);
    EXPECT_TRUE(descriptor.endpoint_uri.empty());
}

TEST(rasn_agent_runtime, tracks_cancellable_inflight_requests)
{
    agent_runtime runtime("model", "unit.model");
    agent_request request;
    request.request_id = "cancel-runtime";
    request.trace_id = "trace-cancel-runtime";
    request.task.id = request.request_id;
    request.task.name = "unit.cancel";
    request.capability = "model.complete";
    request.input = "hello";

    agent_response rejection;
    ASSERT_TRUE(runtime.begin_request(request, &rejection));

    const agent_response accepted = runtime.cancel_request(request);
    EXPECT_TRUE(accepted.ok);
    EXPECT_TRUE(runtime.is_cancelled(request.request_id));

    EXPECT_FALSE(runtime.begin_request(request, &rejection));
    EXPECT_EQ("request_cancelled", rejection.error.code);

    runtime.finish_request(request);

    agent_request missing = request;
    missing.request_id = "missing-cancel-runtime";
    missing.task.id = missing.request_id;
    const agent_response not_found = runtime.cancel_request(missing);
    EXPECT_FALSE(not_found.ok);
    EXPECT_EQ("cancel_not_found", not_found.error.code);
}

TEST(rasn_agent_runtime, keeps_inflight_cancel_tombstones_under_pressure)
{
    agent_runtime runtime("model", "unit.model");
    agent_request original;
    original.request_id = "cancel-retained";
    original.trace_id = "trace-cancel-retained";
    original.task.id = original.request_id;
    original.task.name = "unit.cancel.retained";
    original.capability = "model.complete";
    original.input = "hello";

    agent_response rejection;
    ASSERT_TRUE(runtime.begin_request(original, &rejection));
    ASSERT_TRUE(runtime.cancel_request(original).ok);

    for (int i = 0; i < 1100; ++i)
    {
        agent_request filler = original;
        filler.request_id = "cancel-filler-" + std::to_string(i);
        filler.trace_id = filler.request_id;
        filler.task.id = filler.request_id;
        ASSERT_TRUE(runtime.begin_request(filler, &rejection));
        ASSERT_TRUE(runtime.cancel_request(filler).ok);
        runtime.finish_request(filler);
    }

    EXPECT_TRUE(runtime.is_cancelled(original.request_id));
    EXPECT_FALSE(runtime.begin_request(original, &rejection));
    EXPECT_EQ("request_cancelled", rejection.error.code);
    runtime.finish_request(original);
}

TEST(rasn_agent_runtime, tool_invoke_reports_cancellation_requested_while_inflight)
{
    rasn_tool_agent_service tool_agent;
    tool_agent.set_tool_provider(std::unique_ptr<agent_tool_provider>(new sleeping_tool_provider(50)));
    nucleus_runtime runtime;

    agent_request request;
    request.request_id = "cancel-tool";
    request.trace_id = runtime.trace_id();
    request.task.id = request.request_id;
    request.task.name = "unit.tool.cancel";
    request.capability = "tool.run";
    request.input = "list";
    agent_context_entry argument;
    argument.kind = "argument";
    argument.name = "arg";
    argument.value = ".";
    request.context.push_back(argument);

    agent_response response;
    std::thread invocation([&]() { response = tool_agent.invoke(request, runtime); });

    agent_response cancel;
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        cancel = tool_agent.cancel_request(request);
        if (cancel.ok)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    invocation.join();

    ASSERT_TRUE(cancel.ok);
    EXPECT_FALSE(response.ok);
    EXPECT_EQ("request_cancelled", response.error.code);
}

TEST(rasn_tool_resilience, admission_guard_caps_concurrent_tool_invocations)
{
    rasn_tool_agent_service tool_agent;
    blocking_tool_provider *provider = new blocking_tool_provider();
    tool_agent.set_tool_provider(std::unique_ptr<agent_tool_provider>(provider));
    nucleus_runtime runtime;

    const uint32_t configured_cap = 16;
    const uint32_t requests = configured_cap + 4;
    std::vector<tool_result> results(requests);
    std::vector<std::thread> threads;
    threads.reserve(requests);
    std::atomic<uint32_t> finished(0);
    for (uint32_t i = 0; i < requests; ++i)
    {
        threads.push_back(std::thread([&, i]() {
            agent_task task;
            task.id = "tool-admission-" + std::to_string(i);
            task.name = "unit.tool.admission";
            task.input = ".";
            results[i] = tool_agent.run_tool("list", std::vector<std::string>{"."}, runtime, task);
            ++finished;
        }));
    }

    const bool saturated = provider->wait_for_running(configured_cap, 5000);
    for (int attempt = 0; attempt < 200 && finished.load() < requests - configured_cap; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    provider->release_all();
    for (std::thread &thread : threads)
    {
        thread.join();
    }

    ASSERT_TRUE(saturated);
    EXPECT_EQ(configured_cap, provider->max_running());
    uint32_t ok = 0;
    uint32_t rejected = 0;
    for (const tool_result &result : results)
    {
        if (result.ok)
        {
            ++ok;
        }
        else if (result.error.find("tool admission control rejected") != std::string::npos)
        {
            ++rejected;
        }
    }
    EXPECT_EQ(configured_cap, ok);
    EXPECT_EQ(requests - configured_cap, rejected);

    uint32_t rejection_events = 0;
    for (const runtime_event &event : runtime.events())
    {
        if (event.kind == "tool.admission.rejected")
        {
            ++rejection_events;
        }
    }
    EXPECT_EQ(requests - configured_cap, rejection_events);
    EXPECT_NE(std::string::npos, tool_agent.tool_resilience_report().find("tool admission control"));
}

TEST(rasn_registry, tracks_heartbeat_leases_and_endpoint_refresh)
{
    agent_registry registry;
    agent_descriptor descriptor;
    descriptor.agent_id = "unit.agent";
    descriptor.role = "model";
    descriptor.app_name = "unit.model";
    descriptor.host = "127.0.0.1";
    descriptor.port = 27102;
    descriptor.health = "healthy";
    descriptor.capabilities.push_back(make_capability("model.complete", "text", "text", "none"));

    std::string error;
    agent_descriptor static_descriptor = descriptor;
    static_descriptor.agent_id = "unit.static";
    ASSERT_TRUE(registry.register_agent(static_descriptor, &error)) << error;
    ASSERT_TRUE(registry.register_agent(descriptor, &error, true)) << error;
    ASSERT_TRUE(registry.register_agent(descriptor, &error, true)) << error;
    EXPECT_EQ(2u, registry.query_by_capability("model.complete", true).size());

    agent_descriptor heartbeat = descriptor;
    heartbeat.host = "127.0.0.2";
    heartbeat.port = 27112;
    heartbeat.endpoint_uri = "dsn://rasn/unit.agent";
    ASSERT_TRUE(registry.heartbeat(heartbeat, &error)) << error;

    agent_descriptor found;
    ASSERT_TRUE(registry.find_agent("unit.agent", &found));
    EXPECT_EQ("127.0.0.2", found.host);
    EXPECT_EQ(27112u, found.port);
    EXPECT_EQ("dsn://rasn/unit.agent", found.endpoint_uri);

    EXPECT_EQ(1u, registry.expire_leases(::dsn_now_ms() + 10, 1));
    const std::vector<agent_descriptor> live_agents = registry.query_by_capability("model.complete", true);
    ASSERT_EQ(1u, live_agents.size());
    EXPECT_EQ("unit.static", live_agents[0].agent_id);
}

TEST(rasn_registry, shard_capabilities_select_exact_partition_owners)
{
    agent_registry registry;
    agent_descriptor shard0;
    shard0.agent_id = "rasn.runtime.blackboard@node-a";
    shard0.role = "rasn.runtime.blackboard";
    shard0.app_name = "rasn.runtime.blackboard";
    shard0.host = "127.0.0.1";
    shard0.port = 27117;
    shard0.health = "healthy";
    shard0.capabilities.push_back(
        make_capability("rasn.runtime.blackboard", "rasn_runtime_request", "rasn_runtime_response", "stateful"));
    shard0.capabilities.push_back(
        make_capability("rasn.runtime.blackboard.shard.0", "rasn_runtime_request", "rasn_runtime_response", "stateful"));

    agent_descriptor shard1 = shard0;
    shard1.agent_id = "rasn.runtime.blackboard@node-b";
    shard1.host = "127.0.0.2";
    shard1.port = 27127;
    shard1.capabilities.pop_back();
    shard1.capabilities.push_back(
        make_capability("rasn.runtime.blackboard.shard.1", "rasn_runtime_request", "rasn_runtime_response", "stateful"));

    std::string error;
    ASSERT_TRUE(registry.register_agent(shard0, &error, true)) << error;
    ASSERT_TRUE(registry.register_agent(shard1, &error, true)) << error;

    EXPECT_EQ(2u, registry.query_by_capability("rasn.runtime.blackboard", true).size());
    const std::vector<agent_descriptor> shard0_owners =
        registry.query_by_capability("rasn.runtime.blackboard.shard.0", true);
    ASSERT_EQ(1u, shard0_owners.size());
    EXPECT_EQ("rasn.runtime.blackboard@node-a", shard0_owners[0].agent_id);
    EXPECT_EQ(27117u, shard0_owners[0].port);

    const std::vector<agent_descriptor> shard1_owners =
        registry.query_by_capability("rasn.runtime.blackboard.shard.1", true);
    ASSERT_EQ(1u, shard1_owners.size());
    EXPECT_EQ("rasn.runtime.blackboard@node-b", shard1_owners[0].agent_id);
    EXPECT_EQ(27127u, shard1_owners[0].port);
}

TEST(rasn_registry, shared_state_record_round_trips_and_rejects_corruption)
{
    registry_state_record record;
    record.writer_fence = 0;
    record.descriptor.agent_id = "rasn.runtime.blackboard@dsn://node-a:27117/path";
    record.descriptor.role = "rasn.runtime.blackboard";
    record.descriptor.app_name = "rasn.runtime";
    record.descriptor.endpoint_uri = "dsn://node-a:27117/blackboard";
    record.descriptor.health = "healthy";
    record.descriptor.capabilities.push_back(
        make_capability("rasn.runtime.blackboard", "request\nbytes", "response", "stateful"));
    record.last_heartbeat_ms = 123456;
    record.lease_tracked = true;

    std::string encoded;
    std::string error;
    ASSERT_TRUE(encode_registry_state_record(record, &encoded, &error)) << error;

    registry_state_record decoded;
    ASSERT_TRUE(decode_registry_state_record(encoded, &decoded, &error)) << error;
    EXPECT_EQ(record.writer_fence, decoded.writer_fence);
    EXPECT_EQ(record.descriptor.agent_id, decoded.descriptor.agent_id);
    EXPECT_EQ(record.descriptor.endpoint_uri, decoded.descriptor.endpoint_uri);
    ASSERT_EQ(1u, decoded.descriptor.capabilities.size());
    EXPECT_EQ("request\nbytes", decoded.descriptor.capabilities[0].input_type);
    EXPECT_EQ(record.last_heartbeat_ms, decoded.last_heartbeat_ms);
    EXPECT_TRUE(decoded.lease_tracked);

    encoded.push_back('\0');
    EXPECT_FALSE(decode_registry_state_record(encoded, &decoded, &error));
    EXPECT_FALSE(error.empty());
}

TEST(rasn_registry, parses_bounded_unique_frontend_group)
{
    std::vector< ::dsn::rpc_address> addresses;
    std::string error;
    ASSERT_TRUE(parse_rasn_registry_addresses(
        "127.0.0.1:27100, 127.0.0.1:27101,127.0.0.1:27100",
        &addresses,
        &error))
        << error;
    ASSERT_EQ(2u, addresses.size());
    EXPECT_EQ(27100u, addresses[0].port());
    EXPECT_EQ(27101u, addresses[1].port());

    EXPECT_FALSE(parse_rasn_registry_addresses("dsn://cluster/registry", &addresses, &error));
    EXPECT_TRUE(addresses.empty());
    EXPECT_FALSE(error.empty());
}

TEST(rasn_registry, validates_ha_thread_pool_wiring)
{
    std::string error;
    EXPECT_FALSE(validate_rasn_registry_ha_pools(
        "THREAD_POOL_DEFAULT,THREAD_POOL_META_SERVER", true, &error));
    EXPECT_NE(std::string::npos, error.find("THREAD_POOL_DLOCK"));

    error.clear();
    EXPECT_FALSE(validate_rasn_registry_ha_pools(
        "THREAD_POOL_DEFAULT,THREAD_POOL_META_SERVER,THREAD_POOL_DLOCK",
        false,
        &error));
    EXPECT_NE(std::string::npos, error.find("partitioned=true"));

    error.clear();
    EXPECT_TRUE(validate_rasn_registry_ha_pools(
        "THREAD_POOL_DLOCK, THREAD_POOL_DEFAULT,THREAD_POOL_META_SERVER",
        true,
        &error))
        << error;
}

TEST(rasn_registry, prunes_old_epochs_without_copying_tombstones)
{
    rasn_coordination_config config;
    config.provider = "inproc";
    config.state_namespace =
        "/rasn-registry-prune-" + std::to_string(::dsn_now_ns());
    const std::shared_ptr<rasn_coordination_context> coordination =
        std::make_shared<rasn_coordination_context>(config);
    ASSERT_EQ(::dsn::ERR_OK, coordination->start());

    const std::string state_prefix = "registry/unit";
    const std::string resource = "rasn.registry.unit";
    const std::string owner = "unit-owner";
    agent_registry registry;
    std::string error;
    ASSERT_TRUE(registry.configure_shared_backend(
        coordination, state_prefix, resource, owner, &error))
        << error;

    std::string agent_key;
    for (uint64_t generation = 1; generation <= 5; ++generation)
    {
        uint64_t fence = 0;
        ASSERT_EQ(::dsn::ERR_OK,
                  coordination->service()->acquire_ownership(
                      resource, owner, 1000, &fence));
        ASSERT_EQ(generation, fence);
        const std::shared_ptr<std::atomic<bool>> leadership_lost =
            std::make_shared<std::atomic<bool>>(false);
        ASSERT_TRUE(registry.activate_shared_writer(
            fence, std::vector<agent_descriptor>(), leadership_lost, &error))
            << error;

        if (generation == 1)
        {
            agent_descriptor descriptor;
            descriptor.agent_id = "unit.pruned-agent";
            descriptor.role = "tool";
            descriptor.health = "healthy";
            ASSERT_TRUE(registry.register_agent(descriptor, &error, true)) << error;
            ASSERT_TRUE(registry.unregister_agent(descriptor.agent_id, &error)) << error;

            std::vector<std::string> agent_keys;
            ASSERT_EQ(::dsn::ERR_OK,
                      coordination->service()->list_state(
                          state_prefix + "/agents", agent_keys));
            ASSERT_EQ(1u, agent_keys.size());
            agent_key = agent_keys[0];
        }
        else if (generation == 2)
        {
            std::vector<std::string> versions;
            ASSERT_EQ(::dsn::ERR_OK,
                      coordination->service()->list_state(
                          state_prefix + "/agents/" + agent_key, versions));
            ASSERT_EQ(1u, versions.size());
            EXPECT_EQ("1", versions[0]);
        }

        size_t pruned = 0;
        ASSERT_TRUE(registry.prune_shared_history(3, &pruned, &error)) << error;
        registry.clear_shared_writer();
        ASSERT_EQ(::dsn::ERR_OK,
                  coordination->service()->release_ownership(
                      resource, owner, false));
    }

    std::vector<std::string> epochs;
    ASSERT_EQ(::dsn::ERR_OK,
              coordination->service()->list_state(
                  state_prefix + "/epochs", epochs));
    const std::set<std::string> retained(epochs.begin(), epochs.end());
    EXPECT_EQ((std::set<std::string>{"3", "4", "5"}), retained);
}

TEST(rasn_registry, unregistering_an_absent_agent_is_successful)
{
    agent_registry registry;
    std::string error;
    EXPECT_TRUE(registry.unregister_agent("already-absent", &error)) << error;

    agent_descriptor descriptor;
    descriptor.agent_id = "unit.unregister";
    descriptor.role = "tool";
    ASSERT_TRUE(registry.register_agent(descriptor, &error, true)) << error;
    EXPECT_TRUE(registry.unregister_agent(descriptor.agent_id, &error)) << error;
    EXPECT_TRUE(registry.unregister_agent(descriptor.agent_id, &error)) << error;
}

TEST(rasn_coordinator, retries_retryable_model_invocations_with_trace)
{
    agent_request request;
    request.request_id = "retry-model";
    request.trace_id = "trace-retry-model";
    request.capability = "model.complete";
    request.retry_budget = 2;
    request.task.id = request.request_id;
    request.task.name = "unit.retry";
    request.task.input = "retry";

    agent_descriptor agent;
    agent.agent_id = "unit.llm";

    nucleus_runtime runtime;
    int calls = 0;
    const agent_response response = coordinator_router::invoke_with_retries(
        request,
        runtime,
        agent,
        "unit.invoke",
        [&request, &calls](uint32_t) {
            ++calls;
            agent_response result;
            result.request_id = request.request_id;
            result.trace_id = request.trace_id;
            if (calls == 1)
            {
                result.ok = false;
                result.error = make_agent_error("provider", "transient", "temporary provider failure", true, "unit.llm");
                return result;
            }
            result.ok = true;
            result.output = "ok";
            return result;
        });

    ASSERT_TRUE(response.ok) << response.error.message;
    EXPECT_EQ(2, calls);

    size_t retry_events = 0;
    size_t retryable_failures = 0;
    for (const runtime_event &event : runtime.events())
    {
        if (event.kind == "retry")
        {
            ++retry_events;
            EXPECT_EQ(1u, event.retry_attempt);
        }
        if (event.kind == "failure" && event.retryable)
        {
            ++retryable_failures;
            EXPECT_EQ("transient", event.failure_code);
        }
    }
    EXPECT_EQ(1u, retry_events);
    EXPECT_EQ(1u, retryable_failures);
}

TEST(rasn_agent_executor, parses_tool_directive)
{
    agent_executor_tool_call tool;
    ASSERT_TRUE(parse_agent_tool_directive("thinking\nRASN_TOOL read_file src/main.cpp\n", "RASN_TOOL ", &tool));
    EXPECT_EQ("read_file", tool.name);
    ASSERT_EQ(1u, tool.args.size());
    EXPECT_EQ("src/main.cpp", tool.args[0]);

    EXPECT_FALSE(parse_agent_tool_directive("no tool needed", "RASN_TOOL ", &tool));
    EXPECT_FALSE(parse_agent_tool_directive("RASN_TOOL   \n", "RASN_TOOL ", &tool));
}

TEST(rasn_agent_executor, executes_tool_then_final_answer)
{
    agent_executor_request request;
    request.task.id = "executor-task";
    request.task.name = "unit.executor";
    request.prompt = "inspect the repo";
    request.system_prompt = "system";
    request.context.push_back("initial context");

    agent_executor_options options;
    options.max_tool_calls = 3;
    options.tool_instruction = "tool instructions";

    int model_calls = 0;
    std::string second_model_context;
    agent_plan_executor executor;
    const agent_executor_result result = executor.execute(
        request,
        options,
        [&model_calls, &second_model_context](const agent_executor_model_request &model_request) {
            ++model_calls;
            agent_response response;
            response.request_id = model_request.request_id;
            response.trace_id = "trace-executor";
            response.ok = true;
            if (model_calls == 1)
            {
                EXPECT_EQ("executor-task/model/0", model_request.request_id);
                EXPECT_NE(std::string::npos, model_request.system_prompt.find("system tool instructions"));
                response.output = "RASN_TOOL read_file src/main.cpp";
            }
            else
            {
                EXPECT_EQ("executor-task/model/1", model_request.request_id);
                EXPECT_FALSE(model_request.context.empty());
                if (!model_request.context.empty())
                {
                    second_model_context = model_request.context.back();
                }
                response.output = "final answer";
            }
            return response;
        },
        [](const agent_executor_tool_call &tool, std::vector<std::string> *policy_labels) {
            EXPECT_EQ("read_file", tool.name);
            if (policy_labels != nullptr)
            {
                policy_labels->push_back("approved");
            }
            return true;
        },
        [](const agent_executor_tool_request &tool_request) {
            EXPECT_EQ("executor-task/tool/0", tool_request.request_id);
            EXPECT_EQ("read_file", tool_request.tool.name);
            EXPECT_EQ(1u, tool_request.policy_labels.size());
            if (!tool_request.policy_labels.empty())
            {
                EXPECT_EQ("approved", tool_request.policy_labels[0]);
            }
            tool_result result;
            result.ok = true;
            result.output = "file contents";
            return result;
        });

    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_EQ("ok", result.status);
    EXPECT_EQ("final answer", result.output);
    EXPECT_EQ(2, model_calls);
    ASSERT_EQ(2u, result.steps.size());
    EXPECT_TRUE(result.steps[0].requested_tool);
    EXPECT_EQ("read_file", result.steps[0].tool.name);
    EXPECT_FALSE(result.steps[1].requested_tool);
    EXPECT_NE(std::string::npos, second_model_context.find("Tool read_file succeeded"));
    EXPECT_NE(std::string::npos, second_model_context.find("file contents"));
}

TEST(rasn_agent_executor, stops_at_tool_limit)
{
    agent_executor_request request;
    request.task.id = "executor-limit";
    request.prompt = "loop";

    agent_executor_options options;
    options.max_tool_calls = 2;

    agent_plan_executor executor;
    const agent_executor_result result = executor.execute(
        request,
        options,
        [](const agent_executor_model_request &model_request) {
            agent_response response;
            response.request_id = model_request.request_id;
            response.trace_id = "trace-limit";
            response.ok = true;
            response.output = "RASN_TOOL echo hi";
            return response;
        },
        agent_plan_executor::approval_callback(),
        [](const agent_executor_tool_request &) {
            tool_result result;
            result.ok = true;
            result.output = "hi";
            return result;
        });

    EXPECT_FALSE(result.ok);
    EXPECT_EQ("tool-limit", result.status);
    EXPECT_EQ("agent stopped after reaching the tool-call limit", result.error);
    ASSERT_EQ(2u, result.steps.size());
    EXPECT_TRUE(result.steps[0].requested_tool);
    EXPECT_TRUE(result.steps[1].requested_tool);
}

TEST(rasn_coordinator, does_not_retry_tool_invocations)
{
    agent_request request;
    request.request_id = "retry-tool";
    request.trace_id = "trace-retry-tool";
    request.capability = "tool.run";
    request.retry_budget = 2;
    request.task.id = request.request_id;
    request.task.name = "unit.tool";
    request.task.input = "tool";

    agent_descriptor agent;
    agent.agent_id = "unit.tool";

    nucleus_runtime runtime;
    int calls = 0;
    const agent_response response = coordinator_router::invoke_with_retries(
        request,
        runtime,
        agent,
        "unit.invoke",
        [&request, &calls](uint32_t) {
            ++calls;
            agent_response result;
            result.request_id = request.request_id;
            result.trace_id = request.trace_id;
            result.ok = false;
            result.error = make_agent_error("rpc", "ambiguous_tool_rpc", "tool RPC may have executed", true, "unit.tool");
            return result;
        });

    EXPECT_FALSE(response.ok);
    EXPECT_EQ(1, calls);
    for (const runtime_event &event : runtime.events())
    {
        EXPECT_NE("retry", event.kind);
    }
}

TEST(rasn_coordinator, validates_remote_endpoint_before_rpc_dispatch)
{
    agent_descriptor agent;
    agent.agent_id = "unit.invalid";

    ::dsn::rpc_address address;
    std::string error = "stale";
    EXPECT_FALSE(coordinator_router::validate_remote_endpoint(agent, &address, &error));
    EXPECT_NE(std::string::npos, error.find("no endpoint"));

    agent.host = "0.0.0.0";
    agent.port = 27102;
    EXPECT_FALSE(coordinator_router::validate_remote_endpoint(agent, &address, &error));
    EXPECT_NE(std::string::npos, error.find("could not be resolved"));

    agent.host = "127.0.0.1";
    EXPECT_TRUE(coordinator_router::validate_remote_endpoint(agent, &address, &error));
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(static_cast<uint16_t>(27102), address.port());
    EXPECT_FALSE(address.is_invalid());

    // Address is optional: a null out-param must still validate.
    EXPECT_TRUE(coordinator_router::validate_remote_endpoint(agent, nullptr, &error));
}

TEST(rasn_policy, classifies_codepilot_tools_and_builds_requests)
{
    EXPECT_EQ(tool_side_effect::read_only, classify_tool_side_effect("list"));
    EXPECT_EQ(tool_side_effect::read_only, classify_tool_side_effect("read"));
    EXPECT_EQ(tool_side_effect::read_only, classify_tool_side_effect("search"));
    EXPECT_EQ(tool_side_effect::write, classify_tool_side_effect("write"));
    EXPECT_EQ(tool_side_effect::write, classify_tool_side_effect("replace"));
    EXPECT_EQ(tool_side_effect::shell, classify_tool_side_effect("shell"));
    EXPECT_EQ(tool_side_effect::unknown, classify_tool_side_effect("unknown"));

    agent_task task;
    task.name = "unit.actor";
    const policy_request request = make_policy_request("read", std::vector<std::string>{"README.md"}, task);
    EXPECT_EQ("read", request.tool_name);
    EXPECT_EQ("read_only", request.side_effect);
    EXPECT_EQ("README.md", request.target);
    EXPECT_EQ("unit.actor", request.actor);
}

TEST(rasn_policy, checks_tool_targets_against_workspace_root)
{
    const std::string root = temp_file_path("rasn-policy-root");
    ::dsn::utils::filesystem::create_directory(root);

#if defined(_WIN32)
    const std::string inside = root + "\\child.txt";
    const std::string outside = root + "-outside\\child.txt";
#else
    const std::string inside = root + "/child.txt";
    const std::string outside = root + "-outside/child.txt";
#endif

    std::string normalized_target;
    std::string normalized_root;
    EXPECT_TRUE(policy_target_within_workspace(inside, root, &normalized_target, &normalized_root));
    EXPECT_FALSE(normalized_target.empty());
    EXPECT_FALSE(normalized_root.empty());
    EXPECT_FALSE(policy_target_within_workspace(outside, root, &normalized_target, &normalized_root));
    EXPECT_TRUE(policy_target_within_workspace(outside, "", &normalized_target, &normalized_root));
    EXPECT_TRUE(normalized_root.empty());

    ::dsn::utils::filesystem::remove_path(root);
}

TEST(rasn_policy, indexes_oversized_tool_output_through_configured_state_writer)
{
    captured_policy_state_records().clear();
    set_policy_state_writer(&capture_policy_state_record);

    agent_task task;
    task.id = "policy-artifact-unit";
    task.name = "unit.policy";
    tool_result result;
    result.ok = true;
    result.output.assign(70000, 'x');

    const tool_result bounded = global_policy_manager().apply_tool_output_bounds("read", task, result);
    reset_policy_state_writer();

    ASSERT_EQ(1u, captured_policy_state_records().size());
    const state_record record = captured_policy_state_records()[0];
    EXPECT_EQ("artifact", record.kind);
    EXPECT_EQ("rasn.policy", record.scope);
    EXPECT_NE(std::string::npos, bounded.output.find("state_key=" + record.key));
    const std::string artifact_path = artifact_path_from_record(record);
    EXPECT_FALSE(artifact_path.empty());
    std::remove(artifact_path.c_str());
}

TEST(rasn_policy, redacts_tool_output_before_preview_and_artifact_spill)
{
    captured_policy_state_records().clear();
    set_policy_state_writer(&capture_policy_state_record);

    agent_task task;
    task.id = "policy-redaction-unit";
    task.name = "unit.policy";
    tool_result result;
    result.ok = true;
    result.output = "prefix api_key=sk-tool-artifact-secret ";
    result.output.append(70000, 'x');

    const tool_result bounded = global_policy_manager().apply_tool_output_bounds("read", task, result);
    reset_policy_state_writer();

    EXPECT_EQ(std::string::npos, bounded.output.find("sk-tool-artifact-secret"));
    EXPECT_NE(std::string::npos, bounded.output.find("<redacted-secret>"));
    ASSERT_EQ(1u, captured_policy_state_records().size());
    const std::string artifact_path = artifact_path_from_record(captured_policy_state_records()[0]);
    ASSERT_FALSE(artifact_path.empty());
    const std::string artifact = read_text_file(artifact_path);
    EXPECT_EQ(std::string::npos, artifact.find("sk-tool-artifact-secret"));
    EXPECT_NE(std::string::npos, artifact.find("<redacted-secret>"));
    std::remove(artifact_path.c_str());
}

TEST(rasn_observability, indexes_snapshot_through_configured_state_writer)
{
    captured_observability_state_records().clear();
    set_observability_state_writer(&capture_observability_state_record);

    observability_response snapshot;
    runtime_event event;
    event.sequence = 7;
    event.trace_id = "trace-unit";
    event.task_id = "task-unit";
    event.kind = "task.finish";
    event.name = "unit";
    event.value = "ok";
    snapshot.events.push_back(event);
    snapshot.last_sequence = event.sequence;
    failure_record failure;
    failure.sequence = event.sequence;
    failure.trace_id = event.trace_id;
    failure.task_id = event.task_id;
    failure.failure_class = "unit";
    failure.code = "simulated";
    snapshot.failures.push_back(failure);

    const state_response stored = index_observability_snapshot(snapshot, "trace-unit", "trace.jsonl");
    reset_observability_state_writer();

    ASSERT_TRUE(stored.ok) << stored.error;
    ASSERT_EQ(1u, captured_observability_state_records().size());
    const state_record record = captured_observability_state_records()[0];
    EXPECT_EQ("observability_snapshot", record.kind);
    EXPECT_EQ("rasn.observability", record.scope);
    EXPECT_EQ("observability-snapshot/trace-unit/7", record.key);
    EXPECT_NE(std::string::npos, record.value.find("trace_file=trace.jsonl"));
    EXPECT_NE(std::string::npos, record.value.find("events=1"));
    EXPECT_NE(std::string::npos, record.value.find("failures=1"));
    EXPECT_NE(std::string::npos, record.value.find("last_sequence=7"));
}

TEST(rasn_service_graph, retains_lifecycle_until_last_owner_releases)
{
    rasn_service_graph services;
    EXPECT_FALSE(services.is_started());
    EXPECT_EQ(0u, services.lifecycle_ref_count());

    services.acquire();
    EXPECT_TRUE(services.is_started());
    EXPECT_EQ(1u, services.lifecycle_ref_count());

    services.acquire();
    EXPECT_TRUE(services.is_started());
    EXPECT_EQ(2u, services.lifecycle_ref_count());

    services.stop();
    EXPECT_TRUE(services.is_started());
    EXPECT_EQ(2u, services.lifecycle_ref_count());

    services.release();
    EXPECT_TRUE(services.is_started());
    EXPECT_EQ(1u, services.lifecycle_ref_count());

    services.release();
    EXPECT_FALSE(services.is_started());
    EXPECT_EQ(0u, services.lifecycle_ref_count());

    services.release();
    EXPECT_FALSE(services.is_started());
    EXPECT_EQ(0u, services.lifecycle_ref_count());
}

TEST(rasn_service_graph, installing_tool_provider_does_not_eagerly_start_graph)
{
    rasn_service_graph services;
    EXPECT_FALSE(services.is_started());

    services.set_tool_provider(std::unique_ptr<agent_tool_provider>(new sleeping_tool_provider(0)));

    EXPECT_FALSE(services.is_started());
    EXPECT_EQ(0u, services.lifecycle_ref_count());
}

TEST(rasn_core, split_words_and_normalize_platform_paths)
{
    const std::vector<std::string> words = split_words("task inspect ask \"hello world\" after prepare");
    ASSERT_EQ(6u, words.size());
    EXPECT_EQ("task", words[0]);
    EXPECT_EQ("inspect", words[1]);
    EXPECT_EQ("ask", words[2]);
    EXPECT_EQ("hello world", words[3]);

#if defined(_WIN32)
    EXPECT_EQ("a\\b\\c", normalize_platform_path("a/b\\c"));
#else
    EXPECT_EQ("a/b/c", normalize_platform_path("a\\b/c"));
#endif
}

TEST(rasn_agent_control_plane, manages_lifecycle_capabilities_and_leases)
{
    agent_control_plane plane;
    agent_control_record record;
    record.descriptor = make_unit_agent_descriptor("agent-a", "worker", "model.complete");
    record.state = "starting";
    record.placement = "node-a";
    record.restart_policy = "always";

    std::string error;
    ASSERT_TRUE(plane.upsert_agent(record, &error)) << error;

    agent_control_lease lease;
    lease = plane.acquire_lease("agent-a", "owner-a", 900, 0);
    ASSERT_TRUE(lease.ok) << lease.error;
    EXPECT_FALSE(plane.acquire_lease("agent-a", "owner-b", 950, 100).ok);
    EXPECT_EQ(0u, plane.expire_leases(1000));
    EXPECT_TRUE(plane.release_lease("agent-a", "owner-a", &error)) << error;

    lease = plane.acquire_lease("agent-a", "owner-a", 1000, 100);
    ASSERT_TRUE(lease.ok) << lease.error;
    EXPECT_EQ("owner-a", lease.owner);
    EXPECT_FALSE(plane.acquire_lease("agent-a", "owner-b", 1050, 100).ok);
    EXPECT_EQ(1u, plane.expire_leases(1200));

    lease = plane.acquire_lease("agent-a", "owner-b", 1200, 100);
    ASSERT_TRUE(lease.ok) << lease.error;
    EXPECT_TRUE(plane.heartbeat("agent-a", 1210, &error)) << error;

    std::vector<agent_control_record> capable = plane.query_by_capability("model.complete", true, 1210);
    ASSERT_EQ(1u, capable.size());
    EXPECT_EQ("running", capable[0].state);
    EXPECT_EQ("owner-b", capable[0].owner);

    EXPECT_TRUE(plane.transition("agent-a", "failed", "boom", &error)) << error;
    EXPECT_TRUE(plane.query_by_capability("model.complete", true, 1220).empty());
}

TEST(rasn_agent_message_bus, delivers_defers_acks_and_deadletters_messages)
{
    agent_message_bus bus;
    agent_message message;
    message.message_id = "msg-1";
    message.sender = "planner";
    message.receiver = "worker";
    message.type = "task.request";
    message.payload = "inspect";
    message.deadline_ms = 2000;

    std::string error;
    ASSERT_TRUE(bus.publish(message, nullptr, &error)) << error;

    std::vector<agent_message> pulled = bus.pull("worker", 1, 1000);
    ASSERT_EQ(1u, pulled.size());
    EXPECT_EQ("delivered", pulled[0].state);
    EXPECT_EQ(1u, pulled[0].attempt);

    EXPECT_TRUE(bus.defer("msg-1", 1500, "backpressure", &error)) << error;
    EXPECT_TRUE(bus.pull("worker", 1, 1400).empty());
    pulled = bus.pull("worker", 1, 1500);
    ASSERT_EQ(1u, pulled.size());
    EXPECT_EQ(2u, pulled[0].attempt);
    EXPECT_TRUE(bus.ack("msg-1", &error)) << error;
    EXPECT_FALSE(bus.dead_letter("msg-1", "after ack", &error));
    agent_message terminal;
    ASSERT_TRUE(bus.find("msg-1", &terminal));
    EXPECT_EQ("acked", terminal.state);

    message.message_id = "msg-2";
    message.deadline_ms = 1600;
    ASSERT_TRUE(bus.publish(message, nullptr, &error)) << error;
    EXPECT_EQ(1u, bus.expire_deadlines(1700));
    agent_message expired;
    ASSERT_TRUE(bus.find("msg-2", &expired));
    EXPECT_EQ("deadline_expired", expired.state);
    EXPECT_FALSE(bus.ack("msg-2", &error));

    message.message_id = "msg-3";
    message.deadline_ms = 0;
    ASSERT_TRUE(bus.publish(message, nullptr, &error)) << error;
    EXPECT_TRUE(bus.dead_letter("msg-3", "failed", &error)) << error;
    ASSERT_TRUE(bus.find("msg-3", &expired));
    EXPECT_EQ("dead_letter", expired.state);
    EXPECT_FALSE(bus.ack("msg-3", &error));
    ASSERT_TRUE(bus.find("msg-3", &expired));
    EXPECT_EQ("dead_letter", expired.state);
}

TEST(rasn_task_orchestration, schedules_dependencies_and_terminal_transitions)
{
    task_orchestration_kernel kernel;
    orchestration_task inspect;
    inspect.task_id = "inspect";
    inspect.input = "list files";
    orchestration_task summarize;
    summarize.task_id = "summarize";
    summarize.depends_on.push_back("inspect");

    std::string error;
    ASSERT_TRUE(kernel.add_task(inspect, &error)) << error;
    ASSERT_TRUE(kernel.add_task(summarize, &error)) << error;

    std::vector<orchestration_task> ready = kernel.ready_tasks();
    ASSERT_EQ(1u, ready.size());
    EXPECT_EQ("inspect", ready[0].task_id);
    EXPECT_EQ(1u, kernel.blocked_tasks().size());

    EXPECT_TRUE(kernel.start("inspect", "agent-a", &error)) << error;
    EXPECT_TRUE(kernel.complete("inspect", "files", &error)) << error;
    EXPECT_FALSE(kernel.start("inspect", "agent-a", &error));
    orchestration_task loaded;
    ASSERT_TRUE(kernel.find("inspect", &loaded));
    EXPECT_EQ("completed", loaded.state);
    ready = kernel.ready_tasks();
    ASSERT_EQ(1u, ready.size());
    EXPECT_EQ("summarize", ready[0].task_id);

    EXPECT_TRUE(kernel.assign("summarize", "agent-b", &error)) << error;
    EXPECT_TRUE(kernel.start("summarize", "agent-b", &error)) << error;
    EXPECT_TRUE(kernel.fail("summarize", "retry", true, &error)) << error;
    ASSERT_TRUE(kernel.find("summarize", &loaded));
    EXPECT_EQ("pending", loaded.state);
}

TEST(rasn_determinism_ledger, records_and_replays_choices)
{
    determinism_ledger ledger;
    deterministic_replay_result first = ledger.choose("task", "model", "unit", []() { return "generated"; });
    ASSERT_TRUE(first.ok) << first.error;
    EXPECT_FALSE(first.replayed);
    EXPECT_EQ("generated", first.choice.value);
    EXPECT_NE(std::string::npos, ledger.to_jsonl().find("\"key\":\"model\""));

    determinism_ledger replay;
    replay.set_replay_choices(ledger.snapshot());
    bool called = false;
    deterministic_replay_result second = replay.choose("task", "model", "unit", [&called]() {
        called = true;
        return "new";
    });
    ASSERT_TRUE(second.ok) << second.error;
    EXPECT_TRUE(second.replayed);
    EXPECT_FALSE(called);
    EXPECT_EQ("generated", second.choice.value);
}

TEST(rasn_determinism_ledger, hydration_preserves_repeated_keys_by_sequence)
{
    determinism_ledger ledger;
    deterministic_choice first;
    first.sequence = 1;
    first.task_id = "task";
    first.key = "model";
    first.source = "unit";
    first.value = "first";
    std::string error;
    ASSERT_TRUE(ledger.hydrate_choice(first, &error)) << error;

    deterministic_choice second = first;
    second.sequence = 2;
    second.value = "second";
    ASSERT_TRUE(ledger.hydrate_choice(second, &error)) << error;
    ASSERT_EQ(2u, ledger.snapshot().size());
    const deterministic_replay_result replayed = ledger.replay("task", "model");
    ASSERT_TRUE(replayed.ok) << replayed.error;
    EXPECT_EQ("second", replayed.choice.value);

    deterministic_choice conflicting = second;
    conflicting.key = "tool";
    EXPECT_FALSE(ledger.hydrate_choice(conflicting, &error));
}

TEST(rasn_sandbox_runtime, evaluates_filesystem_network_and_process_policy)
{
    const std::string root = temp_file_path("rasn-sandbox-root");
    sandbox_profile read_only = default_read_only_sandbox_profile();

    sandbox_request request;
    request.operation = "fs.read";
    request.path = ::dsn::utils::filesystem::path_combine(root, "file.txt");
    EXPECT_TRUE(evaluate_sandbox_request(read_only, request).allowed);

    request.operation = "fs.write";
    EXPECT_FALSE(evaluate_sandbox_request(read_only, request).allowed);

    sandbox_profile write_profile = default_workspace_write_sandbox_profile(root);
    EXPECT_TRUE(evaluate_sandbox_request(write_profile, request).allowed);
    request.path = temp_file_path("rasn-sandbox-outside.txt");
    EXPECT_FALSE(evaluate_sandbox_request(write_profile, request).allowed);

    sandbox_profile network = read_only;
    network.name = "network";
    network.allow_network = true;
    network.allowed_network_hosts.push_back("api.example.test");
    request = sandbox_request();
    request.operation = "network";
    request.network_host = "api.example.test";
    EXPECT_TRUE(evaluate_sandbox_request(network, request).allowed);
    request.network_host = "evil.example.test";
    EXPECT_FALSE(evaluate_sandbox_request(network, request).allowed);
}

TEST(rasn_capability_directory, ranks_and_filters_capability_providers)
{
    capability_directory directory;
    capability_provider fast;
    fast.descriptor = make_unit_agent_descriptor("agent-fast", "worker", "model.complete");
    fast.descriptor.capabilities[0].latency_hint_ms = 50;
    fast.descriptor.capabilities[0].reliability_hint = 99;
    fast.descriptor.health = "healthy";
    fast.labels.push_back("local");
    fast.load = 10;

    capability_provider slow = fast;
    slow.descriptor.agent_id = "agent-slow";
    slow.descriptor.capabilities[0].latency_hint_ms = 500;
    slow.load = 20;

    std::string error;
    ASSERT_TRUE(directory.upsert_provider(slow, &error)) << error;
    ASSERT_TRUE(directory.upsert_provider(fast, &error)) << error;

    capability_query query;
    query.capability = "model.complete";
    query.required_labels.push_back("local");
    capability_match best;
    ASSERT_TRUE(directory.choose_best(query, &best, &error)) << error;
    EXPECT_EQ("agent-fast", best.provider.descriptor.agent_id);
    EXPECT_EQ(2u, directory.query(query).size());

    query.max_load = 15;
    std::vector<capability_match> filtered = directory.query(query);
    ASSERT_EQ(1u, filtered.size());
    EXPECT_EQ("agent-fast", filtered[0].provider.descriptor.agent_id);
}

TEST(rasn_resource_budget, reserves_and_denies_over_budget_requests)
{
    resource_budget_manager budgets;
    resource_quota quota;
    quota.scope = "session";
    quota.max_tokens = 100;
    quota.max_tool_calls = 2;

    std::string error;
    ASSERT_TRUE(budgets.configure(quota, &error)) << error;

    resource_request request;
    request.scope = "session";
    request.tokens = 40;
    request.tool_calls = 1;
    EXPECT_TRUE(budgets.reserve(request).allowed);
    EXPECT_TRUE(budgets.reserve(request).allowed);
    resource_budget_decision denied = budgets.reserve(request);
    EXPECT_FALSE(denied.allowed);
    EXPECT_NE(std::string::npos, denied.reason.find("token budget"));

    EXPECT_TRUE(budgets.release(request, &error)) << error;
    resource_usage usage;
    ASSERT_TRUE(budgets.usage("session", &usage));
    EXPECT_EQ(40u, usage.tokens);
    EXPECT_EQ(1u, usage.tool_calls);
}

TEST(rasn_runtime_hydration, hydrate_methods_preserve_generated_state)
{
    std::string error;

    agent_control_plane control;
    agent_control_record agent;
    agent.descriptor.agent_id = "agent/hydrated";
    agent.descriptor.role = "cli";
    agent.state = "running";
    agent.generation = 7;
    agent.last_heartbeat_ms = 1234;
    ASSERT_TRUE(control.hydrate_agent(agent, &error)) << error;
    agent_control_record found_agent;
    ASSERT_TRUE(control.find("agent/hydrated", &found_agent));
    EXPECT_EQ(7u, found_agent.generation);
    EXPECT_EQ(1234u, found_agent.last_heartbeat_ms);

    task_orchestration_kernel tasks;
    orchestration_task task;
    task.task_id = "task/hydrated";
    task.state = "completed";
    task.output = "done";
    task.generation = 9;
    ASSERT_TRUE(tasks.hydrate_task(task, &error)) << error;
    orchestration_task found_task;
    ASSERT_TRUE(tasks.find("task/hydrated", &found_task));
    EXPECT_EQ("completed", found_task.state);
    EXPECT_EQ(9u, found_task.generation);

    resource_budget_manager budgets;
    resource_usage usage;
    usage.scope = "session/hydrated";
    usage.tokens = 42;
    usage.tool_calls = 3;
    ASSERT_TRUE(budgets.hydrate_usage(usage, &error)) << error;
    resource_usage found_usage;
    ASSERT_TRUE(budgets.usage("session/hydrated", &found_usage));
    EXPECT_EQ(42u, found_usage.tokens);
    EXPECT_EQ(3u, found_usage.tool_calls);

    shared_blackboard board;
    blackboard_entry entry;
    entry.key = "entry/hydrated";
    entry.value = "payload";
    entry.generation = 11;
    entry.created_at_ms = 100;
    entry.updated_at_ms = 200;
    ASSERT_TRUE(board.hydrate_entry(entry, &error)) << error;
    blackboard_entry found_entry;
    ASSERT_TRUE(board.get("entry/hydrated", &found_entry));
    EXPECT_EQ(11u, found_entry.generation);
    EXPECT_EQ(200u, found_entry.updated_at_ms);

    determinism_ledger ledger;
    deterministic_choice choice;
    choice.sequence = 13;
    choice.task_id = "task";
    choice.key = "choice";
    choice.value = "selected";
    ASSERT_TRUE(ledger.hydrate_choice(choice, &error)) << error;
    ASSERT_EQ(1u, ledger.snapshot().size());
    EXPECT_EQ(13u, ledger.snapshot()[0].sequence);

    recovery_supervisor recovery;
    failure_observation failure;
    failure.task_id = "task/hydrated";
    failure.component = "cli";
    failure.failure_class = "transient";
    failure.code = "timeout";
    failure.attempt = 1;
    failure.time_ms = 123;
    ASSERT_TRUE(recovery.hydrate_failure(failure, &error)) << error;
    failure.message = "updated";
    ASSERT_TRUE(recovery.hydrate_failure(failure, &error)) << error;
    ASSERT_EQ(1u, recovery.history().size());
    EXPECT_EQ("updated", recovery.history()[0].message);

    human_interaction_queue humans;
    human_interaction_request human;
    human.request_id = "human/hydrated";
    human.prompt = "approve?";
    human.state = "answered";
    human.answer = "yes";
    ASSERT_TRUE(humans.hydrate_request(human, &error)) << error;
    ASSERT_EQ(1u, humans.snapshot().size());
    EXPECT_EQ("answered", humans.snapshot()[0].state);
}

TEST(rasn_recovery_supervisor, chooses_retry_escalate_and_abort_actions)
{
    recovery_supervisor supervisor;
    recovery_policy transient;
    transient.failure_class = "transient";
    transient.retryable = true;
    transient.max_attempts = 3;
    transient.retry_delay_ms = 25;
    transient.escalate_after_attempts = 3;

    std::string error;
    ASSERT_TRUE(supervisor.set_policy(transient, &error)) << error;

    failure_observation failure;
    failure.task_id = "task";
    failure.component = "model";
    failure.failure_class = "transient";
    failure.retryable = true;
    failure.attempt = 1;
    recovery_action action = supervisor.observe(failure);
    EXPECT_EQ("retry", action.action);
    EXPECT_EQ(25u, action.delay_ms);

    failure.attempt = 3;
    action = supervisor.observe(failure);
    EXPECT_EQ("escalate", action.action);

    failure.failure_class = "policy";
    failure.retryable = false;
    action = supervisor.observe(failure);
    EXPECT_EQ("abort", action.action);
    EXPECT_EQ(3u, supervisor.history().size());
}

TEST(rasn_blackboard, stores_queries_and_compacts_entries)
{
    shared_blackboard board;
    blackboard_entry entry;
    entry.key = "task/1/input";
    entry.kind = "input";
    entry.owner = "agent";
    entry.value = "payload";
    entry.tags.push_back("task");
    entry.expires_at_ms = 200;

    std::string error;
    blackboard_entry stored;
    ASSERT_TRUE(board.put(entry, &stored, &error)) << error;
    EXPECT_EQ(1u, stored.generation);
    entry.value = "updated";
    ASSERT_TRUE(board.put(entry, &stored, &error)) << error;
    EXPECT_EQ(2u, stored.generation);

    blackboard_query query;
    query.key_prefix = "task/";
    query.tags.push_back("task");
    query.now_ms = 100;
    ASSERT_EQ(1u, board.query(query).size());
    EXPECT_EQ(1u, board.compact_expired(250));
    EXPECT_TRUE(board.snapshot(false, 250).empty());
}

TEST(rasn_contract_verifier, reports_contract_violations)
{
    contract_verifier verifier;
    agent_contract contract;
    contract.contract_id = "answer";
    contract.require_input_non_empty = true;
    contract.require_output_non_empty = true;
    contract.required_output_fragments.push_back("Summary");
    contract.forbidden_output_fragments.push_back("SECRET");
    contract.required_policy_labels.push_back("sandbox:read-only");

    std::string error;
    ASSERT_TRUE(verifier.register_contract(contract, &error)) << error;
    contract_evaluation ok =
        verifier.evaluate("answer", "input", "Summary: safe", std::vector<std::string>{"sandbox:read-only"});
    EXPECT_TRUE(ok.ok);

    contract_evaluation bad =
        verifier.evaluate("answer", "", "SECRET", std::vector<std::string>());
    EXPECT_FALSE(bad.ok);
    EXPECT_GE(bad.violations.size(), 4u);
}

TEST(rasn_human_interaction, tracks_answers_cancellation_and_expiry)
{
    human_interaction_queue queue;
    human_interaction_request request;
    request.request_id = "approval-1";
    request.kind = "approval";
    request.requester = "agent";
    request.prompt = "Approve?";
    request.choices.push_back("yes");
    request.choices.push_back("no");
    request.deadline_ms = 200;

    human_interaction_result opened = queue.open(request, 100);
    ASSERT_TRUE(opened.ok) << opened.error;
    EXPECT_EQ(100u, opened.request.created_at_ms);
    EXPECT_EQ(100u, opened.request.updated_at_ms);
    EXPECT_EQ(1u, queue.pending("agent").size());

    human_interaction_result rejected = queue.answer("approval-1", "maybe", 120);
    EXPECT_FALSE(rejected.ok);
    human_interaction_result answered = queue.answer("approval-1", "yes", 130);
    ASSERT_TRUE(answered.ok) << answered.error;
    EXPECT_EQ("answered", answered.request.state);
    EXPECT_EQ(130u, answered.request.updated_at_ms);

    request.request_id = "approval-2";
    opened = queue.open(request, 140);
    ASSERT_TRUE(opened.ok) << opened.error;
    EXPECT_EQ(1u, queue.expire(250));
    human_interaction_request expired;
    ASSERT_TRUE(queue.find("approval-2", &expired));
    EXPECT_EQ("expired", expired.state);
    EXPECT_EQ(250u, expired.updated_at_ms);

    request.request_id = "approval-3";
    request.deadline_ms = 0;
    opened = queue.open(request, 260);
    ASSERT_TRUE(opened.ok) << opened.error;
    const human_interaction_result cancelled = queue.cancel("approval-3", "superseded", 270);
    ASSERT_TRUE(cancelled.ok) << cancelled.error;
    EXPECT_EQ("cancelled", cancelled.request.state);
    EXPECT_EQ("superseded", cancelled.request.answer);
    EXPECT_EQ(270u, cancelled.request.updated_at_ms);
}

TEST(rasn_tool_catalog, describes_aliases_and_normalizes_invocations)
{
    tool_catalog catalog;
    catalog.add(make_tool_descriptor("read",
                                     "read_only",
                                     "Read a file.",
                                     std::vector<tool_argument_descriptor>{
                                         make_tool_argument("path", true, "File to read."),
                                         make_tool_argument("max_bytes", false, "Maximum bytes.")}),
                std::vector<std::string>{"cat"});

    EXPECT_TRUE(catalog.contains("read"));
    EXPECT_TRUE(catalog.contains("cat"));
    EXPECT_EQ("read", catalog.canonical_name("cat"));
    EXPECT_NE(std::string::npos, catalog.describe().find("aliases=cat"));

    const tool_invocation ok = normalize_tool_invocation(catalog, "cat", std::vector<std::string>{"a.txt"});
    ASSERT_TRUE(ok.ok) << ok.error;
    EXPECT_EQ("read", ok.name);
    ASSERT_EQ(1u, ok.args.size());
    EXPECT_EQ("a.txt", ok.args[0]);

    const tool_invocation missing = normalize_tool_invocation(catalog, "read", std::vector<std::string>());
    EXPECT_FALSE(missing.ok);
    EXPECT_NE(std::string::npos, missing.error.find("usage: tool read"));
    EXPECT_NE(std::string::npos, missing.error.find("[max-bytes]"));

    const tool_invocation unknown = normalize_tool_invocation(catalog, "unknown", std::vector<std::string>());
    EXPECT_FALSE(unknown.ok);
    EXPECT_NE(std::string::npos, unknown.error.find("unknown tool"));
}

TEST(rasn_provider_router, resolves_known_and_generic_provider_profiles)
{
    EXPECT_EQ("simulator", normalize_model_provider_name("random"));
    EXPECT_EQ("copilot", normalize_model_provider_name("github-copilot"));
    EXPECT_EQ("llama.cpp", normalize_model_provider_name("llamacpp"));
    EXPECT_EQ("llama_cpp_endpoint", model_provider_config_key("llama.cpp", "endpoint"));

    const model_provider_profile simulator = resolve_model_provider_profile("mock", "unit-model");
    EXPECT_EQ("simulator", simulator.name);
    EXPECT_TRUE(simulator.in_process);
    EXPECT_EQ("unit-model", simulator.model);

    const model_provider_profile copilot = resolve_model_provider_profile("github-copilot", "gpt-review");
    EXPECT_EQ("copilot", copilot.name);
    EXPECT_FALSE(copilot.local);
    EXPECT_EQ("gpt-review", copilot.model);
    EXPECT_NE(std::string::npos, copilot.token_env.find("GH_TOKEN"));
    ASSERT_FALSE(copilot.headers.empty());

    const model_provider_profile llama = resolve_model_provider_profile("llama-cpp");
    EXPECT_EQ("llama.cpp", llama.name);
    EXPECT_TRUE(llama.local);
    EXPECT_EQ("openai.chat", llama.payload_format);

    const model_provider_profile custom = resolve_model_provider_profile("custom-provider", "custom-model");
    EXPECT_EQ("custom-provider", custom.name);
    EXPECT_FALSE(custom.local);
    EXPECT_EQ("custom-model", custom.model);
    EXPECT_EQ("openai.chat", custom.payload_format);
}

TEST(rasn_cli_support, zero_context_budget_does_not_report_truncation)
{
    const std::string path = temp_file_path("rasn-cli-support-zero-context.txt");
    write_text_file(path, "non-empty context");

    std::string content;
    std::string error;
    bool truncated = true;
    EXPECT_TRUE(cli_support_detail::read_context_prefix(path, 0, &content, &truncated, &error));
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_TRUE(content.empty());
    EXPECT_FALSE(truncated);

    std::remove(path.c_str());
}

TEST(rasn_cli_support, help_items_render_slash_command_usage)
{
    const std::string interactive = cli_help_item(true, "ask <prompt>", "send a coding prompt");
    EXPECT_NE(std::string::npos, interactive.find("  /ask <prompt>"));

    const std::string option = cli_help_item(true, "--prompt <prompt>", "run one prompt");
    EXPECT_NE(std::string::npos, option.find("  --prompt <prompt>"));
    EXPECT_EQ(std::string::npos, option.find("  /--prompt <prompt>"));

    const std::string direct = cli_help_item(false, "ask <prompt>", "send a coding prompt");
    EXPECT_NE(std::string::npos, direct.find("  /ask <prompt>"));
}

TEST(rasn_cli_support, workspace_source_context_includes_index_and_excerpts)
{
    const std::string root = temp_file_path("rasn-cli-workspace-context");
    ::dsn::utils::filesystem::remove_path(root);
    ASSERT_TRUE(::dsn::utils::filesystem::create_directory(root));
    const std::string src_dir = ::dsn::utils::filesystem::path_combine(root, "src");
    ASSERT_TRUE(::dsn::utils::filesystem::create_directory(src_dir));

    write_text_file(::dsn::utils::filesystem::path_combine(root, "README.md"), "# demo workspace\n");
    write_text_file(::dsn::utils::filesystem::path_combine(src_dir, "main.c"), "int main() { return 0; }\n");
    write_text_file(::dsn::utils::filesystem::path_combine(src_dir, "token_parser.cpp"), "int parse_token() { return 1; }\n");
    write_text_file(::dsn::utils::filesystem::path_combine(src_dir, "private_impl.h"), "int private_impl();\n");
    write_text_file(::dsn::utils::filesystem::path_combine(root, ".env.local"), "TOKEN=secret\n");
    write_text_file(::dsn::utils::filesystem::path_combine(root, "config.json"), "{\"token\":\"secret\"}\n");
    write_text_file(::dsn::utils::filesystem::path_combine(root, "secrets.json"), "{\"token\":\"secret\"}\n");
    const std::string secrets_dir = ::dsn::utils::filesystem::path_combine(root, "secrets");
    ASSERT_TRUE(::dsn::utils::filesystem::create_directory(secrets_dir));
    write_text_file(::dsn::utils::filesystem::path_combine(secrets_dir, "config.yml"), "password: secret\n");

    cli_workspace_context_options options;
    options.max_files = 10;
    options.max_sampled_files = 6;
    options.max_file_bytes = 128;
    options.max_total_bytes = 512;

    std::string context;
    std::string error;
    bool truncated = true;
    EXPECT_TRUE(build_workspace_source_context(root, options, &context, &truncated, &error));
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_FALSE(truncated);
    EXPECT_NE(std::string::npos, context.find("workspace source snapshot"));
    EXPECT_NE(std::string::npos, context.find("README.md"));
    EXPECT_NE(std::string::npos, context.find("src"));
    EXPECT_NE(std::string::npos, context.find("main.c"));
    EXPECT_NE(std::string::npos, context.find("int main()"));
    EXPECT_NE(std::string::npos, context.find("token_parser.cpp"));
    EXPECT_NE(std::string::npos, context.find("private_impl.h"));
    EXPECT_NE(std::string::npos, context.find("parse_token"));
    EXPECT_EQ(std::string::npos, context.find(".env.local"));
    EXPECT_EQ(std::string::npos, context.find("config.json"));
    EXPECT_EQ(std::string::npos, context.find("secrets.json"));
    EXPECT_EQ(std::string::npos, context.find("TOKEN=secret"));
    EXPECT_EQ(std::string::npos, context.find("password: secret"));

    workspace_index_result index;
    EXPECT_TRUE(build_workspace_index(root, options, &index, &error));
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_EQ(4u, index.matched_files);
    ASSERT_EQ(4u, index.files.size());
    EXPECT_NE(std::string::npos, index.files[0].relative_path.find("README.md"));
    EXPECT_EQ(0, index.files[0].priority);
    EXPECT_GT(index.entries_seen, 0u);
    for (const workspace_file_entry &file : index.files)
    {
        EXPECT_EQ(std::string::npos, file.relative_path.find(".env"));
        EXPECT_EQ(std::string::npos, file.relative_path.find("config.json"));
        EXPECT_EQ(std::string::npos, file.relative_path.find("secrets"));
    }

    ::dsn::utils::filesystem::remove_path(root);
}

TEST(rasn_session_store, persists_loads_and_formats_resume_context)
{
    const std::string root = temp_file_path("rasn-session-store-unit");
    ::dsn::utils::filesystem::remove_path(root);

    rasn_session_store_options options;
    options.directory = ::dsn::utils::filesystem::path_combine(root, "sessions");
    rasn_session_store store(options);

    std::string error;
    rasn_session_summary first;
    ASSERT_TRUE(store.begin_session("codepilot", "workspace-a", "trace-a.jsonl", "unit-session-a", &first, &error))
        << error;
    EXPECT_TRUE(store.append_event(first.session_id, "prompt", "ask", "explain rASN", &error)) << error;
    EXPECT_TRUE(store.append_event(first.session_id, "response", "ok", "rASN summary", &error)) << error;
    const std::string control_value = std::string("ansi ") + static_cast<char>(0x1b) + "[31m" +
                                      static_cast<char>(0x01) + "\b\f done";
    EXPECT_TRUE(store.append_event(first.session_id, "tool", "control", control_value, &error)) << error;

    rasn_session_summary second;
    ASSERT_TRUE(store.begin_session("codepilot", "workspace-b", "trace-b.jsonl", "unit-session-b", &second, &error))
        << error;
    EXPECT_TRUE(store.append_event(second.session_id, "prompt", "ask", "continue work", &error)) << error;

    rasn_session_summary loaded;
    std::vector<rasn_session_event> events;
    ASSERT_TRUE(store.load_session(first.session_id, &loaded, &events, &error)) << error;
    EXPECT_EQ("unit-session-a", loaded.session_id);
    EXPECT_EQ("codepilot", loaded.app_name);
    EXPECT_EQ("workspace-a", loaded.workspace_root);
    EXPECT_EQ("trace-a.jsonl", loaded.trace_file);
    EXPECT_EQ("explain rASN", loaded.last_prompt);
    ASSERT_EQ(4u, events.size());
    EXPECT_EQ("session", events[0].kind);
    EXPECT_EQ("prompt", events[1].kind);
    EXPECT_EQ(control_value, events[3].value);

    rasn_session_summary latest;
    ASSERT_TRUE(store.latest_session(&latest, &error)) << error;
    EXPECT_EQ("unit-session-b", latest.session_id);

    const std::string context = format_session_resume_context(loaded, events, 3);
    EXPECT_NE(std::string::npos, context.find("resumed rASN session: unit-session-a"));
    EXPECT_NE(std::string::npos, context.find("last prompt: explain rASN"));
    EXPECT_NE(std::string::npos, context.find("prompt.ask"));
    EXPECT_EQ(std::string::npos, context.find("session.begin"));

    ::dsn::utils::filesystem::remove_path(root);
}

TEST(rasn_workspace_change, plans_and_applies_write_and_replace)
{
    const std::string root = temp_file_path("rasn-workspace-change-unit");
    ::dsn::utils::filesystem::remove_path(root);
    ASSERT_TRUE(::dsn::utils::filesystem::create_directory(root));

    workspace_change_request write_request;
    write_request.kind = workspace_change_kind::write_file;
    write_request.workspace_root = root;
    write_request.path = "notes.txt";
    write_request.content = "alpha beta\n";
    workspace_change_plan write_plan = plan_workspace_change(write_request);
    ASSERT_TRUE(write_plan.ok) << write_plan.error;
    EXPECT_TRUE(write_plan.creates_file);
    EXPECT_EQ(workspace_change_kind::write_file, write_plan.kind);
    EXPECT_NE(std::string::npos, write_plan.path.find("notes.txt"));
    EXPECT_NE(std::string::npos, describe_workspace_change_plan(write_plan).find("create"));

    std::string error;
    ASSERT_TRUE(apply_workspace_change_plan(write_plan, &error)) << error;
    EXPECT_EQ("alpha beta\n", read_text_file(write_plan.path));

    workspace_change_request replace_request;
    replace_request.kind = workspace_change_kind::replace_text;
    replace_request.path = write_plan.path;
    replace_request.old_text = "beta";
    replace_request.new_text = "gamma";
    workspace_change_plan replace_plan = plan_workspace_change(replace_request);
    ASSERT_TRUE(replace_plan.ok) << replace_plan.error;
    EXPECT_EQ(workspace_change_kind::replace_text, replace_plan.kind);
    EXPECT_EQ(write_plan.path, replace_plan.path);
    EXPECT_NE(std::string::npos, replace_plan.summary.find("replace first occurrence"));
    ASSERT_TRUE(apply_workspace_change_plan(replace_plan, &error)) << error;
    EXPECT_EQ("alpha gamma\n", read_text_file(write_plan.path));

    replace_request.old_text.clear();
    const workspace_change_plan invalid = plan_workspace_change(replace_request);
    EXPECT_FALSE(invalid.ok);
    EXPECT_NE(std::string::npos, invalid.error.find("old text cannot be empty"));

    ::dsn::utils::filesystem::remove_path(root);
}

TEST(rasn_workflow, parses_metadata_and_rejects_cycles)
{
    workflow_graph graph;
    std::string error;
    ASSERT_TRUE(graph.load_from_text(
        "task inspect tool \"list .\" capability tool.run policy read_only budget_ms 5000 latency_ms 50 cost_hint 1 reliability 98 state unit/inspect\n"
        "task summarize ask \"summarize\" after inspect capability model.complete policy read_only budget_ms 10000 latency_ms 250 cost_hint 2 reliability 95 state unit/summary\n",
        "<unit>",
        &error)) << error;

    const std::vector<workflow_node> nodes = graph.nodes();
    ASSERT_EQ(2u, nodes.size());
    EXPECT_EQ("inspect", nodes[0].id);
    EXPECT_EQ("tool", nodes[0].action);
    EXPECT_EQ("tool.run", nodes[0].capability);
    ASSERT_EQ(1u, nodes[1].depends_on.size());
    EXPECT_EQ("inspect", nodes[1].depends_on[0]);
    EXPECT_NE(std::string::npos, graph.describe_plan().find("Optimization plan: stages=2"));

    workflow_graph cyclic;
    EXPECT_FALSE(cyclic.load_from_text("task a ask A after b\n"
                                       "task b ask B after a\n",
                                       "<cycle>",
                                       &error));
    EXPECT_NE(std::string::npos, error.find("cycle"));
}

TEST(rasn_workflow, parses_json_workflow_specs)
{
    workflow_graph graph;
    std::string error;
    ASSERT_TRUE(graph.load_from_text(
        "{\n"
        "  \"nodes\": [\n"
        "    {\n"
        "      \"id\": \"inspect\",\n"
        "      \"action\": \"tool\",\n"
        "      \"prompt\": \"list .\",\n"
        "      \"capability\": \"tool.run\",\n"
        "      \"policy_labels\": [\"read_only\"],\n"
        "      \"budget_ms\": 5000,\n"
        "      \"latency_hint_ms\": 50,\n"
        "      \"cost_hint\": 1,\n"
        "      \"reliability_hint\": 98,\n"
        "      \"state_key\": \"unit/inspect\"\n"
        "    },\n"
        "    {\n"
        "      \"id\": \"summarize\",\n"
        "      \"action\": \"ask\",\n"
        "      \"prompt\": \"summarize\\nresults\",\n"
        "      \"depends_on\": [\"inspect\"],\n"
        "      \"retry_budget\": 2,\n"
        "      \"state_key\": \"unit/summary\"\n"
        "    }\n"
        "  ]\n"
        "}\n",
        "<json-workflow>",
        &error)) << error;

    const std::vector<workflow_node> nodes = graph.nodes();
    ASSERT_EQ(2u, nodes.size());
    EXPECT_EQ("inspect", nodes[0].id);
    EXPECT_EQ("tool.run", nodes[0].capability);
    ASSERT_EQ(1u, nodes[0].policy_labels.size());
    EXPECT_EQ("read_only", nodes[0].policy_labels[0]);
    EXPECT_EQ(5000u, nodes[0].budget_ms);
    EXPECT_EQ(98u, nodes[0].reliability_hint);
    EXPECT_EQ("summarize\nresults", nodes[1].prompt);
    EXPECT_EQ("model.complete", nodes[1].capability);
    EXPECT_EQ(2u, nodes[1].retry_budget);
    ASSERT_EQ(1u, nodes[1].depends_on.size());
    EXPECT_EQ("inspect", nodes[1].depends_on[0]);
    EXPECT_NE(std::string::npos, graph.describe_plan().find("Optimization plan: stages=2"));

    workflow_graph invalid;
    EXPECT_FALSE(invalid.load_from_text("{\"nodes\":[{\"id\":\"bad\",\"action\":\"ask\",\"prompt\":\"x\",\"reliability_hint\":101}]}",
                                        "<bad-json>",
                                        &error));
    EXPECT_NE(std::string::npos, error.find("reliability_hint"));
}

TEST(rasn_workflow, cancellation_marks_remaining_nodes)
{
    workflow_graph graph;
    std::string error;
    ASSERT_TRUE(graph.load_from_text("task first ask \"first prompt\"\n"
                                     "task second ask \"second prompt\" after first\n",
                                     "<cancel>",
                                     &error)) << error;

    fixed_llm_provider provider;
    nucleus_runtime runtime;
    int cancel_checks = 0;
    std::vector<workflow_node_status> progress;
    const workflow_result result = graph.execute(provider,
                                                 runtime,
                                                 workflow_graph::workflow_tool_runner(),
                                                 [&progress](const workflow_node_status &status) {
                                                     progress.push_back(status);
                                                 },
                                                 [&cancel_checks]() {
                                                     return cancel_checks++ > 0;
                                                 });

    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.cancelled);
    EXPECT_EQ("cancelled by request", result.error);
    EXPECT_EQ(1, provider.calls);
    ASSERT_FALSE(result.nodes.empty());
    EXPECT_EQ("cancelled", result.nodes.back().status);
    EXPECT_EQ("second", result.nodes.back().node_id);
    EXPECT_EQ(result.nodes.size(), progress.size());
}

TEST(rasn_workflow, propagates_node_budgets_to_model_and_tool_dispatch)
{
    workflow_graph model_graph;
    std::string error;
    ASSERT_TRUE(model_graph.load_from_text("task ask_model ask \"model prompt\" budget_ms 123 retry_budget 2 policy deterministic,read_only\n",
                                           "<model-budget>",
                                           &error)) << error;

    fixed_llm_provider provider;
    nucleus_runtime runtime;
    const workflow_result model_result = model_graph.execute(provider, runtime);
    ASSERT_TRUE(model_result.ok) << model_result.error;
    EXPECT_EQ(123u, provider.last_timeout_ms);
    EXPECT_EQ(2u, provider.last_retry_budget);
    ASSERT_EQ(2u, provider.last_policy_labels.size());
    EXPECT_EQ("deterministic", provider.last_policy_labels[0]);
    EXPECT_EQ("read_only", provider.last_policy_labels[1]);

    workflow_graph tool_graph;
    ASSERT_TRUE(tool_graph.load_from_text("task inspect tool \"list .\" budget_ms 456\n", "<tool-budget>", &error)) << error;

    uint32_t observed_timeout_ms = 0;
    const workflow_result tool_workflow_result =
        tool_graph.execute(provider,
                           runtime,
                           [&observed_timeout_ms](const std::string &name,
                                                  const std::vector<std::string> &args,
                                                  nucleus_runtime &,
                                                  const agent_task &,
                                                  uint32_t timeout_ms) {
                               observed_timeout_ms = timeout_ms;
                               tool_result result;
                               result.ok = true;
                               result.output = name + ":" + (args.empty() ? "" : args[0]);
                               return result;
                           });
    ASSERT_TRUE(tool_workflow_result.ok) << tool_workflow_result.error;
    EXPECT_EQ(456u, observed_timeout_ms);
}

TEST(rasn_workflow, redacts_model_prompts_context_and_outputs)
{
    workflow_graph graph;
    std::string error;
    ASSERT_TRUE(graph.load_from_text("task inspect ask \"summarize api_key=sk-workflow-secret\" state unit/redact\n",
                                     "<workflow-redaction>",
                                     &error)) << error;

    fixed_llm_provider provider;
    nucleus_runtime runtime;
    const workflow_result result = graph.execute(provider, runtime);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(1, provider.calls);
    EXPECT_EQ(std::string::npos, provider.last_user_prompt.find("sk-workflow-secret"));
    EXPECT_NE(std::string::npos, provider.last_user_prompt.find("<redacted-secret>"));
    EXPECT_EQ(std::string::npos, result.text.find("sk-workflow-secret"));
    EXPECT_NE(std::string::npos, result.text.find("<redacted-secret>"));
}

TEST(rasn_workflow, records_workflow_node_schedule_events)
{
    const std::string trace_path = temp_file_path("rasn-workflow-schedule-trace.jsonl");
    std::remove(trace_path.c_str());

    workflow_graph graph;
    std::string error;
    ASSERT_TRUE(graph.load_from_text("task first ask \"first prompt\"\n"
                                     "task second ask \"second prompt\" after first\n",
                                     "<schedule-record>",
                                     &error)) << error;

    fixed_llm_provider provider;
    nucleus_runtime runtime;
    runtime.set_trace_file(trace_path);
    const workflow_result result = graph.execute(provider, runtime);
    ASSERT_TRUE(result.ok) << result.error;

    const std::string trace = read_text_file(trace_path);
    EXPECT_NE(std::string::npos, trace.find("\"kind\":\"workflow.node.start\",\"name\":\"first\""));
    EXPECT_NE(std::string::npos, trace.find("\"kind\":\"workflow.node.start\",\"name\":\"second\""));
    EXPECT_NE(std::string::npos, trace.find("\"kind\":\"workflow.node.finish\",\"name\":\"first\",\"value\":\"completed\""));
    std::remove(trace_path.c_str());
}

TEST(rasn_workflow, replay_detects_workflow_node_schedule_mismatch)
{
    const std::string trace_path = temp_file_path("rasn-workflow-schedule-mismatch.jsonl");
    write_text_file(trace_path,
                    "{\"schema_version\":1,\"sequence\":1,\"trace_id\":\"trace\",\"task_id\":\"second\","
                    "\"kind\":\"workflow.node.start\",\"name\":\"second\",\"value\":\"ask\","
                    "\"timestamp\":\"now\"}\n");

    nucleus_runtime runtime;
    std::string error;
    ASSERT_TRUE(runtime.enable_replay(trace_path, &error)) << error;

    workflow_graph graph;
    ASSERT_TRUE(graph.load_from_text("task first ask \"first prompt\"\n"
                                     "task second ask \"second prompt\" after first\n",
                                     "<schedule-replay>",
                                     &error)) << error;

    fixed_llm_provider provider;
    const workflow_result result = graph.execute(provider, runtime);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(std::string::npos, result.error.find("workflow scheduler replay mismatch"));
    EXPECT_EQ(0, provider.calls);

    const std::vector<runtime_event> events = runtime.events();
    bool found_miss = false;
    for (const runtime_event &event : events)
    {
        if (event.kind == "replay.miss" && event.failure_code == "workflow_schedule_mismatch")
        {
            found_miss = true;
        }
    }
    EXPECT_TRUE(found_miss);
    std::remove(trace_path.c_str());
}

TEST(rasn_workflow, resumes_completed_nodes_and_reuses_dependency_output)
{
    workflow_graph graph;
    std::string error;
    ASSERT_TRUE(graph.load_from_text("task first ask \"first prompt\"\n"
                                     "task second ask \"second prompt\" after first\n",
                                     "<resume>",
                                     &error)) << error;

    workflow_node_status recovered;
    recovered.node_id = "first";
    recovered.action = "ask";
    recovered.status = "completed";
    recovered.output = "recovered-first-output";
    workflow_graph::workflow_resume_state resume_state;
    resume_state[recovered.node_id] = recovered;

    fixed_llm_provider provider;
    nucleus_runtime runtime;
    std::vector<workflow_node_status> progress;
    const workflow_result result = graph.execute(provider,
                                                 runtime,
                                                 workflow_graph::workflow_tool_runner(),
                                                 [&progress](const workflow_node_status &status) {
                                                     progress.push_back(status);
                                                 },
                                                 workflow_graph::workflow_cancel_checker(),
                                                 resume_state);

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(1, provider.calls);
    ASSERT_EQ(3u, progress.size());
    EXPECT_EQ("resumed", progress[0].status);
    EXPECT_EQ("first", progress[0].node_id);
    EXPECT_EQ("running", progress[1].status);
    EXPECT_EQ("second", progress[1].node_id);
    EXPECT_EQ("completed", progress[2].status);
    EXPECT_EQ("second", progress[2].node_id);
    EXPECT_NE(std::string::npos, provider.last_context.find("recovered-first-output"));
    EXPECT_NE(std::string::npos, result.text.find("## first"));
    EXPECT_NE(std::string::npos, result.text.find("recovered-first-output"));
}

TEST(rasn_state, checkpoints_and_recovers_records)
{
    const std::string checkpoint_path = temp_file_path("rasn-state-recovery-unit.chkpt");
    std::remove(checkpoint_path.c_str());
    std::remove((checkpoint_path + ".tmp").c_str());
    std::remove((checkpoint_path + ".bak").c_str());

    state_store writer;
    state_record record;
    record.key = "unit/checkpoint";
    record.kind = "observation";
    record.scope = "unit";
    record.value = "recoverable";
    const state_response put = writer.put(record);
    ASSERT_TRUE(put.ok) << put.error;
    ASSERT_EQ(1u, put.last_sequence);

    state_checkpoint_request checkpoint_request;
    checkpoint_request.path = checkpoint_path;
    const state_response checkpoint = writer.checkpoint(checkpoint_request);
    ASSERT_TRUE(checkpoint.ok) << checkpoint.error;
    EXPECT_EQ(1u, checkpoint.records.size());

    state_store reader;
    const state_response recovered = reader.recover(checkpoint_request);
    ASSERT_TRUE(recovered.ok) << recovered.error;
    ASSERT_EQ(1u, recovered.records.size());
    EXPECT_EQ("unit/checkpoint", recovered.records[0].key);
    EXPECT_EQ("recoverable", recovered.records[0].value);
    EXPECT_EQ(1u, recovered.last_sequence);

    std::remove(checkpoint_path.c_str());
    std::remove((checkpoint_path + ".tmp").c_str());
    std::remove((checkpoint_path + ".bak").c_str());
}

TEST(rasn_state, replicated_checkpoint_copy_does_not_replace_live_state)
{
    const std::string source_path =
        temp_file_path("rasn-state-replicated-source-unit.chkpt");
    const std::string copied_path =
        temp_file_path("rasn-state-replicated-copy-unit.chkpt");
    for (const std::string &path : {source_path, copied_path})
    {
        std::remove(path.c_str());
        std::remove((path + ".tmp").c_str());
        std::remove((path + ".bak").c_str());
    }

    state_store source(false);
    state_record durable;
    durable.key = "unit/replicated";
    durable.kind = "observation";
    durable.scope = "unit";
    durable.value = "quorum-value";
    ASSERT_TRUE(source.put(durable).ok);

    state_checkpoint_request checkpoint;
    checkpoint.path = source_path;
    ASSERT_TRUE(source.checkpoint(checkpoint).ok);

    state_store live(false);
    state_record current;
    current.key = "unit/live";
    current.kind = "observation";
    current.scope = "unit";
    current.value = "primary-value";
    ASSERT_TRUE(live.put(current).ok);

    const state_response copied = live.copy_checkpoint(checkpoint, copied_path);
    ASSERT_TRUE(copied.ok) << copied.error;
    state_key_request live_key;
    live_key.key = current.key;
    EXPECT_TRUE(live.get(live_key).ok);

    state_checkpoint_request learned;
    learned.path = copied_path;
    const state_response replaced = live.replace_from_checkpoint(learned);
    ASSERT_TRUE(replaced.ok) << replaced.error;
    EXPECT_FALSE(live.get(live_key).ok);

    state_key_request durable_key;
    durable_key.key = durable.key;
    const state_response recovered = live.get(durable_key);
    ASSERT_TRUE(recovered.ok) << recovered.error;
    EXPECT_EQ("quorum-value", recovered.record.value);

    for (const std::string &path : {source_path, copied_path})
    {
        std::remove(path.c_str());
        std::remove((path + ".tmp").c_str());
        std::remove((path + ".bak").c_str());
    }
}

TEST(rasn_state, replicated_stores_assign_deterministic_sequences)
{
    state_store first(false);
    state_store second(false);

    state_record record;
    record.key = "unit/deterministic";
    record.kind = "observation";
    record.scope = "unit";
    record.value = "created";
    const state_response first_created = first.put(record);
    const state_response second_created = second.put(record);
    ASSERT_TRUE(first_created.ok) << first_created.error;
    ASSERT_TRUE(second_created.ok) << second_created.error;
    EXPECT_EQ(first_created.record.sequence, second_created.record.sequence);

    state_put_request update;
    update.record = record;
    update.record.value = "updated";
    update.check_sequence = true;
    update.expected_sequence = first_created.record.sequence;
    const state_response first_updated = first.put(update);
    const state_response second_updated = second.put(update);
    ASSERT_TRUE(first_updated.ok) << first_updated.error;
    ASSERT_TRUE(second_updated.ok) << second_updated.error;
    EXPECT_EQ(first_updated.record.sequence, second_updated.record.sequence);
    EXPECT_EQ(first_updated.last_sequence, second_updated.last_sequence);
}

TEST(rasn_wire_limits, bounded_reserve_caps_untrusted_counts)
{
    // Small, legitimate counts pass through unchanged.
    EXPECT_EQ(0u, rasn_bounded_reserve_count<std::string>(0));
    EXPECT_EQ(7u, rasn_bounded_reserve_count<std::string>(7));

    // A hostile/corrupt length prefix is capped to a fixed 64 KiB byte budget
    // instead of being reserved verbatim, so one crafted or truncated message
    // cannot force a multi-gigabyte speculative allocation before any element is
    // even read.
    const uint32_t hostile = 0xFFFFFFFFu;
    const uint32_t expected_string_cap =
        static_cast<uint32_t>((64u * 1024u) / sizeof(std::string));
    EXPECT_EQ(expected_string_cap, rasn_bounded_reserve_count<std::string>(hostile));
    EXPECT_LT(rasn_bounded_reserve_count<std::string>(hostile), hostile);

    // The budget is measured in bytes, so an element larger than the whole budget
    // still reserves at least one slot (never zero, which would defeat reserve()).
    struct huge_element
    {
        char bytes[128 * 1024];
    };
    EXPECT_EQ(1u, rasn_bounded_reserve_count<huge_element>(hostile));
}

TEST(rasn_state, recovers_past_a_torn_trailing_journal_record)
{
    const std::string journal_path = configured_state_journal_path();
    const std::string checkpoint_path = configured_state_checkpoint_path();
    ASSERT_FALSE(journal_path.empty());
    ASSERT_FALSE(checkpoint_path.empty());

    std::remove(checkpoint_path.c_str());
    std::remove((checkpoint_path + ".tmp").c_str());
    std::remove((checkpoint_path + ".bak").c_str());
    std::remove(journal_path.c_str());

    state_store writer;
    state_record first;
    first.key = "unit/torn-a";
    first.kind = "observation";
    first.scope = "unit";
    first.value = "first-value";
    ASSERT_TRUE(writer.put(first).ok);

    state_checkpoint_request checkpoint;
    checkpoint.path = checkpoint_path;
    ASSERT_TRUE(writer.checkpoint(checkpoint).ok);
    ASSERT_FALSE(::dsn::utils::filesystem::file_exists(journal_path));

    state_record second;
    second.key = "unit/torn-b";
    second.kind = "observation";
    second.scope = "unit";
    second.value = "second-value";
    ASSERT_TRUE(writer.put(second).ok);

    state_record third;
    third.key = "unit/torn-c";
    third.kind = "observation";
    third.scope = "unit";
    third.value = "third-value";
    ASSERT_TRUE(writer.put(third).ok);

    std::string journal = read_text_file(journal_path);
    ASSERT_GT(journal.size(), 3u);
    ASSERT_EQ('\n', journal[journal.size() - 1]);
    journal.resize(journal.size() - 3);
    write_text_file(journal_path, journal);

    state_store reader;
    state_checkpoint_request request;
    request.path = checkpoint_path;
    const state_response recovered = reader.recover(request);
    ASSERT_TRUE(recovered.ok) << recovered.error;

    bool saw_first = false;
    bool saw_second = false;
    bool saw_torn = false;
    for (const state_record &record : recovered.records)
    {
        if (record.key == "unit/torn-a")
        {
            saw_first = true;
        }
        else if (record.key == "unit/torn-b")
        {
            saw_second = true;
        }
        else if (record.key == "unit/torn-c")
        {
            saw_torn = true;
        }
    }
    EXPECT_TRUE(saw_first);
    EXPECT_TRUE(saw_second);
    EXPECT_FALSE(saw_torn);

    state_record fourth;
    fourth.key = "unit/torn-d";
    fourth.kind = "observation";
    fourth.scope = "unit";
    fourth.value = "fourth-value";
    const state_response appended = reader.put(fourth);
    ASSERT_TRUE(appended.ok) << appended.error;

    state_store final_reader;
    const state_response recovered_after_append = final_reader.recover(request);
    ASSERT_TRUE(recovered_after_append.ok) << recovered_after_append.error;

    saw_first = false;
    saw_second = false;
    saw_torn = false;
    bool saw_fourth = false;
    for (const state_record &record : recovered_after_append.records)
    {
        if (record.key == "unit/torn-a")
        {
            saw_first = true;
        }
        else if (record.key == "unit/torn-b")
        {
            saw_second = true;
        }
        else if (record.key == "unit/torn-c")
        {
            saw_torn = true;
        }
        else if (record.key == "unit/torn-d")
        {
            saw_fourth = true;
        }
    }
    EXPECT_TRUE(saw_first);
    EXPECT_TRUE(saw_second);
    EXPECT_FALSE(saw_torn);
    EXPECT_TRUE(saw_fourth);

    std::remove(checkpoint_path.c_str());
    std::remove((checkpoint_path + ".tmp").c_str());
    std::remove((checkpoint_path + ".bak").c_str());
    std::remove(journal_path.c_str());
}

TEST(rasn_state, rejects_corrupt_newline_terminated_journal_record)
{
    const std::string journal_path = configured_state_journal_path();
    const std::string checkpoint_path = configured_state_checkpoint_path();
    ASSERT_FALSE(journal_path.empty());
    ASSERT_FALSE(checkpoint_path.empty());

    std::remove(checkpoint_path.c_str());
    std::remove((checkpoint_path + ".tmp").c_str());
    std::remove((checkpoint_path + ".bak").c_str());
    std::remove(journal_path.c_str());

    state_store writer;
    state_record record;
    record.key = "unit/corrupt-a";
    record.kind = "observation";
    record.scope = "unit";
    record.value = "value";
    ASSERT_TRUE(writer.put(record).ok);

    {
        std::ofstream corrupt(journal_path.c_str(), std::ios::binary | std::ios::app);
        ASSERT_TRUE(corrupt.good());
        corrupt << "not-a-state-record\n";
    }

    state_store reader;
    state_checkpoint_request request;
    request.path = checkpoint_path;
    const state_response recovered = reader.recover(request);
    EXPECT_FALSE(recovered.ok);
    EXPECT_NE(std::string::npos, recovered.error.find("invalid checkpoint record"));

    std::remove(checkpoint_path.c_str());
    std::remove((checkpoint_path + ".tmp").c_str());
    std::remove((checkpoint_path + ".bak").c_str());
    std::remove(journal_path.c_str());
}

TEST(rasn_state, recovers_past_an_interrupted_first_journal_append)
{
    const std::string journal_path = configured_state_journal_path();
    const std::string checkpoint_path = configured_state_checkpoint_path();
    ASSERT_FALSE(journal_path.empty());
    ASSERT_FALSE(checkpoint_path.empty());

    std::remove(checkpoint_path.c_str());
    std::remove((checkpoint_path + ".tmp").c_str());
    std::remove((checkpoint_path + ".bak").c_str());
    std::remove(journal_path.c_str());

    state_checkpoint_request request;
    request.path = checkpoint_path;

    write_text_file(journal_path, "");
    state_store empty_reader;
    const state_response recovered_empty = empty_reader.recover(request);
    ASSERT_TRUE(recovered_empty.ok) << recovered_empty.error;
    EXPECT_TRUE(recovered_empty.records.empty());

    write_text_file(journal_path, "rasn-state-journ");
    state_store torn_header_reader;
    const state_response recovered_torn_header = torn_header_reader.recover(request);
    ASSERT_TRUE(recovered_torn_header.ok) << recovered_torn_header.error;
    EXPECT_TRUE(recovered_torn_header.records.empty());

    std::remove(checkpoint_path.c_str());
    std::remove((checkpoint_path + ".tmp").c_str());
    std::remove((checkpoint_path + ".bak").c_str());
    std::remove(journal_path.c_str());
}

TEST(rasn_state, conditional_put_guards_create_and_expected_sequence)
{
    state_store store;
    state_record record;
    record.key = "unit/cas-" + make_trace_id();
    record.kind = "observation";
    record.scope = "unit";
    record.value = "created";

    state_put_request create;
    create.record = record;
    create.create_only = true;
    const state_response created = store.put(create);
    ASSERT_TRUE(created.ok) << created.error;
    EXPECT_EQ(1u, created.record.sequence);

    const state_response duplicate = store.put(create);
    EXPECT_FALSE(duplicate.ok);
    EXPECT_NE(std::string::npos, duplicate.error.find("already exists"));
    EXPECT_EQ(created.record.sequence, duplicate.record.sequence);

    state_put_request stale_update;
    stale_update.record = record;
    stale_update.record.value = "stale";
    stale_update.check_sequence = true;
    stale_update.expected_sequence = created.record.sequence + 1;
    const state_response stale = store.put(stale_update);
    EXPECT_FALSE(stale.ok);
    EXPECT_NE(std::string::npos, stale.error.find("expected sequence"));

    state_put_request update;
    update.record = record;
    update.record.value = "updated";
    update.check_sequence = true;
    update.expected_sequence = created.record.sequence;
    const state_response updated = store.put(update);
    ASSERT_TRUE(updated.ok) << updated.error;
    EXPECT_GT(updated.record.sequence, created.record.sequence);

    state_key_request get;
    get.key = record.key;
    const state_response read = store.get(get);
    ASSERT_TRUE(read.ok) << read.error;
    EXPECT_EQ("updated", read.record.value);
    EXPECT_EQ(updated.record.sequence, read.record.sequence);

    state_put_request replay = update;
    replay.record = updated.record;
    const state_response replayed = store.put(replay);
    ASSERT_TRUE(replayed.ok) << replayed.error;
    EXPECT_EQ(updated.record.sequence, replayed.record.sequence);
    EXPECT_EQ(updated.last_sequence, replayed.last_sequence);
}

TEST(rasn_state, custom_checkpoint_export_preserves_recovery_journal)
{
    const std::string journal_path = configured_state_journal_path();
    const std::string recovery_path = configured_state_checkpoint_path();
    const std::string export_path =
        temp_file_path("rasn-state-custom-export-unit.chkpt");
    for (const std::string &path : {journal_path, recovery_path, export_path})
    {
        std::remove(path.c_str());
        std::remove((path + ".tmp").c_str());
        std::remove((path + ".bak").c_str());
    }

    state_store writer;
    state_record record;
    record.key = "unit/custom-export";
    record.kind = "observation";
    record.scope = "unit";
    record.value = "journal-must-survive";
    ASSERT_TRUE(writer.put(record).ok);

    state_checkpoint_request export_request;
    export_request.path = export_path;
    const state_checkpoint_result exported =
        writer.checkpoint_detailed(export_request);
    ASSERT_TRUE(exported.response.ok) << exported.response.error;
    EXPECT_TRUE(exported.details_available);
    EXPECT_EQ(export_path, exported.checkpoint_path);
    EXPECT_FALSE(exported.journal_compacted);
    EXPECT_TRUE(::dsn::utils::filesystem::file_exists(journal_path));

    state_store recovered;
    const state_response response =
        recovered.recover(state_checkpoint_request());
    ASSERT_TRUE(response.ok) << response.error;
    state_key_request key;
    key.key = record.key;
    const state_response read = recovered.get(key);
    ASSERT_TRUE(read.ok) << read.error;
    EXPECT_EQ(record.value, read.record.value);

    for (const std::string &path : {journal_path, recovery_path, export_path})
    {
        std::remove(path.c_str());
        std::remove((path + ".tmp").c_str());
        std::remove((path + ".bak").c_str());
    }
}

TEST(rasn_state, delete_prefix_replays_tombstone_and_preserves_newer_records)
{
    const std::string journal_path = configured_state_journal_path();
    const std::string checkpoint_path = configured_state_checkpoint_path();
    for (const std::string &path : {journal_path, checkpoint_path})
    {
        std::remove(path.c_str());
        std::remove((path + ".tmp").c_str());
        std::remove((path + ".bak").c_str());
    }

    state_store writer;
    state_record old_record;
    old_record.key = "unit/prune/old";
    old_record.kind = "observation";
    old_record.scope = "unit";
    old_record.value = "old";
    const state_response old_put = writer.put(old_record);
    ASSERT_TRUE(old_put.ok) << old_put.error;

    state_record newer_record = old_record;
    newer_record.key = "unit/prune/newer";
    newer_record.value = "newer";
    const state_response newer_put = writer.put(newer_record);
    ASSERT_TRUE(newer_put.ok) << newer_put.error;

    state_delete_prefix_request deletion;
    deletion.key_prefix = "unit/prune";
    deletion.max_sequence = old_put.record.sequence;
    const state_response deleted = writer.delete_prefix(deletion);
    ASSERT_TRUE(deleted.ok) << deleted.error;
    ASSERT_EQ(1u, deleted.records.size());
    EXPECT_EQ(old_record.key, deleted.records[0].key);
    EXPECT_GT(deleted.last_sequence, newer_put.last_sequence);

    state_sequence_barrier_request barrier;
    barrier.minimum_sequence = deleted.last_sequence + 10;
    const state_response advanced = writer.advance_sequence(barrier);
    ASSERT_TRUE(advanced.ok) << advanced.error;
    EXPECT_EQ(barrier.minimum_sequence, advanced.last_sequence);

    state_store recovered;
    const state_response replayed =
        recovered.recover(state_checkpoint_request());
    ASSERT_TRUE(replayed.ok) << replayed.error;
    EXPECT_EQ(barrier.minimum_sequence, replayed.last_sequence);
    state_key_request old_key;
    old_key.key = old_record.key;
    EXPECT_FALSE(recovered.get(old_key).ok);
    state_key_request newer_key;
    newer_key.key = newer_record.key;
    EXPECT_TRUE(recovered.get(newer_key).ok);

    for (const std::string &path : {journal_path, checkpoint_path})
    {
        std::remove(path.c_str());
        std::remove((path + ".tmp").c_str());
        std::remove((path + ".bak").c_str());
    }
}

TEST(rasn_state, quarantine_sidecar_blocks_state_lifecycle_operations)
{
    const std::string journal_path = configured_state_journal_path();
    const std::string marker_path = journal_path + ".quarantine";
    const std::string quarantined_path = journal_path + ".quarantined";
    const std::string checkpoint_path =
        temp_file_path("rasn-state-quarantine-unit.chkpt");
    for (const std::string &path :
         {marker_path, quarantined_path, checkpoint_path})
    {
        std::remove(path.c_str());
        std::remove((path + ".tmp").c_str());
        std::remove((path + ".bak").c_str());
    }
    write_text_file(marker_path, "unprovable mirror rollback\n");

    state_store store;
    state_record record;
    record.key = "unit/quarantined";
    record.kind = "observation";
    record.scope = "unit";
    record.value = "must-not-be-visible";
    EXPECT_FALSE(store.put(record).ok);

    state_key_request key;
    key.key = record.key;
    EXPECT_FALSE(store.get(key).ok);
    EXPECT_FALSE(store.query(state_query_request()).ok);

    state_delete_prefix_request deletion;
    deletion.key_prefix = "unit/quarantined";
    deletion.max_sequence = 1;
    EXPECT_FALSE(store.delete_prefix(deletion).ok);

    state_sequence_barrier_request barrier;
    barrier.minimum_sequence = 1;
    EXPECT_FALSE(store.advance_sequence(barrier).ok);

    state_checkpoint_request checkpoint;
    checkpoint.path = checkpoint_path;
    EXPECT_FALSE(store.checkpoint(checkpoint).ok);
    EXPECT_FALSE(store.copy_checkpoint(checkpoint, "").ok);
    EXPECT_FALSE(store.recover(checkpoint).ok);
    EXPECT_TRUE(store.has_recovery_state(state_checkpoint_request()));

    std::remove(marker_path.c_str());
    std::remove(quarantined_path.c_str());
    std::remove(checkpoint_path.c_str());
    std::remove((checkpoint_path + ".tmp").c_str());
    std::remove((checkpoint_path + ".bak").c_str());
}

TEST(rasn_state, checkpoint_migration_is_dry_run_resumable_and_conflict_safe)
{
    const std::string checkpoint_path =
        temp_file_path("rasn-state-migration-unit.chkpt");
    std::remove(checkpoint_path.c_str());
    std::remove((checkpoint_path + ".tmp").c_str());
    std::remove((checkpoint_path + ".bak").c_str());

    const std::string prefix = "unit/migration-" + make_trace_id();
    state_store source(false);
    state_record first;
    first.key = prefix + "/first";
    first.kind = "observation";
    first.scope = "unit";
    first.value = "one";
    first.sequence = uint64_t{1} << 60;
    ASSERT_TRUE(source.put(first).ok);
    state_record second = first;
    second.key = prefix + "/second";
    second.value = "two";
    second.sequence = 0;
    const state_response source_second_put = source.put(second);
    ASSERT_TRUE(source_second_put.ok) << source_second_put.error;
    state_record third = second;
    third.key = prefix + "/third";
    third.value = "three";
    ASSERT_TRUE(source.put(third).ok);

    state_delete_prefix_request source_deletion;
    source_deletion.key_prefix = first.key;
    source_deletion.max_sequence = first.sequence;
    ASSERT_TRUE(source.delete_prefix(source_deletion).ok);

    state_checkpoint_request checkpoint;
    checkpoint.path = checkpoint_path;
    ASSERT_TRUE(source.checkpoint(checkpoint).ok);

    rasn_service_graph services;
    const state_migration_report dry_run =
        migrate_state_checkpoint(services, checkpoint_path, prefix, false);
    ASSERT_TRUE(dry_run.ok) << dry_run.error;
    EXPECT_EQ(2u, dry_run.planned_records);
    EXPECT_EQ(0u, dry_run.migrated_records);
    state_query_request query;
    query.key_prefix = prefix;
    EXPECT_TRUE(services.query_state(query).records.empty());

    const state_migration_report applied =
        migrate_state_checkpoint(services, checkpoint_path, prefix, true);
    ASSERT_TRUE(applied.ok) << applied.error;
    EXPECT_EQ(2u, applied.migrated_records);
    EXPECT_EQ(2u, applied.verified_records);
    EXPECT_GE(applied.target_last_sequence, applied.source_last_sequence);

    const state_migration_report resumed =
        migrate_state_checkpoint(services, checkpoint_path, prefix, true);
    ASSERT_TRUE(resumed.ok) << resumed.error;
    EXPECT_EQ(0u, resumed.planned_records);
    EXPECT_EQ(2u, resumed.unchanged_records);

    state_record target_only = second;
    target_only.key = prefix + "/target-only";
    target_only.value = "stale";
    const state_response target_only_put = services.put_state(target_only);
    ASSERT_TRUE(target_only_put.ok) << target_only_put.error;
    const state_migration_report target_only_conflict =
        migrate_state_checkpoint(services, checkpoint_path, prefix, true);
    EXPECT_FALSE(target_only_conflict.ok);
    ASSERT_EQ(1u, target_only_conflict.conflict_keys.size());
    EXPECT_EQ(target_only.key, target_only_conflict.conflict_keys[0]);
    state_delete_prefix_request remove_target_only;
    remove_target_only.key_prefix = target_only.key;
    remove_target_only.max_sequence = target_only_put.record.sequence;
    ASSERT_TRUE(services.delete_state_prefix(remove_target_only).ok);

    state_record newer_same = source_second_put.record;
    state_query_request all_target;
    const state_response target_before_newer = services.query_state(all_target);
    ASSERT_TRUE(target_before_newer.ok);
    newer_same.sequence = target_before_newer.last_sequence + 1;
    ASSERT_TRUE(services.put_state(newer_same).ok);
    const state_migration_report newer_conflict =
        migrate_state_checkpoint(services, checkpoint_path, prefix, true);
    EXPECT_FALSE(newer_conflict.ok);
    ASSERT_EQ(1u, newer_conflict.conflict_keys.size());
    EXPECT_EQ(second.key, newer_conflict.conflict_keys[0]);

    ASSERT_TRUE(services.put_state(source_second_put.record).ok);
    state_record divergent = source_second_put.record;
    divergent.sequence = source_second_put.record.sequence - 1;
    divergent.value = "target-divergent";
    ASSERT_TRUE(services.put_state(divergent).ok);
    const state_migration_report conflict =
        migrate_state_checkpoint(services, checkpoint_path, prefix, true);
    EXPECT_FALSE(conflict.ok);
    ASSERT_EQ(1u, conflict.conflict_keys.size());
    EXPECT_EQ(second.key, conflict.conflict_keys[0]);

    const std::string empty_checkpoint_path =
        temp_file_path("rasn-state-empty-migration-unit.chkpt");
    std::remove(empty_checkpoint_path.c_str());
    const state_response target_before_empty = services.query_state(all_target);
    ASSERT_TRUE(target_before_empty.ok);
    state_store empty_source(false);
    state_sequence_barrier_request empty_barrier;
    empty_barrier.minimum_sequence = target_before_empty.last_sequence + 10;
    ASSERT_TRUE(empty_source.advance_sequence(empty_barrier).ok);
    state_checkpoint_request empty_checkpoint;
    empty_checkpoint.path = empty_checkpoint_path;
    ASSERT_TRUE(empty_source.checkpoint(empty_checkpoint).ok);
    const std::string empty_prefix = prefix + "-empty";
    const state_migration_report empty_migration =
        migrate_state_checkpoint(
            services, empty_checkpoint_path, empty_prefix, true);
    ASSERT_TRUE(empty_migration.ok) << empty_migration.error;
    EXPECT_EQ(0u, empty_migration.source_records);
    EXPECT_GE(empty_migration.target_last_sequence,
              empty_barrier.minimum_sequence);

    std::remove(checkpoint_path.c_str());
    std::remove((checkpoint_path + ".tmp").c_str());
    std::remove((checkpoint_path + ".bak").c_str());
    std::remove(empty_checkpoint_path.c_str());
    std::remove((empty_checkpoint_path + ".tmp").c_str());
    std::remove((empty_checkpoint_path + ".bak").c_str());
}

TEST(rasn_state, inline_lifecycle_commands_recover_before_checkpoint_and_prune)
{
    const std::string journal_path = configured_state_journal_path();
    const std::string checkpoint_path = configured_state_checkpoint_path();
    const std::string export_path =
        temp_file_path("rasn-state-inline-lifecycle-unit.chkpt");
    for (const std::string &path :
         {journal_path,
          journal_path + ".quarantine",
          journal_path + ".quarantined",
          checkpoint_path,
          export_path})
    {
        std::remove(path.c_str());
        std::remove((path + ".tmp").c_str());
        std::remove((path + ".bak").c_str());
    }

    const state_response initial =
        global_state_store().query(state_query_request());
    ASSERT_TRUE(initial.ok) << initial.error;
    ASSERT_LT(initial.last_sequence,
              (std::numeric_limits<uint64_t>::max)() - 20);

    const std::string prefix = "unit/inline-lifecycle-" + make_trace_id();
    state_record checkpoint_record;
    checkpoint_record.key = prefix + "/checkpoint";
    checkpoint_record.kind = "observation";
    checkpoint_record.scope = "unit";
    checkpoint_record.value = "checkpoint-sensitive-value";
    checkpoint_record.sequence = initial.last_sequence + 10;
    state_store checkpoint_writer;
    ASSERT_TRUE(checkpoint_writer.put(checkpoint_record).ok);

    rasn_service_graph checkpoint_services;
    int checkpoint_exit = -1;
    std::string checkpoint_output;
    {
        scoped_cout_capture capture;
        checkpoint_exit = run_rasn_state_command(
            checkpoint_services, {"checkpoint", export_path});
        checkpoint_output = capture.str();
    }
    EXPECT_EQ(0, checkpoint_exit);
    EXPECT_NE(std::string::npos,
              checkpoint_output.find("checkpointed records="));
    EXPECT_EQ(std::string::npos,
              checkpoint_output.find(checkpoint_record.value));

    state_store exported(false);
    state_checkpoint_request exported_request;
    exported_request.path = export_path;
    const state_response exported_records =
        exported.copy_checkpoint(exported_request, "");
    ASSERT_TRUE(exported_records.ok) << exported_records.error;
    EXPECT_NE(exported_records.records.end(),
              std::find_if(exported_records.records.begin(),
                           exported_records.records.end(),
                           [&](const state_record &record) {
                               return record.key == checkpoint_record.key;
                           }));

    const state_response after_checkpoint =
        global_state_store().query(state_query_request());
    ASSERT_TRUE(after_checkpoint.ok) << after_checkpoint.error;
    ASSERT_LT(after_checkpoint.last_sequence,
              (std::numeric_limits<uint64_t>::max)() - 10);

    state_record prune_record = checkpoint_record;
    prune_record.key = prefix + "/prune";
    prune_record.value = "prune-sensitive-value";
    prune_record.sequence = after_checkpoint.last_sequence + 10;
    state_store prune_writer;
    ASSERT_TRUE(prune_writer.put(prune_record).ok);

    rasn_service_graph prune_services;
    int prune_exit = -1;
    std::string prune_output;
    {
        scoped_cout_capture capture;
        prune_exit = run_rasn_state_command(
            prune_services,
            {"prune",
             "--prefix",
             prune_record.key,
             "--max-sequence",
             std::to_string(prune_record.sequence),
             "--apply"});
        prune_output = capture.str();
    }
    EXPECT_EQ(0, prune_exit);
    EXPECT_NE(std::string::npos, prune_output.find("deleted=1"));
    EXPECT_EQ(std::string::npos, prune_output.find(prune_record.value));

    state_key_request pruned_key;
    pruned_key.key = prune_record.key;
    EXPECT_FALSE(global_state_store().get(pruned_key).ok);

    int query_exit = -1;
    std::string query_output;
    {
        scoped_cout_capture capture;
        query_exit = run_rasn_state_command(
            checkpoint_services, {"query", checkpoint_record.key});
        query_output = capture.str();
    }
    EXPECT_EQ(0, query_exit);
    EXPECT_NE(std::string::npos, query_output.find(checkpoint_record.value));
    EXPECT_NE(std::string::npos, query_output.find("records=1"));
    EXPECT_NE(std::string::npos, query_output.find("last_sequence="));

    const state_response before_cleanup =
        global_state_store().query(state_query_request());
    ASSERT_TRUE(before_cleanup.ok) << before_cleanup.error;
    state_delete_prefix_request cleanup;
    cleanup.key_prefix = checkpoint_record.key;
    cleanup.max_sequence = before_cleanup.last_sequence;
    EXPECT_TRUE(global_state_store().delete_prefix(cleanup).ok);

    for (const std::string &path :
         {journal_path, checkpoint_path, export_path})
    {
        std::remove(path.c_str());
        std::remove((path + ".tmp").c_str());
        std::remove((path + ".bak").c_str());
    }
}

TEST(rasn_workflow_service, propagates_state_lookup_failures_when_recovering_run)
{
    const scoped_workflow_state_readers readers(&fail_workflow_state_get, nullptr);
    const std::string run_id = "unit-state-failure-" + make_trace_id();

    workflow_store query_store;
    workflow_run_query query;
    query.run_id = run_id;
    const workflow_response query_response = query_store.query(query);
    EXPECT_FALSE(query_response.ok);
    EXPECT_NE(std::string::npos, query_response.error.find("workflow state lookup failed for run " + run_id));
    EXPECT_NE(std::string::npos, query_response.error.find("injected state get failure"));

    workflow_start_request start;
    start.run_id = run_id;
    start.source.workflow_id = "unit.workflow.state-failure";
    start.source.source_name = "unit-state-failure";
    start.source.source_text = "task inspect tool \"list .\" capability tool.run\n";
    const workflow_response start_response = query_store.start(start);
    EXPECT_FALSE(start_response.ok);
    EXPECT_NE(std::string::npos, start_response.error.find("workflow state lookup failed for run " + run_id));
}

TEST(rasn_workflow_service, cancel_recovers_run_from_state)
{
    const std::string run_id = "unit-cancel-recovered-" + make_trace_id();
    const std::string workflow_id = "unit.workflow.cancel-recovered";
    const uint64_t initial_sequence = 17;
    const state_response persisted = put_workflow_run_state(run_id, workflow_id, "running", initial_sequence);
    ASSERT_TRUE(persisted.ok) << persisted.error;

    workflow_store store;
    workflow_run_query query;
    query.run_id = run_id;
    const workflow_response response = store.cancel(query);
    ASSERT_TRUE(response.ok) << response.error;
    EXPECT_EQ("cancelled", response.run.status);
    EXPECT_EQ("cancelled by request", response.run.error);
    EXPECT_EQ(initial_sequence + 1, response.run.sequence);

    state_key_request get;
    get.key = "workflow/" + run_id;
    const state_response stored = global_state_store().get(get);
    ASSERT_TRUE(stored.ok) << stored.error;
    EXPECT_EQ(initial_sequence + 1, stored.record.sequence);
    EXPECT_NE(std::string::npos, stored.record.value.find("status=cancelled"));
    EXPECT_NE(std::string::npos, stored.record.value.find("error=cancelled by request"));
}

TEST(rasn_workflow_service, cancel_reports_state_persist_failures)
{
    const std::string run_id = "unit-cancel-persist-failure-" + make_trace_id();
    const std::string workflow_id = "unit.workflow.cancel-persist-failure";
    const uint64_t initial_sequence = 23;
    const state_response persisted = put_workflow_run_state(run_id, workflow_id, "running", initial_sequence);
    ASSERT_TRUE(persisted.ok) << persisted.error;

    const scoped_workflow_state_writer writer(&fail_workflow_state_put);
    workflow_store store;
    workflow_run_query query;
    query.run_id = run_id;
    const workflow_response response = store.cancel(query);
    EXPECT_FALSE(response.ok);
    EXPECT_NE(std::string::npos, response.error.find("workflow cancel state persist failed for run " + run_id));
    EXPECT_NE(std::string::npos, response.error.find("injected state put failure"));

    state_key_request get;
    get.key = "workflow/" + run_id;
    const state_response stored = global_state_store().get(get);
    ASSERT_TRUE(stored.ok) << stored.error;
    EXPECT_EQ(initial_sequence, stored.record.sequence);
    EXPECT_NE(std::string::npos, stored.record.value.find("status=running"));
}

TEST(rasn_workflow_service, cancel_is_idempotent_for_already_cancelled_run)
{
    // A cancel whose success reply was lost is retried by the (idempotent) client.
    // The retry observes the run already in the terminal "cancelled" state and
    // must return that same success rather than an "already terminal" error, and
    // must not re-persist or advance the sequence.
    const std::string run_id = "unit-cancel-idempotent-" + make_trace_id();
    const std::string workflow_id = "unit.workflow.cancel-idempotent";
    const uint64_t cancelled_sequence = 31;
    const state_response persisted =
        put_workflow_run_state(run_id, workflow_id, "cancelled", cancelled_sequence);
    ASSERT_TRUE(persisted.ok) << persisted.error;

    workflow_store store;
    workflow_run_query query;
    query.run_id = run_id;
    const workflow_response response = store.cancel(query);
    ASSERT_TRUE(response.ok) << response.error;
    EXPECT_EQ("cancelled", response.run.status);
    EXPECT_EQ(cancelled_sequence, response.run.sequence);

    state_key_request get;
    get.key = "workflow/" + run_id;
    const state_response stored = global_state_store().get(get);
    ASSERT_TRUE(stored.ok) << stored.error;
    EXPECT_EQ(cancelled_sequence, stored.record.sequence);
}

TEST(rasn_workflow_service, cancel_rejects_non_cancelled_terminal_run)
{
    // A run that reached a *different* terminal outcome ("completed"/"failed")
    // before any cancel took effect is a genuine failure: cancel must still
    // report "already terminal" so the retry-safety carve-out is limited to the
    // cancelled state only.
    const std::string run_id = "unit-cancel-terminal-" + make_trace_id();
    const std::string workflow_id = "unit.workflow.cancel-terminal";
    const state_response persisted =
        put_workflow_run_state(run_id, workflow_id, "completed", 41);
    ASSERT_TRUE(persisted.ok) << persisted.error;

    workflow_store store;
    workflow_run_query query;
    query.run_id = run_id;
    const workflow_response response = store.cancel(query);
    EXPECT_FALSE(response.ok);
    EXPECT_NE(std::string::npos, response.error.find("workflow run is already terminal"));
}

TEST(rasn_workflow_service, rejects_duplicate_active_execution_lease)
{
    const std::string run_id = "unit-active-" + make_trace_id();
    const std::string workflow_id = "unit.workflow.lease";

    state_record lease;
    lease.key = "workflow-lease/" + run_id;
    lease.kind = "workflow.lease";
    lease.scope = "rasn.workflow";
    lease.value = lease_record_value(run_id, workflow_id, "owner-a", "running", ::dsn_now_ms() + 600000);
    state_put_request create_lease;
    create_lease.record = lease;
    create_lease.create_only = true;
    const state_response lease_response = global_state_store().put(create_lease);
    ASSERT_TRUE(lease_response.ok) << lease_response.error;

    workflow_start_request request;
    request.run_id = run_id;
    request.source.workflow_id = workflow_id;
    request.source.source_name = "unit-active-lease";
    request.source.source_text = "task inspect tool \"list .\" capability tool.run\n";

    workflow_store store;
    const workflow_response response = store.start(request);
    EXPECT_FALSE(response.ok);
    EXPECT_NE(std::string::npos, response.error.find("already owned"));
}

TEST(rasn_workflow_service, takes_over_stale_execution_lease_and_releases_it)
{
    const std::string run_id = "unit-stale-" + make_trace_id();
    const std::string workflow_id = "unit.workflow.lease";

    state_record lease;
    lease.key = "workflow-lease/" + run_id;
    lease.kind = "workflow.lease";
    lease.scope = "rasn.workflow";
    lease.value = lease_record_value(run_id, workflow_id, "stale-owner", "running", ::dsn_now_ms() - 1);
    state_put_request create_lease;
    create_lease.record = lease;
    create_lease.create_only = true;
    const state_response stale_lease = global_state_store().put(create_lease);
    ASSERT_TRUE(stale_lease.ok) << stale_lease.error;

    workflow_start_request request;
    request.run_id = run_id;
    request.source.workflow_id = workflow_id;
    request.source.source_name = "unit-stale-lease";
    request.source.source_text = "task inspect tool \"list .\" capability tool.run\n";

    register_default_tool_provider(&create_codepilot_tool_provider);
    global_rasn_services().set_tool_provider(create_codepilot_tool_provider());

    workflow_store store;
    const workflow_response response = store.start(request);
    ASSERT_TRUE(response.ok) << response.error;
    EXPECT_EQ("completed", response.run.status);
    EXPECT_FALSE(response.run.execution_owner.empty());
    EXPECT_GT(response.run.lease_sequence, stale_lease.record.sequence);

    state_key_request get_lease;
    get_lease.key = lease.key;
    const state_response released = global_state_store().get(get_lease);
    ASSERT_TRUE(released.ok) << released.error;
    EXPECT_NE(std::string::npos, released.record.value.find("status=completed"));
    EXPECT_NE(std::string::npos, released.record.value.find("owner=" + response.run.execution_owner));
}

TEST(rasn_workflow_service, renews_active_execution_lease_during_long_node)
{
    const std::string run_id = "unit-renew-" + make_trace_id();
    const std::string workflow_id = "unit.workflow.renew";

    workflow_start_request request;
    request.run_id = run_id;
    request.source.workflow_id = workflow_id;
    request.source.source_name = "unit-renew-lease";
    request.source.source_text = "task inspect tool \"list .\" capability tool.run\n";

    global_rasn_services().set_tool_provider(std::unique_ptr<agent_tool_provider>(new sleeping_tool_provider(700)));

    workflow_store primary_store;
    workflow_response primary_response;
    std::thread worker([&primary_store, &request, &primary_response]() { primary_response = primary_store.start(request); });

    bool observed_running_state = false;
    for (int i = 0; i < 100; ++i)
    {
        state_key_request lease_query;
        lease_query.key = "workflow-lease/" + run_id;
        state_key_request run_query;
        run_query.key = "workflow/" + run_id;
        const state_response lease = global_state_store().get(lease_query);
        const state_response run = global_state_store().get(run_query);
        if (lease.ok && run.ok && lease.record.value.find("status=running") != std::string::npos &&
            run.record.value.find("status=running") != std::string::npos)
        {
            observed_running_state = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(observed_running_state);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    workflow_start_request duplicate = request;
    duplicate.resume = true;
    workflow_store duplicate_store;
    const workflow_response duplicate_response = duplicate_store.start(duplicate);
    EXPECT_FALSE(duplicate_response.ok);
    EXPECT_NE(std::string::npos, duplicate_response.error.find("already owned"));

    worker.join();
    global_rasn_services().set_tool_provider(create_codepilot_tool_provider());

    ASSERT_TRUE(primary_response.ok) << primary_response.error;
    EXPECT_EQ("completed", primary_response.run.status);

    state_key_request lease_query;
    lease_query.key = "workflow-lease/" + run_id;
    const state_response released = global_state_store().get(lease_query);
    ASSERT_TRUE(released.ok) << released.error;
    EXPECT_NE(std::string::npos, released.record.value.find("status=completed"));
    EXPECT_NE(std::string::npos, released.record.value.find("owner=" + primary_response.run.execution_owner));
}

TEST(codepilot_tools, read_and_search_files)
{
    const std::string path = workspace_temp_file_path("rasn-codepilot-tools-unit.txt");
    write_text_file(path, "alpha\nneedle beta\ngamma\n");

    codepilot_tool_provider provider;
    nucleus_runtime runtime;
    agent_task task;
    task.id = "tool-test";
    task.name = "codepilot.unit";

    const tool_result read = provider.run("read", std::vector<std::string>{path, "5"}, runtime, task);
    ASSERT_TRUE(read.ok) << read.error;
    EXPECT_NE(std::string::npos, read.output.find("alpha"));
    EXPECT_NE(std::string::npos, read.output.find("[truncated at 5 bytes]"));

    const tool_result search = provider.run("search", std::vector<std::string>{path, "needle"}, runtime, task);
    ASSERT_TRUE(search.ok) << search.error;
    EXPECT_NE(std::string::npos, search.output.find("needle beta"));

    std::remove(path.c_str());
}

TEST(codepilot_tools, records_and_replay_checks_filesystem_snapshots)
{
    const std::string path = workspace_temp_file_path("rasn-codepilot-fs-replay.txt");
    const std::string trace_path = workspace_temp_file_path("rasn-codepilot-fs-replay.jsonl");
    std::remove(trace_path.c_str());
    write_text_file(path, "first\n");

    codepilot_tool_provider provider;
    agent_task task;
    task.id = "fs-replay";
    task.name = "codepilot.fs";

    nucleus_runtime writer;
    writer.set_trace_file(trace_path);
    const tool_result first = provider.run("read", std::vector<std::string>{path}, writer, task);
    ASSERT_TRUE(first.ok) << first.error;
    EXPECT_NE(std::string::npos, read_text_file(trace_path).find("\"kind\":\"filesystem.snapshot\""));

    write_text_file(path, "second\n");
    nucleus_runtime replay;
    std::string error;
    ASSERT_TRUE(replay.enable_replay(trace_path, &error)) << error;
    const tool_result mismatched = provider.run("read", std::vector<std::string>{path}, replay, task);
    EXPECT_FALSE(mismatched.ok);
    EXPECT_NE(std::string::npos, mismatched.error.find("filesystem replay snapshot mismatch"));

    std::remove(path.c_str());
    std::remove(trace_path.c_str());
}

TEST(codepilot_tools, denies_write_and_shell_by_default_at_provider_boundary)
{
    const std::string path = temp_file_path("rasn-codepilot-tools-denied.txt");
    std::remove(path.c_str());

    codepilot_tool_provider provider;
    nucleus_runtime runtime;
    agent_task task;
    task.id = "tool-denied";
    task.name = "codepilot.unit";

    const tool_result write = provider.run("write", std::vector<std::string>{path, "unsafe"}, runtime, task);
    EXPECT_FALSE(write.ok);
    EXPECT_NE(std::string::npos, write.error.find("policy denied tool 'write'"));
    EXPECT_FALSE(::dsn::utils::filesystem::file_exists(path));

    const tool_result shell = provider.run("shell", std::vector<std::string>{"echo", "unsafe"}, runtime, task);
    EXPECT_FALSE(shell.ok);
    EXPECT_NE(std::string::npos, shell.error.find("policy denied tool 'shell'"));
}

TEST(codepilot_tools, writes_files_atomically_with_rdsn_filesystem_helpers)
{
    const std::string path = temp_file_path("rasn-codepilot-tools-atomic.txt");
    std::remove(path.c_str());

    std::string error;
    ASSERT_TRUE(codepilot_write_file_atomically(path, "first", &error)) << error;
    EXPECT_EQ("first", read_text_file(path));

    ASSERT_TRUE(codepilot_write_file_atomically(path, "second", &error)) << error;
    EXPECT_EQ("second", read_text_file(path));

    std::remove(path.c_str());
}

TEST(codepilot_tools, validates_shell_allowlist_and_workspace_wrapper)
{
    std::string error;
    EXPECT_TRUE(codepilot_shell_command_allowed("git status", std::vector<std::string>(), &error));
    EXPECT_TRUE(codepilot_shell_command_allowed("git status", std::vector<std::string>{"git"}, &error)) << error;
    EXPECT_TRUE(codepilot_shell_command_allowed("C:\\tools\\git.exe status", std::vector<std::string>{"git"}, &error)) << error;

    EXPECT_FALSE(codepilot_shell_command_allowed("cmd /c echo unsafe", std::vector<std::string>{"git"}, &error));
    EXPECT_NE(std::string::npos, error.find("not in shell_allowed_commands"));
    EXPECT_FALSE(codepilot_shell_command_allowed("git status && del file", std::vector<std::string>{"git"}, &error));
    EXPECT_NE(std::string::npos, error.find("metacharacters"));

    const std::string wrapped =
        codepilot_shell_command_with_working_directory("git status", "C:\\repo with spaces");
    EXPECT_NE(std::string::npos, wrapped.find("git status"));
    EXPECT_NE(std::string::npos, wrapped.find("repo with spaces"));

    const std::string container = codepilot_shell_command_with_container_template(
        "git status", "C:\\repo with spaces", "docker run -v {workspace}:/workspace image sh -lc {command}", &error);
    ASSERT_TRUE(error.empty()) << error;
    EXPECT_NE(std::string::npos, container.find("docker run"));
    EXPECT_NE(std::string::npos, container.find("repo with spaces"));
    EXPECT_NE(std::string::npos, container.find("git status"));

    const std::string invalid =
        codepilot_shell_command_with_container_template("git status", ".", "docker run image", &error);
    EXPECT_TRUE(invalid.empty());
    EXPECT_NE(std::string::npos, error.find("{command}"));
}

TEST(codepilot_tools, times_out_shell_commands)
{
#if defined(_WIN32)
    const tool_result timed_out = codepilot_run_shell_command("ping -n 3 127.0.0.1", 100);
    EXPECT_FALSE(timed_out.ok);
    EXPECT_NE(std::string::npos, timed_out.error.find("timed out"));
#else
    SUCCEED() << "native shell timeout termination is currently implemented on Windows";
#endif
}

TEST(codepilot_tools, exposes_structured_tool_descriptors)
{
    codepilot_tool_provider provider;
    const std::vector<tool_descriptor> tools = provider.describe_tool_schemas();
    ASSERT_EQ(6u, tools.size());
    EXPECT_EQ("list", tools[0].name);
    EXPECT_EQ("read_only", tools[0].side_effect);

    bool found_shell = false;
    for (const tool_descriptor &tool : tools)
    {
        if (tool.name == "shell")
        {
            found_shell = true;
            EXPECT_EQ("shell", tool.side_effect);
            ASSERT_EQ(1u, tool.arguments.size());
            EXPECT_EQ("command", tool.arguments[0].name);
        }
    }
    EXPECT_TRUE(found_shell);
    EXPECT_NE(std::string::npos, provider.describe_tools().find("[shell]"));
}

TEST(rasn_policy, recognizes_side_effect_human_approval_labels)
{
    const std::vector<std::string> write_labels{human_approval_policy_label(tool_side_effect::write)};
    EXPECT_TRUE(policy_labels_include_human_approval(write_labels, tool_side_effect::write));
    EXPECT_FALSE(policy_labels_include_human_approval(write_labels, tool_side_effect::shell));

    const std::vector<std::string> generic_labels{"human_approved"};
    EXPECT_TRUE(policy_labels_include_human_approval(generic_labels, tool_side_effect::write));
    EXPECT_TRUE(policy_labels_include_human_approval(generic_labels, tool_side_effect::shell));
}

TEST(rasn_approval_sandbox, classifies_approval_and_sandbox_decisions)
{
    approval_sandbox_options options;
    options.require_write_approval = true;
    options.require_shell_approval = true;
    options.write_sandbox_mode = "workspace-write";
    options.shell_sandbox_mode = "command-allowlist";

    approval_sandbox_request read_request;
    read_request.tool_name = "read";
    read_request.args.push_back("src/main.cpp");
    approval_sandbox_decision read = evaluate_approval_sandbox_request(read_request, options);
    EXPECT_TRUE(read.approved);
    EXPECT_FALSE(read.prompt_required);
    EXPECT_EQ(tool_side_effect::read_only, read.side_effect);
    EXPECT_EQ("read-only", read.sandbox_mode);
    EXPECT_TRUE(read.policy_labels.empty());

    approval_sandbox_request write_request;
    write_request.tool_name = "write";
    write_request.args.push_back("src/main.cpp");
    write_request.args.push_back("updated");
    write_request.actor = "unit";
    approval_sandbox_decision write = evaluate_approval_sandbox_request(write_request, options);
    EXPECT_FALSE(write.approved);
    EXPECT_TRUE(write.prompt_required);
    EXPECT_EQ(tool_side_effect::write, write.side_effect);
    EXPECT_EQ("workspace-write", write.sandbox_mode);
    EXPECT_NE(std::string::npos, write.prompt.find("Approval required for write tool 'write' target 'src/main.cpp'"));
    EXPECT_NE(std::string::npos, write.review_text.find("content_bytes=7"));

    grant_human_approval(&write);
    ASSERT_EQ(1u, write.policy_labels.size());
    EXPECT_TRUE(write.approved);
    EXPECT_EQ(human_approval_policy_label(tool_side_effect::write), write.policy_labels[0]);

    approval_sandbox_request shell_request;
    shell_request.tool_name = "shell";
    shell_request.args.push_back("git status");
    shell_request.explicit_approval = true;
    approval_sandbox_decision shell = evaluate_approval_sandbox_request(shell_request, options);
    EXPECT_TRUE(shell.approved);
    EXPECT_FALSE(shell.prompt_required);
    ASSERT_EQ(1u, shell.policy_labels.size());
    EXPECT_EQ(human_approval_policy_label(tool_side_effect::shell), shell.policy_labels[0]);
    EXPECT_EQ("command-allowlist", shell.sandbox_mode);
}

TEST(rasn_policy, replays_recorded_side_effect_tool_results_without_execution)
{
    const std::string trace_path = temp_file_path("rasn-tool-replay-trace.jsonl");
    write_text_file(trace_path,
                    "{\"schema_version\":1,\"sequence\":1,\"trace_id\":\"trace\",\"task_id\":\"task\","
                    "\"kind\":\"tool.ok\",\"name\":\"shell\",\"value\":\"echo replayed\\nreplayed output\","
                    "\"timestamp\":\"now\"}\n");

    nucleus_runtime runtime;
    std::string error;
    ASSERT_TRUE(runtime.enable_replay(trace_path, &error)) << error;

    rasn_tool_agent_service tool_agent;
    agent_task task;
    task.id = "tool-replay";
    task.name = "tool.replay.test";

    const tool_result replayed =
        tool_agent.run_tool("shell", std::vector<std::string>{"echo", "replayed"}, runtime, task);
    ASSERT_TRUE(replayed.ok) << replayed.error;
    EXPECT_EQ("replayed output", replayed.output);

    codepilot_tool_provider provider;
    const tool_result provider_replayed =
        provider.run("shell", std::vector<std::string>{"echo", "replayed"}, runtime, task);
    ASSERT_TRUE(provider_replayed.ok) << provider_replayed.error;
    EXPECT_EQ("replayed output", provider_replayed.output);

    const tool_result missing =
        tool_agent.run_tool("shell", std::vector<std::string>{"echo", "missing"}, runtime, task);
    EXPECT_FALSE(missing.ok);
    EXPECT_NE(std::string::npos, missing.error.find("replay missing recorded side-effect tool result"));
    bool saw_replay_miss_effect = false;
    bool saw_replayed_effect = false;
    for (const runtime_event &event : runtime.events())
    {
        if (event.kind == "external.effect" && event.name == "shell:shell" &&
            event.value.find("replay_policy=recorded_result_required") != std::string::npos)
        {
            saw_replayed_effect =
                saw_replayed_effect || event.value.find("status=replayed.ok") != std::string::npos;
            saw_replay_miss_effect =
                saw_replay_miss_effect || event.value.find("status=replay_miss") != std::string::npos;
        }
    }
    EXPECT_TRUE(saw_replayed_effect);
    EXPECT_TRUE(saw_replay_miss_effect);

    std::remove(trace_path.c_str());
}

TEST(rasn_metrics_format, sanitize_metric_label_produces_prometheus_safe_tokens)
{
    EXPECT_EQ("tool_timeout", sanitize_metric_label("tool.timeout"));
    EXPECT_EQ("schema_violation", sanitize_metric_label("schema violation"));
    EXPECT_EQ("a_b_c", sanitize_metric_label("a/b-c"));
    EXPECT_EQ("unknown", sanitize_metric_label(""));
    EXPECT_EQ("_5xx", sanitize_metric_label("5xx"));
    EXPECT_EQ("already_ok_1", sanitize_metric_label("already_ok_1"));
}

TEST(rasn_metrics_format, snapshot_accessors_and_text_rendering)
{
    metrics_snapshot snapshot;
    snapshot.enabled = true;

    metric_sample counter;
    counter.name = "rasn_tasks_begin_total";
    counter.help = "agent tasks started";
    counter.is_latency = false;
    counter.value = 7;
    snapshot.samples.push_back(counter);

    metric_sample latency;
    latency.name = "rasn_task_latency_ms";
    latency.help = "agent task wall-clock latency in milliseconds";
    latency.is_latency = true;
    latency.p50 = 1.0;
    latency.p95 = 2.5;
    latency.p99 = 3.0;
    latency.p999 = 4.0;
    snapshot.samples.push_back(latency);

    ASSERT_NE(nullptr, snapshot.find("rasn_tasks_begin_total"));
    EXPECT_EQ(nullptr, snapshot.find("does_not_exist"));
    EXPECT_EQ(7u, snapshot.counter("rasn_tasks_begin_total"));
    EXPECT_EQ(0u, snapshot.counter("does_not_exist"));

    const std::string text = snapshot.to_text();
    EXPECT_NE(std::string::npos, text.find("rasn metrics (enabled=true)"));
    EXPECT_NE(std::string::npos, text.find("rasn_tasks_begin_total"));
    EXPECT_NE(std::string::npos, text.find("  7"));
    EXPECT_NE(std::string::npos, text.find("p50=1ms"));
    EXPECT_NE(std::string::npos, text.find("p95=2.5ms"));
}

TEST(rasn_metrics_format, prometheus_and_json_rendering)
{
    metrics_snapshot snapshot;
    snapshot.enabled = false;

    metric_sample counter;
    counter.name = "rasn_failures_total";
    counter.help = "classified failures";
    counter.value = 3;
    snapshot.samples.push_back(counter);

    metric_sample latency;
    latency.name = "rasn_llm_latency_ms";
    latency.help = "model completion latency in milliseconds";
    latency.is_latency = true;
    latency.p50 = 10.0;
    latency.p99 = 42.0;
    snapshot.samples.push_back(latency);

    const std::string prom = snapshot.to_prometheus();
    EXPECT_NE(std::string::npos, prom.find("# HELP rasn_failures_total classified failures"));
    EXPECT_NE(std::string::npos, prom.find("# TYPE rasn_failures_total counter"));
    EXPECT_NE(std::string::npos, prom.find("rasn_failures_total 3"));
    EXPECT_NE(std::string::npos, prom.find("# TYPE rasn_llm_latency_ms summary"));
    EXPECT_NE(std::string::npos, prom.find("rasn_llm_latency_ms{quantile=\"0.5\"} 10"));
    EXPECT_NE(std::string::npos, prom.find("rasn_llm_latency_ms{quantile=\"0.99\"} 42"));

    const std::string json = snapshot.to_json();
    EXPECT_NE(std::string::npos, json.find("\"enabled\":false"));
    EXPECT_NE(std::string::npos, json.find("\"name\":\"rasn_failures_total\""));
    EXPECT_NE(std::string::npos, json.find("\"type\":\"counter\""));
    EXPECT_NE(std::string::npos, json.find("\"value\":3"));
    EXPECT_NE(std::string::npos, json.find("\"type\":\"latency_ms\""));
    EXPECT_NE(std::string::npos, json.find("\"p99\":42"));
}

TEST(rasn_metrics_registry, snapshot_exposes_core_and_latency_series)
{
    // Bind this thread to an rDSN service node so perf counters are live
    // (enable_default_app_mimic installs the mimic node on first access).
    (void)::dsn::task::get_current_node();

    metrics_registry::instance().ensure_core_counters();
    const metrics_snapshot snapshot = metrics_registry::instance().snapshot();

    // Core cumulative counters are always present.
    EXPECT_NE(nullptr, snapshot.find("rasn_tasks_begin_total"));
    EXPECT_NE(nullptr, snapshot.find("rasn_tasks_finish_total"));
    EXPECT_NE(nullptr, snapshot.find("rasn_llm_requests_total"));
    EXPECT_NE(nullptr, snapshot.find("rasn_tool_ok_total"));
    EXPECT_NE(nullptr, snapshot.find("rasn_failures_total"));
    EXPECT_NE(nullptr, snapshot.find("rasn_remote_agent_breaker_open_total"));
    EXPECT_NE(nullptr, snapshot.find("rasn_remote_agent_admission_rejected_total"));
    EXPECT_NE(nullptr, snapshot.find("rasn_remote_agent_rate_limited_total"));
    EXPECT_NE(nullptr, snapshot.find("rasn_remote_agent_endpoint_invalid_total"));
    EXPECT_NE(nullptr, snapshot.find("rasn_overload_admission_rejected_total"));
    EXPECT_NE(nullptr, snapshot.find("rasn_overload_admission_delayed_total"));
    EXPECT_NE(nullptr, snapshot.find("rasn_overload_rate_limited_total"));
    EXPECT_NE(nullptr, snapshot.find("rasn_overload_rate_delayed_total"));
    EXPECT_NE(nullptr, snapshot.find("rasn_runtime_dedup_hit_total"));
    EXPECT_NE(nullptr, snapshot.find("rasn_runtime_dedup_miss_total"));
    EXPECT_NE(nullptr, snapshot.find("rasn_runtime_dedup_wait_total"));
    EXPECT_NE(nullptr, snapshot.find("rasn_runtime_dedup_evicted_total"));
    EXPECT_NE(nullptr, snapshot.find("rasn_runtime_dedup_expired_total"));

    // Latency series are present and flagged as latency samples.
    const metric_sample *task_latency = snapshot.find("rasn_task_latency_ms");
    const metric_sample *llm_latency = snapshot.find("rasn_llm_latency_ms");
    const metric_sample *tool_latency = snapshot.find("rasn_tool_latency_ms");
    ASSERT_NE(nullptr, task_latency);
    ASSERT_NE(nullptr, llm_latency);
    ASSERT_NE(nullptr, tool_latency);
    EXPECT_TRUE(task_latency->is_latency);
    EXPECT_TRUE(llm_latency->is_latency);
    EXPECT_TRUE(tool_latency->is_latency);
}

TEST(rasn_metrics_registry, runtime_events_increment_cumulative_counters)
{
    (void)::dsn::task::get_current_node();

    metrics_registry &registry = metrics_registry::instance();
    registry.ensure_core_counters();

    const uint64_t begin_before = registry.snapshot().counter("rasn_tasks_begin_total");
    const uint64_t finish_before = registry.snapshot().counter("rasn_tasks_finish_total");
    const uint64_t remote_breaker_before = registry.snapshot().counter("rasn_remote_agent_breaker_open_total");
    const uint64_t remote_admission_before =
        registry.snapshot().counter("rasn_remote_agent_admission_rejected_total");
    const uint64_t remote_rate_before = registry.snapshot().counter("rasn_remote_agent_rate_limited_total");
    const uint64_t remote_endpoint_before =
        registry.snapshot().counter("rasn_remote_agent_endpoint_invalid_total");
    const uint64_t overload_admission_before =
        registry.snapshot().counter("rasn_overload_admission_rejected_total");
    const uint64_t overload_admission_delayed_before =
        registry.snapshot().counter("rasn_overload_admission_delayed_total");
    const uint64_t overload_rate_before = registry.snapshot().counter("rasn_overload_rate_limited_total");
    const uint64_t overload_rate_delayed_before =
        registry.snapshot().counter("rasn_overload_rate_delayed_total");
    const uint64_t runtime_dedup_hit_before = registry.snapshot().counter("rasn_runtime_dedup_hit_total");

    nucleus_runtime runtime;
    agent_task task;
    task.id = "metrics-counter-task";
    task.name = "unit.metrics";
    task.input = "noop";
    runtime.begin_task(task);
    runtime.record_remote_agent_breaker_open(task, "unit.remote", 2);
    runtime.record_remote_agent_admission_rejected(task, "unit.remote", 3, 2);
    runtime.record_remote_agent_rate_limited(task, "unit.remote", 60);
    runtime.record_remote_agent_endpoint_invalid(task, "unit.remote", "agent descriptor has no endpoint");
    runtime.record_overload_admission_rejected(task, 3, 2);
    runtime.record_overload_admission_delayed(task, 2, 50);
    runtime.record_overload_rate_limited(task, 60);
    runtime.record_overload_rate_delayed(task, 40);
    registry.on_event("runtime.dedup.hit", "");
    runtime.finish_task(task, "ok");

    const uint64_t begin_after = registry.snapshot().counter("rasn_tasks_begin_total");
    const uint64_t finish_after = registry.snapshot().counter("rasn_tasks_finish_total");

    EXPECT_EQ(begin_before + 1, begin_after);
    EXPECT_EQ(finish_before + 1, finish_after);
    EXPECT_EQ(remote_breaker_before + 1, registry.snapshot().counter("rasn_remote_agent_breaker_open_total"));
    EXPECT_EQ(remote_admission_before + 1,
              registry.snapshot().counter("rasn_remote_agent_admission_rejected_total"));
    EXPECT_EQ(remote_rate_before + 1, registry.snapshot().counter("rasn_remote_agent_rate_limited_total"));
    EXPECT_EQ(remote_endpoint_before + 1,
              registry.snapshot().counter("rasn_remote_agent_endpoint_invalid_total"));
    EXPECT_EQ(overload_admission_before + 1,
              registry.snapshot().counter("rasn_overload_admission_rejected_total"));
    EXPECT_EQ(overload_admission_delayed_before + 1,
              registry.snapshot().counter("rasn_overload_admission_delayed_total"));
    EXPECT_EQ(overload_rate_before + 1, registry.snapshot().counter("rasn_overload_rate_limited_total"));
    EXPECT_EQ(overload_rate_delayed_before + 1,
              registry.snapshot().counter("rasn_overload_rate_delayed_total"));
    EXPECT_EQ(runtime_dedup_hit_before + 1, registry.snapshot().counter("rasn_runtime_dedup_hit_total"));

    // Observing a latency value must never crash, even though percentiles are
    // computed asynchronously by rDSN counter timers.
    registry.observe_task_latency_ms(5);
    registry.observe_llm_latency_ms(5);
    registry.observe_tool_latency_ms(5);
}

TEST(rasn_metrics_registry, model_cost_events_increment_counters)
{
    (void)::dsn::task::get_current_node();

    metrics_registry &registry = metrics_registry::instance();
    registry.ensure_core_counters();

    const uint64_t limited_before = registry.snapshot().counter("rasn_model_cost_limited_total");
    const uint64_t delayed_before = registry.snapshot().counter("rasn_model_cost_delayed_total");

    nucleus_runtime runtime;
    agent_task task;
    task.id = "model-cost-task";
    task.name = "unit.model.cost";
    task.input = "noop";
    runtime.begin_task(task);
    runtime.record_model_cost_limited(task, "unit.provider", 12000, 150);
    runtime.record_model_cost_delayed(task, "unit.provider", 40);
    runtime.finish_task(task, "ok");

    EXPECT_EQ(limited_before + 1, registry.snapshot().counter("rasn_model_cost_limited_total"));
    EXPECT_EQ(delayed_before + 1, registry.snapshot().counter("rasn_model_cost_delayed_total"));
}

TEST(rasn_metrics_registry, failure_events_create_per_class_counters)
{
    (void)::dsn::task::get_current_node();

    metrics_registry &registry = metrics_registry::instance();
    registry.on_event("failure", "tool.timeout");
    registry.on_event("failure", "tool.timeout");

    const metrics_snapshot snapshot = registry.snapshot();
    const metric_sample *per_class = snapshot.find("rasn_failures_class_tool_timeout_total");
    ASSERT_NE(nullptr, per_class);
    EXPECT_GE(per_class->value, 2u);
}

TEST(rasn_metrics_registry, ops_command_dumps_metrics_in_requested_format)
{
    rasn_service_graph services;
    services.acquire();

    ::dsn::safe_string text_out;
    ASSERT_TRUE(::dsn::run_command("rasn.metrics text", text_out));
    EXPECT_NE(std::string::npos, std::string(text_out.c_str()).find("rasn metrics (enabled="));

    ::dsn::safe_string prom_out;
    ASSERT_TRUE(::dsn::run_command("rasn.metrics prometheus", prom_out));
    EXPECT_NE(std::string::npos, std::string(prom_out.c_str()).find("# TYPE rasn_tasks_begin_total counter"));

    ::dsn::safe_string json_out;
    ASSERT_TRUE(::dsn::run_command("rasn.metrics json", json_out));
    EXPECT_NE(std::string::npos, std::string(json_out.c_str()).find("\"metrics\":["));

    services.release();
}

TEST(rasn_circuit_breaker, opens_after_consecutive_failures_and_short_circuits)
{
    breaker_config cfg;
    cfg.failure_threshold = 3;
    cfg.open_ms = 1000;
    circuit_breaker breaker(cfg);

    // Two failures keep the breaker closed.
    EXPECT_TRUE(breaker.allow(0).allowed);
    EXPECT_FALSE(breaker.report(false, 0));
    EXPECT_TRUE(breaker.allow(0).allowed);
    EXPECT_FALSE(breaker.report(false, 0));
    EXPECT_EQ(breaker_state::closed, breaker.state());

    // The third consecutive failure trips it; report() signals the transition.
    EXPECT_TRUE(breaker.allow(0).allowed);
    EXPECT_TRUE(breaker.report(false, 0));
    EXPECT_EQ(breaker_state::open, breaker.state());

    // Within the cooldown, requests are short-circuited.
    const breaker_decision blocked = breaker.allow(500);
    EXPECT_FALSE(blocked.allowed);
    EXPECT_EQ(breaker_state::open, blocked.state);
}

TEST(rasn_rpc_resilience, retries_are_idempotency_aware_and_breaker_short_circuits)
{
    // Fake typed response so the test exercises resilient_rpc_call end to end
    // without any rDSN transport, mirroring the core-service call sites in
    // agent_services.cpp / coordinator_service.cpp.
    struct fake_response
    {
        std::string value;
        bool ok = false;
    };

    breaker_config cfg;
    cfg.enabled = true;
    cfg.failure_threshold = 2;
    cfg.open_ms = 600000; // long cooldown so the breaker stays open for the test
    circuit_breaker_registry breakers(cfg);

    rpc_resilience_options options;
    options.breaker_enabled = true;
    options.max_attempts = 3;
    options.backoff_ms = 0; // keep the unit test instantaneous

    // 1. A pre-apply transport error is retried until the endpoint recovers, even
    //    for a non-idempotent operation (the request provably never applied).
    {
        int attempts = 0;
        const std::pair< ::dsn::error_code, fake_response> result =
            resilient_rpc_call<fake_response>(
                breakers, "case.pre_apply", options, /*idempotent=*/false,
                std::chrono::milliseconds(10),
                [&](std::chrono::milliseconds) {
                    ++attempts;
                    if (attempts < 2)
                    {
                        return std::make_pair(
                            ::dsn::ERR_NETWORK_INIT_FAILED, fake_response());
                    }
                    fake_response ok;
                    ok.value = "ok";
                    ok.ok = true;
                    return std::make_pair(::dsn::ERR_OK, ok);
                });
        EXPECT_EQ(::dsn::ERR_OK, result.first);
        EXPECT_EQ(2, attempts);
        EXPECT_TRUE(result.second.ok);
    }

    // 2. An ambiguous ERR_TIMEOUT is NOT retried for a non-idempotent operation:
    //    exactly one attempt, and the timeout is surfaced to the caller.
    {
        int attempts = 0;
        const std::pair< ::dsn::error_code, fake_response> result =
            resilient_rpc_call<fake_response>(
                breakers, "case.timeout_non_idempotent", options, /*idempotent=*/false,
                std::chrono::milliseconds(10),
                [&](std::chrono::milliseconds) {
                    ++attempts;
                    return std::make_pair(::dsn::ERR_TIMEOUT, fake_response());
                });
        EXPECT_EQ(::dsn::ERR_TIMEOUT, result.first);
        EXPECT_EQ(1, attempts);
    }

    // 3. ERR_NETWORK_FAILURE can represent a lost reply after application. It is
    //    therefore not retried for a non-idempotent operation.
    {
        int attempts = 0;
        const std::pair< ::dsn::error_code, fake_response> result =
            resilient_rpc_call<fake_response>(
                breakers,
                "case.network_failure_non_idempotent",
                options,
                /*idempotent=*/false,
                std::chrono::milliseconds(10),
                [&](std::chrono::milliseconds) {
                    ++attempts;
                    return std::make_pair(
                        ::dsn::ERR_NETWORK_FAILURE, fake_response());
                });
        EXPECT_EQ(::dsn::ERR_NETWORK_FAILURE, result.first);
        EXPECT_EQ(1, attempts);
    }

    // 4. The same ERR_TIMEOUT IS retried up to max_attempts for an idempotent op.
    {
        int attempts = 0;
        const std::pair< ::dsn::error_code, fake_response> result =
            resilient_rpc_call<fake_response>(
                breakers, "case.timeout_idempotent", options, /*idempotent=*/true,
                std::chrono::milliseconds(10),
                [&](std::chrono::milliseconds) {
                    ++attempts;
                    return std::make_pair(::dsn::ERR_TIMEOUT, fake_response());
                });
        EXPECT_EQ(::dsn::ERR_TIMEOUT, result.first);
        EXPECT_EQ(3, attempts);
    }

    // 5. After enough hard failures the per-endpoint breaker opens and the next
    //    call short-circuits with ERR_BUSY WITHOUT invoking the dependency.
    {
        const auto always_fail = [](std::chrono::milliseconds) {
            return std::make_pair(::dsn::ERR_NETWORK_FAILURE, fake_response());
        };
        // Two calls -> two reported failures -> breaker trips (threshold == 2).
        resilient_rpc_call<fake_response>(breakers, "case.breaker", options, false,
                                          std::chrono::milliseconds(10), always_fail);
        resilient_rpc_call<fake_response>(breakers, "case.breaker", options, false,
                                          std::chrono::milliseconds(10), always_fail);

        bool invoked = false;
        const std::pair< ::dsn::error_code, fake_response> result =
            resilient_rpc_call<fake_response>(
                breakers, "case.breaker", options, false, std::chrono::milliseconds(10),
                [&](std::chrono::milliseconds) {
                    invoked = true;
                    fake_response unreached;
                    unreached.ok = true;
                    return std::make_pair(::dsn::ERR_OK, unreached);
                });
        EXPECT_EQ(::dsn::ERR_BUSY, result.first);
        EXPECT_FALSE(invoked);
    }
}

TEST(rasn_rpc_resilience, breaker_key_is_service_scoped_so_ops_trip_together)
{
    // Finding: keying the breaker per (operation, endpoint) splits an endpoint's
    // failures across keys, so no single operation reaches the threshold and the
    // breaker never opens even though the endpoint is down. The key must be
    // service-scoped: every operation of a service to one endpoint shares it.
    EXPECT_EQ("state@host:34801", core_service_breaker_key("state.put", "host:34801"));
    EXPECT_EQ(core_service_breaker_key("state.put", "host:34801"),
              core_service_breaker_key("state.get", "host:34801"));
    EXPECT_NE(core_service_breaker_key("state.put", "host:34801"),
              core_service_breaker_key("workflow.cancel", "host:34801"));
    EXPECT_NE(core_service_breaker_key("state.put", "host:34801"),
              core_service_breaker_key("state.put", "host:34802"));
    // An op with no '.' is treated as its own service.
    EXPECT_EQ("ping@host:34801", core_service_breaker_key("ping", "host:34801"));

    breaker_config cfg;
    cfg.enabled = true;
    cfg.failure_threshold = 2;
    cfg.open_ms = 600000;
    circuit_breaker_registry breakers(cfg);

    rpc_resilience_options options;
    options.breaker_enabled = true;
    options.max_attempts = 1; // one shot per call so each failure is one report
    options.backoff_ms = 0;

    struct fake_response
    {
        bool ok = false;
    };
    const std::string endpoint = "host:34801";
    const auto always_fail = [](std::chrono::milliseconds) {
        return std::make_pair(::dsn::ERR_NETWORK_FAILURE, fake_response());
    };

    // Two DIFFERENT operations of the same service each fail once. Because they
    // share the service-scoped breaker key, the two failures aggregate and trip
    // the breaker (threshold == 2).
    resilient_rpc_call<fake_response>(breakers,
                                      core_service_breaker_key("state.put", endpoint),
                                      options, false, std::chrono::milliseconds(10), always_fail);
    resilient_rpc_call<fake_response>(breakers,
                                      core_service_breaker_key("state.get", endpoint),
                                      options, false, std::chrono::milliseconds(10), always_fail);

    // A third operation of the same service to the same endpoint now short-
    // circuits WITHOUT invoking the dependency -- the breaker is open for the
    // whole service, not just one operation.
    bool invoked = false;
    const std::pair< ::dsn::error_code, fake_response> result =
        resilient_rpc_call<fake_response>(
            breakers, core_service_breaker_key("state.query", endpoint), options, true,
            std::chrono::milliseconds(10), [&](std::chrono::milliseconds) {
                invoked = true;
                return std::make_pair(::dsn::ERR_OK, fake_response());
            });
    EXPECT_EQ(::dsn::ERR_BUSY, result.first);
    EXPECT_FALSE(invoked);
}

TEST(rasn_circuit_breaker, half_open_admits_single_probe_then_recovers)
{
    breaker_config cfg;
    cfg.failure_threshold = 1;
    cfg.open_ms = 1000;
    circuit_breaker breaker(cfg);

    EXPECT_TRUE(breaker.report(false, 0)); // opens immediately (threshold 1)
    EXPECT_EQ(breaker_state::open, breaker.state());

    // After the cooldown, exactly one probe is admitted.
    const breaker_decision probe = breaker.allow(1000);
    EXPECT_TRUE(probe.allowed);
    EXPECT_TRUE(probe.half_open_probe);
    EXPECT_EQ(breaker_state::half_open, breaker.state());

    // A concurrent request while the probe is outstanding is denied.
    EXPECT_FALSE(breaker.allow(1000).allowed);

    // A successful probe closes the breaker and clears the failure count.
    EXPECT_FALSE(breaker.report(true, 1000));
    EXPECT_EQ(breaker_state::closed, breaker.state());
    EXPECT_EQ(0u, breaker.consecutive_failures());
}

TEST(rasn_circuit_breaker, stale_closed_report_cannot_resolve_half_open_probe)
{
    breaker_config cfg;
    cfg.failure_threshold = 1;
    cfg.open_ms = 100;
    circuit_breaker breaker(cfg);

    const breaker_decision stale_closed = breaker.allow(0);
    ASSERT_TRUE(stale_closed.allowed);
    EXPECT_TRUE(breaker.report(false, 0));

    const breaker_decision probe = breaker.allow(100);
    ASSERT_TRUE(probe.allowed);
    ASSERT_TRUE(probe.half_open_probe);
    const breaker_report stale =
        breaker.report(stale_closed, true, 100);
    EXPECT_FALSE(stale.applied);
    EXPECT_EQ(breaker_state::half_open, stale.state);
    EXPECT_FALSE(breaker.allow(100).allowed);

    const breaker_report recovered = breaker.report(probe, true, 100);
    EXPECT_TRUE(recovered.applied);
    EXPECT_EQ(breaker_state::closed, recovered.state);
}

TEST(rasn_circuit_breaker, stale_preopen_failure_cannot_reopen_recovered_breaker)
{
    breaker_config cfg;
    cfg.failure_threshold = 1;
    cfg.open_ms = 100;
    circuit_breaker breaker(cfg);

    const breaker_decision stale = breaker.allow(0);
    const breaker_decision opener = breaker.allow(0);
    ASSERT_TRUE(stale.allowed);
    ASSERT_TRUE(opener.allowed);
    EXPECT_TRUE(breaker.report(opener, false, 0).opened);

    const breaker_decision probe = breaker.allow(100);
    ASSERT_TRUE(probe.half_open_probe);
    EXPECT_TRUE(breaker.report(probe, true, 100).applied);
    ASSERT_EQ(breaker_state::closed, breaker.state());

    const breaker_report ignored = breaker.report(stale, false, 101);
    EXPECT_FALSE(ignored.applied);
    EXPECT_EQ(breaker_state::closed, ignored.state);
    EXPECT_EQ(0u, ignored.consecutive_failures);
}

TEST(rasn_circuit_breaker, is_open_precheck_is_nonmutating_and_respects_cooldown)
{
    breaker_config cfg;
    cfg.failure_threshold = 1;
    cfg.open_ms = 1000;
    circuit_breaker breaker(cfg);

    // A closed breaker is never reported open.
    EXPECT_FALSE(breaker.is_open(0));

    EXPECT_TRUE(breaker.report(false, 0)); // opens immediately (threshold 1)
    EXPECT_EQ(breaker_state::open, breaker.state());

    // While cooling down, the non-mutating precheck reports open...
    EXPECT_TRUE(breaker.is_open(10));
    EXPECT_TRUE(breaker.is_open(999));
    // ...and repeated prechecks do not consume the one-shot half-open probe.
    EXPECT_EQ(breaker_state::open, breaker.state());

    // Once the cooldown elapses the breaker is ready to admit a probe, so the
    // precheck no longer reports a hard-open state (the request should flow
    // through the other gates and let allow() admit the probe).
    EXPECT_FALSE(breaker.is_open(1000));
    EXPECT_EQ(breaker_state::open, breaker.state()); // still not mutated by the precheck

    // allow() (the authoritative, mutating check) still admits exactly one probe.
    const breaker_decision probe = breaker.allow(1000);
    EXPECT_TRUE(probe.allowed);
    EXPECT_TRUE(probe.half_open_probe);
    EXPECT_EQ(breaker_state::half_open, breaker.state());
    // In half-open the precheck does not claim hard-open (allow() arbitrates).
    EXPECT_FALSE(breaker.is_open(1000));
}

TEST(rasn_circuit_breaker, disabled_breaker_is_never_open)
{
    breaker_config cfg;
    cfg.enabled = false;
    cfg.failure_threshold = 1;
    circuit_breaker breaker(cfg);
    EXPECT_FALSE(breaker.report(false, 0));
    EXPECT_FALSE(breaker.is_open(0));
}

TEST(rasn_circuit_breaker, half_open_probe_failure_reopens)
{
    breaker_config cfg;
    cfg.failure_threshold = 1;
    cfg.open_ms = 100;
    circuit_breaker breaker(cfg);

    EXPECT_TRUE(breaker.report(false, 0));
    const breaker_decision probe = breaker.allow(100);
    EXPECT_TRUE(probe.allowed);
    EXPECT_TRUE(probe.half_open_probe);

    // A failed probe reopens the breaker and restarts the cooldown.
    EXPECT_TRUE(breaker.report(false, 100));
    EXPECT_EQ(breaker_state::open, breaker.state());
    EXPECT_FALSE(breaker.allow(150).allowed);
    EXPECT_TRUE(breaker.allow(200).allowed);
}

TEST(rasn_circuit_breaker, success_resets_consecutive_failures)
{
    breaker_config cfg;
    cfg.failure_threshold = 3;
    cfg.open_ms = 1000;
    circuit_breaker breaker(cfg);

    breaker.report(false, 0);
    breaker.report(false, 0);
    EXPECT_EQ(2u, breaker.consecutive_failures());
    breaker.report(true, 0);
    EXPECT_EQ(0u, breaker.consecutive_failures());

    // Two more failures must not open it because the count was reset.
    breaker.report(false, 0);
    breaker.report(false, 0);
    EXPECT_EQ(breaker_state::closed, breaker.state());
}

TEST(rasn_circuit_breaker, disabled_breaker_always_allows)
{
    breaker_config cfg;
    cfg.enabled = false;
    cfg.failure_threshold = 1;
    circuit_breaker breaker(cfg);

    EXPECT_TRUE(breaker.allow(0).allowed);
    EXPECT_FALSE(breaker.report(false, 0));
    EXPECT_TRUE(breaker.allow(0).allowed);
    EXPECT_EQ(breaker_state::closed, breaker.state());
}

TEST(rasn_circuit_breaker, registry_isolates_breakers_per_key)
{
    breaker_config cfg;
    cfg.failure_threshold = 1;
    cfg.open_ms = 1000;
    circuit_breaker_registry registry(cfg);

    circuit_breaker &a = registry.get("provider.a");
    circuit_breaker &b = registry.get("provider.b");
    EXPECT_TRUE(a.report(false, 0)); // opens a only
    EXPECT_EQ(breaker_state::open, a.state());
    EXPECT_EQ(breaker_state::closed, b.state());

    // The same key always returns the same breaker instance.
    EXPECT_EQ(&a, &registry.get("provider.a"));

    const std::vector<circuit_breaker_registry::entry> snapshot = registry.snapshot();
    ASSERT_EQ(2u, snapshot.size());
    EXPECT_EQ("provider.a", snapshot[0].key);
    EXPECT_EQ(breaker_state::open, snapshot[0].state);
    EXPECT_EQ("provider.b", snapshot[1].key);
    EXPECT_EQ(breaker_state::closed, snapshot[1].state);
}

TEST(rasn_circuit_breaker, ops_command_reports_breaker_state)
{
    rasn_service_graph services;
    services.acquire();

    ::dsn::safe_string out;
    ASSERT_TRUE(::dsn::run_command("rasn.resilience", out));
    EXPECT_NE(std::string::npos, std::string(out.c_str()).find("model circuit breakers"));
    EXPECT_NE(std::string::npos, std::string(out.c_str()).find("model admission control"));
    EXPECT_NE(std::string::npos, std::string(out.c_str()).find("model rate limiters"));
    EXPECT_NE(std::string::npos, std::string(out.c_str()).find("tool admission control"));
    EXPECT_NE(std::string::npos, std::string(out.c_str()).find("tool rate limiters"));
    EXPECT_NE(std::string::npos, std::string(out.c_str()).find("remote agent circuit breakers"));
    EXPECT_NE(std::string::npos, std::string(out.c_str()).find("remote agent admission control"));
    EXPECT_NE(std::string::npos, std::string(out.c_str()).find("remote agent rate limiters"));

    services.release();
}

TEST(rasn_circuit_breaker, network_providers_are_breaker_guarded)
{
    // In-process providers (simulator) must be exempt from the breaker.
    EXPECT_TRUE(create_provider("simulator")->in_process());

    // Loopback HTTP providers are marked local in their descriptor but still
    // issue curl/HTTP requests, so they must NOT be exempt -- this is the exact
    // regression: "local" must not imply breaker-exempt.
    const char *loopback_http_providers[] = {"ollama", "llamacpp", "lmstudio"};
    for (const char *provider : loopback_http_providers)
    {
        std::unique_ptr<llm_provider> instance = create_provider(provider);
        EXPECT_TRUE(instance->describe().local) << "provider " << provider << " expected to be local";
        EXPECT_FALSE(instance->in_process()) << "provider " << provider << " should be breaker-guarded";
    }

    // Remote providers are guarded too.
    EXPECT_FALSE(create_provider("copilot")->in_process());
}

TEST(rasn_admission_gate, bulkhead_rejects_over_concurrency_cap)
{
    admission_config cfg;
    cfg.enabled = true;
    cfg.max_concurrency = 3;
    cfg.soft_concurrency = 0; // backpressure off; isolate the hard cap
    admission_gate gate(cfg);

    admission_slot s1 = gate.try_admit();
    admission_slot s2 = gate.try_admit();
    admission_slot s3 = gate.try_admit();
    EXPECT_TRUE(s1.admitted());
    EXPECT_TRUE(s2.admitted());
    EXPECT_TRUE(s3.admitted());
    EXPECT_EQ(3u, gate.in_flight());

    admission_slot s4 = gate.try_admit();
    EXPECT_FALSE(s4.admitted());
    EXPECT_EQ(3u, s4.limit());
    EXPECT_EQ(3u, gate.in_flight()); // rejection reserves nothing
}

TEST(rasn_admission_gate, releasing_a_slot_restores_capacity)
{
    admission_config cfg;
    cfg.max_concurrency = 1;
    cfg.soft_concurrency = 0;
    admission_gate gate(cfg);

    {
        admission_slot held = gate.try_admit();
        ASSERT_TRUE(held.admitted());
        EXPECT_EQ(1u, gate.in_flight());
        EXPECT_FALSE(gate.try_admit().admitted()); // at cap
    }
    // The RAII slot released on scope exit, so capacity is available again.
    EXPECT_EQ(0u, gate.in_flight());
    admission_slot again = gate.try_admit();
    EXPECT_TRUE(again.admitted());
}

TEST(rasn_admission_gate, backpressure_is_zero_below_soft_then_grows_and_clamps)
{
    admission_config cfg;
    cfg.enabled = true;
    cfg.max_concurrency = 10;
    cfg.soft_concurrency = 2;
    cfg.max_backpressure_ms = 200;
    admission_gate gate(cfg);

    std::vector<admission_slot> held;
    admission_slot first = gate.try_admit(); // in_flight 1 < soft
    EXPECT_TRUE(first.admitted());
    EXPECT_EQ(0u, first.delay_ms());
    held.push_back(std::move(first));

    admission_slot at_soft = gate.try_admit(); // in_flight 2 == soft
    const uint32_t delay_at_soft = at_soft.delay_ms();
    held.push_back(std::move(at_soft));

    admission_slot above_soft = gate.try_admit(); // in_flight 3 > soft
    const uint32_t delay_above_soft = above_soft.delay_ms();
    held.push_back(std::move(above_soft));

    EXPECT_GE(delay_above_soft, delay_at_soft); // delay grows with load
    EXPECT_LE(delay_above_soft, 200u);          // clamped to max_backpressure_ms
}

TEST(rasn_admission_gate, oversized_backpressure_does_not_overflow)
{
    // A near type-max max_backpressure_ms (read_config_u32 admits up to uint32_t's
    // maximum) must not overflow the signed-int delay curve into a negative or
    // garbage value: the resulting delay stays non-negative and bounded by an
    // int-safe ceiling rather than wrapping.
    admission_config cfg;
    cfg.enabled = true;
    cfg.max_concurrency = 10;
    cfg.soft_concurrency = 2;
    cfg.max_backpressure_ms = (std::numeric_limits<uint32_t>::max)(); // intentionally absurd / out of range
    admission_gate gate(cfg);

    std::vector<admission_slot> held;
    for (int i = 0; i < 4; ++i)
    {
        admission_slot s = gate.try_admit();
        EXPECT_TRUE(s.admitted());
        // Must be a sane, non-wrapped delay (the int-safe peak bound is
        // INT_MAX/10 ms); the key property is it is not a huge/garbage value.
        EXPECT_LE(s.delay_ms(), static_cast<uint32_t>(INT_MAX) / 10u);
        held.push_back(std::move(s));
    }
}

TEST(rasn_admission_gate, disabled_gate_is_passthrough)
{
    admission_config cfg;
    cfg.enabled = false;
    cfg.max_concurrency = 1;
    admission_gate gate(cfg);

    admission_slot s1 = gate.try_admit();
    admission_slot s2 = gate.try_admit();
    EXPECT_TRUE(s1.admitted());
    EXPECT_TRUE(s2.admitted());   // cap not enforced when disabled
    EXPECT_EQ(0u, gate.in_flight()); // passthrough reserves no capacity
}

TEST(rasn_admission_gate, registry_isolates_gates_per_key)
{
    admission_config cfg;
    cfg.max_concurrency = 5;
    cfg.soft_concurrency = 3;
    admission_gate_registry registry(cfg);

    admission_slot a = registry.get("ollama").try_admit();
    admission_slot b = registry.get("copilot").try_admit();
    ASSERT_TRUE(a.admitted());
    ASSERT_TRUE(b.admitted());

    std::vector<admission_gate_registry::entry> snapshot = registry.snapshot();
    ASSERT_EQ(2u, snapshot.size());
    EXPECT_EQ("copilot", snapshot[0].key); // ordered by key
    EXPECT_EQ("ollama", snapshot[1].key);
    for (const admission_gate_registry::entry &entry : snapshot)
    {
        EXPECT_EQ(1u, entry.in_flight);
        EXPECT_EQ(5u, entry.max_concurrency);
        EXPECT_EQ(3u, entry.soft_concurrency);
    }
}

TEST(rasn_rate_limiter, disabled_or_unlimited_is_passthrough)
{
    // requests_per_min == 0 means unlimited: every request passes with no delay.
    rate_limit_config unlimited;
    unlimited.enabled = true;
    unlimited.requests_per_min = 0;
    rate_limiter open(unlimited);
    for (int i = 0; i < 100; ++i)
    {
        const rate_decision d = open.try_acquire(static_cast<uint64_t>(i));
        EXPECT_TRUE(d.allowed);
        EXPECT_EQ(0u, d.delay_ms);
    }

    // enabled == false is also a passthrough even when a rate is configured.
    rate_limit_config off;
    off.enabled = false;
    off.requests_per_min = 60;
    off.burst = 1;
    rate_limiter disabled(off);
    for (int i = 0; i < 10; ++i)
    {
        const rate_decision d = disabled.try_acquire(0);
        EXPECT_TRUE(d.allowed);
        EXPECT_EQ(0u, d.delay_ms);
    }
}

TEST(rasn_rate_limiter, bucket_starts_full_then_paces_to_the_sustained_rate)
{
    rate_limit_config cfg;
    cfg.requests_per_min = 60; // 1 token/sec => 1000ms per token
    cfg.burst = 5;
    cfg.max_wait_ms = 100000; // large: prefer delaying over rejecting
    rate_limiter limiter(cfg);

    // The burst is available immediately at t=0 with no delay.
    for (int i = 0; i < 5; ++i)
    {
        const rate_decision d = limiter.try_acquire(0);
        EXPECT_TRUE(d.allowed);
        EXPECT_EQ(0u, d.delay_ms);
    }
    // The next request must wait ~1 refill period; the one after that ~2.
    const rate_decision sixth = limiter.try_acquire(0);
    EXPECT_TRUE(sixth.allowed);
    EXPECT_EQ(1000u, sixth.delay_ms);
    const rate_decision seventh = limiter.try_acquire(0);
    EXPECT_TRUE(seventh.allowed);
    EXPECT_EQ(2000u, seventh.delay_ms);
}

TEST(rasn_rate_limiter, rejects_when_projected_wait_exceeds_max_wait)
{
    rate_limit_config cfg;
    cfg.requests_per_min = 60; // 1000ms per token
    cfg.burst = 1;
    cfg.max_wait_ms = 500; // shorter than one refill period
    rate_limiter limiter(cfg);

    EXPECT_TRUE(limiter.try_acquire(0).allowed); // spends the single burst token
    const rate_decision rejected = limiter.try_acquire(0);
    EXPECT_FALSE(rejected.allowed); // 1000ms wait > 500ms cap => reject, no delay
    EXPECT_EQ(0u, rejected.delay_ms);
}

TEST(rasn_rate_limiter, tokens_refill_over_time)
{
    rate_limit_config cfg;
    cfg.requests_per_min = 60; // 1000ms per token
    cfg.burst = 1;
    cfg.max_wait_ms = 0; // never delay: empty bucket rejects immediately
    rate_limiter limiter(cfg);

    EXPECT_TRUE(limiter.try_acquire(0).allowed);
    EXPECT_FALSE(limiter.try_acquire(0).allowed); // empty, cannot wait
    // After one refill period a fresh token is available.
    const rate_decision after = limiter.try_acquire(1000);
    EXPECT_TRUE(after.allowed);
    EXPECT_EQ(0u, after.delay_ms);
}

TEST(rasn_rate_limiter, non_monotonic_clock_never_adds_tokens)
{
    rate_limit_config cfg;
    cfg.requests_per_min = 60;
    cfg.burst = 5;
    cfg.max_wait_ms = 0;
    rate_limiter limiter(cfg);

    for (int i = 0; i < 5; ++i)
    {
        EXPECT_TRUE(limiter.try_acquire(10000).allowed);
    }
    // Time going backwards must not refill the bucket.
    EXPECT_FALSE(limiter.try_acquire(5000).allowed);
}

TEST(rasn_rate_limiter, default_burst_is_about_one_second_of_rate)
{
    rate_limit_config cfg;
    cfg.requests_per_min = 600; // 10 tokens/sec => default burst 10
    cfg.burst = 0;
    cfg.max_wait_ms = 0;
    rate_limiter limiter(cfg);

    int admitted = 0;
    for (int i = 0; i < 100; ++i)
    {
        if (limiter.try_acquire(0).allowed)
        {
            ++admitted;
        }
    }
    EXPECT_EQ(10, admitted);
}

TEST(rasn_rate_limiter, registry_isolates_limiters_per_key)
{
    rate_limit_config cfg;
    cfg.requests_per_min = 60;
    cfg.burst = 1;
    cfg.max_wait_ms = 0;
    rate_limiter_registry registry(cfg);

    // Spend provider b's only token; leave a untouched.
    EXPECT_TRUE(registry.get("b").try_acquire(0).allowed);
    registry.get("a");

    const std::vector<rate_limiter_registry::entry> snapshot = registry.snapshot();
    ASSERT_EQ(2u, snapshot.size());
    EXPECT_EQ("a", snapshot[0].key); // ordered by key
    EXPECT_EQ("b", snapshot[1].key);
    EXPECT_EQ(60u, snapshot[0].requests_per_min);
    EXPECT_EQ(1u, snapshot[0].burst);
    EXPECT_GE(snapshot[0].tokens, 1.0); // a is unspent
    EXPECT_LT(snapshot[1].tokens, 1.0); // b spent its token
}

TEST(rasn_rate_limiter, refund_restores_a_consumed_token)
{
    rate_limit_config cfg;
    cfg.requests_per_min = 60; // 1000ms per token
    cfg.burst = 2;
    cfg.max_wait_ms = 0; // empty bucket rejects immediately
    rate_limiter limiter(cfg);

    EXPECT_TRUE(limiter.try_acquire(0).allowed);  // 2 -> 1
    EXPECT_TRUE(limiter.try_acquire(0).allowed);  // 1 -> 0 (this request is short-circuited)
    EXPECT_FALSE(limiter.try_acquire(0).allowed); // bucket empty

    // The breaker short-circuited the second request after its token was taken;
    // refund() returns that token so the fast-fail does not drain the quota.
    limiter.refund();                             // 0 -> 1
    EXPECT_TRUE(limiter.try_acquire(0).allowed);  // succeeds only because of the refund
    EXPECT_FALSE(limiter.try_acquire(0).allowed); // and exactly one was restored, no more
}

TEST(rasn_rate_limiter, refund_never_exceeds_capacity)
{
    rate_limit_config cfg;
    cfg.requests_per_min = 60;
    cfg.burst = 2;
    cfg.max_wait_ms = 0;
    rate_limiter limiter(cfg);

    // Refunds against an already-full bucket must clamp to burst capacity.
    limiter.refund();
    limiter.refund();
    EXPECT_TRUE(limiter.try_acquire(0).allowed);
    EXPECT_TRUE(limiter.try_acquire(0).allowed);
    EXPECT_FALSE(limiter.try_acquire(0).allowed); // capacity respected: only 2

    // Refund on a disabled/unlimited limiter is a harmless no-op (no token taken).
    rate_limit_config off;
    off.requests_per_min = 0; // unlimited => passthrough
    rate_limiter passthrough(off);
    passthrough.refund();
    EXPECT_TRUE(passthrough.try_acquire(0).allowed);
}

TEST(rasn_rate_limiter, backward_clock_holds_high_water_mark_no_premature_refill)
{
    rate_limit_config cfg;
    cfg.requests_per_min = 60; // 1000ms per token
    cfg.burst = 1;
    cfg.max_wait_ms = 0;
    rate_limiter limiter(cfg);

    // Establish the high-water baseline at t=100000 and drain the only token.
    EXPECT_TRUE(limiter.try_acquire(100000).allowed);
    EXPECT_FALSE(limiter.try_acquire(100000).allowed);

    // Clock jumps far backward to t=10000. The baseline must be held at 100000,
    // NOT moved back to 10000 -- otherwise a later reading would measure elapsed
    // time from the stale 10000 baseline and refill prematurely.
    EXPECT_FALSE(limiter.try_acquire(10000).allowed);

    // Creep forward to t=11000: a full refill period past the (would-be) 10000
    // baseline, so the unfixed code would hand out a token here. But 11000 is
    // still below the real 100000 high-water mark, so no token may accrue yet.
    EXPECT_FALSE(limiter.try_acquire(11000).allowed);

    // Only once the clock advances a full period past the high-water mark
    // (100000 + 1000) does a fresh token become available.
    EXPECT_TRUE(limiter.try_acquire(101000).allowed);
}

TEST(rasn_rate_limiter, cost_weight_consumes_multiple_tokens)
{
    // A request with cost N draws N tokens, so the same bucket meters weighted
    // throughput (e.g. estimated LLM tokens) instead of request count. A burst of
    // 10 funds one cost-4 and one cost-6 request, then is empty.
    rate_limit_config cfg;
    cfg.requests_per_min = 60;
    cfg.burst = 10;
    cfg.max_wait_ms = 0; // never delay: empty bucket rejects immediately
    rate_limiter limiter(cfg);

    EXPECT_TRUE(limiter.try_acquire(0, 4.0).allowed);  // 10 -> 6
    EXPECT_TRUE(limiter.try_acquire(0, 6.0).allowed);  // 6 -> 0
    EXPECT_FALSE(limiter.try_acquire(0, 1.0).allowed); // empty
}

TEST(rasn_rate_limiter, refund_returns_the_full_cost_weight)
{
    rate_limit_config cfg;
    cfg.requests_per_min = 60;
    cfg.burst = 10;
    cfg.max_wait_ms = 0;
    rate_limiter limiter(cfg);

    EXPECT_TRUE(limiter.try_acquire(0, 8.0).allowed);  // 10 -> 2
    EXPECT_FALSE(limiter.try_acquire(0, 8.0).allowed); // only 2 left, cannot fund 8
    limiter.refund(8.0);                               // 2 -> 10 (clamped to capacity)
    EXPECT_TRUE(limiter.try_acquire(0, 8.0).allowed);  // funded again by the refund
}

TEST(rasn_rate_limiter, cost_exceeding_capacity_within_wait_is_rejected)
{
    // A single request whose cost exceeds what can accrue within max_wait_ms is
    // rejected outright without draining the bucket: size the burst >= the largest
    // single-request cost you intend to admit.
    rate_limit_config cfg;
    cfg.requests_per_min = 60; // 1 token / 1000ms
    cfg.burst = 5;
    cfg.max_wait_ms = 1000; // at most ~1 extra token can accrue within the wait
    rate_limiter limiter(cfg);

    // Cost 20 needs 15 tokens beyond the full burst of 5 => 15000ms wait, far
    // beyond the 1000ms cap, so it is rejected without reserving anything.
    const rate_decision rejected = limiter.try_acquire(0, 20.0);
    EXPECT_FALSE(rejected.allowed);
    EXPECT_EQ(0u, rejected.delay_ms);
    // The bucket was not drained, so a normal cost-1 request still passes.
    EXPECT_TRUE(limiter.try_acquire(0, 1.0).allowed);
}

TEST(rasn_rate_limiter, non_positive_cost_is_free_passthrough)
{
    rate_limit_config cfg;
    cfg.requests_per_min = 60;
    cfg.burst = 1;
    cfg.max_wait_ms = 0;
    rate_limiter limiter(cfg);

    EXPECT_TRUE(limiter.try_acquire(0, 1.0).allowed);  // drain the only token
    EXPECT_FALSE(limiter.try_acquire(0, 1.0).allowed); // empty
    EXPECT_TRUE(limiter.try_acquire(0, 0.0).allowed);  // zero cost => free passthrough
    EXPECT_TRUE(limiter.try_acquire(0, -5.0).allowed); // negative cost => free passthrough
}

TEST(rasn_rate_limiter, oversized_weighted_wait_rejects_without_overflow)
{
    rate_limit_config cfg;
    cfg.requests_per_min = 1;
    cfg.burst = 1;
    cfg.max_wait_ms = (std::numeric_limits<uint32_t>::max)();
    rate_limiter limiter(cfg);

    EXPECT_TRUE(limiter.try_acquire(0).allowed); // drain the initial token
    const rate_decision rejected = limiter.try_acquire(0, 1.0e300);
    EXPECT_FALSE(rejected.allowed);
    EXPECT_EQ(0u, rejected.delay_ms);
}

TEST(rasn_rate_limiter, nonfinite_weighted_wait_rejects_without_overflow)
{
    rate_limit_config cfg;
    cfg.requests_per_min = 1;
    cfg.burst = 1;
    cfg.max_wait_ms = (std::numeric_limits<uint32_t>::max)();
    rate_limiter limiter(cfg);

    EXPECT_TRUE(limiter.try_acquire(0).allowed); // drain the initial token
    const rate_decision rejected = limiter.try_acquire(0, std::numeric_limits<double>::infinity());
    EXPECT_FALSE(rejected.allowed);
    EXPECT_EQ(0u, rejected.delay_ms);
}

TEST(rasn_model_cost, estimate_scales_with_prompt_and_floors_at_one_token)
{
    model_cost_config cfg; // defaults: chars_per_token = 4, completion_percent = 150

    // An empty prompt still costs at least one token so every request draws budget.
    EXPECT_DOUBLE_EQ(1.0, estimate_prompt_cost_tokens(0, cfg));
    // 400 chars => ceil(400/4) = 100 input tokens; +50% completion => 150 tokens.
    EXPECT_DOUBLE_EQ(150.0, estimate_prompt_cost_tokens(400, cfg));
    // ceil rounds a partial input token up: 401 chars => ceil(401/4) = 101 => 151.5.
    EXPECT_DOUBLE_EQ(151.5, estimate_prompt_cost_tokens(401, cfg));
}

TEST(rasn_model_cost, estimate_honors_chars_per_token_and_completion_percent)
{
    model_cost_config cfg;
    cfg.chars_per_token = 2;      // finer granularity than the default 4
    cfg.completion_percent = 100; // charge input tokens only (no completion add)

    // 10 chars => ceil(10/2) = 5 input tokens; 100% => 5 total.
    EXPECT_DOUBLE_EQ(5.0, estimate_prompt_cost_tokens(10, cfg));
    // 200% doubles the estimate to model a longer completion.
    cfg.completion_percent = 200;
    EXPECT_DOUBLE_EQ(10.0, estimate_prompt_cost_tokens(10, cfg));
}

TEST(rasn_model_cost, to_rate_limit_config_maps_token_budget_onto_the_bucket)
{
    model_cost_config cfg;
    cfg.enabled = true;
    cfg.tokens_per_min = 12000;
    cfg.burst_tokens = 3000;
    cfg.max_wait_ms = 750;

    const rate_limit_config bucket = to_rate_limit_config(cfg);
    EXPECT_TRUE(bucket.enabled);
    EXPECT_EQ(12000u, bucket.requests_per_min); // tokens/min carried as the refill rate
    EXPECT_EQ(3000u, bucket.burst);             // burst tokens as the bucket capacity
    EXPECT_EQ(750u, bucket.max_wait_ms);
}

TEST(rasn_model_cost, token_count_diagnostics_saturate_oversized_estimates)
{
    EXPECT_EQ(0u, saturating_estimated_token_count(0.0));
    EXPECT_EQ(2u, saturating_estimated_token_count(1.1));
    EXPECT_EQ((std::numeric_limits<uint32_t>::max)(), saturating_estimated_token_count(1.0e300));
    EXPECT_EQ((std::numeric_limits<uint32_t>::max)(),
              saturating_estimated_token_count(std::numeric_limits<double>::infinity()));
    EXPECT_EQ((std::numeric_limits<uint32_t>::max)(),
              saturating_estimated_token_count(-std::numeric_limits<double>::infinity()));

    model_cost_config cfg;
    cfg.chars_per_token = 1;
    cfg.completion_percent = (std::numeric_limits<uint32_t>::max)();
    const double estimate = estimate_prompt_cost_tokens((std::numeric_limits<uint32_t>::max)(), cfg);
    EXPECT_EQ((std::numeric_limits<uint32_t>::max)(), saturating_estimated_token_count(estimate));
}

TEST(rasn_runtime, module_names_cover_all_rasn_runtime_modules)
{
    const std::vector<std::string> names = rasn_runtime_module_names();
    EXPECT_EQ(11u, names.size());
    const std::vector<std::string> expected = {"agent_control_plane",
                                               "agent_message_bus",
                                               "task_orchestration_kernel",
                                               "determinism_ledger",
                                               "capability_directory",
                                               "resource_budget",
                                               "recovery_supervisor",
                                               "blackboard",
                                               "contract_verifier",
                                               "human_interaction",
                                               "sandbox_runtime"};
    for (const std::string &module : expected)
    {
        EXPECT_NE(std::find(names.begin(), names.end(), module), names.end()) << module;
    }
}

TEST(rasn_runtime, app_role_maps_aliases_to_standalone_roles)
{
    EXPECT_EQ("rasn.runtime.budget", rasn_runtime_module_app_role("resource_budget"));
    EXPECT_EQ("rasn.runtime.budget", rasn_runtime_module_app_role("budget"));
    EXPECT_EQ("rasn.runtime.budget", rasn_runtime_module_app_role("rasn.runtime.budget"));
    EXPECT_EQ("rasn.runtime.blackboard", rasn_runtime_module_app_role("blackboard"));
    EXPECT_EQ("rasn.runtime.task_kernel", rasn_runtime_module_app_role("task_orchestration_kernel"));
    EXPECT_EQ("rasn.runtime.task_kernel", rasn_runtime_module_app_role("task_orchestration"));
    EXPECT_EQ("rasn.runtime.agent_control", rasn_runtime_module_app_role("agent_control_plane"));
    EXPECT_EQ("rasn.runtime", rasn_runtime_module_app_role("modules"));
    // Backward-compatible aliases from the pre-rename common runtime surface.
    EXPECT_EQ("rasn.runtime.budget", rasn_runtime_module_app_role("rasn.common.budget"));
    EXPECT_EQ("rasn.runtime", rasn_runtime_module_app_role("rasn.common.modules"));
    // Case-insensitive and whitespace tolerant.
    EXPECT_EQ("rasn.runtime.budget", rasn_runtime_module_app_role("  Resource_Budget "));
    // Unknown roles map to empty so callers can pass them through unchanged.
    EXPECT_EQ("", rasn_runtime_module_app_role("rasn.codepilot"));
    EXPECT_EQ("", rasn_runtime_module_app_role("not_a_module"));
}

TEST(rasn_runtime, normalize_app_list_rewrites_modules_and_preserves_overrides)
{
    // Bare module names become standalone roles.
    EXPECT_EQ("rasn.runtime.budget", normalize_rasn_runtime_app_list("resource_budget"));
    // Mixed known/unknown tokens: unknown passed through, separators normalized to ';'.
    EXPECT_EQ("rasn.runtime.budget;rasn.codepilot", normalize_rasn_runtime_app_list("budget, rasn.codepilot"));
    // '@instance' suffixes are preserved on the rewritten role.
    EXPECT_EQ("rasn.runtime.blackboard@3", normalize_rasn_runtime_app_list("blackboard@3"));
    // Multiple modules across ';' and ',' separators.
    EXPECT_EQ("rasn.runtime.budget;rasn.runtime.blackboard",
              normalize_rasn_runtime_app_list("resource_budget;blackboard"));
    // Old common-runtime roles normalize to the new runtime roles.
    EXPECT_EQ("rasn.runtime.budget;rasn.runtime",
              normalize_rasn_runtime_app_list("rasn.common.budget, rasn.common.modules"));
    // Empty tokens are dropped.
    EXPECT_EQ("rasn.runtime.budget", normalize_rasn_runtime_app_list(";resource_budget;"));
}

TEST(rasn_runtime, host_app_validation_rejects_nonstartable_selectors)
{
    const auto app = [](const std::string &name, bool run, int count) {
        rasn_runtime_host_app_spec spec;
        spec.name = name;
        spec.run = run;
        spec.count = count;
        return spec;
    };
    const std::vector<rasn_runtime_host_app_spec> apps = {
        app("rasn.registry", true, 1),
        app("rasn.state", false, 1),
        app("rasn.coordinator", true, 0),
        app("rasn.runtime", true, 1),
        app("rasn.runtime.blackboard", true, 2)};

    size_t matched = 42;
    std::vector<std::string> invalid;
    std::vector<std::string> unstartable =
        rasn_runtime_unstartable_host_apps(apps, "rasn.nonexistant_app", &matched, &invalid);
    EXPECT_EQ(0u, matched);
    EXPECT_EQ((std::vector<std::string>{"rasn.nonexistant_app"}), unstartable);
    EXPECT_TRUE(invalid.empty());

    matched = 0;
    unstartable = rasn_runtime_unstartable_host_apps(
        apps, "rasn.registry;rasn.runtime.blackboard@2", &matched, &invalid);
    EXPECT_EQ(2u, matched);
    EXPECT_TRUE(unstartable.empty());
    EXPECT_TRUE(invalid.empty());

    matched = 0;
    unstartable = rasn_runtime_unstartable_host_apps(
        apps,
        "rasn.runtime;rasn.state;rasn.coordinator;rasn.runtime.blackboard@3",
        &matched,
        &invalid);
    EXPECT_EQ(1u, matched);
    EXPECT_EQ((std::vector<std::string>{
                  "rasn.state", "rasn.coordinator", "rasn.runtime.blackboard@3"}),
              unstartable);
    EXPECT_TRUE(invalid.empty());

    matched = 0;
    unstartable =
        rasn_runtime_unstartable_host_apps(apps, "rasn.runtime@not-an-index", &matched, &invalid);
    EXPECT_EQ(0u, matched);
    EXPECT_TRUE(unstartable.empty());
    EXPECT_EQ((std::vector<std::string>{"rasn.runtime@not-an-index"}), invalid);

    matched = 0;
    unstartable = rasn_runtime_unstartable_host_apps(
        apps, "rasn.runtime@2;rasn.runtime@1", &matched, &invalid);
    EXPECT_EQ(0u, matched);
    EXPECT_EQ((std::vector<std::string>{"rasn.runtime@2", "rasn.runtime@1"}), unstartable);
    EXPECT_TRUE(invalid.empty());

    matched = 0;
    unstartable =
        rasn_runtime_unstartable_host_apps(apps, "rasn.runtime.blackboard", &matched, &invalid);
    EXPECT_EQ(2u, matched);
    EXPECT_TRUE(unstartable.empty());
    EXPECT_TRUE(invalid.empty());
}

TEST(rasn_runtime, host_app_validation_uses_effective_run_and_count)
{
    const std::string config_path = temp_file_path("rasn-runtime-host-app-list.ini");
    std::remove(config_path.c_str());
    write_text_file(config_path,
                    "[core]\n"
                    "enable_default_app_mimic = true\n"
                    "\n"
                    "[apps..default]\n"
                    "run = true\n"
                    "count = 2\n"
                    "\n"
                    "[apps.enabled]\n"
                    "\n"
                    "[apps.disabled]\n"
                    "run = false\n"
                    "\n"
                    "[apps.single]\n"
                    "count = 1\n");

    const rasn_runtime_host_app_list_check check =
        rasn_runtime_check_host_app_list(config_path, "mimic@2;enabled@2;disabled;single@2");
    EXPECT_TRUE(check.config_loaded);
    EXPECT_EQ(2u, check.matched);
    EXPECT_EQ((std::vector<std::string>{"disabled", "single@2"}), check.unstartable);
    EXPECT_TRUE(check.invalid.empty());
    EXPECT_NE(check.apps.end(),
              std::find_if(check.apps.begin(),
                           check.apps.end(),
                           [](const rasn_runtime_host_app_spec &app) {
                               return app.name == "mimic" && app.run && app.count == 2;
                           }));

    std::remove(config_path.c_str());
}

TEST(rasn_runtime, dispatch_requires_module_and_operation)
{
    rasn_runtime_request missing_module;
    missing_module.operation = "describe";
    EXPECT_FALSE(dispatch_rasn_runtime_request(missing_module).ok);

    rasn_runtime_request missing_operation;
    missing_operation.module = "resource_budget";
    EXPECT_FALSE(dispatch_rasn_runtime_request(missing_operation).ok);
}

TEST(rasn_runtime, dispatch_pings_and_describes_every_module)
{
    for (const std::string &module : rasn_runtime_module_names())
    {
        rasn_runtime_request ping;
        ping.module = module;
        ping.operation = "ping";
        const rasn_runtime_response ping_response = dispatch_rasn_runtime_request(ping);
        EXPECT_TRUE(ping_response.ok) << "ping " << module << ": " << ping_response.error;

        rasn_runtime_request describe;
        describe.module = module;
        describe.operation = "describe";
        const rasn_runtime_response describe_response = dispatch_rasn_runtime_request(describe);
        EXPECT_TRUE(describe_response.ok) << "describe " << module << ": " << describe_response.error;
    }
}

TEST(rasn_runtime, dispatch_rejects_unknown_module_and_operation)
{
    rasn_runtime_request unknown_module;
    unknown_module.module = "not_a_module";
    unknown_module.operation = "ping";
    EXPECT_FALSE(dispatch_rasn_runtime_request(unknown_module).ok);

    rasn_runtime_request unknown_operation;
    unknown_operation.module = "resource_budget";
    unknown_operation.operation = "not_an_operation";
    EXPECT_FALSE(dispatch_rasn_runtime_request(unknown_operation).ok);
}

TEST(rasn_runtime, dispatch_accepts_state_mirror_operations)
{
    rasn_runtime_request mirror;
    mirror.module = "blackboard";
    mirror.operation = "mirror_state:entry";
    mirror.key = "runtime-key";
    mirror.payload = "runtime-value";
    EXPECT_TRUE(dispatch_rasn_runtime_request(mirror).ok);
}

TEST(rasn_runtime, compacts_state_mirror_after_watermark_verification)
{
    const std::string prefix = "unit/runtime-compact-ok";
    const std::string checkpoint_path = temp_file_path("rasn-runtime-compact-ok.chkpt");
    std::remove(checkpoint_path.c_str());
    std::remove((checkpoint_path + ".tmp").c_str());
    std::remove((checkpoint_path + ".bak").c_str());

    state_record mirror;
    mirror.key = prefix + "/blackboard/entry/unit-key";
    mirror.kind = "rasn.runtime.blackboard.entry";
    mirror.scope = "rasn.runtime";
    mirror.value = "value=5:hello\n";
    const state_response stored = global_state_store().put(mirror);
    ASSERT_TRUE(stored.ok) << stored.error;

    const auto field = [](const std::string &key, const std::string &value) {
        return key + "=" + std::to_string(value.size()) + ":" + value + "\n";
    };
    state_record watermark;
    watermark.key = prefix + "/blackboard/_meta/watermark";
    watermark.kind = "rasn.runtime.blackboard.watermark";
    watermark.scope = "rasn.runtime";
    watermark.value = field("schema_version", std::to_string(RASN_AGENT_SCHEMA_VERSION)) +
                      field("module", "blackboard") +
                      field("state_prefix", prefix) +
                      field("last_record_sequence", std::to_string(stored.record.sequence)) +
                      field("last_state_sequence", std::to_string(stored.last_sequence)) +
                      field("updated_at_ms", "1");
    ASSERT_TRUE(global_state_store().put(watermark).ok);

    rasn_service_graph services;
    const rasn_runtime_state_compaction_report report =
        compact_rasn_runtime_state_mirror(services, checkpoint_path, prefix);
    ASSERT_TRUE(report.ok) << report.error;
    EXPECT_EQ(prefix, report.state_prefix);
    EXPECT_EQ(1u, report.runtime_records);
    EXPECT_EQ(1u, report.watermark_records);
    EXPECT_GE(report.checkpointed_records, report.runtime_records + report.watermark_records);
    EXPECT_GE(report.last_sequence, stored.record.sequence);
    EXPECT_TRUE(report.compaction_details_available);
    EXPECT_FALSE(report.recovery_journal_compacted);

    std::remove(checkpoint_path.c_str());
    std::remove((checkpoint_path + ".tmp").c_str());
    std::remove((checkpoint_path + ".bak").c_str());
}

TEST(rasn_runtime, compact_state_mirror_rejects_torn_watermark)
{
    const std::string prefix = "unit/runtime-compact-torn";
    state_record mirror;
    mirror.key = prefix + "/blackboard/entry/unit-key";
    mirror.kind = "rasn.runtime.blackboard.entry";
    mirror.scope = "rasn.runtime";
    mirror.value = "value=5:hello\n";
    const state_response stored = global_state_store().put(mirror);
    ASSERT_TRUE(stored.ok) << stored.error;

    const auto field = [](const std::string &key, const std::string &value) {
        return key + "=" + std::to_string(value.size()) + ":" + value + "\n";
    };
    state_record watermark;
    watermark.key = prefix + "/blackboard/_meta/watermark";
    watermark.kind = "rasn.runtime.blackboard.watermark";
    watermark.scope = "rasn.runtime";
    watermark.value = field("schema_version", std::to_string(RASN_AGENT_SCHEMA_VERSION)) +
                      field("module", "blackboard") +
                      field("state_prefix", prefix) +
                      field("last_record_sequence", std::to_string(stored.record.sequence + 100)) +
                      field("last_state_sequence", std::to_string(stored.record.sequence + 100)) +
                      field("updated_at_ms", "1");
    ASSERT_TRUE(global_state_store().put(watermark).ok);

    rasn_service_graph services;
    const rasn_runtime_state_compaction_report report =
        compact_rasn_runtime_state_mirror(services, "", prefix);
    EXPECT_FALSE(report.ok);
    EXPECT_NE(std::string::npos, report.error.find("behind watermark"));
}

TEST(rasn_runtime, module_descriptors_cover_every_module_with_role_and_consistency)
{
    const std::vector<rasn_runtime_descriptor> descriptors = rasn_runtime_module_descriptors();
    const std::vector<std::string> names = rasn_runtime_module_names();
    EXPECT_EQ(11u, descriptors.size());
    ASSERT_EQ(names.size(), descriptors.size());

    for (const rasn_runtime_descriptor &descriptor : descriptors)
    {
        // Every descriptor names a real module and resolves to its standalone role.
        EXPECT_NE(std::find(names.begin(), names.end(), descriptor.name), names.end()) << descriptor.name;
        EXPECT_EQ(rasn_runtime_module_app_role(descriptor.name), descriptor.role) << descriptor.name;
        EXPECT_FALSE(descriptor.role.empty()) << descriptor.name;
        EXPECT_TRUE(descriptor.stateful) << descriptor.name;
        EXPECT_FALSE(descriptor.summary.empty()) << descriptor.name;
        // Consistency is one of the three intended distribution strategies.
        EXPECT_TRUE(descriptor.consistency == "replicated" || descriptor.consistency == "sharded" ||
                    descriptor.consistency == "singleton")
            << descriptor.name << " -> " << descriptor.consistency;
    }

    // Descriptors and module names describe exactly the same set (one descriptor per module).
    for (const std::string &name : names)
    {
        const std::ptrdiff_t matches = std::count_if(
            descriptors.begin(), descriptors.end(), [&name](const rasn_runtime_descriptor &descriptor) {
                return descriptor.name == name;
            });
        EXPECT_EQ(1, static_cast<int>(matches)) << name;
    }
}

TEST(rasn_runtime, request_marshalling_round_trips_request_metadata)
{
    rasn_runtime_request request;
    request.module = "resource_budget";
    request.operation = "reserve";
    request.key = "scope-a";
    request.payload = "amount=5";
    request.request_id = "idem-1234";
    request.route_partition = 7;
    request.auth_token = "shared-runtime-token";
    request.trace_id = "trace-abc-9f2";

    ::dsn::binary_writer writer;
    marshall(writer, request, DSF_THRIFT_BINARY);
    ::dsn::binary_reader reader(writer.get_buffer());

    rasn_runtime_request decoded;
    unmarshall(reader, decoded, DSF_THRIFT_BINARY);

    EXPECT_EQ(request.schema_version, decoded.schema_version);
    EXPECT_EQ(request.module, decoded.module);
    EXPECT_EQ(request.operation, decoded.operation);
    EXPECT_EQ(request.key, decoded.key);
    EXPECT_EQ(request.payload, decoded.payload);
    // The idempotency id survives the wire round-trip so a retry reuses it.
    EXPECT_EQ(request.request_id, decoded.request_id);
    EXPECT_EQ(request.route_partition, decoded.route_partition);
    EXPECT_EQ(request.auth_token, decoded.auth_token);
    // The end-to-end trace id survives so a module request stays correlated.
    EXPECT_EQ(request.trace_id, decoded.trace_id);

    ::dsn::binary_writer legacy_writer;
    legacy_writer.write(request.schema_version);
    legacy_writer.write(request.module);
    legacy_writer.write(request.operation);
    legacy_writer.write(request.key);
    legacy_writer.write(request.payload);
    ::dsn::binary_reader legacy_reader(legacy_writer.get_buffer());

    rasn_runtime_request legacy_decoded;
    unmarshall(legacy_reader, legacy_decoded, DSF_THRIFT_BINARY);
    EXPECT_EQ(request.module, legacy_decoded.module);
    EXPECT_TRUE(legacy_decoded.request_id.empty());
    EXPECT_EQ((std::numeric_limits<uint32_t>::max)(), legacy_decoded.route_partition);
    EXPECT_TRUE(legacy_decoded.auth_token.empty());
    EXPECT_TRUE(legacy_decoded.trace_id.empty());
}

TEST(rasn_runtime, response_marshalling_round_trips_trace_id)
{
    rasn_runtime_response response;
    response.module = "blackboard";
    response.operation = "get";
    response.ok = true;
    response.route_partition = 3;
    response.trace_id = "trace-resp-77";

    ::dsn::binary_writer writer;
    marshall(writer, response, DSF_THRIFT_BINARY);
    ::dsn::binary_reader reader(writer.get_buffer());
    rasn_runtime_response decoded;
    unmarshall(reader, decoded, DSF_THRIFT_BINARY);
    EXPECT_EQ(response.route_partition, decoded.route_partition);
    EXPECT_EQ(response.trace_id, decoded.trace_id);

    // A legacy peer that predates trace_id (encodes only through route_partition)
    // still decodes cleanly, leaving the trace id empty.
    ::dsn::binary_writer legacy_writer;
    legacy_writer.write(response.schema_version);
    legacy_writer.write(response.ok);
    legacy_writer.write(response.error);
    legacy_writer.write(response.module);
    legacy_writer.write(response.operation);
    legacy_writer.write(response.key);
    legacy_writer.write(response.payload);
    legacy_writer.write(response.route_partition);
    ::dsn::binary_reader legacy_reader(legacy_writer.get_buffer());
    rasn_runtime_response legacy_decoded;
    unmarshall(legacy_reader, legacy_decoded, DSF_THRIFT_BINARY);
    EXPECT_EQ(response.route_partition, legacy_decoded.route_partition);
    EXPECT_TRUE(legacy_decoded.trace_id.empty());
}

TEST(rasn_runtime, trace_scope_sets_and_restores_ambient_trace_id)
{
    EXPECT_TRUE(current_rasn_runtime_trace_id().empty());
    {
        rasn_runtime_trace_scope outer("trace-outer");
        EXPECT_EQ("trace-outer", current_rasn_runtime_trace_id());
        {
            rasn_runtime_trace_scope inner("trace-inner");
            EXPECT_EQ("trace-inner", current_rasn_runtime_trace_id());
            {
                // An empty id is a no-op so callers can install unconditionally.
                rasn_runtime_trace_scope noop("");
                EXPECT_EQ("trace-inner", current_rasn_runtime_trace_id());
            }
            EXPECT_EQ("trace-inner", current_rasn_runtime_trace_id());
        }
        EXPECT_EQ("trace-outer", current_rasn_runtime_trace_id());
    }
    EXPECT_TRUE(current_rasn_runtime_trace_id().empty());
}

TEST(rasn_runtime, dispatch_echoes_request_trace_id_onto_response)
{
    // dispatch runs the central response builder for both success and error paths,
    // so the caller's trace id is echoed regardless of the module's verdict.
    rasn_runtime_request request;
    request.module = "determinism_ledger";
    request.operation = "ping";
    request.trace_id = "trace-dispatch-echo";
    const rasn_runtime_response response = dispatch_rasn_runtime_request(request);
    EXPECT_EQ(request.trace_id, response.trace_id);
}

TEST(rasn_runtime, dispatch_dedups_repeated_request_signatures)
{
    const auto encode_field = [](const std::string &key, const std::string &value) {
        return key + "=" + std::to_string(value.size()) + ":" + value + "\n";
    };
    const auto count_substring = [](const std::string &text, const std::string &needle) {
        size_t count = 0;
        size_t offset = 0;
        while ((offset = text.find(needle, offset)) != std::string::npos)
        {
            ++count;
            offset += needle.size();
        }
        return count;
    };
    const std::string task_id = "dedup-task-mutating";
    const std::string payload = encode_field("task_id", task_id) + encode_field("key", "route") +
                                encode_field("source", "test") + encode_field("value", "first");

    // The first mutating request installs an in-flight placeholder, applies once,
    // and stores the response for the same logical retry signature.
    rasn_runtime_request first;
    first.module = "determinism_ledger";
    first.operation = "record";
    first.key = task_id + "/route";
    first.payload = payload;
    first.request_id = "dedup-req-alpha";
    const rasn_runtime_response first_response = dispatch_rasn_runtime_request(first);
    ASSERT_TRUE(first_response.ok) << first_response.error;
    EXPECT_EQ(first.key, first_response.key);

    const rasn_runtime_response duplicate_response = dispatch_rasn_runtime_request(first);
    ASSERT_TRUE(duplicate_response.ok) << duplicate_response.error;
    EXPECT_EQ(first_response.payload, duplicate_response.payload);

    rasn_runtime_request snapshot;
    snapshot.module = "determinism_ledger";
    snapshot.operation = "snapshot";
    const rasn_runtime_response snapshot_response = dispatch_rasn_runtime_request(snapshot);
    ASSERT_TRUE(snapshot_response.ok) << snapshot_response.error;
    EXPECT_EQ(1u, count_substring(snapshot_response.payload, task_id));

    // A retry carrying the same id but a different key/payload is a distinct
    // signature, avoiding stale responses after an accidental id collision.
    rasn_runtime_request retry;
    retry.module = "determinism_ledger";
    retry.operation = "record";
    retry.key = task_id + "/other";
    retry.payload = encode_field("task_id", task_id) + encode_field("key", "other") +
                    encode_field("source", "test") + encode_field("value", "second");
    retry.request_id = "dedup-req-alpha";
    const rasn_runtime_response retry_response = dispatch_rasn_runtime_request(retry);
    EXPECT_TRUE(retry_response.ok) << retry_response.error;
    EXPECT_EQ(retry.key, retry_response.key);

    // A different id for the same module is routed fresh (not deduped).
    rasn_runtime_request other_id;
    other_id.module = "determinism_ledger";
    other_id.operation = "record";
    other_id.key = task_id + "/third";
    other_id.payload = encode_field("task_id", task_id) + encode_field("key", "third") +
                       encode_field("source", "test") + encode_field("value", "third");
    other_id.request_id = "dedup-req-beta";
    const rasn_runtime_response other_id_response = dispatch_rasn_runtime_request(other_id);
    EXPECT_TRUE(other_id_response.ok) << other_id_response.error;
    EXPECT_EQ(other_id.key, other_id_response.key);

    // Read-only operations bypass the mutating-operation dedup cache even with ids.
    rasn_runtime_request read_a;
    read_a.module = "blackboard";
    read_a.operation = "describe";
    read_a.key = "read-key-a";
    read_a.request_id = "dedup-read-id";
    const rasn_runtime_response read_a_response = dispatch_rasn_runtime_request(read_a);
    EXPECT_EQ("read-key-a", read_a_response.key);

    rasn_runtime_request read_b = read_a;
    read_b.key = "read-key-b";
    const rasn_runtime_response read_b_response = dispatch_rasn_runtime_request(read_b);
    EXPECT_EQ("read-key-b", read_b_response.key);

    // Requests without an id are never deduped even if otherwise identical.
    rasn_runtime_request no_id_a;
    no_id_a.module = "blackboard";
    no_id_a.operation = "describe";
    no_id_a.key = "no-id-key-a";
    const rasn_runtime_response no_id_a_response = dispatch_rasn_runtime_request(no_id_a);
    EXPECT_EQ("no-id-key-a", no_id_a_response.key);

    rasn_runtime_request no_id_b = no_id_a;
    no_id_b.key = "no-id-key-b";
    const rasn_runtime_response no_id_b_response = dispatch_rasn_runtime_request(no_id_b);
    EXPECT_EQ("no-id-key-b", no_id_b_response.key);
}

TEST(rasn_runtime, replicated_requests_use_parallel_read_and_write_codes)
{
    rasn_runtime_request read;
    read.module = "determinism_ledger";
    read.operation = "snapshot";
    EXPECT_EQ(RPC_RASN_DETERMINISM_LEDGER, rasn_runtime_rpc_code_for_request(read));

    rasn_runtime_request write = read;
    write.operation = "record";
    EXPECT_EQ(RPC_RASN_DETERMINISM_LEDGER_WRITE, rasn_runtime_rpc_code_for_request(write));
    EXPECT_NE(rasn_runtime_rpc_code_for_request(read), rasn_runtime_rpc_code_for_request(write));
}

TEST(rasn_runtime, replica_checkpoint_restores_module_state_and_dedup)
{
    ::dsn_gpid gpid = {};
    gpid.u.app_id = 1;
    gpid.u.partition_index = 0;
    const int64_t decree = 42;
    const auto encode_field = [](const std::string &key, const std::string &value) {
        return key + "=" + std::to_string(value.size()) + ":" + value + "\n";
    };
    const auto count_substring = [](const std::string &text, const std::string &needle) {
        size_t count = 0;
        size_t offset = 0;
        while ((offset = text.find(needle, offset)) != std::string::npos)
        {
            ++count;
            offset += needle.size();
        }
        return count;
    };

    rasn_runtime_replica_store source("determinism_ledger");
    EXPECT_GT(source.dedup_capacity(), 0u);
    rasn_runtime_request record;
    record.module = "determinism_ledger";
    record.operation = "record";
    record.key = "replica-task/route";
    record.payload = encode_field("task_id", "replica-task") + encode_field("key", "route") +
                     encode_field("source", "unit") + encode_field("value", "primary");
    record.request_id = "replica-dedup-1";
    const rasn_runtime_response first = source.dispatch(record);
    ASSERT_TRUE(first.ok) << first.error;
    EXPECT_EQ(first.payload, source.dispatch(record).payload);
    rasn_runtime_request second = record;
    second.payload = encode_field("task_id", "replica-task") + encode_field("key", "route") +
                     encode_field("source", "unit") + encode_field("value", "secondary");
    second.request_id = "replica-dedup-2";
    ASSERT_TRUE(source.dispatch(second).ok);

    std::vector<state_record> records;
    std::string error;
    ASSERT_TRUE(source.checkpoint_records(gpid, decree, &records, &error)) << error;
    ASSERT_FALSE(records.empty());

    rasn_runtime_replica_store restored("determinism_ledger");
    ASSERT_TRUE(restored.validate_checkpoint_records(records, gpid, decree, &error)) << error;
    ::dsn_gpid wrong_partition = gpid;
    wrong_partition.u.partition_index = 1;
    EXPECT_FALSE(restored.validate_checkpoint_records(records, wrong_partition, decree, &error));
    EXPECT_FALSE(restored.validate_checkpoint_records(records, gpid, decree + 1, &error));
    rasn_runtime_replica_store wrong_module("blackboard");
    EXPECT_FALSE(wrong_module.validate_checkpoint_records(records, gpid, decree, &error));

    rasn_runtime_request snapshot;
    snapshot.module = "determinism_ledger";
    snapshot.operation = "snapshot";
    EXPECT_EQ(0u, count_substring(restored.dispatch(snapshot).payload, "replica-task"));

    ASSERT_TRUE(restored.replace_checkpoint_records(records, gpid, decree, &error)) << error;
    EXPECT_EQ(2u, count_substring(restored.dispatch(snapshot).payload, "replica-task"));
    EXPECT_EQ(first.payload, restored.dispatch(record).payload);
    EXPECT_EQ(2u, count_substring(restored.dispatch(snapshot).payload, "replica-task"));

    bool corrupted = false;
    for (state_record &checkpoint_record : records)
    {
        if (checkpoint_record.kind == "rasn.runtime.determinism_ledger.dedup")
        {
            checkpoint_record.value = "corrupt";
            corrupted = true;
            break;
        }
    }
    ASSERT_TRUE(corrupted);
    EXPECT_FALSE(restored.validate_checkpoint_records(records, gpid, decree, &error));
}

TEST(rasn_runtime, replica_rejects_nondeterministic_mutations)
{
    rasn_runtime_replica_store store("blackboard");
    rasn_runtime_request request;
    request.module = "blackboard";
    request.operation = "put";
    request.key = "missing-timestamp";
    request.request_id = "replica-nondeterministic-1";
    request.payload = "schema_version=1:1\nkey=17:missing-timestamp\n";
    const rasn_runtime_response response = store.dispatch(request);
    EXPECT_FALSE(response.ok);
    EXPECT_NE(std::string::npos, response.error.find("timestamp"));

    request.key = "envelope-key";
    request.request_id = "replica-mismatched-key";
    request.payload = "key=11:payload-key\nupdated_at_ms=1:1\n";
    const rasn_runtime_response mismatched = store.dispatch(request);
    EXPECT_FALSE(mismatched.ok);
    EXPECT_NE(std::string::npos, mismatched.error.find("request key"));

    rasn_runtime_replica_store human_store("human_interaction");
    rasn_runtime_request expire;
    expire.module = "human_interaction";
    expire.operation = "expire";
    expire.key = "*";
    expire.request_id = "replica-missing-expiry-time";
    expire.payload = "now_ms=1:0\n";
    const rasn_runtime_response missing_expiry_time = human_store.dispatch(expire);
    EXPECT_FALSE(missing_expiry_time.ok);
    EXPECT_NE(std::string::npos, missing_expiry_time.error.find("timestamp"));
}

TEST(rasn_runtime, replica_human_interaction_accepts_live_mutations_and_restores)
{
    const auto encode_field = [](const std::string &key, const std::string &value) {
        return key + "=" + std::to_string(value.size()) + ":" + value + "\n";
    };
    const auto open_request = [&encode_field](const std::string &human_id,
                                              const std::string &command_id,
                                              uint64_t deadline_ms) {
        rasn_runtime_request request;
        request.module = "human_interaction";
        request.operation = "open";
        request.key = human_id;
        request.request_id = command_id;
        request.payload = encode_field("request_id", human_id) +
                          encode_field("requester", "reviewer") +
                          encode_field("prompt", "Approve deployment?") +
                          encode_field("choice", "yes") +
                          encode_field("choice", "no") +
                          encode_field("state", "pending") +
                          encode_field("created_at_ms", "100") +
                          encode_field("updated_at_ms", "100") +
                          encode_field("deadline_ms", std::to_string(deadline_ms));
        return request;
    };
    const auto transition_request = [&encode_field](const std::string &human_id,
                                                    const std::string &operation,
                                                    const std::string &value_field,
                                                    const std::string &value,
                                                    const std::string &command_id,
                                                    uint64_t updated_at_ms) {
        rasn_runtime_request request;
        request.module = "human_interaction";
        request.operation = operation;
        request.key = human_id;
        request.request_id = command_id;
        request.payload = encode_field("request_id", human_id) +
                          encode_field(value_field, value) +
                          encode_field("updated_at_ms", std::to_string(updated_at_ms));
        return request;
    };
    const auto find_request = [](const std::string &human_id) {
        rasn_runtime_request request;
        request.module = "human_interaction";
        request.operation = "find";
        request.key = human_id;
        return request;
    };

    rasn_runtime_replica_store source("human_interaction");
    ASSERT_TRUE(source.dispatch(open_request("human-answer", "open-answer", 500)).ok);
    const rasn_runtime_request answer =
        transition_request("human-answer", "answer", "answer", "yes", "answer-command", 120);
    ASSERT_TRUE(source.dispatch(answer).ok);
    EXPECT_NE(std::string::npos,
              source.dispatch(find_request("human-answer")).payload.find("answered"));

    ASSERT_TRUE(source.dispatch(open_request("human-cancel", "open-cancel", 500)).ok);
    ASSERT_TRUE(source.dispatch(transition_request("human-cancel",
                                                   "cancel",
                                                   "reason",
                                                   "superseded",
                                                   "cancel-command",
                                                   130))
                    .ok);
    EXPECT_NE(std::string::npos,
              source.dispatch(find_request("human-cancel")).payload.find("cancelled"));

    ASSERT_TRUE(source.dispatch(open_request("human-expire", "open-expire", 150)).ok);
    rasn_runtime_request expire;
    expire.module = "human_interaction";
    expire.operation = "expire";
    expire.key = "*";
    expire.request_id = "expire-command";
    expire.payload = encode_field("now_ms", "200");
    ASSERT_TRUE(source.dispatch(expire).ok);
    EXPECT_NE(std::string::npos,
              source.dispatch(find_request("human-expire")).payload.find("expired"));

    ::dsn_gpid gpid = {};
    gpid.u.app_id = 11;
    gpid.u.partition_index = 0;
    const int64_t decree = 17;
    std::vector<state_record> records;
    std::string error;
    ASSERT_TRUE(source.checkpoint_records(gpid, decree, &records, &error)) << error;

    rasn_runtime_replica_store restored("human_interaction");
    ASSERT_TRUE(restored.replace_checkpoint_records(records, gpid, decree, &error)) << error;
    EXPECT_NE(std::string::npos,
              restored.dispatch(find_request("human-answer")).payload.find("answered"));
    EXPECT_NE(std::string::npos,
              restored.dispatch(find_request("human-cancel")).payload.find("cancelled"));
    EXPECT_NE(std::string::npos,
              restored.dispatch(find_request("human-expire")).payload.find("expired"));
    EXPECT_EQ(source.dispatch(answer).payload, restored.dispatch(answer).payload);
}

TEST(rasn_runtime, replica_dedup_preserves_failed_mutation_outcome)
{
    const auto encode_field = [](const std::string &key, const std::string &value) {
        return key + "=" + std::to_string(value.size()) + ":" + value + "\n";
    };
    rasn_runtime_replica_store store("task_orchestration_kernel");

    rasn_runtime_request complete;
    complete.module = "task_orchestration_kernel";
    complete.operation = "complete";
    complete.key = "late-task";
    complete.payload = "result";
    complete.request_id = "complete-before-add";
    const rasn_runtime_response first = store.dispatch(complete);
    ASSERT_FALSE(first.ok);

    rasn_runtime_request add;
    add.module = "task_orchestration_kernel";
    add.operation = "add_task";
    add.key = "late-task";
    add.payload = encode_field("task_id", "late-task") + encode_field("state", "pending");
    add.request_id = "add-late-task";
    ASSERT_TRUE(store.dispatch(add).ok);

    const rasn_runtime_response retried = store.dispatch(complete);
    EXPECT_FALSE(retried.ok);
    EXPECT_EQ(first.error, retried.error);
}

TEST(rasn_runtime, ownership_resources_expand_modules_and_shards)
{
    // No hosted shards are configured in the unit-test host and the sharded modules
    // default to a single partition, so each module maps to exactly one
    // module-level ownership resource, in the given order.
    const std::vector<std::string> resources =
        rasn_runtime_module_ownership_resources({"determinism_ledger", "blackboard"});
    ASSERT_EQ(2u, resources.size());
    EXPECT_EQ("rasn.runtime.determinism_ledger", resources[0]);
    EXPECT_EQ("rasn.runtime.blackboard", resources[1]);

    // Every resource is namespaced under the runtime module capability prefix so
    // it never collides with unrelated coordination locks.
    for (const std::string &resource : resources)
    {
        EXPECT_EQ(0u, resource.find("rasn.runtime."));
    }

    // No modules means nothing to own (the gate opens handlers immediately).
    EXPECT_TRUE(rasn_runtime_module_ownership_resources({}).empty());
}

TEST(rasn_runtime, ownership_resources_whole_sharded_module_locks_every_shard)
{
    // An unsharded module takes exactly one module-level lock.
    EXPECT_EQ(std::vector<std::string>({"rasn.runtime.determinism_ledger"}),
              rasn_runtime_module_ownership_resources_for(
                  "determinism_ledger", {}, /*sharded=*/false, /*partition_count=*/1));

    // A sharded module pinned to a single partition is likewise one module-level lock.
    EXPECT_EQ(std::vector<std::string>({"rasn.runtime.blackboard"}),
              rasn_runtime_module_ownership_resources_for(
                  "blackboard", {}, /*sharded=*/true, /*partition_count=*/1));

    // A service that hosts an explicit shard subset locks exactly those shards.
    EXPECT_EQ(std::vector<std::string>({"rasn.runtime.blackboard.shard.0",
                                        "rasn.runtime.blackboard.shard.2"}),
              rasn_runtime_module_ownership_resources_for(
                  "blackboard", {0, 2}, /*sharded=*/true, /*partition_count=*/4));

    // A whole-module host of a sharded module (no explicit hosted subset) must lock
    // EVERY shard rather than the unqualified module resource. Otherwise it would
    // not contend with a peer that locks rasn.runtime.blackboard.shard.N, and both
    // processes would serve shard N -- the split brain this expansion guards against.
    EXPECT_EQ(std::vector<std::string>({"rasn.runtime.blackboard.shard.0",
                                        "rasn.runtime.blackboard.shard.1",
                                        "rasn.runtime.blackboard.shard.2"}),
              rasn_runtime_module_ownership_resources_for(
                  "blackboard", {}, /*sharded=*/true, /*partition_count=*/3));
}

TEST(rasn_runtime, partition_hash_preserves_keys_and_explicit_shard_routes)
{
    rasn_runtime_request keyed;
    keyed.module = "blackboard";
    keyed.key = "topic";
    EXPECT_EQ(5912253781582851172ULL, rasn_runtime_partition_hash(keyed));

    keyed.route_partition = 7;
    EXPECT_EQ(7u, rasn_runtime_partition_hash(keyed));

    rasn_runtime_request empty_shard_key;
    empty_shard_key.module = "blackboard";
    EXPECT_EQ(14695981039346656037ULL,
              rasn_runtime_partition_hash(empty_shard_key));

    rasn_runtime_request unkeyed_control;
    unkeyed_control.module = "agent_control_plane";
    EXPECT_EQ(0u, rasn_runtime_partition_hash(unkeyed_control));

    rasn_runtime_request expire_human;
    expire_human.module = "human_interaction";
    expire_human.operation = "expire";
    expire_human.key = "*";
    EXPECT_TRUE(rasn_runtime_request_is_partition_fanout(expire_human));
    expire_human.route_partition = 3;
    EXPECT_EQ(3u, rasn_runtime_partition_hash(expire_human));

    rasn_runtime_request open_human = expire_human;
    open_human.operation = "open";
    EXPECT_FALSE(rasn_runtime_request_is_partition_fanout(open_human));

    rasn_runtime_request human_snapshot = expire_human;
    human_snapshot.operation = "snapshot";
    EXPECT_FALSE(rasn_runtime_request_is_partition_fanout(human_snapshot));
}

TEST(rasn_runtime, ingress_guard_admits_hosted_shards_and_rejects_others)
{
    rasn_runtime_request request;
    request.module = "agent_message_bus";
    request.operation = "publish";
    request.key = "topic";

    // An empty hosted set means the service owns the whole module, so every
    // request is admitted regardless of the routing hint it carries.
    request.route_partition = 5;
    EXPECT_TRUE(rasn_runtime_service_hosts_request(request, {}));

    // With no shard_count configured in the unit-test host the module is
    // single-partition, so requests resolve to shard 0. A service that hosts shard
    // 0 admits them; one that hosts only other shards rejects them.
    EXPECT_TRUE(rasn_runtime_service_hosts_request(request, {0}));
    EXPECT_TRUE(rasn_runtime_service_hosts_request(request, {0, 2}));
    EXPECT_FALSE(rasn_runtime_service_hosts_request(request, {1}));
    EXPECT_FALSE(rasn_runtime_service_hosts_request(request, {1, 2, 3}));

    // The guard keys off the resolved partition, not the module name, so an
    // unsharded module (always partition 0) is admitted only by a set covering 0.
    rasn_runtime_request unsharded;
    unsharded.module = "agent_control_plane";
    unsharded.operation = "describe";
    EXPECT_TRUE(rasn_runtime_service_hosts_request(unsharded, {}));
    EXPECT_TRUE(rasn_runtime_service_hosts_request(unsharded, {0}));
    EXPECT_FALSE(rasn_runtime_service_hosts_request(unsharded, {4}));
}

// --- parse_chat_completion: provider response interpretation -----------------
// Regression coverage for the empty-"content" fallback that used to surface the
// raw JSON body as the model's answer (llm_provider.cpp). See rASN robustness
// testing against reasoning models (empty content) and provider error envelopes.

TEST(rasn_llm_provider, chat_completion_extracts_content)
{
    const std::string body =
        "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"hello world\"}}]}";
    const chat_completion_parse parsed = parse_chat_completion(body, "openai.chat");
    EXPECT_TRUE(parsed.ok);
    EXPECT_EQ("hello world", parsed.text);
}

TEST(rasn_llm_provider, effective_timeout_matches_network_provider_default)
{
    EXPECT_EQ(120000u, effective_llm_request_timeout_ms(0));
    EXPECT_EQ(1234u, effective_llm_request_timeout_ms(1234));
    EXPECT_EQ(120000u, effective_llm_request_timeout_ms(180000));
}

TEST(rasn_llm_provider, chat_completion_reasoning_content_fallback)
{
    // Reasoning models (gemma, DeepSeek-R1, o1-style) can leave "content" empty
    // and place the answer in "reasoning_content". That text must be surfaced,
    // NOT the raw JSON envelope.
    const std::string body =
        "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"\","
        "\"reasoning_content\":\"the real answer\"}}]}";
    const chat_completion_parse parsed = parse_chat_completion(body, "openai.chat");
    EXPECT_TRUE(parsed.ok);
    EXPECT_EQ("the real answer", parsed.text);
    // The raw JSON blob must never leak through as the completion.
    EXPECT_EQ(std::string::npos, parsed.text.find("reasoning_content"));
}

TEST(rasn_llm_provider, chat_completion_error_envelope_is_failure)
{
    // An HTTP-200 body carrying an OpenAI-style error envelope must be reported
    // as a failure -- not returned as a successful completion -- so provider
    // errors are not masked or persisted downstream as real output.
    const std::string body =
        "{\"error\":{\"message\":\"model not found\",\"type\":\"invalid_request_error\"}}";
    const chat_completion_parse parsed = parse_chat_completion(body, "openai.chat");
    EXPECT_FALSE(parsed.ok);
    EXPECT_EQ("model not found", parsed.error_detail);
    EXPECT_TRUE(parsed.text.empty());
}

TEST(rasn_llm_provider, chat_completion_flat_error_string_is_failure)
{
    // Ollama-style providers report failures as a flat {"error":"..."} string with
    // no nested "message"; that provider error text must be surfaced instead of the
    // generic "empty completion" fallback (review finding 2).
    const std::string body = "{\"error\":\"model 'gemma' not found, try pulling it first\"}";
    const chat_completion_parse parsed = parse_chat_completion(body, "ollama.generate");
    EXPECT_FALSE(parsed.ok);
    EXPECT_EQ("model 'gemma' not found, try pulling it first", parsed.error_detail);
    EXPECT_TRUE(parsed.text.empty());
}

TEST(rasn_llm_provider, chat_completion_empty_message_is_failure)
{
    // A well-formed JSON response with a genuinely empty completion is a failure,
    // not a success that emits the raw JSON.
    const std::string body =
        "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"\"}}]}";
    const chat_completion_parse parsed = parse_chat_completion(body, "openai.chat");
    EXPECT_FALSE(parsed.ok);
    EXPECT_FALSE(parsed.error_detail.empty());
    EXPECT_TRUE(parsed.text.empty());
}

TEST(rasn_llm_provider, chat_completion_non_json_passthrough)
{
    // Unknown/plain-text providers keep the historical best-effort passthrough.
    const std::string body = "plain text answer";
    const chat_completion_parse parsed = parse_chat_completion(body, "openai.chat");
    EXPECT_TRUE(parsed.ok);
    EXPECT_EQ("plain text answer", parsed.text);
}

TEST(rasn_llm_provider, chat_completion_ollama_generate_response_field)
{
    // ollama.generate responses carry the text in "response", and the
    // reasoning_content fallback must not apply to that payload format.
    const std::string body = "{\"response\":\"ollama answer\",\"done\":true}";
    const chat_completion_parse parsed = parse_chat_completion(body, "ollama.generate");
    EXPECT_TRUE(parsed.ok);
    EXPECT_EQ("ollama answer", parsed.text);
}

} // namespace
} // namespace rasn
} // namespace dsn
