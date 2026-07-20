#pragma once

// rASN runtime metrics.
//
// This header is intentionally dependency-light: it pulls in no rDSN headers and
// no thrift/serialization types. The perf-counter wiring and the pure formatting
// logic both live in metrics.cpp, so the data types and formatters here stay free
// of rDSN dependencies.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

// A single metric produced by metrics_registry::snapshot().
struct metric_sample
{
    std::string name; // canonical name, e.g. "rasn_llm_requests_total"
    std::string help; // human-readable description
    bool is_latency = false;
    uint64_t value = 0; // cumulative value for counters
    // latency percentiles in milliseconds (only when is_latency == true)
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double p999 = 0.0;
};

// An immutable view of all rASN counters at a point in time.
struct metrics_snapshot
{
    bool enabled = true;
    std::vector<metric_sample> samples;

    const metric_sample *find(const std::string &name) const;
    uint64_t counter(const std::string &name) const;

    // Human-readable aligned table.
    std::string to_text() const;
    // Prometheus text exposition format (https://prometheus.io/docs/instrumenting/exposition_formats/).
    std::string to_prometheus() const;
    // Compact JSON document.
    std::string to_json() const;
};

enum class endpoint_refresh_metric
{
    attempt,
    rebound,
    unchanged,
    failed,
    superseded,
    exception,
    exhausted
};

// Process-global rASN runtime metrics backed by rDSN perf counters.
//
// Counters are cumulative (Prometheus "_total" convention). Latency metrics use
// rDSN COUNTER_TYPE_NUMBER_PERCENTILES counters whose percentiles are computed by
// rDSN's own counter timers, so p50/p99/p999 update periodically rather than on
// every observation. All updates are null/thread safe and become no-ops when the
// process has no rDSN service node or when [rasn.metrics] enabled = false.
class metrics_registry
{
public:
    static metrics_registry &instance();

    // Pre-create the fixed counters so a snapshot reports zeros before any event.
    // Idempotent; should be called from a thread that owns an rDSN service node.
    void ensure_core_counters();

    // Update counters for a recorded runtime event. failure_class is only consulted
    // when kind == "failure".
    void on_event(const std::string &kind, const std::string &failure_class);

    // Endpoint refresh lifecycle accounting is enum-based and contains backend
    // exceptions so refresh teardown never propagates a metrics failure.
    void on_endpoint_refresh(endpoint_refresh_metric metric) noexcept;

    void observe_task_latency_ms(uint64_t ms);
    void observe_llm_latency_ms(uint64_t ms);
    void observe_tool_latency_ms(uint64_t ms);

    metrics_snapshot snapshot() const;
    bool enabled() const;

    metrics_registry(const metrics_registry &) = delete;
    metrics_registry &operator=(const metrics_registry &) = delete;

private:
    metrics_registry();
    ~metrics_registry();

    struct impl;
    std::unique_ptr<impl> _impl;
};

// Convert an arbitrary label (e.g. a failure class) into a Prometheus-safe token.
std::string sanitize_metric_label(const std::string &label);

} // namespace rasn
} // namespace dsn
