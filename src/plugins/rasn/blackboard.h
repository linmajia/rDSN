#pragma once

#include <dsn/cpp/zlocks.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

struct blackboard_entry
{
    std::string key;
    std::string kind;
    std::string owner;
    std::string value;
    std::vector<std::string> tags;
    uint64_t generation = 0;
    uint64_t created_at_ms = 0;
    uint64_t updated_at_ms = 0;
    uint64_t expires_at_ms = 0;
};

struct blackboard_query
{
    std::string key_prefix;
    std::string kind;
    std::string owner;
    std::vector<std::string> tags;
    bool include_expired = false;
    uint64_t now_ms = 0;
    size_t limit = 0;
};

class shared_blackboard
{
public:
    bool put(blackboard_entry entry, blackboard_entry *stored, std::string *error);
    bool get(const std::string &key, blackboard_entry *entry, uint64_t now_ms = 0) const;
    bool erase(const std::string &key, std::string *error);
    std::vector<blackboard_entry> query(const blackboard_query &query) const;
    size_t compact_expired(uint64_t now_ms);
    std::vector<blackboard_entry> snapshot(bool include_expired = true, uint64_t now_ms = 0) const;
    std::string describe() const;

private:
    bool expired(const blackboard_entry &entry, uint64_t now_ms) const;
    bool matches(const blackboard_entry &entry, const blackboard_query &query) const;
    bool tags_include(const std::vector<std::string> &tags, const std::vector<std::string> &required) const;

    mutable ::dsn::service::zlock _lock;
    std::map<std::string, blackboard_entry> _entries;
};

} // namespace rasn
} // namespace dsn

