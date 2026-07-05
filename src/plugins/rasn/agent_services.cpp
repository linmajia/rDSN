#include <rasn/agent_services.h>

#include <rasn/agent_clients.h>
#include <rasn/agent_registry.h>
#include <rasn/coordinator_service.h>
#include <rasn/metrics.h>
#include <rasn/policy_manager.h>
#include <rasn/redaction.h>
#include <rasn/rpc_resilience.h>
#include <rasn/runtime_provider.h>
#include <rasn/state_service.h>
#include <rasn/workflow_service.h>

#include <dsn/cpp/utils.h>
#include <dsn/tool-api/command.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

namespace dsn {
namespace rasn {

namespace {

bool g_rdsn_rpc_enabled = false;

struct service_endpoint_config
{
    std::string host;
    uint16_t port = 0;
    std::string endpoint_uri;
};

void set_rdsn_rpc_enabled(bool enabled)
{
    g_rdsn_rpc_enabled = enabled;
}

// Wrap a core-service client RPC (state / workflow / observability / registry)
// with the shared rASN client resilience policy: a per-(service, endpoint)
// circuit breaker and idempotency-aware retries with linear backoff. `call`
// performs the actual one-shot `*_sync` RPC and returns {error_code, response}.
// `idempotent` must be false for operations that must not be re-applied on an
// ambiguous timeout (e.g. starting a workflow run, a compare-and-swap, or a plain
// state put that allocates a fresh sequence per apply); those are only retried on
// transport errors that prove the request never reached the server.
template <typename TResponse, typename FCall>
std::pair<::dsn::error_code, TResponse>
core_rpc_with_resilience(const char *op, const ::dsn::rpc_address &address, bool idempotent, FCall &&call)
{
    ensure_rasn_core_breaker_config();
    const rpc_resilience_options options = read_rasn_core_resilience_options();
    // Key the breaker by service+endpoint (not operation+endpoint) so every
    // operation to a down endpoint trips the same breaker together.
    const std::string key = core_service_breaker_key(op, address.to_string());
    return resilient_rpc_call<TResponse>(
        global_rasn_core_breakers(), key, options, idempotent, default_rpc_timeout(), std::forward<FCall>(call));
}

::dsn::rpc_address make_ipv4_address(const std::string &host, uint16_t port)
{
    ::dsn::rpc_address address;
    address.assign_ipv4(host.c_str(), port);
    return address;
}

std::string config_string_or_default(const std::string &section,
                                     const std::string &key,
                                     const std::string &default_value,
                                     const std::string &description)
{
    const char *value =
        ::dsn_config_get_value_string(section.c_str(), key.c_str(), default_value.c_str(), description.c_str());
    return value == nullptr ? default_value : value;
}

uint16_t config_port_or_default(const std::string &key, uint16_t default_port)
{
    const uint64_t configured =
        ::dsn_config_get_value_uint64("rasn.service", key.c_str(), default_port, "rASN service RPC port");
    if (configured > (std::numeric_limits<uint16_t>::max)())
    {
        dwarn("rasn.service.%s=%llu exceeds uint16_t port range; using default %u",
              key.c_str(),
              static_cast<unsigned long long>(configured),
              static_cast<unsigned int>(default_port));
        return default_port;
    }
    return static_cast<uint16_t>(configured);
}

service_endpoint_config config_service_endpoint(const std::string &service_name, uint16_t default_port)
{
    service_endpoint_config endpoint;
    const std::string default_host =
        config_string_or_default("rasn.service", "host", "localhost", "default rASN service RPC host");
    endpoint.host = config_string_or_default(
        "rasn.service", service_name + "_host", default_host, "rASN service RPC host");
    endpoint.port = config_port_or_default(service_name + "_port", default_port);
    endpoint.endpoint_uri =
        config_string_or_default("rasn.service", service_name + "_uri", "", "rASN service URI endpoint");
    return endpoint;
}

std::string stable_effect_fingerprint(const std::string &tool, const std::string &arguments)
{
    uint64_t hash = 1469598103934665603ULL;
    const std::string payload = tool + "\n" + arguments;
    for (char ch : payload)
    {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ULL;
    }
    std::ostringstream stream;
    stream << std::hex << hash;
    return stream.str();
}

std::string external_effect_replay_policy(tool_side_effect side_effect)
{
    return side_effect == tool_side_effect::read_only ? "not_required" : "recorded_result_required";
}

void record_external_effect_if_needed(nucleus_runtime &runtime,
                                      const agent_task &task,
                                      tool_side_effect side_effect,
                                      const std::string &tool,
                                      const std::string &arguments,
                                      const std::string &status)
{
    if (side_effect == tool_side_effect::read_only)
    {
        return;
    }
    runtime.record_external_effect(task,
                                   to_string(side_effect),
                                   tool,
                                   stable_effect_fingerprint(tool, arguments),
                                   external_effect_replay_policy(side_effect),
                                   status);
}

bool registry_dynamic_registration_enabled()
{
    return ::dsn_config_get_value_bool(
        "rasn.registry", "dynamic_registration", true, "Register built-in rASN agents through registry RPC");
}

uint64_t registry_heartbeat_ms()
{
    return ::dsn_config_get_value_uint64(
        "rasn.registry", "heartbeat_ms", 2000, "rASN registry heartbeat interval in milliseconds");
}

std::chrono::milliseconds registry_registration_timeout()
{
    const uint64_t timeout_ms = ::dsn_config_get_value_uint64(
        "rasn.registry", "registration_timeout_ms", 1000, "rASN registry register/heartbeat RPC timeout");
    return std::chrono::milliseconds(timeout_ms);
}

::dsn::rpc_address config_service_address(const std::string &service_name, uint16_t default_port)
{
    const service_endpoint_config endpoint = config_service_endpoint(service_name, default_port);
    if (!endpoint.endpoint_uri.empty())
    {
        return ::dsn::url_host_address(endpoint.endpoint_uri.c_str());
    }
    return make_ipv4_address(endpoint.host, endpoint.port);
}

void set_agent_service_endpoint(agent_runtime &agent, const std::string &service_name, uint16_t default_port)
{
    const service_endpoint_config endpoint = config_service_endpoint(service_name, default_port);
    agent.set_endpoint(endpoint.host, endpoint.port, endpoint.endpoint_uri);
}

std::vector<agent_descriptor> registered_service_agents(const rasn_llm_agent_service &llm_agent,
                                                        const rasn_tool_agent_service &tool_agent,
                                                        const rasn_coordinator_service &coordinator)
{
    std::vector<agent_descriptor> agents;
    agents.push_back(llm_agent.descriptor());
    agents.push_back(tool_agent.descriptor());
    agents.push_back(coordinator.descriptor());
    return agents;
}

llm_response rpc_error_response(const std::string &message)
{
    llm_response response;
    response.ok = false;
    response.error = message;
    return response;
}

tool_result rpc_error_tool_result(const std::string &message)
{
    tool_result result;
    result.ok = false;
    result.error = message;
    return result;
}

model_gateway_response model_error_response(const std::string &message)
{
    model_gateway_response response;
    response.ok = false;
    response.error = message;
    response.provider.health = "unhealthy";
    return response;
}

std::string model_gateway_summary(const model_gateway_response &response, const nucleus_runtime &runtime)
{
    if (!response.ok)
    {
        return response.error;
    }
    const model_provider_descriptor &provider = response.provider;
    std::ostringstream oss;
    oss << "provider=" << provider.provider
        << " model=" << provider.model
        << " endpoint=" << provider.endpoint
        << " payload=" << provider.payload_format
        << " health=" << provider.health
        << " token_env=" << (provider.token_env.empty() ? "<none>" : provider.token_env)
        << " credential_ref=" << (provider.credential_ref.empty() ? "<none>" : provider.credential_ref)
        << " trace=" << (runtime.trace_file().empty() ? "<memory>" : runtime.trace_file())
        << " trace_id=" << runtime.trace_id();
    return oss.str();
}

std::string format_model_breaker_states(const std::vector<circuit_breaker_registry::entry> &states)
{
    if (states.empty())
    {
        return "model circuit breakers: none engaged";
    }
    std::ostringstream oss;
    oss << "model circuit breakers:";
    for (size_t i = 0; i < states.size(); ++i)
    {
        const circuit_breaker_registry::entry &entry = states[i];
        oss << "\n- provider=" << entry.key << " state=" << to_string(entry.state)
            << " consecutive_failures=" << entry.consecutive_failures;
    }
    return oss.str();
}

std::string format_model_admission_states(const std::vector<admission_gate_registry::entry> &states)
{
    if (states.empty())
    {
        return "model admission control: none engaged";
    }
    std::ostringstream oss;
    oss << "model admission control:";
    for (size_t i = 0; i < states.size(); ++i)
    {
        const admission_gate_registry::entry &entry = states[i];
        oss << "\n- provider=" << entry.key << " in_flight=" << entry.in_flight
            << " max_concurrency=" << entry.max_concurrency
            << " soft_concurrency=" << entry.soft_concurrency;
    }
    return oss.str();
}

std::string format_model_rate_states(const std::vector<rate_limiter_registry::entry> &states)
{
    if (states.empty())
    {
        return "model rate limiters: none engaged";
    }
    std::ostringstream oss;
    oss << "model rate limiters:";
    for (size_t i = 0; i < states.size(); ++i)
    {
        const rate_limiter_registry::entry &entry = states[i];
        oss << "\n- provider=" << entry.key << " requests_per_min=" << entry.requests_per_min
            << " burst=" << entry.burst << " tokens=" << std::fixed << std::setprecision(2)
            << entry.tokens;
    }
    return oss.str();
}

std::string format_model_cost_states(const std::vector<rate_limiter_registry::entry> &states)
{
    if (states.empty())
    {
        return "model cost budgets: none engaged";
    }
    // The cost budget reuses the token-bucket registry, so the entry's generic
    // requests_per_min/burst/tokens fields carry token-denominated values here.
    std::ostringstream oss;
    oss << "model cost budgets:";
    for (size_t i = 0; i < states.size(); ++i)
    {
        const rate_limiter_registry::entry &entry = states[i];
        oss << "\n- provider=" << entry.key << " tokens_per_min=" << entry.requests_per_min
            << " burst_tokens=" << entry.burst << " tokens=" << std::fixed << std::setprecision(2)
            << entry.tokens;
    }
    return oss.str();
}

// Total prompt characters charged to the cost budget: the system prompt, the user
// prompt, and every context element including the per-element framing the provider
// wraps around it. The estimate is a function of prompt size only (never the
// provider response), so charging a request stays deterministic and replay-safe.
size_t prompt_cost_chars(const llm_request &llm)
{
    // Fixed characters llm_provider.cpp join_context() wraps around each context
    // element: "\n\n--- context " (14) + " ---\n" (5). The provider sends
    // user_prompt + join_context(context) as one message, so the model tokenizes
    // this framing too; charging only the raw element size would systematically
    // undercount requests with many (or empty) context entries.
    constexpr size_t k_context_frame_chars = 19;
    size_t chars = llm.system_prompt.size() + llm.user_prompt.size();
    for (size_t i = 0; i < llm.context.size(); ++i)
    {
        chars += llm.context[i].size() + k_context_frame_chars + std::to_string(i + 1).size();
    }
    return chars;
}

std::string format_tool_admission_states(const std::vector<admission_gate_registry::entry> &states)
{
    if (states.empty())
    {
        return "tool admission control: none engaged";
    }
    std::ostringstream oss;
    oss << "tool admission control:";
    for (size_t i = 0; i < states.size(); ++i)
    {
        const admission_gate_registry::entry &entry = states[i];
        oss << "\n- tool=" << entry.key << " in_flight=" << entry.in_flight
            << " max_concurrency=" << entry.max_concurrency
            << " soft_concurrency=" << entry.soft_concurrency;
    }
    return oss.str();
}

std::string format_tool_rate_states(const std::vector<rate_limiter_registry::entry> &states)
{
    if (states.empty())
    {
        return "tool rate limiters: none engaged";
    }
    std::ostringstream oss;
    oss << "tool rate limiters:";
    for (size_t i = 0; i < states.size(); ++i)
    {
        const rate_limiter_registry::entry &entry = states[i];
        oss << "\n- tool=" << entry.key << " requests_per_min=" << entry.requests_per_min
            << " burst=" << entry.burst << " tokens=" << std::fixed << std::setprecision(2)
            << entry.tokens;
    }
    return oss.str();
}

std::string remote_agent_key(const agent_descriptor &agent)
{
    if (!agent.agent_id.empty())
    {
        return agent.agent_id;
    }
    if (!agent.endpoint_uri.empty())
    {
        return agent.endpoint_uri;
    }
    if (!agent.host.empty() || agent.port != 0)
    {
        return agent.host + ":" + std::to_string(agent.port);
    }
    return "<unknown-agent>";
}

agent_response remote_agent_gate_response(const agent_request &request,
                                          const std::string &failure_class,
                                          const std::string &code,
                                          const std::string &message,
                                          bool retryable,
                                          const std::string &source)
{
    agent_response response;
    response.request_id = request.request_id;
    response.trace_id = request.trace_id;
    response.ok = false;
    response.error = make_agent_error(failure_class, code, message, retryable, source);
    return response;
}

agent_response overload_gate_response(const agent_request &request,
                                      const std::string &code,
                                      const std::string &message,
                                      const std::string &source)
{
    agent_response response;
    response.request_id = request.request_id;
    response.trace_id = request.trace_id;
    response.ok = false;
    // Process-wide overload rejections are non-retryable at this hop: the whole
    // process is shedding load, so an immediate retry against the same node would
    // just be rejected again. Callers with alternate nodes may still route away.
    response.error = make_agent_error("overload", code, message, false, source);
    return response;
}

bool remote_agent_response_is_dependency_success(const agent_response &response)
{
    // Only retryable failures count against the remote-agent circuit breaker.
    // Non-retryable application outcomes (policy denial, validation failure,
    // deterministic tool errors) are a valid response from the remote service and
    // should not poison the transport/dependency breaker.
    return response.ok || !response.error.retryable;
}

std::string format_remote_agent_breaker_states(const std::vector<circuit_breaker_registry::entry> &states)
{
    if (states.empty())
    {
        return "remote agent circuit breakers: none engaged";
    }
    std::ostringstream oss;
    oss << "remote agent circuit breakers:";
    for (size_t i = 0; i < states.size(); ++i)
    {
        const circuit_breaker_registry::entry &entry = states[i];
        oss << "\n- agent=" << entry.key << " state=" << to_string(entry.state)
            << " consecutive_failures=" << entry.consecutive_failures;
    }
    return oss.str();
}

std::string format_remote_agent_admission_states(const std::vector<admission_gate_registry::entry> &states)
{
    if (states.empty())
    {
        return "remote agent admission control: none engaged";
    }
    std::ostringstream oss;
    oss << "remote agent admission control:";
    for (size_t i = 0; i < states.size(); ++i)
    {
        const admission_gate_registry::entry &entry = states[i];
        oss << "\n- agent=" << entry.key << " in_flight=" << entry.in_flight
            << " max_concurrency=" << entry.max_concurrency
            << " soft_concurrency=" << entry.soft_concurrency;
    }
    return oss.str();
}

std::string format_remote_agent_rate_states(const std::vector<rate_limiter_registry::entry> &states)
{
    if (states.empty())
    {
        return "remote agent rate limiters: none engaged";
    }
    std::ostringstream oss;
    oss << "remote agent rate limiters:";
    for (size_t i = 0; i < states.size(); ++i)
    {
        const rate_limiter_registry::entry &entry = states[i];
        oss << "\n- agent=" << entry.key << " requests_per_min=" << entry.requests_per_min
            << " burst=" << entry.burst << " tokens=" << std::fixed << std::setprecision(2)
            << entry.tokens;
    }
    return oss.str();
}

std::string descriptor_line(const std::string &label, const agent_descriptor &descriptor)
{
    std::ostringstream oss;
    oss << "- " << label << " id=" << descriptor.agent_id
        << " role=" << descriptor.role
        << " health=" << descriptor.health
        << " capabilities=";
    for (size_t i = 0; i < descriptor.capabilities.size(); ++i)
    {
        if (i != 0)
        {
            oss << ",";
        }
        oss << descriptor.capabilities[i].name;
    }
    return oss.str();
}

std::string join_tool_args_for_policy(const std::vector<std::string> &args)
{
    std::ostringstream oss;
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (i != 0)
        {
            oss << " ";
        }
        oss << args[i];
    }
    return oss.str();
}

llm_request redact_llm_request(const llm_request &request)
{
    llm_request redacted = request;
    redacted.system_prompt = redact_sensitive_text(request.system_prompt);
    redacted.user_prompt = redact_sensitive_text(request.user_prompt);
    for (std::string &context : redacted.context)
    {
        context = redact_sensitive_text(context);
    }
    return redacted;
}

llm_response redact_llm_response(const llm_response &response)
{
    llm_response redacted = response;
    redacted.text = redact_sensitive_text(response.text);
    redacted.error = redact_sensitive_text(response.error);
    return redacted;
}

class scoped_agent_request
{
public:
   scoped_agent_request(const agent_runtime &runtime, const agent_request &request)
       : _runtime(runtime), _request(request), _active(true)
   {
   }

