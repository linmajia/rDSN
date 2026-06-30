#pragma once

#include "agent_messages.h"
#include "agent_runtime.h"
#include "agent_tools.h"
#include "circuit_breaker.h"
#include "llm_provider.h"
#include "metrics.h"
#include "rasn.code.definition.h"
#include "state_service.h"
#include "workflow_service.h"

#include <dsn/cpp/zlocks.h>
#include <dsn/service_api_cpp.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

class rasn_llm_agent_service : public agent_runtime
{
public:
    rasn_llm_agent_service();

    void start();
    void stop();
    void set_provider(const std::string &provider_name);
    model_gateway_response set_model_provider(const model_provider_request &request);
    model_gateway_response describe_model_provider() const;
    model_gateway_response model_health() const;
    llm_response complete(const agent_completion_request &request, nucleus_runtime &runtime);
    llm_response complete_streaming(const agent_completion_request &request,
                                    nucleus_runtime &runtime,
                                    const llm_stream_callback &on_chunk);
    agent_response invoke(const agent_request &request, nucleus_runtime &runtime);
    std::string summary(const nucleus_runtime &runtime) const;

    // Point-in-time view of every model-provider circuit breaker (for ops
    // commands and the model gateway summary).
    std::vector<circuit_breaker_registry::entry> model_breaker_states() const;

private:
    // Lazily load [rasn.model] circuit-breaker tunables (config reads are
    // null-safe and return defaults before rDSN config is loaded).
    void ensure_model_breaker_config();
    // Returns false and fills *fast_fail when the breaker for the active provider
    // is open and the request must be short-circuited without calling out.
    bool model_breaker_admit(const std::string &provider,
                             const agent_task &task,
                             nucleus_runtime &runtime,
                             llm_response *fast_fail);
    // Report the outcome of an admitted call back to the provider's breaker.
    void model_breaker_report(const std::string &provider,
                              const agent_task &task,
                              nucleus_runtime &runtime,
                              bool ok);
    // True when the active provider should be guarded by a breaker (remote only).
    bool model_breaker_engaged() const;

    std::unique_ptr<llm_provider> _provider;
    circuit_breaker_registry _model_breakers;
    std::once_flag _model_breaker_config_once;
};

class rasn_tool_agent_service : public agent_runtime
{
public:
    rasn_tool_agent_service();

    void start();
    void stop();
    void set_tool_provider(std::unique_ptr<agent_tool_provider> tools);
    std::string describe_tools() const;
    tool_result run_tool(const std::string &name,
                         const std::vector<std::string> &args,
                         nucleus_runtime &runtime,
                         const agent_task &task,
                         const std::vector<std::string> &policy_labels = std::vector<std::string>()) const;
    agent_response invoke(const agent_request &request, nucleus_runtime &runtime) const;

private:
    std::unique_ptr<agent_tool_provider> _tools;
    mutable ::dsn::service::zlock _tool_lock;
};

class rasn_coordinator_service : public agent_runtime
{
public:
    rasn_coordinator_service(rasn_llm_agent_service &llm_agent, rasn_tool_agent_service &tool_agent);

    void start();
    void stop();
    llm_response complete(const agent_completion_request &request, nucleus_runtime &runtime);
    llm_response complete_streaming(const agent_completion_request &request,
                                    nucleus_runtime &runtime,
                                    const llm_stream_callback &on_chunk);
    tool_result run_tool(const std::string &name,
                         const std::vector<std::string> &args,
                         nucleus_runtime &runtime,
                         const agent_task &task);
    agent_response invoke(const agent_request &request, nucleus_runtime &runtime);
    std::string describe_topology() const;

private:
    rasn_llm_agent_service &_llm_agent;
    rasn_tool_agent_service &_tool_agent;
};

class rasn_service_graph
{
public:
    rasn_service_graph();

    void start();
    void stop();
    void acquire();
    void release();
    bool is_started() const;
    uint32_t lifecycle_ref_count() const;

    nucleus_runtime &runtime() { return _runtime; }
    const nucleus_runtime &runtime() const { return _runtime; }

