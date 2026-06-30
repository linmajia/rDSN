#pragma once

#include "observability.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <dsn/cpp/zlocks.h>

namespace dsn {
namespace rasn {

struct agent_task
{
    std::string id;
    std::string name;
    std::string input;
};

class event_log
{
public:
    void set_output_file(const std::string &path);
    bool load_replay_file(const std::string &path, std::string *error);

    void append(runtime_event event);
    std::vector<runtime_event> snapshot() const;
    bool replay_value(const std::string &name, std::string *value) const;
    bool replay_tool_result(const std::string &tool,
                            const std::string &arguments,
                            bool *ok,
                            std::string *result) const;
    bool replay_llm_response(const std::string &provider, std::string *response);
    bool filesystem_snapshot_replay_enabled() const;
    bool replay_filesystem_snapshot(const std::string &target, std::string *snapshot) const;
    bool workflow_schedule_replay_enabled() const;
    bool replay_workflow_node_start(const std::string &node_id, std::string *expected_node_id);
    bool replay_enabled() const;
    uint64_t last_sequence() const;
    const std::string &output_file() const { return _output_file; }

private:
    std::string _output_file;
    std::vector<runtime_event> _events;
    std::map<std::string, std::string> _replay_values;
    std::map<std::string, runtime_event> _replay_tool_events;
    std::map<std::string, std::string> _replay_filesystem_snapshots;
    std::vector<runtime_event> _replay_llm_events;
    std::map<std::string, size_t> _replay_llm_cursor;
    std::vector<runtime_event> _replay_workflow_node_events;
    size_t _replay_workflow_node_cursor = 0;
    bool _replay_enabled = false;
    uint64_t _last_sequence = 0;
    mutable ::dsn::service::zlock _lock;
};

class nucleus_runtime
{
public:
    nucleus_runtime();

    const std::string &trace_id() const { return _trace_id; }
    void set_trace_file(const std::string &path) { _log.set_output_file(path); }
    const std::string &trace_file() const { return _log.output_file(); }
    bool enable_replay(const std::string &path, std::string *error);

    void begin_task(const agent_task &task);
    void finish_task(const agent_task &task, const std::string &status);
    void record_llm_request(const agent_task &task, const std::string &provider, const std::string &model);
    void record_llm_response(const agent_task &task, const std::string &provider, const std::string &response);
    void record_llm_response_chunk(const agent_task &task,
                                   const std::string &provider,
                                   size_t chunk_index,
                                   const std::string &chunk);
    bool replay_llm_response(const agent_task &task, const std::string &provider, std::string *response);
    void record_tool_call(const agent_task &task,
                          const std::string &tool,
                          const std::string &arguments,
                          bool ok,
                          const std::string &result);
    void record_external_effect(const agent_task &task,
                                const std::string &effect_class,
                                const std::string &operation,
                                const std::string &fingerprint,
                                const std::string &replay_policy,
                                const std::string &status);
    bool replay_tool_call(const agent_task &task,
                          const std::string &tool,
                          const std::string &arguments,
                          bool *ok,
                          std::string *result);
    void record_filesystem_snapshot(const agent_task &task,
                                    const std::string &target,
                                    const std::string &snapshot);
    bool replay_filesystem_snapshot(const agent_task &task,
                                    const std::string &target,
                                    const std::string &current_snapshot,
                                    std::string *error);
    void record_workflow_node_start(const agent_task &task,
                                    const std::string &node_id,
                                    const std::string &action);
    void record_workflow_node_finish(const agent_task &task,
                                     const std::string &node_id,
                                     const std::string &status);
    bool replay_workflow_node_start(const agent_task &task,
                                    const std::string &node_id,
                                    const std::string &action,
                                    std::string *error);
    bool replay_enabled() const;
    void record_route_decision(const agent_task &task,
                               const std::string &capability,
                               const std::string &agent_id);
    void record_failure(const agent_task &task,
                        const std::string &failure_class,
                        const std::string &code,
                        const std::string &message,
                        bool retryable,
                        const std::string &source,
                        uint32_t retry_attempt = 0);
    void record_retry(const agent_task &task,
                      const std::string &operation,
                      uint32_t retry_attempt,
                      const std::string &reason);
    std::string resolve_nondeterminism(const agent_task &task,
                                       const std::string &name,
                                       const std::string &source,
                                       const std::function<std::string()> &generator);
    std::vector<runtime_event> events() const { return _log.snapshot(); }

private:
    void record(const agent_task &task,
                const std::string &kind,
                const std::string &name,
                const std::string &value);
    void record_event(const agent_task &task,
                      const std::string &kind,
                      const std::string &name,
                      const std::string &value,
                      const std::string &failure_class,
                      const std::string &failure_code,
                      const std::string &failure_source,
                      bool retryable,
                      uint32_t retry_attempt);

    std::string _trace_id;
    event_log _log;
};

std::string now_utc_string();
std::string make_trace_id();
std::string json_escape(const std::string &value);
std::string trim(const std::string &value);
std::string normalize_platform_path(const std::string &path);
std::vector<std::string> split_words(const std::string &line);

} // namespace rasn
} // namespace dsn
