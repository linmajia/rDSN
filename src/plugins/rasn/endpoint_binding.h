#pragma once

#include <dsn/service_api_cpp.h>

#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace dsn {
namespace rasn {

struct agent_descriptor;

// rASN-local scalar endpoint storage. Inbound construction is explicit to avoid
// accidental wrapper selection; assignment remains available at visible
// `.address = rpc_address` storage boundaries, and outbound conversion remains
// available at existing RPC client/return boundaries. It allocates and owns
// nothing: URI/GROUP payloads retain rpc_address's raw, non-owning lifetime
// contract, so the underlying handle must outlive every use of this value.
class endpoint_address
{
public:
    endpoint_address() = default;
    endpoint_address(const endpoint_address &) = default;
    endpoint_address &operator=(const endpoint_address &) = default;
    endpoint_address(endpoint_address &&) = default;
    endpoint_address &operator=(endpoint_address &&) = default;

    explicit endpoint_address(const ::dsn::rpc_address &address)
        : _address(address.c_addr())
    {
    }

    endpoint_address &operator=(const ::dsn::rpc_address &address)
    {
        _address = address.c_addr();
        return *this;
    }

    operator ::dsn::rpc_address() const
    {
        return ::dsn::rpc_address(_address);
    }

    dsn_address_t c_addr() const noexcept { return _address; }
    dsn_host_type_t type() const noexcept
    {
        return static_cast<dsn_host_type_t>(_address.u.v4.type);
    }
    uint32_t ip() const noexcept
    {
        return static_cast<uint32_t>(_address.u.v4.ip);
    }
    uint16_t port() const noexcept
    {
        return static_cast<uint16_t>(_address.u.v4.port);
    }
    bool is_invalid() const noexcept { return type() == HOST_TYPE_INVALID; }
    const char *to_string() const { return dsn_address_to_string(_address); }

    bool operator==(const endpoint_address &other) const
    {
        return ::dsn::rpc_address(_address) ==
               ::dsn::rpc_address(other._address);
    }
    bool operator!=(const endpoint_address &other) const
    {
        return !(*this == other);
    }
    bool operator==(const ::dsn::rpc_address &other) const
    {
        return *this == endpoint_address(other);
    }
    bool operator!=(const ::dsn::rpc_address &other) const
    {
        return !(*this == other);
    }

private:
    dsn_address_t _address{};
};

struct endpoint_resolution
{
    endpoint_resolution() = default;
    endpoint_resolution(const endpoint_resolution &) = default;
    endpoint_resolution &operator=(const endpoint_resolution &) = default;
    endpoint_resolution(endpoint_resolution &&) = default;
    endpoint_resolution &operator=(endpoint_resolution &&) = default;

    bool ok = false;
    bool found = false;
    endpoint_address address;
    std::string source;
    std::string error;
};

struct endpoint_snapshot
{
    endpoint_snapshot() = default;
    endpoint_snapshot(const endpoint_snapshot &) = default;
    endpoint_snapshot &operator=(const endpoint_snapshot &) = default;
    endpoint_snapshot(endpoint_snapshot &&) = default;
    endpoint_snapshot &operator=(endpoint_snapshot &&) = default;

    bool ok = false;
    endpoint_address address;
    std::string source;
    std::string error;
    uint64_t generation = 0;
    bool refreshable = false;
};

enum class endpoint_refresh_outcome
{
    rebound,
    unchanged,
    failed,
    superseded
};

struct endpoint_refresh_result
{
    endpoint_refresh_result() = default;
    endpoint_refresh_result(const endpoint_refresh_result &) = default;
    endpoint_refresh_result &
    operator=(const endpoint_refresh_result &) = default;
    endpoint_refresh_result(endpoint_refresh_result &&) = default;
    endpoint_refresh_result &
    operator=(endpoint_refresh_result &&) = default;

    endpoint_refresh_outcome outcome = endpoint_refresh_outcome::failed;
    endpoint_snapshot endpoint;
};

typedef std::function<endpoint_resolution()> endpoint_resolver;

// A race-free endpoint cache. Resolution runs without the state mutex held, one
// refresh is shared by concurrent callers, and a stale resolver result cannot
// overwrite a newer reset/rebind generation.
class refreshable_endpoint_binding
{
public:
    refreshable_endpoint_binding(const std::string &identity,
                                 const ::dsn::rpc_address &fallback,
                                 const std::string &fallback_source,
                                 bool resolve_on_first_use,
                                 endpoint_resolver resolver = endpoint_resolver());

    endpoint_snapshot current();
    endpoint_refresh_result refresh(uint64_t expected_generation);
    void reset(const ::dsn::rpc_address &fallback,
               const std::string &fallback_source,
               bool resolve_on_first_use,
               endpoint_resolver resolver);

    std::string identity() const;
    std::string describe() const;
    bool refreshable() const;
    void record_exhausted() const;

private:
    class refresh_flight_guard;

    endpoint_resolution run_resolver() const;
    endpoint_snapshot snapshot_locked() const;
    endpoint_refresh_result complete_resolution(refresh_flight_guard &flight,
                                                uint64_t expected_generation,
                                                bool initial,
                                                endpoint_resolution resolved);
    endpoint_refresh_result resolve_claimed(refresh_flight_guard &flight,
                                            uint64_t expected_generation,
                                            bool initial);
    bool finish_resolution_locked(uint64_t expected_generation,
                                  endpoint_resolution *next_current,
                                  uint64_t next_generation,
                                  endpoint_refresh_result *joiner_result) noexcept;
    void publish_exception_result_locked() noexcept;
    bool abandon_resolution(uint64_t expected_generation) noexcept;

    const std::string _identity;
    mutable std::mutex _lock;
    std::condition_variable _refresh_done;
    endpoint_resolution _fallback;
    endpoint_resolution _current;
    endpoint_resolver _resolver;
    uint64_t _generation;
    uint64_t _refresh_generation;
    uint64_t _refresh_sequence;
    bool _initialized;
    bool _refreshing;
    endpoint_refresh_result _last_refresh;
};

// Lease-publishes a standalone core-service endpoint through the existing
// registry. Registry itself never owns one of these, avoiding discovery
// recursion.
class rasn_core_service_registration
{
public:
    rasn_core_service_registration();
    ~rasn_core_service_registration();

    void start(const std::string &identity,
               const std::string &capability,
               const ::dsn::rpc_address &endpoint,
               const std::string &app_name);
    void start(const agent_descriptor &hosted_agent,
               const std::string &service_capability,
               const ::dsn::rpc_address &endpoint,
               const std::string &app_name);
    void stop();

    rasn_core_service_registration(
        const rasn_core_service_registration &) = delete;
    rasn_core_service_registration &operator=(
        const rasn_core_service_registration &) = delete;

private:
    struct impl;
    std::unique_ptr<impl> _impl;
};

// Query the existing rASN registry capability API. Local/co-located descriptors
// are checked first; otherwise the configured registry client (including its
// rDSN group failover) is used. A successful empty query may return the supplied
// static fallback, but registry transport/backend failures remain failures.
endpoint_resolution resolve_registry_endpoint(const std::string &capability,
                                              const std::string &preferred_identity,
                                              const ::dsn::rpc_address &registry,
                                              const ::dsn::rpc_address &fallback,
                                              const std::string &fallback_source,
                                              std::chrono::milliseconds timeout);

} // namespace rasn
} // namespace dsn
