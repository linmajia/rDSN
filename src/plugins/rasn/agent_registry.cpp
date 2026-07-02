#include "agent_registry.h"

#include <dsn/service_api_cpp.h>

#include <algorithm>
#include <sstream>

namespace dsn {
namespace rasn {

namespace {

agent_response registry_response(const std::string &request_id, bool ok, const std::string &error)
{
    agent_response response;
    response.request_id = request_id;
    response.ok = ok;
    if (!ok)
    {
        response.error = make_agent_error("registry", "registry_error", error, false, "rasn.registry");
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

std::vector<std::string> split_config_csv(const std::string &value)
{
    std::vector<std::string> result;
    std::stringstream input(value);
    std::string item;
    while (std::getline(input, item, ','))
    {
        std::string normalized = trim(item);
        if (!normalized.empty())
        {
            result.push_back(normalized);
        }
    }
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
    int total_sections = ::dsn_config_get_all_sections(sections, &used_sections);
    if (total_sections > used_sections)
    {
        dwarn("rasn registry config has %d sections, but only first %d are scanned", total_sections, used_sections);
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

} // namespace

bool agent_registry::register_agent(const agent_descriptor &descriptor, std::string *error)
{
    return register_agent(descriptor, error, false);
}

bool agent_registry::register_agent(const agent_descriptor &descriptor, std::string *error, bool lease_tracked)
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
    ::dsn::service::zauto_lock guard(_lock);
    const size_t removed = _agents.erase(agent_id);
    if (removed > 0)
    {
        dinfo("unregistered rASN agent id=%s", agent_id.c_str());
    }
    return removed > 0;
}

std::vector<agent_descriptor> agent_registry::list_agents() const
{
    return list_agents(true);
}

std::vector<agent_descriptor> agent_registry::list_agents(bool healthy_only) const
{
    ::dsn::service::zauto_lock guard(_lock);
    std::vector<agent_descriptor> result;
    result.reserve(_agents.size());
    const uint64_t now_ms = ::dsn_now_ms();
    const uint64_t lease_ms = registry_lease_ms();
    for (const std::map<std::string, registry_entry>::value_type &entry : _agents)
    {
        if (healthy_only && (!is_healthy_descriptor(entry.second.descriptor) || !is_live_entry(entry.second, now_ms, lease_ms)))
        {
            continue;
        }
        result.push_back(entry.second.descriptor);
    }
    return result;
}

std::vector<agent_descriptor> agent_registry::query_by_capability(const std::string &capability) const
{
    return query_by_capability(capability, true);
}

std::vector<agent_descriptor> agent_registry::query_by_capability(const std::string &capability, bool healthy_only) const
{
    ::dsn::service::zauto_lock guard(_lock);
    std::vector<agent_descriptor> result;
    const uint64_t now_ms = ::dsn_now_ms();
    const uint64_t lease_ms = registry_lease_ms();
    for (const std::map<std::string, registry_entry>::value_type &entry : _agents)
    {
        if (healthy_only && (!is_healthy_descriptor(entry.second.descriptor) || !is_live_entry(entry.second, now_ms, lease_ms)))
        {
            continue;
        }
        if (has_capability(entry.second.descriptor, capability))
        {
            result.push_back(entry.second.descriptor);
        }
    }
    return result;
}

bool agent_registry::find_agent(const std::string &agent_id, agent_descriptor *descriptor) const
{
    ::dsn::service::zauto_lock guard(_lock);
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
    std::map<std::string, registry_entry>::iterator it = _agents.find(descriptor.agent_id);
    if (it == _agents.end())
    {
        if (error != nullptr)
        {
            *error = "heartbeat for unknown agent: " + descriptor.agent_id;
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
    if (lease_ms == 0)
    {
        return 0;
    }

    ::dsn::service::zauto_lock guard(_lock);
    size_t expired = 0;
    for (std::map<std::string, registry_entry>::iterator it = _agents.begin(); it != _agents.end();)
    {
        if (!is_live_entry(it->second, now_ms, lease_ms))
        {
            dinfo("expired rASN agent lease id=%s", it->second.descriptor.agent_id.c_str());
            it = _agents.erase(it);
            ++expired;
        }
        else
        {
            ++it;
        }
    }
    return expired;
}

std::string agent_registry::describe() const
{
    const std::vector<agent_descriptor> agents = list_agents(false);
    std::ostringstream oss;
    oss << "registered agents:\n";
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

bool agent_registry::has_capability(const agent_descriptor &descriptor, const std::string &capability) const
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

bool agent_registry::is_live_entry(const registry_entry &entry, uint64_t now_ms, uint64_t lease_ms) const
{
    if (!entry.lease_tracked || lease_ms == 0)
    {
        return true;
    }
    return now_ms <= entry.last_heartbeat_ms + lease_ms;
}

agent_registry &global_agent_registry()
{
    static agent_registry registry;
    return registry;
}

void load_static_agents_from_config_once()
{
    ::dsn::service::zauto_lock guard(static_registry_config_lock());
    if (static_registry_config_loaded())
    {
        return;
    }
    static_registry_config_loaded() = true;

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
        descriptor.role = ::dsn_config_get_value_string(section.c_str(), "role", "custom", "");
        descriptor.app_name =
            ::dsn_config_get_value_string(section.c_str(), "app_name", descriptor.agent_id.c_str(), "");
        descriptor.endpoint_uri = ::dsn_config_get_value_string(section.c_str(), "uri", "", "");
        descriptor.endpoint_uri =
            ::dsn_config_get_value_string(section.c_str(), "endpoint_uri", descriptor.endpoint_uri.c_str(), "");
        descriptor.host =
            ::dsn_config_get_value_string(section.c_str(), "host", descriptor.endpoint_uri.empty() ? "localhost" : "", "");
        descriptor.port =
            static_cast<uint32_t>(::dsn_config_get_value_uint64(section.c_str(), "port", 0, ""));
        descriptor.version = ::dsn_config_get_value_string(section.c_str(), "version", "static-config", "");
        descriptor.health = ::dsn_config_get_value_string(section.c_str(), "health", "unknown", "");

        const std::string side_effect_class =
            ::dsn_config_get_value_string(section.c_str(), "side_effect_class", "none", "");
        const std::string capabilities =
            ::dsn_config_get_value_string(section.c_str(), "capabilities", "", "");
        for (const std::string &capability : split_config_csv(capabilities))
        {
            descriptor.capabilities.push_back(make_static_capability(capability, side_effect_class));
        }

        std::string error;
        if (!global_agent_registry().register_agent(descriptor, &error))
        {
            dwarn("failed to load static rasn agent %s from [%s]: %s",
                  descriptor.agent_id.c_str(),
                  section.c_str(),
                  error.c_str());
        }
    }
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

void rasn_registry_rpc_service::on_register(const agent_descriptor &request,
                                            ::dsn::rpc_replier<agent_response> &reply)
{
    if (request.schema_version == 0)
    {
        reply(registry_response(request.agent_id, false, "missing registry register schema version"));
        return;
    }
    if (request.agent_id.empty())
    {
        reply(registry_response(request.agent_id, false, "missing registry register agent id"));
        return;
    }
    std::string error;
    const bool ok = global_agent_registry().register_agent(request, &error, true);
    reply(registry_response(request.agent_id, ok, error));
}

void rasn_registry_rpc_service::on_unregister(const std::string &agent_id,
                                              ::dsn::rpc_replier<agent_response> &reply)
{
    if (agent_id.empty())
    {
        reply(registry_response(agent_id, false, "missing registry unregister agent id"));
        return;
    }
    const bool ok = global_agent_registry().unregister_agent(agent_id);
    reply(registry_response(agent_id, ok, ok ? "" : "unknown agent: " + agent_id));
}

void rasn_registry_rpc_service::on_query(const registry_query_request &request,
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
    response.agents = global_agent_registry().query_by_capability(request.capability, request.healthy_only);
    reply(response);
}

void rasn_registry_rpc_service::on_list(const std::string &request,
                                        ::dsn::rpc_replier<registry_query_response> &reply)
{
    registry_query_response response;
    response.agents = global_agent_registry().list_agents(true);
    reply(response);
}

void rasn_registry_rpc_service::on_heartbeat(const agent_descriptor &request,
                                             ::dsn::rpc_replier<agent_response> &reply)
{
    if (request.schema_version == 0)
    {
        reply(registry_response(request.agent_id, false, "missing registry heartbeat schema version"));
        return;
    }
    std::string error;
    const bool ok = global_agent_registry().heartbeat(request, &error);
    reply(registry_response(request.agent_id, ok, error));
}

std::pair<::dsn::error_code, agent_response>
rasn_registry_client::register_sync(const agent_descriptor &request,
                                    std::chrono::milliseconds timeout,
                                    int thread_hash,
                                    uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<agent_response>(::dsn::rpc::call(
        _server, RPC_RASN_REGISTRY_REGISTER, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

std::pair<::dsn::error_code, agent_response>
rasn_registry_client::unregister_sync(const std::string &agent_id,
                                      std::chrono::milliseconds timeout,
                                      int thread_hash,
                                      uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<agent_response>(::dsn::rpc::call(
        _server, RPC_RASN_REGISTRY_UNREGISTER, agent_id, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

std::pair<::dsn::error_code, registry_query_response>
rasn_registry_client::query_sync(const registry_query_request &request,
                                 std::chrono::milliseconds timeout,
                                 int thread_hash,
                                 uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<registry_query_response>(::dsn::rpc::call(
        _server, RPC_RASN_REGISTRY_QUERY, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

std::pair<::dsn::error_code, registry_query_response>
rasn_registry_client::list_sync(const std::string &request,
                                std::chrono::milliseconds timeout,
                                int thread_hash,
                                uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<registry_query_response>(::dsn::rpc::call(
        _server, RPC_RASN_REGISTRY_LIST, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

std::pair<::dsn::error_code, agent_response>
rasn_registry_client::heartbeat_sync(const agent_descriptor &request,
                                     std::chrono::milliseconds timeout,
                                     int thread_hash,
                                     uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<agent_response>(::dsn::rpc::call(
        _server, RPC_RASN_REGISTRY_HEARTBEAT, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

::dsn::error_code rasn_registry_app::start(int argc, char **argv)
{
    load_static_agents_from_config_once();
    _rpc.open_service();
    start_lease_sweep_timer();
    return ::dsn::ERR_OK;
}

::dsn::error_code rasn_registry_app::stop(bool cleanup)
{
    cancel_lease_sweep_timer();
    _rpc.close_service();
    return ::dsn::ERR_OK;
}

void rasn_registry_app::start_lease_sweep_timer()
{
    if (_lease_sweep_timer != nullptr)
    {
        return;
    }

    const uint64_t sweep_ms = registry_lease_sweep_ms();
    if (sweep_ms == 0)
    {
        return;
    }

    const std::chrono::milliseconds interval(sweep_ms);
    _lease_sweep_timer = ::dsn::tasking::enqueue_timer(
        LPC_RASN_REGISTRY_LEASE_SWEEP_TIMER,
        nullptr,
        [this]() { sweep_leases(); },
        interval,
        0,
        interval);
    if (_lease_sweep_timer == nullptr)
    {
        dwarn("failed to start rASN registry lease sweep timer");
    }
}

void rasn_registry_app::cancel_lease_sweep_timer()
{
    if (_lease_sweep_timer != nullptr)
    {
        _lease_sweep_timer->cancel(true);
        _lease_sweep_timer = nullptr;
    }
}

void rasn_registry_app::sweep_leases()
{
    const uint64_t lease_ms = registry_lease_ms();
    if (lease_ms == 0)
    {
        return;
    }

    const size_t expired = global_agent_registry().expire_leases(::dsn_now_ms(), lease_ms);
    if (expired > 0)
    {
        dinfo("swept %llu expired rASN registry leases", static_cast<unsigned long long>(expired));
    }
}

} // namespace rasn
} // namespace dsn
