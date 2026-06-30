#pragma once

#include "agent_types.h"
#include "rasn.code.definition.h"

#include <dsn/cpp/zlocks.h>
#include <dsn/service_api_cpp.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace dsn {
namespace rasn {

struct state_record
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    std::string key;
    std::string kind;
    std::string scope;
    std::string value;
    uint64_t sequence = 0;
};

struct state_put_request
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    state_record record;
    bool create_only = false;
    bool check_sequence = false;
    uint64_t expected_sequence = 0;
};

struct state_key_request
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    std::string key;
};

struct state_query_request
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    std::string key_prefix;
};

struct state_checkpoint_request
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    std::string path;
};

struct state_response
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    bool ok = true;
    std::string error;
    state_record record;
    std::vector<state_record> records;
    uint64_t last_sequence = 0;
};

inline void marshall(::dsn::binary_writer &writer, const state_record &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.key);
    writer.write(value.kind);
    writer.write(value.scope);
    writer.write(value.value);
    writer.write(value.sequence);
}

inline void unmarshall(::dsn::binary_reader &reader, state_record &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.key);
    reader.read(value.kind);
    reader.read(value.scope);
    reader.read(value.value);
    reader.read(value.sequence);
}

inline void marshall(::dsn::binary_writer &writer, const state_put_request &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    marshall(writer, value.record, fmt);
    writer.write(value.create_only);
    writer.write(value.check_sequence);
    writer.write(value.expected_sequence);
}

inline void unmarshall(::dsn::binary_reader &reader, state_put_request &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    unmarshall(reader, value.record, fmt);
    reader.read(value.create_only);
    reader.read(value.check_sequence);
    reader.read(value.expected_sequence);
}

inline void marshall_state_records(::dsn::binary_writer &writer,
                                   const std::vector<state_record> &values,
                                   ::dsn_msg_serialize_format fmt)
{
    writer.write(static_cast<uint32_t>(values.size()));
    for (const state_record &value : values)
    {
        marshall(writer, value, fmt);
    }
}

inline void unmarshall_state_records(::dsn::binary_reader &reader,
                                     std::vector<state_record> &values,
                                     ::dsn_msg_serialize_format fmt)
{
    uint32_t count = 0;
    reader.read(count);
    values.clear();
    values.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        state_record value;
        unmarshall(reader, value, fmt);
        values.push_back(value);
    }
}

inline void marshall(::dsn::binary_writer &writer, const state_key_request &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.key);
}

inline void unmarshall(::dsn::binary_reader &reader, state_key_request &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.key);
}

inline void marshall(::dsn::binary_writer &writer, const state_query_request &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.key_prefix);
}

inline void unmarshall(::dsn::binary_reader &reader, state_query_request &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.key_prefix);
}

inline void marshall(::dsn::binary_writer &writer, const state_checkpoint_request &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.path);
}

inline void unmarshall(::dsn::binary_reader &reader, state_checkpoint_request &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.path);
}

inline void marshall(::dsn::binary_writer &writer, const state_response &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.ok);
    writer.write(value.error);
    marshall(writer, value.record, fmt);
    marshall_state_records(writer, value.records, fmt);
    writer.write(value.last_sequence);
}

inline void unmarshall(::dsn::binary_reader &reader, state_response &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.ok);
    reader.read(value.error);
    unmarshall(reader, value.record, fmt);
    unmarshall_state_records(reader, value.records, fmt);
    reader.read(value.last_sequence);
}

class state_store
{
public:
    state_response put(const state_record &record);
    state_response put(const state_put_request &request);
    state_response get(const state_key_request &request) const;
    state_response query(const state_query_request &request) const;
    state_response checkpoint(const state_checkpoint_request &request) const;
    state_response recover(const state_checkpoint_request &request);
    bool has_recovery_state(const state_checkpoint_request &request) const;

private:
    state_response error_response(const std::string &error) const;
    std::string default_checkpoint_path() const;
    std::string journal_path_for_checkpoint(const std::string &checkpoint_path) const;
    bool append_journal_record(const state_record &record, std::string *error) const;

    mutable ::dsn::service::zlock _lock;
    std::map<std::string, state_record> _records;
    uint64_t _last_sequence = 0;
};

state_store &global_state_store();

class rasn_state_rpc_service : public ::dsn::serverlet<rasn_state_rpc_service>
{
public:
    rasn_state_rpc_service() : ::dsn::serverlet<rasn_state_rpc_service>("rasn.state") {}
    void open_service();
    void close_service();

protected:
    void on_put(const state_record &request, ::dsn::rpc_replier<state_response> &reply);
    void on_put_conditional(const state_put_request &request, ::dsn::rpc_replier<state_response> &reply);
    void on_get(const state_key_request &request, ::dsn::rpc_replier<state_response> &reply);
    void on_query(const state_query_request &request, ::dsn::rpc_replier<state_response> &reply);
    void on_checkpoint(const state_checkpoint_request &request, ::dsn::rpc_replier<state_response> &reply);
    void on_recover(const state_checkpoint_request &request, ::dsn::rpc_replier<state_response> &reply);
};

class rasn_state_client : public virtual ::dsn::clientlet
{
public:
    explicit rasn_state_client(::dsn::rpc_address server) : _server(server) {}

    std::pair<::dsn::error_code, state_response>
    put_sync(const state_record &request,
             std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
             int thread_hash = 0,
             uint64_t partition_hash = 0);

    std::pair<::dsn::error_code, state_response>
    put_conditional_sync(const state_put_request &request,
                         std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                         int thread_hash = 0,
                         uint64_t partition_hash = 0);

    std::pair<::dsn::error_code, state_response>
    get_sync(const state_key_request &request,
             std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
             int thread_hash = 0,
             uint64_t partition_hash = 0);

    std::pair<::dsn::error_code, state_response>
    query_sync(const state_query_request &request,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
               int thread_hash = 0,
               uint64_t partition_hash = 0);

    std::pair<::dsn::error_code, state_response>
    checkpoint_sync(const state_checkpoint_request &request,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                    int thread_hash = 0,
                    uint64_t partition_hash = 0);

    std::pair<::dsn::error_code, state_response>
    recover_sync(const state_checkpoint_request &request,
                 std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                 int thread_hash = 0,
                 uint64_t partition_hash = 0);

private:
    ::dsn::rpc_address _server;
};

class rasn_state_app : public ::dsn::service_app
{
public:
    explicit rasn_state_app(::dsn_gpid gpid) : ::dsn::service_app(gpid) {}
    virtual ::dsn::error_code start(int argc, char **argv);
    virtual ::dsn::error_code stop(bool cleanup = false);

private:
    rasn_state_rpc_service _rpc;
};

} // namespace rasn
} // namespace dsn
