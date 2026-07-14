#include <rasn/agent_registry.h>

#include <rasn/rpc_resilience.h>

#include <dsn/c/api_layer1.h>
#include <dsn/cpp/utils.h>
#include <dsn/service_api_cpp.h>

#include <algorithm>
#include <exception>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <thread>

namespace dsn {
namespace rasn {

namespace {

const char *const k_registry_state_magic = "rasn.registry.state";
const char *const k_registry_epoch_magic = "rasn.registry.epoch";
const char *const k_registry_not_primary = "registry_not_primary";
const char *const k_registry_backend_unavailable = "registry_backend_unavailable";
const char *const k_registry_agent_not_found = "registry_agent_not_found";
const char *const k_registry_mutation_outcome_unknown =
    "registry_mutation_outcome_unknown";
const size_t k_max_registry_group_members = 64;

bool starts_with(const std::string &value, const char *prefix)
{
    return value.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
}

agent_response registry_response(const std::string &request_id, bool ok, const std::string &error)
{
    agent_response response;
    response.request_id = request_id;
    response.ok = ok;
    if (!ok)
    {
        const bool not_primary = starts_with(error, k_registry_not_primary);
        const bool backend_unavailable =
            starts_with(error, k_registry_backend_unavailable);
        const bool agent_not_found =
            starts_with(error, k_registry_agent_not_found);
        const bool outcome_unknown =
            starts_with(error, k_registry_mutation_outcome_unknown);
        response.error = make_agent_error("registry",
                                          not_primary
                                              ? k_registry_not_primary
                                              : (backend_unavailable
                                                     ? k_registry_backend_unavailable
                                                     : (agent_not_found
                                                            ? k_registry_agent_not_found
                                                            : (outcome_unknown
                                                                   ? k_registry_mutation_outcome_unknown
                                                                   : "registry_error"))),
                                          error,
                                          not_primary || backend_unavailable,
                                          "rasn.registry");
    }
    return response;
}

registry_query_response registry_error_response(const std::string &error)
{
    registry_query_response response;
    response.ok = false;
    response.error = error;
    return response;
}

bool is_healthy_descriptor(const agent_descriptor &descriptor)
{
    return descriptor.health.empty() || descriptor.health == "healthy";
}

uint64_t registry_lease_ms()
{
    return ::dsn_config_get_value_uint64(
        "rasn.registry", "lease_ms", 30000, "rASN dynamic registry lease in milliseconds; 0 disables TTL filtering");
}

uint64_t registry_lease_sweep_ms()
{
    return ::dsn_config_get_value_uint64(
        "rasn.registry", "sweep_interval_ms", 5000, "rASN registry lease sweep interval in milliseconds; 0 disables active sweeping");
}

bool registry_shared_state_enabled()
{
    return ::dsn_config_get_value_bool(
        "rasn.registry", "shared_state_enabled", false, "Use ZooKeeper-backed shared rASN registry state");
}

std::string registry_shared_state_prefix()
{
    return ::dsn_config_get_value_string(
        "rasn.registry", "shared_state_prefix", "registry/v1", "Relative coordination-state prefix for the HA registry");
}

std::string registry_leader_resource()
{
    return ::dsn_config_get_value_string(
        "rasn.registry", "leader_resource", "rasn.registry.primary", "Distributed ownership resource for registry mutations");
}

int registry_leader_acquire_timeout_ms()
{
    const uint64_t configured = ::dsn_config_get_value_uint64(
        "rasn.registry", "leader_acquire_timeout_ms", 1000, "HA registry primary-election attempt timeout");
    return static_cast<int>((std::min)(
        configured, static_cast<uint64_t>((std::numeric_limits<int>::max)())));
}

uint64_t registry_leader_retry_ms()
{
    return (std::max)(
        static_cast<uint64_t>(1),
        ::dsn_config_get_value_uint64(
            "rasn.registry", "leader_retry_interval_ms", 1000, "HA registry standby election retry interval"));
}

uint32_t registry_client_max_attempts()
{
    const uint64_t configured = ::dsn_config_get_value_uint64(
        "rasn.registry", "client_max_attempts", 3, "Bounded registry RPC attempts across frontend replicas");
    return static_cast<uint32_t>((std::max)(
        static_cast<uint64_t>(1),
        (std::min)(configured,
                   static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)()))));
}

uint64_t registry_client_backoff_ms()
{
    return ::dsn_config_get_value_uint64(
        "rasn.registry", "client_retry_backoff_ms", 50, "Linear registry RPC retry backoff");
}

void fail_stop_if_registry_release_is_uncertain(::dsn::error_code released,
                                                const char *context)
{
    if (released == ::dsn::ERR_OK)
    {
        return;
    }
    derror("HA rASN registry leadership release is ambiguous during %s (%s); "
           "fail-stopping so the shared ZooKeeper session cannot retain a ghost owner",
           context,
           released.to_string());
    ::dsn_exit(1);
}

std::vector<std::string> split_config_csv(const std::string &value)
{
    std::vector<std::string> result;
    ::dsn::utils::split_args(value.c_str(), result, ',');
    return result;
}

agent_capability make_static_capability(const std::string &value, const std::string &side_effect_class)
{
    agent_capability capability;
    capability.name = value;
    capability.input_type = "text";
    capability.output_type = "text";
    capability.side_effect_class = side_effect_class;
    return capability;
}

std::vector<std::string> config_sections()
{
    const char *sections[1024];
    int used_sections = static_cast<int>(sizeof(sections) / sizeof(sections[0]));
    const int total_sections = ::dsn_config_get_all_sections(sections, &used_sections);
    if (total_sections > used_sections)
    {
        dwarn("rasn registry config has %d sections, but only first %d are scanned",
              total_sections,
              used_sections);
    }

    std::vector<std::string> result;
    result.reserve(static_cast<size_t>((std::max)(used_sections, 0)));
    for (int i = 0; i < used_sections; ++i)
    {
        if (sections[i] != nullptr)
        {
            result.push_back(sections[i]);
        }
    }
    return result;
}

::dsn::service::zlock &static_registry_config_lock()
{
    static ::dsn::service::zlock lock;
    return lock;
}

bool &static_registry_config_loaded()
{
    static bool loaded = false;
    return loaded;
}

std::string hex_encode(const std::string &value)
{
    static const char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(value.size() * 2);
    for (unsigned char ch : value)
    {
        encoded.push_back(digits[ch >> 4]);
        encoded.push_back(digits[ch & 0x0f]);
    }
    return encoded;
}

bool normalize_relative_state_path(const std::string &value,
                                   std::string *normalized,
                                   std::string *error)
{
    if (normalized == nullptr)
    {
        if (error != nullptr)
        {
            *error = "registry state prefix output is null";
        }
        return false;
    }

    size_t begin = 0;
    while (begin < value.size() && value[begin] == '/')
    {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && value[end - 1] == '/')
    {
        --end;
    }
    *normalized = value.substr(begin, end - begin);
    if (normalized->empty() || normalized->find("//") != std::string::npos ||
        *normalized == ".." || normalized->find("../") == 0 ||
        normalized->find("/../") != std::string::npos ||
        (normalized->size() >= 3 &&
         normalized->compare(normalized->size() - 3, 3, "/..") == 0))
    {
        if (error != nullptr)
        {
            *error = "invalid registry shared_state_prefix: " + value;
        }
        return false;
    }
    return true;
}

bool parse_fence(const std::string &value, uint64_t *fence)
{
    if (value.empty() || fence == nullptr ||
        !::dsn::utils::lexical_cast_integer<uint64_t>(value, *fence))
    {
        return false;
    }
    return std::to_string(*fence) == value;
}

bool error_is_not_found(::dsn::error_code error)
{
    return error == ::dsn::ERR_OBJECT_NOT_FOUND;
}

void set_backend_error(const std::string &operation,
                       ::dsn::error_code code,
                       std::string *error)
{
    if (error != nullptr)
    {
        *error = std::string(k_registry_backend_unavailable) + ": " +
                 operation + " failed: " + code.to_string();
    }
}

struct registry_epoch_record
{
    uint32_t schema_version = RASN_REGISTRY_STATE_SCHEMA_VERSION;
    uint64_t writer_fence = 0;
    std::string writer_owner;
};

bool encode_registry_epoch_record(const registry_epoch_record &record,
                                  std::string *encoded,
                                  std::string *error)
{
    if (encoded == nullptr || record.writer_owner.empty())
    {
        if (error != nullptr)
        {
            *error = "invalid registry epoch record";
        }
        return false;
    }
    ::dsn::binary_writer writer;
    writer.write(std::string(k_registry_epoch_magic));
    writer.write(record.schema_version);
    writer.write(record.writer_fence);
    writer.write(record.writer_owner);
    const ::dsn::blob buffer = writer.get_buffer();
    encoded->assign(buffer.data(), buffer.length());
    return true;
}

bool decode_registry_epoch_record(const std::string &encoded,
                                  registry_epoch_record *record,
                                  std::string *error)
{
    if (record == nullptr ||
        encoded.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
    {
        if (error != nullptr)
        {
            *error = "invalid registry epoch record output/size";
        }
        return false;
    }
    try
    {
        ::dsn::binary_reader reader(
            ::dsn::blob(encoded.data(), 0, static_cast<unsigned int>(encoded.size())));
        std::string magic;
        registry_epoch_record decoded;
        reader.read(magic);
        reader.read(decoded.schema_version);
        reader.read(decoded.writer_fence);
        reader.read(decoded.writer_owner);
        if (magic != k_registry_epoch_magic ||
            decoded.schema_version != RASN_REGISTRY_STATE_SCHEMA_VERSION ||
            decoded.writer_owner.empty() || !reader.is_eof())
        {
            if (error != nullptr)
            {
                *error = "invalid or unsupported registry epoch record";
            }
            return false;
        }
        *record = decoded;
        return true;
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr)
        {
            *error = std::string("invalid registry epoch record: ") + ex.what();
        }
        return false;
    }
}

bool response_requires_failover(const agent_response &response)
{
    return !response.ok &&
           (response.error.code == k_registry_not_primary ||
            response.error.code == k_registry_backend_unavailable);
}

bool response_requires_rotation(const agent_response &response)
{
    return response_requires_failover(response) ||
           (!response.ok &&
            response.error.code == k_registry_mutation_outcome_unknown);
}

bool response_requires_failover(const registry_query_response &response)
{
    return !response.ok &&
           (starts_with(response.error, k_registry_not_primary) ||
            starts_with(response.error, k_registry_backend_unavailable));
}

bool response_requires_rotation(const registry_query_response &response)
{
    return response_requires_failover(response);
}

bool registry_mutation_transport_retry_safe(::dsn::error_code code)
{
    return rpc_error_is_pre_apply(code);
}

template <typename TResponse, typename FCall>
std::pair< ::dsn::error_code, TResponse>
registry_call_with_failover(const ::dsn::rpc_address &server,
                            bool retry_ambiguous_transport,
                            FCall call)
{
    const uint32_t attempts = registry_client_max_attempts();
    const uint64_t backoff_ms = registry_client_backoff_ms();
    std::pair< ::dsn::error_code, TResponse> result(::dsn::ERR_UNKNOWN, TResponse());
    for (uint32_t attempt = 1; attempt <= attempts; ++attempt)
    {
        const ::dsn::rpc_address attempted_leader =
            server.type() == HOST_TYPE_GROUP
                ? ::dsn::rpc_address(
                      ::dsn_group_get_leader(server.group_handle()))
                : ::dsn::rpc_address();
        result = call();
        if (result.first == ::dsn::ERR_OK)
        {
            const bool retry_response =
                response_requires_failover(result.second) &&
                server.type() == HOST_TYPE_GROUP && attempt < attempts;
            if (response_requires_rotation(result.second) &&
                server.type() == HOST_TYPE_GROUP &&
                ::dsn::rpc_address(
                    ::dsn_group_get_leader(server.group_handle())) ==
                    attempted_leader)
            {
                (void)::dsn_group_forward_leader(server.group_handle());
            }
            if (!retry_response)
            {
                return result;
            }
        }
        else
        {
            const bool retry_transport =
                (retry_ambiguous_transport
                     ? rpc_should_retry(result.first, true)
                     : registry_mutation_transport_retry_safe(result.first)) &&
                attempt < attempts;
            if (rpc_should_retry(result.first, true) &&
                server.type() == HOST_TYPE_GROUP &&
                ::dsn::rpc_address(
                    ::dsn_group_get_leader(server.group_handle())) ==
                    attempted_leader)
            {
                // Rotate even when this mutation cannot be replayed. The next
                // heartbeat/operation must not stay pinned to a timed-out peer.
                (void)::dsn_group_forward_leader(server.group_handle());
            }
            if (!retry_transport)
            {
                return result;
            }
        }
        if (backoff_ms > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms * attempt));
        }
    }
    return result;
}

} // namespace

