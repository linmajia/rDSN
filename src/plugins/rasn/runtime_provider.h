#pragma once

#include <rasn/agent_control_plane.h>
#include <rasn/agent_message_bus.h>
#include <rasn/agent_registry.h>
#include <rasn/blackboard.h>
#include <rasn/capability_directory.h>
#include <rasn/contract_verifier.h>
#include <rasn/determinism_ledger.h>
#include <rasn/human_interaction.h>
#include <rasn/recovery_supervisor.h>
#include <rasn/resource_budget.h>
#include <rasn/sandbox_runtime.h>
#include <rasn/task_orchestration.h>

#include <dsn/service_api_cpp.h>

#include <chrono>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dsn {
namespace rasn {

class rasn_service_graph;

struct rasn_runtime_request
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    std::string module;
    std::string operation;
    std::string key;
    std::string payload;
    // Optional idempotency id. When set, the service store returns the cached
    // response for a repeated id instead of re-applying the operation, so a
    // client-side retry after a lost reply does not double-apply a write.
    std::string request_id;
    // Optional client-side routing hint used by distributed providers for
    // shard fan-out reads. Runtime module handlers ignore this value.
    uint32_t route_partition = (std::numeric_limits<uint32_t>::max)();
    // Optional shared-token credential for cross-node runtime module RPC. Local
    // and LPC paths leave this empty; RPC services verify it only when auth is
    // enabled in [rasn.service].
    std::string auth_token;
    // Optional end-to-end trace id propagated from the originating operation so a
    // module request can be correlated across nodes in logs and metrics. Stamped
    // from the ambient trace scope by make_module_request; empty for untraced calls.
    std::string trace_id;
};

struct rasn_runtime_response
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    bool ok = true;
    std::string error;
    std::string module;
    std::string operation;
    std::string key;
    std::string payload;
    uint32_t route_partition = (std::numeric_limits<uint32_t>::max)();
    // Echoes the request trace id so a caller can correlate the reply in logs.
    std::string trace_id;
};

inline void marshall(::dsn::binary_writer &writer, const rasn_runtime_request &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.module);
    writer.write(value.operation);
    writer.write(value.key);
    writer.write(value.payload);
    writer.write(value.request_id);
    writer.write(value.route_partition);
    writer.write(value.auth_token);
    writer.write(value.trace_id);
}

inline void unmarshall(::dsn::binary_reader &reader, rasn_runtime_request &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.module);
    reader.read(value.operation);
    reader.read(value.key);
    reader.read(value.payload);
    if (!reader.is_eof())
    {
        reader.read(value.request_id);
    }
    else
    {
        value.request_id.clear();
    }
    if (!reader.is_eof())
    {
        reader.read(value.route_partition);
    }
    else
    {
        value.route_partition = (std::numeric_limits<uint32_t>::max)();
    }
    if (!reader.is_eof())
    {
        reader.read(value.auth_token);
    }
    else
    {
        value.auth_token.clear();
    }
    if (!reader.is_eof())
    {
        reader.read(value.trace_id);
    }
    else
    {
        value.trace_id.clear();
    }
}

inline void marshall(::dsn::binary_writer &writer, const rasn_runtime_response &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.ok);
    writer.write(value.error);
    writer.write(value.module);
    writer.write(value.operation);
    writer.write(value.key);
    writer.write(value.payload);
    writer.write(value.route_partition);
    writer.write(value.trace_id);
}

inline void unmarshall(::dsn::binary_reader &reader, rasn_runtime_response &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.ok);
    reader.read(value.error);
    reader.read(value.module);
    reader.read(value.operation);
    reader.read(value.key);
    reader.read(value.payload);
    if (!reader.is_eof())
    {
        reader.read(value.route_partition);
    }
    else
    {
        value.route_partition = (std::numeric_limits<uint32_t>::max)();
    }
    if (!reader.is_eof())
    {
        reader.read(value.trace_id);
    }
    else
    {
        value.trace_id.clear();
    }
}

// Ambient end-to-end trace propagation for runtime module RPC.
//
// make_module_request stamps the request envelope with the trace id currently in
// scope on this thread, so a logical operation's trace flows onto every module
// request it triggers without threading the id through dozens of call sites. An
// operation origin (e.g. the coordinator/service-graph invoke) or the server RPC
// ingress installs a scope for the duration of the call; nested module calls and
// the echoed response then share the id. An empty id leaves the ambient unchanged.
std::string current_rasn_runtime_trace_id();

class rasn_runtime_trace_scope
{
public:
    explicit rasn_runtime_trace_scope(const std::string &trace_id);
    ~rasn_runtime_trace_scope();

