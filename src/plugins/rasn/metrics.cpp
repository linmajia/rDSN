// rASN runtime metrics: the rDSN perf-counter backed registry plus the pure
// snapshot formatting/rendering helpers (text, Prometheus, JSON).
//
// Only thrift-free rDSN headers are used here (the C perf-counter API and the
// task/node accessor), so this translation unit stays self-contained.

#include "metrics.h"

#include <dsn/c/api_utilities.h>
#include <dsn/tool-api/task.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace dsn {
namespace rasn {

namespace {

// --- Pure formatting helpers (no rDSN dependency) ---

std::string format_double(double value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    std::string text(buffer);
    // Trim trailing zeros (and a trailing dot) for compact output.
    const std::string::size_type dot = text.find('.');
    if (dot != std::string::npos)
    {
        std::string::size_type last = text.find_last_not_of('0');
        if (last == dot)
        {
            last -= 1;
        }
        text.erase(last + 1);
    }
    return text;
}

std::string json_escape_text(const std::string &value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (const char c : value)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

// --- Perf-counter backend helpers ---

// Perf-counter creation asserts an rDSN service node is current, so guard it.
bool have_node_context()
{
    return ::dsn::task::get_current_node2() != nullptr;
}

struct counter_def
{
    const char *metric; // canonical (Prometheus) name
    const char *perf;   // rDSN perf-counter name within the "rasn" section
    const char *help;   // human-readable description
};

enum core_counter
{
    CC_TASKS_BEGIN = 0,
    CC_TASKS_FINISH,
    CC_LLM_REQUESTS,
    CC_LLM_RESPONSES,
    CC_LLM_RESPONSE_CHUNKS,
    CC_TOOL_OK,
    CC_TOOL_ERROR,
    CC_EXTERNAL_EFFECTS,
    CC_FILESYSTEM_SNAPSHOTS,
    CC_WORKFLOW_NODE_START,
    CC_WORKFLOW_NODE_FINISH,
    CC_ROUTING_DECISIONS,
    CC_NONDETERMINISM,
    CC_RETRIES,
    CC_REPLAYS,
    CC_REPLAY_LOADS,
    CC_FAILURES,
    CC_COUNT
};

const counter_def k_core_counters[CC_COUNT] = {
    {"rasn_tasks_begin_total", "tasks.begin", "agent tasks started"},
    {"rasn_tasks_finish_total", "tasks.finish", "agent tasks finished"},
    {"rasn_llm_requests_total", "llm.requests", "model completion requests"},
    {"rasn_llm_responses_total", "llm.responses", "model completion responses"},
    {"rasn_llm_response_chunks_total", "llm.response.chunks", "streamed model response chunks"},
    {"rasn_tool_ok_total", "tool.ok", "successful tool invocations"},
    {"rasn_tool_error_total", "tool.error", "failed tool invocations"},
    {"rasn_external_effects_total", "external.effects", "recorded external side effects"},
    {"rasn_filesystem_snapshots_total", "filesystem.snapshots", "filesystem snapshots recorded"},
    {"rasn_workflow_node_start_total", "workflow.node.start", "workflow nodes started"},
    {"rasn_workflow_node_finish_total", "workflow.node.finish", "workflow nodes finished"},
    {"rasn_routing_decisions_total", "routing.decisions", "coordinator routing decisions"},
    {"rasn_nondeterminism_total", "nondeterminism", "captured nondeterministic values"},
    {"rasn_retries_total", "retries", "retried operations"},
    {"rasn_replays_total", "replays", "events served from a replay trace"},
    {"rasn_replay_loads_total", "replay.loads", "replay traces loaded"},
    {"rasn_failures_total", "failures", "classified failures"},
};

enum latency_counter
{
    LC_TASK = 0,
    LC_LLM,
    LC_TOOL,
    LC_COUNT
};

const counter_def k_latency_counters[LC_COUNT] = {
    {"rasn_task_latency_ms", "task.latency.ms", "agent task wall-clock latency in milliseconds"},
    {"rasn_llm_latency_ms", "llm.latency.ms", "model completion latency in milliseconds"},
    {"rasn_tool_latency_ms", "tool.latency.ms", "tool invocation latency in milliseconds"},
};

const std::unordered_map<std::string, int> &kind_to_core_counter()
{
    static const std::unordered_map<std::string, int> m = {
        {"task.begin", CC_TASKS_BEGIN},
        {"task.finish", CC_TASKS_FINISH},
        {"llm.request", CC_LLM_REQUESTS},
        {"llm.response", CC_LLM_RESPONSES},
        {"llm.response.chunk", CC_LLM_RESPONSE_CHUNKS},
        {"tool.ok", CC_TOOL_OK},
        {"tool.error", CC_TOOL_ERROR},
        {"external.effect", CC_EXTERNAL_EFFECTS},
        {"filesystem.snapshot", CC_FILESYSTEM_SNAPSHOTS},
        {"workflow.node.start", CC_WORKFLOW_NODE_START},
        {"workflow.node.finish", CC_WORKFLOW_NODE_FINISH},
        {"routing.decision", CC_ROUTING_DECISIONS},
        {"nondeterminism", CC_NONDETERMINISM},
        {"retry", CC_RETRIES},
        {"replay", CC_REPLAYS},
        {"replay.load", CC_REPLAY_LOADS},
        {"failure", CC_FAILURES},
    };
    return m;
}

uint64_t read_integer(dsn_handle_t handle)
{
    return handle == nullptr ? 0 : dsn_perf_counter_get_integer_value(handle);
}

double read_percentile(dsn_handle_t handle, dsn_perf_counter_percentile_type_t type)
{
    return handle == nullptr ? 0.0 : dsn_perf_counter_get_percentile(handle, type);
}

} // namespace

struct metrics_registry::impl
{
    mutable std::mutex lock;
    bool config_read = false;
    bool enabled = true;
    dsn_handle_t core[CC_COUNT] = {nullptr};
    dsn_handle_t latency[LC_COUNT] = {nullptr};
    std::map<std::string, dsn_handle_t> failure_class;

