#pragma once

#include <rasn/runtime_provider.h>

namespace dsn {
namespace rasn {
namespace runtime_provider_internal {

// Test-only access to the production mirror-hydration path. Keep this out of the
// public runtime facade so standalone startup remains the sole production caller.
struct replica_store_test_accessor
{
    static bool replace_mirrored_state_records(
        rasn_runtime_replica_store &store,
        const std::vector<state_record> &records,
        const std::vector<uint32_t> &hosted_shards,
        size_t *applied,
        std::string *error);

    static uint32_t partition_for_key(const std::string &module, const std::string &key);
};

} // namespace runtime_provider_internal
} // namespace rasn
} // namespace dsn