bool encode_registry_state_record(const registry_state_record &record,
                                  std::string *encoded,
                                  std::string *error)
{
    if (encoded == nullptr)
    {
        if (error != nullptr)
        {
            *error = "registry record output is null";
        }
        return false;
    }
    if (record.schema_version != RASN_REGISTRY_STATE_SCHEMA_VERSION ||
        record.descriptor.agent_id.empty())
    {
        if (error != nullptr)
        {
            *error = "invalid registry state record";
        }
        return false;
    }

    ::dsn::binary_writer writer;
    writer.write(std::string(k_registry_state_magic));
    writer.write(record.schema_version);
    writer.write(record.writer_fence);
    writer.write(record.tombstone);
    marshall(writer, record.descriptor, DSF_THRIFT_BINARY);
    writer.write(record.last_heartbeat_ms);
    writer.write(record.lease_tracked);
    const ::dsn::blob buffer = writer.get_buffer();
    encoded->assign(buffer.data(), buffer.length());
    return true;
}

bool decode_registry_state_record(const std::string &encoded,
                                  registry_state_record *record,
                                  std::string *error)
{
    if (record == nullptr)
    {
        if (error != nullptr)
        {
            *error = "registry record output is null";
        }
        return false;
    }
    if (encoded.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
    {
        if (error != nullptr)
        {
            *error = "registry state record is too large";
        }
        return false;
    }

    try
    {
        ::dsn::binary_reader reader(
            ::dsn::blob(encoded.data(), 0, static_cast<unsigned int>(encoded.size())));
        std::string magic;
        registry_state_record decoded;
        reader.read(magic);
        reader.read(decoded.schema_version);
        reader.read(decoded.writer_fence);
        reader.read(decoded.tombstone);
        unmarshall(reader, decoded.descriptor, DSF_THRIFT_BINARY);
        reader.read(decoded.last_heartbeat_ms);
        reader.read(decoded.lease_tracked);
        if (magic != k_registry_state_magic ||
            decoded.schema_version != RASN_REGISTRY_STATE_SCHEMA_VERSION ||
            decoded.descriptor.agent_id.empty() ||
            !reader.is_eof())
        {
            if (error != nullptr)
            {
                *error = "invalid or unsupported registry state record";
            }
            return false;
        }
        *record = decoded;
        return true;
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr)
        {
            *error = std::string("invalid registry state record: ") + ex.what();
        }
        return false;
    }
}

agent_registry::agent_registry()
    : _writer_active(false), _writer_promoting(false), _writer_fence(0)
{
}

bool agent_registry::configure_shared_backend(
    const std::shared_ptr<rasn_coordination_context> &coordination,
    const std::string &state_prefix,
    const std::string &leader_resource,
    const std::string &leader_owner,
    std::string *error)
{
    if (coordination == nullptr || coordination->service() == nullptr)
    {
        if (error != nullptr)
        {
            *error = "HA registry coordination context is unavailable";
        }
        return false;
    }
    std::string normalized;
    if (!normalize_relative_state_path(state_prefix, &normalized, error))
    {
        return false;
    }
    if (leader_resource.empty() || leader_owner.empty())
    {
        if (error != nullptr)
        {
            *error = "HA registry leader resource and owner must be non-empty";
        }
        return false;
    }

    ::dsn::service::zauto_lock guard(_lock);
    _coordination = coordination;
    _shared_state_prefix = normalized;
    _shared_agents_prefix = normalized + "/agents";
    _shared_epochs_prefix = normalized + "/epochs";
    _leader_resource = leader_resource;
    _leader_owner = leader_owner;
    _leadership_lost.reset();
    _writer_active = false;
    _writer_promoting = false;
    _writer_fence = 0;
    _agents.clear();
    return true;
}

void agent_registry::clear_shared_writer()
{
    ::dsn::service::zauto_lock guard(_lock);
    _writer_active = false;
    _writer_promoting = false;
    _writer_fence = 0;
    _leadership_lost.reset();
}

bool agent_registry::activate_shared_writer(
    uint64_t fencing_token,
    const std::vector<agent_descriptor> &static_descriptors,
    const std::shared_ptr<std::atomic<bool>> &leadership_lost,
    std::string *error)
{
    if (leadership_lost == nullptr)
    {
        if (error != nullptr)
        {
            *error = "shared registry leadership-loss signal is unavailable";
        }
        return false;
    }

    std::map<std::string, agent_descriptor> desired;
    for (const agent_descriptor &descriptor : static_descriptors)
    {
        if (descriptor.agent_id.empty() || descriptor.role.empty())
        {
            if (error != nullptr)
            {
                *error = "static registry descriptor is missing id or role";
            }
            return false;
        }
        desired[descriptor.agent_id] = descriptor;
    }

    ::dsn::service::zauto_lock guard(_lock);
    if (_coordination == nullptr)
    {
        if (error != nullptr)
        {
            *error = std::string(k_registry_backend_unavailable) +
                     ": shared registry backend is not configured";
        }
        return false;
    }
    _leadership_lost = leadership_lost;
    _writer_active = false;
    _writer_promoting = true;
    _writer_fence = fencing_token;
    if (!replace_static_agents_locked(desired, error) ||
        !commit_shared_epoch(error))
    {
        _writer_active = false;
        _writer_promoting = false;
        _writer_fence = 0;
        _leadership_lost.reset();
        return false;
    }
    _writer_promoting = false;
    _writer_active = true;
    return true;
}