    void maybe_read_config()
    {
        if (config_read || !have_node_context())
        {
            return;
        }
        enabled = dsn_config_get_value_bool(
            "rasn.metrics", "enabled", true, "enable rASN runtime perf-counter metrics");
        config_read = true;
    }

    dsn_handle_t ensure(dsn_handle_t &slot,
                        const char *perf,
                        const char *help,
                        dsn_perf_counter_type_t type)
    {
        if (slot != nullptr || !have_node_context())
        {
            return slot;
        }
        slot = dsn_perf_counter_create("rasn", perf, type, help);
        return slot;
    }

    dsn_handle_t ensure_failure_class(const std::string &label)
    {
        const std::string key = sanitize_metric_label(label);
        dsn_handle_t &slot = failure_class[key];
        if (slot != nullptr || !have_node_context())
        {
            return slot;
        }
        const std::string perf = "failures.class." + key;
        slot = dsn_perf_counter_create(
            "rasn", perf.c_str(), COUNTER_TYPE_NUMBER, "classified failures by class");
        return slot;
    }
};

metrics_registry::metrics_registry() : _impl(new impl()) {}

metrics_registry::~metrics_registry() = default;

metrics_registry &metrics_registry::instance()
{
    static metrics_registry registry;
    return registry;
}

void metrics_registry::ensure_core_counters()
{
    std::lock_guard<std::mutex> guard(_impl->lock);
    _impl->maybe_read_config();
    if (!_impl->enabled)
    {
        return;
    }
    for (int i = 0; i < CC_COUNT; ++i)
    {
        _impl->ensure(_impl->core[i],
                      k_core_counters[i].perf,
                      k_core_counters[i].help,
                      COUNTER_TYPE_NUMBER);
    }
    for (int i = 0; i < LC_COUNT; ++i)
    {
        _impl->ensure(_impl->latency[i],
                      k_latency_counters[i].perf,
                      k_latency_counters[i].help,
                      COUNTER_TYPE_NUMBER_PERCENTILES);
    }
}

void metrics_registry::on_event(const std::string &kind, const std::string &failure_class)
{
    std::lock_guard<std::mutex> guard(_impl->lock);
    _impl->maybe_read_config();
    if (!_impl->enabled)
    {
        return;
    }

    const std::unordered_map<std::string, int> &map = kind_to_core_counter();
    const std::unordered_map<std::string, int>::const_iterator it = map.find(kind);
    if (it != map.end())
    {
        const int idx = it->second;
        dsn_handle_t handle = _impl->ensure(
            _impl->core[idx], k_core_counters[idx].perf, k_core_counters[idx].help, COUNTER_TYPE_NUMBER);
        if (handle != nullptr)
        {
            dsn_perf_counter_increment(handle);
        }
    }

    if (kind == "failure" && !failure_class.empty())
    {
        dsn_handle_t handle = _impl->ensure_failure_class(failure_class);
        if (handle != nullptr)
        {
            dsn_perf_counter_increment(handle);
        }
    }
}

void metrics_registry::observe_task_latency_ms(uint64_t ms)
{
    std::lock_guard<std::mutex> guard(_impl->lock);
    _impl->maybe_read_config();
    if (!_impl->enabled)
    {
        return;
    }
    dsn_handle_t handle = _impl->ensure(_impl->latency[LC_TASK],
                                        k_latency_counters[LC_TASK].perf,
                                        k_latency_counters[LC_TASK].help,
                                        COUNTER_TYPE_NUMBER_PERCENTILES);
    if (handle != nullptr)
    {
        dsn_perf_counter_set(handle, ms);
    }
}

void metrics_registry::observe_llm_latency_ms(uint64_t ms)
{
    std::lock_guard<std::mutex> guard(_impl->lock);
    _impl->maybe_read_config();
    if (!_impl->enabled)
    {
        return;
    }
    dsn_handle_t handle = _impl->ensure(_impl->latency[LC_LLM],
                                        k_latency_counters[LC_LLM].perf,
                                        k_latency_counters[LC_LLM].help,
                                        COUNTER_TYPE_NUMBER_PERCENTILES);
    if (handle != nullptr)
    {
        dsn_perf_counter_set(handle, ms);
    }
}

void metrics_registry::observe_tool_latency_ms(uint64_t ms)
{
    std::lock_guard<std::mutex> guard(_impl->lock);
    _impl->maybe_read_config();
    if (!_impl->enabled)
    {
        return;
    }
    dsn_handle_t handle = _impl->ensure(_impl->latency[LC_TOOL],
                                        k_latency_counters[LC_TOOL].perf,
                                        k_latency_counters[LC_TOOL].help,
                                        COUNTER_TYPE_NUMBER_PERCENTILES);
    if (handle != nullptr)
    {
        dsn_perf_counter_set(handle, ms);
    }
}

metrics_snapshot metrics_registry::snapshot() const
{
    std::lock_guard<std::mutex> guard(_impl->lock);
    _impl->maybe_read_config();

    metrics_snapshot result;
    result.enabled = _impl->enabled;

    for (int i = 0; i < CC_COUNT; ++i)
    {
        metric_sample sample;
        sample.name = k_core_counters[i].metric;
        sample.help = k_core_counters[i].help;
        sample.is_latency = false;
        sample.value = read_integer(_impl->core[i]);
        result.samples.push_back(sample);
    }

    for (std::map<std::string, dsn_handle_t>::const_iterator it = _impl->failure_class.begin();
         it != _impl->failure_class.end();
         ++it)
    {
        metric_sample sample;
        sample.name = "rasn_failures_class_" + it->first + "_total";
        sample.help = "classified failures by class";
        sample.is_latency = false;
        sample.value = read_integer(it->second);
        result.samples.push_back(sample);
    }

    for (int i = 0; i < LC_COUNT; ++i)
    {
        metric_sample sample;
        sample.name = k_latency_counters[i].metric;
        sample.help = k_latency_counters[i].help;
        sample.is_latency = true;
        sample.p50 = read_percentile(_impl->latency[i], COUNTER_PERCENTILE_50);
        sample.p95 = read_percentile(_impl->latency[i], COUNTER_PERCENTILE_95);
        sample.p99 = read_percentile(_impl->latency[i], COUNTER_PERCENTILE_99);
        sample.p999 = read_percentile(_impl->latency[i], COUNTER_PERCENTILE_999);
        result.samples.push_back(sample);
    }

    return result;
}

bool metrics_registry::enabled() const
{
    std::lock_guard<std::mutex> guard(_impl->lock);
    _impl->maybe_read_config();
    return _impl->enabled;
}

// ---------------------------------------------------------------------------
// Pure data/formatting logic (no rDSN or perf-counter dependency). Kept in this
// translation unit so the metric types and their renderers live together; the
// functions below are exercised directly by the gtest formatter cases.
// ---------------------------------------------------------------------------

std::string sanitize_metric_label(const std::string &label)
{
    std::string out;
    out.reserve(label.size());
    for (const char c : label)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        out += ok ? c : '_';
    }
    if (out.empty())
    {
        out = "unknown";
    }
    // A Prometheus name component must not start with a digit.
    if (out[0] >= '0' && out[0] <= '9')
    {
        out.insert(out.begin(), '_');
    }
    return out;
}

const metric_sample *metrics_snapshot::find(const std::string &name) const
{
    for (const metric_sample &sample : samples)
    {
        if (sample.name == name)
        {
            return &sample;
        }
    }
    return nullptr;
}

uint64_t metrics_snapshot::counter(const std::string &name) const
{
    const metric_sample *sample = find(name);
    return sample == nullptr ? 0 : sample->value;
}

std::string metrics_snapshot::to_text() const
{
    std::ostringstream out;
    out << "rasn metrics (enabled=" << (enabled ? "true" : "false") << ")\n";

    std::string::size_type width = 0;
    for (const metric_sample &sample : samples)
    {
        width = std::max(width, sample.name.size());
    }

    for (const metric_sample &sample : samples)
    {
        out << "  " << sample.name;
        for (std::string::size_type i = sample.name.size(); i < width; ++i)
        {
            out << ' ';
        }
        if (sample.is_latency)
        {
            out << "  p50=" << format_double(sample.p50) << "ms"
                << " p95=" << format_double(sample.p95) << "ms"
                << " p99=" << format_double(sample.p99) << "ms"
                << " p999=" << format_double(sample.p999) << "ms";
        }
        else
        {
            out << "  " << sample.value;
        }
        out << "\n";
    }
    return out.str();
}

std::string metrics_snapshot::to_prometheus() const
{
    std::ostringstream out;
    for (const metric_sample &sample : samples)
    {
        if (!sample.help.empty())
        {
            out << "# HELP " << sample.name << ' ' << sample.help << "\n";
        }
        if (sample.is_latency)
        {
            out << "# TYPE " << sample.name << " summary\n";
            out << sample.name << "{quantile=\"0.5\"} " << format_double(sample.p50) << "\n";
            out << sample.name << "{quantile=\"0.95\"} " << format_double(sample.p95) << "\n";
            out << sample.name << "{quantile=\"0.99\"} " << format_double(sample.p99) << "\n";
            out << sample.name << "{quantile=\"0.999\"} " << format_double(sample.p999) << "\n";
        }
        else
        {
            out << "# TYPE " << sample.name << " counter\n";
            out << sample.name << ' ' << sample.value << "\n";
        }
    }
    return out.str();
}

std::string metrics_snapshot::to_json() const
{
    std::ostringstream out;
    out << "{\"enabled\":" << (enabled ? "true" : "false") << ",\"metrics\":[";
    bool first = true;
    for (const metric_sample &sample : samples)
    {
        if (!first)
        {
            out << ',';
        }
        first = false;
        out << "{\"name\":\"" << json_escape_text(sample.name) << "\"";
        out << ",\"help\":\"" << json_escape_text(sample.help) << "\"";
        if (sample.is_latency)
        {
            out << ",\"type\":\"latency_ms\"";
            out << ",\"p50\":" << format_double(sample.p50);
            out << ",\"p95\":" << format_double(sample.p95);
            out << ",\"p99\":" << format_double(sample.p99);
            out << ",\"p999\":" << format_double(sample.p999);
        }
        else
        {
            out << ",\"type\":\"counter\"";
            out << ",\"value\":" << sample.value;
        }
        out << '}';
    }
    out << "]}";
    return out.str();
}

} // namespace rasn
} // namespace dsn
