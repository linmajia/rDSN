#include <rasn/agent_clients.h>

namespace dsn {
namespace rasn {

std::chrono::milliseconds default_rpc_timeout()
{
    const uint64_t timeout_ms =
        ::dsn_config_get_value_uint64("rasn.rpc", "timeout_ms", 5000, "Default rASN RPC timeout in milliseconds");
    return std::chrono::milliseconds(timeout_ms);
}

std::chrono::milliseconds request_rpc_timeout(const agent_request &request)
{
    if (request.timeout_ms != 0)
    {
        return std::chrono::milliseconds(request.timeout_ms);
    }
    return default_rpc_timeout();
}

std::pair<::dsn::error_code, agent_descriptor>
rasn_agent_client::describe_sync(const std::string &request,
                                 std::chrono::milliseconds timeout,
                                 int thread_hash,
                                 uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<agent_descriptor>(::dsn::rpc::call(
        _server, RPC_RASN_AGENT_DESCRIBE, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

std::pair<::dsn::error_code, agent_response>
rasn_agent_client::invoke_sync(const agent_request &request,
                               std::chrono::milliseconds timeout,
                               int thread_hash,
                               uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<agent_response>(::dsn::rpc::call(
        _server, RPC_RASN_AGENT_INVOKE, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

std::pair<::dsn::error_code, agent_response>
rasn_agent_client::cancel_sync(const agent_request &request,
                               std::chrono::milliseconds timeout,
                               int thread_hash,
                               uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<agent_response>(::dsn::rpc::call(
        _server, RPC_RASN_AGENT_CANCEL, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

std::pair<::dsn::error_code, agent_descriptor>
rasn_agent_client::heartbeat_sync(const std::string &request,
                                  std::chrono::milliseconds timeout,
                                  int thread_hash,
                                  uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<agent_descriptor>(::dsn::rpc::call(
        _server, RPC_RASN_AGENT_HEARTBEAT, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

std::pair<::dsn::error_code, agent_descriptor>
rasn_agent_client::query_sync(const std::string &request,
                              std::chrono::milliseconds timeout,
                              int thread_hash,
                              uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<agent_descriptor>(::dsn::rpc::call(
        _server, RPC_RASN_AGENT_QUERY, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

} // namespace rasn
} // namespace dsn
