#pragma once

#include <rasn/endpoint_binding.h>
#include <rasn/agent_types.h>
#include <rasn/rasn.code.definition.h>
#include <rasn/state_service.h>
#include <rasn/workflow.h>

#include <dsn/cpp/zlocks.h>
#include <dsn/service_api_cpp.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace dsn {
namespace rasn {

struct workflow_source
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    std::string workflow_id;
    std::string source_name;
    std::string source_text;
};

struct workflow_start_request
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    std::string run_id;
    workflow_source source;
    bool resume = false;
};

struct workflow_run_query
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    std::string run_id;
};

struct workflow_run_record
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    std::string run_id;
    std::string workflow_id;
    std::string source_name;
    std::string status;
    std::string plan;
    std::string result_text;
    std::string error;
    std::string state_key;
    std::string execution_owner;
    std::string lease_key;
    uint64_t sequence = 0;
    uint64_t lease_sequence = 0;
    uint64_t lease_expires_ms = 0;
};

struct workflow_response
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    bool ok = true;
    std::string error;
    workflow_run_record run;
};

typedef state_response (*workflow_state_getter)(const state_key_request &request);
typedef state_response (*workflow_state_queryer)(const state_query_request &request);
typedef state_response (*workflow_state_writer)(const state_put_request &request);
void set_workflow_state_readers(workflow_state_getter getter, workflow_state_queryer queryer);
void reset_workflow_state_readers();
void set_workflow_state_writer(workflow_state_writer writer);
void reset_workflow_state_writer();

inline void marshall(::dsn::binary_writer &writer, const workflow_source &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.workflow_id);
    writer.write(value.source_name);
    writer.write(value.source_text);
}

inline void unmarshall(::dsn::binary_reader &reader, workflow_source &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.workflow_id);
    reader.read(value.source_name);
    reader.read(value.source_text);
}

inline void marshall(::dsn::binary_writer &writer, const workflow_start_request &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.run_id);
    marshall(writer, value.source, fmt);
    writer.write(value.resume);
}

inline void unmarshall(::dsn::binary_reader &reader, workflow_start_request &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.run_id);
    unmarshall(reader, value.source, fmt);
    reader.read(value.resume);
}

inline void marshall(::dsn::binary_writer &writer, const workflow_run_query &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.run_id);
}

inline void unmarshall(::dsn::binary_reader &reader, workflow_run_query &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.run_id);
}

inline void marshall(::dsn::binary_writer &writer, const workflow_run_record &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.run_id);
    writer.write(value.workflow_id);
    writer.write(value.source_name);
    writer.write(value.status);
    writer.write(value.plan);
    writer.write(value.result_text);
    writer.write(value.error);
    writer.write(value.state_key);
    writer.write(value.execution_owner);
    writer.write(value.lease_key);
    writer.write(value.sequence);
    writer.write(value.lease_sequence);
    writer.write(value.lease_expires_ms);
}

inline void unmarshall(::dsn::binary_reader &reader, workflow_run_record &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.run_id);
    reader.read(value.workflow_id);
    reader.read(value.source_name);
    reader.read(value.status);
    reader.read(value.plan);
    reader.read(value.result_text);
    reader.read(value.error);
    reader.read(value.state_key);
    reader.read(value.execution_owner);
    reader.read(value.lease_key);
    reader.read(value.sequence);
    reader.read(value.lease_sequence);
    reader.read(value.lease_expires_ms);
}

inline void marshall(::dsn::binary_writer &writer, const workflow_response &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.ok);
    writer.write(value.error);
    marshall(writer, value.run, fmt);
}

inline void unmarshall(::dsn::binary_reader &reader, workflow_response &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.ok);
    reader.read(value.error);
    unmarshall(reader, value.run, fmt);
}

