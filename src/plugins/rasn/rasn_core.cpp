#include "rasn_core.h"

#include "metrics.h"
#include "redaction.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace dsn {
namespace rasn {

namespace {

// Upper bound on runtime events retained in memory for observability queries.
// 0 disables the cap (unbounded, legacy behavior). Read once; config is fixed at
// startup for the lifetime of the process.
size_t event_log_memory_capacity()
{
    static const size_t capacity = static_cast<size_t>(::dsn_config_get_value_uint64(
        "rasn.observability",
        "max_in_memory_events",
        100000,
        "maximum runtime events retained in memory for observability queries (0 = unbounded)"));
    return capacity;
}

std::string extract_json_string_field(const std::string &json, const std::string &field)
{
    const std::string needle = "\"" + field + "\"";
    const std::string::size_type pos = json.find(needle);
    if (pos == std::string::npos)
    {
        return "";
    }

    const std::string::size_type colon = json.find(':', pos + needle.size());
    if (colon == std::string::npos)
    {
        return "";
    }

    std::string::size_type quote = json.find('"', colon + 1);
    if (quote == std::string::npos)
    {
        return "";
    }

    std::string result;
    bool escaping = false;
    for (++quote; quote < json.size(); ++quote)
    {
        const char c = json[quote];
        if (escaping)
        {
            switch (c)
            {
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            default:
                result.push_back(c);
                break;
            }
            escaping = false;
            continue;
        }
        if (c == '\\')
        {
            escaping = true;
            continue;
        }
        if (c == '"')
        {
            break;
        }
        result.push_back(c);
    }
    return result;
}

std::string event_to_json(const runtime_event &event)
{
    std::ostringstream oss;
    oss << "{\"schema_version\":" << event.schema_version
        << ",\"sequence\":" << event.sequence
        << ",\"trace_id\":\"" << json_escape(event.trace_id)
        << "\",\"task_id\":\"" << json_escape(event.task_id)
        << "\",\"kind\":\"" << json_escape(event.kind)
        << "\",\"name\":\"" << json_escape(event.name)
        << "\",\"value\":\"" << json_escape(event.value)
        << "\",\"timestamp\":\"" << json_escape(event.timestamp) << "\"";
    if (!event.failure_class.empty())
    {
        oss << ",\"failure_class\":\"" << json_escape(event.failure_class)
            << "\",\"failure_code\":\"" << json_escape(event.failure_code)
            << "\",\"failure_source\":\"" << json_escape(event.failure_source)
            << "\",\"retryable\":" << (event.retryable ? "true" : "false")
            << ",\"retry_attempt\":" << event.retry_attempt;
    }
    oss << "}";
    return oss.str();
}

std::string tool_replay_key(const std::string &tool, const std::string &arguments)
{
    return tool + "\n" + arguments;
}

std::string tool_arguments_from_event_value(const std::string &value)
{
    const std::string::size_type split = value.find('\n');
    return split == std::string::npos ? value : value.substr(0, split);
}

std::string tool_result_from_event_value(const std::string &value)
{
    const std::string::size_type split = value.find('\n');
    return split == std::string::npos ? "" : value.substr(split + 1);
}

} // namespace

std::string now_utc_string()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm_value;
#if defined(_WIN32)
    gmtime_s(&tm_value, &t);
#else
    gmtime_r(&t, &tm_value);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_value, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::string make_trace_id()
{
    // Draw the identifier from rDSN's pluggable environment provider
    // (dsn_random64) rather than a private wall-clock-seeded RNG. This matches
    // how rDSN itself mints RPC trace ids and lets replay/emulator tooling seed
    // or virtualize the value for deterministic runs.
    std::ostringstream oss;
    oss << "rasn-" << std::hex << ::dsn_random64(0, 0xffffffffffffffffULL);
    return oss.str();
}

std::string json_escape(const std::string &value)
{
    std::ostringstream oss;
    for (const char c : value)
    {
        switch (c)
        {
        case '\\':
            oss << "\\\\";
            break;
        case '"':
            oss << "\\\"";
            break;
        case '\n':
            oss << "\\n";
            break;
        case '\r':
            oss << "\\r";
            break;
        case '\t':
            oss << "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(static_cast<unsigned char>(c));
            }
            else
            {
                oss << c;
            }
            break;
        }
    }
    return oss.str();
}

