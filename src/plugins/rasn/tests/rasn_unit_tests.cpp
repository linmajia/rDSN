#include "../agent_types.h"
#include "../agent_messages.h"
#include "../agent_registry.h"
#include "../agent_runtime.h"
#include "../agent_services.h"
#include "../admission_gate.h"
#include "../circuit_breaker.h"
#include "../coordinator_service.h"
#include "../codepilot/local_tools.h"
#include "../metrics.h"
#include "../policy_manager.h"
#include "../rate_limiter.h"
#include "../redaction.h"
#include "../state_service.h"
#include "../schema_manifest.h"
#include "../workflow.h"
#include "../workflow_service.h"

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
#include <memory>
#include <mutex>
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
    EXPECT_NE(std::string::npos, cpp_clients.find("std::pair<::dsn::error_code, state_response>"));

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
    const std::string path = temp_file_path("rasn-codepilot-tools-unit.txt");
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
    const std::string path = temp_file_path("rasn-codepilot-fs-replay.txt");
    const std::string trace_path = temp_file_path("rasn-codepilot-fs-replay.jsonl");
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

    nucleus_runtime runtime;
    agent_task task;
    task.id = "metrics-counter-task";
    task.name = "unit.metrics";
    task.input = "noop";
    runtime.begin_task(task);
    runtime.finish_task(task, "ok");

    const uint64_t begin_after = registry.snapshot().counter("rasn_tasks_begin_total");
    const uint64_t finish_after = registry.snapshot().counter("rasn_tasks_finish_total");

    EXPECT_EQ(begin_before + 1, begin_after);
    EXPECT_EQ(finish_before + 1, finish_after);

    // Observing a latency value must never crash, even though percentiles are
    // computed asynchronously by rDSN counter timers.
    registry.observe_task_latency_ms(5);
    registry.observe_llm_latency_ms(5);
    registry.observe_tool_latency_ms(5);
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
    // A near-UINT32_MAX max_backpressure_ms (read_config_u32 admits up to
    // UINT32_MAX) must not overflow the signed-int delay curve into a negative or
    // garbage value: the resulting delay stays non-negative and bounded by an
    // int-safe ceiling rather than wrapping.
    admission_config cfg;
    cfg.enabled = true;
    cfg.max_concurrency = 10;
    cfg.soft_concurrency = 2;
    cfg.max_backpressure_ms = 0xFFFFFFFFu; // intentionally absurd / out of range
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

} // namespace
} // namespace rasn
} // namespace dsn
