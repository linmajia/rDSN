#pragma once

#include "rasn_core.h"

#include <dsn/cpp/address.h>
#include <dsn/cpp/serialization.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

const uint32_t RASN_AGENT_SCHEMA_VERSION = 1;

struct agent_error
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    std::string failure_class;
    std::string code;
    std::string message;
    bool retryable = false;
    std::string rdsn_error;
    std::string source;
};

struct agent_artifact
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    std::string id;
    std::string kind;
    std::string uri;
    std::string mime_type;
    uint64_t size = 0;
    std::string digest;
};

struct agent_context_entry
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    std::string kind;
    std::string name;
    std::string value;
    std::string artifact_id;
};

struct agent_capability
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    std::string name;
    std::string input_type;
    std::string output_type;
    std::string side_effect_class;
    uint32_t cost_hint = 0;
    uint32_t latency_hint_ms = 0;
    uint32_t reliability_hint = 0;
};

struct agent_descriptor
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    std::string agent_id;
    std::string role;
    std::string app_name;
    std::string host;
    uint32_t port = 0;
    std::string endpoint_uri;
    std::string version;
    std::string health;
    std::vector<agent_capability> capabilities;
};

struct agent_request
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    std::string request_id;
    std::string parent_request_id;
    std::string trace_id;
    std::string workflow_id;
    std::string workflow_node_id;
    agent_task task;
    std::string capability;
    std::string input;
    std::vector<agent_context_entry> context;
    uint32_t timeout_ms = 0;
    uint32_t retry_budget = 0;
    std::vector<std::string> policy_labels;
    std::string replay_mode;
};

struct agent_response
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    std::string request_id;
    std::string trace_id;
    bool ok = false;
    std::string output;
    std::vector<agent_artifact> artifacts;
    agent_error error;
    std::vector<std::string> state_refs;
    std::string trace_summary;
};

inline bool validate_agent_request(const agent_request &request, std::string *error)
{
    if (request.schema_version == 0)
    {
        if (error != nullptr)
        {
            *error = "missing agent request schema version";
        }
        return false;
    }
    if (request.request_id.empty())
    {
        if (error != nullptr)
        {
            *error = "missing agent request id";
        }
        return false;
    }
    if (request.trace_id.empty())
    {
        if (error != nullptr)
        {
            *error = "missing agent request trace id";
        }
        return false;
    }
    if (request.capability.empty())
    {
        if (error != nullptr)
        {
            *error = "missing agent request capability";
        }
        return false;
    }
    return true;
}

inline bool validate_agent_response(const agent_response &response, std::string *error)
{
    if (response.schema_version == 0)
    {
        if (error != nullptr)
        {
            *error = "missing agent response schema version";
        }
        return false;
    }
    if (response.request_id.empty())
    {
        if (error != nullptr)
        {
            *error = "missing agent response request id";
        }
        return false;
    }
    if (response.ok && !response.error.message.empty())
    {
        if (error != nullptr)
        {
            *error = "agent response cannot contain both success and error";
        }
        return false;
    }
    return true;
}

inline agent_error make_agent_error(const std::string &failure_class,
                                    const std::string &code,
                                    const std::string &message,
                                    bool retryable,
                                    const std::string &source)
{
    agent_error error;
    error.failure_class = failure_class;
    error.code = code;
    error.message = message;
    error.retryable = retryable;
    error.source = source;
    return error;
}

inline void rasn_marshall_string_vector(::dsn::binary_writer &writer, const std::vector<std::string> &values)
{
    writer.write(static_cast<uint32_t>(values.size()));
    for (const std::string &value : values)
    {
        writer.write(value);
    }
}

inline void rasn_unmarshall_string_vector(::dsn::binary_reader &reader, std::vector<std::string> &values)
{
    uint32_t count = 0;
    reader.read(count);
    values.clear();
    values.reserve(rasn_bounded_reserve_count<std::string>(count));
    for (uint32_t i = 0; i < count; ++i)
    {
        std::string value;
        reader.read(value);
        values.push_back(value);
    }
}