std::string trim(const std::string &value)
{
    std::string::size_type begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])))
    {
        ++begin;
    }

    std::string::size_type end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
    {
        --end;
    }

    return value.substr(begin, end - begin);
}

std::string normalize_platform_path(const std::string &path)
{
    std::string normalized = path;
    for (char &c : normalized)
    {
#if defined(_WIN32)
        if (c == '/')
        {
            c = '\\';
        }
#else
        if (c == '\\')
        {
            c = '/';
        }
#endif
    }
    return normalized;
}

std::vector<std::string> split_words(const std::string &line)
{
    std::vector<std::string> words;
    std::string current;
    bool in_quote = false;
    char quote_char = 0;

    for (const char c : line)
    {
        if ((c == '"' || c == '\'') && (!in_quote || quote_char == c))
        {
            in_quote = !in_quote;
            quote_char = in_quote ? c : 0;
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(c)) && !in_quote)
        {
            if (!current.empty())
            {
                words.push_back(current);
                current.clear();
            }
            continue;
        }

        current.push_back(c);
    }

    if (!current.empty())
    {
        words.push_back(current);
    }
    return words;
}

void event_log::set_output_file(const std::string &path)
{
    ::dsn::service::zauto_lock guard(_lock);
    if (_output_stream.is_open())
    {
        _output_stream.flush();
        _output_stream.close();
    }
    _output_file = path;
    _output_stream.clear();
    if (!path.empty())
    {
        // Open once and keep the stream; the previous code re-opened (and flushed
        // and closed) the file on every single event under _lock, stalling all
        // recording and observability queries behind per-event filesystem calls.
        _output_stream.open(path.c_str(), std::ios::app);
    }
}

std::string event_log::output_file() const
{
    ::dsn::service::zauto_lock guard(_lock);
    return _output_file;
}

event_log::~event_log()
{
    ::dsn::service::zauto_lock guard(_lock);
    if (_output_stream.is_open())
    {
        _output_stream.flush();
        _output_stream.close();
    }
}

bool event_log::load_replay_file(const std::string &path, std::string *error)
{
    std::ifstream input(path.c_str());
    if (!input)
    {
        if (error != nullptr)
        {
            *error = "cannot open replay file: " + path;
        }
        return false;
    }

    std::map<std::string, std::string> values;
    std::map<std::string, runtime_event> tool_events;
    std::map<std::string, std::string> filesystem_snapshots;
    std::vector<runtime_event> llm_events;
    std::vector<runtime_event> workflow_node_events;
    std::string line;
    while (std::getline(input, line))
    {
        const std::string kind = extract_json_string_field(line, "kind");
        if (kind != "nondeterminism" && kind != "tool.ok" && kind != "tool.error" &&
            kind != "llm.response" && kind != "filesystem.snapshot" &&
            kind != "workflow.node.start")
        {
            continue;
        }

        const std::string name = extract_json_string_field(line, "name");
        const std::string value = extract_json_string_field(line, "value");
        if (kind == "nondeterminism" && !name.empty())
        {
            values[name] = value;
        }
        else if ((kind == "tool.ok" || kind == "tool.error") && !name.empty())
        {
            runtime_event event;
            event.kind = kind;
            event.name = name;
            event.value = value;
            tool_events[tool_replay_key(name, tool_arguments_from_event_value(value))] = event;
        }
        else if (kind == "llm.response" && !name.empty())
        {
            runtime_event event;
            event.kind = kind;
            event.name = name;
            event.value = value;
            llm_events.push_back(event);
        }
        else if (kind == "filesystem.snapshot" && !name.empty())
        {
            filesystem_snapshots[name] = value;
        }
        else if (kind == "workflow.node.start" && !name.empty())
        {
            runtime_event event;
            event.kind = kind;
            event.name = name;
            event.value = value;
            workflow_node_events.push_back(event);
        }
    }

    ::dsn::service::zauto_lock guard(_lock);
    _replay_values.swap(values);
    _replay_tool_events.swap(tool_events);
    _replay_filesystem_snapshots.swap(filesystem_snapshots);
    _replay_llm_events.swap(llm_events);
    _replay_llm_cursor.clear();
    _replay_workflow_node_events.swap(workflow_node_events);
    _replay_workflow_node_cursor = 0;
    _replay_enabled = true;
    return true;
}

