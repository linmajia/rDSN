#include <rasn/runtime_rpc_schema.h>

#include <rasn/agent_types.h>

#include <algorithm>
#include <cstddef>
#include <limits>

namespace dsn {
namespace rasn {
namespace {

const size_t kMaxRuntimeRpcItems = 4096;
const size_t kMaxRuntimeRpcTextBytes = 16 * 1024 * 1024;

bool invalid(std::string *error, const std::string &message)
{
    if (error != nullptr)
    {
        *error = message;
    }
    return false;
}

bool invalid_with_default(std::string *error, const std::string &message)
{
    if (error == nullptr || error->empty())
    {
        return invalid(error, message);
    }
    return false;
}

bool validate_metadata(const ::dsn::rasn::rpc::runtime_request_metadata &metadata,
                       std::string *error)
{
    if (error != nullptr)
    {
        error->clear();
    }
    if (metadata.wire_version <= 0 || metadata.min_compatible_version <= 0 ||
        metadata.min_compatible_version > metadata.wire_version)
    {
        return invalid(error, "runtime RPC version range is invalid");
    }
    if (metadata.min_compatible_version > RASN_RUNTIME_WIRE_VERSION ||
        metadata.wire_version < RASN_RUNTIME_MIN_COMPATIBLE_VERSION)
    {
        return invalid(error, "runtime RPC version range is unsupported");
    }
    if (metadata.__isset.route_partition &&
        (metadata.route_partition < 0 ||
         static_cast<uint64_t>(metadata.route_partition) >
             static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)())))
    {
        return invalid(error, "runtime RPC route partition is out of range");
    }
    if (metadata.__isset.request_id && metadata.request_id.size() > 1024)
    {
        return invalid(error, "runtime RPC request id is too large");
    }
    if (metadata.__isset.trace_id && metadata.trace_id.size() > 1024)
    {
        return invalid(error, "runtime RPC trace id is too large");
    }
    if (metadata.__isset.auth_token && metadata.auth_token.size() > 64 * 1024)
    {
        return invalid(error, "runtime RPC auth token is too large");
    }
    return true;
}

bool validate_mutation_id(const ::dsn::rasn::rpc::runtime_request_metadata &metadata,
                          bool mutating,
                          std::string *error)
{
    if (mutating && (!metadata.__isset.request_id || metadata.request_id.empty()))
    {
        return invalid(error, "runtime mutation requires a request id");
    }
    return true;
}

bool validate_text(const std::string &value, const char *field, std::string *error)
{
    return value.size() <= kMaxRuntimeRpcTextBytes
               ? true
               : invalid(error, std::string("runtime RPC field is too large: ") + field);
}

bool validate_items(size_t count, const char *field, std::string *error)
{
    return count <= kMaxRuntimeRpcItems
               ? true
               : invalid(error, std::string("runtime RPC collection is too large: ") + field);
}

template <typename Unsigned, typename Signed>
bool decode_unsigned(Signed value, Unsigned *result, const char *field, std::string *error)
{
    static_assert(std::is_integral<Signed>::value && std::is_signed<Signed>::value,
                      "runtime wire type must be a signed integer");
    static_assert(std::is_integral<Unsigned>::value && std::is_unsigned<Unsigned>::value,
                      "runtime domain type must be an unsigned integer");
    using wire_unsigned = typename std::make_unsigned<Signed>::type;
    wire_unsigned bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    if (static_cast<uint64_t>(bits) >
            static_cast<uint64_t>((std::numeric_limits<Unsigned>::max)()))
    {
            return invalid(error, std::string("runtime RPC unsigned field is out of range: ") + field);
    }
    *result = static_cast<Unsigned>(bits);
    return true;
}

template <typename T>
bool require_output(T *result, std::string *error)
{
    return result != nullptr ? true : invalid(error, "runtime RPC conversion output is null");
}

size_t count_agent_control_bodies(const ::dsn::rasn::rpc::agent_control_request &request)
{
    return request.__isset.upsert_agent + request.__isset.acquire_lease +
           request.__isset.heartbeat + request.__isset.find +
           request.__isset.expire_leases + request.__isset.list_agents +
           request.__isset.describe;
}

size_t count_message_bodies(const ::dsn::rasn::rpc::message_bus_request &request)
{
    return request.__isset.publish + request.__isset.acknowledge +
           request.__isset.dead_letter + request.__isset.find;
}

size_t count_task_bodies(const ::dsn::rasn::rpc::task_orchestration_request &request)
{
    return request.__isset.add_task + request.__isset.start + request.__isset.complete +
           request.__isset.fail + request.__isset.find + request.__isset.ready;
}

size_t count_human_bodies(const ::dsn::rasn::rpc::human_interaction_rpc_request &request)
{
    return request.__isset.open + request.__isset.answer + request.__isset.cancel +
           request.__isset.find + request.__isset.expire + request.__isset.pending;
}

template <typename Request>
bool finish_validation(const Request &request,
                       size_t bodies,
                       size_t expected,
                       bool matching,
                       const char *operation_error,
                       std::string *error)
{
    if (bodies != expected)
    {
        return invalid(error, "runtime RPC operation must have exactly its matching typed body");
    }
    if (!matching)
    {
        return invalid_with_default(error, operation_error);
    }
    if (!validate_mutation_id(request.metadata, runtime_request_is_mutating(request), error))
    {
        return false;
    }
    if (error != nullptr)
    {
        error->clear();
    }
    return true;
}

} // namespace

::dsn::rasn::rpc::runtime_request_metadata make_runtime_request_metadata()
{
    ::dsn::rasn::rpc::runtime_request_metadata metadata;
    metadata.wire_version = RASN_RUNTIME_WIRE_VERSION;
    metadata.min_compatible_version = RASN_RUNTIME_MIN_COMPATIBLE_VERSION;
    return metadata;
}

::dsn::rasn::rpc::runtime_error_code::type runtime_validation_error_code(
    const std::string &error)
{
    return error.find("version") != std::string::npos
               ? ::dsn::rasn::rpc::runtime_error_code::unsupported_version
               : ::dsn::rasn::rpc::runtime_error_code::invalid_request;
}

#define RASN_MODULE_NAME(type, name)                                                                    \
    const char *runtime_module_name(const ::dsn::rasn::rpc::type &) { return name; }

RASN_MODULE_NAME(agent_control_request, "agent_control_plane")
RASN_MODULE_NAME(message_bus_request, "agent_message_bus")
RASN_MODULE_NAME(task_orchestration_request, "task_orchestration_kernel")
RASN_MODULE_NAME(determinism_request, "determinism_ledger")
RASN_MODULE_NAME(capability_directory_request, "capability_directory")
RASN_MODULE_NAME(resource_budget_request, "resource_budget")
RASN_MODULE_NAME(recovery_supervisor_request, "recovery_supervisor")
RASN_MODULE_NAME(blackboard_request, "blackboard")
RASN_MODULE_NAME(contract_verifier_request, "contract_verifier")
RASN_MODULE_NAME(human_interaction_rpc_request, "human_interaction")
RASN_MODULE_NAME(sandbox_runtime_request, "sandbox_runtime")