bool agent_registry::shared_backend_enabled() const
{
    ::dsn::service::zauto_lock guard(_lock);
    return _coordination != nullptr;
}

bool agent_registry::shared_writer_active() const
{
    ::dsn::service::zauto_lock guard(_lock);
    return _writer_active;
}

uint64_t agent_registry::shared_writer_fence() const
{
    ::dsn::service::zauto_lock guard(_lock);
    return _writer_fence;
}

bool agent_registry::register_agent(const agent_descriptor &descriptor, std::string *error)
{
    return register_agent(descriptor, error, false);
}

bool agent_registry::register_agent(const agent_descriptor &descriptor,
                                    std::string *error,
                                    bool lease_tracked)
{
    if (descriptor.agent_id.empty())
    {
        if (error != nullptr)
        {
            *error = "agent descriptor missing id";
        }
        return false;
    }
    if (descriptor.role.empty())
    {
        if (error != nullptr)
        {
            *error = "agent descriptor missing role";
        }
        return false;
    }

    ::dsn::service::zauto_lock guard(_lock);
    if (_coordination != nullptr)
    {
        if (!_writer_active)
        {
            if (error != nullptr)
            {
                *error = std::string(k_registry_not_primary) +
                         ": this registry frontend is not the active writer";
            }
            return false;
        }

        registry_state_record existing;
        bool found = false;
        if (!read_shared_record(descriptor.agent_id, &existing, &found, error))
        {
            return false;
        }
        registry_state_record record;
        record.writer_fence = _writer_fence;
        record.descriptor = descriptor;
        record.last_heartbeat_ms = ::dsn_now_ms();
        record.lease_tracked =
            lease_tracked || (found && !existing.tombstone && existing.lease_tracked);
        if (!write_shared_record(record, error))
        {
            return false;
        }
        dinfo("%s shared rASN agent id=%s role=%s capabilities=%llu lease_tracked=%s fence=%llu",
              found && !existing.tombstone ? "updated" : "registered",
              descriptor.agent_id.c_str(),
              descriptor.role.c_str(),
              static_cast<unsigned long long>(descriptor.capabilities.size()),
              record.lease_tracked ? "true" : "false",
              static_cast<unsigned long long>(record.writer_fence));
        return true;
    }

    registry_entry &entry = _agents[descriptor.agent_id];
    const bool existed = !entry.descriptor.agent_id.empty();
    entry.descriptor = descriptor;
    entry.last_heartbeat_ms = ::dsn_now_ms();
    entry.lease_tracked = entry.lease_tracked || lease_tracked;
    dinfo("%s rASN agent id=%s role=%s capabilities=%llu lease_tracked=%s",
          existed ? "updated" : "registered",
          descriptor.agent_id.c_str(),
          descriptor.role.c_str(),
          static_cast<unsigned long long>(descriptor.capabilities.size()),
          entry.lease_tracked ? "true" : "false");
    return true;
}

bool agent_registry::unregister_agent(const std::string &agent_id)
{
    return unregister_agent(agent_id, nullptr);
}

bool agent_registry::unregister_agent(const std::string &agent_id, std::string *error)
{
    if (agent_id.empty())
    {
        if (error != nullptr)
        {
            *error = "registry unregister missing agent id";
        }
        return false;
    }

    ::dsn::service::zauto_lock guard(_lock);
    if (_coordination != nullptr)
    {
        if (!_writer_active)
        {
            if (error != nullptr)
            {
                *error = std::string(k_registry_not_primary) +
                         ": this registry frontend is not the active writer";
            }
            return false;
        }

        registry_state_record existing;
        bool found = false;
        if (!read_shared_record(agent_id, &existing, &found, error))
        {
            return false;
        }
        if (!found || existing.tombstone)
        {
            return true;
        }

        existing.writer_fence = _writer_fence;
        existing.tombstone = true;
        existing.last_heartbeat_ms = ::dsn_now_ms();
        if (!write_shared_record(existing, error))
        {
            return false;
        }
        dinfo("unregistered shared rASN agent id=%s fence=%llu",
              agent_id.c_str(),
              static_cast<unsigned long long>(_writer_fence));
        return true;
    }

    const size_t removed = _agents.erase(agent_id);
    if (removed > 0)
    {
        dinfo("unregistered rASN agent id=%s", agent_id.c_str());
    }
    // Treat an absent registration as already removed.
    return true;
}

std::vector<agent_descriptor> agent_registry::list_agents() const
{
    return list_agents(true);
}

std::vector<agent_descriptor> agent_registry::list_agents(bool healthy_only) const
{
    std::vector<agent_descriptor> result;
    std::string error;
    (void)list_agents(healthy_only, &result, &error);
    return result;
}

bool agent_registry::list_agents(bool healthy_only,
                                 std::vector<agent_descriptor> *agents,
                                 std::string *error) const
{
    if (agents == nullptr)
    {
        if (error != nullptr)
        {
            *error = "registry list output is null";
        }
        return false;
    }

    ::dsn::service::zauto_lock guard(_lock);
    agents->clear();
    const uint64_t now_ms = ::dsn_now_ms();
    const uint64_t lease_ms = registry_lease_ms();
    if (_coordination != nullptr)
    {
        std::vector<registry_state_record> records;
        if (!read_all_shared_records(&records, error))
        {
            return false;
        }
        agents->reserve(records.size());
        for (const registry_state_record &record : records)
        {
            if (record.tombstone)
            {
                continue;
            }
            // The elected writer is the sole HA lease-clock authority and turns
            // expired records into tombstones. Comparing its persisted timestamp
            // with a standby's local clock would make reads depend on node skew.
            if (healthy_only && !is_healthy_descriptor(record.descriptor))
            {
                continue;
            }
            agents->push_back(record.descriptor);
        }
        return true;
    }

    agents->reserve(_agents.size());
    for (const std::map<std::string, registry_entry>::value_type &entry : _agents)
    {
        if (healthy_only &&
            (!is_healthy_descriptor(entry.second.descriptor) ||
             !is_live_entry(entry.second, now_ms, lease_ms)))
        {
            continue;
        }
        agents->push_back(entry.second.descriptor);
    }
    return true;
}

std::vector<agent_descriptor>
agent_registry::query_by_capability(const std::string &capability) const
{
    return query_by_capability(capability, true);
}

std::vector<agent_descriptor>
agent_registry::query_by_capability(const std::string &capability, bool healthy_only) const
{
    std::vector<agent_descriptor> result;
    std::string error;
    (void)query_by_capability(capability, healthy_only, &result, &error);
    return result;
}

bool agent_registry::query_by_capability(const std::string &capability,
                                         bool healthy_only,
                                         std::vector<agent_descriptor> *agents,
                                         std::string *error) const
{
    std::vector<agent_descriptor> listed;
    if (!list_agents(healthy_only, &listed, error))
    {
        return false;
    }
    if (agents == nullptr)
    {
        if (error != nullptr)
        {
            *error = "registry query output is null";
        }
        return false;
    }
    agents->clear();
    for (const agent_descriptor &descriptor : listed)
    {
        if (has_capability(descriptor, capability))
        {
            agents->push_back(descriptor);
        }
    }
    return true;
}

bool agent_registry::find_agent(const std::string &agent_id,
                                agent_descriptor *descriptor) const
{
    return find_agent(agent_id, descriptor, nullptr);
}

bool agent_registry::find_agent(const std::string &agent_id,
                                agent_descriptor *descriptor,
                                std::string *error) const
{
    ::dsn::service::zauto_lock guard(_lock);
    if (_coordination != nullptr)
    {
        registry_state_record record;
        bool found = false;
        if (!read_shared_record(agent_id, &record, &found, error))
        {
            return false;
        }
        if (!found || record.tombstone)
        {
            return false;
        }
        if (descriptor != nullptr)
        {
            *descriptor = record.descriptor;
        }
        return true;
    }

    const std::map<std::string, registry_entry>::const_iterator it = _agents.find(agent_id);
    if (it == _agents.end())
    {
        return false;
    }
    if (descriptor != nullptr)
    {
        *descriptor = it->second.descriptor;
    }
    return true;
}

