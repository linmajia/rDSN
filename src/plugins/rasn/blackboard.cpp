#include <rasn/blackboard.h>

#include <dsn/service_api_cpp.h>

#include <algorithm>
#include <sstream>

namespace dsn {
namespace rasn {

bool shared_blackboard::put(blackboard_entry entry, blackboard_entry *stored, std::string *error)
{
    if (entry.key.empty())
    {
        if (error != nullptr)
        {
            *error = "blackboard entry missing key";
        }
        return false;
    }
    const uint64_t now_ms = ::dsn_now_ms();
    ::dsn::service::zauto_lock guard(_lock);
    const std::map<std::string, blackboard_entry>::const_iterator previous = _entries.find(entry.key);
    entry.generation = previous == _entries.end() ? 1 : previous->second.generation + 1;
    if (entry.created_at_ms == 0)
    {
        entry.created_at_ms = previous == _entries.end() ? now_ms : previous->second.created_at_ms;
    }
    entry.updated_at_ms = now_ms;
    _entries[entry.key] = entry;
    if (stored != nullptr)
    {
        *stored = entry;
    }
    return true;
}

bool shared_blackboard::get(const std::string &key, blackboard_entry *entry, uint64_t now_ms) const
{
    ::dsn::service::zauto_lock guard(_lock);
    const std::map<std::string, blackboard_entry>::const_iterator it = _entries.find(key);
    if (it == _entries.end() || expired(it->second, now_ms))
    {
        return false;
    }
    if (entry != nullptr)
    {
        *entry = it->second;
    }
    return true;
}

bool shared_blackboard::erase(const std::string &key, std::string *error)
{
    ::dsn::service::zauto_lock guard(_lock);
    if (_entries.erase(key) == 0)
    {
        if (error != nullptr)
        {
            *error = "blackboard entry not found: " + key;
        }
        return false;
    }
    return true;
}

std::vector<blackboard_entry> shared_blackboard::query(const blackboard_query &request) const
{
    ::dsn::service::zauto_lock guard(_lock);
    std::vector<blackboard_entry> result;
    for (const std::map<std::string, blackboard_entry>::value_type &entry : _entries)
    {
        if (matches(entry.second, request))
        {
            result.push_back(entry.second);
            if (request.limit != 0 && result.size() >= request.limit)
            {
                break;
            }
        }
    }
    return result;
}

size_t shared_blackboard::compact_expired(uint64_t now_ms)
{
    ::dsn::service::zauto_lock guard(_lock);
    size_t removed = 0;
    for (std::map<std::string, blackboard_entry>::iterator it = _entries.begin(); it != _entries.end();)
    {
        if (expired(it->second, now_ms))
        {
            it = _entries.erase(it);
            ++removed;
        }
        else
        {
            ++it;
        }
    }
    return removed;
}

std::vector<blackboard_entry> shared_blackboard::snapshot(bool include_expired, uint64_t now_ms) const
{
    blackboard_query request;
    request.include_expired = include_expired;
    request.now_ms = now_ms;
    return query(request);
}

std::string shared_blackboard::describe() const
{
    ::dsn::service::zauto_lock guard(_lock);
    std::ostringstream output;
    output << "entries=" << _entries.size();
    for (const std::map<std::string, blackboard_entry>::value_type &entry : _entries)
    {
        output << "\n" << entry.first
               << " kind=" << entry.second.kind
               << " owner=" << entry.second.owner
               << " generation=" << entry.second.generation;
    }
    return output.str();
}

bool shared_blackboard::expired(const blackboard_entry &entry, uint64_t now_ms) const
{
    return entry.expires_at_ms != 0 && now_ms != 0 && entry.expires_at_ms <= now_ms;
}

bool shared_blackboard::matches(const blackboard_entry &entry, const blackboard_query &request) const
{
    if (!request.include_expired && expired(entry, request.now_ms))
    {
        return false;
    }
    if (!request.key_prefix.empty() && entry.key.find(request.key_prefix) != 0)
    {
        return false;
    }
    if (!request.kind.empty() && entry.kind != request.kind)
    {
        return false;
    }
    if (!request.owner.empty() && entry.owner != request.owner)
    {
        return false;
    }
    return tags_include(entry.tags, request.tags);
}

bool shared_blackboard::tags_include(const std::vector<std::string> &tags,
                                     const std::vector<std::string> &required) const
{
    for (const std::string &tag : required)
    {
        if (std::find(tags.begin(), tags.end(), tag) == tags.end())
        {
            return false;
        }
    }
    return true;
}

} // namespace rasn
} // namespace dsn