#undef RASN_MODULE_NAME

bool runtime_request_is_mutating(const ::dsn::rasn::rpc::agent_control_request &request)
{
    using operation = ::dsn::rasn::rpc::agent_control_operation;
    return request.operation == operation::upsert_agent ||
           request.operation == operation::acquire_lease ||
           request.operation == operation::heartbeat ||
           request.operation == operation::expire_leases;
}

bool runtime_request_is_mutating(const ::dsn::rasn::rpc::message_bus_request &request)
{
    using operation = ::dsn::rasn::rpc::message_bus_operation;
    return request.operation == operation::publish ||
           request.operation == operation::acknowledge ||
           request.operation == operation::dead_letter;
}

bool runtime_request_is_mutating(const ::dsn::rasn::rpc::task_orchestration_request &request)
{
    using operation = ::dsn::rasn::rpc::task_orchestration_operation;
    return request.operation == operation::add_task || request.operation == operation::start ||
           request.operation == operation::complete || request.operation == operation::fail;
}

bool runtime_request_is_mutating(const ::dsn::rasn::rpc::determinism_request &request)
{
    return request.operation == ::dsn::rasn::rpc::determinism_operation::record;
}

bool runtime_request_is_mutating(const ::dsn::rasn::rpc::capability_directory_request &request)
{
    return request.operation == ::dsn::rasn::rpc::capability_directory_operation::upsert_provider;
}

bool runtime_request_is_mutating(const ::dsn::rasn::rpc::resource_budget_request &request)
{
    using operation = ::dsn::rasn::rpc::resource_budget_operation;
    return request.operation == operation::configure || request.operation == operation::reserve ||
           request.operation == operation::release;
}

bool runtime_request_is_mutating(const ::dsn::rasn::rpc::recovery_supervisor_request &request)
{
    using operation = ::dsn::rasn::rpc::recovery_supervisor_operation;
    return request.operation == operation::set_policy ||
           request.operation == operation::observe_failure;
}

bool runtime_request_is_mutating(const ::dsn::rasn::rpc::blackboard_request &request)
{
    return request.operation == ::dsn::rasn::rpc::blackboard_operation::put;
}

bool runtime_request_is_mutating(const ::dsn::rasn::rpc::contract_verifier_request &request)
{
    return request.operation ==
           ::dsn::rasn::rpc::contract_verifier_operation::register_contract;
}

bool runtime_request_is_mutating(const ::dsn::rasn::rpc::human_interaction_rpc_request &request)
{
    using operation = ::dsn::rasn::rpc::human_interaction_operation;
    return request.operation == operation::open || request.operation == operation::answer ||
           request.operation == operation::cancel || request.operation == operation::expire;
}

bool runtime_request_is_mutating(const ::dsn::rasn::rpc::sandbox_runtime_request &request)
{
    return request.operation == ::dsn::rasn::rpc::sandbox_runtime_operation::set_profile;
}

#define RASN_NO_FANOUT(type)                                                                            \
    bool runtime_request_is_partition_fanout(const ::dsn::rasn::rpc::type &) { return false; }

RASN_NO_FANOUT(agent_control_request)
RASN_NO_FANOUT(message_bus_request)
RASN_NO_FANOUT(task_orchestration_request)
RASN_NO_FANOUT(determinism_request)
RASN_NO_FANOUT(capability_directory_request)
RASN_NO_FANOUT(resource_budget_request)
RASN_NO_FANOUT(recovery_supervisor_request)
RASN_NO_FANOUT(blackboard_request)
RASN_NO_FANOUT(contract_verifier_request)
RASN_NO_FANOUT(sandbox_runtime_request)

#undef RASN_NO_FANOUT

bool runtime_request_is_partition_fanout(
    const ::dsn::rasn::rpc::human_interaction_rpc_request &request)
{
    using operation = ::dsn::rasn::rpc::human_interaction_operation;
    // Read aggregation happens at the facade; this trait only gates mutating replica ingress.
    return request.operation == operation::expire;
}

std::string runtime_request_key(const ::dsn::rasn::rpc::agent_control_request &request)
{
    using operation = ::dsn::rasn::rpc::agent_control_operation;
    if (request.operation == operation::upsert_agent && request.__isset.upsert_agent)
        return request.upsert_agent.descriptor.agent_id;
    if (request.operation == operation::acquire_lease && request.__isset.acquire_lease)
        return request.acquire_lease.agent_id;
    if (request.operation == operation::heartbeat && request.__isset.heartbeat)
        return request.heartbeat.agent_id;
    if (request.operation == operation::find && request.__isset.find)
        return request.find.agent_id;
    return std::string();
}

std::string runtime_request_key(const ::dsn::rasn::rpc::message_bus_request &request)
{
    using operation = ::dsn::rasn::rpc::message_bus_operation;
    if (request.operation == operation::publish && request.__isset.publish)
        return request.publish.message_id;
    if (request.operation == operation::acknowledge && request.__isset.acknowledge)
        return request.acknowledge.message_id;
    if (request.operation == operation::dead_letter && request.__isset.dead_letter)
        return request.dead_letter.message_id;
    if (request.operation == operation::find && request.__isset.find)
        return request.find.message_id;
    return std::string();
}

std::string runtime_request_key(const ::dsn::rasn::rpc::task_orchestration_request &request)
{
    using operation = ::dsn::rasn::rpc::task_orchestration_operation;
    if (request.operation == operation::add_task && request.__isset.add_task)
        return request.add_task.task_id;
    if (request.operation == operation::start && request.__isset.start)
        return request.start.task_id;
    if (request.operation == operation::complete && request.__isset.complete)
        return request.complete.task_id;
    if (request.operation == operation::fail && request.__isset.fail)
        return request.fail.task_id;
    if (request.operation == operation::find && request.__isset.find)
        return request.find.task_id;
    return std::string();
}

std::string runtime_request_key(const ::dsn::rasn::rpc::determinism_request &request)
{
    return request.__isset.record ? request.record.task_id + "/" + request.record.key
                                  : std::string();
}

std::string runtime_request_key(const ::dsn::rasn::rpc::capability_directory_request &request)
{
    return request.__isset.upsert_provider ? request.upsert_provider.descriptor.agent_id
                                          : std::string();
}

std::string runtime_request_key(const ::dsn::rasn::rpc::resource_budget_request &request)
{
    using operation = ::dsn::rasn::rpc::resource_budget_operation;
    if (request.operation == operation::configure && request.__isset.configure)
        return request.configure.scope;
    if (request.operation == operation::reserve && request.__isset.reserve)
        return request.reserve.scope;
    if (request.operation == operation::release && request.__isset.release)
        return request.release.scope;
    if (request.operation == operation::usage && request.__isset.usage)
        return request.usage.scope;
    return std::string();
}

std::string runtime_request_key(const ::dsn::rasn::rpc::recovery_supervisor_request &request)
{
    using operation = ::dsn::rasn::rpc::recovery_supervisor_operation;
    if (request.operation == operation::set_policy && request.__isset.set_policy)
        return request.set_policy.failure_class;
    if (request.operation == operation::observe_failure && request.__isset.observe_failure)
        return request.observe_failure.task_id;
    return std::string();
}