void event_log::append(runtime_event event)
{
    ::dsn::service::zauto_lock guard(_lock);
    event.schema_version = RASN_OBSERVABILITY_SCHEMA_VERSION;
    event.sequence = ++_last_sequence;
    event.value = redact_sensitive_text(event.value);
    _events.push_back(event);

    // Bound in-memory retention so a long-running process cannot grow _events (and
    // therefore every observability snapshot copy) without limit. Events already
    // persist to the trace file; the in-memory buffer is a recent-events window.
    const size_t capacity = event_log_memory_capacity();
    if (capacity != 0)
    {
        while (_events.size() > capacity)
        {
            _events.pop_front();
        }
    }

    if (_output_stream.is_open())
    {
        _output_stream << event_to_json(event) << '\n';
        _output_stream.flush();
        if (!_output_stream)
        {
            // Surface the failure by clearing error flags so subsequent writes are
            // attempted, rather than silently dropping every later trace event once
            // the stream latches a failbit.
            _output_stream.clear();
        }
    }
}

std::vector<runtime_event> event_log::snapshot() const
{
    ::dsn::service::zauto_lock guard(_lock);
    return std::vector<runtime_event>(_events.begin(), _events.end());
}

bool event_log::replay_value(const std::string &name, std::string *value) const
{
    ::dsn::service::zauto_lock guard(_lock);
    const std::map<std::string, std::string>::const_iterator it = _replay_values.find(name);
    if (it == _replay_values.end())
    {
        return false;
    }

    if (value != nullptr)
    {
        *value = it->second;
    }
    return true;
}

bool event_log::replay_tool_result(const std::string &tool,
                                   const std::string &arguments,
                                   bool *ok,
                                   std::string *result) const
{
    ::dsn::service::zauto_lock guard(_lock);
    const std::map<std::string, runtime_event>::const_iterator it =
        _replay_tool_events.find(tool_replay_key(tool, arguments));
    if (it == _replay_tool_events.end())
    {
        return false;
    }

    if (ok != nullptr)
    {
        *ok = it->second.kind == "tool.ok";
    }
    if (result != nullptr)
    {
        *result = tool_result_from_event_value(it->second.value);
    }
    return true;
}

bool event_log::replay_llm_response(const std::string &provider, std::string *response)
{
    ::dsn::service::zauto_lock guard(_lock);
    size_t &cursor = _replay_llm_cursor[provider];
    while (cursor < _replay_llm_events.size())
    {
        const runtime_event &event = _replay_llm_events[cursor++];
        if (event.name != provider)
        {
            continue;
        }
        if (response != nullptr)
        {
            *response = event.value;
        }
        return true;
    }
    return false;
}

bool event_log::filesystem_snapshot_replay_enabled() const
{
    ::dsn::service::zauto_lock guard(_lock);
    return !_replay_filesystem_snapshots.empty();
}

bool event_log::replay_filesystem_snapshot(const std::string &target, std::string *snapshot) const
{
    ::dsn::service::zauto_lock guard(_lock);
    const std::map<std::string, std::string>::const_iterator it = _replay_filesystem_snapshots.find(target);
    if (it == _replay_filesystem_snapshots.end())
    {
        return false;
    }
    if (snapshot != nullptr)
    {
        *snapshot = it->second;
    }
    return true;
}

bool event_log::workflow_schedule_replay_enabled() const
{
    ::dsn::service::zauto_lock guard(_lock);
    return !_replay_workflow_node_events.empty();
}

