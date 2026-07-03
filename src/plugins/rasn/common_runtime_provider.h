#pragma once

#include <rasn/agent_control_plane.h>
#include <rasn/agent_message_bus.h>
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
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dsn {
namespace rasn {

class rasn_service_graph;

struct common_module_request
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    std::string module;
    std::string operation;
    std::string key;
    std::string payload;
};

struct common_module_response
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    bool ok = true;
    std::string error;
    std::string module;
    std::string operation;
    std::string key;
    std::string payload;
};

inline void marshall(::dsn::binary_writer &writer, const common_module_request &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.module);
    writer.write(value.operation);
    writer.write(value.key);
    writer.write(value.payload);
}

inline void unmarshall(::dsn::binary_reader &reader, common_module_request &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.module);
    reader.read(value.operation);
    reader.read(value.key);
    reader.read(value.payload);
}

inline void marshall(::dsn::binary_writer &writer, const common_module_response &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.ok);
    writer.write(value.error);
    writer.write(value.module);
    writer.write(value.operation);
    writer.write(value.key);
    writer.write(value.payload);
}

inline void unmarshall(::dsn::binary_reader &reader, common_module_response &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.ok);
    reader.read(value.error);
    reader.read(value.module);
    reader.read(value.operation);
    reader.read(value.key);
    reader.read(value.payload);
}

struct common_runtime_config
{
    std::string provider = "local";
    std::string state_prefix = "rasn/runtime/modules";
    bool strict = false;
};

class common_runtime_provider;

class common_runtime
{
public:
    explicit common_runtime(std::unique_ptr<common_runtime_provider> provider);
    ~common_runtime();

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

private:
    std::unique_ptr<common_runtime_provider> _provider;
};

class common_runtime_provider
{
public:
    explicit common_runtime_provider(common_runtime_config config);
    virtual ~common_runtime_provider() {}

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

protected:
    virtual common_module_response call_module_api(const common_module_request &request) const = 0;
    virtual bool write_state(const std::string &module,
                             const std::string &kind,
                             const std::string &key,
                             const std::string &value,
                             std::string *error) = 0;
    std::string state_key(const std::string &module, const std::string &kind, const std::string &key) const;

private:
    common_runtime_config _config;
};

common_runtime_config load_common_runtime_config();
std::unique_ptr<common_runtime> create_common_runtime(rasn_service_graph &services, const common_runtime_config &config);
common_module_response dispatch_common_module_request(const common_module_request &request);
std::vector<std::string> common_runtime_module_names();
std::string common_runtime_module_app_role(const std::string &module_or_role);
std::string normalize_common_runtime_app_list(const std::string &app_list);
void register_rasn_common_module_apps();

class rasn_common_module_rpc_service : public ::dsn::serverlet<rasn_common_module_rpc_service>
{
public:
    explicit rasn_common_module_rpc_service(std::vector<std::string> modules = std::vector<std::string>());
    void open_service();
    void close_service();

protected:
    void on_agent_control(const common_module_request &request, ::dsn::rpc_replier<common_module_response> &reply);
    void on_message_bus(const common_module_request &request, ::dsn::rpc_replier<common_module_response> &reply);
    void on_task_orchestration(const common_module_request &request, ::dsn::rpc_replier<common_module_response> &reply);
    void on_determinism_ledger(const common_module_request &request, ::dsn::rpc_replier<common_module_response> &reply);
    void on_capability_directory(const common_module_request &request, ::dsn::rpc_replier<common_module_response> &reply);
    void on_resource_budget(const common_module_request &request, ::dsn::rpc_replier<common_module_response> &reply);
    void on_recovery_supervisor(const common_module_request &request, ::dsn::rpc_replier<common_module_response> &reply);
    void on_blackboard(const common_module_request &request, ::dsn::rpc_replier<common_module_response> &reply);
    void on_contract_verifier(const common_module_request &request, ::dsn::rpc_replier<common_module_response> &reply);
    void on_human_interaction(const common_module_request &request, ::dsn::rpc_replier<common_module_response> &reply);
    void on_sandbox_runtime(const common_module_request &request, ::dsn::rpc_replier<common_module_response> &reply);

private:
    bool register_module_handler(const std::string &module);
    void unregister_module_handler(const std::string &module);