std::string runtime_request_key(const ::dsn::rasn::rpc::blackboard_request &request)
{
    using operation = ::dsn::rasn::rpc::blackboard_operation;
    if (request.operation == operation::put && request.__isset.put)
        return request.put.key;
    if (request.operation == operation::get && request.__isset.get)
        return request.get.key;
    return std::string();
}

std::string runtime_request_key(const ::dsn::rasn::rpc::contract_verifier_request &request)
{
    using operation = ::dsn::rasn::rpc::contract_verifier_operation;
    if (request.operation == operation::register_contract && request.__isset.register_contract)
        return request.register_contract.contract_id;
    if (request.operation == operation::evaluate_input && request.__isset.evaluate_input)
        return request.evaluate_input.contract_id;
    if (request.operation == operation::evaluate_output && request.__isset.evaluate_output)
        return request.evaluate_output.contract_id;
    return std::string();
}

std::string runtime_request_key(const ::dsn::rasn::rpc::human_interaction_rpc_request &request)
{
    using operation = ::dsn::rasn::rpc::human_interaction_operation;
    if (request.operation == operation::open && request.__isset.open)
        return request.open.request_id;
    if (request.operation == operation::answer && request.__isset.answer)
        return request.answer.request_id;
    if (request.operation == operation::cancel && request.__isset.cancel)
        return request.cancel.request_id;
    if (request.operation == operation::find && request.__isset.find)
        return request.find.request_id;
    if (request.operation == operation::pending && request.__isset.pending)
        return request.pending.requester;
    return std::string();
}

std::string runtime_request_key(const ::dsn::rasn::rpc::sandbox_runtime_request &request)
{
    return request.__isset.set_profile ? request.set_profile.name : std::string();
}

bool validate_runtime_request(const ::dsn::rasn::rpc::agent_control_request &request,
                              std::string *error)
{
    using operation = ::dsn::rasn::rpc::agent_control_operation;
    if (!validate_metadata(request.metadata, error))
        return false;
    const size_t bodies = count_agent_control_bodies(request);
    size_t expected = 0;
    bool matching = request.operation == operation::ping;
    if (request.operation == operation::upsert_agent)
        expected = 1, matching = request.__isset.upsert_agent &&
                                 !request.upsert_agent.descriptor.agent_id.empty();
    else if (request.operation == operation::acquire_lease)
        expected = 1, matching = request.__isset.acquire_lease &&
                                 !request.acquire_lease.agent_id.empty() &&
                                 !request.acquire_lease.owner.empty();
    else if (request.operation == operation::heartbeat)
        expected = 1, matching = request.__isset.heartbeat &&
                                 !request.heartbeat.agent_id.empty();
    else if (request.operation == operation::find)
        expected = 1, matching = request.__isset.find && !request.find.agent_id.empty();
    else if (request.operation == operation::expire_leases)
        expected = 1, matching = request.__isset.expire_leases;
    else if (request.operation == operation::list_agents)
        expected = 1, matching = request.__isset.list_agents;
    else if (request.operation == operation::describe)
        expected = 1, matching = request.__isset.describe;
    return finish_validation(
        request, bodies, expected, matching, "invalid agent-control operation body", error);
}

bool validate_runtime_request(const ::dsn::rasn::rpc::message_bus_request &request,
                              std::string *error)
{
    using operation = ::dsn::rasn::rpc::message_bus_operation;
    if (!validate_metadata(request.metadata, error))
        return false;
    const size_t bodies = count_message_bodies(request);
    size_t expected = 0;
    bool matching = request.operation == operation::ping ||
                    request.operation == operation::snapshot ||
                    request.operation == operation::describe;
    if (request.operation == operation::publish)
        expected = 1, matching = request.__isset.publish && !request.publish.message_id.empty() &&
                                 validate_text(request.publish.payload, "message.payload", error);
    else if (request.operation == operation::acknowledge)
        expected = 1, matching = request.__isset.acknowledge &&
                                 !request.acknowledge.message_id.empty();
    else if (request.operation == operation::dead_letter)
        expected = 1, matching = request.__isset.dead_letter &&
                                 !request.dead_letter.message_id.empty();
    else if (request.operation == operation::find)
        expected = 1, matching = request.__isset.find && !request.find.message_id.empty();
    return finish_validation(
        request, bodies, expected, matching, "invalid message-bus operation body", error);
}

bool validate_runtime_request(const ::dsn::rasn::rpc::task_orchestration_request &request,
                              std::string *error)
{
    using operation = ::dsn::rasn::rpc::task_orchestration_operation;
    if (!validate_metadata(request.metadata, error))
        return false;
    const size_t bodies = count_task_bodies(request);
    size_t expected = 0;
    bool matching = request.operation == operation::ping ||
                    request.operation == operation::snapshot ||
                    request.operation == operation::blocked ||
                    request.operation == operation::describe;
    if (request.operation == operation::add_task)
        expected = 1, matching = request.__isset.add_task && !request.add_task.task_id.empty() &&
                                 validate_items(request.add_task.depends_on.size(), "task.depends_on", error);
    else if (request.operation == operation::start)
        expected = 1, matching = request.__isset.start && !request.start.task_id.empty();
    else if (request.operation == operation::complete)
        expected = 1, matching = request.__isset.complete && !request.complete.task_id.empty();
    else if (request.operation == operation::fail)
        expected = 1, matching = request.__isset.fail && !request.fail.task_id.empty();
    else if (request.operation == operation::find)
        expected = 1, matching = request.__isset.find && !request.find.task_id.empty();
    else if (request.operation == operation::ready)
        expected = 1, matching = request.__isset.ready;
    return finish_validation(request,
                             bodies,
                             expected,
                             matching,
                             "invalid task-orchestration operation body",
                             error);
}

bool validate_runtime_request(const ::dsn::rasn::rpc::determinism_request &request,
                              std::string *error)
{
    using operation = ::dsn::rasn::rpc::determinism_operation;
    if (!validate_metadata(request.metadata, error))
        return false;
    const size_t bodies = request.__isset.record;
    const bool is_record = request.operation == operation::record;
    const bool matching =
        is_record ? request.__isset.record && !request.record.task_id.empty() &&
                        !request.record.key.empty()
                  : request.operation == operation::ping ||
                        request.operation == operation::snapshot ||
                        request.operation == operation::describe;
    return finish_validation(request,
                             bodies,
                             is_record ? 1 : 0,
                             matching,
                             "invalid determinism operation body",
                             error);
}

bool validate_runtime_request(const ::dsn::rasn::rpc::capability_directory_request &request,
                              std::string *error)
{
    using operation = ::dsn::rasn::rpc::capability_directory_operation;
    if (!validate_metadata(request.metadata, error))
        return false;
    const bool is_upsert = request.operation == operation::upsert_provider;
    const bool matching =
        is_upsert ? request.__isset.upsert_provider &&
                        !request.upsert_provider.descriptor.agent_id.empty() &&
                        validate_items(request.upsert_provider.labels.size(), "provider.labels", error)
                  : request.operation == operation::ping || request.operation == operation::describe;
    return finish_validation(request,
                             request.__isset.upsert_provider,
                             is_upsert ? 1 : 0,
                             matching,
                             "invalid capability-directory operation body",
                             error);
}