bool event_log::replay_workflow_node_start(const std::string &node_id, std::string *expected_node_id)
{
    ::dsn::service::zauto_lock guard(_lock);
    if (_replay_workflow_node_events.empty())
    {
        if (expected_node_id != nullptr)
        {
            expected_node_id->clear();
        }
        return true;
    }
    if (_replay_workflow_node_cursor >= _replay_workflow_node_events.size())
    {
        if (expected_node_id != nullptr)
        {
            *expected_node_id = "<end-of-recorded-workflow-schedule>";
        }
        return false;
    }

    const runtime_event &event = _replay_workflow_node_events[_replay_workflow_node_cursor];
    if (expected_node_id != nullptr)
    {
        *expected_node_id = event.name;
    }
    if (event.name != node_id)
    {
        return false;
    }
    ++_replay_workflow_node_cursor;
    return true;
}

bool event_log::replay_enabled() const
{
    ::dsn::service::zauto_lock guard(_lock);
    return _replay_enabled;
}

uint64_t event_log::last_sequence() const
{
    ::dsn::service::zauto_lock guard(_lock);
    return _last_sequence;
}

nucleus_runtime::nucleus_runtime() : _trace_id(make_trace_id()) {}

bool nucleus_runtime::enable_replay(const std::string &path, std::string *error)
{
    agent_task task;
    task.id = _trace_id;
    task.name = "runtime.replay";
    task.input = path;

    std::string load_error;
    const bool ok = _log.load_replay_file(path, &load_error);
    if (!ok)
    {
        if (error != nullptr)
        {
            *error = load_error;
        }
        record_failure(task, "replay", "load_failed", load_error, false, "event_log");
        return false;
    }

    record(task, "replay.load", path, "ok");
    return true;
}

void nucleus_runtime::begin_task(const agent_task &task)
{
    {
        ::dsn::service::zauto_lock guard(_timing_lock);
        _task_start_ms[task.id].push_back(::dsn_now_ms());
    }
    record(task, "task.begin", task.name, task.input);
}

void nucleus_runtime::finish_task(const agent_task &task, const std::string &status)
{
    record(task, "task.finish", task.name, status);
    uint64_t start_ms = 0;
    bool found = false;
    {
        ::dsn::service::zauto_lock guard(_timing_lock);
        std::map<std::string, std::vector<uint64_t>>::iterator it = _task_start_ms.find(task.id);
        if (it != _task_start_ms.end() && !it->second.empty())
        {
            start_ms = it->second.front();
            found = true;
            it->second.erase(it->second.begin());
            if (it->second.empty())
            {
                _task_start_ms.erase(it);
            }
        }
    }
    if (found)
    {
        const uint64_t now_ms = ::dsn_now_ms();
        metrics_registry::instance().observe_task_latency_ms(now_ms >= start_ms ? now_ms - start_ms : 0);
    }
}

void nucleus_runtime::record_llm_request(const agent_task &task, const std::string &provider, const std::string &model)
{
    {
        ::dsn::service::zauto_lock guard(_timing_lock);
        _llm_start_ms[task.id + "\x1f" + provider].push_back(::dsn_now_ms());
    }
    record(task, "llm.request", provider, model);
}

void nucleus_runtime::record_llm_response(const agent_task &task,
                                          const std::string &provider,
                                          const std::string &response)
{
    record(task, "llm.response", provider, response);
    finalize_llm_timing(task, provider);
}

void nucleus_runtime::record_llm_failure(const agent_task &task, const std::string &provider)
{
    finalize_llm_timing(task, provider);
}

void nucleus_runtime::finalize_llm_timing(const agent_task &task, const std::string &provider)
{
    uint64_t start_ms = 0;
    bool found = false;
    {
        ::dsn::service::zauto_lock guard(_timing_lock);
        std::map<std::string, std::vector<uint64_t>>::iterator it = _llm_start_ms.find(task.id + "\x1f" + provider);
        if (it != _llm_start_ms.end() && !it->second.empty())
        {
            start_ms = it->second.front();
            found = true;
            it->second.erase(it->second.begin());
            if (it->second.empty())
            {
                _llm_start_ms.erase(it);
            }
        }
    }
    if (found)
    {
        const uint64_t now_ms = ::dsn_now_ms();
        metrics_registry::instance().observe_llm_latency_ms(now_ms >= start_ms ? now_ms - start_ms : 0);
    }
}

