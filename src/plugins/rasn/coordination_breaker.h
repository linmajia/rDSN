#pragma once

// Optional cluster-shared circuit-breaker state.
//
// Mutations are serialized by rasn_coordination_service ownership locks and
// persisted as versioned records in meta_state_service. The feature defaults off;
// when enabled, ZooKeeper is required for cross-process authority.

#include <rasn/circuit_breaker.h>
#include <rasn/coordination_service.h>

#include <cstdint>
#include <memory>
#include <string>

namespace dsn {
namespace rasn {

struct rasn_shared_breaker_config
{
    bool enabled = false;
    std::string state_prefix = "resilience/circuit_breakers";
    int lock_timeout_ms = 1000;
    uint64_t probe_lease_ms = 120000;
    uint64_t max_probe_lease_ms = 600000;
    uint64_t clock_skew_ms = 5000;
};

rasn_shared_breaker_config load_rasn_shared_breaker_config();

std::shared_ptr<circuit_breaker_registry_backend>
create_rasn_shared_breaker_backend(
    const std::string &scope,
    const rasn_shared_breaker_config &config,
    const std::shared_ptr<rasn_coordination_context> &coordination);

// Installs the configured backend on one logical breaker family. Safe to call
// under the family's existing once_flag.
void configure_rasn_shared_breaker_registry(circuit_breaker_registry &registry,
                                            const std::string &scope);

} // namespace rasn
} // namespace dsn
