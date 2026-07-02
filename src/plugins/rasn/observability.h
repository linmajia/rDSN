#pragma once

#include <rasn/rasn.code.definition.h>

#include <dsn/cpp/serialization.h>
#include <dsn/service_api_cpp.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dsn {
namespace rasn {

// Bound the speculative up-front reserve for a length-prefixed sequence that is
// unmarshalled from an untrusted or possibly-corrupt peer.
//
// Every rASN unmarshall_*() helper reads a uint32_t element count off the wire
// and then reserves that many elements before decoding any of them. The count
// is attacker-controllable, so reserving it directly -- values.reserve(count)
// -- lets a single crafted or truncated message claim up to ~4 billion elements
// and trigger a huge allocation (an out-of-memory abort) before a single
// element is read.
//
// Mirror rDSN's own Thrift deserializer (see read_base() in
// include/dsn/cpp/serialization_helper/thrift_helper.h): cap the reservation to
// a fixed *byte* budget rather than a flat element count, because the memory
// reserved is count * sizeof(T) and a flat count would reserve wildly different
// amounts for different element types. This is the only speculative allocation
// per call, so it directly caps per-message memory. The decode loop still runs
// to the claimed count, but binary_reader::read() throws std::out_of_range at
// end-of-stream the moment the peer's claim outruns the buffer, so a lying
// count can drive neither a large allocation nor an unbounded loop -- it becomes
// a clean parse error instead of an OOM abort. Realistic lists stay well under
// the budget and so are unaffected; larger legitimate lists simply grow via the
// vector's amortized O(1) reallocation.
template <typename T>
inline uint32_t rasn_bounded_reserve_count(uint32_t claimed_count)
{
    const uint32_t max_preallocate_bytes = 64 * 1024;
    uint32_t cap = max_preallocate_bytes / static_cast<uint32_t>(sizeof(T));
    if (cap == 0)
    {
        cap = 1;
    }
    return claimed_count < cap ? claimed_count : cap;
}

struct state_record;
struct state_response;

const uint32_t RASN_OBSERVABILITY_SCHEMA_VERSION = 1;

struct runtime_event
{
    uint32_t schema_version = RASN_OBSERVABILITY_SCHEMA_VERSION;
    uint64_t sequence = 0;
    std::string trace_id;
    std::string task_id;
    std::string kind;
    std::string name;
    std::string value;
    std::string timestamp;
    std::string failure_class;
    std::string failure_code;
    std::string failure_source;
    bool retryable = false;
    uint32_t retry_attempt = 0;
};

struct failure_record
{
    uint32_t schema_version = RASN_OBSERVABILITY_SCHEMA_VERSION;
    uint64_t sequence = 0;
    std::string trace_id;
    std::string task_id;
    std::string failure_class;
    std::string code;
    std::string message;
    bool retryable = false;
    uint32_t retry_attempt = 0;
    std::string source;
    std::string timestamp;
};

struct observability_query_request
{
    uint32_t schema_version = RASN_OBSERVABILITY_SCHEMA_VERSION;
    std::string trace_id;
    std::string kind;
    std::string name;
    uint64_t min_sequence = 0;
    uint32_t limit = 100;
};

struct replay_load_request
{
    uint32_t schema_version = RASN_OBSERVABILITY_SCHEMA_VERSION;
    std::string path;
};

struct observability_response
{
    uint32_t schema_version = RASN_OBSERVABILITY_SCHEMA_VERSION;
    bool ok = true;
    std::string error;
    std::vector<runtime_event> events;
    std::vector<failure_record> failures;
    uint64_t last_sequence = 0;
    bool truncated = false;
};

failure_record failure_from_event(const runtime_event &event);
observability_response query_observability_events(const std::vector<runtime_event> &events,
                                                  const observability_query_request &request);
observability_response query_observability_failures(const std::vector<runtime_event> &events,
                                                    const observability_query_request &request);
std::string format_observability_event(const runtime_event &event);
std::string format_failure_record(const failure_record &failure);
std::string format_observability_timeline(const std::vector<runtime_event> &events, const std::string &trace_id);
std::string diagnose_observability_events(const std::vector<runtime_event> &events, const std::string &trace_id);
typedef state_response (*observability_state_writer)(const state_record &record);
void set_observability_state_writer(observability_state_writer writer);
void reset_observability_state_writer();
state_response index_observability_snapshot(const observability_response &snapshot,
                                            const std::string &trace_id,
                                            const std::string &trace_file);

inline void marshall(::dsn::binary_writer &writer, const runtime_event &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.sequence);
    writer.write(value.trace_id);
    writer.write(value.task_id);
    writer.write(value.kind);
    writer.write(value.name);
    writer.write(value.value);
    writer.write(value.timestamp);
    writer.write(value.failure_class);
    writer.write(value.failure_code);
    writer.write(value.failure_source);
    writer.write(value.retryable);
    writer.write(value.retry_attempt);
}

inline void unmarshall(::dsn::binary_reader &reader, runtime_event &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.sequence);
    reader.read(value.trace_id);
    reader.read(value.task_id);
    reader.read(value.kind);
    reader.read(value.name);
    reader.read(value.value);
    reader.read(value.timestamp);
    reader.read(value.failure_class);
    reader.read(value.failure_code);
    reader.read(value.failure_source);
    reader.read(value.retryable);
    reader.read(value.retry_attempt);
}

