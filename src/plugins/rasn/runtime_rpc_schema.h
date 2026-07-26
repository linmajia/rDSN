#pragma once

#include <rasn/agent_control_plane.h>
#include <rasn/agent_message_bus.h>
#include <rasn/blackboard.h>
#include <rasn/capability_directory.h>
#include <rasn/contract_verifier.h>
#include <rasn/determinism_ledger.h>
#include <rasn/human_interaction.h>
#include <rasn/rasn_runtime.types.h>
#include <rasn/recovery_supervisor.h>
#include <rasn/resource_budget.h>
#include <rasn/sandbox_runtime.h>
#include <rasn/task_orchestration.h>

#include <dsn/cpp/blob.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

namespace dsn {
namespace rasn {

constexpr int32_t RASN_RUNTIME_WIRE_VERSION = 1;
constexpr int32_t RASN_RUNTIME_MIN_COMPATIBLE_VERSION = 1;
constexpr int64_t RASN_RUNTIME_NO_ROUTE = -1;

::dsn::rasn::rpc::runtime_request_metadata make_runtime_request_metadata();

template <typename Signed, typename Unsigned>
Signed encode_runtime_unsigned(Unsigned value)
{
    static_assert(std::is_integral<Signed>::value && std::is_signed<Signed>::value,
                  "runtime wire type must be a signed integer");
    static_assert(std::is_integral<Unsigned>::value && std::is_unsigned<Unsigned>::value,
                  "runtime domain type must be an unsigned integer");
    static_assert(sizeof(Unsigned) <= sizeof(Signed),
                  "runtime wire integer cannot represent the domain width");
    using wire_unsigned = typename std::make_unsigned<Signed>::type;
    const wire_unsigned bits = static_cast<wire_unsigned>(value);
    Signed encoded = 0;
    std::memcpy(&encoded, &bits, sizeof(encoded));
    return encoded;
}

template <typename Request, typename Response>
Response make_runtime_response(const Request &request)
{
    Response response;
    response.operation = request.operation;
    response.metadata.wire_version = RASN_RUNTIME_WIRE_VERSION;
    response.metadata.min_compatible_version = RASN_RUNTIME_MIN_COMPATIBLE_VERSION;
    if (request.metadata.__isset.route_partition)
    {
        response.metadata.__set_route_partition(request.metadata.route_partition);
    }
    if (request.metadata.__isset.trace_id)
    {
        response.metadata.__set_trace_id(request.metadata.trace_id);
    }
    response.status.ok = true;
    response.status.code = ::dsn::rasn::rpc::runtime_error_code::none;
    response.status.retryable = false;
    return response;
}

template <typename Response>
void set_runtime_error(Response *response,
                       ::dsn::rasn::rpc::runtime_error_code::type code,
                       const std::string &message,
                       bool retryable = false)
{
    response->status.ok = false;
    response->status.code = code;
    response->status.__set_message(message);
    response->status.retryable = retryable;
}

// A validation failure that names the wire version is a negotiation failure the
// peer cannot fix by correcting its payload, so every ingress path must keep it
// distinguishable from a malformed request. Sharing one classifier stops the
// standalone and replicated dispatch paths from drifting apart.
::dsn::rasn::rpc::runtime_error_code::type runtime_validation_error_code(
    const std::string &error);

template <typename Response>
std::string runtime_error_message(const Response &response)
{
    return response.status.__isset.message && !response.status.message.empty()
               ? response.status.message
               : "runtime module API request failed";
}

template <typename T>
std::string serialize_runtime_rpc_value(const T &value)
{
    ::dsn::binary_writer writer;
    marshall(writer, value, DSF_THRIFT_BINARY);
    const ::dsn::blob buffer = writer.get_buffer();
    return std::string(buffer.data(), buffer.length());
}

template <typename T>
bool deserialize_runtime_rpc_value(const std::string &encoded, T *value, std::string *error)
{
    if (value == nullptr || encoded.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
    {
        if (error != nullptr)
        {
            *error = "typed runtime RPC decode output or input is invalid";
        }
        return false;
    }
    try
    {
        ::dsn::binary_reader reader(
            ::dsn::blob(encoded.data(), 0, static_cast<unsigned int>(encoded.size())));
        unmarshall(reader, *value, DSF_THRIFT_BINARY);
        if (!reader.is_eof())
        {
            if (error != nullptr)
            {
                *error = "typed runtime RPC value contains trailing data";
            }
            return false;
        }
        if (error != nullptr)
        {
            error->clear();
        }
        return true;
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr)
        {
            *error = std::string("typed runtime RPC decode failed: ") + ex.what();
        }
        return false;
    }
}

// Partition fan-out traits mark mutating ingress that requires an explicit per-partition route;
// facade read aggregation is implemented separately.
#define RASN_DECLARE_RUNTIME_REQUEST_TRAITS(request_type)                                               \
    const char *runtime_module_name(const ::dsn::rasn::rpc::request_type &request);                     \
    bool validate_runtime_request(const ::dsn::rasn::rpc::request_type &request, std::string *error);   \
    bool runtime_request_is_mutating(const ::dsn::rasn::rpc::request_type &request);                    \
    bool runtime_request_is_partition_fanout(const ::dsn::rasn::rpc::request_type &request);            \
    std::string runtime_request_key(const ::dsn::rasn::rpc::request_type &request)

RASN_DECLARE_RUNTIME_REQUEST_TRAITS(agent_control_request);
RASN_DECLARE_RUNTIME_REQUEST_TRAITS(message_bus_request);
RASN_DECLARE_RUNTIME_REQUEST_TRAITS(task_orchestration_request);
RASN_DECLARE_RUNTIME_REQUEST_TRAITS(determinism_request);
RASN_DECLARE_RUNTIME_REQUEST_TRAITS(capability_directory_request);
RASN_DECLARE_RUNTIME_REQUEST_TRAITS(resource_budget_request);
RASN_DECLARE_RUNTIME_REQUEST_TRAITS(recovery_supervisor_request);
RASN_DECLARE_RUNTIME_REQUEST_TRAITS(blackboard_request);
RASN_DECLARE_RUNTIME_REQUEST_TRAITS(contract_verifier_request);
RASN_DECLARE_RUNTIME_REQUEST_TRAITS(human_interaction_rpc_request);
RASN_DECLARE_RUNTIME_REQUEST_TRAITS(sandbox_runtime_request);

#undef RASN_DECLARE_RUNTIME_REQUEST_TRAITS

::dsn::rasn::rpc::wire_agent_capability to_wire(const agent_capability &value);
::dsn::rasn::rpc::wire_agent_descriptor to_wire(const agent_descriptor &value);
::dsn::rasn::rpc::wire_agent_control_record to_wire(const agent_control_record &value);
::dsn::rasn::rpc::wire_agent_control_lease to_wire(const agent_control_lease &value);
::dsn::rasn::rpc::wire_agent_message to_wire(const agent_message &value);
::dsn::rasn::rpc::wire_orchestration_task to_wire(const orchestration_task &value);
::dsn::rasn::rpc::wire_deterministic_choice to_wire(const deterministic_choice &value);
::dsn::rasn::rpc::wire_capability_provider to_wire(const capability_provider &value);
::dsn::rasn::rpc::wire_resource_quota to_wire(const resource_quota &value);
::dsn::rasn::rpc::wire_resource_usage to_wire(const resource_usage &value);
::dsn::rasn::rpc::wire_resource_request to_wire(const resource_request &value);
::dsn::rasn::rpc::wire_resource_budget_decision to_wire(const resource_budget_decision &value);
::dsn::rasn::rpc::wire_recovery_policy to_wire(const recovery_policy &value);
::dsn::rasn::rpc::wire_failure_observation to_wire(const failure_observation &value);
::dsn::rasn::rpc::wire_recovery_action to_wire(const recovery_action &value);
::dsn::rasn::rpc::wire_blackboard_entry to_wire(const blackboard_entry &value);
::dsn::rasn::rpc::wire_agent_contract to_wire(const agent_contract &value);
::dsn::rasn::rpc::wire_contract_evaluation to_wire(const contract_evaluation &value);
::dsn::rasn::rpc::wire_human_interaction_request to_wire(const human_interaction_request &value);
::dsn::rasn::rpc::wire_sandbox_profile to_wire(const sandbox_profile &value);
::dsn::rasn::rpc::wire_sandbox_request to_wire(const sandbox_request &value);
::dsn::rasn::rpc::wire_sandbox_decision to_wire(const sandbox_decision &value);

bool from_wire(const ::dsn::rasn::rpc::wire_agent_capability &value,
               agent_capability *result,
               std::string *error);
bool from_wire(const ::dsn::rasn::rpc::wire_agent_descriptor &value,
               agent_descriptor *result,
               std::string *error);
bool from_wire(const ::dsn::rasn::rpc::wire_agent_control_record &value,
               agent_control_record *result,
               std::string *error);
bool from_wire(const ::dsn::rasn::rpc::wire_agent_control_lease &value,
               agent_control_lease *result,
               std::string *error);
bool from_wire(const ::dsn::rasn::rpc::wire_agent_message &value,
               agent_message *result,
               std::string *error);
bool from_wire(const ::dsn::rasn::rpc::wire_orchestration_task &value,
               orchestration_task *result,
               std::string *error);
bool from_wire(const ::dsn::rasn::rpc::wire_deterministic_choice &value,
               deterministic_choice *result,
               std::string *error);
bool from_wire(const ::dsn::rasn::rpc::wire_capability_provider &value,
               capability_provider *result,
               std::string *error);
bool from_wire(const ::dsn::rasn::rpc::wire_resource_quota &value,
               resource_quota *result,
               std::string *error);
bool from_wire(const ::dsn::rasn::rpc::wire_resource_usage &value,
               resource_usage *result,
               std::string *error);
bool from_wire(const ::dsn::rasn::rpc::wire_resource_request &value,
               resource_request *result,
               std::string *error);
bool from_wire(const ::dsn::rasn::rpc::wire_resource_budget_decision &value,
               resource_budget_decision *result,
               std::string *error);
bool from_wire(const ::dsn::rasn::rpc::wire_recovery_policy &value,
               recovery_policy *result,
               std::string *error);
bool from_wire(const ::dsn::rasn::rpc::wire_failure_observation &value,
               failure_observation *result,
               std::string *error);
bool from_wire(const ::dsn::rasn::rpc::wire_recovery_action &value,
               recovery_action *result,
               std::string *error);
bool from_wire(const ::dsn::rasn::rpc::wire_blackboard_entry &value,
               blackboard_entry *result,
               std::string *error);
bool from_wire(const ::dsn::rasn::rpc::wire_agent_contract &value,
               agent_contract *result,
               std::string *error);
bool from_wire(const ::dsn::rasn::rpc::wire_contract_evaluation &value,
               contract_evaluation *result,
               std::string *error);
bool from_wire(const ::dsn::rasn::rpc::wire_human_interaction_request &value,
               human_interaction_request *result,
               std::string *error);
bool from_wire(const ::dsn::rasn::rpc::wire_sandbox_profile &value,
               sandbox_profile *result,
               std::string *error);
bool from_wire(const ::dsn::rasn::rpc::wire_sandbox_request &value,
               sandbox_request *result,
               std::string *error);
bool from_wire(const ::dsn::rasn::rpc::wire_sandbox_decision &value,
               sandbox_decision *result,
               std::string *error);

} // namespace rasn
} // namespace dsn
