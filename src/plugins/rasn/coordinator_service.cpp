#include "coordinator_service.h"

#include "agent_clients.h"

#include <algorithm>
#include <limits>
#include <tuple>

namespace dsn {
namespace rasn {

namespace {

uint32_t coordinator_max_retry_budget()
{
    return static_cast<uint32_t>(std::min<uint64_t>(
        ::dsn_config_get_value_uint64(
            "rasn.coordinator", "max_retry_budget", 3, "maximum rASN coordinator retries for one request"),
        static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())));
}

std::string retry_reason(const agent_response &response)
{
    if (!response.error.message.empty())
    {
        return response.error.message;
    }
    if (!response.error.code.empty())
    {
        return response.error.code;
    }
    return "retryable agent failure";
}

std::string retry_failure_class(const agent_response &response)
{
    return response.error.failure_class.empty() ? "agent" : response.error.failure_class;
}

std::string retry_failure_code(const agent_response &response)
{
    return response.error.code.empty() ? "agent_invoke_failed" : response.error.code;
}

std::string retry_failure_source(const agent_response &response)
{
    return response.error.source.empty() ? "rasn.coordinator" : response.error.source;
}

} // namespace

bool coordinator_router::is_tool_capability(const std::string &capability)
{
    return capability == "tool.run" || capability.find("tool.") == 0;
}

std::string coordinator_router::routed_capability(const std::string &capability)
{
    return is_tool_capability(capability) ? "tool.run" : capability;
}

coordinator_route coordinator_router::resolve(const agent_request &request,
                                              bool use_registry_rpc,
                                              const ::dsn::rpc_address &registry_address)
{
    const std::string capability = routed_capability(request.capability);
    std::vector<agent_descriptor> candidates;
    if (use_registry_rpc)
    {
        rasn_registry_client registry(registry_address);
        registry_query_request query;
        query.capability = capability;
        query.healthy_only = true;
        ::dsn::error_code err;
        registry_query_response response;
        std::tie(err, response) = registry.query_sync(query, request_rpc_timeout(request));
        if (err != ::dsn::ERR_OK)
        {
            return route_error(request,
                               "registry",
                               "registry_query_failed",
                               std::string("RPC_RASN_REGISTRY_QUERY failed: ") + err.to_string(),
                               true);
        }
        if (!response.ok)
        {
            return route_error(request, "registry", "registry_query_failed", response.error, false);
        }
        candidates = response.agents;
    }
    else
    {
        candidates = global_agent_registry().query_by_capability(capability, true);
    }

    return select_first(request, capability, candidates);
}

agent_response coordinator_router::invoke_remote(const agent_request &request,
                                                 const agent_descriptor &agent,
                                                 const std::string &source)
{
    std::string address_error;
    const ::dsn::rpc_address address = address_from_descriptor(agent, &address_error);
    if (!address_error.empty())
    {
        agent_response response;
        response.request_id = request.request_id;
        response.trace_id = request.trace_id;
        response.ok = false;
        response.error = make_agent_error("routing", "invalid_agent_endpoint", address_error, false, source);
        return response;
    }

    rasn_agent_client client(address);
    ::dsn::error_code err;
    agent_response response;
    std::tie(err, response) = client.invoke_sync(request, request_rpc_timeout(request));
    if (err != ::dsn::ERR_OK)
    {
        agent_response failure;
        failure.request_id = request.request_id;
        failure.trace_id = request.trace_id;
        failure.ok = false;
        failure.error = make_agent_error("rpc",
                                         "agent_invoke_failed",
                                         std::string("RPC_RASN_AGENT_INVOKE failed for ") + agent.agent_id +
                                             ": " + err.to_string(),
                                         true,
                                         source);
        return failure;
    }
    return response;
}

agent_response coordinator_router::invoke_with_retries(
    const agent_request &request,
    nucleus_runtime &runtime,
    const agent_descriptor &agent,
    const std::string &operation,
    const std::function<agent_response(uint32_t retry_attempt)> &invoke_once)
{
    const uint32_t retry_budget = std::min(request.retry_budget, coordinator_max_retry_budget());
    const bool retry_allowed = retry_budget > 0 && !is_tool_capability(request.capability);
    for (uint32_t attempt = 0;; ++attempt)
    {
        agent_response response = invoke_once(attempt);
        if (response.ok)
        {
            if (attempt > 0)
            {
                dinfo("rASN coordinator request=%s agent=%s succeeded after retry attempt=%u",
                      request.request_id.c_str(),
                      agent.agent_id.c_str(),
                      static_cast<unsigned int>(attempt));
            }
            return response;
        }

        if (response.error.retryable || attempt > 0)
        {
            runtime.record_failure(request.task,
                                   retry_failure_class(response),
                                   retry_failure_code(response),
                                   retry_reason(response),
                                   response.error.retryable,
                                   retry_failure_source(response),
                                   attempt);
        }

        if (!retry_allowed || !response.error.retryable || attempt >= retry_budget)
        {
            return response;
        }

        runtime.record_retry(request.task,
                             operation + ":" + agent.agent_id,
                             attempt + 1,
                             retry_reason(response));
    }
}

coordinator_route coordinator_router::route_error(const agent_request &request,
                                                  const std::string &failure_class,
                                                  const std::string &code,
                                                  const std::string &message,
                                                  bool retryable)
{
    coordinator_route route;
    route.ok = false;
    route.error.request_id = request.request_id;
    route.error.trace_id = request.trace_id;
    route.error.ok = false;
    route.error.error = make_agent_error(failure_class, code, message, retryable, "rasn.coordinator");
    return route;
}

::dsn::rpc_address coordinator_router::address_from_descriptor(const agent_descriptor &agent, std::string *error)
{
    if (!agent.endpoint_uri.empty())
    {
        ::dsn::url_host_address address(agent.endpoint_uri.c_str());
        if (address.is_invalid())
        {
            if (error != nullptr)
            {
                *error = "agent descriptor has invalid endpoint uri: " + agent.agent_id;
            }
            return ::dsn::rpc_address();
        }
        return address;
    }

    ::dsn::rpc_address address;
    if (agent.host.empty() || agent.port == 0)
    {
        if (error != nullptr)
        {
            *error = "agent descriptor has no endpoint: " + agent.agent_id;
        }
        return address;
    }

    address.assign_ipv4(agent.host.c_str(), static_cast<uint16_t>(agent.port));
    return address;
}

coordinator_route coordinator_router::select_first(const agent_request &request,
                                                   const std::string &capability,
                                                   const std::vector<agent_descriptor> &candidates)
{
    if (candidates.empty())
    {
        return route_error(request,
                           "routing",
                           "no_capability_route",
                           "no registered agent supports capability: " + request.capability,
                           false);
    }

    coordinator_route route;
    route.ok = true;
    route.capability = capability;
    route.agent = candidates.front();
    return route;
}

} // namespace rasn
} // namespace dsn