void nucleus_runtime::record_llm_response_chunk(const agent_task &task,
                                                const std::string &provider,
                                                size_t chunk_index,
                                                const std::string &chunk)
{
    record(task, "llm.response.chunk", provider + ":" + std::to_string(chunk_index), chunk);
}

bool nucleus_runtime::replay_llm_response(const agent_task &task, const std::string &provider, std::string *response)
{
    if (_log.replay_llm_response(provider, response))
    {
        if (response != nullptr)
        {
            *response = redact_sensitive_text(*response);
        }
        record(task, "replay", "llm:" + provider, response == nullptr ? "" : *response);
        return true;
    }

    if (_log.replay_enabled())
    {
        record_event(task,
                     "replay.miss",
                     "llm:" + provider,
                     "missing replay value for llm provider response",
                     "replay",
                     "missing_llm_response",
                     "event_log",
                     false,
                     0);
    }
    return false;
}

void nucleus_runtime::record_tool_call(const agent_task &task,
                                       const std::string &tool,
                                       const std::string &arguments,
                                       bool ok,
                                       const std::string &result)
{
    if (ok)
    {
        record(task, "tool.ok", tool, arguments + "\n" + result);
    }
    else
    {
        record_event(task, "tool.error", tool, arguments + "\n" + result, "tool", "tool_error", "tool_agent", false, 0);
    }
}

void nucleus_runtime::record_external_effect(const agent_task &task,
                                             const std::string &effect_class,
                                             const std::string &operation,
                                             const std::string &fingerprint,
                                             const std::string &replay_policy,
                                             const std::string &status)
{
    std::ostringstream value;
    value << "effect_class=" << effect_class << "\n"
          << "operation=" << operation << "\n"
          << "fingerprint=" << fingerprint << "\n"
          << "replay_policy=" << replay_policy << "\n"
          << "status=" << status;
    record(task, "external.effect", effect_class + ":" + operation, value.str());
}

bool nucleus_runtime::replay_tool_call(const agent_task &task,
                                       const std::string &tool,
                                       const std::string &arguments,
                                       bool *ok,
                                       std::string *result)
{
    if (_log.replay_tool_result(tool, arguments, ok, result))
    {
        if (result != nullptr)
        {
            *result = redact_sensitive_text(*result);
        }
        record(task, "replay", "tool:" + tool, arguments + "\n" + (result == nullptr ? "" : *result));
        return true;
    }

    if (_log.replay_enabled())
    {
        record_event(task,
                     "replay.miss",
                     "tool:" + tool,
                     "missing replay value for tool arguments: " + arguments,
                     "replay",
                     "missing_tool_result",
                     "event_log",
                     false,
                     0);
    }
    return false;
}

void nucleus_runtime::record_filesystem_snapshot(const agent_task &task,
                                                 const std::string &target,
                                                 const std::string &snapshot)
{
    record(task, "filesystem.snapshot", target, snapshot);
}

bool nucleus_runtime::replay_filesystem_snapshot(const agent_task &task,
                                                 const std::string &target,
                                                 const std::string &current_snapshot,
                                                 std::string *error)
{
    if (!_log.filesystem_snapshot_replay_enabled())
    {
        return true;
    }

    std::string recorded_snapshot;
    if (!_log.replay_filesystem_snapshot(target, &recorded_snapshot))
    {
        const std::string message = "filesystem replay missing snapshot for " + target;
        if (error != nullptr)
        {
            *error = message;
        }
        record_event(task,
                     "replay.miss",
                     "filesystem:" + target,
                     message,
                     "replay",
                     "missing_filesystem_snapshot",
                     "filesystem",
                     false,
                     0);
        return false;
    }

    if (recorded_snapshot != current_snapshot)
    {
        const std::string message = "filesystem replay snapshot mismatch for " + target;
        if (error != nullptr)
        {
            *error = message;
        }
        record_event(task,
                     "replay.miss",
                     "filesystem:" + target,
                     message,
                     "replay",
                     "filesystem_snapshot_mismatch",
                     "filesystem",
                     false,
                     0);
        return false;
    }

    record(task, "replay", "filesystem:" + target, "snapshot matched");
    return true;
}

