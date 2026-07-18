#pragma once

#include <string>
#include <utility>

namespace dsn {
namespace rasn {
namespace state_service_internal {

struct existing_state_path_identities
{
    std::string preferred;
    std::string fallback;
};

enum class state_path_identity_status
{
    absent,
    resolved,
    uninspectable
};

template <typename PreferredQuery, typename LegacyQuery>
state_path_identity_status collect_windows_identity_query_results(
    existing_state_path_identities &identities,
    PreferredQuery preferred_query,
    LegacyQuery legacy_query)
{
    std::string preferred;
    std::string fallback;
    const bool preferred_resolved = preferred_query(preferred);
    const bool legacy_resolved = legacy_query(fallback);
    identities.preferred =
        preferred_resolved ? std::move(preferred) : std::string();
    identities.fallback =
        legacy_resolved ? std::move(fallback) : std::string();
    return legacy_resolved ? state_path_identity_status::resolved
                           : state_path_identity_status::uninspectable;
}

state_path_identity_status resolve_existing_state_path_identities(
    const std::string &path, existing_state_path_identities &identities);

} // namespace state_service_internal
} // namespace rasn
} // namespace dsn