bool agent_registry::heartbeat(const agent_descriptor &descriptor, std::string *error)
{
    if (descriptor.agent_id.empty())
    {
        if (error != nullptr)
        {
            *error = "heartbeat missing agent id";
        }
        return false;
    }

    ::dsn::service::zauto_lock guard(_lock);
    if (_coordination != nullptr)
    {
        if (!_writer_active)
        {
            if (error != nullptr)
            {
                *error = std::string(k_registry_not_primary) +
                         ": this registry frontend is not the active writer";
            }
            return false;
        }
        registry_state_record record;
        bool found = false;
        if (!read_shared_record(descriptor.agent_id, &record, &found, error))
        {
            return false;
        }
        if (!found || record.tombstone)
        {
            if (error != nullptr)
            {
                *error = std::string(k_registry_agent_not_found) +
                         ": heartbeat for unknown agent " +
                         descriptor.agent_id;
            }
            return false;
        }

        record.writer_fence = _writer_fence;
        record.descriptor.health = descriptor.health.empty() ? "healthy" : descriptor.health;
        if (!descriptor.host.empty())
        {
            record.descriptor.host = descriptor.host;
        }
        if (descriptor.port != 0)
        {
            record.descriptor.port = descriptor.port;
        }
        if (!descriptor.endpoint_uri.empty())
        {
            record.descriptor.endpoint_uri = descriptor.endpoint_uri;
        }
        record.last_heartbeat_ms = ::dsn_now_ms();
        record.lease_tracked = true;
        return write_shared_record(record, error);
    }

    std::map<std::string, registry_entry>::iterator it = _agents.find(descriptor.agent_id);
    if (it == _agents.end())
    {
        if (error != nullptr)
        {
            *error = std::string(k_registry_agent_not_found) +
                     ": heartbeat for unknown agent " +
                     descriptor.agent_id;
        }
        return false;
    }

    it->second.descriptor.health = descriptor.health.empty() ? "healthy" : descriptor.health;
    if (!descriptor.host.empty())
    {
        it->second.descriptor.host = descriptor.host;
    }
    if (descriptor.port != 0)
    {
        it->second.descriptor.port = descriptor.port;
    }
    if (!descriptor.endpoint_uri.empty())
    {
        it->second.descriptor.endpoint_uri = descriptor.endpoint_uri;
    }
    it->second.last_heartbeat_ms = ::dsn_now_ms();
    it->second.lease_tracked = true;
    return true;
}

size_t agent_registry::expire_leases(uint64_t now_ms, uint64_t lease_ms)
{
    size_t expired = 0;
    std::string error;
    (void)expire_leases(now_ms, lease_ms, &expired, &error);
    return expired;
}

bool agent_registry::expire_leases(uint64_t now_ms,
                                   uint64_t lease_ms,
                                   size_t *expired,
                                   std::string *error)
{
    if (expired == nullptr)
    {
        if (error != nullptr)
        {
            *error = "registry expiry output is null";
        }
        return false;
    }
    *expired = 0;
    if (lease_ms == 0)
    {
        return true;
    }

    ::dsn::service::zauto_lock guard(_lock);
    if (_coordination != nullptr)
    {
        if (!_writer_active)
        {
            if (error != nullptr)
            {
                *error = std::string(k_registry_not_primary) +
                         ": this registry frontend is not the active writer";
            }
            return false;
        }
        std::vector<registry_state_record> records;
        if (!read_all_shared_records(&records, error))
        {
            return false;
        }
        for (registry_state_record &record : records)
        {
            if (record.tombstone || is_live_record(record, now_ms, lease_ms))
            {
                continue;
            }
            record.writer_fence = _writer_fence;
            record.tombstone = true;
            if (!write_shared_record(record, error))
            {
                return false;
            }
            dinfo("expired shared rASN agent lease id=%s fence=%llu",
                  record.descriptor.agent_id.c_str(),
                  static_cast<unsigned long long>(_writer_fence));
            ++(*expired);
        }
        return true;
    }

    for (std::map<std::string, registry_entry>::iterator it = _agents.begin();
         it != _agents.end();)
    {
        if (!is_live_entry(it->second, now_ms, lease_ms))
        {
            dinfo("expired rASN agent lease id=%s", it->second.descriptor.agent_id.c_str());
            it = _agents.erase(it);
            ++(*expired);
        }
        else
        {
            ++it;
        }
    }
    return true;
}

bool agent_registry::replace_static_agents(
    const std::vector<agent_descriptor> &descriptors,
    std::string *error)
{
    std::map<std::string, agent_descriptor> desired;
    for (const agent_descriptor &descriptor : descriptors)
    {
        if (descriptor.agent_id.empty() || descriptor.role.empty())
        {
            if (error != nullptr)
            {
                *error = "static registry descriptor is missing id or role";
            }
            return false;
        }
        desired[descriptor.agent_id] = descriptor;
    }

    ::dsn::service::zauto_lock guard(_lock);
    return replace_static_agents_locked(desired, error);
}

bool agent_registry::replace_static_agents_locked(
    const std::map<std::string, agent_descriptor> &desired,
    std::string *error)
{
    if (_coordination == nullptr)
    {
        for (const std::map<std::string, agent_descriptor>::value_type &entry : desired)
        {
            registry_entry &stored = _agents[entry.first];
            stored.descriptor = entry.second;
            stored.last_heartbeat_ms = ::dsn_now_ms();
            stored.lease_tracked = false;
        }
        return true;
    }
    if (!_writer_active && !_writer_promoting)
    {
        if (error != nullptr)
        {
            *error = std::string(k_registry_not_primary) +
                     ": this registry frontend is not the active writer";
        }
        return false;
    }
    if (!verify_shared_writer(error))
    {
        return false;
    }

    std::vector<registry_state_record> current;
    if (!read_all_shared_records(false, &current, error))
    {
        return false;
    }

    // Promote every current record into the new leader epoch before exposing
    // mutations. Without this barrier, a delayed old leader could still overwrite
    // its own lower-fence child for an agent the successor had not touched yet.
    // Lease timestamps are process-clock values, so grant every live dynamic
    // registration a fresh lease in the new writer's clock domain.
    const uint64_t promotion_now_ms = ::dsn_now_ms();
    for (registry_state_record &record : current)
    {
        record.writer_fence = _writer_fence;
        if (!record.tombstone && record.lease_tracked)
        {
            record.last_heartbeat_ms = promotion_now_ms;
        }
        if (!write_shared_record(record, error))
        {
            return false;
        }
    }

    for (registry_state_record &record : current)
    {
        if (!record.tombstone && !record.lease_tracked &&
            desired.find(record.descriptor.agent_id) == desired.end())
        {
            record.writer_fence = _writer_fence;
            record.tombstone = true;
            record.last_heartbeat_ms = ::dsn_now_ms();
            if (!write_shared_record(record, error))
            {
                return false;
            }
        }
    }
    for (const std::map<std::string, agent_descriptor>::value_type &entry : desired)
    {
        const std::vector<registry_state_record>::const_iterator current_entry =
            std::find_if(current.begin(),
                         current.end(),
                         [&entry](const registry_state_record &record) {
                             return !record.tombstone && record.lease_tracked &&
                                    record.descriptor.agent_id == entry.first;
                         });
        if (current_entry != current.end())
        {
            // A live dynamic registration with the same id superseded the static
            // bootstrap descriptor; preserve it across registry failover.
            continue;
        }
        registry_state_record record;
        record.writer_fence = _writer_fence;
        record.descriptor = entry.second;
        record.last_heartbeat_ms = ::dsn_now_ms();
        record.lease_tracked = false;
        if (!write_shared_record(record, error))
        {
            return false;
        }
    }
    return true;
}

std::string agent_registry::describe() const
{
    std::vector<agent_descriptor> agents;
    std::string error;
    std::ostringstream oss;
    oss << "registered agents:\n";
    if (!list_agents(false, &agents, &error))
    {
        oss << "- <unavailable: " << error << ">\n";
        return oss.str();
    }
    if (agents.empty())
    {
        oss << "- <none>\n";
        return oss.str();
    }

    for (const agent_descriptor &agent : agents)
    {
        oss << "- " << agent.agent_id << " role=" << agent.role << " health=" << agent.health;
        if (!agent.endpoint_uri.empty())
        {
            oss << " endpoint=" << agent.endpoint_uri;
        }
        else if (!agent.host.empty() || agent.port != 0)
        {
            oss << " endpoint=" << agent.host << ":" << agent.port;
        }
        oss << " capabilities=";
        for (size_t i = 0; i < agent.capabilities.size(); ++i)
        {
            if (i != 0)
            {
                oss << ",";
            }
            oss << agent.capabilities[i].name;
        }
        oss << "\n";
    }
    return oss.str();
}

