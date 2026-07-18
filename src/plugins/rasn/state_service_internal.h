#pragma once

namespace dsn {
namespace rasn {
namespace state_service_internal {

enum class windows_identity_query_status
{
    uninspectable,
    legacy_only,
    preferred_and_legacy
};

constexpr windows_identity_query_status classify_windows_identity_queries(
    bool preferred_resolved, bool legacy_resolved)
{
    return !legacy_resolved
               ? windows_identity_query_status::uninspectable
               : preferred_resolved
                     ? windows_identity_query_status::preferred_and_legacy
                     : windows_identity_query_status::legacy_only;
}

} // namespace state_service_internal
} // namespace rasn
} // namespace dsn
