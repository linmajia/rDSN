#pragma once

#include <rasn/agent_types.h>

#include <dsn/cpp/zlocks.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

struct agent_control_record
{
    agent_descriptor descriptor;
    std::string state = "starting";
    std::string placement;
    std::string owner;
    std::string restart_policy = "never";
    std::string last_error;
    uint64_t generation = 0;
    uint64_t last_heartbeat_ms = 0;
    uint64_t lease_expires_ms = 0;
};

struct agent_control_lease
{
    bool ok = false;
    std::string agent_id;
    std::string owner;
    uint64_t generation = 0;
    uint64_t expires_ms = 0;
    std::string error;
};

class agent_control_plane
{
public:
    bool upsert_agent(const agent_control_record &record, std::string *error);
    bool hydrate_agent(const agent_control_record &record, std::string *error);
    bool transition(const std::string &agent_id,
                    const std::string &state,
                    const std::string &last_error,
                    std::string *error);
    agent_control_lease acquire_lease(const std::string &agent_id,
                                      const std::string &owner,
                                      uint64_t now_ms,
                                      uint64_t lease_ms);
    bool release_lease(const std::string &agent_id, const std::string &owner, std::string *error);
    bool heartbeat(const std::string &agent_id, uint64_t now_ms, std::string *error);
    size_t expire_leases(uint64_t now_ms);
    bool find(const std::string &agent_id, agent_control_record *record) const;
    std::vector<agent_control_record> list(bool live_only = false, uint64_t now_ms = 0) const;
    std::vector<agent_control_record> query_by_capability(const std::string &capability,
                                                          bool live_only = false,
                                                          uint64_t now_ms = 0) const;
    std::string describe(uint64_t now_ms = 0) const;

private:
    bool has_capability(const agent_descriptor &descriptor, const std::string &capability) const;
    bool is_live(const agent_control_record &record, uint64_t now_ms) const;

    mutable ::dsn::service::zlock _lock;
    std::map<std::string, agent_control_record> _agents;
};

} // namespace rasn
} // namespace dsn