std::string agent_registry::shared_agent_path(const std::string &agent_id) const
{
    return _shared_agents_prefix + "/" + hex_encode(agent_id);
}

bool agent_registry::read_shared_record(const std::string &agent_id,
                                        registry_state_record *record,
                                        bool *found,
                                        std::string *error) const
{
    if (record == nullptr || found == nullptr || _coordination == nullptr ||
        _coordination->service() == nullptr)
    {
        if (error != nullptr)
        {
            *error = std::string(k_registry_backend_unavailable) +
                     ": shared registry backend is not configured";
        }
        return false;
    }
    *found = false;

    uint64_t committed_fence = 0;
    std::string committed_owner;
    bool epoch_found = false;
    if (!read_committed_epoch(
            true, &committed_fence, &committed_owner, &epoch_found, error))
    {
        return false;
    }
    if (!epoch_found)
    {
        return true;
    }

    std::string encoded;
    const ::dsn::error_code got = _coordination->service()->get_state(
        shared_agent_path(agent_id) + "/" + std::to_string(committed_fence),
        encoded);
    if (error_is_not_found(got))
    {
        return verify_committed_epoch_owner(
            committed_fence, committed_owner, error);
    }
    if (got != ::dsn::ERR_OK)
    {
        set_backend_error("reading shared registry record for " + agent_id, got, error);
        return false;
    }

    registry_state_record decoded;
    std::string decode_error;
    if (!decode_registry_state_record(encoded, &decoded, &decode_error))
    {
        if (error != nullptr)
        {
            *error = std::string(k_registry_backend_unavailable) +
                     ": corrupt shared registry record for " + agent_id +
                     ": " + decode_error;
        }
        return false;
    }
    if (decoded.writer_fence != committed_fence ||
        decoded.descriptor.agent_id != agent_id)
    {
        if (error != nullptr)
        {
            *error = std::string(k_registry_backend_unavailable) +
                     ": shared registry record identity/fence mismatch for " +
                     agent_id;
        }
        return false;
    }
    *record = decoded;
    *found = true;
    return verify_committed_epoch_owner(
        committed_fence, committed_owner, error);
}

bool agent_registry::read_all_shared_records(
    std::vector<registry_state_record> *records,
    std::string *error) const
{
    return read_all_shared_records(true, records, error);
}

bool agent_registry::read_all_shared_records(
    bool require_current_owner,
    std::vector<registry_state_record> *records,
    std::string *error) const
{
    if (records == nullptr || _coordination == nullptr || _coordination->service() == nullptr)
    {
        if (error != nullptr)
        {
            *error = std::string(k_registry_backend_unavailable) +
                     ": shared registry list output/backend is unavailable";
        }
        return false;
    }
    records->clear();

    uint64_t committed_fence = 0;
    std::string committed_owner;
    bool epoch_found = false;
    if (!read_committed_epoch(
            require_current_owner,
            &committed_fence,
            &committed_owner,
            &epoch_found,
            error))
    {
        return false;
    }
    if (!epoch_found)
    {
        return true;
    }

    std::vector<std::string> agent_keys;
    const ::dsn::error_code listed =
        _coordination->service()->list_state(_shared_agents_prefix, agent_keys);
    if (error_is_not_found(listed))
    {
        return !require_current_owner ||
               verify_committed_epoch_owner(
                   committed_fence, committed_owner, error);
    }
    if (listed != ::dsn::ERR_OK)
    {
        set_backend_error("listing shared registry agents", listed, error);
        return false;
    }

    records->reserve(agent_keys.size());
    for (const std::string &agent_key : agent_keys)
    {
        std::string encoded;
        const ::dsn::error_code got = _coordination->service()->get_state(
            _shared_agents_prefix + "/" + agent_key + "/" +
                std::to_string(committed_fence),
            encoded);
        if (error_is_not_found(got))
        {
            continue;
        }
        if (got != ::dsn::ERR_OK)
        {
            set_backend_error("reading shared registry record", got, error);
            return false;
        }

        registry_state_record record;
        std::string decode_error;
        if (!decode_registry_state_record(encoded, &record, &decode_error))
        {
            if (error != nullptr)
            {
                *error = std::string(k_registry_backend_unavailable) +
                         ": corrupt shared registry record: " + decode_error;
            }
            return false;
        }
        if (record.writer_fence != committed_fence ||
            hex_encode(record.descriptor.agent_id) != agent_key)
        {
            if (error != nullptr)
            {
                *error = std::string(k_registry_backend_unavailable) +
                         ": shared registry record identity/fence mismatch";
            }
            return false;
        }
        records->push_back(record);
    }
    return !require_current_owner ||
           verify_committed_epoch_owner(
               committed_fence, committed_owner, error);
}

bool agent_registry::read_committed_epoch(bool require_current_owner,
                                          uint64_t *fencing_token,
                                          std::string *writer_owner,
                                          bool *found,
                                          std::string *error) const
{
    if (fencing_token == nullptr || found == nullptr || _coordination == nullptr ||
        _coordination->service() == nullptr)
    {
        if (error != nullptr)
        {
            *error = std::string(k_registry_backend_unavailable) +
                     ": shared registry epoch backend/output is unavailable";
        }
        return false;
    }
    *found = false;

    std::vector<std::string> epochs;
    const ::dsn::error_code listed =
        _coordination->service()->list_state(_shared_epochs_prefix, epochs);
    if (error_is_not_found(listed))
    {
        if (require_current_owner && error != nullptr)
        {
            *error = std::string(k_registry_backend_unavailable) +
                     ": shared registry has no committed leadership epoch";
        }
        return !require_current_owner;
    }
    if (listed != ::dsn::ERR_OK)
    {
        set_backend_error("listing shared registry epochs", listed, error);
        return false;
    }

    uint64_t committed_fence = 0;
    bool parsed_any = false;
    for (const std::string &epoch : epochs)
    {
        uint64_t candidate = 0;
        if (!parse_fence(epoch, &candidate))
        {
            if (error != nullptr)
            {
                *error = std::string(k_registry_backend_unavailable) +
                         ": invalid shared registry epoch child: " + epoch;
            }
            return false;
        }
        if (!parsed_any || candidate > committed_fence)
        {
            committed_fence = candidate;
            parsed_any = true;
        }
    }
    if (!parsed_any)
    {
        if (require_current_owner && error != nullptr)
        {
            *error = std::string(k_registry_backend_unavailable) +
                     ": shared registry epoch directory is empty";
        }
        return !require_current_owner;
    }

    std::string encoded;
    const ::dsn::error_code got = _coordination->service()->get_state(
        _shared_epochs_prefix + "/" + std::to_string(committed_fence), encoded);
    if (got != ::dsn::ERR_OK)
    {
        set_backend_error("reading shared registry epoch", got, error);
        return false;
    }
    registry_epoch_record epoch;
    std::string decode_error;
    if (!decode_registry_epoch_record(encoded, &epoch, &decode_error) ||
        epoch.writer_fence != committed_fence)
    {
        if (error != nullptr)
        {
            *error = std::string(k_registry_backend_unavailable) +
                     ": corrupt shared registry epoch: " + decode_error;
        }
        return false;
    }

    if (require_current_owner &&
        !verify_committed_epoch_owner(
            committed_fence, epoch.writer_owner, error))
    {
        return false;
    }

    *fencing_token = committed_fence;
    if (writer_owner != nullptr)
    {
        *writer_owner = epoch.writer_owner;
    }
    *found = true;
    return true;
}

bool agent_registry::verify_committed_epoch_owner(
    uint64_t fencing_token,
    const std::string &writer_owner,
    std::string *error) const
{
    std::string current_owner;
    uint64_t current_fence = 0;
    if (_coordination == nullptr || _coordination->service() == nullptr ||
        !_coordination->service()->query_owner(
            _leader_resource, current_owner, &current_fence))
    {
        if (error != nullptr)
        {
            *error = std::string(k_registry_backend_unavailable) +
                     ": registry leadership is unavailable";
        }
        return false;
    }
    if (current_fence != fencing_token || current_owner != writer_owner)
    {
        if (error != nullptr)
        {
            *error = std::string(k_registry_backend_unavailable) +
                     ": registry leadership epoch is transitioning";
        }
        return false;
    }
    return true;
}

bool agent_registry::commit_shared_epoch(std::string *error)
{
    if (!verify_shared_writer(error))
    {
        return false;
    }
    registry_epoch_record epoch;
    epoch.writer_fence = _writer_fence;
    epoch.writer_owner = _leader_owner;
    std::string encoded;
    if (!encode_registry_epoch_record(epoch, &encoded, error))
    {
        return false;
    }
    const ::dsn::error_code put = _coordination->service()->put_state(
        _shared_epochs_prefix + "/" + std::to_string(_writer_fence), encoded);
    if (put != ::dsn::ERR_OK)
    {
        set_backend_error("committing shared registry epoch", put, error);
        return false;
    }
    return verify_shared_writer(error);
}

