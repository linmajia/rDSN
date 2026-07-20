#pragma once

#include <rasn/agent_messages.h>
#include <rasn/agent_runtime.h>
#include <rasn/agent_tools.h>
#include <rasn/admission_gate.h>
#include <rasn/circuit_breaker.h>
#include <rasn/llm_provider.h>
#include <rasn/rate_limiter.h>
#include <rasn/model_cost.h>
#include <rasn/metrics.h>
#include <rasn/rasn.code.definition.h>
#include <rasn/state_service.h>
#include <rasn/workflow_service.h>

#include <dsn/cpp/zlocks.h>
#include <dsn/service_api_cpp.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

class refreshable_endpoint_binding;

class rasn_llm_agent_service : public agent_runtime
{
public:
    rasn_llm_agent_service();

    void start();
    void stop();
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

    // Point-in-time view of every model-provider admission gate (for ops
    // commands and the model gateway summary).
    std::vector<admission_gate_registry::entry> model_admission_states() const;

    // Point-in-time view of every model-provider rate limiter (for ops commands
    // and the model gateway summary).
    std::vector<rate_limiter_registry::entry> model_rate_states() const;

    // Point-in-time view of every model-provider cost/token budget (for ops
    // commands and the model gateway summary). The entry's requests_per_min/burst
    // fields carry tokens/minute and burst tokens rather than request counts.
    std::vector<rate_limiter_registry::entry> model_cost_states() const;

private:
    // Lazily load [rasn.model] circuit-breaker tunables (config reads are
    // null-safe and return defaults before rDSN config is loaded).
    void ensure_model_breaker_config();
    // Lazily load [rasn.model] admission-control tunables (null-safe; defaults
    // before rDSN config is loaded).
    void ensure_model_admission_config();
    // Lazily load [rasn.model] rate-limit tunables (null-safe; defaults before
    // rDSN config is loaded).
    void ensure_model_rate_config();
    // Lazily load [rasn.model] cost/token-budget tunables (null-safe; defaults
    // before rDSN config is loaded). Caches the full model_cost_config for the
    // estimator in addition to seeding the token bucket.
    void ensure_model_cost_config();
    // Non-mutating breaker precheck: returns true and fills *fast_fail when the
    // provider's breaker is open and still cooling down, so an open breaker
    // fast-fails ahead of admission/rate rejection. Does not consume the half-open
    // probe; the authoritative model_breaker_admit() runs after the other gates.
    bool model_breaker_is_open(const std::string &provider,
                               const agent_task &task,
                               nucleus_runtime &runtime,
                               llm_response *fast_fail);
    // Returns the authoritative admission decision and fills *fast_fail when the
    // request must be short-circuited. The decision is carried through report()
    // so only the admitted half-open probe may resolve shared breaker state.
    breaker_decision model_breaker_admit(const std::string &provider,
                                         const agent_task &task,
                                         nucleus_runtime &runtime,
                                         uint64_t probe_lease_hint_ms,
                                         llm_response *fast_fail);
    // Report the outcome of an admitted call back to the provider's breaker.
    void model_breaker_report(const std::string &provider,
                              const agent_task &task,
                              nucleus_runtime &runtime,
                              const breaker_decision &admission,
                              bool ok);
    // Acquire an admission slot for the active provider. The returned slot
    // reserves capacity until destroyed; on rejection slot.admitted() is false and
    // *fast_fail is filled (recording the rejection metric). The slot's graceful
    // backpressure delay is NOT applied here -- call apply_model_backpressure()
    // once the rate limiter and breaker have also admitted the request, so a
    // short-circuited request never sleeps.
    admission_slot model_admission_admit(const std::string &provider,
                                         const agent_task &task,
                                         nucleus_runtime &runtime,
                                         llm_response *fast_fail);
    // Acquire a rate-limiter token for the active provider. On rejection the
    // returned decision has allowed() false and *fast_fail is filled (recording
    // the rejection metric). The decision's pacing delay is NOT applied here --
    // it is applied by apply_model_backpressure() once the breaker has also
    // admitted the request. Rate acquisition precedes the breaker so a
    // rate-rejected request never consumes a half-open breaker probe.
    rate_decision model_rate_acquire(const std::string &provider,
                                     const agent_task &task,
                                     nucleus_runtime &runtime,
                                     llm_response *fast_fail);
    // Return a rate-limiter token taken by model_rate_acquire() when the request is
    // abandoned before reaching the provider (the breaker short-circuited it after
    // the token was acquired). Keeps a breaker fast-fail from draining the quota. A
    // no-op when the limiter is disabled/unlimited (no token was taken).
    void model_rate_refund(const std::string &provider);
    // Acquire cost/token budget for the active provider, charging the request's
    // estimated token cost derived from prompt_chars (system + user + context
    // characters). On rejection the returned decision has allowed() false and
    // *fast_fail is filled (recording the rejection metric); *charged is always
    // set to the estimated cost so the caller can refund it if a later gate (the
    // breaker) abandons the request. Ordered after the request-count rate limiter
    // and before the breaker, like model_rate_acquire(), so a cost-rejected
    // request never consumes a half-open breaker probe. The pacing delay carried
    // by the decision is NOT applied here; apply_model_backpressure() applies it.
    rate_decision model_cost_acquire(const std::string &provider,
                                     size_t prompt_chars,
                                     const agent_task &task,
                                     nucleus_runtime &runtime,
                                     double *charged,
                                     llm_response *fast_fail);
    // Return the token charge taken by model_cost_acquire() when the request is
    // abandoned before reaching the provider (the breaker short-circuited it after
    // the charge was taken). A no-op when the budget is disabled/unlimited (no
    // charge was deducted).
    void model_cost_refund(const std::string &provider, double charged);
    // Apply (and record) the combined admission + rate-limiter + cost-budget
    // backpressure delay, if any. Called after admission, the rate limiter, the
    // cost budget, and the breaker have all admitted the request. Sleeps once for
    // the largest of the delays so the governors never stack into additive
    // over-delay.
    void apply_model_backpressure(const std::string &provider,
                                  const agent_task &task,
                                  nucleus_runtime &runtime,
                                  const admission_slot &slot,
                                  const rate_decision &rate,
                                  const rate_decision &cost);
    // True when the active provider should be guarded by overload/failure
    // protection (network-backed providers only; in-process providers are
    // exempt). Shared by the circuit breaker, admission control, and rate limiter.
    bool provider_needs_guards() const;

