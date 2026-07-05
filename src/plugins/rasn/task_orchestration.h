#pragma once

#include <dsn/cpp/zlocks.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

struct orchestration_task
{
    std::string task_id;
    std::string parent_task_id;
    std::string owner_agent;
    std::string state = "pending";
    std::string input;
    std::string output;
    std::string error;
    std::vector<std::string> depends_on;
    std::string compensation;
    uint64_t deadline_ms = 0;
    uint64_t generation = 0;
};

class task_orchestration_kernel
{
public:
    bool add_task(const orchestration_task &task, std::string *error);
    bool hydrate_task(const orchestration_task &task, std::string *error);
    bool assign(const std::string &task_id, const std::string &owner_agent, std::string *error);
    bool start(const std::string &task_id, const std::string &owner_agent, std::string *error);
    bool complete(const std::string &task_id, const std::string &output, std::string *error);
    bool fail(const std::string &task_id, const std::string &error_text, bool retryable, std::string *error);
    bool cancel(const std::string &task_id, const std::string &reason, std::string *error);
    std::vector<orchestration_task> ready_tasks(uint64_t now_ms = 0) const;
    std::vector<orchestration_task> blocked_tasks() const;
    bool find(const std::string &task_id, orchestration_task *task) const;
    std::vector<orchestration_task> snapshot() const;

private:
    bool dependencies_complete(const orchestration_task &task) const;
    bool terminal_state(const std::string &state) const;
    bool transition_to(const std::string &task_id,
                       const std::string &state,
                       const std::string &output,
                       const std::string &error_text,
                       std::string *error);

    mutable ::dsn::service::zlock _lock;
    std::map<std::string, orchestration_task> _tasks;
};

} // namespace rasn
} // namespace dsn