bool validate_runtime_request(const ::dsn::rasn::rpc::resource_budget_request &request,
                              std::string *error)
{
    using operation = ::dsn::rasn::rpc::resource_budget_operation;
    if (!validate_metadata(request.metadata, error))
        return false;
    const size_t bodies = request.__isset.configure + request.__isset.reserve +
                          request.__isset.release + request.__isset.usage;
    size_t expected = 0;
    bool matching = request.operation == operation::ping || request.operation == operation::describe;
    if (request.operation == operation::configure)
        expected = 1, matching = request.__isset.configure && !request.configure.scope.empty();
    else if (request.operation == operation::reserve)
        expected = 1, matching = request.__isset.reserve && !request.reserve.scope.empty();
    else if (request.operation == operation::release)
        expected = 1, matching = request.__isset.release && !request.release.scope.empty();
    else if (request.operation == operation::usage)
        expected = 1, matching = request.__isset.usage && !request.usage.scope.empty();
    return finish_validation(
        request, bodies, expected, matching, "invalid resource-budget operation body", error);
}

bool validate_runtime_request(const ::dsn::rasn::rpc::recovery_supervisor_request &request,
                              std::string *error)
{
    using operation = ::dsn::rasn::rpc::recovery_supervisor_operation;
    if (!validate_metadata(request.metadata, error))
        return false;
    const size_t bodies = request.__isset.set_policy + request.__isset.observe_failure;
    size_t expected = 0;
    bool matching = request.operation == operation::ping || request.operation == operation::describe;
    if (request.operation == operation::set_policy)
        expected = 1, matching = request.__isset.set_policy &&
                                 !request.set_policy.failure_class.empty();
    else if (request.operation == operation::observe_failure)
        expected = 1, matching = request.__isset.observe_failure &&
                                 !request.observe_failure.task_id.empty();
    return finish_validation(request,
                             bodies,
                             expected,
                             matching,
                             "invalid recovery-supervisor operation body",
                             error);
}

bool validate_runtime_request(const ::dsn::rasn::rpc::blackboard_request &request,
                              std::string *error)
{
    using operation = ::dsn::rasn::rpc::blackboard_operation;
    if (!validate_metadata(request.metadata, error))
        return false;
    const size_t bodies = request.__isset.put + request.__isset.get + request.__isset.snapshot;
    size_t expected = 0;
    bool matching = request.operation == operation::ping || request.operation == operation::describe;
    if (request.operation == operation::put)
        expected = 1, matching = request.__isset.put && !request.put.key.empty() &&
                                 validate_items(request.put.tags.size(), "blackboard.tags", error);
    else if (request.operation == operation::get)
        expected = 1, matching = request.__isset.get && !request.get.key.empty();
    else if (request.operation == operation::snapshot)
        expected = 1, matching = request.__isset.snapshot;
    return finish_validation(
        request, bodies, expected, matching, "invalid blackboard operation body", error);
}

bool validate_runtime_request(const ::dsn::rasn::rpc::contract_verifier_request &request,
                              std::string *error)
{
    using operation = ::dsn::rasn::rpc::contract_verifier_operation;
    if (!validate_metadata(request.metadata, error))
        return false;
    const size_t bodies = request.__isset.register_contract + request.__isset.evaluate_input +
                          request.__isset.evaluate_output;
    size_t expected = 0;
    bool matching = request.operation == operation::ping || request.operation == operation::describe;
    if (request.operation == operation::register_contract)
        expected = 1, matching = request.__isset.register_contract &&
                                 !request.register_contract.contract_id.empty();
    else if (request.operation == operation::evaluate_input)
        expected = 1, matching = request.__isset.evaluate_input &&
                                 !request.evaluate_input.contract_id.empty();
    else if (request.operation == operation::evaluate_output)
        expected = 1, matching = request.__isset.evaluate_output &&
                                 !request.evaluate_output.contract_id.empty() &&
                                 validate_items(request.evaluate_output.policy_labels.size(),
                                                "contract.policy_labels",
                                                error);
    return finish_validation(request,
                             bodies,
                             expected,
                             matching,
                             "invalid contract-verifier operation body",
                             error);
}

bool validate_runtime_request(const ::dsn::rasn::rpc::human_interaction_rpc_request &request,
                              std::string *error)
{
    using operation = ::dsn::rasn::rpc::human_interaction_operation;
    if (!validate_metadata(request.metadata, error))
        return false;
    const size_t bodies = count_human_bodies(request);
    size_t expected = 0;
    bool matching = request.operation == operation::ping ||
                    request.operation == operation::snapshot ||
                    request.operation == operation::describe;
    if (request.operation == operation::open)
        expected = 1, matching = request.__isset.open && !request.open.request_id.empty() &&
                                 validate_items(request.open.choices.size(), "human.choices", error);
    else if (request.operation == operation::answer)
        expected = 1, matching = request.__isset.answer && !request.answer.request_id.empty();
    else if (request.operation == operation::cancel)
        expected = 1, matching = request.__isset.cancel && !request.cancel.request_id.empty();
    else if (request.operation == operation::find)
        expected = 1, matching = request.__isset.find && !request.find.request_id.empty();
    else if (request.operation == operation::expire)
        expected = 1, matching = request.__isset.expire;
    else if (request.operation == operation::pending)
        expected = 1, matching = request.__isset.pending;
    return finish_validation(request,
                             bodies,
                             expected,
                             matching,
                             "invalid human-interaction operation body",
                             error);
}

bool validate_runtime_request(const ::dsn::rasn::rpc::sandbox_runtime_request &request,
                              std::string *error)
{
    using operation = ::dsn::rasn::rpc::sandbox_runtime_operation;
    if (!validate_metadata(request.metadata, error))
        return false;
    const size_t bodies = request.__isset.set_profile + request.__isset.evaluate;
    size_t expected = 0;
    bool matching = request.operation == operation::ping ||
                    request.operation == operation::get_profile ||
                    request.operation == operation::describe;
    if (request.operation == operation::set_profile)
        expected = 1, matching = request.__isset.set_profile &&
                                 !request.set_profile.name.empty() &&
                                 validate_items(request.set_profile.allowed_roots.size(),
                                                "sandbox.allowed_roots",
                                                error);
    else if (request.operation == operation::evaluate)
        expected = 1, matching = request.__isset.evaluate &&
                                 !request.evaluate.operation.empty();
    return finish_validation(
        request, bodies, expected, matching, "invalid sandbox-runtime operation body", error);
}