    std::unique_ptr<llm_provider> _provider;
    circuit_breaker_registry _model_breakers;
    std::once_flag _model_breaker_config_once;
    admission_gate_registry _model_admission;
    std::once_flag _model_admission_config_once;
    rate_limiter_registry _model_rate;
    std::once_flag _model_rate_config_once;
    rate_limiter_registry _model_cost;
    std::once_flag _model_cost_config_once;
    model_cost_config _model_cost_config_cache;
};

class rasn_tool_agent_service : public agent_runtime
{
public:
    rasn_tool_agent_service();

    void start();
    void stop();
    void set_tool_provider(std::unique_ptr<agent_tool_provider> tools);
    std::string describe_tools() const;
    std::string tool_resilience_report() const;
    tool_result run_tool(const std::string &name,
                         const std::vector<std::string> &args,
                         nucleus_runtime &runtime,
                         const agent_task &task,
                         const std::vector<std::string> &policy_labels = std::vector<std::string>()) const;
    agent_response invoke(const agent_request &request, nucleus_runtime &runtime) const;

private:
    std::shared_ptr<agent_tool_provider> current_tool_provider() const;
    void ensure_tool_admission_config() const;
    void ensure_tool_rate_config() const;
    admission_slot tool_admission_admit(const std::string &tool,
                                        const agent_task &task,
                                        nucleus_runtime &runtime,
                                        tool_result *fast_fail) const;
    rate_decision tool_rate_acquire(const std::string &tool,
                                    const agent_task &task,
                                    nucleus_runtime &runtime,
                                    tool_result *fast_fail) const;
    void apply_tool_backpressure(const std::string &tool,
                                 const agent_task &task,
                                 nucleus_runtime &runtime,
                                 const admission_slot &slot,
                                 const rate_decision &rate) const;