void nucleus_runtime::record_workflow_node_start(const agent_task &task,
                                                 const std::string &node_id,
                                                 const std::string &action)
{
    record(task, "workflow.node.start", node_id, action);
}

void nucleus_runtime::record_workflow_node_finish(const agent_task &task,
                                                  const std::string &node_id,
                                                  const std::string &status)
{
    record(task, "workflow.node.finish", node_id, status);
}

bool nucleus_runtime::replay_workflow_node_start(const agent_task &task,
                                                 const std::string &node_id,
                                                 const std::string &action,
                                                 std::string *error)
{
    if (!_log.workflow_schedule_replay_enabled())
    {
        return true;
    }

    std::string expected_node_id;
    if (_log.replay_workflow_node_start(node_id, &expected_node_id))
    {
        record(task, "replay", "workflow.node:" + node_id, action);
        return true;
    }

    const std::string message = "workflow scheduler replay mismatch: expected node '" +
                                expected_node_id + "' but runtime selected '" + node_id + "'";
    if (error != nullptr)
    {
        *error = message;
    }
    record_event(task,
                 "replay.miss",
                 "workflow.node:" + node_id,
                 message,
                 "replay",
                 "workflow_schedule_mismatch",
                 "workflow",
                 false,
                 0);
    return false;
}

bool nucleus_runtime::replay_enabled() const
{
    return _log.replay_enabled();
}

void nucleus_runtime::record_route_decision(const agent_task &task,
                                            const std::string &capability,
                                            const std::string &agent_id)
{
    record(task, "routing.decision", capability, agent_id);
}

void nucleus_runtime::record_failure(const agent_task &task,
                                     const std::string &failure_class,
                                     const std::string &code,
                                     const std::string &message,
                                     bool retryable,
                                     const std::string &source,
                                     uint32_t retry_attempt)
{
    record_event(task, "failure", code, message, failure_class, code, source, retryable, retry_attempt);
}

void nucleus_runtime::record_retry(const agent_task &task,
                                   const std::string &operation,
                                   uint32_t retry_attempt,
                                   const std::string &reason)
{
    record_event(task, "retry", operation, reason, "", "", "", true, retry_attempt);
}

void nucleus_runtime::record_model_breaker_open(const agent_task &task,
                                                const std::string &provider,
                                                uint32_t consecutive_failures)
{
    record_event(task,
                 "model.breaker.open",
                 provider,
                 "consecutive_failures=" + std::to_string(consecutive_failures),
                 "",
                 "",
                 "",
                 false,
                 0);
}

void nucleus_runtime::record_model_breaker_short_circuit(const agent_task &task,
                                                         const std::string &provider,
                                                         const std::string &breaker_state)
{
    record_event(task, "model.breaker.short_circuit", provider, breaker_state, "", "", "", false, 0);
}

void nucleus_runtime::record_model_admission_rejected(const agent_task &task,
                                                      const std::string &provider,
                                                      uint32_t in_flight,
                                                      uint32_t limit)
{
    record_event(task,
                 "model.admission.rejected",
                 provider,
                 "in_flight=" + std::to_string(in_flight) + " limit=" + std::to_string(limit),
                 "",
                 "",
                 "",
                 false,
                 0);
}

void nucleus_runtime::record_model_admission_delayed(const agent_task &task,
                                                     const std::string &provider,
                                                     uint32_t in_flight,
                                                     uint32_t delay_ms)
{
    record_event(task,
                 "model.admission.delayed",
                 provider,
                 "in_flight=" + std::to_string(in_flight) + " delay_ms=" + std::to_string(delay_ms),
                 "",
                 "",
                 "",
                 false,
                 0);
}

