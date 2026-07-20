#pragma once

#include <rasn/agent_registry.h>
#include <rasn/agent_types.h>

#include <dsn/service_api_cpp.h>

#include <functional>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

struct coordinator_route
{
    bool ok = false;
    std::string capability;
    agent_descriptor agent;
    agent_response error;
};

class coordinator_router
{
public:
    static bool is_tool_capability(const std::string &capability);
    static std::string routed_capability(const std::string &capability);

    static coordinator_route resolve(const agent_request &request,
                                     bool use_registry_rpc,
                                     const ::dsn::rpc_address &registry_address);

    static bool validate_remote_endpoint(const agent_descriptor &agent,
                                         ::dsn::rpc_address *address,
                                         std::string *error);
    static agent_response invoke_remote(const agent_request &request,
                                        const agent_descriptor &agent,
                                        const std::string &source);
    // Overload that dispatches to an already-resolved endpoint, avoiding a
    // second descriptor resolution when the caller has preflighted the address
    // via validate_remote_endpoint().
    static agent_response invoke_remote(const agent_request &request,
                                        const agent_descriptor &agent,
                                        const ::dsn::rpc_address &address,
                                        const std::string &source);
    static agent_response invoke_with_retries(const agent_request &request,
                                              nucleus_runtime &runtime,
                                              const agent_descriptor &agent,
                                              const std::string &operation,
                                              const std::function<agent_response(
                                                  uint32_t retry_attempt,
                                                  const agent_response *previous)> &invoke_once);

private:
    static coordinator_route route_error(const agent_request &request,
                                         const std::string &failure_class,
                                         const std::string &code,
                                         const std::string &message,
                                         bool retryable);
    static ::dsn::rpc_address address_from_descriptor(const agent_descriptor &agent, std::string *error);
    static coordinator_route select_first(const agent_request &request,
                                          const std::string &capability,
                                          const std::vector<agent_descriptor> &candidates);
};

} // namespace rasn
} // namespace dsn
