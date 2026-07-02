#include "agent_executor.h"

#include "rasn_core.h"

#include <sstream>

namespace dsn {
namespace rasn {

namespace {

std::string executor_error_message(const agent_response &response)
{
    if (!response.error.message.empty())
    {
        return response.error.message;
    }
    if (!response.output.empty())
    {
        return response.output;
    }
    return "agent model request failed";
}

std::string executor_system_prompt(const agent_executor_request &request, const agent_executor_options &options)
{
    if (options.tool_instruction.empty())
    {
        return request.system_prompt;
    }
    if (request.system_prompt.empty())
    {
        return options.tool_instruction;
    }
    return request.system_prompt + " " + options.tool_instruction;
}

} // namespace

bool parse_agent_tool_directive(const std::string &text,
                                const std::string &directive_prefix,
                                agent_executor_tool_call *tool)
{
    if (tool != nullptr)
    {
        *tool = agent_executor_tool_call();
    }
    if (directive_prefix.empty())
    {
        return false;
    }

    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line))
    {
        line = trim(line);
        if (line.find(directive_prefix) != 0)
        {
            continue;
        }

        const std::vector<std::string> words = split_words(line.substr(directive_prefix.size()));
        if (words.empty())
        {
            return false;
        }
        if (tool != nullptr)
        {
            tool->name = words[0];
            tool->args.assign(words.begin() + 1, words.end());
        }
        return true;
    }
    return false;
}

std::string format_agent_tool_observation(const agent_executor_tool_call &tool, const tool_result &result)
{
    std::ostringstream output;
    output << "Tool " << tool.name << (result.ok ? " succeeded" : " failed") << ":\n"
           << (result.ok ? result.output : result.error);
    if (!result.output.empty() && !result.ok)
    {
        output << "\nstdout/stderr:\n" << result.output;
    }
    return output.str();
}

agent_executor_result agent_plan_executor::execute(const agent_executor_request &request,
                                                   const agent_executor_options &options,
                                                   const model_callback &model,
                                                   const approval_callback &approve,
                                                   const tool_callback &tool) const
{
    agent_executor_result result;
    std::vector<std::string> context = request.context;
    std::string conversation = request.prompt;
    const std::string system_prompt = executor_system_prompt(request, options);
    const std::string request_id_prefix = request.task.id.empty() ? "agent-executor" : request.task.id;

    if (!model)
    {
        result.status = "failed";
        result.error = "agent executor missing model callback";
        return result;
    }

    for (uint32_t step = 0; step < options.max_tool_calls; ++step)
    {
        agent_executor_model_request model_request;
        model_request.step_index = step;
        model_request.request_id = request_id_prefix + "/model/" + std::to_string(step);
        model_request.conversation = conversation;
        model_request.system_prompt = system_prompt;
        model_request.context = context;

        agent_executor_step step_record;
        step_record.step_index = step;
        step_record.model_request_id = model_request.request_id;

        const agent_response response = model(model_request);
        step_record.model_output = response.output;
        if (!response.ok)
        {
            result.steps.push_back(step_record);
            result.status = "failed";
            result.error = executor_error_message(response);
            return result;
        }

        agent_executor_tool_call tool_call;
        if (!parse_agent_tool_directive(response.output, options.tool_directive_prefix, &tool_call))
        {
            result.steps.push_back(step_record);
            result.ok = true;
            result.status = "ok";
            result.output = response.output;
            return result;
        }

        step_record.requested_tool = true;
        step_record.tool = tool_call;
        std::vector<std::string> policy_labels;
        if (approve && !approve(tool_call, &policy_labels))
        {
            step_record.policy_labels = policy_labels;
            result.steps.push_back(step_record);
            result.status = "approval-denied";
            result.error = "tool denied by user approval";
            return result;
        }

        if (!tool)
        {
            result.steps.push_back(step_record);
            result.status = "failed";
            result.error = "agent executor missing tool callback";
            return result;
        }

        agent_executor_tool_request tool_request;
        tool_request.step_index = step;
        tool_request.request_id = request_id_prefix + "/tool/" + std::to_string(step);
        tool_request.tool = tool_call;
        tool_request.policy_labels = policy_labels;

        const tool_result tool_response = tool(tool_request);
        step_record.policy_labels = policy_labels;
        step_record.tool_ok = tool_response.ok;
        step_record.tool_output = tool_response.output;
        step_record.tool_error = tool_response.error;
        result.steps.push_back(step_record);

        context.push_back(format_agent_tool_observation(tool_call, tool_response));
        conversation = request.prompt + "\n\n" + options.continuation_prompt;
    }

    result.status = "tool-limit";
    result.error = "agent stopped after reaching the tool-call limit";
    return result;
}

} // namespace rasn
} // namespace dsn