    std::shared_ptr<agent_tool_provider> _tools;
    mutable ::dsn::service::zlock _tool_lock;
    mutable admission_gate_registry _tool_admission;
    mutable std::once_flag _tool_admission_config_once;
    mutable rate_limiter_registry _tool_rate;
    mutable std::once_flag _tool_rate_config_once;
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
    std::string remote_agent_resilience_report() const;
    // Human-readable process-wide overload budget report (global concurrency
    // bulkhead + request-rate ceiling that bound total in-flight work across all
    // dependencies). Shared by `rasn.resilience` and CodePilot observe resilience.
    std::string overload_resilience_report() const;

private:
    void ensure_remote_agent_breaker_config();
    void ensure_remote_agent_admission_config();
    void ensure_remote_agent_rate_config();
    bool remote_agent_breaker_is_open(const agent_descriptor &agent,
                                      const agent_request &request,
                                      nucleus_runtime &runtime,
                                      agent_response *fast_fail);
    breaker_decision remote_agent_breaker_admit(const agent_descriptor &agent,
                                                const agent_request &request,
                                                nucleus_runtime &runtime,
                                                agent_response *fast_fail);
    void remote_agent_breaker_report(const agent_descriptor &agent,
                                     const agent_task &task,
                                     nucleus_runtime &runtime,
                                     const breaker_decision &admission,
                                     bool ok);
    admission_slot remote_agent_admission_admit(const agent_descriptor &agent,
                                                const agent_request &request,
                                                nucleus_runtime &runtime,
                                                agent_response *fast_fail);
    rate_decision remote_agent_rate_acquire(const agent_descriptor &agent,
                                            const agent_request &request,
                                            nucleus_runtime &runtime,
                                            agent_response *fast_fail);
    void remote_agent_rate_refund(const agent_descriptor &agent);
    void apply_remote_agent_backpressure(const agent_descriptor &agent,
                                         const agent_task &task,
                                         nucleus_runtime &runtime,
                                         const admission_slot &slot,
                                         const rate_decision &rate);
    agent_response invoke_remote_agent(const agent_request &request,
                                       nucleus_runtime &runtime,
                                       const agent_descriptor &agent);
    // Process-wide overload budget: a single global concurrency bulkhead and a
    // single global request-rate ceiling applied at the coordinator invoke
    // chokepoint, bounding total in-flight work and throughput across ALL
    // dependencies (model, tool, remote-agent) in both inline and RPC modes.
    // Reuses the same admission_gate (exp_delay backpressure) and rate_limiter
    // (dsn_now_ms token bucket) engines as the per-dependency gateways. Lazily
    // configured from [rasn.overload]; defaults are passthrough.
    void ensure_overload_config() const;
    admission_slot overload_admit(const agent_request &request,
                                  nucleus_runtime &runtime,
                                  agent_response *fast_fail);
    rate_decision overload_rate_acquire(const agent_request &request,
                                        nucleus_runtime &runtime,
                                        agent_response *fast_fail);
    void apply_overload_backpressure(const agent_task &task,
                                     nucleus_runtime &runtime,
                                     const admission_slot &slot,
                                     const rate_decision &rate);
    // Return a process-wide overload rate token taken by overload_rate_acquire()
    // when the request is abandoned before dispatch (e.g. route resolution fails),
    // so pre-dispatch failures cannot permanently drain the global rate budget.
    // Mirrors model_rate_refund()/remote_agent_rate_refund(); a no-op when the rate
    // ceiling is disabled or unlimited (no token was taken).
    void overload_rate_refund();
    // Result of entering the process-wide overload budget. Holds the RAII admission
    // slot (reserved capacity is released when this object is destroyed), whether
    // the gate passed, and a populated fast-fail response when it did not.
    struct overload_gate_hold
    {
        admission_slot slot;
        bool passed = false;
        agent_response rejection;
    };
    // Combined entry into the process-wide overload budget: admission bulkhead, then
    // rate ceiling, then coalesced graceful backpressure. Shared by invoke() and the
    // inline streaming fast path so neither can bypass the global budget. On success
    // the returned hold keeps the admission slot reserved for the caller's dispatch.
    overload_gate_hold enter_overload_gate(const agent_request &request, nucleus_runtime &runtime);

