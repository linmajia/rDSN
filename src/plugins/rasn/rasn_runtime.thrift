namespace cpp dsn.rasn.rpc

const i32 RASN_RUNTIME_WIRE_VERSION = 1
const i32 RASN_RUNTIME_MIN_COMPATIBLE_VERSION = 1

// C++ uint32/uint64 domain fields use the same-width signed Thrift integer as a
// bit-preserving wire container so the full unsigned facade range round-trips.

enum runtime_error_code
{
    none = 0,
    invalid_request = 1,
    unsupported_version = 2,
    not_found = 3,
    conflict = 4,
    unauthorized = 5,
    misrouted = 6,
    unavailable = 7,
    internal = 8
}

struct runtime_request_metadata
{
    1: i32 wire_version = 1
    2: i32 min_compatible_version = 1
    3: optional string request_id
    4: optional i64 route_partition
    5: optional string auth_token
    6: optional string trace_id
}

struct runtime_response_status
{
    1: bool ok = true
    2: runtime_error_code code = runtime_error_code.none
    3: optional string message
    4: bool retryable = false
}

struct runtime_response_metadata
{
    1: i32 wire_version = 1
    2: i32 min_compatible_version = 1
    3: optional i64 route_partition
    4: optional string trace_id
}

struct wire_agent_capability
{
    1: i32 schema_version
    2: string name
    3: string input_type
    4: string output_type
    5: string side_effect_class
    6: i32 cost_hint
    7: i32 latency_hint_ms
    8: i32 reliability_hint
}

struct wire_agent_descriptor
{
    1: i32 schema_version
    2: string agent_id
    3: string role
    4: string app_name
    5: string host
    6: i32 port
    7: string endpoint_uri
    8: string version
    9: string health
    10: list<wire_agent_capability> capabilities
}

struct wire_agent_control_record
{
    1: wire_agent_descriptor descriptor
    2: string state
    3: string placement
    4: string owner
    5: string restart_policy
    6: string last_error
    7: i64 generation
    8: i64 last_heartbeat_ms
    9: i64 lease_expires_ms
}

struct wire_agent_control_lease
{
    1: bool ok
    2: string agent_id
    3: string owner
    4: i64 generation
    5: i64 expires_ms
    6: string error
}

struct wire_agent_message
{
    1: string message_id
    2: string correlation_id
    3: string sender
    4: string receiver
    5: string type
    6: string payload
    7: string state
    8: string error
    9: i32 attempt
    10: i64 deadline_ms
    11: i64 available_at_ms
    12: i64 created_at_ms
    13: i64 updated_at_ms
}

struct wire_orchestration_task
{
    1: string task_id
    2: string parent_task_id
    3: string owner_agent
    4: string state
    5: string input
    6: string output
    7: string error
    8: list<string> depends_on
    9: string compensation
    10: i64 deadline_ms
    11: i64 generation
}

struct wire_deterministic_choice
{
    1: i64 sequence
    2: string task_id
    3: string key
    4: string source
    5: string value
}

struct wire_capability_provider
{
    1: wire_agent_descriptor descriptor
    2: string state
    3: string placement
    4: list<string> labels
    5: i32 load
    6: i64 last_seen_ms
}

struct wire_resource_quota
{
    1: string scope
    2: i64 max_cost_units
    3: i64 max_latency_ms
    4: i64 max_tokens
    5: i64 max_tool_calls
}

struct wire_resource_usage
{
    1: string scope
    2: i64 cost_units
    3: i64 latency_ms
    4: i64 tokens
    5: i64 tool_calls
}

struct wire_resource_request
{
    1: string scope
    2: i64 cost_units
    3: i64 latency_ms
    4: i64 tokens
    5: i64 tool_calls
    6: string reason
}

struct wire_resource_budget_decision
{
    1: bool allowed
    2: string scope
    3: string reason
    4: wire_resource_usage usage_after
    5: wire_resource_quota quota
}

