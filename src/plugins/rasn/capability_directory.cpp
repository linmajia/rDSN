#include <rasn/capability_directory.h>

#include <dsn/service_api_cpp.h>

#include <algorithm>
#include <sstream>

namespace dsn {
namespace rasn {

bool capability_directory::upsert_provider(capability_provider provider, std::string *error)
{
    if (provider.descriptor.agent_id.empty())
    {
        if (error != nullptr)
        {
            *error = "capability provider missing agent id";
        }
        return false;
    }
    if (provider.descriptor.capabilities.empty())
    {
        if (error != nullptr)
        {
            *error = "capability provider has no capabilities: " + provider.descriptor.agent_id;
        }
        return false;
    }
    if (provider.state.empty())
    {
        provider.state = "running";
    }
    if (provider.last_seen_ms == 0)
    {
        provider.last_seen_ms = ::dsn_now_ms();
    }

    ::dsn::service::zauto_lock guard(_lock);
    _providers[provider.descriptor.agent_id] = provider;
    return true;
}

bool capability_directory::remove_provider(const std::string &provider_id, std::string *error)
{
    ::dsn::service::zauto_lock guard(_lock);
    if (_providers.erase(provider_id) == 0)
    {
        if (error != nullptr)
        {
            *error = "unknown capability provider: " + provider_id;
        }
        return false;
    }
    return true;
}

bool capability_directory::find_provider(const std::string &provider_id, capability_provider *provider) const
{
    ::dsn::service::zauto_lock guard(_lock);
    const std::map<std::string, capability_provider>::const_iterator it = _providers.find(provider_id);
    if (it == _providers.end())
    {
        return false;
    }
    if (provider != nullptr)
    {
        *provider = it->second;
    }
    return true;
}

std::vector<capability_match> capability_directory::query(const capability_query &request) const
{
    ::dsn::service::zauto_lock guard(_lock);
    std::vector<capability_match> matches;
    for (const std::map<std::string, capability_provider>::value_type &entry : _providers)
    {
        const capability_provider &provider = entry.second;
        if (!provider_matches(provider, request))
        {
            continue;
        }
        for (const agent_capability &capability : provider.descriptor.capabilities)
        {
            if (!capability_matches(capability, request))
            {
                continue;
            }
            capability_match match;
            match.provider = provider;
            match.capability = capability;
            match.score = score_match(provider, capability);
            match.reason = "matched capability " + capability.name;
            matches.push_back(match);
        }
    }

    std::sort(matches.begin(), matches.end(), [](const capability_match &left, const capability_match &right) {
        if (left.score != right.score)
        {
            return left.score > right.score;
        }
        return left.provider.descriptor.agent_id < right.provider.descriptor.agent_id;
    });
    if (request.limit != 0 && matches.size() > request.limit)
    {
        matches.resize(request.limit);
    }
    return matches;
}

bool capability_directory::choose_best(const capability_query &request, capability_match *match, std::string *error) const
{
    capability_query first = request;
    first.limit = 1;
    const std::vector<capability_match> matches = query(first);
    if (matches.empty())
    {
        if (error != nullptr)
        {
            *error = "no provider matches capability: " + request.capability;
        }
        return false;
    }
    if (match != nullptr)
    {
        *match = matches[0];
    }
    return true;
}

std::vector<capability_provider> capability_directory::snapshot() const
{
    ::dsn::service::zauto_lock guard(_lock);
    std::vector<capability_provider> providers;
    providers.reserve(_providers.size());
    for (const std::map<std::string, capability_provider>::value_type &entry : _providers)
    {
        providers.push_back(entry.second);
    }
    return providers;
}

std::string capability_directory::describe() const
{
    const std::vector<capability_provider> providers = snapshot();
    size_t capabilities = 0;
    std::ostringstream output;
    output << "providers=" << providers.size();
    for (const capability_provider &provider : providers)
    {
        capabilities += provider.descriptor.capabilities.size();
        output << "\n" << provider.descriptor.agent_id
               << " role=" << provider.descriptor.role
               << " state=" << provider.state
               << " load=" << provider.load
               << " capabilities=" << provider.descriptor.capabilities.size();
    }
    output << "\ncapabilities=" << capabilities;
    return output.str();
}

bool capability_directory::provider_matches(const capability_provider &provider, const capability_query &request) const
{
    if (request.healthy_only && provider.descriptor.health != "healthy" && !provider.descriptor.health.empty())
    {
        return false;
    }
    if (provider.state == "failed" || provider.state == "stopped" || provider.state == "cancelled")
    {
        return false;
    }
    if (provider.load > request.max_load)
    {
        return false;
    }
    if (request.max_age_ms != 0 && request.now_ms != 0 &&
        (provider.last_seen_ms == 0 || provider.last_seen_ms + request.max_age_ms < request.now_ms))
    {
        return false;
    }
    return labels_include(provider.labels, request.required_labels);
}

bool capability_directory::capability_matches(const agent_capability &capability, const capability_query &request) const
{
    if (!request.capability.empty() && capability.name != request.capability)
    {
        return false;
    }
    if (!request.input_type.empty() && capability.input_type != request.input_type)
    {
        return false;
    }
    if (!request.side_effect_class.empty() && capability.side_effect_class != request.side_effect_class)
    {
        return false;
    }
    return true;
}

bool capability_directory::labels_include(const std::vector<std::string> &labels,
                                          const std::vector<std::string> &required) const
{
    for (const std::string &label : required)
    {
        if (std::find(labels.begin(), labels.end(), label) == labels.end())
        {
            return false;
        }
    }
    return true;
}

int64_t capability_directory::score_match(const capability_provider &provider, const agent_capability &capability) const
{
    int64_t score = static_cast<int64_t>(capability.reliability_hint) * 1000;
    score -= static_cast<int64_t>(capability.cost_hint) * 10;
    score -= static_cast<int64_t>(capability.latency_hint_ms / 10);
    score -= static_cast<int64_t>(provider.load);
    return score;
}

} // namespace rasn
} // namespace dsn

