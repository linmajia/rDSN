#pragma once

#include <dsn/cpp/zlocks.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

struct resource_quota
{
    std::string scope;
    uint64_t max_cost_units = 0;
    uint64_t max_latency_ms = 0;
    uint64_t max_tokens = 0;
    uint64_t max_tool_calls = 0;
};

struct resource_usage
{
    std::string scope;
    uint64_t cost_units = 0;
    uint64_t latency_ms = 0;
    uint64_t tokens = 0;
    uint64_t tool_calls = 0;
};

struct resource_request
{
    std::string scope;
    uint64_t cost_units = 0;
    uint64_t latency_ms = 0;
    uint64_t tokens = 0;
    uint64_t tool_calls = 0;
    std::string reason;
};

struct resource_budget_decision
{
    bool allowed = false;
    std::string scope;
    std::string reason;
    resource_usage usage_after;
    resource_quota quota;
};

class resource_budget_manager
{
public:
    bool configure(const resource_quota &quota, std::string *error);
    resource_budget_decision reserve(const resource_request &request);
    bool release(const resource_request &request, std::string *error);
    bool usage(const std::string &scope, resource_usage *usage) const;
    std::vector<resource_usage> snapshot() const;
    std::string describe() const;

private:
    bool exceeds(uint64_t current, uint64_t requested, uint64_t limit) const;
    resource_usage add(const resource_usage &usage, const resource_request &request) const;
    void subtract(uint64_t amount, uint64_t *value) const;

    mutable ::dsn::service::zlock _lock;
    std::map<std::string, resource_quota> _quotas;
    std::map<std::string, resource_usage> _usage;
};

} // namespace rasn
} // namespace dsn
