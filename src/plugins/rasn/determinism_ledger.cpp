#include <rasn/determinism_ledger.h>

#include <rasn/rasn_core.h>

#include <sstream>

namespace dsn {
namespace rasn {

bool determinism_ledger::record(const std::string &task_id,
                                const std::string &key,
                                const std::string &source,
                                const std::string &value,
                                deterministic_choice *choice,
                                std::string *error)
{
    if (key.empty())
    {
        if (error != nullptr)
        {
            *error = "deterministic choice key cannot be empty";
        }
        return false;
    }

    ::dsn::service::zauto_lock guard(_lock);
    deterministic_choice next;
    next.sequence = _next_sequence++;
    next.task_id = task_id;
    next.key = key;
    next.source = source;
    next.value = value;
    _choices.push_back(next);
    if (choice != nullptr)
    {
        *choice = next;
    }
    return true;
}

deterministic_replay_result determinism_ledger::replay(const std::string &task_id, const std::string &key)
{
    deterministic_replay_result result;
    ::dsn::service::zauto_lock guard(_lock);
    const std::map<std::string, deterministic_choice>::const_iterator it = _replay.find(replay_key(task_id, key));
    if (it == _replay.end())
    {
        result.error = "replay choice not found: " + key;
        return result;
    }
    result.ok = true;
    result.replayed = true;
    result.choice = it->second;
    return result;
}

deterministic_replay_result determinism_ledger::choose(const std::string &task_id,
                                                       const std::string &key,
                                                       const std::string &source,
                                                       const std::function<std::string()> &generator)
{
    deterministic_replay_result replayed = replay(task_id, key);
    if (replayed.ok)
    {
        return replayed;
    }

    deterministic_replay_result result;
    std::string error;
    deterministic_choice choice;
    if (!record(task_id, key, source, generator ? generator() : "", &choice, &error))
    {
        result.error = error;
        return result;
    }
    result.ok = true;
    result.replayed = false;
    result.choice = choice;
    return result;
}

void determinism_ledger::set_replay_choices(const std::vector<deterministic_choice> &choices)
{
    ::dsn::service::zauto_lock guard(_lock);
    _replay.clear();
    for (const deterministic_choice &choice : choices)
    {
        _replay[replay_key(choice.task_id, choice.key)] = choice;
    }
}

std::vector<deterministic_choice> determinism_ledger::snapshot() const
{
    ::dsn::service::zauto_lock guard(_lock);
    return _choices;
}

std::string determinism_ledger::to_jsonl() const
{
    ::dsn::service::zauto_lock guard(_lock);
    std::ostringstream output;
    for (const deterministic_choice &choice : _choices)
    {
        output << "{\"schema_version\":1"
               << ",\"sequence\":" << choice.sequence
               << ",\"task_id\":\"" << json_escape(choice.task_id)
               << "\",\"key\":\"" << json_escape(choice.key)
               << "\",\"source\":\"" << json_escape(choice.source)
               << "\",\"value\":\"" << json_escape(choice.value)
               << "\"}\n";
    }
    return output.str();
}

std::string determinism_ledger::replay_key(const std::string &task_id, const std::string &key) const
{
    return task_id + "\n" + key;
}

} // namespace rasn
} // namespace dsn
