#pragma once

// rASN circuit breaker for protecting outbound dependencies (model providers).
//
// This header is intentionally dependency-light: it pulls in no rDSN headers and
// no thrift/serialization types. Wall-clock time is supplied by the caller as a
// monotonic millisecond value (e.g. ::dsn_now_ms(), which rDSN routes through the
// pluggable environment provider). Keeping time out of the type makes the breaker
// deterministic under replay and unit-testable without a live rDSN service node,
// mirroring the same philosophy used by metrics.h.

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

enum class breaker_state
{
    closed,    // requests pass through to the dependency
    open,      // requests are short-circuited until the cooldown elapses
    half_open, // a single probe request is admitted to test recovery
};

const char *to_string(breaker_state state);

// Tunables for a consecutive-failure circuit breaker.
struct breaker_config
{
    bool enabled = true;
    // Consecutive failures (while closed) required to open the breaker.
    uint32_t failure_threshold = 5;
    // Cooldown after opening before a half-open probe is admitted, in ms.
    uint64_t open_ms = 30000;
};

// Outcome of circuit_breaker::allow().
struct breaker_decision
{
    bool allowed = true;                       // may the request proceed?
    breaker_state state = breaker_state::closed; // state after the call
    bool opened = false;                       // unused for allow(); see report()
    bool half_open_probe = false;              // this call is the admitted probe
};

// A single-dependency consecutive-failure circuit breaker with a one-shot
// half-open probe. All methods are thread safe.
//
// State machine:
//   closed   --(failure_threshold consecutive failures)-->            open
//   open     --(cooldown elapsed, admits exactly one probe)-->   half_open
//   half_open--(probe succeeds)--> closed   |   --(probe fails)--> open
class circuit_breaker
{
public:
    explicit circuit_breaker(const breaker_config &config);

    // Decide whether a request may proceed. When allowed is false the caller
    // should fast-fail without touching the dependency.
    breaker_decision allow(uint64_t now_ms);

    // Report the outcome of a request previously admitted by allow(). Returns
    // true if this report transitioned the breaker into the open state.
    bool report(bool ok, uint64_t now_ms);

    breaker_state state() const;
    uint32_t consecutive_failures() const;
    const breaker_config &config() const { return _config; }

private:
    mutable std::mutex _lock;
    breaker_config _config;
    breaker_state _state = breaker_state::closed;
    uint32_t _consecutive_failures = 0;
    uint64_t _opened_at_ms = 0;
    bool _probe_inflight = false;
};

// A registry of per-key (e.g. per model provider) breakers that share one
// configuration. Thread safe.
class circuit_breaker_registry
{
public:
    explicit circuit_breaker_registry(const breaker_config &config = breaker_config());

    // Get (creating on first use) the breaker for a key.
    circuit_breaker &get(const std::string &key);

    struct entry
    {
        std::string key;
        breaker_state state = breaker_state::closed;
        uint32_t consecutive_failures = 0;
    };

    // A point-in-time view of every known breaker, ordered by key.
    std::vector<entry> snapshot() const;

    // Replace the configuration applied to breakers created after this call.
    void set_config(const breaker_config &config);

    const breaker_config &config() const { return _config; }

private:
    mutable std::mutex _lock;
    breaker_config _config;
    std::map<std::string, std::unique_ptr<circuit_breaker>> _breakers;
};

} // namespace rasn
} // namespace dsn
