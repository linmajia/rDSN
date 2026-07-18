#pragma once

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <rasn/state_service_internal.h>

namespace dsn {
namespace rasn {
namespace state_service_internal {

struct windows_identity_query_observation
{
    bool preferred_supported = false;
    windows_file_id_128 preferred;
    bool fallback_supported = false;
    BY_HANDLE_FILE_INFORMATION fallback = {};
};

// The handle must be valid. The caller retains ownership and closes it.
state_path_identity_status resolve_open_windows_state_path_identities(
    HANDLE handle, existing_state_path_identities &identities);

// A non-null observation performs independent queries on the same opened
// handle before resolution. Production passes null and incurs no extra I/O.
state_path_identity_status resolve_windows_state_path_identities(
    const std::string &path,
    existing_state_path_identities &identities,
    windows_identity_query_observation *observation);

} // namespace state_service_internal
} // namespace rasn
} // namespace dsn
#endif
