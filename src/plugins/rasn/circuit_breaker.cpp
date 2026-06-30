// rASN circuit breaker implementation. Dependency-light by design: no rDSN
// headers, so this translation unit compiles and unit-tests without a live
// service node. Callers supply monotonic time (ms).

#include "circuit_breaker.h"

namespace dsn {
namespace rasn {

const char *to_string(breaker_state state)
{
    switch (state)
    {
    case breaker_state::closed:
        return "closed";
    case breaker_state::open:
        return "open";
    case breaker_state::half_open:
        return "half_open";
    }
    return "unknown";
}

circuit_breaker::circuit_breaker(const breaker_config &config) : _config(config)
{
    // A zero threshold would open on the first failure, which is rarely intended;
    // treat it as "one failure to open" so the breaker is always well defined.
    if (_config.failure_threshold == 0)
    {
        _config.failure_threshold = 1;
    }
}

breaker_decision circuit_breaker::allow(uint64_t now_ms)
{
    std::lock_guard<std::mutex> guard(_lock);

    breaker_decision decision;
    if (!_config.enabled)
    {
        decision.allowed = true;
        decision.state = breaker_state::closed;
        return decision;
    }

    switch (_state)
    {
    case breaker_state::closed:
        decision.allowed = true;
        break;

    case breaker_state::open:
        // Only move to half-open once the cooldown has fully elapsed. The
        // now_ms >= _opened_at_ms guard keeps a non-monotonic clock reading from
        // prematurely admitting a probe.
        if (now_ms >= _opened_at_ms && (now_ms - _opened_at_ms) >= _config.open_ms)
        {
            _state = breaker_state::half_open;
            _probe_inflight = true;
            decision.allowed = true;
            decision.half_open_probe = true;
        }
        else
        {
            decision.allowed = false;
        }
        break;

    case breaker_state::half_open:
        // Admit exactly one probe at a time.
        if (!_probe_inflight)
        {
            _probe_inflight = true;
            decision.allowed = true;
            decision.half_open_probe = true;
        }
        else
        {
            decision.allowed = false;
        }
        break;
    }

    decision.state = _state;
    return decision;
}

bool circuit_breaker::report(bool ok, uint64_t now_ms)
{
    std::lock_guard<std::mutex> guard(_lock);
    if (!_config.enabled)
    {
        return false;
    }

    bool opened = false;
    switch (_state)
    {
    case breaker_state::closed:
        if (ok)
        {
            _consecutive_failures = 0;
        }
        else if (++_consecutive_failures >= _config.failure_threshold)
        {
            _state = breaker_state::open;
            _opened_at_ms = now_ms;
            opened = true;
        }
        break;

    case breaker_state::half_open:
        _probe_inflight = false;
        if (ok)
        {
            _state = breaker_state::closed;
            _consecutive_failures = 0;
        }
        else
        {
            _state = breaker_state::open;
            _opened_at_ms = now_ms;
            opened = true;
        }
        break;

    case breaker_state::open:
        // No request is normally admitted while fully open; ignore late reports
        // so a straggler cannot silently reset the cooldown.
        break;
    }

    return opened;
}

breaker_state circuit_breaker::state() const
{
    std::lock_guard<std::mutex> guard(_lock);
    return _state;
}

bool circuit_breaker::is_open(uint64_t now_ms) const
{
    std::lock_guard<std::mutex> guard(_lock);
    if (!_config.enabled || _state != breaker_state::open)
    {
        return false;
    }
    // Mirror allow()'s open-state cooldown test without mutating anything: the
    // breaker short-circuits while open unless the cooldown has fully elapsed (at
    // which point allow() would admit a half-open probe instead). The
    // now_ms >= _opened_at_ms guard keeps a non-monotonic clock reading from
    // reporting a premature recovery.
    const bool cooldown_elapsed =
        now_ms >= _opened_at_ms && (now_ms - _opened_at_ms) >= _config.open_ms;
    return !cooldown_elapsed;
}

uint32_t circuit_breaker::consecutive_failures() const
{
    std::lock_guard<std::mutex> guard(_lock);
    return _consecutive_failures;
}

circuit_breaker_registry::circuit_breaker_registry(const breaker_config &config) : _config(config) {}

circuit_breaker &circuit_breaker_registry::get(const std::string &key)
{
    std::lock_guard<std::mutex> guard(_lock);
    std::unique_ptr<circuit_breaker> &slot = _breakers[key];
    if (slot == nullptr)
    {
        slot.reset(new circuit_breaker(_config));
    }
    return *slot;
}

std::vector<circuit_breaker_registry::entry> circuit_breaker_registry::snapshot() const
{
    std::lock_guard<std::mutex> guard(_lock);
    std::vector<entry> result;
    result.reserve(_breakers.size());
    for (const auto &kv : _breakers)
    {
        entry e;
        e.key = kv.first;
        e.state = kv.second->state();
        e.consecutive_failures = kv.second->consecutive_failures();
        result.push_back(e);
    }
    return result;
}

void circuit_breaker_registry::set_config(const breaker_config &config)
{
    std::lock_guard<std::mutex> guard(_lock);
    _config = config;
}

} // namespace rasn
} // namespace dsn
