#pragma once

#include <rasn/agent_types.h>

#include <dsn/cpp/zlocks.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

struct capability_provider
{
    agent_descriptor descriptor;
    std::string state = "running";
    std::string placement;
    std::vector<std::string> labels;
    uint32_t load = 0;
    uint64_t last_seen_ms = 0;
};

struct capability_query
{
    std::string capability;
    std::string input_type;
    std::string side_effect_class;
    bool healthy_only = true;
    uint32_t max_load = (std::numeric_limits<uint32_t>::max)();
    uint64_t now_ms = 0;
    uint64_t max_age_ms = 0;
    std::vector<std::string> required_labels;
    size_t limit = 0;
};

struct capability_match
{
    capability_provider provider;
    agent_capability capability;
    int64_t score = 0;
    std::string reason;
};

class capability_directory
{
public:
    bool upsert_provider(capability_provider provider, std::string *error);
    bool remove_provider(const std::string &provider_id, std::string *error);
    bool find_provider(const std::string &provider_id, capability_provider *provider) const;
    std::vector<capability_match> query(const capability_query &query) const;
    bool choose_best(const capability_query &query, capability_match *match, std::string *error) const;
    std::vector<capability_provider> snapshot() const;
    std::string describe() const;

private:
    bool provider_matches(const capability_provider &provider, const capability_query &query) const;
    bool capability_matches(const agent_capability &capability, const capability_query &query) const;
    bool labels_include(const std::vector<std::string> &labels, const std::vector<std::string> &required) const;
    int64_t score_match(const capability_provider &provider, const agent_capability &capability) const;

    mutable ::dsn::service::zlock _lock;
    std::map<std::string, capability_provider> _providers;
};

} // namespace rasn
} // namespace dsn
