#pragma once

// rASN admission control for outbound dependencies (model providers).
//
// This is the overload-protection counterpart to circuit_breaker.h: where the
// breaker stops calling a *broken* dependency, the admission gate keeps a
// *healthy* dependency from being overwhelmed by capping concurrency (a
// bulkhead) and smoothing bursts with graceful backpressure.
//
// Like circuit_breaker.h this header is intentionally dependency-light: it pulls
// in no rDSN headers and no thrift/serialization types, so the engine is
// unit-testable without a live rDSN service node. The graceful-backpressure delay
// curve is computed in admission_gate.cpp by reusing rDSN's dsn::exp_delay
// utility (include/dsn/utility/exp_delay.h), which rDSN itself documents as
// "delay for admission control"; only the .cpp depends on it, keeping this
// header rDSN-free.

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

// Tunables for a per-dependency concurrency bulkhead with graceful backpressure.
struct admission_config
{
    bool enabled = true;
    // Hard cap on concurrent in-flight requests. A request that would exceed the
    // cap is rejected (fast-failed). 0 disables the hard cap (bulkhead off).
    uint32_t max_concurrency = 32;
    // In-flight level at which graceful backpressure begins. Once in-flight
    // reaches this, admitted requests incur an increasing delay (up to
    // max_backpressure_ms) as they approach max_concurrency. 0 disables
    // backpressure. Clamped to <= max_concurrency.
    uint32_t soft_concurrency = 16;
    // Upper bound on the graceful backpressure delay, in milliseconds.
    uint32_t max_backpressure_ms = 200;
};

// Outcome of an admission attempt.
struct admission_decision
{
    bool admitted = true;    // false => rejected by the hard cap
    uint32_t in_flight = 0;  // in-flight count after this attempt
    uint32_t limit = 0;      // hard cap in effect (0 => unlimited)
    uint32_t delay_ms = 0;   // graceful backpressure to apply before proceeding
};

class admission_gate;

// RAII handle for one admitted slot. Releases the reserved capacity back to its
// gate on destruction. Movable, not copyable. A handle that reserved no capacity
// (passthrough when disabled, or a rejection) owns nothing and releases nothing.
class admission_slot
{
public:
    admission_slot() = default;
    admission_slot(admission_gate *gate, const admission_decision &decision);
    admission_slot(admission_slot &&other) noexcept;
    admission_slot &operator=(admission_slot &&other) noexcept;
    ~admission_slot();

    admission_slot(const admission_slot &) = delete;
    admission_slot &operator=(const admission_slot &) = delete;

    // True unless the request was rejected by the hard concurrency cap.
    bool admitted() const { return _decision.admitted; }
    uint32_t in_flight() const { return _decision.in_flight; }
    uint32_t limit() const { return _decision.limit; }
    uint32_t delay_ms() const { return _decision.delay_ms; }

    // Release the reserved slot early (also done automatically on destruction).
    // Idempotent.
    void release();

private:
    admission_gate *_gate = nullptr; // non-null only while holding reserved capacity
    admission_decision _decision;
};

// A single-dependency concurrency bulkhead with graceful backpressure. Thread
// safe.
class admission_gate
{
public:
    explicit admission_gate(const admission_config &config);

    // Attempt to admit a request. On success the returned slot reserves one unit
    // of capacity until destroyed and admitted() is true; on rejection the slot
    // reserves nothing and admitted() is false. When disabled, every request is
    // admitted as a passthrough that reserves nothing.
    admission_slot try_admit();

    uint32_t in_flight() const;
    const admission_config &config() const { return _config; }

private:
    friend class admission_slot;
    void release();

    mutable std::mutex _lock;
    admission_config _config;
    uint32_t _in_flight = 0;
};

// A registry of per-key (e.g. per model provider) gates that share one
// configuration. Thread safe.
class admission_gate_registry
{
public:
    explicit admission_gate_registry(const admission_config &config = admission_config());

    // Get (creating on first use) the gate for a key.
    admission_gate &get(const std::string &key);

    struct entry
    {
        std::string key;
        uint32_t in_flight = 0;
        uint32_t max_concurrency = 0;
        uint32_t soft_concurrency = 0;
    };

    // A point-in-time view of every known gate, ordered by key.
    std::vector<entry> snapshot() const;

    // Replace the configuration applied to gates created after this call.
    void set_config(const admission_config &config);

    const admission_config &config() const { return _config; }

private:
    mutable std::mutex _lock;
    admission_config _config;
    std::map<std::string, std::unique_ptr<admission_gate>> _gates;
};

} // namespace rasn
} // namespace dsn
