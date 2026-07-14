#pragma once

#include <rasn/agent_types.h>
#include <rasn/coordination_service.h>
#include <rasn/rasn.code.definition.h>

#include <dsn/service_api_cpp.h>
#include <dsn/cpp/zlocks.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dsn {
namespace rasn {

const uint32_t RASN_REGISTRY_STATE_SCHEMA_VERSION = 1;

// Durable record stored by the HA registry backend. Each registry-primary epoch
// writes a distinct fence child, and readers use only the globally committed
// epoch. A delayed writer from an expired epoch therefore cannot expose a stale
// registration, heartbeat, expiry, or tombstone.
struct registry_state_record
{
    uint32_t schema_version = RASN_REGISTRY_STATE_SCHEMA_VERSION;
    uint64_t writer_fence = 0;
    bool tombstone = false;
    agent_descriptor descriptor;
    uint64_t last_heartbeat_ms = 0;
    bool lease_tracked = false;
};

bool encode_registry_state_record(const registry_state_record &record,
                                  std::string *encoded,
                                  std::string *error);
bool decode_registry_state_record(const std::string &encoded,
                                  registry_state_record *record,
                                  std::string *error);

class agent_registry
{
public:
    agent_registry();

    // Switch this registry instance from its process-local map to the shared
    // coordination store. Reads are available on every registry frontend;
    // mutations require activate_shared_writer() to install a committed epoch.
    bool configure_shared_backend(const std::shared_ptr<rasn_coordination_context> &coordination,
                                  const std::string &state_prefix,
                                  const std::string &leader_resource,
                                  const std::string &leader_owner,
                                  std::string *error);
    bool activate_shared_writer(uint64_t fencing_token,
                                const std::vector<agent_descriptor> &static_descriptors,
                                const std::shared_ptr<std::atomic<bool>> &leadership_lost,
                                std::string *error);
    void clear_shared_writer();
    bool shared_backend_enabled() const;
    bool shared_writer_active() const;
    uint64_t shared_writer_fence() const;
    bool prune_shared_history(size_t retained_epochs,
                              size_t *pruned_epochs,
                              std::string *error);

    bool register_agent(const agent_descriptor &descriptor, std::string *error);
    bool register_agent(const agent_descriptor &descriptor, std::string *error, bool lease_tracked);
    bool unregister_agent(const std::string &agent_id);
    bool unregister_agent(const std::string &agent_id, std::string *error);

    // Convenience overloads default to healthy_only=true so capability routing
    // never selects unhealthy or lease-expired agents. Pass the explicit
    // healthy_only=false overload for diagnostics that need the full roster.
    std::vector<agent_descriptor> list_agents() const;
    std::vector<agent_descriptor> list_agents(bool healthy_only) const;
    std::vector<agent_descriptor> query_by_capability(const std::string &capability) const;
    std::vector<agent_descriptor> query_by_capability(const std::string &capability, bool healthy_only) const;
    bool find_agent(const std::string &agent_id, agent_descriptor *descriptor) const;
    bool list_agents(bool healthy_only,
                     std::vector<agent_descriptor> *agents,
                     std::string *error) const;
    bool query_by_capability(const std::string &capability,
                             bool healthy_only,
                             std::vector<agent_descriptor> *agents,
                             std::string *error) const;
    bool find_agent(const std::string &agent_id,
                    agent_descriptor *descriptor,
                    std::string *error) const;
    bool heartbeat(const agent_descriptor &descriptor, std::string *error);
    size_t expire_leases(uint64_t now_ms, uint64_t lease_ms);
    bool expire_leases(uint64_t now_ms,
                       uint64_t lease_ms,
                       size_t *expired,
                       std::string *error);
    bool replace_static_agents(const std::vector<agent_descriptor> &descriptors,
                               std::string *error);
    std::string describe() const;

private:
    struct registry_entry
    {
        agent_descriptor descriptor;
        uint64_t last_heartbeat_ms = 0;
        bool lease_tracked = false;
    };