inline void marshall(::dsn::binary_writer &writer, const failure_record &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.sequence);
    writer.write(value.trace_id);
    writer.write(value.task_id);
    writer.write(value.failure_class);
    writer.write(value.code);
    writer.write(value.message);
    writer.write(value.retryable);
    writer.write(value.retry_attempt);
    writer.write(value.source);
    writer.write(value.timestamp);
}

inline void unmarshall(::dsn::binary_reader &reader, failure_record &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.sequence);
    reader.read(value.trace_id);
    reader.read(value.task_id);
    reader.read(value.failure_class);
    reader.read(value.code);
    reader.read(value.message);
    reader.read(value.retryable);
    reader.read(value.retry_attempt);
    reader.read(value.source);
    reader.read(value.timestamp);
}

inline void marshall(::dsn::binary_writer &writer, const observability_query_request &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.trace_id);
    writer.write(value.kind);
    writer.write(value.name);
    writer.write(value.min_sequence);
    writer.write(value.limit);
}

inline void unmarshall(::dsn::binary_reader &reader, observability_query_request &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.trace_id);
    reader.read(value.kind);
    reader.read(value.name);
    reader.read(value.min_sequence);
    reader.read(value.limit);
}

inline void marshall(::dsn::binary_writer &writer, const replay_load_request &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.path);
}

inline void unmarshall(::dsn::binary_reader &reader, replay_load_request &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.path);
}

inline void marshall_runtime_events(::dsn::binary_writer &writer,
                                    const std::vector<runtime_event> &values,
                                    ::dsn_msg_serialize_format fmt)
{
    writer.write(static_cast<uint32_t>(values.size()));
    for (const runtime_event &value : values)
    {
        marshall(writer, value, fmt);
    }
}

inline void unmarshall_runtime_events(::dsn::binary_reader &reader,
                                      std::vector<runtime_event> &values,
                                      ::dsn_msg_serialize_format fmt)
{
    uint32_t count = 0;
    reader.read(count);
    values.clear();
    values.reserve(rasn_bounded_reserve_count<runtime_event>(count));
    for (uint32_t i = 0; i < count; ++i)
    {
        runtime_event value;
        unmarshall(reader, value, fmt);
        values.push_back(value);
    }
}

inline void marshall_failures(::dsn::binary_writer &writer,
                              const std::vector<failure_record> &values,
                              ::dsn_msg_serialize_format fmt)
{
    writer.write(static_cast<uint32_t>(values.size()));
    for (const failure_record &value : values)
    {
        marshall(writer, value, fmt);
    }
}

inline void unmarshall_failures(::dsn::binary_reader &reader,
                                std::vector<failure_record> &values,
                                ::dsn_msg_serialize_format fmt)
{
    uint32_t count = 0;
    reader.read(count);
    values.clear();
    values.reserve(rasn_bounded_reserve_count<failure_record>(count));
    for (uint32_t i = 0; i < count; ++i)
    {
        failure_record value;
        unmarshall(reader, value, fmt);
        values.push_back(value);
    }
}

inline void marshall(::dsn::binary_writer &writer, const observability_response &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.ok);
    writer.write(value.error);
    marshall_runtime_events(writer, value.events, fmt);
    marshall_failures(writer, value.failures, fmt);
    writer.write(value.last_sequence);
    writer.write(value.truncated);
}

inline void unmarshall(::dsn::binary_reader &reader, observability_response &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.ok);
    reader.read(value.error);
    unmarshall_runtime_events(reader, value.events, fmt);
    unmarshall_failures(reader, value.failures, fmt);
    reader.read(value.last_sequence);
    reader.read(value.truncated);
}

class rasn_observability_rpc_service : public ::dsn::serverlet<rasn_observability_rpc_service>
{
public:
    rasn_observability_rpc_service() : ::dsn::serverlet<rasn_observability_rpc_service>("rasn.observability") {}
    void open_service();
    void close_service();

protected:
    void on_query(const observability_query_request &request, ::dsn::rpc_replier<observability_response> &reply);
    void on_failures(const observability_query_request &request, ::dsn::rpc_replier<observability_response> &reply);
    void on_load_replay(const replay_load_request &request, ::dsn::rpc_replier<observability_response> &reply);
    void on_snapshot(const std::string &request, ::dsn::rpc_replier<observability_response> &reply);
};

class rasn_observability_client : public virtual ::dsn::clientlet
{
public:
    explicit rasn_observability_client(::dsn::rpc_address server) : _server(server) {}

    std::pair<::dsn::error_code, observability_response>
    query_sync(const observability_query_request &request,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
               int thread_hash = 0,
               uint64_t partition_hash = 0);

    std::pair<::dsn::error_code, observability_response>
    failures_sync(const observability_query_request &request,
                  std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                  int thread_hash = 0,
                  uint64_t partition_hash = 0);

    std::pair<::dsn::error_code, observability_response>
    load_replay_sync(const replay_load_request &request,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                     int thread_hash = 0,
                     uint64_t partition_hash = 0);

    std::pair<::dsn::error_code, observability_response>
    snapshot_sync(const std::string &request,
                  std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                  int thread_hash = 0,
                  uint64_t partition_hash = 0);

private:
    ::dsn::rpc_address _server;
};

class rasn_observability_app : public ::dsn::service_app
{
public:
    explicit rasn_observability_app(::dsn_gpid gpid) : ::dsn::service_app(gpid) {}
    virtual ::dsn::error_code start(int argc, char **argv);
    virtual ::dsn::error_code stop(bool cleanup = false);

private:
    rasn_observability_rpc_service _rpc;
};

} // namespace rasn
} // namespace dsn
