#pragma once

// rASN circuit breaker for protecting outbound dependencies (model providers).
//
// This header is intentionally dependency-light: it pulls in no rDSN headers and
// no thrift/serialization types. Physical time is supplied by the caller as a
// millisecond value (e.g. ::dsn_now_ms(), which rDSN routes through the
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

// Normalize edge-case tunables once so local and coordinated breaker backends
// apply the same state-machine contract.
breaker_config normalize_breaker_config(const breaker_config &config);

// Outcome of circuit_breaker::allow().
struct breaker_decision
{
    bool allowed = true;                         // may the request proceed?
    breaker_state state = breaker_state::closed; // state after the call
    bool opened = false;                         // unused for allow(); see report()
    bool half_open_probe = false;                // this call is the admitted probe
    // Correlates the admitted half-open probe with its report. Zero for ordinary
    // closed-state requests. This prevents a late closed-state request from
    // closing or reopening a half-open breaker.
    uint64_t probe_token = 0;
    // Changes whenever the breaker opens. Ordinary reports from an older closed
    // generation are ignored after recovery, just like superseded probe tokens.
    uint64_t generation = 0;
    // A coordinated backend denies requests when shared state cannot be read or
    // locked. Local breakers always leave available=true and error empty.
    bool available = true;
    std::string error;
};

// Non-mutating point-in-time status used for the advisory open-state precheck.
struct breaker_status
{
    bool open = false;
    breaker_state state = breaker_state::closed;
    uint32_t consecutive_failures = 0;
    bool available = true;
    std::string error;
};

// Result of reporting one admitted request.
struct breaker_report
{
    bool opened = false;
    bool applied = true;
    breaker_state state = breaker_state::closed;
    uint32_t consecutive_failures = 0;
    bool available = true;
    std::string error;
};

struct breaker_registry_entry
{
    std::string key;
    breaker_state state = breaker_state::closed;
    uint32_t consecutive_failures = 0;
    bool shared = false;
    bool available = true;
    uint64_t revision = 0;
    std::string error;
};

// Optional authoritative state backend for a registry. The coordination-backed
// implementation lives in coordination_breaker.{h,cpp}; keeping this interface
// dependency-light preserves standalone unit testing of the local state machine.
class circuit_breaker_registry_backend
{
public:
    virtual ~circuit_breaker_registry_backend() {}

    virtual breaker_decision allow(const std::string &key,
                                   const breaker_config &config,
                                   uint64_t now_ms,
                                   uint64_t probe_lease_hint_ms) = 0;
    virtual breaker_status
    inspect(const std::string &key, const breaker_config &config, uint64_t now_ms) = 0;
    virtual breaker_report report(const std::string &key,
                                  const breaker_config &config,
                                  const breaker_decision &admission,
                                  bool ok,
                                  uint64_t now_ms) = 0;
    virtual std::vector<breaker_registry_entry> snapshot() const = 0;
    virtual const char *name() const = 0;
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

    // Correlated report used by production call paths. Only the decision that
    // admitted the active half-open probe may resolve it; stale reports are
    // ignored and returned with applied=false.
    breaker_report report(const breaker_decision &admission, bool ok, uint64_t now_ms);

    // Non-mutating: is the breaker currently open and still within its cooldown,
    // so the next allow() would short-circuit without admitting a half-open probe?
    // Lets a caller fast-fail a still-open breaker ahead of other gates without
    // consuming the one-shot probe. Returns false once the cooldown has elapsed
    // (allow() would then admit a probe) or when the breaker is disabled.
    bool is_open(uint64_t now_ms) const;
    breaker_status inspect(uint64_t now_ms) const;

    breaker_state state() const;
    uint32_t consecutive_failures() const;
    const breaker_config &config() const { return _config; }

private:
    breaker_report
    report_locked(const breaker_decision *admission, bool ok, uint64_t now_ms);

    mutable std::mutex _lock;
    breaker_config _config;
    breaker_state _state = breaker_state::closed;
    uint32_t _consecutive_failures = 0;
    uint64_t _opened_at_ms = 0;
    bool _probe_inflight = false;
    uint64_t _active_probe_token = 0;
    uint64_t _last_probe_token = 0;
    uint64_t _generation = 0;
};

// A registry of per-key (e.g. per model provider) breakers that share one
// configuration. Thread safe.
class circuit_breaker_registry
{
public:
    explicit circuit_breaker_registry(const breaker_config &config = breaker_config());

    // Get (creating on first use) the process-local breaker for a key. Production
    // call paths should use allow()/inspect()/report() below so an installed
    // shared backend cannot be bypassed.
    circuit_breaker &get(const std::string &key);

    using entry = breaker_registry_entry;

    breaker_decision
    allow(const std::string &key, uint64_t now_ms, uint64_t probe_lease_hint_ms = 0);
    breaker_status inspect(const std::string &key, uint64_t now_ms);
    breaker_report
    report(const std::string &key, const breaker_decision &admission, bool ok, uint64_t now_ms);

    // A point-in-time view of every known breaker, ordered by key.
    std::vector<entry> snapshot() const;

    // Replace the configuration applied to breakers created after this call.
    void set_config(const breaker_config &config);

    // Install an authoritative backend. Existing local entries are retained but
    // no longer consulted by the registry-level API.
    void set_backend(const std::shared_ptr<circuit_breaker_registry_backend> &backend);
    bool uses_shared_state() const;
    const char *backend_name() const;

    const breaker_config &config() const { return _config; }

private:
    mutable std::mutex _lock;
    breaker_config _config;
    std::map<std::string, std::unique_ptr<circuit_breaker>> _breakers;
    std::shared_ptr<circuit_breaker_registry_backend> _backend;
};

} // namespace rasn
} // namespace dsn
