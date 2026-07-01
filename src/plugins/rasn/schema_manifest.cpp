#include "schema_manifest.h"

#include "agent_types.h"
#include "observability.h"
#include "policy_manager.h"
#include "rasn_core.h"
#include "state_service.h"
#include "workflow.h"

#include <cctype>
#include <sstream>

namespace dsn {
namespace rasn {

namespace {

schema_field_descriptor field(const std::string &name,
                              const std::string &type,
                              bool required,
                              const std::string &notes = "")
{
    schema_field_descriptor descriptor;
    descriptor.name = name;
    descriptor.type = type;
    descriptor.required = required;
    descriptor.notes = notes;
    return descriptor;
}

schema_type_descriptor schema(const std::string &name,
                              uint32_t version,
                              const std::string &purpose,
                              const std::vector<schema_field_descriptor> &fields)
{
    schema_type_descriptor descriptor;
    descriptor.name = name;
    descriptor.version = version;
    descriptor.purpose = purpose;
    descriptor.fields = fields;
    return descriptor;
}

rpc_operation_descriptor operation(const std::string &service,
                                   const std::string &client_class,
                                   const std::string &method,
                                   const std::string &task_code,
                                   const std::string &request_type,
                                   const std::string &response_type)
{
    rpc_operation_descriptor descriptor;
    descriptor.service = service;
    descriptor.client_class = client_class;
    descriptor.method = method;
    descriptor.task_code = task_code;
    descriptor.request_type = request_type;
    descriptor.response_type = response_type;
    return descriptor;
}

std::string idl_type(std::string type)
{
    const bool array = type.size() > 2 && type.substr(type.size() - 2) == "[]";
    if (array)
    {
        type = type.substr(0, type.size() - 2);
    }
    std::string result;
    if (type == "string")
    {
        result = "string";
    }
    else if (type == "bool")
    {
        result = "bool";
    }
    else if (type == "uint32" || type == "uint64")
    {
        result = type;
    }
    else
    {
        result = type;
    }
    return array ? "list<" + result + ">" : result;
}

std::string cpp_type(std::string type)
{
    const bool array = type.size() > 2 && type.substr(type.size() - 2) == "[]";
    if (array)
    {
        type = type.substr(0, type.size() - 2);
    }

    std::string result;
    if (type == "string")
    {
        result = "std::string";
    }
    else if (type == "bool")
    {
        result = "bool";
    }
    else if (type == "uint32")
    {
        result = "std::uint32_t";
    }
    else if (type == "uint64")
    {
        result = "std::uint64_t";
    }
    else
    {
        result = type;
    }

    return array ? "std::vector<" + result + ">" : result;
}

std::string typescript_type(std::string type)
{
    const bool array = type.size() > 2 && type.substr(type.size() - 2) == "[]";
    if (array)
    {
        type = type.substr(0, type.size() - 2);
    }

    std::string result;
    if (type == "string")
    {
        result = "string";
    }
    else if (type == "bool")
    {
        result = "boolean";
    }
    else if (type == "uint32" || type == "uint64")
    {
        result = "number";
    }
    else
    {
        result = type;
    }

    return array ? result + "[]" : result;
}

std::string python_type(std::string type)
{
    const bool array = type.size() > 2 && type.substr(type.size() - 2) == "[]";
    if (array)
    {
        type = type.substr(0, type.size() - 2);
    }

    std::string result;
    if (type == "string")
    {
        result = "str";
    }
    else if (type == "bool")
    {
        result = "bool";
    }
    else if (type == "uint32" || type == "uint64")
    {
        result = "int";
    }
    else
    {
        result = type;
    }

    return array ? "List[" + result + "]" : result;
}

std::string typescript_client_type(const std::string &type)
{
    if (type == "std::string")
    {
        return "string";
    }
    return typescript_type(type);
}

std::string python_client_type(const std::string &type)
{
    if (type == "std::string")
    {
        return "str";
    }
    return python_type(type);
}

std::string pascal_case_identifier(const std::string &value)
{
    std::string result;
    bool uppercase_next = true;
    for (const char c : value)
    {
        if (c == '_' || c == '-' || c == '.')
        {
            uppercase_next = true;
            continue;
        }
        if (uppercase_next)
        {
            result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            uppercase_next = false;
        }
        else
        {
            result.push_back(c);
        }
    }
    return result.empty() ? "RasnClient" : result;
}

bool schema_type_is_array(const std::string &type)
{
    return type.size() > 2 && type.substr(type.size() - 2) == "[]";
}

bool schema_type_is_scalar(const std::string &type)
{
    return type == "string" || type == "bool" || type == "uint32" || type == "uint64";
}

std::string python_field_type(const schema_field_descriptor &field)
{
    if (!field.required && !schema_type_is_array(field.type) && !schema_type_is_scalar(field.type))
    {
        return "Optional[" + python_type(field.type) + "]";
    }
    return python_type(field.type);
}

std::string cpp_default_initializer(const schema_field_descriptor &field)
{
    if (field.name == "schema_version")
    {
        return " = schema_version_value";
    }
    if (field.type == "bool")
    {
        return " = false";
    }
    if (field.type == "uint32" || field.type == "uint64")
    {
        return " = 0";
    }
    return "";
}

std::string python_default_initializer(const schema_field_descriptor &field)
{
    if (field.name == "schema_version")
    {
        return " = schema_version_value";
    }
    if (schema_type_is_array(field.type))
    {
        return " = field(default_factory=list)";
    }
    if (field.type == "string")
    {
        return " = \"\"";
    }
    if (field.type == "bool")
    {
        return " = False";
    }
    if (field.type == "uint32" || field.type == "uint64")
    {
        return " = 0";
    }
    if (field.required)
    {
        return " = field(default_factory=" + field.type + ")";
    }
    return " = None";
}

void emit_cpp_comment(std::ostringstream &output, const std::string &text, const std::string &indent)
{
    if (!text.empty())
    {
        output << indent << "// " << text << "\n";
    }
}

void emit_line_comment(std::ostringstream &output, const std::string &text, const std::string &indent)
{
    if (!text.empty())
    {
        output << indent << "# " << text << "\n";
    }
}

void emit_cpp_client_method(std::ostringstream &output, const rpc_operation_descriptor &op)
{
    output << "    std::pair<::dsn::error_code, " << op.response_type << ">\n";
    output << "    " << op.method << "_sync(const " << op.request_type << " &request,\n";
    output << "                  std::chrono::milliseconds timeout = std::chrono::milliseconds(0),\n";
    output << "                  int thread_hash = 0,\n";
    output << "                  std::uint64_t partition_hash = 0)\n";
    output << "    {\n";
    output << "        return ::dsn::rpc::wait_and_unwrap<" << op.response_type << ">(::dsn::rpc::call(\n";
    output << "            _server, " << op.task_code
           << ", request, nullptr, empty_callback, timeout, thread_hash, partition_hash));\n";
    output << "    }\n\n";
}

} // namespace

std::vector<schema_type_descriptor> rasn_schema_manifest()
{
    std::vector<schema_type_descriptor> manifest;
    manifest.push_back(schema(
        "agent_task",
        RASN_AGENT_SCHEMA_VERSION,
        "Stable task identity passed across runtime, model, tool, and workflow boundaries.",
        std::vector<schema_field_descriptor>{
            field("id", "string", true, "Trace-local task identifier."),
            field("name", "string", true, "Human-readable operation name."),
            field("input", "string", false, "Original user or workflow input.")}));
    manifest.push_back(schema(
        "agent_error",
        RASN_AGENT_SCHEMA_VERSION,
        "Structured error payload carried by agent responses.",
        std::vector<schema_field_descriptor>{
            field("schema_version", "uint32", true),
            field("failure_class", "string", true),
            field("code", "string", true),
            field("message", "string", true),
            field("retryable", "bool", true),
            field("rdsn_error", "string", false),
            field("source", "string", false)}));
    manifest.push_back(schema(
        "agent_artifact",
        RASN_AGENT_SCHEMA_VERSION,
        "Durable artifact reference returned by tools, workflows, or model adapters.",
        std::vector<schema_field_descriptor>{
            field("schema_version", "uint32", true),
            field("id", "string", true),
            field("kind", "string", true),
            field("uri", "string", true),
            field("mime_type", "string", false),
            field("size", "uint64", false),
            field("digest", "string", false)}));
    manifest.push_back(schema(
        "agent_context_entry",
        RASN_AGENT_SCHEMA_VERSION,
        "Context item attached to a model or generic agent request.",
        std::vector<schema_field_descriptor>{
            field("schema_version", "uint32", true),
            field("kind", "string", true),
            field("name", "string", true),
            field("value", "string", false),
            field("artifact_id", "string", false)}));
    manifest.push_back(schema(
        "agent_capability",
        RASN_AGENT_SCHEMA_VERSION,
        "Capability advertised by an agent descriptor for registry routing.",
        std::vector<schema_field_descriptor>{
            field("schema_version", "uint32", true),
            field("name", "string", true),
            field("input_type", "string", false),
            field("output_type", "string", false),
            field("side_effect_class", "string", false),
            field("cost_hint", "uint32", false),
            field("latency_hint_ms", "uint32", false),
            field("reliability_hint", "uint32", false)}));
    manifest.push_back(schema(
        "agent_descriptor",
        RASN_AGENT_SCHEMA_VERSION,
        "Registry descriptor for an agent service instance and its capabilities.",
        std::vector<schema_field_descriptor>{
            field("schema_version", "uint32", true),
            field("agent_id", "string", true),
            field("role", "string", true),
            field("app_name", "string", false),
            field("host", "string", false),
            field("port", "uint32", false),
            field("endpoint_uri", "string", false),
            field("version", "string", false),
            field("health", "string", false),
            field("capabilities", "agent_capability[]", false)}));
    manifest.push_back(schema(
        "agent_request",
        RASN_AGENT_SCHEMA_VERSION,
        "Generic versioned request envelope for model, tool, and coordinator agents.",
        std::vector<schema_field_descriptor>{
            field("schema_version", "uint32", true),
            field("request_id", "string", true),
            field("parent_request_id", "string", false),
            field("trace_id", "string", true),
            field("workflow_id", "string", false),
            field("workflow_node_id", "string", false),
            field("task", "agent_task", true),
            field("capability", "string", true),
            field("input", "string", false),
            field("context", "agent_context_entry[]", false),
            field("timeout_ms", "uint32", false),
            field("retry_budget", "uint32", false),
            field("policy_labels", "string[]", false, "Includes explicit approvals such as human_approved:write."),
            field("replay_mode", "string", false)}));
    manifest.push_back(schema(
        "agent_response",
        RASN_AGENT_SCHEMA_VERSION,
        "Generic response envelope with structured errors and artifact/state references.",
        std::vector<schema_field_descriptor>{
            field("schema_version", "uint32", true),
            field("request_id", "string", true),
            field("trace_id", "string", true),
            field("ok", "bool", true),
            field("output", "string", false),
            field("artifacts", "agent_artifact[]", false),
            field("error", "agent_error", false),
            field("state_refs", "string[]", false),
            field("trace_summary", "string", false)}));
    manifest.push_back(schema(
        "model_provider_descriptor",
        RASN_AGENT_SCHEMA_VERSION,
        "Model-provider descriptor returned by the model gateway without exposing credential values.",
        std::vector<schema_field_descriptor>{
            field("schema_version", "uint32", true),
            field("provider", "string", true),
            field("model", "string", false),
            field("endpoint", "string", false),
            field("payload_format", "string", false),
            field("token_env", "string", false, "Legacy environment-variable handle; values are never exposed."),
            field("token_command_ref", "string", false, "Reference to a command handle, not command output."),
            field("credential_ref", "string", false, "Opaque credential handle such as env:, file:, or cmd:<configured>."),
            field("token_required", "bool", false),
            field("local", "bool", false),
            field("streaming", "bool", false),
            field("health", "string", false)}));
    manifest.push_back(schema(
        "model_provider_request",
        RASN_AGENT_SCHEMA_VERSION,
        "Request to switch the active model provider.",
        std::vector<schema_field_descriptor>{
            field("schema_version", "uint32", true),
            field("provider", "string", true)}));
    manifest.push_back(schema(
        "model_gateway_response",
        RASN_AGENT_SCHEMA_VERSION,
        "Model gateway response carrying provider status and safe credential handles.",
        std::vector<schema_field_descriptor>{
            field("schema_version", "uint32", true),
            field("ok", "bool", true),
            field("error", "string", false),
            field("provider", "model_provider_descriptor", false)}));
    manifest.push_back(schema(
        "tool_argument_descriptor",
        RASN_AGENT_SCHEMA_VERSION,
        "Argument descriptor for a structured tool contract.",
        std::vector<schema_field_descriptor>{
            field("name", "string", true),
            field("required", "bool", true),
            field("description", "string", false)}));
    manifest.push_back(schema(
        "tool_descriptor",
        RASN_AGENT_SCHEMA_VERSION,
        "Structured local-tool contract exposed by tool providers.",
        std::vector<schema_field_descriptor>{
            field("name", "string", true),
            field("side_effect", "string", true),
            field("description", "string", false),
            field("arguments", "tool_argument_descriptor[]", false)}));
    manifest.push_back(schema(
        "policy_request",
        RASN_AGENT_SCHEMA_VERSION,
        "Policy manager input for local tool side-effect decisions.",
        std::vector<schema_field_descriptor>{
            field("schema_version", "uint32", true),
            field("tool_name", "string", true),
            field("args", "string[]", false),
            field("side_effect", "string", true),
            field("target", "string", false),
            field("actor", "string", false),
            field("policy_labels", "string[]", false)}));
    manifest.push_back(schema(
        "policy_decision",
        RASN_AGENT_SCHEMA_VERSION,
        "Policy manager result for a local tool side-effect request.",
        std::vector<schema_field_descriptor>{
            field("schema_version", "uint32", true),
            field("allowed", "bool", true),
            field("reason", "string", false),
            field("side_effect", "string", false),
            field("policy_name", "string", false)}));
    manifest.push_back(schema(
        "runtime_event",
        RASN_OBSERVABILITY_SCHEMA_VERSION,
        "Append-only observability event used for diagnosis and replay; text values pass through policy redaction before persistence.",
        std::vector<schema_field_descriptor>{
            field("schema_version", "uint32", true),
            field("sequence", "uint64", true),
            field("trace_id", "string", true),
            field("task_id", "string", false),
            field("kind", "string", true),
            field("name", "string", true),
            field("value", "string", false),
            field("timestamp", "string", true),
            field("failure_class", "string", false),
            field("failure_code", "string", false),
            field("failure_source", "string", false),
            field("retryable", "bool", false),
            field("retry_attempt", "uint32", false)}));
    manifest.push_back(schema(
        "failure_record",
        RASN_OBSERVABILITY_SCHEMA_VERSION,
        "Derived failure view for observability queries and debugging summaries.",
        std::vector<schema_field_descriptor>{
            field("schema_version", "uint32", true),
            field("sequence", "uint64", true),
            field("trace_id", "string", true),
            field("task_id", "string", false),
            field("failure_class", "string", true),
            field("code", "string", true),
            field("message", "string", false),
            field("retryable", "bool", false),
            field("retry_attempt", "uint32", false),
            field("source", "string", false),
            field("timestamp", "string", false)}));
    manifest.push_back(schema(
        "observability_query_request",
        RASN_OBSERVABILITY_SCHEMA_VERSION,
        "Filter for querying runtime events or derived failures.",
        std::vector<schema_field_descriptor>{
            field("schema_version", "uint32", true),
            field("trace_id", "string", false),
            field("kind", "string", false),
            field("name", "string", false),
            field("min_sequence", "uint64", false),
            field("limit", "uint32", false)}));
    manifest.push_back(schema(
        "observability_response",
        RASN_OBSERVABILITY_SCHEMA_VERSION,
        "Event/failure query response returned by observability APIs.",
        std::vector<schema_field_descriptor>{
            field("schema_version", "uint32", true),
            field("ok", "bool", true),
            field("error", "string", false),
            field("events", "runtime_event[]", false),
            field("failures", "failure_record[]", false),
            field("last_sequence", "uint64", false),
            field("truncated", "bool", false)}));
    manifest.push_back(schema(
        "redaction_policy",
        RASN_AGENT_SCHEMA_VERSION,
        "Config-backed policy for removing credential material before traces, artifacts, or model-provider prompts persist or cross boundaries.",
        std::vector<schema_field_descriptor>{
            field("redaction_enabled", "bool", true),
            field("redact_env_names", "string[]", false, "Environment variable names whose values are treated as exact secrets."),
            field("redact_literal_values", "string[]", false, "Configured literal values to remove from text surfaces."),
            field("redact_min_secret_length", "uint64", false, "Minimum exact-match secret length.")}));
    manifest.push_back(schema(
        "state_record",
        RASN_AGENT_SCHEMA_VERSION,
        "Namespaced durable state cell persisted through checkpoints and journals.",
        std::vector<schema_field_descriptor>{
            field("schema_version", "uint32", true),
            field("key", "string", true),
            field("kind", "string", false),
            field("scope", "string", false),
            field("value", "string", false),
            field("sequence", "uint64", true)}));
    manifest.push_back(schema(
        "state_put_request",
        RASN_AGENT_SCHEMA_VERSION,
        "Conditional state write request used for CAS and lease ownership.",
        std::vector<schema_field_descriptor>{
            field("schema_version", "uint32", true),
            field("record", "state_record", true),
            field("create_only", "bool", false),
            field("check_sequence", "bool", false),
            field("expected_sequence", "uint64", false)}));
    manifest.push_back(schema(
        "state_key_request",
        RASN_AGENT_SCHEMA_VERSION,
        "State lookup request for a single key.",
        std::vector<schema_field_descriptor>{
            field("schema_version", "uint32", true),
            field("key", "string", true)}));
    manifest.push_back(schema(
        "state_query_request",
        RASN_AGENT_SCHEMA_VERSION,
        "State query request over a key prefix.",
        std::vector<schema_field_descriptor>{
            field("schema_version", "uint32", true),
            field("key_prefix", "string", false)}));
    manifest.push_back(schema(
        "state_checkpoint_request",
        RASN_AGENT_SCHEMA_VERSION,
        "Checkpoint or recovery request naming a state snapshot path.",
        std::vector<schema_field_descriptor>{
            field("schema_version", "uint32", true),
            field("path", "string", false)}));
    manifest.push_back(schema(
        "state_response",
        RASN_AGENT_SCHEMA_VERSION,
        "State service response carrying records, errors, and latest sequence.",
        std::vector<schema_field_descriptor>{
            field("schema_version", "uint32", true),
            field("ok", "bool", true),
            field("error", "string", false),
            field("record", "state_record", false),
            field("records", "state_record[]", false),
            field("last_sequence", "uint64", false)}));
    manifest.push_back(schema(
        "workflow_node",
        RASN_AGENT_SCHEMA_VERSION,
        "Declarative workflow node compiled into an executable agent graph.",
        std::vector<schema_field_descriptor>{
            field("id", "string", true),
            field("action", "string", true),
            field("prompt", "string", false),
            field("depends_on", "string[]", false),
            field("capability", "string", false),
            field("policy_labels", "string[]", false),
            field("budget_ms", "uint64", false),
            field("retry_budget", "uint32", false),
            field("cost_hint", "uint32", false),
            field("latency_hint_ms", "uint32", false),
            field("reliability_hint", "uint32", false),
            field("state_key", "string", false),
            field("artifact", "string", false)}));
    manifest.push_back(schema(
        "workflow_node_status",
        RASN_AGENT_SCHEMA_VERSION,
        "Execution status for one workflow node.",
        std::vector<schema_field_descriptor>{
            field("node_id", "string", true),
            field("action", "string", true),
            field("status", "string", true),
            field("output", "string", false),
            field("error", "string", false)}));
    manifest.push_back(schema(
        "workflow_result",
        RASN_AGENT_SCHEMA_VERSION,
        "Workflow execution result with terminal status and per-node outcomes.",
        std::vector<schema_field_descriptor>{
            field("ok", "bool", true),
            field("cancelled", "bool", false),
            field("text", "string", false),
            field("error", "string", false),
            field("nodes", "workflow_node_status[]", false)}));
    return manifest;
}

std::vector<rpc_operation_descriptor> rasn_rpc_operation_manifest()
{
    std::vector<rpc_operation_descriptor> operations;
    operations.push_back(operation("rasn.agent", "agent_rpc_client", "describe", "RPC_RASN_AGENT_DESCRIBE", "std::string", "agent_descriptor"));
    operations.push_back(operation("rasn.agent", "agent_rpc_client", "invoke", "RPC_RASN_AGENT_INVOKE", "agent_request", "agent_response"));
    operations.push_back(operation("rasn.agent", "agent_rpc_client", "cancel", "RPC_RASN_AGENT_CANCEL", "agent_request", "agent_response"));
    operations.push_back(operation("rasn.agent", "agent_rpc_client", "heartbeat", "RPC_RASN_AGENT_HEARTBEAT", "std::string", "agent_descriptor"));
    operations.push_back(operation("rasn.agent", "agent_rpc_client", "query", "RPC_RASN_AGENT_QUERY", "std::string", "agent_descriptor"));

    operations.push_back(operation("rasn.registry", "registry_rpc_client", "register_agent", "RPC_RASN_REGISTRY_REGISTER", "agent_descriptor", "agent_response"));
    operations.push_back(operation("rasn.registry", "registry_rpc_client", "unregister_agent", "RPC_RASN_REGISTRY_UNREGISTER", "std::string", "agent_response"));
    operations.push_back(operation("rasn.registry", "registry_rpc_client", "query", "RPC_RASN_REGISTRY_QUERY", "registry_query_request", "registry_query_response"));
    operations.push_back(operation("rasn.registry", "registry_rpc_client", "list", "RPC_RASN_REGISTRY_LIST", "std::string", "registry_query_response"));
    operations.push_back(operation("rasn.registry", "registry_rpc_client", "heartbeat", "RPC_RASN_REGISTRY_HEARTBEAT", "agent_descriptor", "agent_response"));

    operations.push_back(operation("rasn.state", "state_rpc_client", "put", "RPC_RASN_STATE_PUT", "state_record", "state_response"));
    operations.push_back(operation("rasn.state", "state_rpc_client", "put_conditional", "RPC_RASN_STATE_PUT_CONDITIONAL", "state_put_request", "state_response"));
    operations.push_back(operation("rasn.state", "state_rpc_client", "get", "RPC_RASN_STATE_GET", "state_key_request", "state_response"));
    operations.push_back(operation("rasn.state", "state_rpc_client", "query", "RPC_RASN_STATE_QUERY", "state_query_request", "state_response"));
    operations.push_back(operation("rasn.state", "state_rpc_client", "checkpoint", "RPC_RASN_STATE_CHECKPOINT", "state_checkpoint_request", "state_response"));
    operations.push_back(operation("rasn.state", "state_rpc_client", "recover", "RPC_RASN_STATE_RECOVER", "state_checkpoint_request", "state_response"));

    operations.push_back(operation("rasn.workflow", "workflow_rpc_client", "validate", "RPC_RASN_WORKFLOW_VALIDATE", "workflow_source", "workflow_response"));
    operations.push_back(operation("rasn.workflow", "workflow_rpc_client", "compile", "RPC_RASN_WORKFLOW_COMPILE", "workflow_source", "workflow_response"));
    operations.push_back(operation("rasn.workflow", "workflow_rpc_client", "start", "RPC_RASN_WORKFLOW_START", "workflow_start_request", "workflow_response"));
    operations.push_back(operation("rasn.workflow", "workflow_rpc_client", "query", "RPC_RASN_WORKFLOW_QUERY", "workflow_run_query", "workflow_response"));
    operations.push_back(operation("rasn.workflow", "workflow_rpc_client", "cancel", "RPC_RASN_WORKFLOW_CANCEL", "workflow_run_query", "workflow_response"));

    operations.push_back(operation("rasn.model", "model_gateway_rpc_client", "describe", "RPC_RASN_MODEL_DESCRIBE", "std::string", "model_gateway_response"));
    operations.push_back(operation("rasn.model", "model_gateway_rpc_client", "set_provider", "RPC_RASN_MODEL_SET_PROVIDER", "model_provider_request", "model_gateway_response"));
    operations.push_back(operation("rasn.model", "model_gateway_rpc_client", "health", "RPC_RASN_MODEL_HEALTH", "std::string", "model_gateway_response"));

    operations.push_back(operation("rasn.observability", "observability_rpc_client", "query", "RPC_RASN_OBSERVABILITY_QUERY", "observability_query_request", "observability_response"));
    operations.push_back(operation("rasn.observability", "observability_rpc_client", "failures", "RPC_RASN_OBSERVABILITY_FAILURES", "observability_query_request", "observability_response"));
    operations.push_back(operation("rasn.observability", "observability_rpc_client", "load_replay", "RPC_RASN_OBSERVABILITY_LOAD_REPLAY", "replay_load_request", "observability_response"));
    operations.push_back(operation("rasn.observability", "observability_rpc_client", "snapshot", "RPC_RASN_OBSERVABILITY_SNAPSHOT", "std::string", "observability_response"));
    return operations;
}

std::string rasn_schema_manifest_text()
{
    std::ostringstream output;
    output << "rASN schema manifest\n";
    for (const schema_type_descriptor &type : rasn_schema_manifest())
    {
        output << "\n[" << type.name << "] version=" << type.version << "\n";
        output << type.purpose << "\n";
        for (const schema_field_descriptor &field : type.fields)
        {
            output << "- " << field.name << ": " << field.type
                   << (field.required ? " required" : " optional");
            if (!field.notes.empty())
            {
                output << " -- " << field.notes;
            }
            output << "\n";
        }
    }
    return output.str();
}

std::string rasn_schema_manifest_json()
{
    std::ostringstream output;
    output << "{\n  \"schema_manifest_version\":1,\n  \"types\":[";
    const std::vector<schema_type_descriptor> manifest = rasn_schema_manifest();
    for (size_t i = 0; i < manifest.size(); ++i)
    {
        const schema_type_descriptor &type = manifest[i];
        if (i != 0)
        {
            output << ",";
        }
        output << "\n    {\"name\":\"" << json_escape(type.name)
               << "\",\"version\":" << type.version
               << ",\"purpose\":\"" << json_escape(type.purpose)
               << "\",\"fields\":[";
        for (size_t j = 0; j < type.fields.size(); ++j)
        {
            const schema_field_descriptor &field = type.fields[j];
            if (j != 0)
            {
                output << ",";
            }
            output << "{\"name\":\"" << json_escape(field.name)
                   << "\",\"type\":\"" << json_escape(field.type)
                   << "\",\"required\":" << (field.required ? "true" : "false")
                   << ",\"notes\":\"" << json_escape(field.notes) << "\"}";
        }
        output << "]}";
    }
    output << "\n  ]\n}\n";
    return output.str();
}

std::string rasn_schema_manifest_idl()
{
    std::ostringstream output;
    output << "namespace rasn;\n";
    output << "schema_manifest_version 1;\n\n";
    for (const schema_type_descriptor &type : rasn_schema_manifest())
    {
        output << "record " << type.name << " version " << type.version << " {\n";
        for (const schema_field_descriptor &field : type.fields)
        {
            output << "  " << (field.required ? "required " : "optional ")
                   << idl_type(field.type) << " " << field.name << ";\n";
        }
        output << "}\n\n";
    }
    return output.str();
}

std::string rasn_schema_manifest_cpp_header()
{
    std::ostringstream output;
    output << "#ifndef RASN_GENERATED_SCHEMA_H\n";
    output << "#define RASN_GENERATED_SCHEMA_H\n\n";
    output << "#include <cstdint>\n";
    output << "#include <string>\n";
    output << "#include <vector>\n\n";
    output << "namespace dsn {\n";
    output << "namespace rasn {\n";
    output << "namespace generated {\n\n";
    output << "constexpr std::uint32_t RASN_SCHEMA_MANIFEST_VERSION = 1;\n\n";

    for (const schema_type_descriptor &type : rasn_schema_manifest())
    {
        emit_cpp_comment(output, type.purpose, "");
        output << "struct " << type.name << "\n";
        output << "{\n";
        output << "    static constexpr std::uint32_t schema_version_value = " << type.version << ";\n";
        for (const schema_field_descriptor &field : type.fields)
        {
            emit_cpp_comment(output, field.notes, "    ");
            output << "    " << cpp_type(field.type) << " " << field.name
                   << cpp_default_initializer(field) << ";\n";
        }
        output << "};\n\n";
    }

    output << "} // namespace generated\n";
    output << "} // namespace rasn\n";
    output << "} // namespace dsn\n\n";
    output << "#endif // RASN_GENERATED_SCHEMA_H\n";
    return output.str();
}

std::string rasn_schema_manifest_cpp_clients()
{
    std::ostringstream output;
    output << "#ifndef RASN_GENERATED_RPC_CLIENTS_H\n";
    output << "#define RASN_GENERATED_RPC_CLIENTS_H\n\n";
    output << "// Generated from the rASN RPC operation manifest. Do not edit by hand.\n";
    output << "#include \"agent_messages.h\"\n";
    output << "#include \"agent_registry.h\"\n";
    output << "#include \"agent_types.h\"\n";
    output << "#include \"model_agent.h\"\n";
    output << "#include \"observability.h\"\n";
    output << "#include \"rasn.code.definition.h\"\n";
    output << "#include \"state_service.h\"\n";
    output << "#include \"workflow_service.h\"\n\n";
    output << "#include <dsn/service_api_cpp.h>\n\n";
    output << "#include <chrono>\n";
    output << "#include <cstdint>\n";
    output << "#include <string>\n";
    output << "#include <utility>\n\n";
    output << "namespace dsn {\n";
    output << "namespace rasn {\n";
    output << "namespace generated {\n\n";
    output << "constexpr std::uint32_t RASN_RPC_CLIENT_MANIFEST_VERSION = 1;\n\n";

    std::string current_class;
    for (const rpc_operation_descriptor &op : rasn_rpc_operation_manifest())
    {
        if (op.client_class != current_class)
        {
            if (!current_class.empty())
            {
                output << "private:\n";
                output << "    ::dsn::rpc_address _server;\n";
                output << "};\n\n";
            }
            current_class = op.client_class;
            output << "class " << current_class << " : public virtual ::dsn::clientlet\n";
            output << "{\n";
            output << "public:\n";
            output << "    explicit " << current_class << "(::dsn::rpc_address server) : _server(server) {}\n\n";
        }
        output << "    // " << op.service << "." << op.method << " -> " << op.task_code << "\n";
        emit_cpp_client_method(output, op);
    }

    if (!current_class.empty())
    {
        output << "private:\n";
        output << "    ::dsn::rpc_address _server;\n";
        output << "};\n\n";
    }

    output << "} // namespace generated\n";
    output << "} // namespace rasn\n";
    output << "} // namespace dsn\n\n";
    output << "#endif // RASN_GENERATED_RPC_CLIENTS_H\n";
    return output.str();
}

std::string rasn_schema_manifest_typescript()
{
    std::ostringstream output;
    output << "// Generated from the rASN schema manifest. Do not edit by hand.\n";
    output << "export const RASN_SCHEMA_MANIFEST_VERSION = 1 as const;\n\n";

    for (const schema_type_descriptor &type : rasn_schema_manifest())
    {
        emit_cpp_comment(output, type.purpose, "");
        output << "export const " << type.name << "_schema_version = " << type.version << " as const;\n";
        output << "export interface " << type.name << " {\n";
        for (const schema_field_descriptor &field : type.fields)
        {
            emit_cpp_comment(output, field.notes, "  ");
            output << "  " << field.name << (field.required ? "" : "?") << ": "
                   << typescript_type(field.type) << ";\n";
        }
        output << "}\n\n";
    }

    return output.str();
}

std::string rasn_schema_manifest_typescript_clients()
{
    std::ostringstream output;
    output << "// Generated from the rASN RPC operation manifest. Do not edit by hand.\n";
    output << "import type {\n";
    for (const schema_type_descriptor &type : rasn_schema_manifest())
    {
        output << "  " << type.name << ",\n";
    }
    output << "} from './rasn-generated-schema';\n\n";
    output << "export const RASN_RPC_CLIENT_MANIFEST_VERSION = 1 as const;\n\n";
    output << "export interface RasnRpcTransport {\n";
    output << "  call<TRequest, TResponse>(taskCode: string, request: TRequest, timeoutMs?: number): Promise<TResponse>;\n";
    output << "}\n\n";

    std::string current_class;
    for (const rpc_operation_descriptor &op : rasn_rpc_operation_manifest())
    {
        if (op.client_class != current_class)
        {
            if (!current_class.empty())
            {
                output << "}\n\n";
            }
            current_class = op.client_class;
            output << "export class " << pascal_case_identifier(current_class) << " {\n";
            output << "  constructor(private readonly transport: RasnRpcTransport) {}\n\n";
        }
        output << "  // " << op.service << "." << op.method << " -> " << op.task_code << "\n";
        output << "  " << op.method << "(request: " << typescript_client_type(op.request_type)
               << ", timeoutMs?: number): Promise<" << typescript_client_type(op.response_type) << "> {\n";
        output << "    return this.transport.call<" << typescript_client_type(op.request_type)
               << ", " << typescript_client_type(op.response_type) << ">(\"" << op.task_code
               << "\", request, timeoutMs);\n";
        output << "  }\n\n";
    }
    if (!current_class.empty())
    {
        output << "}\n";
    }
    return output.str();
}

std::string rasn_schema_manifest_python()
{
    std::ostringstream output;
    output << "# Generated from the rASN schema manifest. Do not edit by hand.\n";
    output << "from dataclasses import dataclass, field\n";
    output << "from typing import ClassVar, List, Optional\n\n";
    output << "RASN_SCHEMA_MANIFEST_VERSION = 1\n\n";

    for (const schema_type_descriptor &type : rasn_schema_manifest())
    {
        emit_line_comment(output, type.purpose, "");
        output << "@dataclass\n";
        output << "class " << type.name << ":\n";
        output << "    schema_version_value: ClassVar[int] = " << type.version << "\n";
        if (type.fields.empty())
        {
            output << "    pass\n";
        }
        for (const schema_field_descriptor &field : type.fields)
        {
            emit_line_comment(output, field.notes, "    ");
            output << "    " << field.name << ": " << python_field_type(field)
                   << python_default_initializer(field) << "\n";
        }
        output << "\n";
    }

    return output.str();
}

std::string rasn_schema_manifest_python_clients()
{
    std::ostringstream output;
    output << "# Generated from the rASN RPC operation manifest. Do not edit by hand.\n";
    output << "from typing import Any, Optional, Protocol, TypeVar, cast\n\n";
    output << "from rasn_generated_schema import *\n\n";
    output << "TResponse = TypeVar(\"TResponse\")\n\n";
    output << "RASN_RPC_CLIENT_MANIFEST_VERSION = 1\n\n";
    output << "class RasnRpcTransport(Protocol):\n";
    output << "    def call(self, task_code: str, request: Any, timeout_ms: Optional[int] = None) -> Any:\n";
    output << "        ...\n\n";

    std::string current_class;
    for (const rpc_operation_descriptor &op : rasn_rpc_operation_manifest())
    {
        if (op.client_class != current_class)
        {
            if (!current_class.empty())
            {
                output << "\n";
            }
            current_class = op.client_class;
            output << "class " << pascal_case_identifier(current_class) << ":\n";
            output << "    def __init__(self, transport: RasnRpcTransport) -> None:\n";
            output << "        self._transport = transport\n\n";
        }
        output << "    # " << op.service << "." << op.method << " -> " << op.task_code << "\n";
        output << "    def " << op.method << "(self, request: " << python_client_type(op.request_type)
               << ", timeout_ms: Optional[int] = None) -> " << python_client_type(op.response_type) << ":\n";
        output << "        return cast(" << python_client_type(op.response_type)
               << ", self._transport.call(\"" << op.task_code << "\", request, timeout_ms))\n\n";
    }
    return output.str();
}

} // namespace rasn
} // namespace dsn
