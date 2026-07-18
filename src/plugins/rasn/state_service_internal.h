#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
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

struct windows_file_id_128
{
    uint64_t volume_serial_number = 0;
    uint8_t file_id[16] = {};
};

static_assert(sizeof(windows_file_id_128) == 24,
              "FileIdInfo-compatible identity layout must remain 24 bytes");
static_assert(offsetof(windows_file_id_128, file_id) == sizeof(uint64_t),
              "FileIdInfo-compatible byte identifier must follow its volume");
static_assert(std::is_standard_layout<windows_file_id_128>::value,
              "FileIdInfo-compatible identity must remain standard-layout");
static_assert(std::is_trivially_copyable<windows_file_id_128>::value,
              "FileIdInfo-compatible identity must remain trivially copyable");

struct windows_file_index_64
{
    uint32_t volume_serial_number = 0;
    uint32_t file_index_high = 0;
    uint32_t file_index_low = 0;
};

static_assert(sizeof(windows_file_index_64) == 12,
              "legacy Windows identity must remain three 32-bit values");
static_assert(offsetof(windows_file_index_64, file_index_high) ==
                  sizeof(uint32_t),
              "legacy Windows high index must follow its volume");
static_assert(offsetof(windows_file_index_64, file_index_low) ==
                  2 * sizeof(uint32_t),
              "legacy Windows low index must follow its high index");
static_assert(std::is_standard_layout<windows_file_index_64>::value,
              "legacy Windows identity must remain standard-layout");
static_assert(std::is_trivially_copyable<windows_file_index_64>::value,
              "legacy Windows identity must remain trivially copyable");

template <typename Identity, typename Query>
bool query_windows_identity_buffer(Identity &identity, Query &&query)
{
    return std::forward<Query>(query)(
        &identity, sizeof(identity));
}

template <typename PreferredQuery, typename LegacyQuery>
state_path_identity_status collect_windows_identity_query_results(
    existing_state_path_identities &identities,
    PreferredQuery &&preferred_query,
    LegacyQuery &&legacy_query)
{
    windows_file_id_128 preferred;
    windows_file_index_64 fallback;
    const bool preferred_resolved =
        query_windows_identity_buffer(
            preferred, std::forward<PreferredQuery>(preferred_query));
    const bool legacy_resolved =
        query_windows_identity_buffer(
            fallback, std::forward<LegacyQuery>(legacy_query));
    identities.preferred.clear();
    identities.fallback.clear();
    if (preferred_resolved)
    {
        identities.preferred = "windows-file-id-128:";
        identities.preferred.append(
            reinterpret_cast<const char *>(&preferred.volume_serial_number),
            sizeof(preferred.volume_serial_number));
        identities.preferred.append(
            reinterpret_cast<const char *>(preferred.file_id),
            sizeof(preferred.file_id));
    }
    if (legacy_resolved)
    {
        identities.fallback = "windows-file-index-64:";
        identities.fallback.append(
            reinterpret_cast<const char *>(&fallback.volume_serial_number),
            sizeof(fallback.volume_serial_number));
        identities.fallback.append(
            reinterpret_cast<const char *>(&fallback.file_index_high),
            sizeof(fallback.file_index_high));
        identities.fallback.append(
            reinterpret_cast<const char *>(&fallback.file_index_low),
            sizeof(fallback.file_index_low));
    }
    return legacy_resolved ? state_path_identity_status::resolved
                           : state_path_identity_status::uninspectable;
}

state_path_identity_status resolve_existing_state_path_identities(
    const std::string &path, existing_state_path_identities &identities);

#if defined(_WIN32)
state_path_identity_status resolve_open_windows_state_path_identities(
    void *native_handle, existing_state_path_identities &identities);
#endif

} // namespace state_service_internal
} // namespace rasn
} // namespace dsn