void nucleus_runtime::record_model_rate_limited(const agent_task &task,
                                                const std::string &provider,
                                                uint32_t limit_per_min)
{
    record_event(task,
                 "model.rate.limited",
                 provider,
                 "requests_per_min=" + std::to_string(limit_per_min),
                 "",
                 "",
                 "",
                 false,
                 0);
}

void nucleus_runtime::record_model_rate_delayed(const agent_task &task,
                                                const std::string &provider,
                                                uint32_t delay_ms)
{
    record_event(task,
                 "model.rate.delayed",
                 provider,
                 "delay_ms=" + std::to_string(delay_ms),
                 "",
                 "",
                 "",
                 false,
                 0);
}

void nucleus_runtime::record_tool_admission_rejected(const agent_task &task,
                                                     const std::string &tool,
                                                     uint32_t in_flight,
                                                     uint32_t limit)
{
    record_event(task,
                 "tool.admission.rejected",
                 tool,
                 "in_flight=" + std::to_string(in_flight) + " limit=" + std::to_string(limit),
                 "",
                 "",
                 "",
                 false,
                 0);
}

void nucleus_runtime::record_tool_admission_delayed(const agent_task &task,
                                                    const std::string &tool,
                                                    uint32_t in_flight,
                                                    uint32_t delay_ms)
{
    record_event(task,
                 "tool.admission.delayed",
                 tool,
                 "in_flight=" + std::to_string(in_flight) + " delay_ms=" + std::to_string(delay_ms),
                 "",
                 "",
                 "",
                 false,
                 0);
}

void nucleus_runtime::record_tool_rate_limited(const agent_task &task,
                                               const std::string &tool,
                                               uint32_t limit_per_min)
{
    record_event(task,
                 "tool.rate.limited",
                 tool,
                 "requests_per_min=" + std::to_string(limit_per_min),
                 "",
                 "",
                 "",
                 false,
                 0);
}

void nucleus_runtime::record_tool_rate_delayed(const agent_task &task,
                                               const std::string &tool,
                                               uint32_t delay_ms)
{
    record_event(task,
                 "tool.rate.delayed",
                 tool,
                 "delay_ms=" + std::to_string(delay_ms),
                 "",
                 "",
                 "",
                 false,
                 0);
}

void nucleus_runtime::record_remote_agent_breaker_open(const agent_task &task,
                                                       const std::string &agent,
                                                       uint32_t consecutive_failures)
{
    record_event(task,
                 "remote_agent.breaker.open",
                 agent,
                 "consecutive_failures=" + std::to_string(consecutive_failures),
                 "",
                 "",
                 "",
                 false,
                 0);
}

void nucleus_runtime::record_remote_agent_breaker_short_circuit(const agent_task &task,
                                                                const std::string &agent,
                                                                const std::string &breaker_state)
{
    record_event(task, "remote_agent.breaker.short_circuit", agent, breaker_state, "", "", "", false, 0);
}

void nucleus_runtime::record_remote_agent_admission_rejected(const agent_task &task,
                                                             const std::string &agent,
                                                             uint32_t in_flight,
                                                             uint32_t limit)
{
    record_event(task,
                 "remote_agent.admission.rejected",
                 agent,
                 "in_flight=" + std::to_string(in_flight) + " limit=" + std::to_string(limit),
                 "",
                 "",
                 "",
                 false,
                 0);
}

void nucleus_runtime::record_remote_agent_admission_delayed(const agent_task &task,
                                                            const std::string &agent,
                                                            uint32_t in_flight,
                                                            uint32_t delay_ms)
{
    record_event(task,
                 "remote_agent.admission.delayed",
                 agent,
                 "in_flight=" + std::to_string(in_flight) + " delay_ms=" + std::to_string(delay_ms),
                 "",
                 "",
                 "",
                 false,
                 0);
}