inline void marshall(::dsn::binary_writer &writer, const agent_task &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.id);
    writer.write(value.name);
    writer.write(value.input);
}

inline void unmarshall(::dsn::binary_reader &reader, agent_task &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.id);
    reader.read(value.name);
    reader.read(value.input);
}

inline void marshall(::dsn::binary_writer &writer, const agent_error &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.failure_class);
    writer.write(value.code);
    writer.write(value.message);
    writer.write(value.retryable);
    writer.write(value.rdsn_error);
    writer.write(value.source);
}

inline void unmarshall(::dsn::binary_reader &reader, agent_error &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.failure_class);
    reader.read(value.code);
    reader.read(value.message);
    reader.read(value.retryable);
    reader.read(value.rdsn_error);
    reader.read(value.source);
}

inline void marshall(::dsn::binary_writer &writer, const agent_artifact &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.id);
    writer.write(value.kind);
    writer.write(value.uri);
    writer.write(value.mime_type);
    writer.write(value.size);
    writer.write(value.digest);
}

inline void unmarshall(::dsn::binary_reader &reader, agent_artifact &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.id);
    reader.read(value.kind);
    reader.read(value.uri);
    reader.read(value.mime_type);
    reader.read(value.size);
    reader.read(value.digest);
}

inline void marshall(::dsn::binary_writer &writer, const agent_context_entry &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.kind);
    writer.write(value.name);
    writer.write(value.value);
    writer.write(value.artifact_id);
}

inline void unmarshall(::dsn::binary_reader &reader, agent_context_entry &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.kind);
    reader.read(value.name);
    reader.read(value.value);
    reader.read(value.artifact_id);
}

inline void marshall(::dsn::binary_writer &writer, const agent_capability &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.name);
    writer.write(value.input_type);
    writer.write(value.output_type);
    writer.write(value.side_effect_class);
    writer.write(value.cost_hint);
    writer.write(value.latency_hint_ms);
    writer.write(value.reliability_hint);
}

inline void unmarshall(::dsn::binary_reader &reader, agent_capability &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.name);
    reader.read(value.input_type);
    reader.read(value.output_type);
    reader.read(value.side_effect_class);
    reader.read(value.cost_hint);
    reader.read(value.latency_hint_ms);
    reader.read(value.reliability_hint);
}

inline void rasn_marshall_capabilities(::dsn::binary_writer &writer, const std::vector<agent_capability> &values, ::dsn_msg_serialize_format fmt)
{
    writer.write(static_cast<uint32_t>(values.size()));
    for (const agent_capability &value : values)
    {
        marshall(writer, value, fmt);
    }
}

inline void rasn_unmarshall_capabilities(::dsn::binary_reader &reader, std::vector<agent_capability> &values, ::dsn_msg_serialize_format fmt)
{
    uint32_t count = 0;
    reader.read(count);
    values.clear();
    values.reserve(rasn_bounded_reserve_count<agent_capability>(count));
    for (uint32_t i = 0; i < count; ++i)
    {
        agent_capability value;
        unmarshall(reader, value, fmt);
        values.push_back(value);
    }
}

inline void marshall(::dsn::binary_writer &writer, const agent_descriptor &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.agent_id);
    writer.write(value.role);
    writer.write(value.app_name);
    writer.write(value.host);
    writer.write(value.port);
    writer.write(value.endpoint_uri);
    writer.write(value.version);
    writer.write(value.health);
    rasn_marshall_capabilities(writer, value.capabilities, fmt);
}

inline void unmarshall(::dsn::binary_reader &reader, agent_descriptor &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.agent_id);
    reader.read(value.role);
    reader.read(value.app_name);
    reader.read(value.host);
    reader.read(value.port);
    reader.read(value.endpoint_uri);
    reader.read(value.version);
    reader.read(value.health);
    rasn_unmarshall_capabilities(reader, value.capabilities, fmt);
}