   ~scoped_agent_request()
   {
       if (_active)
       {
           _runtime.finish_request(_request);
       }
   }

private:
   const agent_runtime &_runtime;
   const agent_request &_request;
   bool _active;
};

state_response service_graph_policy_state_writer(const state_record &record)
{
    return global_rasn_services().put_state(record);
}

state_response service_graph_observability_state_writer(const state_record &record)
{
    return global_rasn_services().put_state(record);
}

// Read a [section] key as a uint32_t, clamping out-of-range values to the type
// max instead of silently wrapping. dsn_config returns uint64_t; a value above
// the type max would otherwise truncate on the narrowing cast -- e.g. 2^32 -> 0,
// which for a concurrency cap means "unlimited" and would disable the bulkhead.
// An explicit 0 is preserved (0 keeps its documented meaning).
uint32_t read_config_u32(const char *section,
                         const char *key,
                         uint32_t default_value,
                         const char *dsptr)
{
    const uint64_t raw = ::dsn_config_get_value_uint64(section, key, default_value, dsptr);
    const uint32_t max_u32 = (std::numeric_limits<uint32_t>::max)();
    return raw > max_u32 ? max_u32 : static_cast<uint32_t>(raw);
}

breaker_config read_model_breaker_config()
{
    breaker_config cfg;
    cfg.enabled = ::dsn_config_get_value_bool(
        "rasn.model", "circuit_breaker_enabled", true, "enable the rASN model-gateway circuit breaker");
    cfg.failure_threshold = read_config_u32(
        "rasn.model",
        "circuit_breaker_failure_threshold",
        5,
        "consecutive model-provider failures before the circuit breaker opens");
    cfg.open_ms = ::dsn_config_get_value_uint64(
        "rasn.model",
        "circuit_breaker_open_ms",
        30000,
        "cooldown in ms before an open model circuit breaker admits a half-open probe");
    return cfg;
}

admission_config read_model_admission_config()
{
    admission_config cfg;
    cfg.enabled = ::dsn_config_get_value_bool(
        "rasn.model",
        "admission_enabled",
        true,
        "enable rASN model-gateway admission control (concurrency bulkhead + backpressure)");
    cfg.max_concurrency = read_config_u32(
        "rasn.model",
        "max_concurrent_requests",
        32,
        "hard cap on concurrent in-flight model requests per provider (0 = unlimited)");
    cfg.soft_concurrency = read_config_u32(
        "rasn.model",
        "soft_concurrent_requests",
        16,
        "in-flight level at which graceful admission backpressure begins (0 = no backpressure)");
    cfg.max_backpressure_ms = read_config_u32(
        "rasn.model",
        "max_backpressure_ms",
        200,
        "upper bound in ms on the graceful admission backpressure delay");
    return cfg;
}

rate_limit_config read_model_rate_config()
{
    rate_limit_config cfg;
    cfg.enabled = ::dsn_config_get_value_bool(
        "rasn.model",
        "rate_limit_enabled",
        true,
        "enable the rASN model-gateway client-side rate limiter (token bucket)");
    cfg.requests_per_min = read_config_u32(
        "rasn.model",
        "rate_limit_requests_per_min",
        0,
        "sustained model request rate per provider in requests/minute (0 = unlimited)");
    cfg.burst = read_config_u32(
        "rasn.model",
        "rate_limit_burst",
        0,
        "rate-limiter burst capacity in tokens (0 = ~1s of the sustained rate)");
    cfg.max_wait_ms = read_config_u32(
        "rasn.model",
        "rate_limit_max_wait_ms",
        1000,
        "max ms a request may be paced waiting for a token before it is rejected (0 = reject immediately)");
    return cfg;
}

model_cost_config read_model_cost_config()
{
    model_cost_config cfg;
    cfg.enabled = ::dsn_config_get_value_bool(
        "rasn.model",
        "cost_budget_enabled",
        true,
        "enable the rASN model-gateway token/cost budget (weighs each request by estimated tokens)");
    cfg.tokens_per_min = read_config_u32(
        "rasn.model",
        "cost_tokens_per_min",
        0,
        "sustained model token budget per provider in estimated tokens/minute (0 = unlimited)");
    cfg.burst_tokens = read_config_u32(
        "rasn.model",
        "cost_burst_tokens",
        0,
        "cost-budget burst capacity in estimated tokens (0 = ~1s of the sustained budget)");
    cfg.max_wait_ms = read_config_u32(
        "rasn.model",
        "cost_max_wait_ms",
        1000,
        "max ms a request may be paced waiting for token budget before it is rejected (0 = reject immediately)");
    cfg.chars_per_token = read_config_u32(
        "rasn.model",
        "cost_chars_per_token",
        0,
        "prompt characters per estimated token (0 = default 4)");
    cfg.completion_percent = read_config_u32(
        "rasn.model",
        "cost_completion_percent",
        0,
        "completion allowance as a percent of estimated input tokens (0 = default 150; 100 = input only)");
    return cfg;
}

admission_config read_tool_admission_config()
{
    admission_config cfg;
    cfg.enabled = ::dsn_config_get_value_bool(
        "rasn.tool",
        "admission_enabled",
        true,
        "enable rASN tool-gateway admission control (concurrency bulkhead + backpressure)");
    cfg.max_concurrency = read_config_u32(
        "rasn.tool",
        "max_concurrent_requests",
        16,
        "hard cap on concurrent in-flight tool invocations per tool name (0 = unlimited)");
    cfg.soft_concurrency = read_config_u32(
        "rasn.tool",
        "soft_concurrent_requests",
        8,
        "in-flight level at which graceful tool admission backpressure begins (0 = no backpressure)");
    cfg.max_backpressure_ms = read_config_u32(
        "rasn.tool",
        "max_backpressure_ms",
        100,
        "upper bound in ms on graceful tool admission backpressure delay");
    return cfg;
}

rate_limit_config read_tool_rate_config()
{
    rate_limit_config cfg;
    cfg.enabled = ::dsn_config_get_value_bool(
        "rasn.tool",
        "rate_limit_enabled",
        true,
        "enable rASN tool-gateway client-side rate limiter (token bucket)");
    cfg.requests_per_min = read_config_u32(
        "rasn.tool",
        "rate_limit_requests_per_min",
        0,
        "sustained tool invocation rate per tool name in requests/minute (0 = unlimited)");
    cfg.burst = read_config_u32(
        "rasn.tool",
        "rate_limit_burst",
        0,
        "tool rate-limiter burst capacity in tokens (0 = ~1s of the sustained rate)");
    cfg.max_wait_ms = read_config_u32(
        "rasn.tool",
        "rate_limit_max_wait_ms",
        1000,
        "max ms a tool invocation may be paced waiting for a token before it is rejected");
    return cfg;
}

breaker_config read_remote_agent_breaker_config()
{
    breaker_config cfg;
    cfg.enabled = ::dsn_config_get_value_bool(
        "rasn.remote_agent",
        "circuit_breaker_enabled",
        true,
        "enable the rASN coordinator remote-agent circuit breaker");
    cfg.failure_threshold = read_config_u32(
        "rasn.remote_agent",
        "circuit_breaker_failure_threshold",
        5,
        "consecutive retryable remote-agent dispatch failures before the circuit breaker opens");
    cfg.open_ms = ::dsn_config_get_value_uint64(
        "rasn.remote_agent",
        "circuit_breaker_open_ms",
        30000,
        "cooldown in ms before an open remote-agent circuit breaker admits a half-open probe");
    return cfg;
}

admission_config read_remote_agent_admission_config()
{
    admission_config cfg;
    cfg.enabled = ::dsn_config_get_value_bool(
        "rasn.remote_agent",
        "admission_enabled",
        true,
        "enable rASN coordinator remote-agent admission control (concurrency bulkhead + backpressure)");
    cfg.max_concurrency = read_config_u32(
        "rasn.remote_agent",
        "max_concurrent_requests",
        64,
        "hard cap on concurrent in-flight coordinator RPC dispatches per remote agent (0 = unlimited)");
    cfg.soft_concurrency = read_config_u32(
        "rasn.remote_agent",
        "soft_concurrent_requests",
        32,
        "in-flight level at which graceful remote-agent admission backpressure begins (0 = no backpressure)");
    cfg.max_backpressure_ms = read_config_u32(
        "rasn.remote_agent",
        "max_backpressure_ms",
        200,
        "upper bound in ms on graceful remote-agent admission backpressure delay");
    return cfg;
}

rate_limit_config read_remote_agent_rate_config()
{
    rate_limit_config cfg;
    cfg.enabled = ::dsn_config_get_value_bool(
        "rasn.remote_agent",
        "rate_limit_enabled",
        true,
        "enable rASN coordinator remote-agent client-side rate limiter (token bucket)");
    cfg.requests_per_min = read_config_u32(
        "rasn.remote_agent",
        "rate_limit_requests_per_min",
        0,
        "sustained coordinator RPC dispatch rate per remote agent in requests/minute (0 = unlimited)");
    cfg.burst = read_config_u32(
        "rasn.remote_agent",
        "rate_limit_burst",
        0,
        "remote-agent rate-limiter burst capacity in tokens (0 = ~1s of the sustained rate)");
    cfg.max_wait_ms = read_config_u32(
        "rasn.remote_agent",
        "rate_limit_max_wait_ms",
        1000,
        "max ms a remote-agent dispatch may be paced waiting for a token before it is rejected");
    return cfg;
}

// Process-wide overload budget config. Unlike the per-dependency gateways these
// govern the WHOLE process (a single global bulkhead + a single global rate
// ceiling), so the concurrency defaults are 0 (disabled/passthrough): operators
// opt in by setting a cap sized to their host, and the framework's zero-config
// behavior is unchanged.
admission_config read_overload_admission_config()
{
    admission_config cfg;
    cfg.enabled = ::dsn_config_get_value_bool(
        "rasn.overload",
        "admission_enabled",
        true,
        "enable the rASN process-wide overload admission control (global concurrency bulkhead + backpressure)");
    cfg.max_concurrency = read_config_u32(
        "rasn.overload",
        "max_concurrent_operations",
        0,
        "hard cap on concurrent in-flight coordinator operations across all dependencies (0 = unlimited)");
    cfg.soft_concurrency = read_config_u32(
        "rasn.overload",
        "soft_concurrent_operations",
        0,
        "in-flight level at which graceful process-wide admission backpressure begins (0 = no backpressure)");
    cfg.max_backpressure_ms = read_config_u32(
        "rasn.overload",
        "max_backpressure_ms",
        200,
        "upper bound in ms on graceful process-wide admission backpressure delay");
    return cfg;
}

rate_limit_config read_overload_rate_config()
{
    rate_limit_config cfg;
    cfg.enabled = ::dsn_config_get_value_bool(
        "rasn.overload",
        "rate_limit_enabled",
        true,
        "enable the rASN process-wide overload rate limiter (global token bucket)");
    cfg.requests_per_min = read_config_u32(
        "rasn.overload",
        "rate_limit_requests_per_min",
        0,
        "sustained process-wide coordinator operation rate in requests/minute (0 = unlimited)");
    cfg.burst = read_config_u32(
        "rasn.overload",
        "rate_limit_burst",
        0,
        "process-wide rate-limiter burst capacity in tokens (0 = ~1s of the sustained rate)");
    cfg.max_wait_ms = read_config_u32(
        "rasn.overload",
        "rate_limit_max_wait_ms",
        1000,
        "max ms a coordinator operation may be paced waiting for a process-wide token before it is rejected");
    return cfg;
}

} // namespace

rasn_llm_agent_service::rasn_llm_agent_service()
    : agent_runtime("rasn.model.agent", "rasn.llm.agent"), _provider(create_provider_from_environment())
{
    add_capability(make_capability("model.complete", "agent_request", "agent_response", "nondeterministic"));
}

void rasn_llm_agent_service::start()
{
    dinfo("starting rasn.llm.agent service");
    agent_runtime::start();
}

void rasn_llm_agent_service::stop()
{
    dinfo("stopping rasn.llm.agent service");
    agent_runtime::stop();
}

model_gateway_response rasn_llm_agent_service::set_model_provider(const model_provider_request &request)
{
    if (request.schema_version != RASN_AGENT_SCHEMA_VERSION)
    {
        return model_error_response("model provider request has unsupported schema version");
    }
    const std::string provider = trim(request.provider);
    if (provider.empty())
    {
        return model_error_response("model provider request missing provider");
    }

    _provider = create_provider(provider, trim(request.model));
    model_gateway_response response = describe_model_provider();
    dinfo("set rASN model provider=%s model=%s endpoint=%s",
          response.provider.provider.c_str(),
          response.provider.model.c_str(),
          response.provider.endpoint.c_str());
    return response;
}

model_gateway_response rasn_llm_agent_service::describe_model_provider() const
{
    if (_provider == nullptr)
    {
        return model_error_response("no model provider configured");
    }
    model_gateway_response response;
    response.provider = _provider->describe();
    return response;
}

model_gateway_response rasn_llm_agent_service::model_health() const
{
    model_gateway_response response = describe_model_provider();
    if (!response.ok)
    {
        return response;
    }
    if (response.provider.provider.empty() || response.provider.model.empty())
    {
        return model_error_response("model provider metadata is incomplete");
    }
    response.provider.health = response.provider.health.empty() ? "configured" : response.provider.health;
    return response;
}

void rasn_llm_agent_service::ensure_model_breaker_config()
{
    std::call_once(_model_breaker_config_once,
                   [this] { _model_breakers.set_config(read_model_breaker_config()); });
}

void rasn_llm_agent_service::ensure_model_admission_config()
{
    std::call_once(_model_admission_config_once,
                   [this] { _model_admission.set_config(read_model_admission_config()); });
}

void rasn_llm_agent_service::ensure_model_rate_config()
{
    std::call_once(_model_rate_config_once,
                   [this] { _model_rate.set_config(read_model_rate_config()); });
}

void rasn_llm_agent_service::ensure_model_cost_config()
{
    std::call_once(_model_cost_config_once, [this] {
        _model_cost_config_cache = read_model_cost_config();
        _model_cost.set_config(to_rate_limit_config(_model_cost_config_cache));
    });
}

bool rasn_llm_agent_service::provider_needs_guards() const
{
    // Overload and failure protection only matter for providers that perform
    // network I/O and can therefore fail, hang, or be overwhelmed at a remote
    // endpoint. Providers that run entirely in this process (the deterministic
    // simulator, the workflow service-graph bridge, and test fakes) report
    // in_process() == true and bypass the breaker and admission control, keeping
    // existing behavior unchanged. Note this is distinct from describe().local:
    // loopback HTTP providers such as Ollama, llama.cpp, and LM Studio are
    // "local" but still issue curl/HTTP requests, so they are guarded.
    return _provider != nullptr && !_provider->in_process();
}

bool rasn_llm_agent_service::model_breaker_is_open(const std::string &provider,
                                                   const agent_task &task,
                                                   nucleus_runtime &runtime,
                                                   llm_response *fast_fail)
{
    ensure_model_breaker_config();
    circuit_breaker &breaker = _model_breakers.get(provider);
    if (!breaker.is_open(::dsn_now_ms()))
    {
        return false;
    }
    // The breaker is open and still cooling down: the request will be
    // short-circuited regardless of the other gates, so fast-fail it here -- ahead
    // of admission and the rate limiter -- so an open breaker (broken dependency)
    // wins over an admission/rate rejection (overload/quota) and does not waste an
    // admission slot or rate token. This precheck is non-mutating and leaves the
    // one-shot half-open probe intact for the authoritative model_breaker_admit()
    // once the cooldown elapses and the request has passed the other gates.
    dwarn("rASN model circuit breaker open for provider=%s; short-circuiting request before admission",
          provider.c_str());
    runtime.record_model_breaker_short_circuit(task, provider, to_string(breaker_state::open));
    if (fast_fail != nullptr)
    {
        *fast_fail = rpc_error_response("model circuit breaker " + std::string(to_string(breaker_state::open)) +
                                        " for provider " + provider + "; request short-circuited");
    }
    return true;
}

bool rasn_llm_agent_service::model_breaker_admit(const std::string &provider,
                                                 const agent_task &task,
                                                 nucleus_runtime &runtime,
                                                 llm_response *fast_fail)
{
    ensure_model_breaker_config();
    circuit_breaker &breaker = _model_breakers.get(provider);
    const breaker_decision decision = breaker.allow(::dsn_now_ms());
    if (decision.half_open_probe)
    {
        dinfo("rASN model circuit breaker half-open: admitting probe for provider=%s", provider.c_str());
    }
    if (decision.allowed)
    {
        return true;
    }
    dwarn("rASN model circuit breaker %s for provider=%s; short-circuiting request",
          to_string(decision.state),
          provider.c_str());
    runtime.record_model_breaker_short_circuit(task, provider, to_string(decision.state));
    if (fast_fail != nullptr)
    {
        *fast_fail = rpc_error_response("model circuit breaker " + std::string(to_string(decision.state)) +
                                        " for provider " + provider + "; request short-circuited");
    }
    return false;
}

void rasn_llm_agent_service::model_breaker_report(const std::string &provider,
                                                  const agent_task &task,
                                                  nucleus_runtime &runtime,
                                                  bool ok)
{
    circuit_breaker &breaker = _model_breakers.get(provider);
    if (breaker.report(ok, ::dsn_now_ms()))
    {
        dwarn("rASN model circuit breaker opened for provider=%s after %u consecutive failures",
              provider.c_str(),
              static_cast<unsigned int>(breaker.consecutive_failures()));
        runtime.record_model_breaker_open(task, provider, breaker.consecutive_failures());
    }
}

std::vector<circuit_breaker_registry::entry> rasn_llm_agent_service::model_breaker_states() const
{
    return _model_breakers.snapshot();
}

admission_slot rasn_llm_agent_service::model_admission_admit(const std::string &provider,
                                                             const agent_task &task,
                                                             nucleus_runtime &runtime,
                                                             llm_response *fast_fail)
{
    ensure_model_admission_config();
    admission_gate &gate = _model_admission.get(provider);
    admission_slot slot = gate.try_admit();
    if (!slot.admitted())
    {
        dwarn("rASN model admission control rejected request for provider=%s; concurrency limit %u reached",
              provider.c_str(),
              static_cast<unsigned int>(slot.limit()));
        runtime.record_model_admission_rejected(task, provider, slot.in_flight(), slot.limit());
        if (fast_fail != nullptr)
        {
            *fast_fail = rpc_error_response("model admission control rejected request for provider " + provider +
                                            "; concurrency limit " + std::to_string(slot.limit()) + " reached");
        }
    }
    // The graceful backpressure delay carried by the slot is intentionally NOT
    // applied here. The caller applies it via apply_model_backpressure() only
    // after the rate limiter and circuit breaker have also admitted the request,
    // so a request the breaker short-circuits neither sleeps nor holds its slot
    // through a sleep.
    return slot;
}

rate_decision rasn_llm_agent_service::model_rate_acquire(const std::string &provider,
                                                         const agent_task &task,
                                                         nucleus_runtime &runtime,
                                                         llm_response *fast_fail)
{
    ensure_model_rate_config();
    rate_limiter &limiter = _model_rate.get(provider);
    const rate_decision decision = limiter.try_acquire(::dsn_now_ms());
    if (!decision.allowed)
    {
        dwarn("rASN model rate limiter rejected request for provider=%s; rate %u req/min exceeded",
              provider.c_str(),
              static_cast<unsigned int>(decision.limit_per_min));
        runtime.record_model_rate_limited(task, provider, decision.limit_per_min);
        if (fast_fail != nullptr)
        {
            *fast_fail = rpc_error_response("model rate limiter rejected request for provider " + provider +
                                            "; rate " + std::to_string(decision.limit_per_min) +
                                            " req/min exceeded");
        }
    }
    // Like admission, the pacing delay carried by the decision is NOT applied
    // here; apply_model_backpressure() applies it after the breaker also admits.
    return decision;
}

void rasn_llm_agent_service::model_rate_refund(const std::string &provider)
{
    _model_rate.get(provider).refund();
}

rate_decision rasn_llm_agent_service::model_cost_acquire(const std::string &provider,
                                                         size_t prompt_chars,
                                                         const agent_task &task,
                                                         nucleus_runtime &runtime,
                                                         double *charged,
                                                         llm_response *fast_fail)
{
    ensure_model_cost_config();
    // Estimate the request's token charge from prompt size and meter it against
    // the provider's token bucket. The estimate is deterministic in prompt_chars,
    // so charging is replay-safe and never depends on the provider response.
    const double cost = estimate_prompt_cost_tokens(prompt_chars, _model_cost_config_cache);
    if (charged != nullptr)
    {
        *charged = cost;
    }
    rate_limiter &limiter = _model_cost.get(provider);
    const rate_decision decision = limiter.try_acquire(::dsn_now_ms(), cost);
    if (!decision.allowed)
    {
        const uint32_t estimated_tokens = saturating_estimated_token_count(cost);
        dwarn("rASN model cost budget rejected request for provider=%s; budget %u tokens/min exceeded "
              "(estimated %u tokens)",
              provider.c_str(),
              static_cast<unsigned int>(decision.limit_per_min),
              static_cast<unsigned int>(estimated_tokens));
        runtime.record_model_cost_limited(task, provider, decision.limit_per_min, estimated_tokens);
        if (fast_fail != nullptr)
        {
            *fast_fail = rpc_error_response("model cost budget rejected request for provider " + provider +
                                            "; budget " + std::to_string(decision.limit_per_min) +
                                            " tokens/min exceeded (estimated " +
                                            std::to_string(estimated_tokens) + " tokens)");
        }
    }
    // Like admission and the rate limiter, the pacing delay carried by the
    // decision is NOT applied here; apply_model_backpressure() applies it after
    // the breaker also admits.
    return decision;
}

void rasn_llm_agent_service::model_cost_refund(const std::string &provider, double charged)
{
    _model_cost.get(provider).refund(charged);
}

void rasn_llm_agent_service::apply_model_backpressure(const std::string &provider,
                                                      const agent_task &task,
                                                      nucleus_runtime &runtime,
                                                      const admission_slot &slot,
                                                      const rate_decision &rate,
                                                      const rate_decision &cost)
{
    const uint32_t admission_delay = slot.delay_ms();
    const uint32_t rate_delay = rate.delay_ms;
    const uint32_t cost_delay = cost.delay_ms;
    if (admission_delay > 0)
    {
        runtime.record_model_admission_delayed(task, provider, slot.in_flight(), admission_delay);
    }
    if (rate_delay > 0)
    {
        runtime.record_model_rate_delayed(task, provider, rate_delay);
    }
    if (cost_delay > 0)
    {
        runtime.record_model_cost_delayed(task, provider, cost_delay);
    }
    // Sleep once for the largest of the delays: a single wait that is >= each
    // governor's requested delay satisfies all of them (concurrency smoothing,
    // rate pacing, and cost/token pacing) without stacking them into additive
    // over-delay.
    const uint32_t delay = (std::max)((std::max)(admission_delay, rate_delay), cost_delay);
    if (delay > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
}

std::vector<admission_gate_registry::entry> rasn_llm_agent_service::model_admission_states() const
{
    return _model_admission.snapshot();
}

std::vector<rate_limiter_registry::entry> rasn_llm_agent_service::model_rate_states() const
{
    return _model_rate.snapshot();
}

std::vector<rate_limiter_registry::entry> rasn_llm_agent_service::model_cost_states() const
{
    return _model_cost.snapshot();
}

llm_response rasn_llm_agent_service::complete(const agent_completion_request &request, nucleus_runtime &runtime)
{
    const agent_request generic_request = make_model_agent_request(request, runtime.trace_id());
    std::string validation_error;
    if (!validate_agent_request(generic_request, &validation_error))
    {
        return rpc_error_response(validation_error);
    }

    llm_request llm;
    llm.task_id = request.task.id;
    llm.system_prompt = request.system_prompt;
    llm.user_prompt = request.user_prompt;
    llm.context = request.context;
    llm.timeout_ms = request.timeout_ms;
    llm.retry_budget = request.retry_budget;
    std::string replayed_response;
    if (runtime.replay_llm_response(request.task, _provider->name(), &replayed_response))
    {
        llm_response response;
        response.ok = true;
        response.text = replayed_response;
        return response;
    }

    // Guard network providers with overload, rate, and failure protection: an
    // admission bulkhead keeps a healthy endpoint from being overwhelmed, a
    // client-side rate limiter paces calls under the provider's quota, and a
    // circuit breaker fast-fails a failing endpoint before its own retry loop.
    // An already-open breaker is prechecked first (non-mutating) so a broken
    // dependency fast-fails ahead of an admission/rate rejection. Otherwise
    // admission and the rate limiter reserve first (and may reject) so that the
    // breaker -- which consumes a one-shot half-open probe -- is the last gate; a
    // request rejected by admission or the rate limiter therefore never wastes a
    // breaker probe. Their pacing delays are applied only after the breaker also
    // admits, so a short-circuited request neither sleeps nor holds its admission
    // slot through a sleep. The RAII slot releases on scope exit, covering the
    // whole provider call otherwise.
    const std::string provider_name = _provider->name();
    const bool guarded = provider_needs_guards();
    admission_slot admission;
    rate_decision rate;
    rate_decision cost;
    if (guarded)
    {
        llm_response fast_fail;
        if (model_breaker_is_open(provider_name, request.task, runtime, &fast_fail))
        {
            return fast_fail;
        }
        admission = model_admission_admit(provider_name, request.task, runtime, &fast_fail);
        if (!admission.admitted())
        {
            return fast_fail;
        }
        rate = model_rate_acquire(provider_name, request.task, runtime, &fast_fail);
        if (!rate.allowed)
        {
            return fast_fail;
        }
        double cost_charged = 0.0;
        cost = model_cost_acquire(
            provider_name, prompt_cost_chars(llm), request.task, runtime, &cost_charged, &fast_fail);
        if (!cost.allowed)
        {
            // The token/cost budget rejected this request after the rate token was
            // taken; refund the request-count token so a cost rejection does not
            // also drain the request-rate quota. The RAII admission slot releases
            // itself on return.
            model_rate_refund(provider_name);
            return fast_fail;
        }
        if (!model_breaker_admit(provider_name, request.task, runtime, &fast_fail))
        {
            // The breaker short-circuited this request after the rate token and the
            // cost charge were taken, so it will not reach the provider; refund both
            // so a breaker fast-fail does not drain the quota or the token budget or
            // delay the eventual recovery probe. The RAII admission slot releases
            // itself on return.
            model_rate_refund(provider_name);
            model_cost_refund(provider_name, cost_charged);
            return fast_fail;
        }
        apply_model_backpressure(provider_name, request.task, runtime, admission, rate, cost);
    }

    const llm_response response = redact_llm_response(_provider->complete(redact_llm_request(llm), runtime));
    if (guarded)
    {
        model_breaker_report(provider_name, request.task, runtime, response.ok);
    }
    const agent_response generic_response = make_agent_response_from_llm(generic_request, response);
    if (!validate_agent_response(generic_response, &validation_error))
    {
        return rpc_error_response(validation_error);
    }
    return response;
}

llm_response rasn_llm_agent_service::complete_streaming(const agent_completion_request &request,
                                                        nucleus_runtime &runtime,
                                                        const llm_stream_callback &on_chunk)
{
    const agent_request generic_request = make_model_agent_request(request, runtime.trace_id());
    std::string validation_error;
    if (!validate_agent_request(generic_request, &validation_error))
    {
        return rpc_error_response(validation_error);
    }

    llm_request llm;
    llm.task_id = request.task.id;
    llm.system_prompt = request.system_prompt;
    llm.user_prompt = request.user_prompt;
    llm.context = request.context;
    llm.timeout_ms = request.timeout_ms;
    llm.retry_budget = request.retry_budget;

    std::string replayed_response;
    if (runtime.replay_llm_response(request.task, _provider->name(), &replayed_response))
    {
        llm_response response;
        response.ok = true;
        response.text = replayed_response;
        emit_llm_stream_chunks(request.task, _provider->name(), response.text, runtime, on_chunk);
        return response;
    }

    const std::string provider_name = _provider->name();
    const bool guarded = provider_needs_guards();
    admission_slot admission;
    rate_decision rate;
    rate_decision cost;
    if (guarded)
    {
        llm_response fast_fail;
        // Open-breaker precheck wins over admission/rate rejection; see complete().
        if (model_breaker_is_open(provider_name, request.task, runtime, &fast_fail))
        {
            return fast_fail;
        }
        admission = model_admission_admit(provider_name, request.task, runtime, &fast_fail);
        if (!admission.admitted())
        {
            return fast_fail;
        }
        rate = model_rate_acquire(provider_name, request.task, runtime, &fast_fail);
        if (!rate.allowed)
        {
            return fast_fail;
        }
        double cost_charged = 0.0;
        cost = model_cost_acquire(
            provider_name, prompt_cost_chars(llm), request.task, runtime, &cost_charged, &fast_fail);
        if (!cost.allowed)
        {
            // See complete(): refund the rate token when the cost budget rejects so
            // a cost rejection does not also drain the request-rate quota.
            model_rate_refund(provider_name);
            return fast_fail;
        }
        if (!model_breaker_admit(provider_name, request.task, runtime, &fast_fail))
        {
            // See complete(): refund both the rate token and the cost charge taken
            // above when the breaker short-circuits, so a fast-failed request does
            // not drain the quota or the token budget.
            model_rate_refund(provider_name);
            model_cost_refund(provider_name, cost_charged);
            return fast_fail;
        }
        apply_model_backpressure(provider_name, request.task, runtime, admission, rate, cost);
    }

    const llm_response response =
        redact_llm_response(_provider->complete_streaming(redact_llm_request(llm), runtime, on_chunk));
    if (guarded)
    {
        model_breaker_report(provider_name, request.task, runtime, response.ok);
    }
    const agent_response generic_response = make_agent_response_from_llm(generic_request, response);
    if (!validate_agent_response(generic_response, &validation_error))
    {
        return rpc_error_response(validation_error);
    }
    return response;
}

agent_response rasn_llm_agent_service::invoke(const agent_request &request, nucleus_runtime &runtime)
{
    agent_response rejection;
    if (!begin_request(request, &rejection))
    {
        return rejection;
    }
    scoped_agent_request request_scope(*this, request);
    if (request.capability != "model.complete")
    {
        return reject(request, "routing", "unsupported_capability", "model agent does not support capability: " + request.capability, false);
    }

    const agent_completion_request completion = make_completion_request_from_agent(request);
    const llm_response response = complete(completion, runtime);
    if (is_cancelled(request.request_id))
    {
        const agent_response cancelled = cancelled_response(request);
        runtime.record_failure(
            request.task, "lifecycle", "request_cancelled", cancelled.error.message, false, descriptor().agent_id);
        return cancelled;
    }
    return make_agent_response_from_llm(request, response);
}

std::string rasn_llm_agent_service::summary(const nucleus_runtime &runtime) const
{
    return model_gateway_summary(describe_model_provider(), runtime);
}

rasn_tool_agent_service::rasn_tool_agent_service()
    : agent_runtime("rasn.tool.agent", "rasn.tool.agent"), _tools(create_default_tool_provider())
{
    add_capability(make_capability("tool.describe", "agent_request", "agent_response", "read_only"));
    add_capability(make_capability("tool.run", "agent_request", "agent_response", "policy_gated"));
}

void rasn_tool_agent_service::start()
{
    dinfo("starting rasn.tool.agent service");
    agent_runtime::start();
}

void rasn_tool_agent_service::stop()
{
    dinfo("stopping rasn.tool.agent service");
    agent_runtime::stop();
}

void rasn_tool_agent_service::set_tool_provider(std::unique_ptr<agent_tool_provider> tools)
{
    dassert(tools != nullptr, "rASN tool provider cannot be null");
    ::dsn::service::zauto_lock guard(_tool_lock);
    _tools = std::shared_ptr<agent_tool_provider>(std::move(tools));
    dinfo("installed rASN tool provider");
}

std::shared_ptr<agent_tool_provider> rasn_tool_agent_service::current_tool_provider() const
{
    ::dsn::service::zauto_lock guard(_tool_lock);
    return _tools;
}

std::string rasn_tool_agent_service::describe_tools() const
{
    const std::shared_ptr<agent_tool_provider> tools = current_tool_provider();
    if (tools == nullptr)
    {
        return "no tool provider registered";
    }
    return tools->describe_tools();
}

void rasn_tool_agent_service::ensure_tool_admission_config() const
{
    std::call_once(_tool_admission_config_once,
                   [this] { _tool_admission.set_config(read_tool_admission_config()); });
}

void rasn_tool_agent_service::ensure_tool_rate_config() const
{
    std::call_once(_tool_rate_config_once,
                   [this] { _tool_rate.set_config(read_tool_rate_config()); });
}

admission_slot rasn_tool_agent_service::tool_admission_admit(const std::string &tool,
                                                             const agent_task &task,
                                                             nucleus_runtime &runtime,
                                                             tool_result *fast_fail) const
{
    ensure_tool_admission_config();
    admission_gate &gate = _tool_admission.get(tool);
    admission_slot slot = gate.try_admit();
    if (!slot.admitted())
    {
        dwarn("rASN tool admission control rejected invocation for tool=%s; concurrency limit %u reached",
              tool.c_str(),
              static_cast<unsigned int>(slot.limit()));
        runtime.record_tool_admission_rejected(task, tool, slot.in_flight(), slot.limit());
        if (fast_fail != nullptr)
        {
            *fast_fail = rpc_error_tool_result("tool admission control rejected invocation for tool " + tool +
                                               "; concurrency limit " + std::to_string(slot.limit()) + " reached");
        }
    }
    return slot;
}

rate_decision rasn_tool_agent_service::tool_rate_acquire(const std::string &tool,
                                                         const agent_task &task,
                                                         nucleus_runtime &runtime,
                                                         tool_result *fast_fail) const
{
    ensure_tool_rate_config();
    rate_limiter &limiter = _tool_rate.get(tool);
    const rate_decision decision = limiter.try_acquire(::dsn_now_ms());
    if (!decision.allowed)
    {
        dwarn("rASN tool rate limiter rejected invocation for tool=%s; rate %u req/min exceeded",
              tool.c_str(),
              static_cast<unsigned int>(decision.limit_per_min));
        runtime.record_tool_rate_limited(task, tool, decision.limit_per_min);
        if (fast_fail != nullptr)
        {
            *fast_fail = rpc_error_tool_result("tool rate limiter rejected invocation for tool " + tool +
                                               "; rate " + std::to_string(decision.limit_per_min) +
                                               " req/min exceeded");
        }
    }
    return decision;
}

void rasn_tool_agent_service::apply_tool_backpressure(const std::string &tool,
                                                      const agent_task &task,
                                                      nucleus_runtime &runtime,
                                                      const admission_slot &slot,
                                                      const rate_decision &rate) const
{
    const uint32_t admission_delay = slot.delay_ms();
    const uint32_t rate_delay = rate.delay_ms;
    if (admission_delay > 0)
    {
        runtime.record_tool_admission_delayed(task, tool, slot.in_flight(), admission_delay);
    }
    if (rate_delay > 0)
    {
        runtime.record_tool_rate_delayed(task, tool, rate_delay);
    }
    const uint32_t delay = (std::max)(admission_delay, rate_delay);
    if (delay > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
}

std::string rasn_tool_agent_service::tool_resilience_report() const
{
    return format_tool_admission_states(_tool_admission.snapshot()) + "\n" +
           format_tool_rate_states(_tool_rate.snapshot());
}

tool_result rasn_tool_agent_service::run_tool(const std::string &name,
                                              const std::vector<std::string> &args,
                                              nucleus_runtime &runtime,
                                              const agent_task &task,
                                              const std::vector<std::string> &policy_labels) const
{
    // Record tool latency at the point where every tool execution converges:
    // the CodePilot CLI path (rasn_service_graph::invoke -> coordinator), the
    // workflow facade (rasn_service_graph::run_tool), the RPC server path, and
    // direct callers all reach this method. Measuring here -- rather than at a
    // single higher-level facade -- ensures the primary CLI path is counted, and
    // an RAII recorder covers every early return (no provider, validation,
    // replay hit, replay miss, policy denial, success).
    const uint64_t tool_start_ms = ::dsn_now_ms();
    struct tool_latency_recorder
    {
        uint64_t start_ms;
        ~tool_latency_recorder()
        {
            const uint64_t now_ms = ::dsn_now_ms();
            metrics_registry::instance().observe_tool_latency_ms(now_ms >= start_ms ? now_ms - start_ms : 0);
        }
    } latency_recorder{tool_start_ms};

    const std::shared_ptr<agent_tool_provider> tools = current_tool_provider();
    if (tools == nullptr)
    {
        return rpc_error_tool_result("no tool provider registered");
    }

    agent_tool_request request;
    request.name = name;
    request.args = args;
    request.task = task;
    request.policy_labels = policy_labels;
    const agent_request generic_request = make_tool_agent_request(request, runtime.trace_id());
    std::string validation_error;
    if (!validate_agent_request(generic_request, &validation_error))
    {
        return rpc_error_tool_result(validation_error);
    }

    const policy_request policy = make_policy_request(name, args, task, policy_labels);
    const std::string arguments = join_tool_args_for_policy(args);
    const tool_side_effect side_effect = classify_tool_side_effect(name);
    bool replay_ok = false;
    std::string replay_result;
    if (runtime.replay_tool_call(task, name, arguments, &replay_ok, &replay_result))
    {
        tool_result result;
        result.ok = replay_ok;
        if (result.ok)
        {
            result.output = replay_result;
        }
        else
        {
            result.error = replay_result;
        }
        record_external_effect_if_needed(
            runtime, task, side_effect, name, arguments, result.ok ? "replayed.ok" : "replayed.error");
        runtime.record_tool_call(task, name, arguments, result.ok, result.ok ? result.output : result.error);
        return result;
    }
    if (runtime.replay_enabled() && side_effect != tool_side_effect::read_only)
    {
        const std::string reason = "replay missing recorded side-effect tool result";
        record_external_effect_if_needed(runtime, task, side_effect, name, arguments, "replay_miss");
        runtime.record_tool_call(task, name, arguments, false, reason);
        runtime.record_failure(task, "replay", "missing_tool_result", reason, false, "rasn.tool.agent");
        return rpc_error_tool_result(reason);
    }

    const policy_decision decision = global_policy_manager().evaluate(policy);
    if (!decision.allowed)
    {
        dwarn("denied rASN tool=%s side_effect=%s reason=%s",
              name.c_str(),
              decision.side_effect.c_str(),
              decision.reason.c_str());
        record_external_effect_if_needed(runtime, task, side_effect, name, arguments, "policy_denied");
        runtime.record_tool_call(task, name, arguments, false, decision.reason);
        runtime.record_failure(
            task, "policy", "tool_denied", "policy denied tool '" + name + "': " + decision.reason, false, "rasn.tool.agent");
        return rpc_error_tool_result("policy denied tool '" + name + "': " + decision.reason);
    }
    dinfo("allowed rASN tool=%s side_effect=%s reason=%s",
          name.c_str(),
          decision.side_effect.c_str(),
          decision.reason.c_str());

    tool_result fast_fail;
    admission_slot admission = tool_admission_admit(name, task, runtime, &fast_fail);
    if (!admission.admitted())
    {
        record_external_effect_if_needed(runtime, task, side_effect, name, arguments, "admission_rejected");
        runtime.record_tool_call(task, name, arguments, false, fast_fail.error);
        runtime.record_failure(task, "tool", "admission_rejected", fast_fail.error, false, "rasn.tool.agent");
        return fast_fail;
    }

    rate_decision rate = tool_rate_acquire(name, task, runtime, &fast_fail);
    if (!rate.allowed)
    {
        record_external_effect_if_needed(runtime, task, side_effect, name, arguments, "rate_limited");
        runtime.record_tool_call(task, name, arguments, false, fast_fail.error);
        runtime.record_failure(task, "tool", "rate_limited", fast_fail.error, false, "rasn.tool.agent");
        return fast_fail;
    }
    apply_tool_backpressure(name, task, runtime, admission, rate);

    const tool_result result = global_policy_manager().apply_tool_output_bounds(
        name, task, tools->run_with_policy_labels(name, args, policy_labels, runtime, task));
    record_external_effect_if_needed(
        runtime, task, side_effect, name, arguments, result.ok ? "committed.ok" : "committed.error");
    runtime.record_tool_call(task, name, arguments, result.ok, result.ok ? result.output : result.error);
    if (!result.ok)
    {
        runtime.record_failure(task, "tool", name, result.error, false, "rasn.tool.agent");
    }
    const agent_response generic_response = make_agent_response_from_tool(generic_request, result);
    if (!validate_agent_response(generic_response, &validation_error))
    {
        return rpc_error_tool_result(validation_error);
    }
    return result;
}

agent_response rasn_tool_agent_service::invoke(const agent_request &request, nucleus_runtime &runtime) const
{
    agent_response rejection;
    if (!begin_request(request, &rejection))
    {
        return rejection;
    }
    scoped_agent_request request_scope(*this, request);
    if (request.capability == "tool.describe")
    {
        agent_response response;
        response.request_id = request.request_id;
        response.trace_id = request.trace_id;
        response.ok = true;
        response.output = describe_tools();
        return response;
    }
    if (!coordinator_router::is_tool_capability(request.capability))
    {
        return reject(request, "routing", "unsupported_capability", "tool agent does not support capability: " + request.capability, false);
    }

    const agent_tool_request tool = make_tool_request_from_agent(request);
    const tool_result result = run_tool(tool.name, tool.args, runtime, tool.task, tool.policy_labels);
    if (is_cancelled(request.request_id))
    {
        const agent_response cancelled = cancelled_response(request);
        runtime.record_failure(
            request.task, "lifecycle", "request_cancelled", cancelled.error.message, false, descriptor().agent_id);
        return cancelled;
    }
    return make_agent_response_from_tool(request, result);
}

rasn_coordinator_service::rasn_coordinator_service(rasn_llm_agent_service &llm_agent,
                                                   rasn_tool_agent_service &tool_agent)
    : agent_runtime("rasn.coordinator", "rasn.coordinator"), _llm_agent(llm_agent), _tool_agent(tool_agent)
{
    add_capability(make_capability("coordinate.invoke", "agent_request", "agent_response", "orchestration"));
    add_capability(make_capability("coordinate.run_tool", "agent_request", "agent_response", "policy_gated"));
}

void rasn_coordinator_service::start()
{
    dinfo("starting rasn.coordinator service");
    agent_runtime::start();
}

void rasn_coordinator_service::stop()
{
    dinfo("stopping rasn.coordinator service");
    agent_runtime::stop();
}

void rasn_coordinator_service::ensure_remote_agent_breaker_config()
{
    std::call_once(_remote_agent_breaker_config_once,
                   [this] { _remote_agent_breakers.set_config(read_remote_agent_breaker_config()); });
}

void rasn_coordinator_service::ensure_remote_agent_admission_config()
{
    std::call_once(_remote_agent_admission_config_once,
                   [this] { _remote_agent_admission.set_config(read_remote_agent_admission_config()); });
}

void rasn_coordinator_service::ensure_remote_agent_rate_config()
{
    std::call_once(_remote_agent_rate_config_once,
                   [this] { _remote_agent_rate.set_config(read_remote_agent_rate_config()); });
}

bool rasn_coordinator_service::remote_agent_breaker_is_open(const agent_descriptor &agent,
                                                           const agent_request &request,
                                                           nucleus_runtime &runtime,
                                                           agent_response *fast_fail)
{
    ensure_remote_agent_breaker_config();
    const std::string key = remote_agent_key(agent);
    circuit_breaker &breaker = _remote_agent_breakers.get(key);
    if (!breaker.is_open(::dsn_now_ms()))
    {
        return false;
    }
    dwarn("rASN remote-agent circuit breaker open for agent=%s; short-circuiting dispatch before admission",
          key.c_str());
    runtime.record_remote_agent_breaker_short_circuit(request.task, key, to_string(breaker_state::open));
    if (fast_fail != nullptr)
    {
        *fast_fail = remote_agent_gate_response(
            request,
            "remote_agent",
            "remote_agent_breaker_open",
            "remote-agent circuit breaker open for agent " + key + "; request short-circuited",
            false,
            descriptor().agent_id);
    }
    return true;
}

bool rasn_coordinator_service::remote_agent_breaker_admit(const agent_descriptor &agent,
                                                         const agent_request &request,
                                                         nucleus_runtime &runtime,
                                                         agent_response *fast_fail)
{
    ensure_remote_agent_breaker_config();
    const std::string key = remote_agent_key(agent);
    circuit_breaker &breaker = _remote_agent_breakers.get(key);
    const breaker_decision decision = breaker.allow(::dsn_now_ms());
    if (decision.half_open_probe)
    {
        dinfo("rASN remote-agent circuit breaker half-open: admitting probe for agent=%s", key.c_str());
    }
    if (decision.allowed)
    {
        return true;
    }
    dwarn("rASN remote-agent circuit breaker %s for agent=%s; short-circuiting dispatch",
          to_string(decision.state),
          key.c_str());
    runtime.record_remote_agent_breaker_short_circuit(request.task, key, to_string(decision.state));
    if (fast_fail != nullptr)
    {
        *fast_fail = remote_agent_gate_response(
            request,
            "remote_agent",
            "remote_agent_breaker_open",
            "remote-agent circuit breaker " + std::string(to_string(decision.state)) +
                " for agent " + key + "; request short-circuited",
            false,
            descriptor().agent_id);
    }
    return false;
}

void rasn_coordinator_service::remote_agent_breaker_report(const agent_descriptor &agent,
                                                          const agent_task &task,
                                                          nucleus_runtime &runtime,
                                                          bool ok)
{
    const std::string key = remote_agent_key(agent);
    circuit_breaker &breaker = _remote_agent_breakers.get(key);
    if (breaker.report(ok, ::dsn_now_ms()))
    {
        dwarn("rASN remote-agent circuit breaker opened for agent=%s after %u consecutive retryable failures",
              key.c_str(),
              static_cast<unsigned int>(breaker.consecutive_failures()));
        runtime.record_remote_agent_breaker_open(task, key, breaker.consecutive_failures());
    }
}

admission_slot rasn_coordinator_service::remote_agent_admission_admit(const agent_descriptor &agent,
                                                                      const agent_request &request,
                                                                      nucleus_runtime &runtime,
                                                                      agent_response *fast_fail)
{
    ensure_remote_agent_admission_config();
    const std::string key = remote_agent_key(agent);
    admission_gate &gate = _remote_agent_admission.get(key);
    admission_slot slot = gate.try_admit();
    if (!slot.admitted())
    {
        dwarn("rASN remote-agent admission control rejected dispatch for agent=%s; concurrency limit %u reached",
              key.c_str(),
              static_cast<unsigned int>(slot.limit()));
        runtime.record_remote_agent_admission_rejected(request.task, key, slot.in_flight(), slot.limit());
        if (fast_fail != nullptr)
        {
            *fast_fail = remote_agent_gate_response(
                request,
                "remote_agent",
                "remote_agent_admission_rejected",
                "remote-agent admission control rejected dispatch for agent " + key +
                    "; concurrency limit " + std::to_string(slot.limit()) + " reached",
                false,
                descriptor().agent_id);
        }
    }
    return slot;
}

rate_decision rasn_coordinator_service::remote_agent_rate_acquire(const agent_descriptor &agent,
                                                                  const agent_request &request,
                                                                  nucleus_runtime &runtime,
                                                                  agent_response *fast_fail)
{
    ensure_remote_agent_rate_config();
    const std::string key = remote_agent_key(agent);
    rate_limiter &limiter = _remote_agent_rate.get(key);
    const rate_decision decision = limiter.try_acquire(::dsn_now_ms());
    if (!decision.allowed)
    {
        dwarn("rASN remote-agent rate limiter rejected dispatch for agent=%s; rate %u req/min exceeded",
              key.c_str(),
              static_cast<unsigned int>(decision.limit_per_min));
        runtime.record_remote_agent_rate_limited(request.task, key, decision.limit_per_min);
        if (fast_fail != nullptr)
        {
            *fast_fail = remote_agent_gate_response(
                request,
                "remote_agent",
                "remote_agent_rate_limited",
                "remote-agent rate limiter rejected dispatch for agent " + key +
                    "; rate " + std::to_string(decision.limit_per_min) + " req/min exceeded",
                false,
                descriptor().agent_id);
        }
    }
    return decision;
}

void rasn_coordinator_service::remote_agent_rate_refund(const agent_descriptor &agent)
{
    _remote_agent_rate.get(remote_agent_key(agent)).refund();
}

void rasn_coordinator_service::apply_remote_agent_backpressure(const agent_descriptor &agent,
                                                              const agent_task &task,
                                                              nucleus_runtime &runtime,
                                                              const admission_slot &slot,
                                                              const rate_decision &rate)
{
    const std::string key = remote_agent_key(agent);
    const uint32_t admission_delay = slot.delay_ms();
    const uint32_t rate_delay = rate.delay_ms;
    if (admission_delay > 0)
    {
        runtime.record_remote_agent_admission_delayed(task, key, slot.in_flight(), admission_delay);
    }
    if (rate_delay > 0)
    {
        runtime.record_remote_agent_rate_delayed(task, key, rate_delay);
    }
    const uint32_t delay = (std::max)(admission_delay, rate_delay);
    if (delay > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
}

agent_response rasn_coordinator_service::invoke_remote_agent(const agent_request &request,
                                                            nucleus_runtime &runtime,
                                                            const agent_descriptor &agent)
{
    ::dsn::rpc_address address;
    std::string address_error;
    if (!coordinator_router::validate_remote_endpoint(agent, &address, &address_error))
    {
        runtime.record_remote_agent_endpoint_invalid(request.task, remote_agent_key(agent), address_error);
        return remote_agent_gate_response(
            request, "routing", "invalid_agent_endpoint", address_error, false, descriptor().agent_id);
    }

    agent_response fast_fail;
    if (remote_agent_breaker_is_open(agent, request, runtime, &fast_fail))
    {
        return fast_fail;
    }
    admission_slot admission = remote_agent_admission_admit(agent, request, runtime, &fast_fail);
    if (!admission.admitted())
    {
        return fast_fail;
    }
    rate_decision rate = remote_agent_rate_acquire(agent, request, runtime, &fast_fail);
    if (!rate.allowed)
    {
        return fast_fail;
    }
    if (!remote_agent_breaker_admit(agent, request, runtime, &fast_fail))
    {
        remote_agent_rate_refund(agent);
        return fast_fail;
    }
    apply_remote_agent_backpressure(agent, request.task, runtime, admission, rate);

    const agent_response response = coordinator_router::invoke_remote(request, agent, address, descriptor().agent_id);
    remote_agent_breaker_report(agent, request.task, runtime, remote_agent_response_is_dependency_success(response));
    return response;
}

std::string rasn_coordinator_service::remote_agent_resilience_report() const
{
    return format_remote_agent_breaker_states(_remote_agent_breakers.snapshot()) + "\n" +
           format_remote_agent_admission_states(_remote_agent_admission.snapshot()) + "\n" +
           format_remote_agent_rate_states(_remote_agent_rate.snapshot());
}

void rasn_coordinator_service::ensure_overload_config() const
{
    // admission_gate and rate_limiter hold a mutex (non-copyable, non-movable) and
    // expose no set_config, so the process-wide singletons are lazily constructed
    // under a once_flag on first use. Members are mutable so the const report path
    // (`rasn.resilience` / observe resilience) can trigger the same lazy init.
    std::call_once(_overload_config_once, [this] {
        _overload_admission.reset(new admission_gate(read_overload_admission_config()));
        _overload_rate.reset(new rate_limiter(read_overload_rate_config()));
    });
}

admission_slot rasn_coordinator_service::overload_admit(const agent_request &request,
                                                        nucleus_runtime &runtime,
                                                        agent_response *fast_fail)
{
    ensure_overload_config();
    admission_slot slot = _overload_admission->try_admit();
    if (!slot.admitted())
    {
        dwarn("rASN process-wide overload admission rejected operation; concurrency limit %u reached",
              static_cast<unsigned int>(slot.limit()));
        runtime.record_overload_admission_rejected(request.task, slot.in_flight(), slot.limit());
        if (fast_fail != nullptr)
        {
            *fast_fail = overload_gate_response(
                request,
                "overload_admission_rejected",
                "process-wide overload admission rejected operation; concurrency limit " +
                    std::to_string(slot.limit()) + " reached",
                descriptor().agent_id);
        }
    }
    return slot;
}

rate_decision rasn_coordinator_service::overload_rate_acquire(const agent_request &request,
                                                              nucleus_runtime &runtime,
                                                              agent_response *fast_fail)
{
    ensure_overload_config();
    const rate_decision decision = _overload_rate->try_acquire(::dsn_now_ms());
    if (!decision.allowed)
    {
        dwarn("rASN process-wide overload rate limiter rejected operation; rate %u req/min exceeded",
              static_cast<unsigned int>(decision.limit_per_min));
        runtime.record_overload_rate_limited(request.task, decision.limit_per_min);
        if (fast_fail != nullptr)
        {
            *fast_fail = overload_gate_response(
                request,
                "overload_rate_limited",
                "process-wide overload rate limiter rejected operation; rate " +
                    std::to_string(decision.limit_per_min) + " req/min exceeded",
                descriptor().agent_id);
        }
    }
    return decision;
}

void rasn_coordinator_service::apply_overload_backpressure(const agent_task &task,
                                                           nucleus_runtime &runtime,
                                                           const admission_slot &slot,
                                                           const rate_decision &rate)
{
    const uint32_t admission_delay = slot.delay_ms();
    const uint32_t rate_delay = rate.delay_ms;
    if (admission_delay > 0)
    {
        runtime.record_overload_admission_delayed(task, slot.in_flight(), admission_delay);
    }
    if (rate_delay > 0)
    {
        runtime.record_overload_rate_delayed(task, rate_delay);
    }
    // Coalesce the two graceful delays: a single sleep of the larger value
    // satisfies both governors (mirrors the per-dependency backpressure path).
    const uint32_t delay = (std::max)(admission_delay, rate_delay);
    if (delay > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
}

void rasn_coordinator_service::overload_rate_refund()
{
    ensure_overload_config();
    _overload_rate->refund();
}

rasn_coordinator_service::overload_gate_hold
rasn_coordinator_service::enter_overload_gate(const agent_request &request, nucleus_runtime &runtime)
{
    overload_gate_hold hold;
    hold.slot = overload_admit(request, runtime, &hold.rejection);
    if (!hold.slot.admitted())
    {
        return hold;
    }
    const rate_decision rate = overload_rate_acquire(request, runtime, &hold.rejection);
    if (!rate.allowed)
    {
        // The admission slot auto-releases when `hold` is destroyed on the caller's
        // early return; a rejected rate decision reserved no token, so no refund.
        return hold;
    }
    apply_overload_backpressure(request.task, runtime, hold.slot, rate);
    hold.passed = true;
    return hold;
}

std::string rasn_coordinator_service::overload_resilience_report() const
{
    ensure_overload_config();
    const admission_config &adm = _overload_admission->config();
    const rate_limit_config &rate = _overload_rate->config();
    std::ostringstream oss;
    oss << "process-wide overload budget:";
    oss << "\n- admission: enabled=" << (adm.enabled ? "true" : "false")
        << " in_flight=" << _overload_admission->in_flight()
        << " max_concurrent_operations=" << adm.max_concurrency
        << " soft_concurrent_operations=" << adm.soft_concurrency
        << " max_backpressure_ms=" << adm.max_backpressure_ms;
    oss << "\n- rate: enabled=" << (rate.enabled ? "true" : "false")
        << " requests_per_min=" << rate.requests_per_min
        << " burst=" << rate.burst
        << " tokens=" << std::fixed << std::setprecision(2) << _overload_rate->cached_tokens();
    return oss.str();
}

llm_response rasn_coordinator_service::complete(const agent_completion_request &request, nucleus_runtime &runtime)
{
    const agent_request generic = make_model_agent_request(request, runtime.trace_id());
    return make_llm_response_from_agent(invoke(generic, runtime));
}

llm_response rasn_coordinator_service::complete_streaming(const agent_completion_request &request,
                                                          nucleus_runtime &runtime,
                                                          const llm_stream_callback &on_chunk)
{
    const agent_request generic = make_model_agent_request(request, runtime.trace_id());
    rasn_service_graph &services = global_rasn_services();
    const coordinator_route route =
        coordinator_router::resolve(generic, services.rpc_clients_enabled(), services.registry_address());
    if (!route.ok)
    {
        return make_llm_response_from_agent(route.error);
    }
    runtime.record_route_decision(generic.task, route.capability, route.agent.agent_id);

    if (!services.rpc_clients_enabled() && route.agent.agent_id == "rasn.llm.agent")
    {
        // The inline streaming fast path dispatches straight to the local model
        // agent (true token-by-token streaming) instead of buffering through
        // invoke(), so apply the same process-wide overload budget here; otherwise
        // interactive streaming prompts would bypass the global concurrency
        // bulkhead and rate ceiling. The admission slot is held for the whole
        // stream (released when `overload` is destroyed on return). Route was
        // already resolved above and this path is inline-only, so there is no
        // pre-dispatch short-circuit after the gate and thus no token to refund.
        overload_gate_hold overload = enter_overload_gate(generic, runtime);
        if (!overload.passed)
        {
            return make_llm_response_from_agent(overload.rejection);
        }
        return _llm_agent.complete_streaming(request, runtime, on_chunk);
    }

    const llm_response response = make_llm_response_from_agent(invoke(generic, runtime));
    if (response.ok)
    {
        emit_llm_stream_chunks(request.task, route.agent.agent_id, response.text, runtime, on_chunk);
    }
    return response;
}

tool_result rasn_coordinator_service::run_tool(const std::string &name,
                                               const std::vector<std::string> &args,
                                               nucleus_runtime &runtime,
                                               const agent_task &task)
{
    agent_tool_request request;
    request.name = name;
    request.args = args;
    request.task = task;
    const agent_request generic = make_tool_agent_request(request, runtime.trace_id());
    return make_tool_result_from_agent(invoke(generic, runtime));
}

agent_response rasn_coordinator_service::invoke(const agent_request &request, nucleus_runtime &runtime)
{
    agent_response rejection;
    if (!begin_request(request, &rejection))
    {
        return rejection;
    }
    scoped_agent_request request_scope(*this, request);

    // Seed the runtime-module trace scope from this operation's trace id so every
    // module RPC issued while coordinating shares one end-to-end trace (finding 1.4).
    // Falls back to the runtime's own trace id when the request carries none.
    rasn_runtime_trace_scope runtime_trace(request.trace_id.empty() ? runtime.trace_id()
                                                                     : request.trace_id);

    // Process-wide overload budget (Round 11): a single global concurrency
    // bulkhead + request-rate ceiling bounding total in-flight work across every
    // dependency (model, tool, remote-agent) in both inline and RPC modes.
    // Applied AFTER begin_request/scoped_agent_request so malformed or cancelled
    // requests never consume budget, and BEFORE routing so the whole process sheds
    // load uniformly at its outermost chokepoint -- including the registry query
    // that route resolution performs in RPC mode, which is itself dependency work
    // the budget must bound. invoke() is never re-entered on the same thread
    // (inline handlers call external providers; workflow node dispatch is a flat
    // sequential loop; RPC handlers run on separate threads), so one slot per
    // top-level operation counts each logical operation exactly once with no
    // self-deadlock. The admission slot is a local RAII held across retries and
    // released on every return path; the rate token is refunded on the only
    // pre-dispatch short-circuit (route resolution failure) so routing failures
    // cannot drain the budget. The same gate guards the inline streaming fast path
    // in complete_streaming(), so interactive prompts cannot bypass it. Defaults
    // are passthrough (opt-in caps).
    overload_gate_hold overload = enter_overload_gate(request, runtime);
    if (!overload.passed)
    {
        return overload.rejection;
    }

    rasn_service_graph &services = global_rasn_services();
    const coordinator_route route =
        coordinator_router::resolve(request, services.rpc_clients_enabled(), services.registry_address());
    if (!route.ok)
    {
        // Route resolution failed before any dependency was dispatched. Return the
        // overload rate token so repeated registry/routing failures cannot slowly
        // drain the global rate budget (the admission slot auto-releases when
        // `overload` goes out of scope on this return).
        overload_rate_refund();
        return route.error;
    }
    runtime.record_route_decision(request.task, route.capability, route.agent.agent_id);

    if (services.rpc_clients_enabled())
    {
        const agent_response response = coordinator_router::invoke_with_retries(
            request,
            runtime,
            route.agent,
            "coordinator.invoke",
            [request, route, this, &runtime](uint32_t) {
                if (is_cancelled(request.request_id))
                {
                    return cancelled_response(request);
                }
                return invoke_remote_agent(request, runtime, route.agent);
            });
        if (is_cancelled(request.request_id))
        {
            const agent_response cancelled = cancelled_response(request);
            runtime.record_failure(
                request.task, "lifecycle", "request_cancelled", cancelled.error.message, false, descriptor().agent_id);
            return cancelled;
        }
        return response;
    }

    const agent_response response = coordinator_router::invoke_with_retries(
        request,
        runtime,
        route.agent,
        "coordinator.invoke",
        [request, route, this, &runtime](uint32_t) {
            if (is_cancelled(request.request_id))
            {
                return cancelled_response(request);
            }
            if (route.agent.agent_id == "rasn.llm.agent")
            {
                return _llm_agent.invoke(request, runtime);
            }

            if (route.agent.agent_id == "rasn.tool.agent")
            {
                return _tool_agent.invoke(request, runtime);
            }

            return reject(request,
                          "routing",
                          "unsupported_inline_agent",
                          "inline coordinator has no local implementation for agent: " + route.agent.agent_id,
                          false);
        });
    if (is_cancelled(request.request_id))
    {
        const agent_response cancelled = cancelled_response(request);
        runtime.record_failure(
            request.task, "lifecycle", "request_cancelled", cancelled.error.message, false, descriptor().agent_id);
        return cancelled;
    }
    return response;
}

std::string rasn_coordinator_service::describe_topology() const
{
    return "rASN service graph:\n"
           "- rasn.codepilot: gateway service for rASN CodePilot commands.\n"
           "- rasn.coordinator: serverlet/clientlet gateway that routes RPC_RASN_AGENT_* calls.\n"
           "- rasn.llm.agent: serverlet handling generic agent invoke behind a provider interface.\n"
           "- rasn.tool.agent: serverlet handling generic agent invoke behind explicit opt-in policies.\n"
           "- rasn.state: serverlet/clientlet checkpoint service for namespaced agent state.\n"
           "- rasn.workflow: serverlet/clientlet workflow compiler and execution service.\n"
           "- rasn.observability: serverlet/clientlet trace, failure, and replay query service.\n"
           "Standalone mode uses the same services inline; rDSN mode routes through typed RPC task codes and ports.";
}

rasn_service_graph::rasn_service_graph()
    : _llm_agent(),
      _tool_agent(),
      _coordinator(_llm_agent, _tool_agent),
    _registry_address(config_service_address("registry", 27100)),
    _coordinator_address(config_service_address("coordinator", 27101)),
    _llm_agent_address(config_service_address("llm_agent", 27102)),
    _tool_agent_address(config_service_address("tool_agent", 27103)),
    _state_address(config_service_address("state", 27104)),
    _workflow_address(config_service_address("workflow", 27105)),
    _observability_address(config_service_address("observability", 27106)),
    _lifecycle_ref_count(0),
    _lifecycle_transitioning(false),
    _rpc_clients_enabled(false),
    _started(false)
{
    const char *trace_path_value = ::dsn_config_get_value_string(
        "rasn.runtime", "trace_file", "", "JSONL file for rASN runtime trace events");
    const std::string trace_path = trace_path_value == nullptr ? "" : trace_path_value;
    if (!trace_path.empty())
    {
        _runtime.set_trace_file(trace_path);
    }

    set_agent_service_endpoint(_coordinator, "coordinator", 27101);
    set_agent_service_endpoint(_llm_agent, "llm_agent", 27102);
    set_agent_service_endpoint(_tool_agent, "tool_agent", 27103);
}

void rasn_service_graph::start()
{
    while (true)
    {
        {
            ::dsn::service::zauto_lock guard(_lifecycle_lock);
            if (_started)
            {
                return;
            }
            if (!_lifecycle_transitioning)
            {
                _lifecycle_transitioning = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    start_unlocked();

    {
        ::dsn::service::zauto_lock guard(_lifecycle_lock);
        _started = true;
        _lifecycle_transitioning = false;
    }
}

void rasn_service_graph::stop()
{
    while (true)
    {
        {
            ::dsn::service::zauto_lock guard(_lifecycle_lock);
            if (_lifecycle_ref_count != 0)
            {
                dwarn("defer rASN service graph stop while %u lifecycle owners remain", _lifecycle_ref_count);
                return;
            }
            if (!_started && !_lifecycle_transitioning)
            {
                return;
            }
            if (!_lifecycle_transitioning)
            {
                _lifecycle_transitioning = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    stop_unlocked();

    {
        ::dsn::service::zauto_lock guard(_lifecycle_lock);
        _started = false;
        _lifecycle_transitioning = false;
    }
}

void rasn_service_graph::acquire()
{
    {
        ::dsn::service::zauto_lock guard(_lifecycle_lock);
        ++_lifecycle_ref_count;
    }
    start();
}

void rasn_service_graph::release()
{
    bool should_stop = false;
    {
        ::dsn::service::zauto_lock guard(_lifecycle_lock);
        if (_lifecycle_ref_count == 0)
        {
            dwarn("ignore unmatched rASN service graph release");
            return;
        }
        --_lifecycle_ref_count;
        should_stop = _lifecycle_ref_count == 0;
    }
    if (should_stop)
    {
        stop();
    }
}

bool rasn_service_graph::is_started() const
{
    ::dsn::service::zauto_lock guard(_lifecycle_lock);
    return _started;
}

uint32_t rasn_service_graph::lifecycle_ref_count() const
{
    ::dsn::service::zauto_lock guard(_lifecycle_lock);
    return _lifecycle_ref_count;
}

void rasn_service_graph::start_unlocked()
{
    if (g_rdsn_rpc_enabled && !_rpc_clients_enabled)
    {
        _rpc_clients_enabled = true;
        dinfo("enabled rASN RPC clients from service graph startup");
    }

    set_policy_state_writer(&service_graph_policy_state_writer);
    set_observability_state_writer(&service_graph_observability_state_writer);
    load_static_agents_from_config_once();
    _llm_agent.start();
    _tool_agent.start();
    _coordinator.start();

    std::string error;
    if (!global_agent_registry().register_agent(_llm_agent.descriptor(), &error))
    {
        dwarn("failed to register llm agent: %s", error.c_str());
    }
    if (!global_agent_registry().register_agent(_tool_agent.descriptor(), &error))
    {
        dwarn("failed to register tool agent: %s", error.c_str());
    }
    if (!global_agent_registry().register_agent(_coordinator.descriptor(), &error))
    {
        dwarn("failed to register coordinator agent: %s", error.c_str());
    }
    register_agents_with_registry_rpc();
    start_registry_heartbeat_timer();
    register_ops_commands_once();
}

void rasn_service_graph::register_ops_commands_once()
{
    metrics_registry::instance().ensure_core_counters();

    static std::once_flag once;
    std::call_once(once, [] {
        ::dsn::register_command(
            "rasn.metrics",
            "rasn.metrics - dump rASN runtime metrics",
            "rasn.metrics [text|prometheus|json] - dump rASN runtime metrics in the "
            "requested format (default text)",
            [](const ::dsn::safe_vector<::dsn::safe_string> &args) -> ::dsn::safe_string {
                std::string format = "text";
                if (!args.empty())
                {
                    format = std::string(args[0].c_str());
                }
                const metrics_snapshot snap = metrics_registry::instance().snapshot();
                std::string out;
                if (format == "prometheus" || format == "prom")
                {
                    out = snap.to_prometheus();
                }
                else if (format == "json")
                {
                    out = snap.to_json();
                }
                else
                {
                    out = snap.to_text();
                }
                return ::dsn::safe_string(out.c_str());
            });
        dinfo("registered rASN ops command: rasn.metrics");

        ::dsn::register_command(
            "rasn.resilience",
            "rasn.resilience - dump rASN dependency resilience state",
            "rasn.resilience - list each model provider's circuit-breaker state "
            "(closed|open|half_open) with consecutive failure count, its "
            "admission-control state (in-flight vs concurrency cap), and its "
            "rate-limiter state (requests/min, burst, available tokens), plus "
            "tool admission/rate limiter state and remote-agent dispatch guards",
            [](const ::dsn::safe_vector<::dsn::safe_string> &args) -> ::dsn::safe_string {
                (void)args;
                const std::string out = global_rasn_services().resilience_report();
                return ::dsn::safe_string(out.c_str());
            });
        dinfo("registered rASN ops command: rasn.resilience");
    });
}

void rasn_service_graph::stop_unlocked()
{
    cancel_registry_heartbeat_timer();
    unregister_agents_from_registry_rpc();

    _coordinator.stop();
    _tool_agent.stop();
    _llm_agent.stop();
    global_agent_registry().unregister_agent("rasn.coordinator");
    global_agent_registry().unregister_agent("rasn.tool.agent");
    global_agent_registry().unregister_agent("rasn.llm.agent");
    reset_policy_state_writer();
    reset_observability_state_writer();
    _started = false;
}

void rasn_service_graph::register_agents_with_registry_rpc()
{
    if (!_rpc_clients_enabled || !registry_dynamic_registration_enabled())
    {
        return;
    }

    rasn_registry_client registry(_registry_address);
    const std::chrono::milliseconds timeout = registry_registration_timeout();
    for (const agent_descriptor &descriptor : registered_service_agents(_llm_agent, _tool_agent, _coordinator))
    {
        ::dsn::error_code err;
        agent_response response;
        std::tie(err, response) = registry.register_sync(descriptor, timeout);
        if (err != ::dsn::ERR_OK)
        {
            dwarn("failed to register rASN agent %s through registry RPC: %s",
                  descriptor.agent_id.c_str(),
                  err.to_string());
        }
        else if (!response.ok)
        {
            dwarn("registry rejected rASN agent %s: %s",
                  descriptor.agent_id.c_str(),
                  response.error.message.c_str());
        }
    }
}

void rasn_service_graph::heartbeat_agents_to_registry()
{
    if (!_rpc_clients_enabled || !registry_dynamic_registration_enabled())
    {
        return;
    }

    rasn_registry_client registry(_registry_address);
    const std::chrono::milliseconds timeout = registry_registration_timeout();
    for (const agent_descriptor &descriptor : registered_service_agents(_llm_agent, _tool_agent, _coordinator))
    {
        ::dsn::error_code err;
        agent_response response;
        std::tie(err, response) = registry.heartbeat_sync(descriptor, timeout);
        if (err != ::dsn::ERR_OK)
        {
            dwarn("failed to heartbeat rASN agent %s through registry RPC: %s",
                  descriptor.agent_id.c_str(),
                  err.to_string());
            continue;
        }
        if (!response.ok)
        {
            dwarn("registry heartbeat rejected rASN agent %s: %s; retrying register",
                  descriptor.agent_id.c_str(),
                  response.error.message.c_str());
            std::tie(err, response) = registry.register_sync(descriptor, timeout);
            if (err != ::dsn::ERR_OK)
            {
                dwarn("failed to re-register rASN agent %s after heartbeat rejection: %s",
                      descriptor.agent_id.c_str(),
                      err.to_string());
            }
            else if (!response.ok)
            {
                dwarn("registry rejected rASN agent %s after heartbeat rejection: %s",
                      descriptor.agent_id.c_str(),
                      response.error.message.c_str());
            }
        }
    }
}

void rasn_service_graph::unregister_agents_from_registry_rpc()
{
    if (!_rpc_clients_enabled || !registry_dynamic_registration_enabled())
    {
        return;
    }

    rasn_registry_client registry(_registry_address);
    const std::chrono::milliseconds timeout = registry_registration_timeout();
    for (const agent_descriptor &descriptor : registered_service_agents(_llm_agent, _tool_agent, _coordinator))
    {
        ::dsn::error_code err;
        agent_response response;
        std::tie(err, response) = registry.unregister_sync(descriptor.agent_id, timeout);
        if (err != ::dsn::ERR_OK)
        {
            dwarn("failed to unregister rASN agent %s through registry RPC: %s",
                  descriptor.agent_id.c_str(),
                  err.to_string());
        }
    }
}

void rasn_service_graph::start_registry_heartbeat_timer()
{
    if (!_rpc_clients_enabled || !registry_dynamic_registration_enabled() || _registry_heartbeat_timer != nullptr)
    {
        return;
    }

    const uint64_t interval_ms = registry_heartbeat_ms();
    if (interval_ms == 0)
    {
        return;
    }

    const std::chrono::milliseconds interval(interval_ms);
    _registry_heartbeat_timer = ::dsn::tasking::enqueue_timer(
        LPC_RASN_REGISTRY_HEARTBEAT_TIMER,
        nullptr,
        [this]() { heartbeat_agents_to_registry(); },
        interval,
        0,
        interval);
    if (_registry_heartbeat_timer == nullptr)
    {
        dwarn("failed to start rASN registry heartbeat timer");
    }
}

void rasn_service_graph::cancel_registry_heartbeat_timer()
{
    if (_registry_heartbeat_timer != nullptr)
    {
        _registry_heartbeat_timer->cancel(true);
        _registry_heartbeat_timer = nullptr;
    }
}

model_gateway_response rasn_service_graph::set_provider(const std::string &provider_name, const std::string &model_name)
{
    start();
    model_provider_request request;
    request.provider = provider_name;
    request.model = model_name;
    if (_rpc_clients_enabled)
    {
        rasn_llm_agent_client client(_llm_agent_address);
        ::dsn::error_code err;
        model_gateway_response response;
        std::tie(err, response) = client.set_provider_sync(request, default_rpc_timeout());
        if (err == ::dsn::ERR_OK)
        {
            return response;
        }
        return model_error_response(std::string("RPC_RASN_MODEL_SET_PROVIDER failed: ") + err.to_string());
    }
    return _llm_agent.set_model_provider(request);
}

model_gateway_response rasn_service_graph::model_provider() const
{
    if (_rpc_clients_enabled)
    {
        rasn_llm_agent_client client(_llm_agent_address);
        ::dsn::error_code err;
        model_gateway_response response;
        std::tie(err, response) = client.describe_model_sync("");
        if (err == ::dsn::ERR_OK)
        {
            return response;
        }
        return model_error_response(std::string("RPC_RASN_MODEL_DESCRIBE failed: ") + err.to_string());
    }
    return _llm_agent.describe_model_provider();
}

model_gateway_response rasn_service_graph::model_health() const
{
    if (_rpc_clients_enabled)
    {
        rasn_llm_agent_client client(_llm_agent_address);
        ::dsn::error_code err;
        model_gateway_response response;
        std::tie(err, response) = client.health_sync("", default_rpc_timeout());
        if (err == ::dsn::ERR_OK)
        {
            return response;
        }
        return model_error_response(std::string("RPC_RASN_MODEL_HEALTH failed: ") + err.to_string());
    }
    return _llm_agent.model_health();
}

std::string rasn_service_graph::provider_summary() const
{
    std::string summary = model_gateway_summary(model_provider(), _runtime);
    const std::vector<circuit_breaker_registry::entry> breakers = model_breaker_states();
    if (!breakers.empty())
    {
        summary += "\n" + format_model_breaker_states(breakers);
    }
    const std::vector<admission_gate_registry::entry> admission = model_admission_states();
    if (!admission.empty())
    {
        summary += "\n" + format_model_admission_states(admission);
    }
    const std::vector<rate_limiter_registry::entry> rate = model_rate_states();
    if (!rate.empty())
    {
        summary += "\n" + format_model_rate_states(rate);
    }
    const std::vector<rate_limiter_registry::entry> cost = model_cost_states();
    if (!cost.empty())
    {
        summary += "\n" + format_model_cost_states(cost);
    }
    return summary;
}

std::vector<circuit_breaker_registry::entry> rasn_service_graph::model_breaker_states() const
{
    return _llm_agent.model_breaker_states();
}

std::vector<admission_gate_registry::entry> rasn_service_graph::model_admission_states() const
{
    return _llm_agent.model_admission_states();
}

std::vector<rate_limiter_registry::entry> rasn_service_graph::model_rate_states() const
{
    return _llm_agent.model_rate_states();
}

std::vector<rate_limiter_registry::entry> rasn_service_graph::model_cost_states() const
{
    return _llm_agent.model_cost_states();
}

std::string rasn_service_graph::model_resilience_report() const
{
    return format_model_breaker_states(model_breaker_states()) + "\n" +
           format_model_admission_states(model_admission_states()) + "\n" +
           format_model_rate_states(model_rate_states()) + "\n" +
           format_model_cost_states(model_cost_states());
}

std::string rasn_service_graph::tool_resilience_report() const
{
    return _tool_agent.tool_resilience_report();
}

std::string rasn_service_graph::remote_agent_resilience_report() const
{
    return _coordinator.remote_agent_resilience_report();
}

std::string rasn_service_graph::overload_resilience_report() const
{
    return _coordinator.overload_resilience_report();
}

std::string rasn_service_graph::resilience_report() const
{
    // Ordered outermost-first: the process-wide overload budget governs every
    // dependency, so it heads the report, followed by the per-dependency model,
    // tool, and remote-agent gateways.
    return overload_resilience_report() + "\n" + model_resilience_report() + "\n" +
           tool_resilience_report() + "\n" + remote_agent_resilience_report();
}

std::string rasn_service_graph::topology() const
{
    std::string topology = _coordinator.describe_topology() + "\n" + global_agent_registry().describe();
    if (!_rpc_clients_enabled)
    {
        return topology;
    }

    std::ostringstream oss;
    oss << topology << "generic RPC descriptors:\n";
    const struct
    {
        std::string label;
        ::dsn::rpc_address address;
    } clients[] = {
        {"coordinator", _coordinator_address},
        {"model", _llm_agent_address},
        {"tool", _tool_agent_address},
    };

    for (const auto &entry : clients)
    {
        rasn_agent_client client(entry.address);
        ::dsn::error_code err;
        agent_descriptor descriptor;
        std::tie(err, descriptor) = client.describe_sync("", default_rpc_timeout());
        if (err == ::dsn::ERR_OK)
        {
            oss << descriptor_line(entry.label, descriptor) << "\n";
        }
        else
        {
            oss << "- " << entry.label << " describe failed: " << err.to_string() << "\n";
        }
    }

    const model_gateway_response model = model_health();
    oss << "model gateway:\n";
    if (!model.ok)
    {
        oss << "- health failed: " << model.error << "\n";
    }
    else
    {
        oss << "- provider=" << model.provider.provider
            << " model=" << model.provider.model
            << " endpoint=" << model.provider.endpoint
            << " health=" << model.provider.health
            << " token_env=" << (model.provider.token_env.empty() ? "<none>" : model.provider.token_env)
            << "\n";
    }

    rasn_registry_client registry(_registry_address);
    ::dsn::error_code err;
    registry_query_response listed;
    std::tie(err, listed) = registry.list_sync("", default_rpc_timeout());
    oss << "registry RPC list:\n";
    if (err != ::dsn::ERR_OK)
    {
        oss << "- list failed: " << err.to_string() << "\n";
    }
    else if (!listed.ok)
    {
        oss << "- list failed: " << listed.error << "\n";
    }
    else if (listed.agents.empty())
    {
        oss << "- <none>\n";
    }
    else
    {
        for (const agent_descriptor &agent : listed.agents)
        {
            oss << descriptor_line("registry", agent) << "\n";
        }
    }

    rasn_state_client state(_state_address);
    state_response state_listing;
    std::tie(err, state_listing) = state.query_sync(state_query_request(), default_rpc_timeout());
    oss << "state RPC query:\n";
    if (err != ::dsn::ERR_OK)
    {
        oss << "- query failed: " << err.to_string() << "\n";
    }
    else if (!state_listing.ok)
    {
        oss << "- query failed: " << state_listing.error << "\n";
    }
    else
    {
        oss << "- records=" << state_listing.records.size()
            << " last_sequence=" << state_listing.last_sequence << "\n";
    }

    observability_response observed = observability_snapshot();
    oss << "observability snapshot:\n";
    if (!observed.ok)
    {
        oss << "- snapshot failed: " << observed.error << "\n";
    }
    else
    {
        oss << "- events=" << observed.events.size()
            << " failures=" << observed.failures.size()
            << " last_sequence=" << observed.last_sequence
            << (observed.truncated ? " [truncated]" : "") << "\n";
    }

    return oss.str();
}

std::string rasn_service_graph::tools_summary() const
{
    if (_rpc_clients_enabled)
    {
        agent_task task;
        task.id = make_trace_id();
        task.name = "rasn.tools.describe";
        task.input = "tools";
        agent_request request;
        request.request_id = task.id + "/describe";
        request.trace_id = _runtime.trace_id();
        request.task = task;
        request.capability = "tool.describe";

        rasn_agent_client client(_coordinator_address);
        ::dsn::error_code err;
        agent_response response;
        std::tie(err, response) = client.invoke_sync(request, default_rpc_timeout());
        if (err == ::dsn::ERR_OK)
        {
            return response.ok ? response.output : response.error.message;
        }
        return std::string("RPC_RASN_AGENT_INVOKE tool.describe failed: ") + err.to_string();
    }

    return _tool_agent.describe_tools();
}

void rasn_service_graph::set_tool_provider(std::unique_ptr<agent_tool_provider> tools)
{
    _tool_agent.set_tool_provider(std::move(tools));
}

void rasn_service_graph::enable_rpc_clients(const ::dsn::rpc_address &registry,
                                            const ::dsn::rpc_address &coordinator,
                                            const ::dsn::rpc_address &llm_agent,
                                            const ::dsn::rpc_address &tool_agent,
                                            const ::dsn::rpc_address &state,
                                            const ::dsn::rpc_address &workflow,
                                            const ::dsn::rpc_address &observability)
{
    _registry_address = registry;
    _coordinator_address = coordinator;
    _llm_agent_address = llm_agent;
    _tool_agent_address = tool_agent;
    _state_address = state;
    _workflow_address = workflow;
    _observability_address = observability;
    _rpc_clients_enabled = true;
    dinfo("enabled rASN RPC clients: registry=%s coordinator=%s llm=%s tool=%s state=%s workflow=%s observability=%s",
          registry.to_string(),
          coordinator.to_string(),
          llm_agent.to_string(),
          tool_agent.to_string(),
          state.to_string(),
          workflow.to_string(),
          observability.to_string());
}

llm_response rasn_service_graph::complete(const agent_completion_request &request)
{
    start();
    const agent_request generic_request = make_model_agent_request(request, _runtime.trace_id());
    if (_rpc_clients_enabled)
    {
        rasn_agent_client client(_coordinator_address);
        ::dsn::error_code err;
        agent_response response;
        std::tie(err, response) = client.invoke_sync(generic_request, request_rpc_timeout(generic_request));
        if (err != ::dsn::ERR_OK)
        {
            return rpc_error_response(std::string("RPC_RASN_AGENT_INVOKE coordinator failed: ") + err.to_string());
        }
        return make_llm_response_from_agent(response);
    }

    return make_llm_response_from_agent(_coordinator.invoke(generic_request, _runtime));
}

llm_response rasn_service_graph::complete_streaming(const agent_completion_request &request,
                                                    const llm_stream_callback &on_chunk)
{
    start();
    if (_rpc_clients_enabled)
    {
        const llm_response response = complete(request);
        if (response.ok)
        {
            emit_llm_stream_chunks(request.task, "rasn.coordinator", response.text, _runtime, on_chunk);
        }
        return response;
    }

    return _coordinator.complete_streaming(request, _runtime, on_chunk);
}

tool_result rasn_service_graph::run_tool(const std::string &name,
                                         const std::vector<std::string> &args,
                                         const agent_task &task,
                                         uint32_t timeout_ms)
{
    start();
    agent_tool_request generic_tool;
    generic_tool.name = name;
    generic_tool.args = args;
    generic_tool.task = task;
    agent_request generic_request = make_tool_agent_request(generic_tool, _runtime.trace_id());
    generic_request.timeout_ms = timeout_ms;
    tool_result result;
    if (_rpc_clients_enabled)
    {
        rasn_agent_client client(_coordinator_address);
        ::dsn::error_code err;
        agent_response response;
        const uint64_t rpc_start_ms = ::dsn_now_ms();
        std::tie(err, response) = client.invoke_sync(generic_request, request_rpc_timeout(generic_request));
        if (err != ::dsn::ERR_OK)
        {
            // A failed RPC that never reached the tool agent produced no
            // rasn_tool_latency_ms sample (the server-side
            // rasn_tool_agent_service::run_tool() records that), so record the
            // failed-call latency here to cover pre-dispatch failures such as a
            // missing/unroutable coordinator. We deliberately skip ambiguous
            // dispatch errors (timeout / network failure): there the server may
            // have executed the tool and already recorded its own latency sample,
            // so recording again would double-count and skew the percentiles. The
            // success path is likewise not recorded here -- it reaches the tool
            // agent, which records it.
            const bool ambiguous_dispatch = (err == ::dsn::ERR_TIMEOUT || err == ::dsn::ERR_NETWORK_FAILURE);
            if (!ambiguous_dispatch)
            {
                const uint64_t rpc_now_ms = ::dsn_now_ms();
                metrics_registry::instance().observe_tool_latency_ms(
                    rpc_now_ms >= rpc_start_ms ? rpc_now_ms - rpc_start_ms : 0);
            }
            result = rpc_error_tool_result(std::string("RPC_RASN_AGENT_INVOKE coordinator failed: ") + err.to_string());
        }
        else
        {
            result = make_tool_result_from_agent(response);
        }
    }
    else
    {
        result = make_tool_result_from_agent(_coordinator.invoke(generic_request, _runtime));
    }
    // On the success and in-process paths, tool latency is recorded in
    // rasn_tool_agent_service::run_tool(), the point where this facade, the
    // CodePilot CLI path, and the RPC server path all converge. Only an
    // unambiguous pre-dispatch RPC failure is recorded at this facade (above);
    // ambiguous timeouts/network failures are left to the server-side recorder to
    // avoid double-counting.
    return result;
}

agent_response rasn_service_graph::invoke(const agent_request &request)
{
    start();
    // Client-origin trace scope so inline coordination and any direct module RPC
    // issued from this operation carry one end-to-end trace id (finding 1.4).
    rasn_runtime_trace_scope runtime_trace(request.trace_id.empty() ? _runtime.trace_id()
                                                                     : request.trace_id);
    if (_rpc_clients_enabled)
    {
        rasn_agent_client client(_coordinator_address);
        ::dsn::error_code err;
        agent_response response;
        std::tie(err, response) = client.invoke_sync(request, request_rpc_timeout(request));
        if (err != ::dsn::ERR_OK)
        {
            agent_response failure;
            failure.request_id = request.request_id;
            failure.trace_id = request.trace_id;
            failure.ok = false;
            failure.error = make_agent_error("rpc", "coordinator_invoke_failed", err.to_string(), true, "rasn.service_graph");
            return failure;
        }
        return response;
    }
    return _coordinator.invoke(request, _runtime);
}

state_response rasn_service_graph::put_state(const state_record &record)
{
    start();
    if (_rpc_clients_enabled)
    {
        rasn_state_client client(_state_address);
        ::dsn::error_code err;
        state_response response;
        // A plain put is NOT idempotent: state_store::put allocates a fresh
        // sequence and appends a new journal record on every apply (runtime
        // mirror writes arrive with sequence 0), so re-applying it after an
        // ambiguous timeout would double-journal and advance the sequence twice.
        // Retry only on transport errors that prove the request never applied.
        std::tie(err, response) = core_rpc_with_resilience<state_response>(
            "state.put", _state_address, /*idempotent=*/false, [&](std::chrono::milliseconds timeout) {
                return client.put_sync(record, timeout);
            });
        if (err == ::dsn::ERR_OK)
        {
            return response;
        }
        state_response failure;
        failure.ok = false;
        failure.error = std::string("RPC_RASN_STATE_PUT failed: ") + err.to_string();
        return failure;
    }
    return global_state_store().put(record);
}

state_response rasn_service_graph::put_state(const state_put_request &request)
{
    start();
    if (_rpc_clients_enabled)
    {
        rasn_state_client client(_state_address);
        ::dsn::error_code err;
        state_response response;
        std::tie(err, response) = core_rpc_with_resilience<state_response>(
            "state.put_conditional", _state_address, /*idempotent=*/false, [&](std::chrono::milliseconds timeout) {
                return client.put_conditional_sync(request, timeout);
            });
        if (err == ::dsn::ERR_OK)
        {
            return response;
        }
        state_response failure;
        failure.ok = false;
        failure.error = std::string("RPC_RASN_STATE_PUT_CONDITIONAL failed: ") + err.to_string();
        return failure;
    }
    return global_state_store().put(request);
}

state_response rasn_service_graph::get_state(const state_key_request &request)
{
    start();
    if (_rpc_clients_enabled)
    {
        rasn_state_client client(_state_address);
        ::dsn::error_code err;
        state_response response;
        std::tie(err, response) = core_rpc_with_resilience<state_response>(
            "state.get", _state_address, /*idempotent=*/true, [&](std::chrono::milliseconds timeout) {
                return client.get_sync(request, timeout);
            });
        if (err == ::dsn::ERR_OK)
        {
            return response;
        }
        state_response failure;
        failure.ok = false;
        failure.error = std::string("RPC_RASN_STATE_GET failed: ") + err.to_string();
        return failure;
    }
    return global_state_store().get(request);
}

state_response rasn_service_graph::query_state(const state_query_request &request)
{
    start();
    if (_rpc_clients_enabled)
    {
        rasn_state_client client(_state_address);
        ::dsn::error_code err;
        state_response response;
        std::tie(err, response) = core_rpc_with_resilience<state_response>(
            "state.query", _state_address, /*idempotent=*/true, [&](std::chrono::milliseconds timeout) {
                return client.query_sync(request, timeout);
            });
        if (err == ::dsn::ERR_OK)
        {
            return response;
        }
        state_response failure;
        failure.ok = false;
        failure.error = std::string("RPC_RASN_STATE_QUERY failed: ") + err.to_string();
        return failure;
    }
    return global_state_store().query(request);
}

state_response rasn_service_graph::checkpoint_state(const state_checkpoint_request &request)
{
    start();
    if (_rpc_clients_enabled)
    {
        rasn_state_client client(_state_address);
        ::dsn::error_code err;
        state_response response;
        std::tie(err, response) = core_rpc_with_resilience<state_response>(
            "state.checkpoint", _state_address, /*idempotent=*/true, [&](std::chrono::milliseconds timeout) {
                return client.checkpoint_sync(request, timeout);
            });
        if (err == ::dsn::ERR_OK)
        {
            return response;
        }
        state_response failure;
        failure.ok = false;
        failure.error = std::string("RPC_RASN_STATE_CHECKPOINT failed: ") + err.to_string();
        return failure;
    }
    return global_state_store().checkpoint(request);
}

state_response rasn_service_graph::recover_state(const state_checkpoint_request &request)
{
    start();
    if (_rpc_clients_enabled)
    {
        rasn_state_client client(_state_address);
        ::dsn::error_code err;
        state_response response;
        std::tie(err, response) = core_rpc_with_resilience<state_response>(
            "state.recover", _state_address, /*idempotent=*/true, [&](std::chrono::milliseconds timeout) {
                return client.recover_sync(request, timeout);
            });
        if (err == ::dsn::ERR_OK)
        {
            return response;
        }
        state_response failure;
        failure.ok = false;
        failure.error = std::string("RPC_RASN_STATE_RECOVER failed: ") + err.to_string();
        return failure;
    }
    return global_state_store().recover(request);
}

workflow_response rasn_service_graph::validate_workflow(const workflow_source &source)
{
    start();
    if (_rpc_clients_enabled)
    {
        rasn_workflow_client client(_workflow_address);
        ::dsn::error_code err;
        workflow_response response;
        std::tie(err, response) = core_rpc_with_resilience<workflow_response>(
            "workflow.validate", _workflow_address, /*idempotent=*/true, [&](std::chrono::milliseconds timeout) {
                return client.validate_sync(source, timeout);
            });
        if (err == ::dsn::ERR_OK)
        {
            return response;
        }
        workflow_response failure;
        failure.ok = false;
        failure.error = std::string("RPC_RASN_WORKFLOW_VALIDATE failed: ") + err.to_string();
        return failure;
    }
    return global_workflow_store().validate(source);
}

workflow_response rasn_service_graph::compile_workflow(const workflow_source &source)
{
    start();
    if (_rpc_clients_enabled)
    {
        rasn_workflow_client client(_workflow_address);
        ::dsn::error_code err;
        workflow_response response;
        std::tie(err, response) = core_rpc_with_resilience<workflow_response>(
            "workflow.compile", _workflow_address, /*idempotent=*/true, [&](std::chrono::milliseconds timeout) {
                return client.compile_sync(source, timeout);
            });
        if (err == ::dsn::ERR_OK)
        {
            return response;
        }
        workflow_response failure;
        failure.ok = false;
        failure.error = std::string("RPC_RASN_WORKFLOW_COMPILE failed: ") + err.to_string();
        return failure;
    }
    return global_workflow_store().compile(source);
}

workflow_response rasn_service_graph::start_workflow(const workflow_start_request &request)
{
    start();
    if (_rpc_clients_enabled)
    {
        rasn_workflow_client client(_workflow_address);
        ::dsn::error_code err;
        workflow_response response;
        std::tie(err, response) = core_rpc_with_resilience<workflow_response>(
            "workflow.start", _workflow_address, /*idempotent=*/false, [&](std::chrono::milliseconds timeout) {
                return client.start_sync(request, timeout);
            });
        if (err == ::dsn::ERR_OK)
        {
            return response;
        }
        workflow_response failure;
        failure.ok = false;
        failure.error = std::string("RPC_RASN_WORKFLOW_START failed: ") + err.to_string();
        return failure;
    }
    return global_workflow_store().start(request);
}

workflow_response rasn_service_graph::query_workflow(const workflow_run_query &request)
{
    start();
    if (_rpc_clients_enabled)
    {
        rasn_workflow_client client(_workflow_address);
        ::dsn::error_code err;
        workflow_response response;
        std::tie(err, response) = core_rpc_with_resilience<workflow_response>(
            "workflow.query", _workflow_address, /*idempotent=*/true, [&](std::chrono::milliseconds timeout) {
                return client.query_sync(request, timeout);
            });
        if (err == ::dsn::ERR_OK)
        {
            return response;
        }
        workflow_response failure;
        failure.ok = false;
        failure.error = std::string("RPC_RASN_WORKFLOW_QUERY failed: ") + err.to_string();
        return failure;
    }
    return global_workflow_store().query(request);
}

workflow_response rasn_service_graph::cancel_workflow(const workflow_run_query &request)
{
    start();
    if (_rpc_clients_enabled)
    {
        rasn_workflow_client client(_workflow_address);
        ::dsn::error_code err;
        workflow_response response;
        std::tie(err, response) = core_rpc_with_resilience<workflow_response>(
            "workflow.cancel", _workflow_address, /*idempotent=*/true, [&](std::chrono::milliseconds timeout) {
                return client.cancel_sync(request, timeout);
            });
        if (err == ::dsn::ERR_OK)
        {
            return response;
        }
        workflow_response failure;
        failure.ok = false;
        failure.error = std::string("RPC_RASN_WORKFLOW_CANCEL failed: ") + err.to_string();
        return failure;
    }
    return global_workflow_store().cancel(request);
}

observability_response rasn_service_graph::query_events(const observability_query_request &request) const
{
    if (_rpc_clients_enabled)
    {
        rasn_observability_client client(_observability_address);
        ::dsn::error_code err;
        observability_response response;
        std::tie(err, response) = core_rpc_with_resilience<observability_response>(
            "observability.query", _observability_address, /*idempotent=*/true, [&](std::chrono::milliseconds timeout) {
                return client.query_sync(request, timeout);
            });
        if (err == ::dsn::ERR_OK)
        {
            return response;
        }
        observability_response failure;
        failure.ok = false;
        failure.error = std::string("RPC_RASN_OBSERVABILITY_QUERY failed: ") + err.to_string();
        return failure;
    }
    return query_observability_events(_runtime.events(), request);
}

observability_response rasn_service_graph::query_failures(const observability_query_request &request) const
{
    if (_rpc_clients_enabled)
    {
        rasn_observability_client client(_observability_address);
        ::dsn::error_code err;
        observability_response response;
        std::tie(err, response) = core_rpc_with_resilience<observability_response>(
            "observability.failures", _observability_address, /*idempotent=*/true, [&](std::chrono::milliseconds timeout) {
                return client.failures_sync(request, timeout);
            });
        if (err == ::dsn::ERR_OK)
        {
            return response;
        }
        observability_response failure;
        failure.ok = false;
        failure.error = std::string("RPC_RASN_OBSERVABILITY_FAILURES failed: ") + err.to_string();
        return failure;
    }
    return query_observability_failures(_runtime.events(), request);
}

observability_response rasn_service_graph::load_replay(const replay_load_request &request)
{
    if (_rpc_clients_enabled)
    {
        rasn_observability_client client(_observability_address);
        ::dsn::error_code err;
        observability_response response;
        std::tie(err, response) = core_rpc_with_resilience<observability_response>(
            "observability.load_replay", _observability_address, /*idempotent=*/true, [&](std::chrono::milliseconds timeout) {
                return client.load_replay_sync(request, timeout);
            });
        if (err == ::dsn::ERR_OK)
        {
            return response;
        }
        observability_response failure;
        failure.ok = false;
        failure.error = std::string("RPC_RASN_OBSERVABILITY_LOAD_REPLAY failed: ") + err.to_string();
        return failure;
    }
    std::string error;
    if (!_runtime.enable_replay(request.path, &error))
    {
        observability_response failure;
        failure.ok = false;
        failure.error = error;
        return failure;
    }
    observability_query_request query;
    query.kind = "replay.load";
    query.limit = 10;
    return query_observability_events(_runtime.events(), query);
}

observability_response rasn_service_graph::observability_snapshot() const
{
    if (_rpc_clients_enabled)
    {
        rasn_observability_client client(_observability_address);
        ::dsn::error_code err;
        observability_response response;
        std::tie(err, response) = core_rpc_with_resilience<observability_response>(
            "observability.snapshot", _observability_address, /*idempotent=*/true, [&](std::chrono::milliseconds timeout) {
                return client.snapshot_sync("", timeout);
            });
        if (err == ::dsn::ERR_OK)
        {
            return response;
        }
        observability_response failure;
        failure.ok = false;
        failure.error = std::string("RPC_RASN_OBSERVABILITY_SNAPSHOT failed: ") + err.to_string();
        return failure;
    }

    observability_query_request query;
    query.limit = 0;
    observability_response events = query_events(query);
    if (!events.ok)
    {
        return events;
    }
    observability_response failures = query_failures(query);
    if (!failures.ok)
    {
        return failures;
    }
    events.failures = failures.failures;
    const state_response indexed = index_observability_snapshot(events, _runtime.trace_id(), _runtime.trace_file());
    if (!indexed.ok)
    {
        events.ok = false;
        events.error = "failed to index observability snapshot in state: " + indexed.error;
    }
    return events;
}

metrics_snapshot rasn_service_graph::runtime_metrics() const
{
    return metrics_registry::instance().snapshot();
}

rasn_service_graph &global_rasn_services()
{
    static rasn_service_graph graph;
    return graph;
}

void rasn_llm_agent_rpc_service::open_service()
{
    dinfo("opening rasn.llm.agent serverlet");
    this->register_async_rpc_handler(
        RPC_RASN_AGENT_DESCRIBE, "agent_describe", &rasn_llm_agent_rpc_service::on_agent_describe);
    this->register_async_rpc_handler(
        RPC_RASN_AGENT_INVOKE, "agent_invoke", &rasn_llm_agent_rpc_service::on_agent_invoke);
    this->register_async_rpc_handler(
        RPC_RASN_AGENT_CANCEL, "agent_cancel", &rasn_llm_agent_rpc_service::on_agent_cancel);
    this->register_async_rpc_handler(
        RPC_RASN_AGENT_HEARTBEAT, "agent_heartbeat", &rasn_llm_agent_rpc_service::on_agent_heartbeat);
    this->register_async_rpc_handler(
        RPC_RASN_AGENT_QUERY, "agent_query", &rasn_llm_agent_rpc_service::on_agent_query);
    this->register_async_rpc_handler(
        RPC_RASN_MODEL_DESCRIBE, "model_describe", &rasn_llm_agent_rpc_service::on_model_describe);
    this->register_async_rpc_handler(
        RPC_RASN_MODEL_SET_PROVIDER, "model_set_provider", &rasn_llm_agent_rpc_service::on_model_set_provider);
    this->register_async_rpc_handler(
        RPC_RASN_MODEL_HEALTH, "model_health", &rasn_llm_agent_rpc_service::on_model_health);
}

void rasn_llm_agent_rpc_service::close_service()
{
    dinfo("closing rasn.llm.agent serverlet");
    this->unregister_rpc_handler(RPC_RASN_AGENT_DESCRIBE);
    this->unregister_rpc_handler(RPC_RASN_AGENT_INVOKE);
    this->unregister_rpc_handler(RPC_RASN_AGENT_CANCEL);
    this->unregister_rpc_handler(RPC_RASN_AGENT_HEARTBEAT);
    this->unregister_rpc_handler(RPC_RASN_AGENT_QUERY);
    this->unregister_rpc_handler(RPC_RASN_MODEL_DESCRIBE);
    this->unregister_rpc_handler(RPC_RASN_MODEL_SET_PROVIDER);
    this->unregister_rpc_handler(RPC_RASN_MODEL_HEALTH);
}

void rasn_llm_agent_rpc_service::on_agent_describe(const std::string &request,
                                                   ::dsn::rpc_replier<agent_descriptor> &reply)
{
    rasn_service_graph &services = global_rasn_services();
    services.start();
    reply(services._llm_agent.descriptor());
}

void rasn_llm_agent_rpc_service::on_agent_invoke(const agent_request &request,
                                                 ::dsn::rpc_replier<agent_response> &reply)
{
    rasn_service_graph &services = global_rasn_services();
    services.start();
    reply(services._llm_agent.invoke(request, services.runtime()));
}

void rasn_llm_agent_rpc_service::on_agent_cancel(const agent_request &request,
                                                 ::dsn::rpc_replier<agent_response> &reply)
{
    rasn_service_graph &services = global_rasn_services();
    services.start();
    reply(services._llm_agent.cancel_request(request));
}

void rasn_llm_agent_rpc_service::on_agent_heartbeat(const std::string &request,
                                                    ::dsn::rpc_replier<agent_descriptor> &reply)
{
    rasn_service_graph &services = global_rasn_services();
    services.start();
    reply(services._llm_agent.descriptor());
}

void rasn_llm_agent_rpc_service::on_agent_query(const std::string &request,
                                                ::dsn::rpc_replier<agent_descriptor> &reply)
{
    rasn_service_graph &services = global_rasn_services();
    services.start();
    reply(services._llm_agent.descriptor());
}

void rasn_llm_agent_rpc_service::on_model_describe(const std::string &request,
                                                   ::dsn::rpc_replier<model_gateway_response> &reply)
{
    rasn_service_graph &services = global_rasn_services();
    services.start();
    reply(services._llm_agent.describe_model_provider());
}

void rasn_llm_agent_rpc_service::on_model_set_provider(const model_provider_request &request,
                                                       ::dsn::rpc_replier<model_gateway_response> &reply)
{
    rasn_service_graph &services = global_rasn_services();
    services.start();
    reply(services._llm_agent.set_model_provider(request));
}

void rasn_llm_agent_rpc_service::on_model_health(const std::string &request,
                                                 ::dsn::rpc_replier<model_gateway_response> &reply)
{
    rasn_service_graph &services = global_rasn_services();
    services.start();
    reply(services._llm_agent.model_health());
}

void rasn_tool_agent_rpc_service::open_service()
{
    dinfo("opening rasn.tool.agent serverlet");
    this->register_async_rpc_handler(
        RPC_RASN_AGENT_DESCRIBE, "agent_describe", &rasn_tool_agent_rpc_service::on_agent_describe);
    this->register_async_rpc_handler(
        RPC_RASN_AGENT_INVOKE, "agent_invoke", &rasn_tool_agent_rpc_service::on_agent_invoke);
    this->register_async_rpc_handler(
        RPC_RASN_AGENT_CANCEL, "agent_cancel", &rasn_tool_agent_rpc_service::on_agent_cancel);
    this->register_async_rpc_handler(
        RPC_RASN_AGENT_HEARTBEAT, "agent_heartbeat", &rasn_tool_agent_rpc_service::on_agent_heartbeat);
    this->register_async_rpc_handler(
        RPC_RASN_AGENT_QUERY, "agent_query", &rasn_tool_agent_rpc_service::on_agent_query);
}

void rasn_tool_agent_rpc_service::close_service()
{
    dinfo("closing rasn.tool.agent serverlet");
    this->unregister_rpc_handler(RPC_RASN_AGENT_DESCRIBE);
    this->unregister_rpc_handler(RPC_RASN_AGENT_INVOKE);
    this->unregister_rpc_handler(RPC_RASN_AGENT_CANCEL);
    this->unregister_rpc_handler(RPC_RASN_AGENT_HEARTBEAT);
    this->unregister_rpc_handler(RPC_RASN_AGENT_QUERY);
}

void rasn_tool_agent_rpc_service::on_agent_describe(const std::string &request,
                                                    ::dsn::rpc_replier<agent_descriptor> &reply)
{
    rasn_service_graph &services = global_rasn_services();
    services.start();
    reply(services._tool_agent.descriptor());
}

void rasn_tool_agent_rpc_service::on_agent_invoke(const agent_request &request,
                                                  ::dsn::rpc_replier<agent_response> &reply)
{
    rasn_service_graph &services = global_rasn_services();
    services.start();
    reply(services._tool_agent.invoke(request, services.runtime()));
}

void rasn_tool_agent_rpc_service::on_agent_cancel(const agent_request &request,
                                                  ::dsn::rpc_replier<agent_response> &reply)
{
    rasn_service_graph &services = global_rasn_services();
    services.start();
    reply(services._tool_agent.cancel_request(request));
}

void rasn_tool_agent_rpc_service::on_agent_heartbeat(const std::string &request,
                                                     ::dsn::rpc_replier<agent_descriptor> &reply)
{
    rasn_service_graph &services = global_rasn_services();
    services.start();
    reply(services._tool_agent.descriptor());
}

void rasn_tool_agent_rpc_service::on_agent_query(const std::string &request,
                                                 ::dsn::rpc_replier<agent_descriptor> &reply)
{
    rasn_service_graph &services = global_rasn_services();
    services.start();
    reply(services._tool_agent.descriptor());
}

void rasn_coordinator_rpc_service::open_service()
{
    dinfo("opening rasn.coordinator serverlet");
    this->register_async_rpc_handler(
        RPC_RASN_AGENT_DESCRIBE, "agent_describe", &rasn_coordinator_rpc_service::on_agent_describe);
    this->register_async_rpc_handler(
        RPC_RASN_AGENT_INVOKE, "agent_invoke", &rasn_coordinator_rpc_service::on_agent_invoke);
    this->register_async_rpc_handler(
        RPC_RASN_AGENT_CANCEL, "agent_cancel", &rasn_coordinator_rpc_service::on_agent_cancel);
    this->register_async_rpc_handler(
        RPC_RASN_AGENT_HEARTBEAT, "agent_heartbeat", &rasn_coordinator_rpc_service::on_agent_heartbeat);
    this->register_async_rpc_handler(
        RPC_RASN_AGENT_QUERY, "agent_query", &rasn_coordinator_rpc_service::on_agent_query);
}

void rasn_coordinator_rpc_service::close_service()
{
    dinfo("closing rasn.coordinator serverlet");
    this->unregister_rpc_handler(RPC_RASN_AGENT_DESCRIBE);
    this->unregister_rpc_handler(RPC_RASN_AGENT_INVOKE);
    this->unregister_rpc_handler(RPC_RASN_AGENT_CANCEL);
    this->unregister_rpc_handler(RPC_RASN_AGENT_HEARTBEAT);
    this->unregister_rpc_handler(RPC_RASN_AGENT_QUERY);
}

void rasn_coordinator_rpc_service::on_agent_describe(const std::string &request,
                                                     ::dsn::rpc_replier<agent_descriptor> &reply)
{
    rasn_service_graph &services = global_rasn_services();
    services.start();
    reply(services._coordinator.descriptor());
}

void rasn_coordinator_rpc_service::on_agent_invoke(const agent_request &request,
                                                   ::dsn::rpc_replier<agent_response> &reply)
{
    rasn_service_graph &services = global_rasn_services();
    services.start();
    reply(services._coordinator.invoke(request, services.runtime()));
}

void rasn_coordinator_rpc_service::on_agent_cancel(const agent_request &request,
                                                   ::dsn::rpc_replier<agent_response> &reply)
{
    rasn_service_graph &services = global_rasn_services();
    services.start();
    agent_response response = services._coordinator.cancel_request(request);
    services._llm_agent.cancel_request(request);
    services._tool_agent.cancel_request(request);
    reply(response);
}

void rasn_coordinator_rpc_service::on_agent_heartbeat(const std::string &request,
                                                      ::dsn::rpc_replier<agent_descriptor> &reply)
{
    rasn_service_graph &services = global_rasn_services();
    services.start();
    reply(services._coordinator.descriptor());
}

void rasn_coordinator_rpc_service::on_agent_query(const std::string &request,
                                                  ::dsn::rpc_replier<agent_descriptor> &reply)
{
    rasn_service_graph &services = global_rasn_services();
    services.start();
    reply(services._coordinator.descriptor());
}

std::pair<::dsn::error_code, model_gateway_response>
rasn_llm_agent_client::describe_model_sync(const std::string &request,
                                           std::chrono::milliseconds timeout,
                                           int thread_hash,
                                           uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<model_gateway_response>(::dsn::rpc::call(
        _server, RPC_RASN_MODEL_DESCRIBE, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

std::pair<::dsn::error_code, model_gateway_response>
rasn_llm_agent_client::set_provider_sync(const model_provider_request &request,
                                         std::chrono::milliseconds timeout,
                                         int thread_hash,
                                         uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<model_gateway_response>(::dsn::rpc::call(
        _server, RPC_RASN_MODEL_SET_PROVIDER, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

std::pair<::dsn::error_code, model_gateway_response>
rasn_llm_agent_client::health_sync(const std::string &request,
                                   std::chrono::milliseconds timeout,
                                   int thread_hash,
                                   uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<model_gateway_response>(::dsn::rpc::call(
        _server, RPC_RASN_MODEL_HEALTH, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

::dsn::error_code rasn_llm_agent_app::start(int argc, char **argv)
{
    set_rdsn_rpc_enabled(true);
    global_rasn_services().acquire();
    _rpc.open_service();
    return ::dsn::ERR_OK;
}

::dsn::error_code rasn_llm_agent_app::stop(bool cleanup)
{
    _rpc.close_service();
    global_rasn_services().release();
    return ::dsn::ERR_OK;
}

::dsn::error_code rasn_tool_agent_app::start(int argc, char **argv)
{
    set_rdsn_rpc_enabled(true);
    global_rasn_services().acquire();
    _rpc.open_service();
    return ::dsn::ERR_OK;
}

::dsn::error_code rasn_tool_agent_app::stop(bool cleanup)
{
    _rpc.close_service();
    global_rasn_services().release();
    return ::dsn::ERR_OK;
}

::dsn::error_code rasn_coordinator_app::start(int argc, char **argv)
{
    set_rdsn_rpc_enabled(true);
    const ::dsn::rpc_address registry = config_service_address("registry", 27100);
    const ::dsn::rpc_address coordinator = config_service_address("coordinator", 27101);
    const ::dsn::rpc_address llm_agent = config_service_address("llm_agent", 27102);
    const ::dsn::rpc_address tool_agent = config_service_address("tool_agent", 27103);
    const ::dsn::rpc_address state = config_service_address("state", 27104);
    const ::dsn::rpc_address workflow = config_service_address("workflow", 27105);
    const ::dsn::rpc_address observability = config_service_address("observability", 27106);
    global_rasn_services().enable_rpc_clients(registry, coordinator, llm_agent, tool_agent, state, workflow, observability);
    global_rasn_services().acquire();
    _rpc.open_service();
    return ::dsn::ERR_OK;
}

::dsn::error_code rasn_coordinator_app::stop(bool cleanup)
{
    _rpc.close_service();
    global_rasn_services().release();
    return ::dsn::ERR_OK;
}

} // namespace rasn
} // namespace dsn