    rasn_runtime_trace_scope(const rasn_runtime_trace_scope &) = delete;
    rasn_runtime_trace_scope &operator=(const rasn_runtime_trace_scope &) = delete;

private:
    std::string _previous;
    bool _changed;
};

struct rasn_runtime_config
{
    std::string provider = "local";
    std::string state_prefix = "rasn/runtime";
    bool strict = false;
};

struct rasn_runtime_state_compaction_report
{
    bool ok = true;
    std::string error;
    std::string state_prefix;
    std::string checkpoint_path;
    size_t queried_records = 0;
    size_t runtime_records = 0;
    size_t watermark_records = 0;
    size_t checkpointed_records = 0;
    uint64_t last_sequence = 0;
};

// Static description of a rASN runtime module: its app-facing API name, the
// standalone service role that can host it on its own node, the intended
// distributed consistency model for its state, and whether it owns durable state.
// The consistency model documents the target replication strategy (leveraging
// rDSN-native replication); the current in-memory service store realizes it as a
// single-writer singleton per service.
struct rasn_runtime_descriptor
{
    std::string name;
    std::string role;
    std::string consistency;
    bool stateful = true;
    std::string summary;
};

class rasn_runtime_provider;

class rasn_runtime
{
public:
    explicit rasn_runtime(std::unique_ptr<rasn_runtime_provider> provider);
    ~rasn_runtime();

    std::string provider_name() const;
    bool distributed() const;
    bool strict() const;
    const std::string &state_prefix() const;
    std::string summary_header() const;

    bool upsert_agent(const agent_control_record &record, std::string *error);
    agent_control_lease acquire_agent_lease(const std::string &agent_id,
                                            const std::string &owner,
                                            uint64_t now_ms,
                                            uint64_t lease_ms);
    bool heartbeat_agent(const std::string &agent_id, uint64_t now_ms, std::string *error);
    bool find_agent(const std::string &agent_id, agent_control_record *record) const;
    size_t expire_agent_leases(uint64_t now_ms);
    std::vector<agent_control_record> list_agents(bool include_expired, uint64_t now_ms) const;
    std::string describe_agents(uint64_t now_ms) const;

    bool publish_message(const agent_message &message, agent_message *stored, std::string *error);
    bool ack_message(const std::string &message_id, std::string *error);
    bool dead_letter_message(const std::string &message_id, const std::string &error_text, std::string *error);
    bool find_message(const std::string &message_id, agent_message *message) const;
    std::vector<agent_message> message_snapshot() const;

    bool add_task(const orchestration_task &task, std::string *error);
    bool start_task(const std::string &task_id, const std::string &owner_agent, std::string *error);
    bool complete_task(const std::string &task_id, const std::string &output, std::string *error);
    bool fail_task(const std::string &task_id, const std::string &error_text, bool retryable, std::string *error);
    bool find_task(const std::string &task_id, orchestration_task *task) const;
    std::vector<orchestration_task> task_snapshot() const;
    std::vector<orchestration_task> ready_tasks(uint64_t now_ms) const;
    std::vector<orchestration_task> blocked_tasks() const;

    bool record_choice(const std::string &task_id,
                       const std::string &key,
                       const std::string &source,
                       const std::string &value,
                       deterministic_choice *choice,
                       std::string *error);
    std::vector<deterministic_choice> choice_snapshot() const;

    bool upsert_capability_provider(const capability_provider &provider, std::string *error);
    std::string describe_capabilities() const;
    bool configure_budget(const resource_quota &quota, std::string *error);
    resource_budget_decision reserve_budget(const resource_request &request);
    bool release_budget(const resource_request &request, std::string *error);
    bool budget_usage(const std::string &scope, resource_usage *usage) const;
    std::string describe_budgets() const;
    bool set_recovery_policy(const recovery_policy &policy, std::string *error);
    recovery_action observe_failure(const failure_observation &failure);
    std::string describe_recovery() const;
    bool put_blackboard(const blackboard_entry &entry, blackboard_entry *stored, std::string *error);
    bool get_blackboard(const std::string &key, blackboard_entry *entry) const;
    std::vector<blackboard_entry> blackboard_snapshot(bool include_expired, uint64_t now_ms) const;
    bool register_contract(const agent_contract &contract, std::string *error);
    contract_evaluation evaluate_input(const std::string &contract_id, const std::string &input) const;
    contract_evaluation evaluate_output(const std::string &contract_id,
                                        const std::string &output,
                                        const std::vector<std::string> &policy_labels) const;
    std::string describe_contracts() const;
    std::vector<human_interaction_request> human_snapshot() const;
    std::vector<human_interaction_request> pending_human() const;
    void set_sandbox_profile(const sandbox_profile &profile);
    sandbox_decision evaluate_sandbox(const sandbox_request &request) const;
    sandbox_profile sandbox() const;

