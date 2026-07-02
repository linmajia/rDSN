#include <rasn/approval_sandbox.h>

#include <dsn/service_api_cpp.h>

#include <sstream>

namespace dsn {
namespace rasn {

namespace {

bool config_bool_compat(const std::string &key, bool fallback)
{
    const bool compat_value = ::dsn_config_get_value_bool(
        "rasn.codepilot.tools", key.c_str(), fallback, "CodePilot compatibility approval setting");
    return ::dsn_config_get_value_bool(
        "rasn.policy", key.c_str(), compat_value, "rASN approval policy setting");
}

std::string config_string_compat(const std::string &key, const std::string &fallback)
{
    const char *compat_value = ::dsn_config_get_value_string(
        "rasn.codepilot.tools", key.c_str(), fallback.c_str(), "CodePilot compatibility approval setting");
    const std::string compat = compat_value == nullptr ? "" : compat_value;
    const char *policy_value =
        ::dsn_config_get_value_string("rasn.policy", key.c_str(), compat.empty() ? fallback.c_str() : compat.c_str(), "rASN approval policy setting");
    const std::string policy = policy_value == nullptr ? "" : policy_value;
    return policy.empty() ? fallback : policy;
}

bool side_effect_requires_approval(tool_side_effect side_effect, const approval_sandbox_options &options)
{
    if (side_effect == tool_side_effect::write)
    {
        return options.require_write_approval;
    }
    if (side_effect == tool_side_effect::shell)
    {
        return options.require_shell_approval;
    }
    return false;
}

std::string sandbox_mode_for(tool_side_effect side_effect, const approval_sandbox_options &options)
{
    if (side_effect == tool_side_effect::write)
    {
        return options.write_sandbox_mode;
    }
    if (side_effect == tool_side_effect::shell)
    {
        return options.shell_sandbox_mode;
    }
    if (side_effect == tool_side_effect::read_only)
    {
        return "read-only";
    }
    return "policy-manager";
}

std::string approval_prompt(const approval_sandbox_request &request, tool_side_effect side_effect)
{
    std::ostringstream output;
    output << "Approval required for " << to_string(side_effect) << " tool '" << request.tool_name << "'";
    if (!request.args.empty())
    {
        output << " target '" << request.args[0] << "'";
    }
    output << ". Type 'yes' to continue: ";
    return output.str();
}

std::string approval_review_text(const approval_sandbox_request &request,
                                 tool_side_effect side_effect,
                                 const std::string &sandbox_mode)
{
    std::ostringstream output;
    output << "tool=" << request.tool_name
           << "\nside_effect=" << to_string(side_effect)
           << "\nsandbox=" << sandbox_mode;
    if (!request.actor.empty())
    {
        output << "\nactor=" << request.actor;
    }
    if (!request.args.empty())
    {
        output << "\ntarget=" << request.args[0];
    }
    if (request.tool_name == "replace" && request.args.size() >= 3)
    {
        output << "\nold_bytes=" << request.args[1].size()
               << "\nnew_bytes=" << request.args[2].size();
    }
    else if (request.tool_name == "write" && request.args.size() >= 2)
    {
        output << "\ncontent_bytes=" << request.args[1].size();
    }
    return output.str();
}

} // namespace

approval_sandbox_options default_approval_sandbox_options()
{
    approval_sandbox_options options;
    options.require_write_approval = config_bool_compat("require_write_approval", true);
    options.require_shell_approval = config_bool_compat("require_shell_approval", true);
    options.write_sandbox_mode = config_string_compat("write_sandbox_mode", options.write_sandbox_mode);
    options.shell_sandbox_mode = config_string_compat("shell_sandbox_mode", options.shell_sandbox_mode);
    return options;
}

approval_sandbox_decision evaluate_approval_sandbox_request(const approval_sandbox_request &request,
                                                            const approval_sandbox_options &options)
{
    approval_sandbox_decision decision;
    decision.side_effect = classify_tool_side_effect(request.tool_name);
    decision.sandbox_mode = sandbox_mode_for(decision.side_effect, options);
    decision.review_text = approval_review_text(request, decision.side_effect, decision.sandbox_mode);

    if (decision.side_effect != tool_side_effect::write && decision.side_effect != tool_side_effect::shell)
    {
        decision.approved = true;
        decision.reason = "no CLI approval required";
        return decision;
    }

    if (request.explicit_approval)
    {
        decision.approved = true;
        decision.reason = "explicit approval flag supplied";
        decision.policy_labels.push_back(human_approval_policy_label(decision.side_effect));
        return decision;
    }

    if (!side_effect_requires_approval(decision.side_effect, options))
    {
        decision.approved = true;
        decision.reason = "approval not required by policy";
        return decision;
    }

    decision.prompt_required = true;
    decision.reason = to_string(decision.side_effect) + " tool requires human approval";
    decision.prompt = approval_prompt(request, decision.side_effect);
    return decision;
}

void grant_human_approval(approval_sandbox_decision *decision)
{
    if (decision == nullptr)
    {
        return;
    }
    decision->approved = true;
    decision->prompt_required = false;
    decision->reason = "human approval granted";
    decision->policy_labels.push_back(human_approval_policy_label(decision->side_effect));
}

} // namespace rasn
} // namespace dsn