struct wire_recovery_policy
{
    1: string failure_class
    2: i32 max_attempts
    3: i64 retry_delay_ms
    4: i32 escalate_after_attempts
    5: bool retryable
    6: string compensation
}

struct wire_failure_observation
{
    1: string task_id
    2: string component
    3: string failure_class
    4: string code
    5: string message
    6: i32 attempt
    7: bool retryable
    8: i64 time_ms
}

struct wire_recovery_action
{
    1: bool handled
    2: string action
    3: i64 delay_ms
    4: string reason
    5: list<string> labels
}

struct wire_blackboard_entry
{
    1: string key
    2: string kind
    3: string owner
    4: string value
    5: list<string> tags
    6: i64 generation
    7: i64 created_at_ms
    8: i64 updated_at_ms
    9: i64 expires_at_ms
}

struct wire_agent_contract
{
    1: string contract_id
    2: bool require_input_non_empty
    3: bool require_output_non_empty
    4: i64 max_output_bytes
    5: list<string> required_input_fragments
    6: list<string> required_output_fragments
    7: list<string> forbidden_output_fragments
    8: list<string> required_policy_labels
}

struct wire_contract_evaluation
{
    1: bool ok
    2: string contract_id
    3: list<string> violations
    4: list<string> warnings
}

struct wire_human_interaction_request
{
    1: string request_id
    2: string task_id
    3: string kind
    4: string requester
    5: string prompt
    6: list<string> choices
    7: string state
    8: string answer
    9: i64 created_at_ms
    10: i64 updated_at_ms
    11: i64 deadline_ms
}

struct wire_sandbox_profile
{
    1: string name
    2: bool allow_filesystem_read
    3: bool allow_filesystem_write
    4: bool allow_network
    5: bool allow_process_spawn
    6: i64 max_cpu_ms
    7: i64 max_memory_bytes
    8: list<string> allowed_roots
    9: list<string> denied_paths
    10: list<string> allowed_network_hosts
    11: list<string> allowed_commands
}

struct wire_sandbox_request
{
    1: string operation
    2: string path
    3: string network_host
    4: string command
}

struct wire_sandbox_decision
{
    1: bool allowed
    2: string profile
    3: string reason
    4: i64 max_cpu_ms
    5: i64 max_memory_bytes
}

struct agent_control_acquire_lease_request
{
    1: string agent_id
    2: string owner
    3: i64 now_ms
    4: i64 lease_ms
}

struct agent_control_heartbeat_request
{
    1: string agent_id
    2: i64 now_ms
}

struct agent_control_find_request { 1: string agent_id }
struct agent_control_expire_request { 1: i64 now_ms }
struct agent_control_list_request { 1: bool include_expired, 2: i64 now_ms }
struct agent_control_describe_request { 1: i64 now_ms }

enum agent_control_operation
{
    ping = 1,
    upsert_agent = 2,
    acquire_lease = 3,
    heartbeat = 4,
    find = 5,
    expire_leases = 6,
    list_agents = 7,
    describe = 8
}

struct agent_control_request
{
    1: runtime_request_metadata metadata
    2: agent_control_operation operation
    10: optional wire_agent_control_record upsert_agent
    11: optional agent_control_acquire_lease_request acquire_lease
    12: optional agent_control_heartbeat_request heartbeat
    13: optional agent_control_find_request find
    14: optional agent_control_expire_request expire_leases
    15: optional agent_control_list_request list_agents
    16: optional agent_control_describe_request describe
}

struct agent_control_response
{
    1: runtime_response_metadata metadata
    2: runtime_response_status status
    3: agent_control_operation operation
    10: optional wire_agent_control_lease lease
    11: optional wire_agent_control_record agent
    12: optional list<wire_agent_control_record> agents
    13: optional i64 count
    14: optional string description
}

