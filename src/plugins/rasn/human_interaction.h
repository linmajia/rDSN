#pragma once

#include <dsn/cpp/zlocks.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

struct human_interaction_request
{
    std::string request_id;
    std::string task_id;
    std::string kind = "approval";
    std::string requester;
    std::string prompt;
    std::vector<std::string> choices;
    std::string state = "pending";
    std::string answer;
    uint64_t created_at_ms = 0;
    uint64_t updated_at_ms = 0;
    uint64_t deadline_ms = 0;
};

struct human_interaction_result
{
    bool ok = false;
    human_interaction_request request;
    std::string error;
};

class human_interaction_queue
{
public:
    human_interaction_result open(human_interaction_request request);
    human_interaction_result answer(const std::string &request_id, const std::string &answer);
    human_interaction_result cancel(const std::string &request_id, const std::string &reason);
    size_t expire(uint64_t now_ms);
    bool find(const std::string &request_id, human_interaction_request *request) const;
    std::vector<human_interaction_request> pending(const std::string &requester = "") const;
    std::vector<human_interaction_request> snapshot() const;
    std::string describe() const;

private:
    bool terminal(const human_interaction_request &request) const;
    bool choice_allowed(const human_interaction_request &request, const std::string &answer) const;

    mutable ::dsn::service::zlock _lock;
    std::map<std::string, human_interaction_request> _requests;
};

} // namespace rasn
} // namespace dsn
