#include <rasn/resource_budget.h>

#include <sstream>

namespace dsn {
namespace rasn {

bool resource_budget_manager::configure(const resource_quota &quota, std::string *error)
{
    if (quota.scope.empty())
    {
        if (error != nullptr)
        {
            *error = "resource quota missing scope";
        }
        return false;
    }
    ::dsn::service::zauto_lock guard(_lock);
    _quotas[quota.scope] = quota;
    if (_usage.find(quota.scope) == _usage.end())
    {
        resource_usage usage;
        usage.scope = quota.scope;
        _usage[quota.scope] = usage;
    }
    return true;
}

resource_budget_decision resource_budget_manager::reserve(const resource_request &request)
{
    resource_budget_decision decision;
    decision.scope = request.scope;
    if (request.scope.empty())
    {
        decision.reason = "resource request missing scope";
        return decision;
    }

    ::dsn::service::zauto_lock guard(_lock);
    resource_usage current = _usage[request.scope];
    current.scope = request.scope;
    resource_quota quota = _quotas[request.scope];
    quota.scope = request.scope;
    const resource_usage after = add(current, request);
    decision.usage_after = after;
    decision.quota = quota;

    if (exceeds(current.cost_units, request.cost_units, quota.max_cost_units))
    {
        decision.reason = "cost budget exceeded for " + request.scope;
        return decision;
    }
    if (exceeds(current.latency_ms, request.latency_ms, quota.max_latency_ms))
    {
        decision.reason = "latency budget exceeded for " + request.scope;
        return decision;
    }
    if (exceeds(current.tokens, request.tokens, quota.max_tokens))
    {
        decision.reason = "token budget exceeded for " + request.scope;
        return decision;
    }
    if (exceeds(current.tool_calls, request.tool_calls, quota.max_tool_calls))
    {
        decision.reason = "tool-call budget exceeded for " + request.scope;
        return decision;
    }

    _usage[request.scope] = after;
    decision.allowed = true;
    decision.reason = request.reason;
    return decision;
}

bool resource_budget_manager::release(const resource_request &request, std::string *error)
{
    if (request.scope.empty())
    {
        if (error != nullptr)
        {
            *error = "resource release missing scope";
        }
        return false;
    }
    ::dsn::service::zauto_lock guard(_lock);
    resource_usage &usage = _usage[request.scope];
    usage.scope = request.scope;
    subtract(request.cost_units, &usage.cost_units);
    subtract(request.latency_ms, &usage.latency_ms);
    subtract(request.tokens, &usage.tokens);
    subtract(request.tool_calls, &usage.tool_calls);
    return true;
}

bool resource_budget_manager::usage(const std::string &scope, resource_usage *usage) const
{
    ::dsn::service::zauto_lock guard(_lock);
    const std::map<std::string, resource_usage>::const_iterator it = _usage.find(scope);
    if (it == _usage.end())
    {
        return false;
    }
    if (usage != nullptr)
    {
        *usage = it->second;
    }
    return true;
}

std::vector<resource_usage> resource_budget_manager::snapshot() const
{
    ::dsn::service::zauto_lock guard(_lock);
    std::vector<resource_usage> result;
    result.reserve(_usage.size());
    for (const std::map<std::string, resource_usage>::value_type &entry : _usage)
    {
        result.push_back(entry.second);
    }
    return result;
}

std::string resource_budget_manager::describe() const
{
    ::dsn::service::zauto_lock guard(_lock);
    std::ostringstream output;
    output << "scopes=" << _usage.size();
    for (const std::map<std::string, resource_usage>::value_type &entry : _usage)
    {
        const resource_usage &usage = entry.second;
        output << "\n" << usage.scope
               << " cost=" << usage.cost_units
               << " latency_ms=" << usage.latency_ms
               << " tokens=" << usage.tokens
               << " tool_calls=" << usage.tool_calls;
    }
    return output.str();
}

bool resource_budget_manager::exceeds(uint64_t current, uint64_t requested, uint64_t limit) const
{
    if (limit == 0)
    {
        return false;
    }
    if (requested > limit)
    {
        return true;
    }
    return current > limit - requested;
}

resource_usage resource_budget_manager::add(const resource_usage &usage, const resource_request &request) const
{
    resource_usage after = usage;
    after.cost_units += request.cost_units;
    after.latency_ms += request.latency_ms;
    after.tokens += request.tokens;
    after.tool_calls += request.tool_calls;
    return after;
}

void resource_budget_manager::subtract(uint64_t amount, uint64_t *value) const
{
    if (value == nullptr)
    {
        return;
    }
    *value = amount > *value ? 0 : *value - amount;
}

} // namespace rasn
} // namespace dsn
