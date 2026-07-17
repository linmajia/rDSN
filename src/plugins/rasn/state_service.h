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

constexpr uint64_t k_default_quarantine_probe_interval_ms = 1000;

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

struct state_delete_prefix_request
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    std::string key_prefix;
    uint64_t max_sequence = 0;
};

struct state_sequence_barrier_request
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    uint64_t minimum_sequence = 0;
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

struct state_checkpoint_result
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    state_response response;
    std::string checkpoint_path;
    bool journal_compacted = false;
    bool details_available = true;
};

struct state_delete_prefix_result
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    state_response response;
    uint64_t deleted_records = 0;
    bool details_available = true;
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

inline void marshall(::dsn::binary_writer &writer,
                     const state_delete_prefix_request &value,
                     ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.key_prefix);
    writer.write(value.max_sequence);
}

inline void unmarshall(::dsn::binary_reader &reader,
                       state_delete_prefix_request &value,
                       ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.key_prefix);
    reader.read(value.max_sequence);
}

inline void marshall(::dsn::binary_writer &writer,
                     const state_sequence_barrier_request &value,
                     ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.minimum_sequence);
}

inline void unmarshall(::dsn::binary_reader &reader,
                       state_sequence_barrier_request &value,
                       ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.minimum_sequence);
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

inline void marshall(::dsn::binary_writer &writer,
                     const state_checkpoint_result &value,
                     ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    marshall(writer, value.response, fmt);
    writer.write(value.checkpoint_path);
    writer.write(value.journal_compacted);
    writer.write(value.details_available);
}

inline void unmarshall(::dsn::binary_reader &reader,
                       state_checkpoint_result &value,
                       ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    unmarshall(reader, value.response, fmt);
    reader.read(value.checkpoint_path);
    reader.read(value.journal_compacted);
    reader.read(value.details_available);
}

inline void marshall(::dsn::binary_writer &writer,
                     const state_delete_prefix_result &value,
                     ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    marshall(writer, value.response, fmt);
    writer.write(value.deleted_records);
    writer.write(value.details_available);
}

inline void unmarshall(::dsn::binary_reader &reader,
                       state_delete_prefix_result &value,
                       ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    unmarshall(reader, value.response, fmt);
    reader.read(value.deleted_records);
    reader.read(value.details_available);
}

class state_store
{
public:
    // Journal-enabled callers must explicitly choose the read-side quarantine
    // probe interval; journal-free replicated/checkpoint stores pass zero.
    state_store(bool journal_enabled, uint64_t quarantine_probe_interval_ms);

    state_response put(const state_record &record);
    state_response put(const state_put_request &request);
    state_response get(const state_key_request &request) const;
    state_response query(const state_query_request &request) const;
    state_response delete_prefix(const state_delete_prefix_request &request);
    state_delete_prefix_result
    delete_prefix_detailed(const state_delete_prefix_request &request);
    state_response advance_sequence(const state_sequence_barrier_request &request);
    state_response checkpoint(const state_checkpoint_request &request);
    state_checkpoint_result
    checkpoint_detailed(const state_checkpoint_request &request);
    state_response copy_checkpoint(const state_checkpoint_request &request,
                                   const std::string &durable_path);
    state_response replace_from_checkpoint(const state_checkpoint_request &request,
                                           const std::string &durable_path = "");
    state_response recover(const state_checkpoint_request &request);
    bool has_recovery_state(const state_checkpoint_request &request) const;
    bool validate_storage_paths(std::string *error) const;

private:
    state_delete_prefix_result
    delete_prefix_impl(const state_delete_prefix_request &request,
                       bool include_deleted_records);
    state_response import_checkpoint(const state_checkpoint_request &request,
                                     const std::string &durable_path,
                                     bool replace);
    state_response error_response(const std::string &error) const;
    std::string default_checkpoint_path() const;
    std::string default_journal_path() const;
    std::string quarantine_error() const;
    bool journal_is_quarantined(bool force_refresh) const;
    bool validate_cached_storage_filesystem(std::string *error) const;
    bool append_journal_record(const state_record &record, std::string *error) const;
    bool append_journal_delete_prefix(const state_delete_prefix_request &request,
                                      uint64_t operation_sequence,
                                      std::string *error) const;
    bool append_journal_sequence_barrier(const state_sequence_barrier_request &request,
                                         std::string *error) const;

    // Checkpoint/import/recovery file lifecycles serialize independently from
    // mutations. Mutations may continue while a checkpoint snapshot is written;
    // the epoch guard then keeps the journal when the snapshot was superseded.
    ::dsn::service::zlock _checkpoint_lock;
    ::dsn::service::zlock _mutation_lock;
    // Protects only short in-memory map accesses and read queries. No disk or
    // replica-mirror I/O is performed while this lock is held.
    mutable ::dsn::service::zlock _lock;
    std::map<std::string, state_record> _records;
    uint64_t _last_sequence = 0;
    // Incremented on every successful mutation; lets checkpoint detect whether
    // any write landed since it snapshotted so it can safely compact the journal.
    uint64_t _write_epoch = 0;
    // Standalone stores journal before exposing writes. Type-1 replicated stores
    // disable this because rDSN's quorum mutation log is their durability source.
    const bool _journal_enabled;
    const uint64_t _quarantine_probe_interval_ms;
    // Quarantine is monotonic for a live store. Reads use a bounded disk-probe
    // interval; mutations and lifecycle operations force a refresh.
    mutable std::atomic<bool> _quarantine_seen{false};
    mutable std::atomic<uint64_t> _next_quarantine_probe_ms{0};
    mutable ::dsn::service::zlock _quarantine_probe_lock;
    mutable ::dsn::service::zlock _storage_filesystem_validation_lock;
    mutable bool _storage_filesystem_validation_checked = false;
    mutable bool _storage_filesystem_validation_ok = false;
    mutable std::string _storage_filesystem_validation_error;
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
    void on_delete_prefix(const state_delete_prefix_request &request,
                          ::dsn::rpc_replier<state_response> &reply);
    void on_delete_prefix_detailed(
        const state_delete_prefix_request &request,
        ::dsn::rpc_replier<state_delete_prefix_result> &reply);
    void on_advance_sequence(const state_sequence_barrier_request &request,
                             ::dsn::rpc_replier<state_response> &reply);
    void on_checkpoint(const state_checkpoint_request &request, ::dsn::rpc_replier<state_response> &reply);
    void on_checkpoint_detailed(
        const state_checkpoint_request &request,
        ::dsn::rpc_replier<state_checkpoint_result> &reply);
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
    delete_prefix_sync(const state_delete_prefix_request &request,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                       int thread_hash = 0,
                       uint64_t partition_hash = 0);

    std::pair< ::dsn::error_code, state_delete_prefix_result>
    delete_prefix_detailed_sync(
        const state_delete_prefix_request &request,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
        int thread_hash = 0,
        uint64_t partition_hash = 0);

    std::pair< ::dsn::error_code, state_response>
    advance_sequence_sync(const state_sequence_barrier_request &request,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                          int thread_hash = 0,
                          uint64_t partition_hash = 0);

    std::pair< ::dsn::error_code, state_response>
    checkpoint_sync(const state_checkpoint_request &request,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                    int thread_hash = 0,
                    uint64_t partition_hash = 0);

    std::pair< ::dsn::error_code, state_checkpoint_result>
    checkpoint_detailed_sync(
        const state_checkpoint_request &request,
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
    ::dsn::error_code async_checkpoint(int64_t last_commit) override;
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
