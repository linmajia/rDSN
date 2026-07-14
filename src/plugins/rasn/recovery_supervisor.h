#pragma once

#include <dsn/cpp/zlocks.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

struct recovery_policy
{
    std::string failure_class;
    uint32_t max_attempts = 1;
    uint64_t retry_delay_ms = 0;
    uint32_t escalate_after_attempts = 0;
    bool retryable = false;
    std::string compensation;
};

struct failure_observation
{
    std::string task_id;
    std::string component;
    std::string failure_class;
    std::string code;
    std::string message;
    uint32_t attempt = 0;
    bool retryable = false;
    uint64_t time_ms = 0;
};

struct recovery_action
{
    bool handled = false;
    std::string action;
    uint64_t delay_ms = 0;
    std::string reason;
    std::vector<std::string> labels;
};

class recovery_supervisor
{
public:
    bool set_policy(const recovery_policy &policy, std::string *error);
    bool hydrate_failure(const failure_observation &failure, std::string *error);
    recovery_action decide(const failure_observation &failure) const;
    recovery_action observe(const failure_observation &failure);
    bool clear_history(const std::string &task_id, std::string *error);
    std::vector<recovery_policy> policy_snapshot() const;
    std::vector<failure_observation> history(const std::string &task_id = "") const;
    std::string describe() const;

private:
    recovery_policy policy_for(const std::string &failure_class) const;

    mutable ::dsn::service::zlock _lock;
    std::map<std::string, recovery_policy> _policies;
    std::vector<failure_observation> _history;
};

} // namespace rasn
} // namespace dsn