    bool mirror_state(const std::string &module,
                      const std::string &kind,
                      const std::string &key,
                      const std::string &value,
                      std::string *error);

    bool ping_module(const std::string &module, std::string *error) const;
    std::vector<std::pair<std::string, bool>> module_health() const;
    std::string describe_module_health() const;
    std::string describe_topology() const;

private:
    std::unique_ptr<rasn_runtime_provider> _provider;
};

class rasn_runtime_provider
{
public:
    explicit rasn_runtime_provider(rasn_runtime_config config);
    virtual ~rasn_runtime_provider() {}

    virtual std::string provider_name() const = 0;
    virtual bool distributed() const = 0;
    bool strict() const { return _config.strict; }
    const std::string &state_prefix() const { return _config.state_prefix; }
    std::string summary_header() const;

    void set_sandbox_profile(const sandbox_profile &profile);
    sandbox_decision evaluate_sandbox(const sandbox_request &request) const;
    sandbox_profile sandbox() const;

    bool upsert_agent(const agent_control_record &record, std::string *error);
    agent_control_lease acquire_agent_lease(const std::string &agent_id,
                                            const std::string &owner,
                                            uint64_t now_ms,
                                            uint64_t lease_ms);
    bool heartbeat_agent(const std::string &agent_id, uint64_t now_ms, std::string *error);
    bool find_agent(const std::string &agent_id, agent_control_record *record) const;
    size_t expire_agent_leases(uint64_t now_ms);
    std::vector<agent_control_record> list_agents(bool include_expired, uint64_t now_ms) const;
    std::string describe_agents(uint64_t now_ms) const;

    bool publish_message(const agent_message &message, agent_message *stored, std::string *error);
    bool ack_message(const std::string &message_id, std::string *error);
    bool dead_letter_message(const std::string &message_id, const std::string &error_text, std::string *error);
    bool find_message(const std::string &message_id, agent_message *message) const;
    std::vector<agent_message> message_snapshot() const;

    bool add_task(const orchestration_task &task, std::string *error);
    bool start_task(const std::string &task_id, const std::string &owner_agent, std::string *error);
    bool complete_task(const std::string &task_id, const std::string &output, std::string *error);
    bool fail_task(const std::string &task_id, const std::string &error_text, bool retryable, std::string *error);
    bool find_task(const std::string &task_id, orchestration_task *task) const;
    std::vector<orchestration_task> task_snapshot() const;
    std::vector<orchestration_task> ready_tasks(uint64_t now_ms) const;
    std::vector<orchestration_task> blocked_tasks() const;

    bool record_choice(const std::string &task_id,
                       const std::string &key,
                       const std::string &source,
                       const std::string &value,
                       deterministic_choice *choice,
                       std::string *error);
    std::vector<deterministic_choice> choice_snapshot() const;

    bool upsert_capability_provider(const capability_provider &provider, std::string *error);
    std::string describe_capabilities() const;
    bool configure_budget(const resource_quota &quota, std::string *error);
    resource_budget_decision reserve_budget(const resource_request &request);
    bool release_budget(const resource_request &request, std::string *error);
    bool budget_usage(const std::string &scope, resource_usage *usage) const;
    std::string describe_budgets() const;
    bool set_recovery_policy(const recovery_policy &policy, std::string *error);
    recovery_action observe_failure(const failure_observation &failure);
    std::string describe_recovery() const;
    bool put_blackboard(const blackboard_entry &entry, blackboard_entry *stored, std::string *error);
    bool get_blackboard(const std::string &key, blackboard_entry *entry) const;
    std::vector<blackboard_entry> blackboard_snapshot(bool include_expired, uint64_t now_ms) const;
    bool register_contract(const agent_contract &contract, std::string *error);
    contract_evaluation evaluate_input(const std::string &contract_id, const std::string &input) const;
    contract_evaluation evaluate_output(const std::string &contract_id,
                                        const std::string &output,
                                        const std::vector<std::string> &policy_labels) const;
    std::string describe_contracts() const;
    std::vector<human_interaction_request> human_snapshot() const;
    std::vector<human_interaction_request> pending_human() const;

    bool mirror_state(const std::string &module,
                      const std::string &kind,
                      const std::string &key,
                      const std::string &value,
                      std::string *error);