    std::vector<std::string> _modules;
};

class rasn_common_module_client : public virtual ::dsn::clientlet
{
public:
    explicit rasn_common_module_client(::dsn::rpc_address server) : _server(server) {}
    std::pair<::dsn::error_code, common_module_response>
    call_sync(const common_module_request &request,
              std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
              int thread_hash = 0,
              uint64_t partition_hash = 0);

private:
    ::dsn::rpc_address _server;
};

class rasn_common_module_app : public ::dsn::service_app
{
public:
    explicit rasn_common_module_app(::dsn_gpid gpid);
    virtual ::dsn::error_code start(int argc, char **argv);
    virtual ::dsn::error_code stop(bool cleanup = false);

protected:
    rasn_common_module_app(::dsn_gpid gpid, std::vector<std::string> modules);

private:
    rasn_common_module_rpc_service _rpc;
};

class rasn_agent_control_module_app : public rasn_common_module_app
{
public:
    explicit rasn_agent_control_module_app(::dsn_gpid gpid)
        : rasn_common_module_app(gpid, std::vector<std::string>{"agent_control_plane"})
    {
    }
};

class rasn_message_bus_module_app : public rasn_common_module_app
{
public:
    explicit rasn_message_bus_module_app(::dsn_gpid gpid)
        : rasn_common_module_app(gpid, std::vector<std::string>{"agent_message_bus"})
    {
    }
};

class rasn_task_orchestration_module_app : public rasn_common_module_app
{
public:
    explicit rasn_task_orchestration_module_app(::dsn_gpid gpid)
        : rasn_common_module_app(gpid, std::vector<std::string>{"task_orchestration_kernel"})
    {
    }
};

class rasn_determinism_ledger_module_app : public rasn_common_module_app
{
public:
    explicit rasn_determinism_ledger_module_app(::dsn_gpid gpid)
        : rasn_common_module_app(gpid, std::vector<std::string>{"determinism_ledger"})
    {
    }
};

class rasn_capability_directory_module_app : public rasn_common_module_app
{
public:
    explicit rasn_capability_directory_module_app(::dsn_gpid gpid)
        : rasn_common_module_app(gpid, std::vector<std::string>{"capability_directory"})
    {
    }
};

class rasn_resource_budget_module_app : public rasn_common_module_app
{
public:
    explicit rasn_resource_budget_module_app(::dsn_gpid gpid)
        : rasn_common_module_app(gpid, std::vector<std::string>{"resource_budget"})
    {
    }
};

class rasn_recovery_supervisor_module_app : public rasn_common_module_app
{
public:
    explicit rasn_recovery_supervisor_module_app(::dsn_gpid gpid)
        : rasn_common_module_app(gpid, std::vector<std::string>{"recovery_supervisor"})
    {
    }
};

class rasn_blackboard_module_app : public rasn_common_module_app
{
public:
    explicit rasn_blackboard_module_app(::dsn_gpid gpid)
        : rasn_common_module_app(gpid, std::vector<std::string>{"blackboard"})
    {
    }
};

class rasn_contract_verifier_module_app : public rasn_common_module_app
{
public:
    explicit rasn_contract_verifier_module_app(::dsn_gpid gpid)
        : rasn_common_module_app(gpid, std::vector<std::string>{"contract_verifier"})
    {
    }
};

class rasn_human_interaction_module_app : public rasn_common_module_app
{
public:
    explicit rasn_human_interaction_module_app(::dsn_gpid gpid)
        : rasn_common_module_app(gpid, std::vector<std::string>{"human_interaction"})
    {
    }
};

class rasn_sandbox_runtime_module_app : public rasn_common_module_app
{
public:
    explicit rasn_sandbox_runtime_module_app(::dsn_gpid gpid)
        : rasn_common_module_app(gpid, std::vector<std::string>{"sandbox_runtime"})
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
