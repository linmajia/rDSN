#include <rasn/task_orchestration.h>

namespace dsn {
namespace rasn {

bool task_orchestration_kernel::add_task(const orchestration_task &task, std::string *error)
{
    if (task.task_id.empty())
    {
        if (error != nullptr)
        {
            *error = "orchestration task missing id";
        }
        return false;
    }
    ::dsn::service::zauto_lock guard(_lock);
    if (_tasks.find(task.task_id) != _tasks.end())
    {
        if (error != nullptr)
        {
            *error = "duplicate orchestration task: " + task.task_id;
        }
        return false;
    }
    orchestration_task next = task;
    if (next.state.empty())
    {
        next.state = "pending";
    }
    next.generation = 1;
    _tasks[next.task_id] = next;
    return true;
}

bool task_orchestration_kernel::assign(const std::string &task_id, const std::string &owner_agent, std::string *error)
{
    if (owner_agent.empty())
    {
        if (error != nullptr)
        {
            *error = "task owner cannot be empty";
        }
        return false;
    }
    ::dsn::service::zauto_lock guard(_lock);
    std::map<std::string, orchestration_task>::iterator it = _tasks.find(task_id);
    if (it == _tasks.end())
    {
        if (error != nullptr)
        {
            *error = "unknown orchestration task: " + task_id;
        }
        return false;
    }
    if (terminal_state(it->second.state))
    {
        if (error != nullptr)
        {
            *error = "cannot assign terminal task: " + task_id;
        }
        return false;
    }
    it->second.owner_agent = owner_agent;
    if (it->second.state == "pending" && dependencies_complete(it->second))
    {
        it->second.state = "ready";
    }
    ++it->second.generation;
    return true;
}

bool task_orchestration_kernel::start(const std::string &task_id, const std::string &owner_agent, std::string *error)
{
    ::dsn::service::zauto_lock guard(_lock);
    std::map<std::string, orchestration_task>::iterator it = _tasks.find(task_id);
    if (it == _tasks.end())
    {
        if (error != nullptr)
        {
            *error = "unknown orchestration task: " + task_id;
        }
        return false;
    }
    if (!dependencies_complete(it->second))
    {
        if (error != nullptr)
        {
            *error = "task dependencies are not complete: " + task_id;
        }
        return false;
    }
    if (!it->second.owner_agent.empty() && it->second.owner_agent != owner_agent)
    {
        if (error != nullptr)
        {
            *error = "task owned by " + it->second.owner_agent;
        }
        return false;
    }
    it->second.owner_agent = owner_agent;
    it->second.state = "running";
    ++it->second.generation;
    return true;
}

bool task_orchestration_kernel::complete(const std::string &task_id, const std::string &output, std::string *error)
{
    return transition_to(task_id, "completed", output, "", error);
}

bool task_orchestration_kernel::fail(const std::string &task_id, const std::string &error_text, bool retryable, std::string *error)
{
    return transition_to(task_id, retryable ? "pending" : "failed", "", error_text, error);
}

bool task_orchestration_kernel::cancel(const std::string &task_id, const std::string &reason, std::string *error)
{
    return transition_to(task_id, "cancelled", "", reason, error);
}

std::vector<orchestration_task> task_orchestration_kernel::ready_tasks(uint64_t now_ms) const
{
    ::dsn::service::zauto_lock guard(_lock);
    std::vector<orchestration_task> result;
    for (std::map<std::string, orchestration_task>::const_reference entry : _tasks)
    {
        const orchestration_task &task = entry.second;
        if (terminal_state(task.state))
        {
            continue;
        }
        if (task.deadline_ms != 0 && now_ms != 0 && task.deadline_ms <= now_ms)
        {
            continue;
        }
        if (dependencies_complete(task) && (task.state == "pending" || task.state == "ready"))
        {
            result.push_back(task);
        }
    }
    return result;
}

std::vector<orchestration_task> task_orchestration_kernel::blocked_tasks() const
{
    ::dsn::service::zauto_lock guard(_lock);
    std::vector<orchestration_task> result;
    for (std::map<std::string, orchestration_task>::const_reference entry : _tasks)
    {
        if (!terminal_state(entry.second.state) && !dependencies_complete(entry.second))
        {
            result.push_back(entry.second);
        }
    }
    return result;
}

bool task_orchestration_kernel::find(const std::string &task_id, orchestration_task *task) const
{
    ::dsn::service::zauto_lock guard(_lock);
    const std::map<std::string, orchestration_task>::const_iterator it = _tasks.find(task_id);
    if (it == _tasks.end())
    {
        return false;
    }
    if (task != nullptr)
    {
        *task = it->second;
    }
    return true;
}

std::vector<orchestration_task> task_orchestration_kernel::snapshot() const
{
    ::dsn::service::zauto_lock guard(_lock);
    std::vector<orchestration_task> result;
    result.reserve(_tasks.size());
    for (std::map<std::string, orchestration_task>::const_reference entry : _tasks)
    {
        result.push_back(entry.second);
    }
    return result;
}

bool task_orchestration_kernel::dependencies_complete(const orchestration_task &task) const
{
    for (const std::string &dep_id : task.depends_on)
    {
        const std::map<std::string, orchestration_task>::const_iterator it = _tasks.find(dep_id);
        if (it == _tasks.end() || it->second.state != "completed")
        {
            return false;
        }
    }
    return true;
}

bool task_orchestration_kernel::terminal_state(const std::string &state) const
{
    return state == "completed" || state == "failed" || state == "cancelled";
}

bool task_orchestration_kernel::transition_to(const std::string &task_id,
                                              const std::string &state,
                                              const std::string &output,
                                              const std::string &error_text,
                                              std::string *error)
{
    ::dsn::service::zauto_lock guard(_lock);
    std::map<std::string, orchestration_task>::iterator it = _tasks.find(task_id);
    if (it == _tasks.end())
    {
        if (error != nullptr)
        {
            *error = "unknown orchestration task: " + task_id;
        }
        return false;
    }
    if (terminal_state(it->second.state))
    {
        if (error != nullptr)
        {
            *error = "task is already terminal: " + task_id;
        }
        return false;
    }
    it->second.state = state;
    it->second.output = output;
    it->second.error = error_text;
    ++it->second.generation;
    return true;
}

} // namespace rasn
} // namespace dsn