bool agent_registry::write_shared_record(const registry_state_record &record,
                                         std::string *error)
{
    if (_coordination == nullptr || _coordination->service() == nullptr ||
        (!_writer_active && !_writer_promoting) ||
        record.writer_fence != _writer_fence)
    {
        if (error != nullptr)
        {
            *error = std::string(k_registry_not_primary) +
                     ": shared registry writer fence is unavailable or stale";
        }
        return false;
    }
    if (!verify_shared_writer(error))
    {
        return false;
    }

    std::string encoded;
    if (!encode_registry_state_record(record, &encoded, error))
    {
        return false;
    }
    const std::string path =
        shared_agent_path(record.descriptor.agent_id) + "/" +
        std::to_string(record.writer_fence);
    const ::dsn::error_code put = _coordination->service()->put_state(path, encoded);
    if (put != ::dsn::ERR_OK)
    {
        if (error != nullptr)
        {
            *error = std::string(k_registry_mutation_outcome_unknown) +
                     ": shared registry write returned " + put.to_string() +
                     " after backend submission";
        }
        return false;
    }
    std::string validation_error;
    if (!verify_shared_writer(&validation_error))
    {
        if (error != nullptr)
        {
            *error = std::string(k_registry_mutation_outcome_unknown) +
                     ": shared registry write may have committed before "
                     "ownership validation failed: " +
                     validation_error;
        }
        return false;
    }
    return true;
}

bool agent_registry::verify_shared_writer(std::string *error) const
{
    std::string owner;
    uint64_t fence = 0;
    if (_coordination == nullptr || _coordination->service() == nullptr ||
        (!_writer_active && !_writer_promoting) ||
        _leadership_lost == nullptr ||
        _leadership_lost->load() ||
        !_coordination->service()->query_owner(_leader_resource, owner, &fence) ||
        owner != _leader_owner || fence != _writer_fence)
    {
        if (error != nullptr)
        {
            *error = std::string(k_registry_not_primary) +
                     ": registry leadership changed while persisting the mutation";
        }
        return false;
    }
    return true;
}

bool agent_registry::has_capability(const agent_descriptor &descriptor,
                                    const std::string &capability) const
{
    for (const agent_capability &candidate : descriptor.capabilities)
    {
        if (candidate.name == capability)
        {
            return true;
        }
    }
    return false;
}

bool agent_registry::is_live_entry(const registry_entry &entry,
                                   uint64_t now_ms,
                                   uint64_t lease_ms) const
{
    if (!entry.lease_tracked || lease_ms == 0 || now_ms <= entry.last_heartbeat_ms)
    {
        return true;
    }
    return now_ms - entry.last_heartbeat_ms <= lease_ms;
}

bool agent_registry::is_live_record(const registry_state_record &record,
                                    uint64_t now_ms,
                                    uint64_t lease_ms) const
{
    if (!record.lease_tracked || lease_ms == 0 || now_ms <= record.last_heartbeat_ms)
    {
        return true;
    }
    return now_ms - record.last_heartbeat_ms <= lease_ms;
}

agent_registry &global_agent_registry()
{
    static agent_registry registry;
    return registry;
}

std::vector<agent_descriptor> configured_static_agents()
{
    std::vector<agent_descriptor> descriptors;
    const std::string prefix("rasn.agent.");
    for (const std::string &section : config_sections())
    {
        if (section.compare(0, prefix.size(), prefix) != 0)
        {
            continue;
        }

        const std::string default_agent_id = section.substr(prefix.size());
        agent_descriptor descriptor;
        descriptor.agent_id =
            ::dsn_config_get_value_string(section.c_str(), "agent_id", default_agent_id.c_str(), "");
        descriptor.role =
            ::dsn_config_get_value_string(section.c_str(), "role", "custom", "");
        descriptor.app_name =
            ::dsn_config_get_value_string(section.c_str(), "app_name", descriptor.agent_id.c_str(), "");
        descriptor.endpoint_uri =
            ::dsn_config_get_value_string(section.c_str(), "uri", "", "");
        descriptor.endpoint_uri = ::dsn_config_get_value_string(
            section.c_str(), "endpoint_uri", descriptor.endpoint_uri.c_str(), "");
        descriptor.host = ::dsn_config_get_value_string(
            section.c_str(),
            "host",
            descriptor.endpoint_uri.empty() ? "localhost" : "",
            "");
        descriptor.port = static_cast<uint32_t>(
            ::dsn_config_get_value_uint64(section.c_str(), "port", 0, ""));
        descriptor.version =
            ::dsn_config_get_value_string(section.c_str(), "version", "static-config", "");
        descriptor.health =
            ::dsn_config_get_value_string(section.c_str(), "health", "unknown", "");

        const std::string side_effect_class = ::dsn_config_get_value_string(
            section.c_str(), "side_effect_class", "none", "");
        const std::string capabilities =
            ::dsn_config_get_value_string(section.c_str(), "capabilities", "", "");
        for (const std::string &capability : split_config_csv(capabilities))
        {
            descriptor.capabilities.push_back(
                make_static_capability(capability, side_effect_class));
        }
        descriptors.push_back(descriptor);
    }
    return descriptors;
}

void load_static_agents_from_config_once()
{
    ::dsn::service::zauto_lock guard(static_registry_config_lock());
    if (static_registry_config_loaded())
    {
        return;
    }
    static_registry_config_loaded() = true;

    for (const agent_descriptor &descriptor : configured_static_agents())
    {
        std::string error;
        if (!global_agent_registry().register_agent(descriptor, &error))
        {
            dwarn("failed to load static rasn agent %s: %s",
                  descriptor.agent_id.c_str(),
                  error.c_str());
        }
    }
}

bool parse_rasn_registry_addresses(const std::string &value,
                                   std::vector< ::dsn::rpc_address> *addresses,
                                   std::string *error)
{
    if (addresses == nullptr)
    {
        if (error != nullptr)
        {
            *error = "registry address output is null";
        }
        return false;
    }
    addresses->clear();
    const std::vector<std::string> tokens = split_config_csv(value);
    if (tokens.empty())
    {
        if (error != nullptr)
        {
            *error = "registry_addresses contains no endpoints";
        }
        return false;
    }
    if (tokens.size() > k_max_registry_group_members)
    {
        if (error != nullptr)
        {
            *error = "registry_addresses exceeds the 64-member safety bound";
        }
        return false;
    }

    std::set< ::dsn::rpc_address> unique;
    for (const std::string &token : tokens)
    {
        ::dsn::url_host_address parsed(token.c_str());
        if (parsed.type() != HOST_TYPE_IPV4 || parsed.port() == 0)
        {
            if (error != nullptr)
            {
                *error = "invalid registry_addresses endpoint: " + token;
            }
            addresses->clear();
            return false;
        }
        if (unique.insert(parsed).second)
        {
            addresses->push_back(parsed);
        }
    }
    return true;
}

::dsn::rpc_address configured_rasn_registry_address()
{
    const std::string uri =
        ::dsn_config_get_value_string("rasn.service", "registry_uri", "", "rASN registry URI");
    if (!uri.empty())
    {
        return ::dsn::url_host_address(uri.c_str());
    }

    const std::string configured_group = ::dsn_config_get_value_string(
        "rasn.service",
        "registry_addresses",
        "",
        "Comma-separated HA registry frontend endpoints");
    if (!configured_group.empty())
    {
        std::vector< ::dsn::rpc_address> addresses;
        std::string error;
        if (!parse_rasn_registry_addresses(configured_group, &addresses, &error))
        {
            derror("invalid [rasn.service] registry_addresses: %s", error.c_str());
            return ::dsn::rpc_address();
        }

        // rDSN group addresses have manual lifetime. Configuration is immutable
        // after process startup, so this single group intentionally lives until
        // process exit and remains valid for every registry client facade.
        static dsn_group_t group = nullptr;
        static ::dsn::service::zlock group_lock;
        ::dsn::service::zauto_lock guard(group_lock);
        if (group == nullptr)
        {
            group = ::dsn_group_build("rasn.registry.frontends");
            for (const ::dsn::rpc_address &address : addresses)
            {
                if (!::dsn_group_add(group, address.c_addr()))
                {
                    derror("failed to add %s to the rASN registry frontend group",
                           address.to_string());
                    return ::dsn::rpc_address();
                }
            }
            ::dsn_group_set_leader(group, addresses.front().c_addr());
        }
        ::dsn::rpc_address address;
        address.assign_group(group);
        return address;
    }

    const std::string default_host = ::dsn_config_get_value_string(
        "rasn.service", "host", "localhost", "default rASN service RPC host");
    const std::string host = ::dsn_config_get_value_string(
        "rasn.service", "registry_host", default_host.c_str(), "rASN registry RPC host");
    const uint16_t port = static_cast<uint16_t>(::dsn_config_get_value_uint64(
        "rasn.service", "registry_port", 27100, "rASN registry RPC port"));
    ::dsn::rpc_address address;
    address.assign_ipv4(host.empty() ? "localhost" : host.c_str(), port);
    return address;
}

