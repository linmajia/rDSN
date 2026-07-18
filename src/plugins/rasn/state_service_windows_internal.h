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

// The handle must be valid. The caller retains ownership and closes it.
state_path_identity_status resolve_open_windows_state_path_identities(
    HANDLE handle, existing_state_path_identities &identities);

} // namespace state_service_internal
} // namespace rasn
} // namespace dsn
#endif