::dsn::rasn::rpc::wire_agent_capability to_wire(const agent_capability &value)
{
    ::dsn::rasn::rpc::wire_agent_capability wire;
    wire.schema_version = encode_runtime_unsigned<int32_t>(value.schema_version);
    wire.name = value.name;
    wire.input_type = value.input_type;
    wire.output_type = value.output_type;
    wire.side_effect_class = value.side_effect_class;
    wire.cost_hint = encode_runtime_unsigned<int32_t>(value.cost_hint);
    wire.latency_hint_ms = encode_runtime_unsigned<int32_t>(value.latency_hint_ms);
    wire.reliability_hint = encode_runtime_unsigned<int32_t>(value.reliability_hint);
    return wire;
}

::dsn::rasn::rpc::wire_agent_descriptor to_wire(const agent_descriptor &value)
{
    ::dsn::rasn::rpc::wire_agent_descriptor wire;
    wire.schema_version = encode_runtime_unsigned<int32_t>(value.schema_version);
    wire.agent_id = value.agent_id;
    wire.role = value.role;
    wire.app_name = value.app_name;
    wire.host = value.host;
    wire.port = encode_runtime_unsigned<int32_t>(value.port);
    wire.endpoint_uri = value.endpoint_uri;
    wire.version = value.version;
    wire.health = value.health;
    wire.capabilities.reserve(value.capabilities.size());
    for (const agent_capability &capability : value.capabilities)
        wire.capabilities.push_back(to_wire(capability));
    return wire;
}

::dsn::rasn::rpc::wire_agent_control_record to_wire(const agent_control_record &value)
{
    ::dsn::rasn::rpc::wire_agent_control_record wire;
    wire.descriptor = to_wire(value.descriptor);
    wire.state = value.state;
    wire.placement = value.placement;
    wire.owner = value.owner;
    wire.restart_policy = value.restart_policy;
    wire.last_error = value.last_error;
    wire.generation = encode_runtime_unsigned<int64_t>(value.generation);
    wire.last_heartbeat_ms = encode_runtime_unsigned<int64_t>(value.last_heartbeat_ms);
    wire.lease_expires_ms = encode_runtime_unsigned<int64_t>(value.lease_expires_ms);
    return wire;
}

::dsn::rasn::rpc::wire_agent_control_lease to_wire(const agent_control_lease &value)
{
    ::dsn::rasn::rpc::wire_agent_control_lease wire;
    wire.ok = value.ok;
    wire.agent_id = value.agent_id;
    wire.owner = value.owner;
    wire.generation = encode_runtime_unsigned<int64_t>(value.generation);
    wire.expires_ms = encode_runtime_unsigned<int64_t>(value.expires_ms);
    wire.error = value.error;
    return wire;
}

::dsn::rasn::rpc::wire_agent_message to_wire(const agent_message &value)
{
    ::dsn::rasn::rpc::wire_agent_message wire;
    wire.message_id = value.message_id;
    wire.correlation_id = value.correlation_id;
    wire.sender = value.sender;
    wire.receiver = value.receiver;
    wire.type = value.type;
    wire.payload = value.payload;
    wire.state = value.state;
    wire.error = value.error;
    wire.attempt = encode_runtime_unsigned<int32_t>(value.attempt);
    wire.deadline_ms = encode_runtime_unsigned<int64_t>(value.deadline_ms);
    wire.available_at_ms = encode_runtime_unsigned<int64_t>(value.available_at_ms);
    wire.created_at_ms = encode_runtime_unsigned<int64_t>(value.created_at_ms);
    wire.updated_at_ms = encode_runtime_unsigned<int64_t>(value.updated_at_ms);
    return wire;
}

::dsn::rasn::rpc::wire_orchestration_task to_wire(const orchestration_task &value)
{
    ::dsn::rasn::rpc::wire_orchestration_task wire;
    wire.task_id = value.task_id;
    wire.parent_task_id = value.parent_task_id;
    wire.owner_agent = value.owner_agent;
    wire.state = value.state;
    wire.input = value.input;
    wire.output = value.output;
    wire.error = value.error;
    wire.depends_on = value.depends_on;
    wire.compensation = value.compensation;
    wire.deadline_ms = encode_runtime_unsigned<int64_t>(value.deadline_ms);
    wire.generation = encode_runtime_unsigned<int64_t>(value.generation);
    return wire;
}

::dsn::rasn::rpc::wire_deterministic_choice to_wire(const deterministic_choice &value)
{
    ::dsn::rasn::rpc::wire_deterministic_choice wire;
    wire.sequence = encode_runtime_unsigned<int64_t>(value.sequence);
    wire.task_id = value.task_id;
    wire.key = value.key;
    wire.source = value.source;
    wire.value = value.value;
    return wire;
}

::dsn::rasn::rpc::wire_capability_provider to_wire(const capability_provider &value)
{
    ::dsn::rasn::rpc::wire_capability_provider wire;
    wire.descriptor = to_wire(value.descriptor);
    wire.state = value.state;
    wire.placement = value.placement;
    wire.labels = value.labels;
    wire.load = encode_runtime_unsigned<int32_t>(value.load);
    wire.last_seen_ms = encode_runtime_unsigned<int64_t>(value.last_seen_ms);
    return wire;
}

::dsn::rasn::rpc::wire_resource_quota to_wire(const resource_quota &value)
{
    ::dsn::rasn::rpc::wire_resource_quota wire;
    wire.scope = value.scope;
    wire.max_cost_units = encode_runtime_unsigned<int64_t>(value.max_cost_units);
    wire.max_latency_ms = encode_runtime_unsigned<int64_t>(value.max_latency_ms);
    wire.max_tokens = encode_runtime_unsigned<int64_t>(value.max_tokens);
    wire.max_tool_calls = encode_runtime_unsigned<int64_t>(value.max_tool_calls);
    return wire;
}

::dsn::rasn::rpc::wire_resource_usage to_wire(const resource_usage &value)
{
    ::dsn::rasn::rpc::wire_resource_usage wire;
    wire.scope = value.scope;
    wire.cost_units = encode_runtime_unsigned<int64_t>(value.cost_units);
    wire.latency_ms = encode_runtime_unsigned<int64_t>(value.latency_ms);
    wire.tokens = encode_runtime_unsigned<int64_t>(value.tokens);
    wire.tool_calls = encode_runtime_unsigned<int64_t>(value.tool_calls);
    return wire;
}

::dsn::rasn::rpc::wire_resource_request to_wire(const resource_request &value)
{
    ::dsn::rasn::rpc::wire_resource_request wire;
    wire.scope = value.scope;
    wire.cost_units = encode_runtime_unsigned<int64_t>(value.cost_units);
    wire.latency_ms = encode_runtime_unsigned<int64_t>(value.latency_ms);
    wire.tokens = encode_runtime_unsigned<int64_t>(value.tokens);
    wire.tool_calls = encode_runtime_unsigned<int64_t>(value.tool_calls);
    wire.reason = value.reason;
    return wire;
}

::dsn::rasn::rpc::wire_resource_budget_decision to_wire(const resource_budget_decision &value)
{
    ::dsn::rasn::rpc::wire_resource_budget_decision wire;
    wire.allowed = value.allowed;
    wire.scope = value.scope;
    wire.reason = value.reason;
    wire.usage_after = to_wire(value.usage_after);
    wire.quota = to_wire(value.quota);
    return wire;
}

