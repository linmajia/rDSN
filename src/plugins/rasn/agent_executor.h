#pragma once

#include <rasn/agent_tools.h>
#include <rasn/agent_types.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

struct agent_executor_tool_call
{
    std::string name;
    std::vector<std::string> args;
};

struct agent_executor_model_request
{
    uint32_t step_index = 0;
    std::string request_id;
    std::string conversation;
    std::string system_prompt;
    std::vector<std::string> context;
};

struct agent_executor_tool_request
{
    uint32_t step_index = 0;
    std::string request_id;
    agent_executor_tool_call tool;
    std::vector<std::string> policy_labels;
};

struct agent_executor_step
{
    uint32_t step_index = 0;
    std::string model_request_id;
    std::string model_output;
    bool requested_tool = false;
    agent_executor_tool_call tool;
    std::vector<std::string> policy_labels;
    bool tool_ok = false;
    std::string tool_output;
    std::string tool_error;
};

struct agent_executor_options
{
    uint32_t max_tool_calls = 4;
    std::string tool_directive_prefix = "RASN_TOOL ";
    std::string tool_instruction;
    std::string continuation_prompt = "The requested tool was executed. Continue with the answer.";
};

struct agent_executor_request
{
    agent_task task;
    std::string prompt;
    std::string system_prompt;
    std::vector<std::string> context;
};

struct agent_executor_result
{
    bool ok = false;
    std::string status;
    std::string output;
    std::string error;
    std::vector<agent_executor_step> steps;
};

bool parse_agent_tool_directive(const std::string &text,
                                const std::string &directive_prefix,
                                agent_executor_tool_call *tool);
std::string format_agent_tool_observation(const agent_executor_tool_call &tool, const tool_result &result);

class agent_plan_executor
{
public:
    typedef std::function<agent_response(const agent_executor_model_request &request)> model_callback;
    typedef std::function<bool(const agent_executor_tool_call &tool,
                               std::vector<std::string> *policy_labels)> approval_callback;
    typedef std::function<tool_result(const agent_executor_tool_request &request)> tool_callback;

    agent_executor_result execute(const agent_executor_request &request,
                                  const agent_executor_options &options,
                                  const model_callback &model,
                                  const approval_callback &approve,
                                  const tool_callback &tool) const;
};

} // namespace rasn
} // namespace dsn