inline void rasn_marshall_context(::dsn::binary_writer &writer, const std::vector<agent_context_entry> &values, ::dsn_msg_serialize_format fmt)
{
    writer.write(static_cast<uint32_t>(values.size()));
    for (const agent_context_entry &value : values)
    {
        marshall(writer, value, fmt);
    }
}

inline void rasn_unmarshall_context(::dsn::binary_reader &reader, std::vector<agent_context_entry> &values, ::dsn_msg_serialize_format fmt)
{
    uint32_t count = 0;
    reader.read(count);
    values.clear();
    values.reserve(rasn_bounded_reserve_count<agent_context_entry>(count));
    for (uint32_t i = 0; i < count; ++i)
    {
        agent_context_entry value;
        unmarshall(reader, value, fmt);
        values.push_back(value);
    }
}

inline void marshall(::dsn::binary_writer &writer, const agent_request &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.request_id);
    writer.write(value.parent_request_id);
    writer.write(value.trace_id);
    writer.write(value.workflow_id);
    writer.write(value.workflow_node_id);
    marshall(writer, value.task, fmt);
    writer.write(value.capability);
    writer.write(value.input);
    rasn_marshall_context(writer, value.context, fmt);
    writer.write(value.timeout_ms);
    writer.write(value.retry_budget);
    rasn_marshall_string_vector(writer, value.policy_labels);
    writer.write(value.replay_mode);
}

inline void unmarshall(::dsn::binary_reader &reader, agent_request &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.request_id);
    reader.read(value.parent_request_id);
    reader.read(value.trace_id);
    reader.read(value.workflow_id);
    reader.read(value.workflow_node_id);
    unmarshall(reader, value.task, fmt);
    reader.read(value.capability);
    reader.read(value.input);
    rasn_unmarshall_context(reader, value.context, fmt);
    reader.read(value.timeout_ms);
    reader.read(value.retry_budget);
    rasn_unmarshall_string_vector(reader, value.policy_labels);
    reader.read(value.replay_mode);
}

inline void rasn_marshall_artifacts(::dsn::binary_writer &writer, const std::vector<agent_artifact> &values, ::dsn_msg_serialize_format fmt)
{
    writer.write(static_cast<uint32_t>(values.size()));
    for (const agent_artifact &value : values)
    {
        marshall(writer, value, fmt);
    }
}

inline void rasn_unmarshall_artifacts(::dsn::binary_reader &reader, std::vector<agent_artifact> &values, ::dsn_msg_serialize_format fmt)
{
    uint32_t count = 0;
    reader.read(count);
    values.clear();
    values.reserve(rasn_bounded_reserve_count<agent_artifact>(count));
    for (uint32_t i = 0; i < count; ++i)
    {
        agent_artifact value;
        unmarshall(reader, value, fmt);
        values.push_back(value);
    }
}

inline void marshall(::dsn::binary_writer &writer, const agent_response &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.request_id);
    writer.write(value.trace_id);
    writer.write(value.ok);
    writer.write(value.output);
    rasn_marshall_artifacts(writer, value.artifacts, fmt);
    marshall(writer, value.error, fmt);
    rasn_marshall_string_vector(writer, value.state_refs);
    writer.write(value.trace_summary);
}

inline void unmarshall(::dsn::binary_reader &reader, agent_response &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.request_id);
    reader.read(value.trace_id);
    reader.read(value.ok);
    reader.read(value.output);
    rasn_unmarshall_artifacts(reader, value.artifacts, fmt);
    unmarshall(reader, value.error, fmt);
    rasn_unmarshall_string_vector(reader, value.state_refs);
    reader.read(value.trace_summary);
}

} // namespace rasn
} // namespace dsn