::dsn::rasn::rpc::wire_recovery_policy to_wire(const recovery_policy &value)
{
    ::dsn::rasn::rpc::wire_recovery_policy wire;
    wire.failure_class = value.failure_class;
    wire.max_attempts = encode_runtime_unsigned<int32_t>(value.max_attempts);
    wire.retry_delay_ms = encode_runtime_unsigned<int64_t>(value.retry_delay_ms);
    wire.escalate_after_attempts =
        encode_runtime_unsigned<int32_t>(value.escalate_after_attempts);
    wire.retryable = value.retryable;
    wire.compensation = value.compensation;
    return wire;
}

::dsn::rasn::rpc::wire_failure_observation to_wire(const failure_observation &value)
{
    ::dsn::rasn::rpc::wire_failure_observation wire;
    wire.task_id = value.task_id;
    wire.component = value.component;
    wire.failure_class = value.failure_class;
    wire.code = value.code;
    wire.message = value.message;
    wire.attempt = encode_runtime_unsigned<int32_t>(value.attempt);
    wire.retryable = value.retryable;
    wire.time_ms = encode_runtime_unsigned<int64_t>(value.time_ms);
    return wire;
}

::dsn::rasn::rpc::wire_recovery_action to_wire(const recovery_action &value)
{
    ::dsn::rasn::rpc::wire_recovery_action wire;
    wire.handled = value.handled;
    wire.action = value.action;
    wire.delay_ms = encode_runtime_unsigned<int64_t>(value.delay_ms);
    wire.reason = value.reason;
    wire.labels = value.labels;
    return wire;
}

::dsn::rasn::rpc::wire_blackboard_entry to_wire(const blackboard_entry &value)
{
    ::dsn::rasn::rpc::wire_blackboard_entry wire;
    wire.key = value.key;
    wire.kind = value.kind;
    wire.owner = value.owner;
    wire.value = value.value;
    wire.tags = value.tags;
    wire.generation = encode_runtime_unsigned<int64_t>(value.generation);
    wire.created_at_ms = encode_runtime_unsigned<int64_t>(value.created_at_ms);
    wire.updated_at_ms = encode_runtime_unsigned<int64_t>(value.updated_at_ms);
    wire.expires_at_ms = encode_runtime_unsigned<int64_t>(value.expires_at_ms);
    return wire;
}

::dsn::rasn::rpc::wire_agent_contract to_wire(const agent_contract &value)
{
    ::dsn::rasn::rpc::wire_agent_contract wire;
    wire.contract_id = value.contract_id;
    wire.require_input_non_empty = value.require_input_non_empty;
    wire.require_output_non_empty = value.require_output_non_empty;
    wire.max_output_bytes = encode_runtime_unsigned<int64_t>(value.max_output_bytes);
    wire.required_input_fragments = value.required_input_fragments;
    wire.required_output_fragments = value.required_output_fragments;
    wire.forbidden_output_fragments = value.forbidden_output_fragments;
    wire.required_policy_labels = value.required_policy_labels;
    return wire;
}

::dsn::rasn::rpc::wire_contract_evaluation to_wire(const contract_evaluation &value)
{
    ::dsn::rasn::rpc::wire_contract_evaluation wire;
    wire.ok = value.ok;
    wire.contract_id = value.contract_id;
    wire.violations = value.violations;
    wire.warnings = value.warnings;
    return wire;
}

::dsn::rasn::rpc::wire_human_interaction_request to_wire(const human_interaction_request &value)
{
    ::dsn::rasn::rpc::wire_human_interaction_request wire;
    wire.request_id = value.request_id;
    wire.task_id = value.task_id;
    wire.kind = value.kind;
    wire.requester = value.requester;
    wire.prompt = value.prompt;
    wire.choices = value.choices;
    wire.state = value.state;
    wire.answer = value.answer;
    wire.created_at_ms = encode_runtime_unsigned<int64_t>(value.created_at_ms);
    wire.updated_at_ms = encode_runtime_unsigned<int64_t>(value.updated_at_ms);
    wire.deadline_ms = encode_runtime_unsigned<int64_t>(value.deadline_ms);
    return wire;
}

::dsn::rasn::rpc::wire_sandbox_profile to_wire(const sandbox_profile &value)
{
    ::dsn::rasn::rpc::wire_sandbox_profile wire;
    wire.name = value.name;
    wire.allow_filesystem_read = value.allow_filesystem_read;
    wire.allow_filesystem_write = value.allow_filesystem_write;
    wire.allow_network = value.allow_network;
    wire.allow_process_spawn = value.allow_process_spawn;
    wire.max_cpu_ms = encode_runtime_unsigned<int64_t>(value.max_cpu_ms);
    wire.max_memory_bytes = encode_runtime_unsigned<int64_t>(value.max_memory_bytes);
    wire.allowed_roots = value.allowed_roots;
    wire.denied_paths = value.denied_paths;
    wire.allowed_network_hosts = value.allowed_network_hosts;
    wire.allowed_commands = value.allowed_commands;
    return wire;
}

::dsn::rasn::rpc::wire_sandbox_request to_wire(const sandbox_request &value)
{
    ::dsn::rasn::rpc::wire_sandbox_request wire;
    wire.operation = value.operation;
    wire.path = value.path;
    wire.network_host = value.network_host;
    wire.command = value.command;
    return wire;
}

::dsn::rasn::rpc::wire_sandbox_decision to_wire(const sandbox_decision &value)
{
    ::dsn::rasn::rpc::wire_sandbox_decision wire;
    wire.allowed = value.allowed;
    wire.profile = value.profile;
    wire.reason = value.reason;
    wire.max_cpu_ms = encode_runtime_unsigned<int64_t>(value.max_cpu_ms);
    wire.max_memory_bytes = encode_runtime_unsigned<int64_t>(value.max_memory_bytes);
    return wire;
}

bool from_wire(const ::dsn::rasn::rpc::wire_agent_capability &value,
               agent_capability *result,
               std::string *error)
{
    if (!require_output(result, error) ||
        !decode_unsigned(value.schema_version, &result->schema_version, "capability.schema_version", error) ||
        !decode_unsigned(value.cost_hint, &result->cost_hint, "capability.cost_hint", error) ||
        !decode_unsigned(value.latency_hint_ms, &result->latency_hint_ms, "capability.latency_hint_ms", error) ||
        !decode_unsigned(value.reliability_hint, &result->reliability_hint, "capability.reliability_hint", error))
        return false;
    result->name = value.name;
    result->input_type = value.input_type;
    result->output_type = value.output_type;
    result->side_effect_class = value.side_effect_class;
    return true;
}

bool from_wire(const ::dsn::rasn::rpc::wire_agent_descriptor &value,
               agent_descriptor *result,
               std::string *error)
{
    if (!require_output(result, error) ||
        !validate_items(value.capabilities.size(), "descriptor.capabilities", error) ||
        !decode_unsigned(value.schema_version, &result->schema_version, "descriptor.schema_version", error) ||
        !decode_unsigned(value.port, &result->port, "descriptor.port", error))
        return false;
    result->agent_id = value.agent_id;
    result->role = value.role;
    result->app_name = value.app_name;
    result->host = value.host;
    result->endpoint_uri = value.endpoint_uri;
    result->version = value.version;
    result->health = value.health;
    result->capabilities.clear();
    result->capabilities.reserve(value.capabilities.size());
    for (const auto &wire_capability : value.capabilities)
    {
        agent_capability capability;
        if (!from_wire(wire_capability, &capability, error))
            return false;
        result->capabilities.push_back(capability);
    }
    return true;
}

