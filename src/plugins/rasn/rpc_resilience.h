#pragma once

// rASN client-side RPC resilience shared by the core-service clients
// (state / workflow / observability / registry) and any other rASN caller that
// talks to a remote rASN service over rDSN RPC.
//
// The distributed runtime module path (runtime_provider.cpp::invoke_remote_module)
// already fails fast on an unhealthy endpoint via a per-endpoint circuit breaker
// and retries transient transport errors with linear backoff. Before this helper
// existed the older core services (state, workflow, observability, registry) made
// a single one-shot `::dsn::rpc::call`, so a transient blip on any of those hops
// surfaced as a hard failure with no breaker and no retry. This utility gives
// every cross-node rASN dependency the same policy, reusing the shared
// circuit_breaker engine rather than introducing another bespoke mechanism.
//
// Retry safety is idempotency-aware. Some transport errors mean the request was
// certainly not applied server-side (connection failure, or a pre-dispatch
// rejection); those are safe to retry for any operation. `ERR_TIMEOUT` is
// ambiguous — the server may have applied the mutation before the reply was lost
// — so it is only retried for operations the caller declares idempotent. This
// avoids double-applying non-idempotent mutations (e.g. starting a workflow run)
// while still recovering reads and idempotent writes.

#include <chrono>
#include <string>
#include <thread>
#include <utility>

#include <dsn/c/api_layer1.h>   // dsn_now_ms
#include <dsn/cpp/auto_codes.h> // ::dsn::error_code, ERR_*

#include <rasn/circuit_breaker.h>

namespace dsn {
namespace rasn {

// Tunables for client-side RPC resilience. Total attempts == max_attempts
// (i.e. retries + 1). Populated from config by read_rasn_core_resilience_options().
struct rpc_resilience_options
{
    bool breaker_enabled = true;
    uint32_t max_attempts = 3;
    uint64_t backoff_ms = 50;
};

// Transport errors where the server almost certainly did NOT apply the request
// (connection failed, or the request was rejected before dispatch). Safe to
// retry even for non-idempotent operations.
inline bool rpc_error_is_pre_apply(::dsn::error_code code)
{
    return code == ::dsn::ERR_NETWORK_FAILURE || code == ::dsn::ERR_NETWORK_INIT_FAILED ||
           code == ::dsn::ERR_BUSY || code == ::dsn::ERR_CAPACITY_EXCEEDED ||
           code == ::dsn::ERR_TRY_AGAIN;
}

// Ambiguous transient errors: the request may or may not have been applied. Only
// safe to retry when the operation is idempotent.
inline bool rpc_error_is_ambiguous_transient(::dsn::error_code code)
{
    return code == ::dsn::ERR_TIMEOUT;
}

inline bool rpc_should_retry(::dsn::error_code code, bool idempotent)
{
    if (rpc_error_is_pre_apply(code))
    {
        return true;
    }
    return idempotent && rpc_error_is_ambiguous_transient(code);
}

// Circuit-breaker key for a core-service RPC. Failures are aggregated per
// (service, endpoint) -- NOT per (operation, endpoint). An unhealthy endpoint
// fails *every* operation it serves, so all operations to it must share one
// breaker and trip together. Keying per operation would split failures across
// keys (e.g. "state.put@ep", "state.get@ep", "state.query@ep") so no single key
// reaches the failure threshold and the breaker never opens even though the
// endpoint is plainly down. The service is the token before the first '.' in
// `op` (e.g. "state.put" -> "state"); an op with no '.' is its own service.
inline std::string core_service_breaker_key(const std::string &op, const std::string &endpoint)
{
    const std::string::size_type dot = op.find('.');
    const std::string service = (dot == std::string::npos) ? op : op.substr(0, dot);
    return service + "@" + endpoint;
}

// Shared per-endpoint circuit-breaker registry and config for rASN core-service
// client RPC. Defined in rpc_resilience.cpp so the breaker state and tunables are
// process-global and shared across every translation unit that makes core RPC.
circuit_breaker_registry &global_rasn_core_breakers();
rpc_resilience_options read_rasn_core_resilience_options();
void ensure_rasn_core_breaker_config();

// Generic resilient client RPC. `call` is invoked as `call(timeout)` and must
// return `std::pair<::dsn::error_code, TResponse>`. Applies a per-key circuit
// breaker (fast-fail while the endpoint is unhealthy) and idempotency-aware
// retries with linear backoff.
//
// On a short-circuited breaker the call returns `{ERR_BUSY, TResponse{}}` without
// touching the dependency. On failure it returns the real (non-OK) error/response
// pair so the caller can build its own typed failure message exactly as the
// pre-existing one-shot path did. Exactly one breaker probe is consumed per call
// and exactly one outcome is reported, mirroring invoke_remote_module().
template <typename TResponse, typename FCall>
std::pair<::dsn::error_code, TResponse> resilient_rpc_call(circuit_breaker_registry &breakers,
                                                           const std::string &breaker_key,
                                                           const rpc_resilience_options &options,
                                                           bool idempotent,
                                                           std::chrono::milliseconds timeout,
                                                           FCall &&call)
{
    if (options.breaker_enabled)
    {
        circuit_breaker &breaker = breakers.get(breaker_key);
        const breaker_decision decision = breaker.allow(::dsn_now_ms());
        if (!decision.allowed)
        {
            return std::make_pair(::dsn::ERR_BUSY, TResponse());
        }
    }

    const uint32_t max_attempts = options.max_attempts == 0 ? 1 : options.max_attempts;
    ::dsn::error_code last_error = ::dsn::ERR_UNKNOWN;
    for (uint32_t attempt = 1; attempt <= max_attempts; ++attempt)
    {
        std::pair<::dsn::error_code, TResponse> result = call(timeout);
        if (result.first == ::dsn::ERR_OK)
        {
            if (options.breaker_enabled)
            {
                breakers.get(breaker_key).report(true, ::dsn_now_ms());
            }
            return result;
        }
        last_error = result.first;
        if (attempt >= max_attempts || !rpc_should_retry(result.first, idempotent))
        {
            if (options.breaker_enabled)
            {
                breakers.get(breaker_key).report(false, ::dsn_now_ms());
            }
            return result;
        }
        if (options.backoff_ms > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(options.backoff_ms * attempt));
        }
    }
    return std::make_pair(last_error, TResponse());
}

} // namespace rasn
} // namespace dsn
