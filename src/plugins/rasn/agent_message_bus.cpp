#include <rasn/agent_message_bus.h>

#include <rasn/rasn_core.h>

#include <dsn/service_api_cpp.h>

namespace dsn {
namespace rasn {

namespace {

bool message_ready(const agent_message &message, uint64_t now_ms)
{
    if (message.state != "queued" && message.state != "deferred")
    {
        return false;
    }
    return message.available_at_ms == 0 || message.available_at_ms <= now_ms;
}

} // namespace

bool agent_message_bus::publish(agent_message message, agent_message *stored, std::string *error)
{
    if (message.receiver.empty())
    {
        if (error != nullptr)
        {
            *error = "agent message missing receiver";
        }
        return false;
    }
    if (message.type.empty())
    {
        if (error != nullptr)
        {
            *error = "agent message missing type";
        }
        return false;
    }

    ::dsn::service::zauto_lock guard(_lock);
    if (message.message_id.empty())
    {
        message.message_id = make_trace_id();
    }
    if (_messages.find(message.message_id) != _messages.end())
    {
        if (error != nullptr)
        {
            *error = "duplicate message id: " + message.message_id;
        }
        return false;
    }
    const uint64_t now_ms = ::dsn_now_ms();
    if (message.created_at_ms == 0)
    {
        message.created_at_ms = now_ms;
    }
    message.updated_at_ms = now_ms;
    if (message.state.empty())
    {
        message.state = "queued";
    }
    _messages[message.message_id] = message;
    if (stored != nullptr)
    {
        *stored = message;
    }
    return true;
}

std::vector<agent_message> agent_message_bus::pull(const std::string &receiver, size_t max_messages, uint64_t now_ms)
{
    ::dsn::service::zauto_lock guard(_lock);
    std::vector<agent_message> result;
    for (std::map<std::string, agent_message>::value_type &entry : _messages)
    {
        if (result.size() >= max_messages)
        {
            break;
        }
        agent_message &message = entry.second;
        if (message.receiver != receiver || !message_ready(message, now_ms))
        {
            continue;
        }
        message.state = "delivered";
        message.updated_at_ms = now_ms;
        ++message.attempt;
        result.push_back(message);
    }
    return result;
}

bool agent_message_bus::ack(const std::string &message_id, std::string *error)
{
    ::dsn::service::zauto_lock guard(_lock);
    std::map<std::string, agent_message>::iterator it = _messages.find(message_id);
    if (it == _messages.end())
    {
        if (error != nullptr)
        {
            *error = "unknown message: " + message_id;
        }
        return false;
    }
    it->second.state = "acked";
    it->second.updated_at_ms = ::dsn_now_ms();
    return true;
}

bool agent_message_bus::defer(const std::string &message_id, uint64_t available_at_ms, const std::string &reason, std::string *error)
{
    ::dsn::service::zauto_lock guard(_lock);
    std::map<std::string, agent_message>::iterator it = _messages.find(message_id);
    if (it == _messages.end())
    {
        if (error != nullptr)
        {
            *error = "unknown message: " + message_id;
        }
        return false;
    }
    if (terminal(it->second))
    {
        if (error != nullptr)
        {
            *error = "cannot defer terminal message: " + message_id;
        }
        return false;
    }
    it->second.state = "deferred";
    it->second.available_at_ms = available_at_ms;
    it->second.error = reason;
    it->second.updated_at_ms = ::dsn_now_ms();
    return true;
}

bool agent_message_bus::dead_letter(const std::string &message_id, const std::string &reason, std::string *error)
{
    ::dsn::service::zauto_lock guard(_lock);
    std::map<std::string, agent_message>::iterator it = _messages.find(message_id);
    if (it == _messages.end())
    {
        if (error != nullptr)
        {
            *error = "unknown message: " + message_id;
        }
        return false;
    }
    it->second.state = "dead_letter";
    it->second.error = reason;
    it->second.updated_at_ms = ::dsn_now_ms();
    return true;
}

size_t agent_message_bus::expire_deadlines(uint64_t now_ms)
{
    ::dsn::service::zauto_lock guard(_lock);
    size_t expired = 0;
    for (std::map<std::string, agent_message>::value_type &entry : _messages)
    {
        agent_message &message = entry.second;
        if (!terminal(message) && message.deadline_ms != 0 && message.deadline_ms <= now_ms)
        {
            message.state = "deadline_expired";
            message.error = "message deadline expired";
            message.updated_at_ms = now_ms;
            ++expired;
        }
    }
    return expired;
}

bool agent_message_bus::find(const std::string &message_id, agent_message *message) const
{
    ::dsn::service::zauto_lock guard(_lock);
    const std::map<std::string, agent_message>::const_iterator it = _messages.find(message_id);
    if (it == _messages.end())
    {
        return false;
    }
    if (message != nullptr)
    {
        *message = it->second;
    }
    return true;
}

std::vector<agent_message> agent_message_bus::outbox(const std::string &sender) const
{
    ::dsn::service::zauto_lock guard(_lock);
    std::vector<agent_message> result;
    for (const std::map<std::string, agent_message>::value_type &entry : _messages)
    {
        if (entry.second.sender == sender)
        {
            result.push_back(entry.second);
        }
    }
    return result;
}

std::vector<agent_message> agent_message_bus::inbox(const std::string &receiver, bool include_terminal) const
{
    ::dsn::service::zauto_lock guard(_lock);
    std::vector<agent_message> result;
    for (const std::map<std::string, agent_message>::value_type &entry : _messages)
    {
        if (entry.second.receiver == receiver && (include_terminal || !terminal(entry.second)))
        {
            result.push_back(entry.second);
        }
    }
    return result;
}

std::vector<agent_message> agent_message_bus::snapshot() const
{
    ::dsn::service::zauto_lock guard(_lock);
    std::vector<agent_message> result;
    result.reserve(_messages.size());
    for (const std::map<std::string, agent_message>::value_type &entry : _messages)
    {
        result.push_back(entry.second);
    }
    return result;
}

bool agent_message_bus::terminal(const agent_message &message) const
{
    return message.state == "acked" || message.state == "dead_letter" || message.state == "deadline_expired";
}

} // namespace rasn
} // namespace dsn
