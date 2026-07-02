#include <rasn/agent_control_plane.h>

#include <dsn/service_api_cpp.h>

#include <sstream>

namespace dsn {
namespace rasn {

namespace {

bool terminal_agent_state(const std::string &state)
{
    return state == "stopped" || state == "failed" || state == "cancelled";
}

} // namespace

bool agent_control_plane::upsert_agent(const agent_control_record &record, std::string *error)
{
    if (record.descriptor.agent_id.empty())
    {
        if (error != nullptr)
        {
            *error = "agent control record missing agent id";
        }
        return false;
    }
    if (record.descriptor.role.empty())
    {
        if (error != nullptr)
        {
            *error = "agent control record missing role";
        }
        return false;
    }

    ::dsn::service::zauto_lock guard(_lock);
    agent_control_record next = record;
    agent_control_record &slot = _agents[next.descriptor.agent_id];
    next.generation = slot.generation + 1;
    if (next.state.empty())
    {
        next.state = slot.state.empty() ? "starting" : slot.state;
    }
    if (next.restart_policy.empty())
    {
        next.restart_policy = slot.restart_policy.empty() ? "never" : slot.restart_policy;
    }
    if (next.last_heartbeat_ms == 0)
    {
        next.last_heartbeat_ms = ::dsn_now_ms();
    }
    slot = next;
    return true;
}

bool agent_control_plane::transition(const std::string &agent_id,
                                     const std::string &state,
                                     const std::string &last_error,
                                     std::string *error)
{
    if (state.empty())
    {
        if (error != nullptr)
        {
            *error = "agent state cannot be empty";
        }
        return false;
    }

    ::dsn::service::zauto_lock guard(_lock);
    std::map<std::string, agent_control_record>::iterator it = _agents.find(agent_id);
    if (it == _agents.end())
    {
        if (error != nullptr)
        {
            *error = "unknown agent: " + agent_id;
        }
        return false;
    }
    it->second.state = state;
    it->second.last_error = last_error;
    ++it->second.generation;
    if (terminal_agent_state(state))
    {
        it->second.owner.clear();
        it->second.lease_expires_ms = 0;
    }
    return true;
}

agent_control_lease agent_control_plane::acquire_lease(const std::string &agent_id,
                                                       const std::string &owner,
                                                       uint64_t now_ms,
                                                       uint64_t lease_ms)
{
    agent_control_lease lease;
    lease.agent_id = agent_id;
    lease.owner = owner;
    if (owner.empty())
    {
        lease.error = "lease owner cannot be empty";
        return lease;
    }

    ::dsn::service::zauto_lock guard(_lock);
    std::map<std::string, agent_control_record>::iterator it = _agents.find(agent_id);
    if (it == _agents.end())
    {
        lease.error = "unknown agent: " + agent_id;
        return lease;
    }
    agent_control_record &record = it->second;
    if (!record.owner.empty() && record.owner != owner &&
        (record.lease_expires_ms == 0 || record.lease_expires_ms > now_ms))
    {
        lease.error = "agent lease is held by " + record.owner;
        return lease;
    }
    record.owner = owner;
    record.lease_expires_ms = lease_ms == 0 ? 0 : now_ms + lease_ms;
    ++record.generation;

    lease.ok = true;
    lease.generation = record.generation;
    lease.expires_ms = record.lease_expires_ms;
    return lease;
}

bool agent_control_plane::release_lease(const std::string &agent_id, const std::string &owner, std::string *error)
{
    ::dsn::service::zauto_lock guard(_lock);
    std::map<std::string, agent_control_record>::iterator it = _agents.find(agent_id);
    if (it == _agents.end())
    {
        if (error != nullptr)
        {
            *error = "unknown agent: " + agent_id;
        }
        return false;
    }
    if (!it->second.owner.empty() && it->second.owner != owner)
    {
        if (error != nullptr)
        {
            *error = "agent lease is held by " + it->second.owner;
        }
        return false;
    }
    it->second.owner.clear();
    it->second.lease_expires_ms = 0;
    ++it->second.generation;
    return true;
}

bool agent_control_plane::heartbeat(const std::string &agent_id, uint64_t now_ms, std::string *error)
{
    ::dsn::service::zauto_lock guard(_lock);
    std::map<std::string, agent_control_record>::iterator it = _agents.find(agent_id);
    if (it == _agents.end())
    {
        if (error != nullptr)
        {
            *error = "unknown agent: " + agent_id;
        }
        return false;
    }
    it->second.last_heartbeat_ms = now_ms;
    if (it->second.state == "starting")
    {
        it->second.state = "running";
    }
    return true;
}

size_t agent_control_plane::expire_leases(uint64_t now_ms)
{
    ::dsn::service::zauto_lock guard(_lock);
    size_t expired = 0;
    for (std::map<std::string, agent_control_record>::value_type &entry : _agents)
    {
        agent_control_record &record = entry.second;
        if (!record.owner.empty() && record.lease_expires_ms != 0 && record.lease_expires_ms <= now_ms)
        {
            record.owner.clear();
            record.lease_expires_ms = 0;
            ++record.generation;
            ++expired;
        }
    }
    return expired;
}

bool agent_control_plane::find(const std::string &agent_id, agent_control_record *record) const
{
    ::dsn::service::zauto_lock guard(_lock);
    const std::map<std::string, agent_control_record>::const_iterator it = _agents.find(agent_id);
    if (it == _agents.end())
    {
        return false;
    }
    if (record != nullptr)
    {
        *record = it->second;
    }
    return true;
}

std::vector<agent_control_record> agent_control_plane::list(bool live_only, uint64_t now_ms) const
{
    ::dsn::service::zauto_lock guard(_lock);
    std::vector<agent_control_record> result;
    for (const std::map<std::string, agent_control_record>::value_type &entry : _agents)
    {
        if (!live_only || is_live(entry.second, now_ms))
        {
            result.push_back(entry.second);
        }
    }
    return result;
}

std::vector<agent_control_record> agent_control_plane::query_by_capability(const std::string &capability,
                                                                           bool live_only,
                                                                           uint64_t now_ms) const
{
    ::dsn::service::zauto_lock guard(_lock);
    std::vector<agent_control_record> result;
    for (const std::map<std::string, agent_control_record>::value_type &entry : _agents)
    {
        if ((!live_only || is_live(entry.second, now_ms)) && has_capability(entry.second.descriptor, capability))
        {
            result.push_back(entry.second);
        }
    }
    return result;
}

std::string agent_control_plane::describe(uint64_t now_ms) const
{
    const std::vector<agent_control_record> records = list(false, now_ms);
    std::ostringstream output;
    output << "agents=" << records.size();
    for (const agent_control_record &record : records)
    {
        output << "\n" << record.descriptor.agent_id
               << " role=" << record.descriptor.role
               << " state=" << record.state
               << " owner=" << (record.owner.empty() ? "<none>" : record.owner)
               << " generation=" << record.generation;
    }
    return output.str();
}

bool agent_control_plane::has_capability(const agent_descriptor &descriptor, const std::string &capability) const
{
    if (capability.empty())
    {
        return true;
    }
    for (const agent_capability &candidate : descriptor.capabilities)
    {
        if (candidate.name == capability)
        {
            return true;
        }
    }
    return false;
}

bool agent_control_plane::is_live(const agent_control_record &record, uint64_t now_ms) const
{
    if (terminal_agent_state(record.state))
    {
        return false;
    }
    if (record.lease_expires_ms != 0 && now_ms != 0 && record.lease_expires_ms <= now_ms)
    {
        return false;
    }
    return record.descriptor.health.empty() || record.descriptor.health == "healthy";
}

} // namespace rasn
} // namespace dsn
