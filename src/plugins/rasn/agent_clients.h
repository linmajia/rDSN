#pragma once

#include <rasn/agent_types.h>
#include <rasn/rasn.code.definition.h>

#include <dsn/service_api_cpp.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace dsn {
namespace rasn {

std::chrono::milliseconds default_rpc_timeout();
std::chrono::milliseconds request_rpc_timeout(const agent_request &request);

class rasn_agent_client : public virtual ::dsn::clientlet
{
public:
    explicit rasn_agent_client(::dsn::rpc_address server) : _server(server) {}

    std::pair< ::dsn::error_code, agent_descriptor>
    describe_sync(const std::string &request,
                  std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                  int thread_hash = 0,
                  uint64_t partition_hash = 0);

    std::pair< ::dsn::error_code, agent_response>
    invoke_sync(const agent_request &request,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                int thread_hash = 0,
                uint64_t partition_hash = 0);

    std::pair< ::dsn::error_code, agent_response>
    cancel_sync(const agent_request &request,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                int thread_hash = 0,
                uint64_t partition_hash = 0);

    std::pair< ::dsn::error_code, agent_descriptor>
    heartbeat_sync(const std::string &request,
                   std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                   int thread_hash = 0,
                   uint64_t partition_hash = 0);

    std::pair< ::dsn::error_code, agent_descriptor>
    query_sync(const std::string &request,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
               int thread_hash = 0,
               uint64_t partition_hash = 0);

private:
    ::dsn::rpc_address _server;
};

} // namespace rasn
} // namespace dsn