    rasn_llm_agent_service &_llm_agent;
    rasn_tool_agent_service &_tool_agent;
    circuit_breaker_registry _remote_agent_breakers;
    std::once_flag _remote_agent_breaker_config_once;
    admission_gate_registry _remote_agent_admission;
    std::once_flag _remote_agent_admission_config_once;
    rate_limiter_registry _remote_agent_rate;
    std::once_flag _remote_agent_rate_config_once;
    mutable std::unique_ptr<admission_gate> _overload_admission;
    mutable std::unique_ptr<rate_limiter> _overload_rate;
    mutable std::once_flag _overload_config_once;
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

    model_gateway_response set_provider(const std::string &provider_name, const std::string &model_name = "");
    model_gateway_response model_provider() const;
    model_gateway_response model_health() const;
    std::string provider_summary() const;
    // Per-provider model circuit-breaker states (for ops commands / summaries).
    std::vector<circuit_breaker_registry::entry> model_breaker_states() const;
    // Per-provider model admission-control states (for ops commands / summaries).
    std::vector<admission_gate_registry::entry> model_admission_states() const;
    // Per-provider model rate-limiter states (for ops commands / summaries).
    std::vector<rate_limiter_registry::entry> model_rate_states() const;
    // Per-provider model cost/token-budget states (for ops commands / summaries).
    std::vector<rate_limiter_registry::entry> model_cost_states() const;
    // Human-readable per-provider resilience report covering circuit breakers,
    // admission control, and rate limiting (shared by the `rasn.resilience`
    // command and CodePilot's `observe resilience`).
    std::string model_resilience_report() const;
    // Human-readable per-tool resilience report covering tool admission/rate
    // controls.
    std::string tool_resilience_report() const;
    // Human-readable remote-agent dispatch resilience report covering
    // coordinator-to-agent RPC circuit breakers, admission, and rate controls.
    std::string remote_agent_resilience_report() const;
    // Human-readable process-wide overload budget report (global concurrency
    // bulkhead + request-rate ceiling bounding total in-flight work across all
    // dependencies).
    std::string overload_resilience_report() const;
    // Combined resilience report (model + tool + remote-agent), the single
    // source of truth shared by the `rasn.resilience` command and CodePilot's
    // `observe resilience`.
    std::string resilience_report() const;
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
    bool rpc_clients_enabled() const
    {
        return _rpc_clients_enabled.load(std::memory_order_acquire);
    }
    const ::dsn::rpc_address &registry_address() const
    {
        (void)rpc_clients_enabled();
        return _registry_address;
    }
    const ::dsn::rpc_address &coordinator_address() const
    {
        (void)rpc_clients_enabled();
        return _coordinator_address;
    }
    const ::dsn::rpc_address &llm_agent_address() const
    {
        (void)rpc_clients_enabled();
        return _llm_agent_address;
    }
    const ::dsn::rpc_address &tool_agent_address() const
    {
        (void)rpc_clients_enabled();
        return _tool_agent_address;
    }
    const ::dsn::rpc_address &state_address() const
    {
        (void)rpc_clients_enabled();
        return _state_address;
    }
    const ::dsn::rpc_address &workflow_address() const
    {
        (void)rpc_clients_enabled();
        return _workflow_address;
    }
    const ::dsn::rpc_address &observability_address() const
    {
        (void)rpc_clients_enabled();
        return _observability_address;
    }
    agent_descriptor llm_agent_descriptor() const { return _llm_agent.descriptor(); }
    agent_descriptor tool_agent_descriptor() const { return _tool_agent.descriptor(); }
    agent_descriptor coordinator_descriptor() const { return _coordinator.descriptor(); }
    // Returns null for an unknown logical service. Callers accepting dynamic
    // names must reject that result before dereferencing it.
    std::shared_ptr<refreshable_endpoint_binding>
    service_endpoint_binding(const std::string &service) const;

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
    state_response delete_state_prefix(const state_delete_prefix_request &request);
    state_delete_prefix_result
    delete_state_prefix_detailed(const state_delete_prefix_request &request);
    state_response
    advance_state_sequence(const state_sequence_barrier_request &request);
    state_response checkpoint_state(const state_checkpoint_request &request);
    state_checkpoint_result
    checkpoint_state_detailed(const state_checkpoint_request &request);
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