bool from_wire(const ::dsn::rasn::rpc::wire_agent_control_record &value,
               agent_control_record *result,
               std::string *error)
{
    if (!require_output(result, error) ||
        !from_wire(value.descriptor, &result->descriptor, error) ||
        !decode_unsigned(value.generation, &result->generation, "agent.generation", error) ||
        !decode_unsigned(value.last_heartbeat_ms, &result->last_heartbeat_ms, "agent.last_heartbeat_ms", error) ||
        !decode_unsigned(value.lease_expires_ms, &result->lease_expires_ms, "agent.lease_expires_ms", error))
        return false;
    result->state = value.state;
    result->placement = value.placement;
    result->owner = value.owner;
    result->restart_policy = value.restart_policy;
    result->last_error = value.last_error;
    return true;
}

bool from_wire(const ::dsn::rasn::rpc::wire_agent_control_lease &value,
               agent_control_lease *result,
               std::string *error)
{
    if (!require_output(result, error) ||
        !decode_unsigned(value.generation, &result->generation, "lease.generation", error) ||
        !decode_unsigned(value.expires_ms, &result->expires_ms, "lease.expires_ms", error))
        return false;
    result->ok = value.ok;
    result->agent_id = value.agent_id;
    result->owner = value.owner;
    result->error = value.error;
    return true;
}

bool from_wire(const ::dsn::rasn::rpc::wire_agent_message &value,
               agent_message *result,
               std::string *error)
{
    if (!require_output(result, error) ||
        !decode_unsigned(value.attempt, &result->attempt, "message.attempt", error) ||
        !decode_unsigned(value.deadline_ms, &result->deadline_ms, "message.deadline_ms", error) ||
        !decode_unsigned(value.available_at_ms, &result->available_at_ms, "message.available_at_ms", error) ||
        !decode_unsigned(value.created_at_ms, &result->created_at_ms, "message.created_at_ms", error) ||
        !decode_unsigned(value.updated_at_ms, &result->updated_at_ms, "message.updated_at_ms", error))
        return false;
    result->message_id = value.message_id;
    result->correlation_id = value.correlation_id;
    result->sender = value.sender;
    result->receiver = value.receiver;
    result->type = value.type;
    result->payload = value.payload;
    result->state = value.state;
    result->error = value.error;
    return true;
}

bool from_wire(const ::dsn::rasn::rpc::wire_orchestration_task &value,
               orchestration_task *result,
               std::string *error)
{
    if (!require_output(result, error) ||
        !validate_items(value.depends_on.size(), "task.depends_on", error) ||
        !decode_unsigned(value.deadline_ms, &result->deadline_ms, "task.deadline_ms", error) ||
        !decode_unsigned(value.generation, &result->generation, "task.generation", error))
        return false;
    result->task_id = value.task_id;
    result->parent_task_id = value.parent_task_id;
    result->owner_agent = value.owner_agent;
    result->state = value.state;
    result->input = value.input;
    result->output = value.output;
    result->error = value.error;
    result->depends_on = value.depends_on;
    result->compensation = value.compensation;
    return true;
}

bool from_wire(const ::dsn::rasn::rpc::wire_deterministic_choice &value,
               deterministic_choice *result,
               std::string *error)
{
    if (!require_output(result, error) ||
        !decode_unsigned(value.sequence, &result->sequence, "choice.sequence", error))
        return false;
    result->task_id = value.task_id;
    result->key = value.key;
    result->source = value.source;
    result->value = value.value;
    return true;
}

bool from_wire(const ::dsn::rasn::rpc::wire_capability_provider &value,
               capability_provider *result,
               std::string *error)
{
    if (!require_output(result, error) ||
        !from_wire(value.descriptor, &result->descriptor, error) ||
        !validate_items(value.labels.size(), "provider.labels", error) ||
        !decode_unsigned(value.load, &result->load, "provider.load", error) ||
        !decode_unsigned(value.last_seen_ms, &result->last_seen_ms, "provider.last_seen_ms", error))
        return false;
    result->state = value.state;
    result->placement = value.placement;
    result->labels = value.labels;
    return true;
}

bool from_wire(const ::dsn::rasn::rpc::wire_resource_quota &value,
               resource_quota *result,
               std::string *error)
{
    if (!require_output(result, error) ||
        !decode_unsigned(value.max_cost_units, &result->max_cost_units, "quota.max_cost_units", error) ||
        !decode_unsigned(value.max_latency_ms, &result->max_latency_ms, "quota.max_latency_ms", error) ||
        !decode_unsigned(value.max_tokens, &result->max_tokens, "quota.max_tokens", error) ||
        !decode_unsigned(value.max_tool_calls, &result->max_tool_calls, "quota.max_tool_calls", error))
        return false;
    result->scope = value.scope;
    return true;
}

bool from_wire(const ::dsn::rasn::rpc::wire_resource_usage &value,
               resource_usage *result,
               std::string *error)
{
    if (!require_output(result, error) ||
        !decode_unsigned(value.cost_units, &result->cost_units, "usage.cost_units", error) ||
        !decode_unsigned(value.latency_ms, &result->latency_ms, "usage.latency_ms", error) ||
        !decode_unsigned(value.tokens, &result->tokens, "usage.tokens", error) ||
        !decode_unsigned(value.tool_calls, &result->tool_calls, "usage.tool_calls", error))
        return false;
    result->scope = value.scope;
    return true;
}

bool from_wire(const ::dsn::rasn::rpc::wire_resource_request &value,
               resource_request *result,
               std::string *error)
{
    if (!require_output(result, error) ||
        !decode_unsigned(value.cost_units, &result->cost_units, "request.cost_units", error) ||
        !decode_unsigned(value.latency_ms, &result->latency_ms, "request.latency_ms", error) ||
        !decode_unsigned(value.tokens, &result->tokens, "request.tokens", error) ||
        !decode_unsigned(value.tool_calls, &result->tool_calls, "request.tool_calls", error))
        return false;
    result->scope = value.scope;
    result->reason = value.reason;
    return true;
}

bool from_wire(const ::dsn::rasn::rpc::wire_resource_budget_decision &value,
               resource_budget_decision *result,
               std::string *error)
{
    if (!require_output(result, error) ||
        !from_wire(value.usage_after, &result->usage_after, error) ||
        !from_wire(value.quota, &result->quota, error))
        return false;
    result->allowed = value.allowed;
    result->scope = value.scope;
    result->reason = value.reason;
    return true;
}