struct message_ack_request { 1: string message_id, 2: i64 now_ms }
struct message_dead_letter_request { 1: string message_id, 2: string reason, 3: i64 now_ms }
struct message_find_request { 1: string message_id }

enum message_bus_operation
{
    ping = 1,
    publish = 2,
    acknowledge = 3,
    dead_letter = 4,
    find = 5,
    snapshot = 6,
    describe = 7
}

struct message_bus_request
{
    1: runtime_request_metadata metadata
    2: message_bus_operation operation
    10: optional wire_agent_message publish
    11: optional message_ack_request acknowledge
    12: optional message_dead_letter_request dead_letter
    13: optional message_find_request find
}

struct message_bus_response
{
    1: runtime_response_metadata metadata
    2: runtime_response_status status
    3: message_bus_operation operation
    10: optional wire_agent_message message
    11: optional list<wire_agent_message> messages
    12: optional string description
}

struct task_start_request { 1: string task_id, 2: string owner_agent }
struct task_complete_request { 1: string task_id, 2: string output }
struct task_fail_request { 1: string task_id, 2: string error, 3: bool retryable }
struct task_find_request { 1: string task_id }
struct task_ready_request { 1: i64 now_ms }

enum task_orchestration_operation
{
    ping = 1,
    add_task = 2,
    start = 3,
    complete = 4,
    fail = 5,
    find = 6,
    snapshot = 7,
    ready = 8,
    blocked = 9,
    describe = 10
}

struct task_orchestration_request
{
    1: runtime_request_metadata metadata
    2: task_orchestration_operation operation
    10: optional wire_orchestration_task add_task
    11: optional task_start_request start
    12: optional task_complete_request complete
    13: optional task_fail_request fail
    14: optional task_find_request find
    15: optional task_ready_request ready
}

struct task_orchestration_response
{
    1: runtime_response_metadata metadata
    2: runtime_response_status status
    3: task_orchestration_operation operation
    10: optional wire_orchestration_task task
    11: optional list<wire_orchestration_task> tasks
    12: optional string description
}

struct determinism_record_request
{
    1: string task_id
    2: string key
    3: string source
    4: string value
}

enum determinism_operation
{
    ping = 1,
    record = 2,
    snapshot = 3,
    describe = 4
}

struct determinism_request
{
    1: runtime_request_metadata metadata
    2: determinism_operation operation
    10: optional determinism_record_request record
}

struct determinism_response
{
    1: runtime_response_metadata metadata
    2: runtime_response_status status
    3: determinism_operation operation
    10: optional wire_deterministic_choice choice
    11: optional list<wire_deterministic_choice> choices
    12: optional string description
}

enum capability_directory_operation
{
    ping = 1,
    upsert_provider = 2,
    describe = 3
}

struct capability_directory_request
{
    1: runtime_request_metadata metadata
    2: capability_directory_operation operation
    10: optional wire_capability_provider upsert_provider
}

struct capability_directory_response
{
    1: runtime_response_metadata metadata
    2: runtime_response_status status
    3: capability_directory_operation operation
    10: optional string description
}

struct resource_usage_request { 1: string scope }

enum resource_budget_operation
{
    ping = 1,
    configure = 2,
    reserve = 3,
    release = 4,
    usage = 5,
    describe = 6
}

struct resource_budget_request
{
    1: runtime_request_metadata metadata
    2: resource_budget_operation operation
    10: optional wire_resource_quota configure
    11: optional wire_resource_request reserve
    12: optional wire_resource_request release
    13: optional resource_usage_request usage
}

struct resource_budget_response
{
    1: runtime_response_metadata metadata
    2: runtime_response_status status
    3: resource_budget_operation operation
    10: optional wire_resource_budget_decision decision
    11: optional wire_resource_usage usage
    12: optional string description
}

enum recovery_supervisor_operation
{
    ping = 1,
    set_policy = 2,
    observe_failure = 3,
    describe = 4
}

