#pragma once

#include "agent_types.h"
#include "agent_tools.h"
#include "llm_provider.h"

#include <dsn/cpp/serialization.h>

#include <cstdint>
#include <vector>

namespace dsn {
namespace rasn {

struct agent_completion_request
{
    agent_task task;
    std::string system_prompt;
    std::string user_prompt;
    std::vector<std::string> context;
    uint32_t timeout_ms = 0;
    uint32_t retry_budget = 0;
};

struct agent_tool_request
{
    std::string name;
    std::vector<std::string> args;
    agent_task task;
    std::vector<std::string> policy_labels;
};

inline agent_request make_model_agent_request(const agent_completion_request &request, const std::string &trace_id)
{
    agent_request generic;
    generic.request_id = request.task.id.empty() ? "completion" : request.task.id;
    generic.trace_id = trace_id;
    generic.task = request.task;
    generic.capability = "model.complete";
    generic.input = request.user_prompt;
    generic.timeout_ms = request.timeout_ms;
    generic.retry_budget = request.retry_budget;

    if (!request.system_prompt.empty())
    {
        agent_context_entry entry;
        entry.kind = "system_prompt";
        entry.name = "system";
        entry.value = request.system_prompt;
        generic.context.push_back(entry);
    }

    for (size_t i = 0; i < request.context.size(); ++i)
    {
        agent_context_entry entry;
        entry.kind = "text";
        entry.name = "context";
        entry.value = request.context[i];
        generic.context.push_back(entry);
    }

    return generic;
}

inline agent_request make_tool_agent_request(const agent_tool_request &request, const std::string &trace_id)
{
    agent_request generic;
    generic.request_id = request.task.id.empty() ? "tool" : request.task.id;
    generic.trace_id = trace_id;
    generic.task = request.task;
    generic.capability = "tool.run";
    generic.input = request.name;
    generic.policy_labels = request.policy_labels;

    for (size_t i = 0; i < request.args.size(); ++i)
    {
        agent_context_entry entry;
        entry.kind = "argument";
        entry.name = "arg";
        entry.value = request.args[i];
        generic.context.push_back(entry);
    }

    return generic;
}

inline agent_completion_request make_completion_request_from_agent(const agent_request &request)
{
    agent_completion_request completion;
    completion.task = request.task;
    if (completion.task.id.empty())
    {
        completion.task.id = request.request_id;
    }
    if (completion.task.name.empty())
    {
        completion.task.name = request.capability;
    }
    completion.task.input = request.input;
    completion.user_prompt = request.input;
    completion.timeout_ms = request.timeout_ms;
    completion.retry_budget = request.retry_budget;

    for (const agent_context_entry &entry : request.context)
    {
        if (entry.kind == "system_prompt")
        {
            completion.system_prompt = entry.value;
        }
        else if (entry.kind == "text" || entry.kind == "context")
        {
            completion.context.push_back(entry.value);
        }
    }

    return completion;
}

inline agent_tool_request make_tool_request_from_agent(const agent_request &request)
{
    agent_tool_request tool;
    tool.task = request.task;
    if (tool.task.id.empty())
    {
        tool.task.id = request.request_id;
    }
    if (tool.task.name.empty())
    {
        tool.task.name = request.capability;
    }
    tool.task.input = request.input;
    tool.name = request.input;
    tool.policy_labels = request.policy_labels;
    if (tool.name.empty() && request.capability.find("tool.") == 0)
    {
        tool.name = request.capability.substr(5);
    }

    for (const agent_context_entry &entry : request.context)
    {
        if (entry.kind == "argument")
        {
            tool.args.push_back(entry.value);
        }
    }

    return tool;
}

inline agent_response make_agent_response_from_llm(const agent_request &request, const llm_response &response)
{
    agent_response generic;
    generic.request_id = request.request_id;
    generic.trace_id = request.trace_id;
    generic.ok = response.ok;
    generic.output = response.text;
    if (!response.ok)
    {
        generic.error = make_agent_error("provider", "llm_provider_error", response.error, true, "model_agent");
    }
    return generic;
}

inline agent_response make_agent_response_from_tool(const agent_request &request, const tool_result &result)
{
    agent_response generic;
    generic.request_id = request.request_id;
    generic.trace_id = request.trace_id;
    generic.ok = result.ok;
    generic.output = result.output;
    if (!result.ok)
    {
        generic.error = make_agent_error("tool", "tool_error", result.error, false, "tool_agent");
    }
    return generic;
}

inline llm_response make_llm_response_from_agent(const agent_response &response)
{
    llm_response result;
    result.ok = response.ok;
    result.text = response.output;
    result.error = response.error.message;
    return result;
}

inline tool_result make_tool_result_from_agent(const agent_response &response)
{
    tool_result result;
    result.ok = response.ok;
    result.output = response.output;
    result.error = response.error.message;
    return result;
}

inline void marshall_string_vector(::dsn::binary_writer &writer, const std::vector<std::string> &values)
{
    writer.write(static_cast<uint32_t>(values.size()));
    for (const std::string &value : values)
    {
        writer.write(value);
    }
}

inline void unmarshall_string_vector(::dsn::binary_reader &reader, std::vector<std::string> &values)
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

inline void marshall(::dsn::binary_writer &writer, const llm_response &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.ok);
    writer.write(value.text);
    writer.write(value.error);
}

inline void unmarshall(::dsn::binary_reader &reader, llm_response &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.ok);
    reader.read(value.text);
    reader.read(value.error);
}

inline void marshall(::dsn::binary_writer &writer, const tool_result &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.ok);
    writer.write(value.output);
    writer.write(value.error);
}

inline void unmarshall(::dsn::binary_reader &reader, tool_result &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.ok);
    reader.read(value.output);
    reader.read(value.error);
}

inline void marshall(::dsn::binary_writer &writer, const agent_completion_request &value, ::dsn_msg_serialize_format fmt)
{
    marshall(writer, value.task, fmt);
    writer.write(value.system_prompt);
    writer.write(value.user_prompt);
    marshall_string_vector(writer, value.context);
    writer.write(value.timeout_ms);
    writer.write(value.retry_budget);
}

inline void unmarshall(::dsn::binary_reader &reader, agent_completion_request &value, ::dsn_msg_serialize_format fmt)
{
    unmarshall(reader, value.task, fmt);
    reader.read(value.system_prompt);
    reader.read(value.user_prompt);
    unmarshall_string_vector(reader, value.context);
    reader.read(value.timeout_ms);
    reader.read(value.retry_budget);
}

inline void marshall(::dsn::binary_writer &writer, const agent_tool_request &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.name);
    marshall_string_vector(writer, value.args);
    marshall(writer, value.task, fmt);
}

inline void unmarshall(::dsn::binary_reader &reader, agent_tool_request &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.name);
    unmarshall_string_vector(reader, value.args);
    unmarshall(reader, value.task, fmt);
}

} // namespace rasn
} // namespace dsn
