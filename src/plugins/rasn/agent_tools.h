#pragma once

#include "rasn_core.h"

#include <memory>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

struct tool_result
{
    bool ok;
    std::string output;
    std::string error;
};

struct tool_argument_descriptor
{
    std::string name;
    bool required;
    std::string description;
};

struct tool_descriptor
{
    std::string name;
    std::string side_effect;
    std::string description;
    std::vector<tool_argument_descriptor> arguments;
};

class agent_tool_provider
{
public:
    virtual ~agent_tool_provider() {}
    virtual std::string describe_tools() const = 0;
    virtual std::vector<tool_descriptor> describe_tool_schemas() const { return std::vector<tool_descriptor>(); }
    virtual tool_result run(const std::string &name,
                            const std::vector<std::string> &args,
                            nucleus_runtime &runtime,
                            const agent_task &task) const = 0;
    virtual tool_result run_with_policy_labels(const std::string &name,
                                               const std::vector<std::string> &args,
                                               const std::vector<std::string> &policy_labels,
                                               nucleus_runtime &runtime,
                                               const agent_task &task) const
    {
        return run(name, args, runtime, task);
    }
};

typedef std::unique_ptr<agent_tool_provider> (*agent_tool_provider_factory)();

void register_default_tool_provider(agent_tool_provider_factory factory);
std::unique_ptr<agent_tool_provider> create_unconfigured_tool_provider();
std::unique_ptr<agent_tool_provider> create_default_tool_provider();

} // namespace rasn
} // namespace dsn