bool registry_response_is_agent_not_found(const agent_response &response)
{
    return !response.ok && response.error.code == k_registry_agent_not_found;
}

rasn_registry_rpc_service::rasn_registry_rpc_service(agent_registry *registry)
    : ::dsn::serverlet<rasn_registry_rpc_service>("rasn.registry"),
      _registry(registry == nullptr ? &global_agent_registry() : registry)
{
}

void rasn_registry_rpc_service::open_service()
{
    dinfo("opening rasn.registry serverlet");
    this->register_async_rpc_handler(
        RPC_RASN_REGISTRY_REGISTER, "register", &rasn_registry_rpc_service::on_register);
    this->register_async_rpc_handler(
        RPC_RASN_REGISTRY_UNREGISTER, "unregister", &rasn_registry_rpc_service::on_unregister);
    this->register_async_rpc_handler(
        RPC_RASN_REGISTRY_QUERY, "query", &rasn_registry_rpc_service::on_query);
    this->register_async_rpc_handler(
        RPC_RASN_REGISTRY_LIST, "list", &rasn_registry_rpc_service::on_list);
    this->register_async_rpc_handler(
        RPC_RASN_REGISTRY_HEARTBEAT, "heartbeat", &rasn_registry_rpc_service::on_heartbeat);
}

void rasn_registry_rpc_service::close_service()
{
    dinfo("closing rasn.registry serverlet");
    this->unregister_rpc_handler(RPC_RASN_REGISTRY_REGISTER);
    this->unregister_rpc_handler(RPC_RASN_REGISTRY_UNREGISTER);
    this->unregister_rpc_handler(RPC_RASN_REGISTRY_QUERY);
    this->unregister_rpc_handler(RPC_RASN_REGISTRY_LIST);
    this->unregister_rpc_handler(RPC_RASN_REGISTRY_HEARTBEAT);
}

void rasn_registry_rpc_service::on_register(
    const agent_descriptor &request,
    ::dsn::rpc_replier<agent_response> &reply)
{
    if (request.schema_version == 0)
    {
        reply(registry_response(
            request.agent_id, false, "missing registry register schema version"));
        return;
    }
    if (request.agent_id.empty())
    {
        reply(registry_response(
            request.agent_id, false, "missing registry register agent id"));
        return;
    }
    std::string error;
    const bool ok = _registry->register_agent(request, &error, true);
    reply(registry_response(request.agent_id, ok, error));
}

void rasn_registry_rpc_service::on_unregister(
    const std::string &agent_id,
    ::dsn::rpc_replier<agent_response> &reply)
{
    if (agent_id.empty())
    {
        reply(registry_response(
            agent_id, false, "missing registry unregister agent id"));
        return;
    }
    std::string error;
    const bool ok = _registry->unregister_agent(agent_id, &error);
    reply(registry_response(agent_id, ok, error));
}

void rasn_registry_rpc_service::on_query(
    const registry_query_request &request,
    ::dsn::rpc_replier<registry_query_response> &reply)
{
    if (request.schema_version == 0)
    {
        reply(registry_error_response("missing registry query schema version"));
        return;
    }
    if (request.capability.empty())
    {
        reply(registry_error_response("missing registry query capability"));
        return;
    }

    registry_query_response response;
    if (!_registry->query_by_capability(
            request.capability, request.healthy_only, &response.agents, &response.error))
    {
        response.ok = false;
    }
    reply(response);
}

void rasn_registry_rpc_service::on_list(
    const std::string &request,
    ::dsn::rpc_replier<registry_query_response> &reply)
{
    registry_query_response response;
    if (!_registry->list_agents(true, &response.agents, &response.error))
    {
        response.ok = false;
    }
    reply(response);
}

void rasn_registry_rpc_service::on_heartbeat(
    const agent_descriptor &request,
    ::dsn::rpc_replier<agent_response> &reply)
{
    if (request.schema_version == 0)
    {
        reply(registry_response(
            request.agent_id, false, "missing registry heartbeat schema version"));
        return;
    }
    std::string error;
    const bool ok = _registry->heartbeat(request, &error);
    reply(registry_response(request.agent_id, ok, error));
}

std::pair< ::dsn::error_code, agent_response>
rasn_registry_client::register_sync(const agent_descriptor &request,
                                    std::chrono::milliseconds timeout,
                                    int thread_hash,
                                    uint64_t partition_hash)
{
    return registry_call_with_failover<agent_response>(
        _server,
        false,
        [this, &request, timeout, thread_hash, partition_hash]() {
            return ::dsn::rpc::wait_and_unwrap<agent_response>(::dsn::rpc::call(
                _server,
                RPC_RASN_REGISTRY_REGISTER,
                request,
                nullptr,
                empty_callback,
                timeout,
                thread_hash,
                partition_hash));
        });
}

std::pair< ::dsn::error_code, agent_response>
rasn_registry_client::unregister_sync(const std::string &agent_id,
                                      std::chrono::milliseconds timeout,
                                      int thread_hash,
                                      uint64_t partition_hash)
{
    return registry_call_with_failover<agent_response>(
        _server,
        false,
        [this, &agent_id, timeout, thread_hash, partition_hash]() {
            return ::dsn::rpc::wait_and_unwrap<agent_response>(::dsn::rpc::call(
                _server,
                RPC_RASN_REGISTRY_UNREGISTER,
                agent_id,
                nullptr,
                empty_callback,
                timeout,
                thread_hash,
                partition_hash));
        });
}

std::pair< ::dsn::error_code, registry_query_response>
rasn_registry_client::query_sync(const registry_query_request &request,
                                 std::chrono::milliseconds timeout,
                                 int thread_hash,
                                 uint64_t partition_hash)
{
    return registry_call_with_failover<registry_query_response>(
        _server,
        true,
        [this, &request, timeout, thread_hash, partition_hash]() {
            return ::dsn::rpc::wait_and_unwrap<registry_query_response>(::dsn::rpc::call(
                _server,
                RPC_RASN_REGISTRY_QUERY,
                request,
                nullptr,
                empty_callback,
                timeout,
                thread_hash,
                partition_hash));
        });
}

std::pair< ::dsn::error_code, registry_query_response>
rasn_registry_client::list_sync(const std::string &request,
                                std::chrono::milliseconds timeout,
                                int thread_hash,
                                uint64_t partition_hash)
{
    return registry_call_with_failover<registry_query_response>(
        _server,
        true,
        [this, &request, timeout, thread_hash, partition_hash]() {
            return ::dsn::rpc::wait_and_unwrap<registry_query_response>(::dsn::rpc::call(
                _server,
                RPC_RASN_REGISTRY_LIST,
                request,
                nullptr,
                empty_callback,
                timeout,
                thread_hash,
                partition_hash));
        });
}

std::pair< ::dsn::error_code, agent_response>
rasn_registry_client::heartbeat_sync(const agent_descriptor &request,
                                     std::chrono::milliseconds timeout,
                                     int thread_hash,
                                     uint64_t partition_hash)
{
    return registry_call_with_failover<agent_response>(
        _server,
        false,
        [this, &request, timeout, thread_hash, partition_hash]() {
            return ::dsn::rpc::wait_and_unwrap<agent_response>(::dsn::rpc::call(
                _server,
                RPC_RASN_REGISTRY_HEARTBEAT,
                request,
                nullptr,
                empty_callback,
                timeout,
                thread_hash,
                partition_hash));
        });
}

rasn_registry_app::rasn_registry_app(::dsn_gpid gpid)
    : ::dsn::service_app(gpid),
      _registry(),
      _rpc(&_registry),
      _leader_fence(0),
      _leader_active(false),
      _shared_enabled(false),
      _last_sweep_ms(0),
      _lease_sweep_not_before_ms(0)
{
}

