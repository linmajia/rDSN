// rASN client-side rate limiter implementation.
//
// A standard token bucket: tokens accrue linearly at the configured sustained
// rate up to a capacity (the burst), and each admitted request consumes a
// caller-supplied cost (default one). The engine is dependency-light -- it takes
// the current time as an injected millisecond value rather than reading a clock
// -- so refill is deterministic under replay and the limiter is unit-testable
// without a live rDSN node.

#include "rate_limiter.h"

#include <algorithm>
#include <cmath>

namespace dsn {
namespace rasn {

rate_limiter::rate_limiter(const rate_limit_config &config) : _config(config)
{
    // Start full so an idle dependency can absorb an initial burst.
    _tokens = capacity();
}

double rate_limiter::capacity() const
{
    if (_config.requests_per_min == 0)
    {
        return 0.0; // unlimited: the token path is not used
    }
    if (_config.burst != 0)
    {
        return static_cast<double>(_config.burst);
    }
    // Default burst: roughly one second of the sustained rate, at least one token
    // so a configured rate always admits at least a single request immediately.
    const double per_second = static_cast<double>(_config.requests_per_min) / 60.0;
    return per_second < 1.0 ? 1.0 : per_second;
}

double rate_limiter::tokens_per_ms() const
{
    return static_cast<double>(_config.requests_per_min) / 60000.0;
}

void rate_limiter::refill(uint64_t now_ms)
{
    if (!_seeded)
    {
        // First observation establishes the baseline; tokens already start full.
        _last_refill_ms = now_ms;
        _seeded = true;
        return;
    }
    if (now_ms <= _last_refill_ms)
    {
        // Non-monotonic or same-instant clock: never remove tokens, and do NOT
        // move the baseline backward. Moving it back would let a later-but-still-
        // stale reading (one still below the original high-water mark) refill
        // tokens prematurely. Hold the high-water mark instead; refill resumes
        // only once the clock advances past it.
        return;
    }
    const double added = static_cast<double>(now_ms - _last_refill_ms) * tokens_per_ms();
    _tokens = (std::min)(capacity(), _tokens + added);
    _last_refill_ms = now_ms;
}

rate_decision rate_limiter::try_acquire(uint64_t now_ms, double cost)
{
    std::lock_guard<std::mutex> guard(_lock);

    rate_decision decision;
    decision.limit_per_min = _config.requests_per_min;

    // A non-positive cost reserves nothing; admit it as a free passthrough so
    // callers never have to special-case a zero-weight request.
    if (cost <= 0.0)
    {
        decision.allowed = true;
        decision.delay_ms = 0;
        decision.tokens = _tokens;
        return decision;
    }

    if (!_config.enabled || _config.requests_per_min == 0)
    {
        // Passthrough: no rate configured, admit without touching the bucket.
        decision.allowed = true;
        decision.delay_ms = 0;
        decision.tokens = _tokens;
        return decision;
    }

    refill(now_ms);

    if (_tokens >= cost)
    {
        _tokens -= cost;
        decision.allowed = true;
        decision.delay_ms = 0;
        decision.tokens = _tokens;
        return decision;
    }

    // Not enough tokens: compute the wait until `cost` accrues. Reserve the
    // shortfall (driving the count negative) when the wait is within bounds, so
    // concurrent callers queue with progressively larger -- but bounded -- delays
    // that pace them to the sustained rate. Reject when the wait would exceed
    // max_wait_ms. A single request whose cost exceeds what can accrue within
    // max_wait_ms (e.g. cost > burst and max_wait too small) is rejected; size
    // the burst >= the largest single-request cost you intend to admit.
    const double deficit = cost - _tokens;     // > 0 here
    const double tpm = tokens_per_ms();        // > 0 here (requests_per_min != 0)
    const double wait = std::ceil(deficit / tpm);
    if (!std::isfinite(wait) || wait > static_cast<double>(_config.max_wait_ms))
    {
        decision.allowed = false;
        decision.delay_ms = 0;
        decision.tokens = _tokens;
        return decision;
    }
    const uint32_t wait_ms = wait <= 0.0 ? 0 : static_cast<uint32_t>(wait);

    _tokens -= cost;
    decision.allowed = true;
    decision.delay_ms = wait_ms;
    decision.tokens = _tokens;
    return decision;
}

void rate_limiter::refund(double cost)
{
    std::lock_guard<std::mutex> guard(_lock);
    if (!_config.enabled || _config.requests_per_min == 0 || cost <= 0.0)
    {
        // Passthrough mode (or a zero-weight request) took no token, so there is
        // nothing to give back.
        return;
    }
    // Restore the `cost` tokens taken by the corresponding try_acquire(). This is
    // the inverse of the "_tokens -= cost" there; no refill is needed because the
    // abandon happens in the same instant (the breaker short-circuits before any
    // wait). Clamp to capacity so the bucket never exceeds its burst.
    _tokens = (std::min)(capacity(), _tokens + cost);
}

double rate_limiter::tokens(uint64_t now_ms)
{
    std::lock_guard<std::mutex> guard(_lock);
    if (_config.requests_per_min == 0)
    {
        return _tokens;
    }
    refill(now_ms);
    return _tokens;
}

double rate_limiter::cached_tokens() const
{
    std::lock_guard<std::mutex> guard(_lock);
    return _tokens;
}

rate_limiter_registry::rate_limiter_registry(const rate_limit_config &config) : _config(config) {}

rate_limiter &rate_limiter_registry::get(const std::string &key)
{
    std::lock_guard<std::mutex> guard(_lock);
    std::unique_ptr<rate_limiter> &slot = _limiters[key];
    if (slot == nullptr)
    {
        slot.reset(new rate_limiter(_config));
    }
    return *slot;
}

std::vector<rate_limiter_registry::entry> rate_limiter_registry::snapshot() const
{
    std::lock_guard<std::mutex> guard(_lock);
    std::vector<entry> result;
    result.reserve(_limiters.size());
    for (const auto &kv : _limiters)
    {
        entry e;
        e.key = kv.first;
        e.requests_per_min = kv.second->config().requests_per_min;
        e.burst = kv.second->config().burst;
        e.tokens = kv.second->cached_tokens();
        result.push_back(e);
    }
    return result;
}

void rate_limiter_registry::set_config(const rate_limit_config &config)
{
    std::lock_guard<std::mutex> guard(_lock);
    _config = config;
}

} // namespace rasn
} // namespace dsn
