#include <rasn/human_interaction.h>

#include <rasn/rasn_core.h>

#include <dsn/service_api_cpp.h>

#include <algorithm>
#include <sstream>

namespace dsn {
namespace rasn {

human_interaction_result human_interaction_queue::open(human_interaction_request request)
{
    human_interaction_result result;
    if (request.prompt.empty())
    {
        result.error = "human interaction request missing prompt";
        return result;
    }
    if (request.request_id.empty())
    {
        request.request_id = make_trace_id();
    }
    if (request.state.empty())
    {
        request.state = "pending";
    }
    const uint64_t now_ms = ::dsn_now_ms();
    if (request.created_at_ms == 0)
    {
        request.created_at_ms = now_ms;
    }
    request.updated_at_ms = now_ms;

    ::dsn::service::zauto_lock guard(_lock);
    if (_requests.find(request.request_id) != _requests.end())
    {
        result.error = "duplicate human interaction request: " + request.request_id;
        return result;
    }
    _requests[request.request_id] = request;
    result.ok = true;
    result.request = request;
    return result;
}

human_interaction_result human_interaction_queue::answer(const std::string &request_id, const std::string &answer)
{
    human_interaction_result result;
    ::dsn::service::zauto_lock guard(_lock);
    std::map<std::string, human_interaction_request>::iterator it = _requests.find(request_id);
    if (it == _requests.end())
    {
        result.error = "unknown human interaction request: " + request_id;
        return result;
    }
    if (terminal(it->second))
    {
        result.error = "human interaction request is terminal: " + request_id;
        return result;
    }
    if (!choice_allowed(it->second, answer))
    {
        result.error = "answer is not one of the allowed choices";
        return result;
    }
    it->second.answer = answer;
    it->second.state = "answered";
    it->second.updated_at_ms = ::dsn_now_ms();
    result.ok = true;
    result.request = it->second;
    return result;
}

human_interaction_result human_interaction_queue::cancel(const std::string &request_id, const std::string &reason)
{
    human_interaction_result result;
    ::dsn::service::zauto_lock guard(_lock);
    std::map<std::string, human_interaction_request>::iterator it = _requests.find(request_id);
    if (it == _requests.end())
    {
        result.error = "unknown human interaction request: " + request_id;
        return result;
    }
    if (terminal(it->second))
    {
        result.error = "human interaction request is terminal: " + request_id;
        return result;
    }
    it->second.state = "cancelled";
    it->second.answer = reason;
    it->second.updated_at_ms = ::dsn_now_ms();
    result.ok = true;
    result.request = it->second;
    return result;
}

size_t human_interaction_queue::expire(uint64_t now_ms)
{
    ::dsn::service::zauto_lock guard(_lock);
    size_t expired = 0;
    for (std::map<std::string, human_interaction_request>::value_type &entry : _requests)
    {
        human_interaction_request &request = entry.second;
        if (!terminal(request) && request.deadline_ms != 0 && request.deadline_ms <= now_ms)
        {
            request.state = "expired";
            request.updated_at_ms = now_ms;
            ++expired;
        }
    }
    return expired;
}

bool human_interaction_queue::find(const std::string &request_id, human_interaction_request *request) const
{
    ::dsn::service::zauto_lock guard(_lock);
    const std::map<std::string, human_interaction_request>::const_iterator it = _requests.find(request_id);
    if (it == _requests.end())
    {
        return false;
    }
    if (request != nullptr)
    {
        *request = it->second;
    }
    return true;
}

std::vector<human_interaction_request> human_interaction_queue::pending(const std::string &requester) const
{
    ::dsn::service::zauto_lock guard(_lock);
    std::vector<human_interaction_request> result;
    for (const std::map<std::string, human_interaction_request>::value_type &entry : _requests)
    {
        if (entry.second.state == "pending" && (requester.empty() || entry.second.requester == requester))
        {
            result.push_back(entry.second);
        }
    }
    return result;
}

std::vector<human_interaction_request> human_interaction_queue::snapshot() const
{
    ::dsn::service::zauto_lock guard(_lock);
    std::vector<human_interaction_request> result;
    result.reserve(_requests.size());
    for (const std::map<std::string, human_interaction_request>::value_type &entry : _requests)
    {
        result.push_back(entry.second);
    }
    return result;
}

std::string human_interaction_queue::describe() const
{
    const std::vector<human_interaction_request> requests = snapshot();
    size_t pending_count = 0;
    for (const human_interaction_request &request : requests)
    {
        if (request.state == "pending")
        {
            ++pending_count;
        }
    }
    std::ostringstream output;
    output << "requests=" << requests.size() << " pending=" << pending_count;
    for (const human_interaction_request &request : requests)
    {
        output << "\n" << request.request_id
               << " kind=" << request.kind
               << " requester=" << request.requester
               << " state=" << request.state;
    }
    return output.str();
}

bool human_interaction_queue::terminal(const human_interaction_request &request) const
{
    return request.state == "answered" || request.state == "cancelled" || request.state == "expired";
}

bool human_interaction_queue::choice_allowed(const human_interaction_request &request, const std::string &answer) const
{
    return request.choices.empty() || std::find(request.choices.begin(), request.choices.end(), answer) != request.choices.end();
}

} // namespace rasn
} // namespace dsn

