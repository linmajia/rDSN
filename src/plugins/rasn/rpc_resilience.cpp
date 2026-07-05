#include <rasn/rpc_resilience.h>

#include <limits>
#include <mutex>

#include <dsn/c/api_utilities.h> // dsn_config_get_value_*

namespace dsn {
namespace rasn {

circuit_breaker_registry &global_rasn_core_breakers()
{
    static circuit_breaker_registry registry;
    return registry;
}

static bool rasn_core_breaker_enabled()
{
    return ::dsn_config_get_value_bool(
        "rasn.service",
        "rasn_core_rpc_breaker_enabled",
        true,
        "enable the rASN core-service (state/workflow/observability/registry) client circuit breaker");
}

rpc_resilience_options read_rasn_core_resilience_options()
{
    rpc_resilience_options options;
    options.breaker_enabled = rasn_core_breaker_enabled();
    const uint64_t retries = ::dsn_config_get_value_uint64(
        "rasn.service",
        "rasn_core_rpc_retries",
        2,
        "rASN core-service client RPC retries for transient/pre-apply transport errors "
        "(total attempts = retries + 1)");
    const uint64_t capped = retries > 16 ? 16 : retries;
    options.max_attempts = static_cast<uint32_t>(capped) + 1;
    options.backoff_ms = ::dsn_config_get_value_uint64(
        "rasn.service",
        "rasn_core_rpc_backoff_ms",
        50,
        "rASN core-service client RPC retry backoff in milliseconds (linear, multiplied by attempt)");
    return options;
}

static breaker_config read_rasn_core_breaker_config()
{
    breaker_config cfg;
    cfg.enabled = rasn_core_breaker_enabled();
    const uint64_t failures = ::dsn_config_get_value_uint64(
        "rasn.service",
        "rasn_core_rpc_breaker_failures",
        5,
        "consecutive rASN core-service client RPC failures before the circuit breaker opens");
    cfg.failure_threshold = failures > (std::numeric_limits<uint32_t>::max)()
                                ? (std::numeric_limits<uint32_t>::max)()
                                : static_cast<uint32_t>(failures);
    cfg.open_ms = ::dsn_config_get_value_uint64(
        "rasn.service",
        "rasn_core_rpc_breaker_open_ms",
        30000,
        "cooldown in ms before an open rASN core-service client circuit breaker admits a half-open probe");
    return cfg;
}

void ensure_rasn_core_breaker_config()
{
    static std::once_flag once;
    std::call_once(once, [] { global_rasn_core_breakers().set_config(read_rasn_core_breaker_config()); });
}

} // namespace rasn
} // namespace dsn