    virtual bool ping_module(const std::string &module, std::string *error) const;
    std::string describe_topology() const;

protected:
    virtual rasn_runtime_response call_module_api(const rasn_runtime_request &request) const = 0;
    virtual bool write_state(const std::string &module,
                             const std::string &kind,
                             const std::string &key,
                             const std::string &value,
                             std::string *error) = 0;
    // Topology hooks: the base renders describe_topology() from these so a per-module
    // (hybrid) provider can report where each module is actually routed. Defaults
    // describe a uniform local/remote placement.
    virtual bool module_routed_remote(const std::string &module) const
    {
        (void)module;
        return distributed();
    }
    virtual std::string module_endpoint(const std::string &module) const
    {
        (void)module;
        return "in-process";
    }
    std::vector<rasn_runtime_response> call_module_api_shards(const rasn_runtime_request &request) const;
    bool mirror_state_after_success(const std::string &module,
                                    const std::string &kind,
                                    const std::string &key,
                                    const std::string &value,
                                    std::string *error = nullptr);
    std::string state_key(const std::string &module, const std::string &kind, const std::string &key) const;

private:
    rasn_runtime_config _config;
};

rasn_runtime_config load_rasn_runtime_config();
std::unique_ptr<rasn_runtime> create_rasn_runtime(rasn_service_graph &services, const rasn_runtime_config &config);
rasn_runtime_response dispatch_rasn_runtime_request(const rasn_runtime_request &request);
std::vector<std::string> rasn_runtime_module_names();
std::vector<rasn_runtime_descriptor> rasn_runtime_module_descriptors();
std::string rasn_runtime_module_app_role(const std::string &module_or_role);
std::string normalize_rasn_runtime_app_list(const std::string &app_list);
rasn_runtime_state_compaction_report compact_rasn_runtime_state_mirror(rasn_service_graph &services,
                                                                       const std::string &checkpoint_path = "",
                                                                       const std::string &state_prefix = "");
void register_rasn_runtime_apps();

class rasn_runtime_rpc_service : public ::dsn::serverlet<rasn_runtime_rpc_service>
{
public:
    explicit rasn_runtime_rpc_service(std::vector<std::string> modules = std::vector<std::string>());
    void open_service();
    void close_service();
    const std::vector<std::string> &modules() const { return _modules; }

protected:
    void on_agent_control(const rasn_runtime_request &request, ::dsn::rpc_replier<rasn_runtime_response> &reply);
    void on_message_bus(const rasn_runtime_request &request, ::dsn::rpc_replier<rasn_runtime_response> &reply);
    void on_task_orchestration(const rasn_runtime_request &request, ::dsn::rpc_replier<rasn_runtime_response> &reply);
    void on_determinism_ledger(const rasn_runtime_request &request, ::dsn::rpc_replier<rasn_runtime_response> &reply);
    void on_capability_directory(const rasn_runtime_request &request, ::dsn::rpc_replier<rasn_runtime_response> &reply);
    void on_resource_budget(const rasn_runtime_request &request, ::dsn::rpc_replier<rasn_runtime_response> &reply);
    void on_recovery_supervisor(const rasn_runtime_request &request, ::dsn::rpc_replier<rasn_runtime_response> &reply);
    void on_blackboard(const rasn_runtime_request &request, ::dsn::rpc_replier<rasn_runtime_response> &reply);
    void on_contract_verifier(const rasn_runtime_request &request, ::dsn::rpc_replier<rasn_runtime_response> &reply);
    void on_human_interaction(const rasn_runtime_request &request, ::dsn::rpc_replier<rasn_runtime_response> &reply);
    void on_sandbox_runtime(const rasn_runtime_request &request, ::dsn::rpc_replier<rasn_runtime_response> &reply);

private:
    bool register_module_handler(const std::string &module);
    void unregister_module_handler(const std::string &module);
    void reply_module_request(const std::string &module,
                              const rasn_runtime_request &request,
                              ::dsn::rpc_replier<rasn_runtime_response> &reply);

    std::vector<std::string> _modules;
};

class rasn_runtime_client : public virtual ::dsn::clientlet
{
public:
    explicit rasn_runtime_client(::dsn::rpc_address server) : _server(server) {}
    std::pair<::dsn::error_code, rasn_runtime_response>
    call_sync(const rasn_runtime_request &request,
              std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
              int thread_hash = 0,
              uint64_t partition_hash = 0);

private:
    ::dsn::rpc_address _server;
};

