#pragma once

#include "policy_manager.h"

#include <string>
#include <vector>

namespace dsn {
namespace rasn {

struct approval_sandbox_options
{
    bool require_write_approval = true;
    bool require_shell_approval = true;
    std::string write_sandbox_mode = "workspace-write";
    std::string shell_sandbox_mode = "command-allowlist";
};

struct approval_sandbox_request
{
    std::string tool_name;
    std::vector<std::string> args;
    bool explicit_approval = false;
    std::string actor;
};

struct approval_sandbox_decision
{
    bool approved = false;
    bool prompt_required = false;
    tool_side_effect side_effect = tool_side_effect::unknown;
    std::string sandbox_mode;
    std::string reason;
    std::string prompt;
    std::string review_text;
    std::vector<std::string> policy_labels;
};

approval_sandbox_options default_approval_sandbox_options();
approval_sandbox_decision evaluate_approval_sandbox_request(const approval_sandbox_request &request,
                                                            const approval_sandbox_options &options);
void grant_human_approval(approval_sandbox_decision *decision);

} // namespace rasn
} // namespace dsn