struct recovery_supervisor_request
{
    1: runtime_request_metadata metadata
    2: recovery_supervisor_operation operation
    10: optional wire_recovery_policy set_policy
    11: optional wire_failure_observation observe_failure
}

struct recovery_supervisor_response
{
    1: runtime_response_metadata metadata
    2: runtime_response_status status
    3: recovery_supervisor_operation operation
    10: optional wire_recovery_action action
    11: optional string description
}

struct blackboard_get_request { 1: string key }
struct blackboard_snapshot_request { 1: bool include_expired, 2: i64 now_ms }

enum blackboard_operation
{
    ping = 1,
    put = 2,
    get = 3,
    snapshot = 4,
    describe = 5
}

struct blackboard_request
{
    1: runtime_request_metadata metadata
    2: blackboard_operation operation
    10: optional wire_blackboard_entry put
    11: optional blackboard_get_request get
    12: optional blackboard_snapshot_request snapshot
}

struct blackboard_response
{
    1: runtime_response_metadata metadata
    2: runtime_response_status status
    3: blackboard_operation operation
    10: optional wire_blackboard_entry entry
    11: optional list<wire_blackboard_entry> entries
    12: optional string description
}

struct contract_evaluate_input_request { 1: string contract_id, 2: string input }
struct contract_evaluate_output_request
{
    1: string contract_id
    2: string output
    3: list<string> policy_labels
}

enum contract_verifier_operation
{
    ping = 1,
    register_contract = 2,
    evaluate_input = 3,
    evaluate_output = 4,
    describe = 5
}

struct contract_verifier_request
{
    1: runtime_request_metadata metadata
    2: contract_verifier_operation operation
    10: optional wire_agent_contract register_contract
    11: optional contract_evaluate_input_request evaluate_input
    12: optional contract_evaluate_output_request evaluate_output
}

struct contract_verifier_response
{
    1: runtime_response_metadata metadata
    2: runtime_response_status status
    3: contract_verifier_operation operation
    10: optional wire_contract_evaluation evaluation
    11: optional string description
}

struct human_answer_request { 1: string request_id, 2: string answer, 3: i64 updated_at_ms }
struct human_cancel_request { 1: string request_id, 2: string reason, 3: i64 updated_at_ms }
struct human_find_request { 1: string request_id }
struct human_expire_request { 1: i64 now_ms }
struct human_pending_request { 1: string requester }

enum human_interaction_operation
{
    ping = 1,
    open = 2,
    answer = 3,
    cancel = 4,
    find = 5,
    expire = 6,
    snapshot = 7,
    pending = 8,
    describe = 9
}

struct human_interaction_rpc_request
{
    1: runtime_request_metadata metadata
    2: human_interaction_operation operation
    10: optional wire_human_interaction_request open
    11: optional human_answer_request answer
    12: optional human_cancel_request cancel
    13: optional human_find_request find
    14: optional human_expire_request expire
    15: optional human_pending_request pending
}

struct human_interaction_rpc_response
{
    1: runtime_response_metadata metadata
    2: runtime_response_status status
    3: human_interaction_operation operation
    10: optional wire_human_interaction_request request
    11: optional list<wire_human_interaction_request> requests
    12: optional i64 count
    13: optional string description
}

enum sandbox_runtime_operation
{
    ping = 1,
    set_profile = 2,
    get_profile = 3,
    evaluate = 4,
    describe = 5
}

struct sandbox_runtime_request
{
    1: runtime_request_metadata metadata
    2: sandbox_runtime_operation operation
    10: optional wire_sandbox_profile set_profile
    11: optional wire_sandbox_request evaluate
}

struct sandbox_runtime_response
{
    1: runtime_response_metadata metadata
    2: runtime_response_status status
    3: sandbox_runtime_operation operation
    10: optional wire_sandbox_profile profile
    11: optional wire_sandbox_decision decision
    12: optional string description
}