void nucleus_runtime::record_remote_agent_rate_limited(const agent_task &task,
                                                       const std::string &agent,
                                                       uint32_t limit_per_min)
{
    record_event(task,
                 "remote_agent.rate.limited",
                 agent,
                 "requests_per_min=" + std::to_string(limit_per_min),
                 "",
                 "",
                 "",
                 false,
                 0);
}

void nucleus_runtime::record_remote_agent_rate_delayed(const agent_task &task,
                                                       const std::string &agent,
                                                       uint32_t delay_ms)
{
    record_event(task,
                 "remote_agent.rate.delayed",
                 agent,
                 "delay_ms=" + std::to_string(delay_ms),
                 "",
                 "",
                 "",
                 false,
                 0);
}

void nucleus_runtime::record_remote_agent_endpoint_invalid(const agent_task &task,
                                                           const std::string &agent,
                                                           const std::string &reason)
{
    record_event(task, "remote_agent.endpoint.invalid", agent, reason, "", "", "", false, 0);
}

void nucleus_runtime::record_overload_admission_rejected(const agent_task &task,
                                                         uint32_t in_flight,
                                                         uint32_t limit)
{
    record_event(task,
                 "overload.admission.rejected",
                 "process-wide",
                 "in_flight=" + std::to_string(in_flight) + " limit=" + std::to_string(limit),
                 "",
                 "",
                 "",
                 false,
                 0);
}

void nucleus_runtime::record_overload_admission_delayed(const agent_task &task,
                                                        uint32_t in_flight,
                                                        uint32_t delay_ms)
{
    record_event(task,
                 "overload.admission.delayed",
                 "process-wide",
                 "in_flight=" + std::to_string(in_flight) + " delay_ms=" + std::to_string(delay_ms),
                 "",
                 "",
                 "",
                 false,
                 0);
}

void nucleus_runtime::record_overload_rate_limited(const agent_task &task, uint32_t limit_per_min)
{
    record_event(task,
                 "overload.rate.limited",
                 "process-wide",
                 "requests_per_min=" + std::to_string(limit_per_min),
                 "",
                 "",
                 "",
                 false,
                 0);
}

void nucleus_runtime::record_overload_rate_delayed(const agent_task &task, uint32_t delay_ms)
{
    record_event(task,
                 "overload.rate.delayed",
                 "process-wide",
                 "delay_ms=" + std::to_string(delay_ms),
                 "",
                 "",
                 "",
                 false,
                 0);
}

std::string nucleus_runtime::resolve_nondeterminism(const agent_task &task,
                                                    const std::string &name,
                                                    const std::string &source,
                                                    const std::function<std::string()> &generator)
{
    std::string value;
    if (_log.replay_value(name, &value))
    {
        record(task, "replay", name, value);
        return value;
    }

    if (_log.replay_enabled())
    {
        record_event(task,
                     "replay.miss",
                     name,
                     "missing replay value for nondeterminism key from " + source,
                     "replay",
                     "missing_nondeterminism",
                     "event_log",
                     false,
                     0);
    }

    value = generator();
    record(task, "nondeterminism", name, value);
    return value;
}

void nucleus_runtime::record(const agent_task &task,
                             const std::string &kind,
                             const std::string &name,
                             const std::string &value)
{
    record_event(task, kind, name, value, "", "", "", false, 0);
}

void nucleus_runtime::record_event(const agent_task &task,
                                   const std::string &kind,
                                   const std::string &name,
                                   const std::string &value,
                                   const std::string &failure_class,
                                   const std::string &failure_code,
                                   const std::string &failure_source,
                                   bool retryable,
                                   uint32_t retry_attempt)
{
    runtime_event event;
    event.trace_id = _trace_id;
    event.task_id = task.id;
    event.kind = kind;
    event.name = name;
    event.value = value;
    event.timestamp = now_utc_string();
    event.failure_class = failure_class;
    event.failure_code = failure_code;
    event.failure_source = failure_source;
    event.retryable = retryable;
    event.retry_attempt = retry_attempt;
    _log.append(event);
    metrics_registry::instance().on_event(kind, failure_class);
}

} // namespace rasn
} // namespace dsn