bool from_wire(const ::dsn::rasn::rpc::wire_recovery_policy &value,
               recovery_policy *result,
               std::string *error)
{
    if (!require_output(result, error) ||
        !decode_unsigned(value.max_attempts, &result->max_attempts, "policy.max_attempts", error) ||
        !decode_unsigned(value.retry_delay_ms, &result->retry_delay_ms, "policy.retry_delay_ms", error) ||
        !decode_unsigned(value.escalate_after_attempts,
                         &result->escalate_after_attempts,
                         "policy.escalate_after_attempts",
                         error))
        return false;
    result->failure_class = value.failure_class;
    result->retryable = value.retryable;
    result->compensation = value.compensation;
    return true;
}

bool from_wire(const ::dsn::rasn::rpc::wire_failure_observation &value,
               failure_observation *result,
               std::string *error)
{
    if (!require_output(result, error) ||
        !decode_unsigned(value.attempt, &result->attempt, "failure.attempt", error) ||
        !decode_unsigned(value.time_ms, &result->time_ms, "failure.time_ms", error))
        return false;
    result->task_id = value.task_id;
    result->component = value.component;
    result->failure_class = value.failure_class;
    result->code = value.code;
    result->message = value.message;
    result->retryable = value.retryable;
    return true;
}

bool from_wire(const ::dsn::rasn::rpc::wire_recovery_action &value,
               recovery_action *result,
               std::string *error)
{
    if (!require_output(result, error) ||
        !decode_unsigned(value.delay_ms, &result->delay_ms, "action.delay_ms", error) ||
        !validate_items(value.labels.size(), "action.labels", error))
        return false;
    result->handled = value.handled;
    result->action = value.action;
    result->reason = value.reason;
    result->labels = value.labels;
    return true;
}

bool from_wire(const ::dsn::rasn::rpc::wire_blackboard_entry &value,
               blackboard_entry *result,
               std::string *error)
{
    if (!require_output(result, error) ||
        !validate_items(value.tags.size(), "blackboard.tags", error) ||
        !decode_unsigned(value.generation, &result->generation, "blackboard.generation", error) ||
        !decode_unsigned(value.created_at_ms, &result->created_at_ms, "blackboard.created_at_ms", error) ||
        !decode_unsigned(value.updated_at_ms, &result->updated_at_ms, "blackboard.updated_at_ms", error) ||
        !decode_unsigned(value.expires_at_ms, &result->expires_at_ms, "blackboard.expires_at_ms", error))
        return false;
    result->key = value.key;
    result->kind = value.kind;
    result->owner = value.owner;
    result->value = value.value;
    result->tags = value.tags;
    return true;
}

bool from_wire(const ::dsn::rasn::rpc::wire_agent_contract &value,
               agent_contract *result,
               std::string *error)
{
    if (!require_output(result, error) ||
        !decode_unsigned(value.max_output_bytes, &result->max_output_bytes, "contract.max_output_bytes", error) ||
        !validate_items(value.required_input_fragments.size(), "contract.required_input_fragments", error) ||
        !validate_items(value.required_output_fragments.size(), "contract.required_output_fragments", error) ||
        !validate_items(value.forbidden_output_fragments.size(), "contract.forbidden_output_fragments", error) ||
        !validate_items(value.required_policy_labels.size(), "contract.required_policy_labels", error))
        return false;
    result->contract_id = value.contract_id;
    result->require_input_non_empty = value.require_input_non_empty;
    result->require_output_non_empty = value.require_output_non_empty;
    result->required_input_fragments = value.required_input_fragments;
    result->required_output_fragments = value.required_output_fragments;
    result->forbidden_output_fragments = value.forbidden_output_fragments;
    result->required_policy_labels = value.required_policy_labels;
    return true;
}

bool from_wire(const ::dsn::rasn::rpc::wire_contract_evaluation &value,
               contract_evaluation *result,
               std::string *error)
{
    if (!require_output(result, error) ||
        !validate_items(value.violations.size(), "evaluation.violations", error) ||
        !validate_items(value.warnings.size(), "evaluation.warnings", error))
        return false;
    result->ok = value.ok;
    result->contract_id = value.contract_id;
    result->violations = value.violations;
    result->warnings = value.warnings;
    return true;
}

bool from_wire(const ::dsn::rasn::rpc::wire_human_interaction_request &value,
               human_interaction_request *result,
               std::string *error)
{
    if (!require_output(result, error) ||
        !validate_items(value.choices.size(), "human.choices", error) ||
        !decode_unsigned(value.created_at_ms, &result->created_at_ms, "human.created_at_ms", error) ||
        !decode_unsigned(value.updated_at_ms, &result->updated_at_ms, "human.updated_at_ms", error) ||
        !decode_unsigned(value.deadline_ms, &result->deadline_ms, "human.deadline_ms", error))
        return false;
    result->request_id = value.request_id;
    result->task_id = value.task_id;
    result->kind = value.kind;
    result->requester = value.requester;
    result->prompt = value.prompt;
    result->choices = value.choices;
    result->state = value.state;
    result->answer = value.answer;
    return true;
}

bool from_wire(const ::dsn::rasn::rpc::wire_sandbox_profile &value,
               sandbox_profile *result,
               std::string *error)
{
    if (!require_output(result, error) ||
        !decode_unsigned(value.max_cpu_ms, &result->max_cpu_ms, "sandbox.max_cpu_ms", error) ||
        !decode_unsigned(value.max_memory_bytes, &result->max_memory_bytes, "sandbox.max_memory_bytes", error) ||
        !validate_items(value.allowed_roots.size(), "sandbox.allowed_roots", error) ||
        !validate_items(value.denied_paths.size(), "sandbox.denied_paths", error) ||
        !validate_items(value.allowed_network_hosts.size(), "sandbox.allowed_network_hosts", error) ||
        !validate_items(value.allowed_commands.size(), "sandbox.allowed_commands", error))
        return false;
    result->name = value.name;
    result->allow_filesystem_read = value.allow_filesystem_read;
    result->allow_filesystem_write = value.allow_filesystem_write;
    result->allow_network = value.allow_network;
    result->allow_process_spawn = value.allow_process_spawn;
    result->allowed_roots = value.allowed_roots;
    result->denied_paths = value.denied_paths;
    result->allowed_network_hosts = value.allowed_network_hosts;
    result->allowed_commands = value.allowed_commands;
    return true;
}

bool from_wire(const ::dsn::rasn::rpc::wire_sandbox_request &value,
               sandbox_request *result,
               std::string *error)
{
    if (!require_output(result, error))
        return false;
    result->operation = value.operation;
    result->path = value.path;
    result->network_host = value.network_host;
    result->command = value.command;
    return true;
}

bool from_wire(const ::dsn::rasn::rpc::wire_sandbox_decision &value,
               sandbox_decision *result,
               std::string *error)
{
    if (!require_output(result, error) ||
        !decode_unsigned(value.max_cpu_ms, &result->max_cpu_ms, "decision.max_cpu_ms", error) ||
        !decode_unsigned(value.max_memory_bytes, &result->max_memory_bytes, "decision.max_memory_bytes", error))
        return false;
    result->allowed = value.allowed;
    result->profile = value.profile;
    result->reason = value.reason;
    return true;
}

} // namespace rasn
} // namespace dsn