    void register_ops_commands_once();
    bool ensure_inline_state_recovered(std::string *error);
    void record_inline_state_recovery(const state_response &response);
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
    std::shared_ptr<refreshable_endpoint_binding> _coordinator_binding;
    std::shared_ptr<refreshable_endpoint_binding> _llm_agent_binding;
    std::shared_ptr<refreshable_endpoint_binding> _tool_agent_binding;
    std::shared_ptr<refreshable_endpoint_binding> _state_binding;
    std::shared_ptr<refreshable_endpoint_binding> _workflow_binding;
    std::shared_ptr<refreshable_endpoint_binding> _observability_binding;
    mutable ::dsn::service::zlock _lifecycle_lock;
    mutable ::dsn::service::zlock _inline_state_recovery_lock;
    uint32_t _lifecycle_ref_count;
    bool _lifecycle_transitioning;
    // Addresses and bindings are initialized before this one-way publication
    // flag is released. They are never reconfigured after publication.
    std::atomic<bool> _rpc_clients_enabled;
    bool _started;
    bool _inline_state_recovery_attempted;
    std::string _inline_state_recovery_error;
};

struct state_migration_report
{
    bool ok = false;
    bool applied = false;
    std::string error;
    std::string checkpoint_path;
    std::string key_prefix;
    uint64_t source_last_sequence = 0;
    uint64_t target_last_sequence = 0;
    size_t source_records = 0;
    size_t target_records = 0;
    size_t planned_records = 0;
    size_t unchanged_records = 0;
    size_t migrated_records = 0;
    size_t verified_records = 0;
    bool sequence_advance_required = false;
    std::vector<std::string> conflict_keys;
};

state_migration_report migrate_state_checkpoint(rasn_service_graph &services,
                                                const std::string &checkpoint_path,
                                                const std::string &key_prefix,
                                                bool apply);

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
    std::pair< ::dsn::error_code, model_gateway_response>
    describe_model_sync(const std::string &request,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                        int thread_hash = 0,
                        uint64_t partition_hash = 0);
    std::pair< ::dsn::error_code, model_gateway_response>
    set_provider_sync(const model_provider_request &request,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                      int thread_hash = 0,
                      uint64_t partition_hash = 0);
    std::pair< ::dsn::error_code, model_gateway_response>
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
    rasn_core_service_registration _registration;
};

class rasn_tool_agent_app : public ::dsn::service_app
{
public:
    explicit rasn_tool_agent_app(::dsn_gpid gpid) : ::dsn::service_app(gpid) {}
    virtual ::dsn::error_code start(int argc, char **argv);
    virtual ::dsn::error_code stop(bool cleanup = false);

private:
    rasn_tool_agent_rpc_service _rpc;
    rasn_core_service_registration _registration;
};

class rasn_coordinator_app : public ::dsn::service_app
{
public:
    explicit rasn_coordinator_app(::dsn_gpid gpid) : ::dsn::service_app(gpid) {}
    virtual ::dsn::error_code start(int argc, char **argv);
    virtual ::dsn::error_code stop(bool cleanup = false);

private:
    rasn_coordinator_rpc_service _rpc;
    rasn_core_service_registration _registration;
};

} // namespace rasn
} // namespace dsn