    model_gateway_response set_provider(const std::string &provider_name);
    model_gateway_response model_provider() const;
    model_gateway_response model_health() const;
    std::string provider_summary() const;
    // Per-provider model circuit-breaker states (for ops commands / summaries).
    std::vector<circuit_breaker_registry::entry> model_breaker_states() const;
    // Human-readable per-provider circuit-breaker report (shared by the
    // `rasn.resilience` command and CodePilot's `observe resilience`).
    std::string model_breaker_report() const;
    std::string topology() const;
    std::string tools_summary() const;
    void set_tool_provider(std::unique_ptr<agent_tool_provider> tools);
    void enable_rpc_clients(const ::dsn::rpc_address &registry,
                            const ::dsn::rpc_address &coordinator,
                            const ::dsn::rpc_address &llm_agent,
                            const ::dsn::rpc_address &tool_agent,
                            const ::dsn::rpc_address &state,
                            const ::dsn::rpc_address &workflow,
                            const ::dsn::rpc_address &observability);
    bool rpc_clients_enabled() const { return _rpc_clients_enabled; }
    const ::dsn::rpc_address &registry_address() const { return _registry_address; }
    const ::dsn::rpc_address &coordinator_address() const { return _coordinator_address; }
    const ::dsn::rpc_address &llm_agent_address() const { return _llm_agent_address; }
    const ::dsn::rpc_address &tool_agent_address() const { return _tool_agent_address; }
    const ::dsn::rpc_address &state_address() const { return _state_address; }
    const ::dsn::rpc_address &workflow_address() const { return _workflow_address; }
    const ::dsn::rpc_address &observability_address() const { return _observability_address; }

    llm_response complete(const agent_completion_request &request);
    llm_response complete_streaming(const agent_completion_request &request, const llm_stream_callback &on_chunk);
    tool_result run_tool(const std::string &name,
                         const std::vector<std::string> &args,
                         const agent_task &task,
                         uint32_t timeout_ms = 0);
    agent_response invoke(const agent_request &request);
    state_response put_state(const state_record &record);
    state_response put_state(const state_put_request &request);
    state_response get_state(const state_key_request &request);
    state_response query_state(const state_query_request &request);
    state_response checkpoint_state(const state_checkpoint_request &request);
    state_response recover_state(const state_checkpoint_request &request);
    workflow_response validate_workflow(const workflow_source &source);
    workflow_response compile_workflow(const workflow_source &source);
    workflow_response start_workflow(const workflow_start_request &request);
    workflow_response query_workflow(const workflow_run_query &request);
    workflow_response cancel_workflow(const workflow_run_query &request);
    observability_response query_events(const observability_query_request &request) const;
    observability_response query_failures(const observability_query_request &request) const;
    observability_response load_replay(const replay_load_request &request);
    observability_response observability_snapshot() const;
    metrics_snapshot runtime_metrics() const;

private:
    friend class rasn_llm_agent_rpc_service;
    friend class rasn_tool_agent_rpc_service;
    friend class rasn_coordinator_rpc_service;

    void register_agents_with_registry_rpc();
    void heartbeat_agents_to_registry();
    void unregister_agents_from_registry_rpc();
    void start_registry_heartbeat_timer();
    void cancel_registry_heartbeat_timer();
    void register_ops_commands_once();
    void start_unlocked();
    void stop_unlocked();

    nucleus_runtime _runtime;
    rasn_llm_agent_service _llm_agent;
    rasn_tool_agent_service _tool_agent;
    rasn_coordinator_service _coordinator;
    ::dsn::rpc_address _registry_address;
    ::dsn::rpc_address _coordinator_address;
    ::dsn::rpc_address _llm_agent_address;
    ::dsn::rpc_address _tool_agent_address;
    ::dsn::rpc_address _state_address;
    ::dsn::rpc_address _workflow_address;
    ::dsn::rpc_address _observability_address;
    ::dsn::task_ptr _registry_heartbeat_timer;
    mutable ::dsn::service::zlock _lifecycle_lock;
    uint32_t _lifecycle_ref_count;
    bool _lifecycle_transitioning;
    bool _rpc_clients_enabled;
    bool _started;
};

rasn_service_graph &global_rasn_services();

