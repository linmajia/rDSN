// rASN circuit breaker implementation. Dependency-light by design: no rDSN
// headers, so this translation unit compiles and unit-tests without a live
// service node. Callers supply monotonic time (ms).

#include <rasn/circuit_breaker.h>

namespace dsn {
namespace rasn {

breaker_config normalize_breaker_config(const breaker_config &config)
{
    breaker_config normalized = config;
    // A zero threshold would open on the first failure, which is rarely intended;
    // treat it as "one failure to open" so the breaker is always well defined.
    if (normalized.failure_threshold == 0)
    {
        normalized.failure_threshold = 1;
    }
    return normalized;
}

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

circuit_breaker::circuit_breaker(const breaker_config &config)
    : _config(normalize_breaker_config(config))
{
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
            ++_last_probe_token;
            if (_last_probe_token == 0)
            {
                ++_last_probe_token;
            }
            _active_probe_token = _last_probe_token;
            decision.allowed = true;
            decision.half_open_probe = true;
            decision.probe_token = _active_probe_token;
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
            ++_last_probe_token;
            if (_last_probe_token == 0)
            {
                ++_last_probe_token;
            }
            _active_probe_token = _last_probe_token;
            decision.allowed = true;
            decision.half_open_probe = true;
            decision.probe_token = _active_probe_token;
        }
        else
        {
            decision.allowed = false;
        }
        break;
    }

    decision.state = _state;
    decision.generation = _generation;
    return decision;
}

bool circuit_breaker::report(bool ok, uint64_t now_ms)
{
    std::lock_guard<std::mutex> guard(_lock);
    return report_locked(nullptr, ok, now_ms).opened;
}

breaker_report
circuit_breaker::report(const breaker_decision &admission, bool ok, uint64_t now_ms)
{
    std::lock_guard<std::mutex> guard(_lock);
    return report_locked(&admission, ok, now_ms);
}

breaker_report
circuit_breaker::report_locked(const breaker_decision *admission, bool ok, uint64_t now_ms)
{
    breaker_report result;
    if (!_config.enabled)
    {
        result.applied = false;
        return result;
    }
    if (admission != nullptr && !admission->allowed)
    {
        result.applied = false;
        result.state = _state;
        result.consecutive_failures = _consecutive_failures;
        return result;
    }

    // A half-open transition may be resolved only by the request that claimed
    // the active probe token. Conversely, a probe report that arrives after the
    // probe has already resolved is stale and must not count as an ordinary
    // closed-state outcome.
    if (admission != nullptr)
    {
        const bool valid_probe =
            admission->half_open_probe && admission->probe_token != 0 &&
            _state == breaker_state::half_open && _probe_inflight &&
            admission->probe_token == _active_probe_token &&
            admission->generation == _generation;
        if ((admission->half_open_probe && !valid_probe) ||
            (!admission->half_open_probe &&
             (_state == breaker_state::half_open ||
              admission->generation != _generation)))
        {
            result.applied = false;
            result.state = _state;
            result.consecutive_failures = _consecutive_failures;
            return result;
        }
    }

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
            ++_generation;
            result.opened = true;
        }
        break;

    case breaker_state::half_open:
        _probe_inflight = false;
        _active_probe_token = 0;
        if (ok)
        {
            _state = breaker_state::closed;
            _consecutive_failures = 0;
        }
        else
        {
            _state = breaker_state::open;
            _opened_at_ms = now_ms;
            ++_generation;
            result.opened = true;
        }
        break;

    case breaker_state::open:
        // No request is normally admitted while fully open; ignore late reports
        // so a straggler cannot silently reset the cooldown.
        result.applied = false;
        break;
    }

    result.state = _state;
    result.consecutive_failures = _consecutive_failures;
    return result;
}

breaker_state circuit_breaker::state() const
{
    std::lock_guard<std::mutex> guard(_lock);
    return _state;
}

bool circuit_breaker::is_open(uint64_t now_ms) const
{
    return inspect(now_ms).open;
}

