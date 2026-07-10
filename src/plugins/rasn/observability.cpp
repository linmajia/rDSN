#include <rasn/observability.h>

#include <rasn/agent_services.h>
#include <rasn/state_service.h>

#include <dsn/cpp/zlocks.h>
#include <dsn/service_api_cpp.h>

#include <cctype>
#include <map>
#include <sstream>

namespace dsn {
namespace rasn {

namespace {

observability_response error_response(const std::string &error)
{
    observability_response response;
    response.ok = false;
    response.error = error;
    return response;
}

bool event_matches(const runtime_event &event, const observability_query_request &request)
{
    if (request.schema_version != RASN_OBSERVABILITY_SCHEMA_VERSION)
    {
        return false;
    }
    if (!request.trace_id.empty() && event.trace_id != request.trace_id)
    {
        return false;
    }
    if (!request.kind.empty() && event.kind != request.kind)
    {
        return false;
    }
    if (!request.name.empty() && event.name != request.name)
    {
        return false;
    }
    if (request.min_sequence != 0 && event.sequence < request.min_sequence)
    {
        return false;
    }
    return true;
}

uint32_t effective_limit(uint32_t limit)
{
    return limit == 0 ? 1000 : limit;
}

uint64_t last_sequence_of(const std::vector<runtime_event> &events)
{
    uint64_t last = 0;
    for (const runtime_event &event : events)
    {
        if (event.sequence > last)
        {
            last = event.sequence;
        }
    }
    return last;
}

state_response default_observability_state_writer(const state_record &record)
{
    return global_state_store().put(record);
}

::dsn::service::zlock &observability_state_writer_lock()
{
    static ::dsn::service::zlock lock;
    return lock;
}

observability_state_writer &observability_state_writer_slot()
{
    static observability_state_writer writer = &default_observability_state_writer;
    return writer;
}

state_response write_observability_state_record(const state_record &record)
{
    observability_state_writer writer = nullptr;
    {
        ::dsn::service::zauto_lock guard(observability_state_writer_lock());
        writer = observability_state_writer_slot();
    }
    return writer(record);
}

std::string safe_state_component(const std::string &value)
{
    std::string safe = value.empty() ? "global" : value;
    for (char &c : safe)
    {
        const unsigned char ch = static_cast<unsigned char>(c);
        if (!std::isalnum(ch) && c != '-' && c != '_' && c != '.')
        {
            c = '_';
        }
    }
    return safe;
}

} // namespace

failure_record failure_from_event(const runtime_event &event)
{
    failure_record failure;
    failure.sequence = event.sequence;
    failure.trace_id = event.trace_id;
    failure.task_id = event.task_id;
    failure.failure_class = event.failure_class.empty() ? event.kind : event.failure_class;
    failure.code = event.failure_code.empty() ? event.name : event.failure_code;
    failure.message = event.value;
    failure.retryable = event.retryable;
    failure.retry_attempt = event.retry_attempt;
    failure.source = event.failure_source.empty() ? event.name : event.failure_source;
    failure.timestamp = event.timestamp;
    return failure;
}

observability_response query_observability_events(const std::vector<runtime_event> &events,
                                                  const observability_query_request &request)
{
    if (request.schema_version != RASN_OBSERVABILITY_SCHEMA_VERSION)
    {
        return error_response("observability query has unsupported schema version");
    }

    observability_response response;
    response.last_sequence = last_sequence_of(events);
    const uint32_t limit = effective_limit(request.limit);
    for (const runtime_event &event : events)
    {
        if (!event_matches(event, request))
        {
            continue;
        }
        if (response.events.size() >= limit)
        {
            response.truncated = true;
            break;
        }
        response.events.push_back(event);
    }
    return response;
}

observability_response query_observability_failures(const std::vector<runtime_event> &events,
                                                    const observability_query_request &request)
{
    if (request.schema_version != RASN_OBSERVABILITY_SCHEMA_VERSION)
    {
        return error_response("observability failure query has unsupported schema version");
    }

    observability_response response;
    response.last_sequence = last_sequence_of(events);
    const uint32_t limit = effective_limit(request.limit);
    for (const runtime_event &event : events)
    {
        if (!event_matches(event, request))
        {
            continue;
        }
        if (event.failure_class.empty() && event.kind.find(".error") == std::string::npos &&
            event.kind != "failure" && event.kind != "replay.miss")
            continue;
        if (response.failures.size() >= limit)
        {
            response.truncated = true;
            break;
        }
        response.failures.push_back(failure_from_event(event));
    }
    return response;
}

std::string format_observability_event(const runtime_event &event)
{
    std::ostringstream oss;
    oss << "#" << event.sequence
        << " trace=" << event.trace_id
        << " task=" << event.task_id
        << " kind=" << event.kind
        << " name=" << event.name;
    if (!event.failure_class.empty())
    {
        oss << " failure=" << event.failure_class << "/" << event.failure_code;
    }
    oss << " value=" << event.value;
    return oss.str();
}

std::string format_failure_record(const failure_record &failure)
{
    std::ostringstream oss;
    oss << "#" << failure.sequence
        << " trace=" << failure.trace_id
        << " task=" << failure.task_id
        << " failure=" << failure.failure_class
        << " code=" << failure.code
        << " retryable=" << (failure.retryable ? "true" : "false")
        << " message=" << failure.message;
    return oss.str();
}

std::string format_observability_timeline(const std::vector<runtime_event> &events, const std::string &trace_id)
{
    std::ostringstream oss;
    oss << "Trace timeline";
    if (!trace_id.empty())
    {
        oss << " trace=" << trace_id;
    }
    oss << "\n";

    uint32_t emitted = 0;
    for (const runtime_event &event : events)
    {
        if (!trace_id.empty() && event.trace_id != trace_id)
        {
            continue;
        }
        oss << format_observability_event(event) << "\n";
        ++emitted;
        if (emitted >= 200)
        {
            oss << "... truncated at 200 events\n";
            break;
        }
    }
    if (emitted == 0)
    {
        oss << "<no matching events>\n";
    }
    return oss.str();
}

std::string diagnose_observability_events(const std::vector<runtime_event> &events, const std::string &trace_id)
{
    std::map<std::string, uint32_t> by_kind;
    std::vector<failure_record> failures;
    uint32_t matched = 0;
    uint32_t retries = 0;
    uint32_t replay_misses = 0;
    uint32_t nondeterministic = 0;

    for (const runtime_event &event : events)
    {
        if (!trace_id.empty() && event.trace_id != trace_id)
        {
            continue;
        }
        ++matched;
        ++by_kind[event.kind];
        if (event.kind == "retry")
        {
            ++retries;
        }
        if (event.kind == "replay.miss")
        {
            ++replay_misses;
        }
        if (event.kind == "nondeterminism" || event.kind == "llm.request")
        {
            ++nondeterministic;
        }
        if (!event.failure_class.empty() || event.kind == "failure" || event.kind == "tool.error" ||
            event.kind == "replay.miss")
        {
            failures.push_back(failure_from_event(event));
        }
    }

    std::ostringstream oss;
    oss << "Observability diagnosis";
    if (!trace_id.empty())
    {
        oss << " trace=" << trace_id;
    }
    oss << "\n";
    oss << "events=" << matched
        << " failures=" << failures.size()
        << " retries=" << retries
        << " replay_misses=" << replay_misses
        << " nondeterministic_points=" << nondeterministic << "\n";
    oss << "event kinds:";
    if (by_kind.empty())
    {
        oss << " <none>";
    }
    for (const std::map<std::string, uint32_t>::value_type &entry : by_kind)
    {
        oss << " " << entry.first << "=" << entry.second;
    }
    oss << "\n";

    if (!failures.empty())
    {
        oss << "failures:\n";
        for (size_t i = 0; i < failures.size() && i < 10; ++i)
        {
            oss << "- " << format_failure_record(failures[i]) << "\n";
        }
    }

    if (replay_misses != 0)
    {
        oss << "recommendation: replay trace is incomplete; capture nondeterminism events before replaying this run.\n";
    }
    else if (!failures.empty())
    {
        oss << "recommendation: inspect failure source and retryability before resubmitting dependent workflow nodes.\n";
    }
    else if (matched != 0)
    {
        oss << "recommendation: trace has no classified failures; use timeline for ordering and latency investigation.\n";
    }
    return oss.str();
}

void set_observability_state_writer(observability_state_writer writer)
{
    ::dsn::service::zauto_lock guard(observability_state_writer_lock());
    observability_state_writer_slot() = writer == nullptr ? &default_observability_state_writer : writer;
}

void reset_observability_state_writer()
{
    set_observability_state_writer(nullptr);
}

state_response index_observability_snapshot(const observability_response &snapshot,
                                            const std::string &trace_id,
                                            const std::string &trace_file)
{
    state_record record;
    record.kind = "observability_snapshot";
    record.scope = "rasn.observability";
    record.key = "observability-snapshot/" + safe_state_component(trace_id) + "/" + std::to_string(snapshot.last_sequence);
    std::ostringstream value;
    value << "trace_id=" << trace_id << "\n"
          << "trace_file=" << trace_file << "\n"
          << "events=" << snapshot.events.size() << "\n"
          << "failures=" << snapshot.failures.size() << "\n"
          << "last_sequence=" << snapshot.last_sequence << "\n"
          << "truncated=" << (snapshot.truncated ? "true" : "false") << "\n"
          << "timestamp=" << now_utc_string() << "\n";
    record.value = value.str();
    return write_observability_state_record(record);
}

void rasn_observability_rpc_service::open_service()
{
    dinfo("opening rasn.observability serverlet");
    this->register_async_rpc_handler(
        RPC_RASN_OBSERVABILITY_QUERY, "query", &rasn_observability_rpc_service::on_query);
    this->register_async_rpc_handler(
        RPC_RASN_OBSERVABILITY_FAILURES, "failures", &rasn_observability_rpc_service::on_failures);
    this->register_async_rpc_handler(
        RPC_RASN_OBSERVABILITY_LOAD_REPLAY, "load_replay", &rasn_observability_rpc_service::on_load_replay);
    this->register_async_rpc_handler(
        RPC_RASN_OBSERVABILITY_SNAPSHOT, "snapshot", &rasn_observability_rpc_service::on_snapshot);
}

void rasn_observability_rpc_service::close_service()
{
    dinfo("closing rasn.observability serverlet");
    this->unregister_rpc_handler(RPC_RASN_OBSERVABILITY_QUERY);
    this->unregister_rpc_handler(RPC_RASN_OBSERVABILITY_FAILURES);
    this->unregister_rpc_handler(RPC_RASN_OBSERVABILITY_LOAD_REPLAY);
    this->unregister_rpc_handler(RPC_RASN_OBSERVABILITY_SNAPSHOT);
}

void rasn_observability_rpc_service::on_query(const observability_query_request &request,
                                              ::dsn::rpc_replier<observability_response> &reply)
{
    reply(query_observability_events(global_rasn_services().runtime().events(), request));
}

void rasn_observability_rpc_service::on_failures(const observability_query_request &request,
                                                 ::dsn::rpc_replier<observability_response> &reply)
{
    reply(query_observability_failures(global_rasn_services().runtime().events(), request));
}

void rasn_observability_rpc_service::on_load_replay(const replay_load_request &request,
                                                    ::dsn::rpc_replier<observability_response> &reply)
{
    if (request.schema_version != RASN_OBSERVABILITY_SCHEMA_VERSION)
    {
        reply(error_response("replay load request has unsupported schema version"));
        return;
    }
    if (request.path.empty())
    {
        reply(error_response("replay load request missing path"));
        return;
    }

    std::string error;
    if (!global_rasn_services().runtime().enable_replay(request.path, &error))
    {
        reply(error_response(error));
        return;
    }

    observability_query_request query;
    query.kind = "replay.load";
    query.limit = 10;
    reply(query_observability_events(global_rasn_services().runtime().events(), query));
}

void rasn_observability_rpc_service::on_snapshot(const std::string &request,
                                                 ::dsn::rpc_replier<observability_response> &reply)
{
    observability_query_request query;
    query.limit = 0;
    const std::vector<runtime_event> events = global_rasn_services().runtime().events();
    observability_response snapshot = query_observability_events(events, query);
    if (snapshot.ok)
    {
        observability_response failures = query_observability_failures(events, query);
        if (!failures.ok)
        {
            reply(failures);
            return;
        }
        snapshot.failures = failures.failures;
        const state_response indexed = index_observability_snapshot(
            snapshot, global_rasn_services().runtime().trace_id(), global_rasn_services().runtime().trace_file());
        if (!indexed.ok)
        {
            snapshot.ok = false;
            snapshot.error = "failed to index observability snapshot in state: " + indexed.error;
        }
    }
    reply(snapshot);
}

std::pair< ::dsn::error_code, observability_response>
rasn_observability_client::query_sync(const observability_query_request &request,
                                      std::chrono::milliseconds timeout,
                                      int thread_hash,
                                      uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<observability_response>(::dsn::rpc::call(
        _server, RPC_RASN_OBSERVABILITY_QUERY, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

std::pair< ::dsn::error_code, observability_response>
rasn_observability_client::failures_sync(const observability_query_request &request,
                                         std::chrono::milliseconds timeout,
                                         int thread_hash,
                                         uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<observability_response>(::dsn::rpc::call(
        _server, RPC_RASN_OBSERVABILITY_FAILURES, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

std::pair< ::dsn::error_code, observability_response>
rasn_observability_client::load_replay_sync(const replay_load_request &request,
                                            std::chrono::milliseconds timeout,
                                            int thread_hash,
                                            uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<observability_response>(::dsn::rpc::call(
        _server, RPC_RASN_OBSERVABILITY_LOAD_REPLAY, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

std::pair< ::dsn::error_code, observability_response>
rasn_observability_client::snapshot_sync(const std::string &request,
                                         std::chrono::milliseconds timeout,
                                         int thread_hash,
                                         uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<observability_response>(::dsn::rpc::call(
        _server, RPC_RASN_OBSERVABILITY_SNAPSHOT, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

::dsn::error_code rasn_observability_app::start(int argc, char **argv)
{
    global_rasn_services().acquire();
    _rpc.open_service();
    return ::dsn::ERR_OK;
}

::dsn::error_code rasn_observability_app::stop(bool cleanup)
{
    _rpc.close_service();
    global_rasn_services().release();
    return ::dsn::ERR_OK;
}

} // namespace rasn
} // namespace dsn
