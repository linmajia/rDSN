#pragma once

// rASN client-side rate limiter for outbound dependencies (model providers,
// tool adapters, and remote-agent gateways).
//
// This is the throughput-protection counterpart to circuit_breaker.h (failure
// isolation) and admission_gate.h (concurrency bulkhead): together they form the
// three classic dimensions of outbound-dependency protection. Hosted model APIs
// and remote tool adapters enforce requests-per-minute / tokens-per-minute
// quotas; exceeding them yields 429s, wasted retries, and -- on metered endpoints -- real cost. A
// token-bucket governor paces outbound calls to stay under a configured rate:
// it admits immediately while tokens remain, delays briefly (reserving a token)
// when the caller is slightly ahead of the rate, and fast-fails when the
// projected wait would exceed a bound.
//
// Each acquire consumes a caller-supplied cost (default 1). With cost 1 the
// bucket meters request COUNT -- a requests-per-minute limiter. With a larger
// per-request cost it meters weighted THROUGHPUT: e.g. a model gateway can charge
// each completion its estimated token cost so the same engine enforces a
// tokens-per-minute budget (see model_cost.h), which a request counter cannot
// because one large prompt may cost many times a small one.
//
// Like its siblings this header is intentionally dependency-light: it pulls in no
// rDSN headers and no thrift/serialization types. Wall-clock time is supplied by
// the caller as a monotonic millisecond value (e.g. ::dsn_now_ms(), which rDSN
// routes through the pluggable environment provider). Keeping time out of the
// type makes token refill deterministic under replay and unit-testable without a
// live rDSN service node, mirroring metrics.h, circuit_breaker.h, and
// admission_gate.h.

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

// Tunables for a per-dependency token-bucket rate limiter.
struct rate_limit_config
{
    bool enabled = true;
    // Sustained request rate, in requests per minute. 0 disables rate limiting
    // (every request passes through). This is the bucket's refill rate.
    uint32_t requests_per_min = 0;
    // Bucket capacity: the maximum burst that may be spent above the sustained
    // rate. 0 selects a default of one second of sustained rate (at least one
    // token) when a rate is configured.
    uint32_t burst = 0;
    // Upper bound on how long a request may be delayed waiting for a token before
    // it is rejected (fast-failed) instead. 0 means never delay: a request that
    // finds the bucket empty is rejected immediately. A large value turns the
    // limiter into a pacer that smooths bursts to the sustained rate.
    uint32_t max_wait_ms = 1000;
};

// Outcome of a rate_limiter::try_acquire() attempt.
struct rate_decision
{
    bool allowed = true;        // false => rejected (projected wait > max_wait_ms)
    uint32_t delay_ms = 0;      // backpressure to apply before proceeding
    uint32_t limit_per_min = 0; // configured sustained rate (0 => unlimited)
    double tokens = 0.0;        // tokens remaining after this attempt (diagnostic)
};

// A single-dependency token-bucket rate limiter. Thread safe.
//
// The bucket starts full (capacity tokens), so an idle dependency can absorb an
// initial burst. Tokens accrue at requests_per_min/60000 per millisecond up to
// capacity. try_acquire() consumes `cost` tokens (default 1); when fewer than
// `cost` are available it reserves the shortfall from future refill (driving the
// count negative) and returns the wait until they would have accrued, unless the
// wait exceeds max_wait_ms in which case it rejects without reserving.
class rate_limiter
{
public:
    explicit rate_limiter(const rate_limit_config &config);

    // Attempt to acquire `cost` tokens at now_ms. When rate limiting is disabled
    // this is an unconditional passthrough (allowed, no delay). A non-positive
    // cost is treated as a free passthrough that consumes nothing. The default
    // cost of 1 makes the bucket a plain requests-per-minute limiter.
    rate_decision try_acquire(uint64_t now_ms, double cost = 1.0);

    // Return `cost` tokens previously taken by an allowed try_acquire() whose
    // request was abandoned before it reached the dependency (e.g. a later gate
    // short-circuited it). Adds the tokens back, clamped to capacity, so a
    // fast-failed request does not permanently drain the quota or delay the
    // eventual recovery probe. A no-op when rate limiting is disabled or unlimited
    // (try_acquire took no token then). Pass the same cost that was acquired.
    void refund(double cost = 1.0);

    // Refill to now_ms and return the current token count without consuming one
    // (diagnostic / test helper).
    double tokens(uint64_t now_ms);

    // Last-observed token count without refilling (for ops snapshots).
    double cached_tokens() const;

    const rate_limit_config &config() const { return _config; }

private:
    void refill(uint64_t now_ms);   // caller holds _lock
    double capacity() const;        // effective bucket capacity in tokens
    double tokens_per_ms() const;   // sustained refill rate

    mutable std::mutex _lock;
    rate_limit_config _config;
    double _tokens = 0.0;
    uint64_t _last_refill_ms = 0;
    bool _seeded = false;
};

// A registry of per-key (e.g. per model provider) limiters that share one
// configuration. Thread safe.
class rate_limiter_registry
{
public:
    explicit rate_limiter_registry(const rate_limit_config &config = rate_limit_config());

    // Get (creating on first use) the limiter for a key.
    rate_limiter &get(const std::string &key);

    struct entry
    {
        std::string key;
        uint32_t requests_per_min = 0;
        uint32_t burst = 0;
        double tokens = 0.0; // last-observed token count (see cached_tokens())
    };

    // A point-in-time view of every known limiter, ordered by key. Reports the
    // last-observed token count (no refill) so the snapshot needs no clock.
    std::vector<entry> snapshot() const;

    // Replace the configuration applied to limiters created after this call.
    void set_config(const rate_limit_config &config);

    const rate_limit_config &config() const { return _config; }

private:
    mutable std::mutex _lock;
    rate_limit_config _config;
    std::map<std::string, std::unique_ptr<rate_limiter>> _limiters;
};

} // namespace rasn
} // namespace dsn
