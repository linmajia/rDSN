#pragma once

#include <rasn/agent_types.h>
#include <rasn/rasn.code.definition.h>

#include <dsn/cpp/replicated_service_app.h>
#include <dsn/cpp/zlocks.h>
#include <dsn/service_api_cpp.h>

#include <atomic>
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
    values.reserve(rasn_bounded_reserve_count<state_record>(count));
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
    explicit state_store(bool journal_enabled = true) : _journal_enabled(journal_enabled) {}

    state_response put(const state_record &record);
    state_response put(const state_put_request &request);
    state_response get(const state_key_request &request) const;
    state_response query(const state_query_request &request) const;
    state_response checkpoint(const state_checkpoint_request &request) const;
    state_response copy_checkpoint(const state_checkpoint_request &request,
                                   const std::string &durable_path);
    state_response replace_from_checkpoint(const state_checkpoint_request &request,
                                           const std::string &durable_path = "");
    state_response recover(const state_checkpoint_request &request);
    bool has_recovery_state(const state_checkpoint_request &request) const;

private:
    state_response import_checkpoint(const state_checkpoint_request &request,
                                     const std::string &durable_path,
                                     bool replace);
    state_response error_response(const std::string &error) const;
    std::string default_checkpoint_path() const;
    std::string default_journal_path() const;
    bool append_journal_record(const state_record &record, std::string *error) const;

    mutable ::dsn::service::zlock _lock;
    std::map<std::string, state_record> _records;
    uint64_t _last_sequence = 0;
    // Incremented on every successful put; lets checkpoint detect whether any
    // write landed since it snapshotted so it can safely compact the journal.
    uint64_t _write_epoch = 0;
    // Standalone stores journal before exposing writes. Type-1 replicated stores
    // disable this because rDSN's quorum mutation log is their durability source.
    const bool _journal_enabled;
};

std::string configured_state_checkpoint_path();
std::string configured_state_journal_path();
state_checkpoint_request configured_state_recovery_request();
bool configured_state_recovery_available(const state_checkpoint_request &request);

state_store &global_state_store();

class rasn_state_rpc_service : public ::dsn::serverlet<rasn_state_rpc_service>
{
public:
    explicit rasn_state_rpc_service(state_store *store = nullptr, bool replicated = false)
        : ::dsn::serverlet<rasn_state_rpc_service>("rasn.state"),
          _store(store == nullptr ? &global_state_store() : store),
          _replicated(replicated)
    {
    }
    void open_service(::dsn_gpid gpid = ::dsn_gpid());
    void close_service(::dsn_gpid gpid = ::dsn_gpid());

protected:
    void on_put(const state_record &request, ::dsn::rpc_replier<state_response> &reply);
    void on_put_conditional(const state_put_request &request, ::dsn::rpc_replier<state_response> &reply);
    void on_get(const state_key_request &request, ::dsn::rpc_replier<state_response> &reply);
    void on_query(const state_query_request &request, ::dsn::rpc_replier<state_response> &reply);
    void on_checkpoint(const state_checkpoint_request &request, ::dsn::rpc_replier<state_response> &reply);
    void on_recover(const state_checkpoint_request &request, ::dsn::rpc_replier<state_response> &reply);

private:
    state_store *_store;
    bool _replicated;
};

class rasn_state_client : public virtual ::dsn::clientlet
{
public:
    explicit rasn_state_client(::dsn::rpc_address server) : _server(server) {}

    std::pair< ::dsn::error_code, state_response>
    put_sync(const state_record &request,
             std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
             int thread_hash = 0,
             uint64_t partition_hash = 0);

    std::pair< ::dsn::error_code, state_response>
    put_conditional_sync(const state_put_request &request,
                         std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                         int thread_hash = 0,
                         uint64_t partition_hash = 0);

    std::pair< ::dsn::error_code, state_response>
    get_sync(const state_key_request &request,
             std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
             int thread_hash = 0,
             uint64_t partition_hash = 0);

    std::pair< ::dsn::error_code, state_response>
    query_sync(const state_query_request &request,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
               int thread_hash = 0,
               uint64_t partition_hash = 0);

    std::pair< ::dsn::error_code, state_response>
    checkpoint_sync(const state_checkpoint_request &request,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                    int thread_hash = 0,
                    uint64_t partition_hash = 0);

    std::pair< ::dsn::error_code, state_response>
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

class rasn_replicated_state_app : public ::dsn::replicated_service_app_type_1
{
public:
    explicit rasn_replicated_state_app(::dsn_gpid gpid);

    ::dsn::error_code start(int argc, char **argv) override;
    ::dsn::error_code stop(bool cleanup = false) override;
    ::dsn::error_code sync_checkpoint(int64_t last_commit) override;
    int64_t get_last_checkpoint_decree() override;
    ::dsn::error_code get_checkpoint(int64_t learn_start,
                                     int64_t local_commit,
                                     void *learn_request,
                                     int learn_request_size,
                                     app_learn_state &state) override;
    ::dsn::error_code apply_checkpoint(::dsn_chkpt_apply_mode mode,
                                       int64_t local_commit,
                                       const ::dsn_app_learn_state &state) override;

private:
    ::dsn::error_code recover_latest_checkpoint();
    std::string checkpoint_path(int64_t decree) const;

    ::dsn::service::zlock _checkpoint_lock;
    state_store _store;
    rasn_state_rpc_service _rpc;
    std::string _data_dir;
    std::atomic<int64_t> _last_durable_decree;
};

void register_rasn_state_apps();

} // namespace rasn
} // namespace dsn
