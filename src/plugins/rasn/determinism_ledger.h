#pragma once

#include <dsn/cpp/zlocks.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

struct deterministic_choice
{
    uint64_t sequence = 0;
    std::string task_id;
    std::string key;
    std::string source;
    std::string value;
};

struct deterministic_replay_result
{
    bool ok = false;
    bool replayed = false;
    deterministic_choice choice;
    std::string error;
};

class determinism_ledger
{
public:
    bool record(const std::string &task_id,
                const std::string &key,
                const std::string &source,
                const std::string &value,
                deterministic_choice *choice,
                std::string *error);
    deterministic_replay_result replay(const std::string &task_id, const std::string &key);
    deterministic_replay_result choose(const std::string &task_id,
                                       const std::string &key,
                                       const std::string &source,
                                       const std::function<std::string()> &generator);
    bool hydrate_choice(const deterministic_choice &choice, std::string *error);
    void set_replay_choices(const std::vector<deterministic_choice> &choices);
    std::vector<deterministic_choice> snapshot() const;
    std::string to_jsonl() const;

private:
    std::string replay_key(const std::string &task_id, const std::string &key) const;

    mutable ::dsn::service::zlock _lock;
    uint64_t _next_sequence = 1;
    std::vector<deterministic_choice> _choices;
    std::map<std::string, deterministic_choice> _replay;
};

} // namespace rasn
} // namespace dsn