    std::string shared_agent_path(const std::string &agent_id) const;
    bool read_shared_record(const std::string &agent_id,
                            registry_state_record *record,
                            bool *found,
                            std::string *error) const;
    bool read_all_shared_records(std::vector<registry_state_record> *records,
                                 std::string *error) const;
    bool read_all_shared_records(bool require_current_owner,
                                 std::vector<registry_state_record> *records,
                                 std::string *error) const;
    bool read_committed_epoch(bool require_committed_epoch,
                              uint64_t *fencing_token,
                              std::string *writer_owner,
                              bool *found,
                              std::string *error) const;
    bool verify_committed_epoch_owner(uint64_t fencing_token,
                                      const std::string &writer_owner,
                                      std::string *error) const;
    bool replace_static_agents_locked(
        const std::map<std::string, agent_descriptor> &desired,
        std::string *error);
    bool commit_shared_epoch(std::string *error);
    bool prune_shared_history_locked(size_t retained_epochs,
                                     size_t *pruned_epochs,
                                     std::string *error);
    bool write_shared_record(const registry_state_record &record, std::string *error);
    bool verify_shared_writer(std::string *error) const;
    bool is_live_entry(const registry_entry &entry, uint64_t now_ms, uint64_t lease_ms) const;
    bool is_live_record(const registry_state_record &record,
                        uint64_t now_ms,
                        uint64_t lease_ms) const;
    bool has_capability(const agent_descriptor &descriptor, const std::string &capability) const;

    mutable ::dsn::service::zlock _lock;
    std::map<std::string, registry_entry> _agents;
    std::atomic<bool> _shared_backend_configured;
    std::shared_ptr<rasn_coordination_context> _coordination;
    std::string _shared_state_prefix;
    std::string _shared_agents_prefix;
    std::string _shared_epochs_prefix;
    std::string _leader_resource;
    std::string _leader_owner;
    std::shared_ptr<std::atomic<bool>> _leadership_lost;
    bool _writer_active;
    bool _writer_promoting;
    uint64_t _writer_fence;
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
std::vector<agent_descriptor> configured_static_agents();

// registry_uri remains authoritative. Otherwise registry_addresses creates an
// rDSN group address; the legacy registry_host/registry_port pair is fallback.
bool parse_rasn_registry_addresses(const std::string &value,
                                   std::vector< ::dsn::rpc_address> *addresses,
                                   std::string *error);
bool validate_rasn_registry_ha_pools(const std::string &pools,
                                     bool dlock_partitioned,
                                     std::string *error);
::dsn::rpc_address configured_rasn_registry_address();
bool registry_response_is_agent_not_found(const agent_response &response);

class rasn_registry_rpc_service : public ::dsn::serverlet<rasn_registry_rpc_service>
{
public:
    explicit rasn_registry_rpc_service(agent_registry *registry = nullptr);
    void open_service();
    void close_service();

protected:
    void on_register(const agent_descriptor &request, ::dsn::rpc_replier<agent_response> &reply);
    void on_unregister(const std::string &agent_id, ::dsn::rpc_replier<agent_response> &reply);
    void on_query(const registry_query_request &request, ::dsn::rpc_replier<registry_query_response> &reply);
    void on_list(const std::string &request, ::dsn::rpc_replier<registry_query_response> &reply);
    void on_heartbeat(const agent_descriptor &request, ::dsn::rpc_replier<agent_response> &reply);

private:
    agent_registry *_registry;
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
    explicit rasn_registry_app(::dsn_gpid gpid);
    virtual ::dsn::error_code start(int argc, char **argv);
    virtual ::dsn::error_code stop(bool cleanup = false);

private:
    bool configure_ha_registry(std::string *error);
    ::dsn::error_code try_acquire_leadership();
    void lose_leadership();
    bool start_maintenance_timer();
    void cancel_maintenance_timer();
    void maintain_registry();
    void prune_history();
    void sweep_leases();

    agent_registry _registry;
    rasn_registry_rpc_service _rpc;
    std::shared_ptr<rasn_coordination_context> _coordination;
    std::shared_ptr<std::atomic<bool>> _leadership_lost;
    std::string _leader_resource;
    std::string _leader_owner;
    uint64_t _leader_fence;
    bool _leader_active;
    bool _shared_enabled;
    uint64_t _last_sweep_ms;
    uint64_t _lease_sweep_not_before_ms;
    uint64_t _last_history_prune_ms;
    ::dsn::task_ptr _maintenance_timer;
};

} // namespace rasn
} // namespace dsn