class rasn_runtime_app : public ::dsn::service_app
{
public:
    explicit rasn_runtime_app(::dsn_gpid gpid);
    virtual ::dsn::error_code start(int argc, char **argv);
    virtual ::dsn::error_code stop(bool cleanup = false);

protected:
    rasn_runtime_app(::dsn_gpid gpid, std::vector<std::string> modules);

private:
    ::dsn::error_code hydrate_modules_from_state();
    void register_modules_with_registry();
    void heartbeat_modules_to_registry();
    void unregister_modules_from_registry();
    void start_registry_heartbeat_timer();
    void cancel_registry_heartbeat_timer();

    rasn_runtime_rpc_service _rpc;
    std::vector<agent_descriptor> _registry_descriptors;
    ::dsn::task_ptr _registry_heartbeat_timer;
};

class rasn_agent_control_module_app : public rasn_runtime_app
{
public:
    explicit rasn_agent_control_module_app(::dsn_gpid gpid)
        : rasn_runtime_app(gpid, std::vector<std::string>{"agent_control_plane"})
    {
    }
};

class rasn_message_bus_module_app : public rasn_runtime_app
{
public:
    explicit rasn_message_bus_module_app(::dsn_gpid gpid)
        : rasn_runtime_app(gpid, std::vector<std::string>{"agent_message_bus"})
    {
    }
};

class rasn_task_orchestration_module_app : public rasn_runtime_app
{
public:
    explicit rasn_task_orchestration_module_app(::dsn_gpid gpid)
        : rasn_runtime_app(gpid, std::vector<std::string>{"task_orchestration_kernel"})
    {
    }
};

class rasn_determinism_ledger_module_app : public rasn_runtime_app
{
public:
    explicit rasn_determinism_ledger_module_app(::dsn_gpid gpid)
        : rasn_runtime_app(gpid, std::vector<std::string>{"determinism_ledger"})
    {
    }
};

class rasn_capability_directory_module_app : public rasn_runtime_app
{
public:
    explicit rasn_capability_directory_module_app(::dsn_gpid gpid)
        : rasn_runtime_app(gpid, std::vector<std::string>{"capability_directory"})
    {
    }
};

class rasn_resource_budget_module_app : public rasn_runtime_app
{
public:
    explicit rasn_resource_budget_module_app(::dsn_gpid gpid)
        : rasn_runtime_app(gpid, std::vector<std::string>{"resource_budget"})
    {
    }
};

class rasn_recovery_supervisor_module_app : public rasn_runtime_app
{
public:
    explicit rasn_recovery_supervisor_module_app(::dsn_gpid gpid)
        : rasn_runtime_app(gpid, std::vector<std::string>{"recovery_supervisor"})
    {
    }
};

class rasn_blackboard_module_app : public rasn_runtime_app
{
public:
    explicit rasn_blackboard_module_app(::dsn_gpid gpid)
        : rasn_runtime_app(gpid, std::vector<std::string>{"blackboard"})
    {
    }
};

class rasn_contract_verifier_module_app : public rasn_runtime_app
{
public:
    explicit rasn_contract_verifier_module_app(::dsn_gpid gpid)
        : rasn_runtime_app(gpid, std::vector<std::string>{"contract_verifier"})
    {
    }
};

class rasn_human_interaction_module_app : public rasn_runtime_app
{
public:
    explicit rasn_human_interaction_module_app(::dsn_gpid gpid)
        : rasn_runtime_app(gpid, std::vector<std::string>{"human_interaction"})
    {
    }
};

class rasn_sandbox_runtime_module_app : public rasn_runtime_app
{
public:
    explicit rasn_sandbox_runtime_module_app(::dsn_gpid gpid)
        : rasn_runtime_app(gpid, std::vector<std::string>{"sandbox_runtime"})
    {
    }
};

std::string describe_agent_control_record(const agent_control_record &record);
std::string describe_capability_provider_record(const capability_provider &provider);
std::string describe_resource_quota_record(const resource_quota &quota);
std::string describe_resource_decision_record(const resource_budget_decision &decision);
std::string describe_resource_usage_record(const resource_usage &usage);
std::string describe_recovery_policy_record(const recovery_policy &policy);
std::string describe_failure_observation_record(const failure_observation &failure);
std::string describe_contract_record(const agent_contract &contract);
std::string describe_orchestration_task_record(const orchestration_task &task);
std::string describe_agent_message_record(const agent_message &message);
std::string describe_choice_record(const deterministic_choice &choice);
std::string describe_blackboard_record(const blackboard_entry &entry);

} // namespace rasn
} // namespace dsn