::dsn::error_code rasn_registry_app::start(int argc, char **argv)
{
    _shared_enabled = registry_shared_state_enabled();
    std::string error;
    if (_shared_enabled)
    {
        if (registry_lease_ms() != 0 && registry_lease_sweep_ms() == 0)
        {
            derror("cannot start HA rASN registry with leases enabled and "
                   "sweep_interval_ms=0: only the elected writer may evaluate "
                   "cross-process lease timestamps");
            return ::dsn::ERR_INVALID_PARAMETERS;
        }
        if (!configure_ha_registry(&error))
        {
            derror("cannot start HA rASN registry: %s", error.c_str());
            return ::dsn::ERR_INVALID_PARAMETERS;
        }
        const ::dsn::error_code leadership = try_acquire_leadership();
        if (leadership != ::dsn::ERR_OK && leadership != ::dsn::ERR_TIMEOUT)
        {
            derror("cannot establish HA rASN registry leadership: %s",
                   leadership.to_string());
            return leadership;
        }
    }
    else if (!_registry.replace_static_agents(configured_static_agents(), &error))
    {
        derror("cannot load static rASN registry descriptors: %s", error.c_str());
        return ::dsn::ERR_INVALID_PARAMETERS;
    }

    _rpc.open_service();
    if (!start_maintenance_timer() && _shared_enabled)
    {
        _rpc.close_service();
        _registry.clear_shared_writer();
        if (_leader_active)
        {
            const ::dsn::error_code released =
                _coordination->service()->release_ownership(
                _leader_resource, _leader_owner, false);
            fail_stop_if_registry_release_is_uncertain(
                released, "maintenance-timer startup failure");
            _leader_active = false;
            _leader_fence = 0;
        }
        _lease_sweep_not_before_ms = 0;
        return ::dsn::ERR_INVALID_STATE;
    }
    return ::dsn::ERR_OK;
}

::dsn::error_code rasn_registry_app::stop(bool cleanup)
{
    cancel_maintenance_timer();
    _rpc.close_service();

    ::dsn::error_code release = ::dsn::ERR_OK;
    if (_shared_enabled && _coordination != nullptr && _coordination->service() != nullptr)
    {
        _registry.clear_shared_writer();
        if (_leader_active)
        {
            release = _coordination->service()->release_ownership(
                _leader_resource, _leader_owner, false);
            fail_stop_if_registry_release_is_uncertain(release, "registry shutdown");
        }
        _leader_active = false;
        _leader_fence = 0;
        _leadership_lost.reset();
        _lease_sweep_not_before_ms = 0;
    }
    return release;
}

bool rasn_registry_app::configure_ha_registry(std::string *error)
{
    _coordination = shared_rasn_coordination_context();
    if (_coordination == nullptr)
    {
        if (error != nullptr)
        {
            *error = "coordination context allocation failed";
        }
        return false;
    }
    const ::dsn::error_code started = _coordination->start();
    if (started != ::dsn::ERR_OK)
    {
        set_backend_error("starting registry coordination", started, error);
        return false;
    }
    if (std::string(_coordination->provider_name()) != "zookeeper")
    {
        if (error != nullptr)
        {
            *error = "shared_state_enabled requires [rasn.coordination] provider=zookeeper; "
                     "resolved provider is " +
                     std::string(_coordination->provider_name());
        }
        return false;
    }

    _leader_resource = registry_leader_resource();
    std::ostringstream owner;
    owner << "registry-" << ::dsn::rpc_address(::dsn_primary_address()).to_std_string()
          << "-" << std::hex << reinterpret_cast<uintptr_t>(this)
          << "-" << ::dsn_now_ns();
    _leader_owner = owner.str();
    return _registry.configure_shared_backend(_coordination,
                                              registry_shared_state_prefix(),
                                              _leader_resource,
                                              _leader_owner,
                                              error);
}

::dsn::error_code rasn_registry_app::try_acquire_leadership()
{
    if (!_shared_enabled || _coordination == nullptr ||
        _coordination->service() == nullptr || _leader_active)
    {
        return _leader_active ? ::dsn::ERR_OK : ::dsn::ERR_INVALID_STATE;
    }

    uint64_t fence = 0;
    const std::shared_ptr<std::atomic<bool>> leadership_lost =
        std::make_shared<std::atomic<bool>>(false);
    const std::weak_ptr<std::atomic<bool>> weak_signal(leadership_lost);
    const ::dsn::error_code acquired = _coordination->service()->acquire_ownership(
        _leader_resource,
        _leader_owner,
        registry_leader_acquire_timeout_ms(),
        &fence,
        [weak_signal](const std::string &, uint64_t) {
            const std::shared_ptr<std::atomic<bool>> signal = weak_signal.lock();
            if (signal != nullptr)
            {
                signal->store(true);
            }
        });
    if (acquired != ::dsn::ERR_OK)
    {
        return acquired;
    }

    std::string error;
    if (!_registry.activate_shared_writer(
            fence, configured_static_agents(), leadership_lost, &error))
    {
        derror("HA registry primary fence=%llu failed to reconcile static descriptors: %s",
               static_cast<unsigned long long>(fence),
               error.c_str());
        _registry.clear_shared_writer();
        const ::dsn::error_code released = _coordination->service()->release_ownership(
            _leader_resource, _leader_owner, false);
        fail_stop_if_registry_release_is_uncertain(
            released, "failed epoch promotion");
        return ::dsn::ERR_FILE_OPERATION_FAILED;
    }
    _leader_fence = fence;
    _leader_active = true;
    _leadership_lost = leadership_lost;
    // Promotion may contain an unbounded number of synchronous ZooKeeper writes.
    // Give agents one complete lease after commit to resume heartbeats rather
    // than expiring records timestamped near the start of a long promotion.
    _last_sweep_ms = ::dsn_now_ms();
    const uint64_t lease_ms = registry_lease_ms();
    _lease_sweep_not_before_ms =
        lease_ms > (std::numeric_limits<uint64_t>::max)() - _last_sweep_ms
            ? (std::numeric_limits<uint64_t>::max)()
            : _last_sweep_ms + lease_ms;
    dinfo("rASN registry frontend became active writer owner=%s fence=%llu",
          _leader_owner.c_str(),
          static_cast<unsigned long long>(_leader_fence));
    return ::dsn::ERR_OK;
}

void rasn_registry_app::lose_leadership()
{
    if (!_leader_active)
    {
        return;
    }
    dwarn("rASN registry frontend lost active-writer ownership owner=%s fence=%llu",
          _leader_owner.c_str(),
          static_cast<unsigned long long>(_leader_fence));
    _registry.clear_shared_writer();
    _leader_active = false;
    _leader_fence = 0;
    _leadership_lost.reset();
    _lease_sweep_not_before_ms = 0;
}

bool rasn_registry_app::start_maintenance_timer()
{
    if (_maintenance_timer != nullptr)
    {
        return true;
    }

    uint64_t interval_ms = registry_lease_sweep_ms();
    if (_shared_enabled)
    {
        interval_ms =
            interval_ms == 0 ? registry_leader_retry_ms()
                             : (std::min)(interval_ms, registry_leader_retry_ms());
    }
    if (interval_ms == 0)
    {
        return true;
    }

    const std::chrono::milliseconds interval(interval_ms);
    _maintenance_timer = ::dsn::tasking::enqueue_timer(
        LPC_RASN_REGISTRY_LEASE_SWEEP_TIMER,
        nullptr,
        [this]() { maintain_registry(); },
        interval,
        0,
        interval);
    if (_maintenance_timer == nullptr)
    {
        dwarn("failed to start rASN registry maintenance timer");
        return false;
    }
    return true;
}

void rasn_registry_app::cancel_maintenance_timer()
{
    if (_maintenance_timer != nullptr)
    {
        _maintenance_timer->cancel(true);
        _maintenance_timer = nullptr;
    }
}

void rasn_registry_app::maintain_registry()
{
    if (_shared_enabled)
    {
        if (_leadership_lost != nullptr && _leadership_lost->load())
        {
            lose_leadership();
        }
        if (!_leader_active)
        {
            const ::dsn::error_code acquired = try_acquire_leadership();
            if (acquired != ::dsn::ERR_OK && acquired != ::dsn::ERR_TIMEOUT)
            {
                dwarn("HA rASN registry standby failed to acquire leadership: %s",
                      acquired.to_string());
            }
        }
        if (!_leader_active)
        {
            return;
        }
    }

    const uint64_t sweep_ms = registry_lease_sweep_ms();
    const uint64_t now_ms = ::dsn_now_ms();
    if (_lease_sweep_not_before_ms != 0 &&
        now_ms < _lease_sweep_not_before_ms)
    {
        return;
    }
    _lease_sweep_not_before_ms = 0;
    if (sweep_ms != 0 &&
        (_last_sweep_ms == 0 || now_ms <= _last_sweep_ms ||
         now_ms - _last_sweep_ms >= sweep_ms))
    {
        _last_sweep_ms = now_ms;
        sweep_leases();
    }
}

void rasn_registry_app::sweep_leases()
{
    const uint64_t lease_ms = registry_lease_ms();
    if (lease_ms == 0)
    {
        return;
    }

    size_t expired = 0;
    std::string error;
    if (!_registry.expire_leases(::dsn_now_ms(), lease_ms, &expired, &error))
    {
        dwarn("failed to sweep rASN registry leases: %s", error.c_str());
        return;
    }
    if (expired > 0)
    {
        dinfo("swept %llu expired rASN registry leases",
              static_cast<unsigned long long>(expired));
    }
}

} // namespace rasn
} // namespace dsn
