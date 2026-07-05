#include <rasn/recovery_supervisor.h>

#include <dsn/service_api_cpp.h>

#include <sstream>

namespace dsn {
namespace rasn {

bool recovery_supervisor::set_policy(const recovery_policy &policy, std::string *error)
{
    if (policy.failure_class.empty())
    {
        if (error != nullptr)
        {
            *error = "recovery policy missing failure class";
        }
        return false;
    }
    ::dsn::service::zauto_lock guard(_lock);
    _policies[policy.failure_class] = policy;
    return true;
}

bool recovery_supervisor::hydrate_failure(const failure_observation &failure, std::string *error)
{
    if (failure.task_id.empty())
    {
        if (error != nullptr)
        {
            *error = "failure observation missing task id";
        }
        return false;
    }

    ::dsn::service::zauto_lock guard(_lock);
    for (failure_observation &existing : _history)
    {
        if (existing.task_id == failure.task_id && existing.component == failure.component &&
            existing.failure_class == failure.failure_class && existing.code == failure.code &&
            existing.attempt == failure.attempt && existing.time_ms == failure.time_ms)
        {
            existing = failure;
            return true;
        }
    }
    _history.push_back(failure);
    return true;
}

recovery_action recovery_supervisor::decide(const failure_observation &failure) const
{
    const recovery_policy policy = policy_for(failure.failure_class);
    recovery_action action;
    action.handled = true;
    action.labels.push_back("failure:" + failure.failure_class);
    action.labels.push_back("component:" + failure.component);

    if (!failure.retryable || !policy.retryable)
    {
        action.action = policy.compensation.empty() ? "abort" : "compensate";
        action.reason = "failure is not retryable";
        action.labels.push_back("terminal");
        return action;
    }
    if (policy.escalate_after_attempts != 0 && failure.attempt >= policy.escalate_after_attempts)
    {
        action.action = "escalate";
        action.reason = "retry attempts reached escalation threshold";
        action.labels.push_back("human-review");
        return action;
    }
    if (failure.attempt < policy.max_attempts)
    {
        action.action = "retry";
        action.delay_ms = policy.retry_delay_ms;
        action.reason = "retry allowed by recovery policy";
        action.labels.push_back("retry");
        return action;
    }
    action.action = policy.compensation.empty() ? "abort" : "compensate";
    action.reason = "retry budget exhausted";
    action.labels.push_back("terminal");
    return action;
}

recovery_action recovery_supervisor::observe(const failure_observation &failure)
{
    failure_observation stored = failure;
    if (stored.time_ms == 0)
    {
        stored.time_ms = ::dsn_now_ms();
    }
    recovery_action action = decide(stored);
    ::dsn::service::zauto_lock guard(_lock);
    _history.push_back(stored);
    return action;
}

bool recovery_supervisor::clear_history(const std::string &task_id, std::string *error)
{
    if (task_id.empty())
    {
        if (error != nullptr)
        {
            *error = "recovery history clear missing task id";
        }
        return false;
    }
    ::dsn::service::zauto_lock guard(_lock);
    std::vector<failure_observation> kept;
    for (const failure_observation &failure : _history)
    {
        if (failure.task_id != task_id)
        {
            kept.push_back(failure);
        }
    }
    _history.swap(kept);
    return true;
}

std::vector<failure_observation> recovery_supervisor::history(const std::string &task_id) const
{
    ::dsn::service::zauto_lock guard(_lock);
    std::vector<failure_observation> result;
    for (const failure_observation &failure : _history)
    {
        if (task_id.empty() || failure.task_id == task_id)
        {
            result.push_back(failure);
        }
    }
    return result;
}

std::string recovery_supervisor::describe() const
{
    ::dsn::service::zauto_lock guard(_lock);
    std::ostringstream output;
    output << "policies=" << _policies.size() << " failures=" << _history.size();
    for (const std::map<std::string, recovery_policy>::value_type &entry : _policies)
    {
        output << "\n" << entry.first
               << " retryable=" << (entry.second.retryable ? "yes" : "no")
               << " max_attempts=" << entry.second.max_attempts
               << " retry_delay_ms=" << entry.second.retry_delay_ms;
    }
    return output.str();
}

recovery_policy recovery_supervisor::policy_for(const std::string &failure_class) const
{
    ::dsn::service::zauto_lock guard(_lock);
    const std::map<std::string, recovery_policy>::const_iterator exact = _policies.find(failure_class);
    if (exact != _policies.end())
    {
        return exact->second;
    }
    const std::map<std::string, recovery_policy>::const_iterator fallback = _policies.find("*");
    if (fallback != _policies.end())
    {
        return fallback->second;
    }
    recovery_policy policy;
    policy.failure_class = failure_class.empty() ? "*" : failure_class;
    policy.max_attempts = 1;
    policy.retryable = false;
    return policy;
}

} // namespace rasn
} // namespace dsn