class rasn_llm_agent_rpc_service : public ::dsn::serverlet<rasn_llm_agent_rpc_service>
{
public:
    rasn_llm_agent_rpc_service() : ::dsn::serverlet<rasn_llm_agent_rpc_service>("rasn.llm.agent") {}
    void open_service();
    void close_service();

protected:
    void on_agent_describe(const std::string &request, ::dsn::rpc_replier<agent_descriptor> &reply);
    void on_agent_invoke(const agent_request &request, ::dsn::rpc_replier<agent_response> &reply);
    void on_agent_cancel(const agent_request &request, ::dsn::rpc_replier<agent_response> &reply);
    void on_agent_heartbeat(const std::string &request, ::dsn::rpc_replier<agent_descriptor> &reply);
    void on_agent_query(const std::string &request, ::dsn::rpc_replier<agent_descriptor> &reply);
    void on_model_describe(const std::string &request, ::dsn::rpc_replier<model_gateway_response> &reply);
    void on_model_set_provider(const model_provider_request &request, ::dsn::rpc_replier<model_gateway_response> &reply);
    void on_model_health(const std::string &request, ::dsn::rpc_replier<model_gateway_response> &reply);
};

class rasn_tool_agent_rpc_service : public ::dsn::serverlet<rasn_tool_agent_rpc_service>
{
public:
    rasn_tool_agent_rpc_service() : ::dsn::serverlet<rasn_tool_agent_rpc_service>("rasn.tool.agent") {}
    void open_service();
    void close_service();

protected:
    void on_agent_describe(const std::string &request, ::dsn::rpc_replier<agent_descriptor> &reply);
    void on_agent_invoke(const agent_request &request, ::dsn::rpc_replier<agent_response> &reply);
    void on_agent_cancel(const agent_request &request, ::dsn::rpc_replier<agent_response> &reply);
    void on_agent_heartbeat(const std::string &request, ::dsn::rpc_replier<agent_descriptor> &reply);
    void on_agent_query(const std::string &request, ::dsn::rpc_replier<agent_descriptor> &reply);
};

class rasn_coordinator_rpc_service : public ::dsn::serverlet<rasn_coordinator_rpc_service>
{
public:
    rasn_coordinator_rpc_service() : ::dsn::serverlet<rasn_coordinator_rpc_service>("rasn.coordinator") {}
    void open_service();
    void close_service();

protected:
    void on_agent_describe(const std::string &request, ::dsn::rpc_replier<agent_descriptor> &reply);
    void on_agent_invoke(const agent_request &request, ::dsn::rpc_replier<agent_response> &reply);
    void on_agent_cancel(const agent_request &request, ::dsn::rpc_replier<agent_response> &reply);
    void on_agent_heartbeat(const std::string &request, ::dsn::rpc_replier<agent_descriptor> &reply);
    void on_agent_query(const std::string &request, ::dsn::rpc_replier<agent_descriptor> &reply);
};

class rasn_llm_agent_client : public virtual ::dsn::clientlet
{
public:
    explicit rasn_llm_agent_client(::dsn::rpc_address server) : _server(server) {}
    std::pair<::dsn::error_code, model_gateway_response>
    describe_model_sync(const std::string &request,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                        int thread_hash = 0,
                        uint64_t partition_hash = 0);
    std::pair<::dsn::error_code, model_gateway_response>
    set_provider_sync(const model_provider_request &request,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                      int thread_hash = 0,
                      uint64_t partition_hash = 0);
    std::pair<::dsn::error_code, model_gateway_response>
    health_sync(const std::string &request,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                int thread_hash = 0,
                uint64_t partition_hash = 0);

private:
    ::dsn::rpc_address _server;
};

class rasn_llm_agent_app : public ::dsn::service_app
{
public:
    explicit rasn_llm_agent_app(::dsn_gpid gpid) : ::dsn::service_app(gpid) {}
    virtual ::dsn::error_code start(int argc, char **argv);
    virtual ::dsn::error_code stop(bool cleanup = false);

private:
    rasn_llm_agent_rpc_service _rpc;
};

class rasn_tool_agent_app : public ::dsn::service_app
{
public:
    explicit rasn_tool_agent_app(::dsn_gpid gpid) : ::dsn::service_app(gpid) {}
    virtual ::dsn::error_code start(int argc, char **argv);
    virtual ::dsn::error_code stop(bool cleanup = false);

private:
    rasn_tool_agent_rpc_service _rpc;
};

class rasn_coordinator_app : public ::dsn::service_app
{
public:
    explicit rasn_coordinator_app(::dsn_gpid gpid) : ::dsn::service_app(gpid) {}
    virtual ::dsn::error_code start(int argc, char **argv);
    virtual ::dsn::error_code stop(bool cleanup = false);

private:
    rasn_coordinator_rpc_service _rpc;
};

} // namespace rasn
} // namespace dsn
