#pragma once

#include "agent_tools.h"
#include "llm_provider.h"

#include <functional>
#include <istream>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

struct workflow_node
{
    std::string id;
    std::string action;
    std::string prompt;
    std::vector<std::string> depends_on;
    std::string capability;
    std::vector<std::string> policy_labels;
    uint64_t budget_ms = 0;
    uint32_t retry_budget = 0;
    uint32_t cost_hint = 0;
    uint32_t latency_hint_ms = 0;
    uint32_t reliability_hint = 0;
    std::string state_key;
    std::string artifact;
};

struct workflow_node_status
{
    std::string node_id;
    std::string action;
    std::string status;
    std::string output;
    std::string error;
};

struct workflow_result
{
    bool ok;
    bool cancelled = false;
    std::string text;
    std::string error;
    std::vector<workflow_node_status> nodes;
};

class workflow_graph
{
public:
    bool load_from_file(const std::string &path, std::string *error);
    bool load_from_text(const std::string &text, const std::string &source_name, std::string *error);
    bool add_node(const workflow_node &node, std::string *error);

    std::vector<workflow_node> nodes() const { return _nodes; }
    typedef std::function<tool_result(const std::string &name,
                                      const std::vector<std::string> &args,
                                      nucleus_runtime &runtime,
                                      const agent_task &task,
                                      uint32_t timeout_ms)> workflow_tool_runner;
    typedef std::function<void(const workflow_node_status &status)> workflow_progress_observer;
    typedef std::function<bool()> workflow_cancel_checker;
    typedef std::map<std::string, workflow_node_status> workflow_resume_state;
    workflow_result execute(llm_provider &provider,
                            nucleus_runtime &runtime,
                            const workflow_tool_runner &tool_runner = workflow_tool_runner(),
                            const workflow_progress_observer &progress = workflow_progress_observer(),
                            const workflow_cancel_checker &should_cancel = workflow_cancel_checker(),
                            const workflow_resume_state &resume_state = workflow_resume_state()) const;
    std::string describe_plan() const;

private:
    bool load_from_stream(std::istream &input, const std::string &source_name, std::string *error);
    bool load_from_json_text(const std::string &text, const std::string &source_name, std::string *error);
    bool topological_order(std::vector<workflow_node> *ordered, std::string *error) const;

    std::vector<workflow_node> _nodes;
};

} // namespace rasn
} // namespace dsn