breaker_status circuit_breaker::inspect(uint64_t now_ms) const
{
    std::lock_guard<std::mutex> guard(_lock);
    breaker_status status;
    status.state = _state;
    status.consecutive_failures = _consecutive_failures;
    if (!_config.enabled || _state != breaker_state::open)
    {
        return status;
    }
    // Mirror allow()'s open-state cooldown test without mutating anything: the
    // breaker short-circuits while open unless the cooldown has fully elapsed (at
    // which point allow() would admit a half-open probe instead). The
    // now_ms >= _opened_at_ms guard keeps a non-monotonic clock reading from
    // reporting a premature recovery.
    const bool cooldown_elapsed =
        now_ms >= _opened_at_ms && (now_ms - _opened_at_ms) >= _config.open_ms;
    status.open = !cooldown_elapsed;
    return status;
}

uint32_t circuit_breaker::consecutive_failures() const
{
    std::lock_guard<std::mutex> guard(_lock);
    return _consecutive_failures;
}

circuit_breaker_registry::circuit_breaker_registry(const breaker_config &config)
    : _config(normalize_breaker_config(config))
{
}

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

breaker_decision circuit_breaker_registry::allow(const std::string &key,
                                                 uint64_t now_ms,
                                                 uint64_t probe_lease_hint_ms)
{
    std::shared_ptr<circuit_breaker_registry_backend> backend;
    breaker_config config;
    {
        std::lock_guard<std::mutex> guard(_lock);
        backend = _backend;
        config = _config;
    }
    if (backend != nullptr)
    {
        return backend->allow(key, config, now_ms, probe_lease_hint_ms);
    }
    return get(key).allow(now_ms);
}

breaker_status circuit_breaker_registry::inspect(const std::string &key, uint64_t now_ms)
{
    std::shared_ptr<circuit_breaker_registry_backend> backend;
    breaker_config config;
    {
        std::lock_guard<std::mutex> guard(_lock);
        backend = _backend;
        config = _config;
    }
    if (backend != nullptr)
    {
        return backend->inspect(key, config, now_ms);
    }
    return get(key).inspect(now_ms);
}

breaker_report circuit_breaker_registry::report(const std::string &key,
                                                const breaker_decision &admission,
                                                bool ok,
                                                uint64_t now_ms)
{
    std::shared_ptr<circuit_breaker_registry_backend> backend;
    breaker_config config;
    {
        std::lock_guard<std::mutex> guard(_lock);
        backend = _backend;
        config = _config;
    }
    if (backend != nullptr)
    {
        return backend->report(key, config, admission, ok, now_ms);
    }
    return get(key).report(admission, ok, now_ms);
}

std::vector<circuit_breaker_registry::entry> circuit_breaker_registry::snapshot() const
{
    std::shared_ptr<circuit_breaker_registry_backend> backend;
    {
        std::lock_guard<std::mutex> guard(_lock);
        backend = _backend;
    }
    if (backend != nullptr)
    {
        return backend->snapshot();
    }

    std::vector<entry> result;
    {
        std::lock_guard<std::mutex> guard(_lock);
        result.reserve(_breakers.size());
        for (const auto &kv : _breakers)
        {
            entry e;
            e.key = kv.first;
            const breaker_status status = kv.second->inspect(0);
            e.state = status.state;
            e.consecutive_failures = status.consecutive_failures;
            result.push_back(e);
        }
    }
    return result;
}

void circuit_breaker_registry::set_config(const breaker_config &config)
{
    std::lock_guard<std::mutex> guard(_lock);
    _config = normalize_breaker_config(config);
}

void circuit_breaker_registry::set_backend(
    const std::shared_ptr<circuit_breaker_registry_backend> &backend)
{
    std::lock_guard<std::mutex> guard(_lock);
    _backend = backend;
}

bool circuit_breaker_registry::uses_shared_state() const
{
    std::lock_guard<std::mutex> guard(_lock);
    return _backend != nullptr;
}

const char *circuit_breaker_registry::backend_name() const
{
    std::lock_guard<std::mutex> guard(_lock);
    return _backend == nullptr ? "local" : _backend->name();
}

} // namespace rasn
} // namespace dsn
