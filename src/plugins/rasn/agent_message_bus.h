#pragma once

#include <dsn/cpp/zlocks.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

struct agent_message
{
    std::string message_id;
    std::string correlation_id;
    std::string sender;
    std::string receiver;
    std::string type;
    std::string payload;
    std::string state = "queued";
    std::string error;
    uint32_t attempt = 0;
    uint64_t deadline_ms = 0;
    uint64_t available_at_ms = 0;
    uint64_t created_at_ms = 0;
    uint64_t updated_at_ms = 0;
};

class agent_message_bus
{
public:
    bool publish(agent_message message, agent_message *stored, std::string *error);
    bool hydrate_message(const agent_message &message, std::string *error);
    std::vector<agent_message> pull(const std::string &receiver, size_t max_messages, uint64_t now_ms);
    bool ack(const std::string &message_id, std::string *error);
    bool defer(const std::string &message_id, uint64_t available_at_ms, const std::string &reason, std::string *error);
    bool dead_letter(const std::string &message_id, const std::string &reason, std::string *error);
    size_t expire_deadlines(uint64_t now_ms);
    bool find(const std::string &message_id, agent_message *message) const;
    std::vector<agent_message> outbox(const std::string &sender) const;
    std::vector<agent_message> inbox(const std::string &receiver, bool include_terminal = false) const;
    std::vector<agent_message> snapshot() const;

private:
    bool terminal(const agent_message &message) const;

    mutable ::dsn::service::zlock _lock;
    std::map<std::string, agent_message> _messages;
};

} // namespace rasn
} // namespace dsn