class workflow_store
{
public:
    workflow_response validate(const workflow_source &source);
    workflow_response compile(const workflow_source &source);
    workflow_response start(const workflow_start_request &request);
    workflow_response query(const workflow_run_query &request);
    workflow_response cancel(const workflow_run_query &request);
    workflow_response recover_from_state();

private:
    workflow_response error_response(const std::string &error) const;
    bool parse_source(const workflow_source &source, workflow_graph *graph, workflow_response *response) const;
    workflow_run_record make_record(const workflow_start_request &request, const std::string &status);
    workflow_response store_record(const workflow_run_record &record);
    workflow_response store_record_if_current(const workflow_run_record &record, uint64_t expected_sequence);
    bool recover_run_from_state(const std::string &run_id, workflow_run_record *record, std::string *error);
    workflow_response acquire_execution_lease(const std::string &run_id,
                                              const std::string &workflow_id,
                                              workflow_run_record *record) const;
    bool owns_execution_lease(const workflow_run_record &record, std::string *error) const;
    void release_execution_lease(const workflow_run_record &record, const std::string &status) const;
    bool recover_resume_state_from_state(const std::string &run_id,
                                         const std::string &workflow_id,
                                         workflow_graph::workflow_resume_state *resume_state,
                                         std::string *error) const;
    bool is_cancelled(const std::string &run_id, workflow_run_record *record) const;
    state_response persist_to_state(const workflow_run_record &record,
                                    bool check_sequence = false,
                                    uint64_t expected_sequence = 0) const;
    void persist_node_status(const std::string &run_id,
                             const std::string &workflow_id,
                             const workflow_node_status &status) const;

    mutable ::dsn::service::zlock _lock;
    std::map<std::string, workflow_run_record> _runs;
    uint64_t _last_sequence = 0;
};

workflow_store &global_workflow_store();

class rasn_workflow_rpc_service : public ::dsn::serverlet<rasn_workflow_rpc_service>
{
public:
    rasn_workflow_rpc_service() : ::dsn::serverlet<rasn_workflow_rpc_service>("rasn.workflow") {}
    void open_service();
    void close_service();

protected:
    void on_validate(const workflow_source &request, ::dsn::rpc_replier<workflow_response> &reply);
    void on_compile(const workflow_source &request, ::dsn::rpc_replier<workflow_response> &reply);
    void on_start(const workflow_start_request &request, ::dsn::rpc_replier<workflow_response> &reply);
    void on_query(const workflow_run_query &request, ::dsn::rpc_replier<workflow_response> &reply);
    void on_cancel(const workflow_run_query &request, ::dsn::rpc_replier<workflow_response> &reply);
};

class rasn_workflow_client : public virtual ::dsn::clientlet
{
public:
    explicit rasn_workflow_client(::dsn::rpc_address server) : _server(server) {}

    std::pair< ::dsn::error_code, workflow_response>
    validate_sync(const workflow_source &request,
                  std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                  int thread_hash = 0,
                  uint64_t partition_hash = 0);

    std::pair< ::dsn::error_code, workflow_response>
    compile_sync(const workflow_source &request,
                 std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                 int thread_hash = 0,
                 uint64_t partition_hash = 0);

    std::pair< ::dsn::error_code, workflow_response>
    start_sync(const workflow_start_request &request,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
               int thread_hash = 0,
               uint64_t partition_hash = 0);

    std::pair< ::dsn::error_code, workflow_response>
    query_sync(const workflow_run_query &request,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
               int thread_hash = 0,
               uint64_t partition_hash = 0);

    std::pair< ::dsn::error_code, workflow_response>
    cancel_sync(const workflow_run_query &request,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                int thread_hash = 0,
                uint64_t partition_hash = 0);

private:
    ::dsn::rpc_address _server;
};

class rasn_workflow_app : public ::dsn::service_app
{
public:
    explicit rasn_workflow_app(::dsn_gpid gpid) : ::dsn::service_app(gpid) {}
    virtual ::dsn::error_code start(int argc, char **argv);
    virtual ::dsn::error_code stop(bool cleanup = false);

private:
    rasn_workflow_rpc_service _rpc;
    ::dsn::task_ptr _recovery_task;
    rasn_core_service_registration _registration;
};

} // namespace rasn
} // namespace dsn
