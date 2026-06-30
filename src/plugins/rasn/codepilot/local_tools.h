#pragma once

#include "../agent_tools.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

class codepilot_tool_provider : public agent_tool_provider
{
public:
    codepilot_tool_provider();

    std::string describe_tools() const override;
    std::vector<tool_descriptor> describe_tool_schemas() const override;
    tool_result run(const std::string &name,
                    const std::vector<std::string> &args,
                    nucleus_runtime &runtime,
                    const agent_task &task) const override;
    tool_result run_with_policy_labels(const std::string &name,
                                       const std::vector<std::string> &args,
                                       const std::vector<std::string> &policy_labels,
                                       nucleus_runtime &runtime,
                                       const agent_task &task) const override;

private:
    tool_result run_list(const std::vector<std::string> &args) const;
    tool_result run_read(const std::vector<std::string> &args) const;
    tool_result run_search(const std::vector<std::string> &args) const;
    tool_result run_write(const std::vector<std::string> &args) const;
    tool_result run_replace(const std::vector<std::string> &args) const;
    tool_result run_shell(const std::vector<std::string> &args) const;
    tool_result run_checked(const std::string &name,
                            const std::vector<std::string> &args,
                            const std::vector<std::string> &policy_labels,
                            nucleus_runtime &runtime,
                            const agent_task &task) const;

    size_t _max_read_bytes;
    size_t _max_search_matches;
};

std::unique_ptr<agent_tool_provider> create_codepilot_tool_provider();
bool codepilot_write_file_atomically(const std::string &path, const std::string &content, std::string *error);
bool codepilot_shell_command_allowed(const std::string &command,
                                     const std::vector<std::string> &allowed_commands,
                                     std::string *error);
std::string codepilot_shell_command_with_working_directory(const std::string &command,
                                                           const std::string &working_directory);
std::string codepilot_shell_command_with_container_template(const std::string &command,
                                                            const std::string &working_directory,
                                                            const std::string &template_command,
                                                            std::string *error);
tool_result codepilot_run_shell_command(const std::string &command, uint64_t timeout_ms);

} // namespace rasn
} // namespace dsn
