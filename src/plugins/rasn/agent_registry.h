#pragma once

#include <rasn/agent_types.h>
#include <rasn/rasn.code.definition.h>

#include <dsn/service_api_cpp.h>
#include <dsn/cpp/zlocks.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace dsn {
namespace rasn {

class agent_registry
{
public:
    bool register_agent(const agent_descriptor &descriptor, std::string *error);
    bool register_agent(const agent_descriptor &descriptor, std::string *error, bool lease_tracked);
    bool unregister_agent(const std::string &agent_id);

    // Convenience overloads default to healthy_only=true so capability routing
    // never selects unhealthy or lease-expired agents. Pass the explicit
    // healthy_only=false overload for diagnostics that need the full roster.
    std::vector<agent_descriptor> list_agents() const;
    std::vector<agent_descriptor> list_agents(bool healthy_only) const;
    std::vector<agent_descriptor> query_by_capability(const std::string &capability) const;
    std::vector<agent_descriptor> query_by_capability(const std::string &capability, bool healthy_only) const;
    bool find_agent(const std::string &agent_id, agent_descriptor *descriptor) const;
    bool heartbeat(const agent_descriptor &descriptor, std::string *error);
    size_t expire_leases(uint64_t now_ms, uint64_t lease_ms);
    std::string describe() const;

private:
    struct registry_entry
    {
        agent_descriptor descriptor;
        uint64_t last_heartbeat_ms = 0;
        bool lease_tracked = false;
    };

    bool is_live_entry(const registry_entry &entry, uint64_t now_ms, uint64_t lease_ms) const;
    bool has_capability(const agent_descriptor &descriptor, const std::string &capability) const;

    mutable ::dsn::service::zlock _lock;
    std::map<std::string, registry_entry> _agents;
};
struct registry_query_request
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    std::string capability;
    bool healthy_only = true;
};

struct registry_query_response
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    bool ok = true;
    std::string error;
    std::vector<agent_descriptor> agents;
};

inline void marshall_agent_descriptors(::dsn::binary_writer &writer,
                                       const std::vector<agent_descriptor> &values,
                                       ::dsn_msg_serialize_format fmt)
{
    writer.write(static_cast<uint32_t>(values.size()));
    for (const agent_descriptor &value : values)
    {
        marshall(writer, value, fmt);
    }
}

inline void unmarshall_agent_descriptors(::dsn::binary_reader &reader,
                                         std::vector<agent_descriptor> &values,
                                         ::dsn_msg_serialize_format fmt)
{
    uint32_t count = 0;
    reader.read(count);
    values.clear();
    values.reserve(rasn_bounded_reserve_count<agent_descriptor>(count));
    for (uint32_t i = 0; i < count; ++i)
    {
        agent_descriptor value;
        unmarshall(reader, value, fmt);
        values.push_back(value);
    }
}

inline void marshall(::dsn::binary_writer &writer, const registry_query_request &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.capability);
    writer.write(value.healthy_only);
}

inline void unmarshall(::dsn::binary_reader &reader, registry_query_request &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.capability);
    reader.read(value.healthy_only);
}

inline void marshall(::dsn::binary_writer &writer, const registry_query_response &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.ok);
    writer.write(value.error);
    marshall_agent_descriptors(writer, value.agents, fmt);
}

inline void unmarshall(::dsn::binary_reader &reader, registry_query_response &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.ok);
    reader.read(value.error);
    unmarshall_agent_descriptors(reader, value.agents, fmt);
}

agent_registry &global_agent_registry();
void load_static_agents_from_config_once();

class rasn_registry_rpc_service : public ::dsn::serverlet<rasn_registry_rpc_service>
{
public:
    rasn_registry_rpc_service() : ::dsn::serverlet<rasn_registry_rpc_service>("rasn.registry") {}
    void open_service();
    void close_service();

protected:
    void on_register(const agent_descriptor &request, ::dsn::rpc_replier<agent_response> &reply);
    void on_unregister(const std::string &agent_id, ::dsn::rpc_replier<agent_response> &reply);
    void on_query(const registry_query_request &request, ::dsn::rpc_replier<registry_query_response> &reply);
    void on_list(const std::string &request, ::dsn::rpc_replier<registry_query_response> &reply);
    void on_heartbeat(const agent_descriptor &request, ::dsn::rpc_replier<agent_response> &reply);
};

class rasn_registry_client : public virtual ::dsn::clientlet
{
public:
    explicit rasn_registry_client(::dsn::rpc_address server) : _server(server) {}

    std::pair< ::dsn::error_code, agent_response>
    register_sync(const agent_descriptor &request,
                  std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                  int thread_hash = 0,
                  uint64_t partition_hash = 0);

    std::pair< ::dsn::error_code, agent_response>
    unregister_sync(const std::string &agent_id,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                    int thread_hash = 0,
                    uint64_t partition_hash = 0);

    std::pair< ::dsn::error_code, registry_query_response>
    query_sync(const registry_query_request &request,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
               int thread_hash = 0,
               uint64_t partition_hash = 0);

    std::pair< ::dsn::error_code, registry_query_response>
    list_sync(const std::string &request,
              std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
              int thread_hash = 0,
              uint64_t partition_hash = 0);

    std::pair< ::dsn::error_code, agent_response>
    heartbeat_sync(const agent_descriptor &request,
                   std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                   int thread_hash = 0,
                   uint64_t partition_hash = 0);

private:
    ::dsn::rpc_address _server;
};

class rasn_registry_app : public ::dsn::service_app
{
public:
    explicit rasn_registry_app(::dsn_gpid gpid) : ::dsn::service_app(gpid) {}
    virtual ::dsn::error_code start(int argc, char **argv);
    virtual ::dsn::error_code stop(bool cleanup = false);

private:
    void start_lease_sweep_timer();
    void cancel_lease_sweep_timer();
    void sweep_leases();

    rasn_registry_rpc_service _rpc;
    ::dsn::task_ptr _lease_sweep_timer;
};

} // namespace rasn
} // namespace dsn
