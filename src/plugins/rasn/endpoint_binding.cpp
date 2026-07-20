#include <rasn/endpoint_binding.h>

#include <rasn/agent_registry.h>
#include <rasn/agent_runtime.h>
#include <rasn/metrics.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <limits>
#include <sstream>
#include <type_traits>
#include <tuple>
#include <utility>
#include <vector>

namespace dsn {
namespace rasn {

namespace {

endpoint_resolution select_registry_candidate(const std::string &preferred_identity,
                                              std::vector<agent_descriptor> candidates,
                                              const std::string &source)
{
    endpoint_resolution result;
    result.ok = true;
    if (candidates.empty())
    {
        return result;
    }

    std::sort(candidates.begin(),
              candidates.end(),
              [&preferred_identity](const agent_descriptor &left,
                                    const agent_descriptor &right) {
                  const bool left_preferred =
                      left.agent_id == preferred_identity ||
                      left.role == preferred_identity ||
                      left.app_name == preferred_identity;
                  const bool right_preferred =
                      right.agent_id == preferred_identity ||
                      right.role == preferred_identity ||
                      right.app_name == preferred_identity;
                  if (left_preferred != right_preferred)
                  {
                      return left_preferred;
                  }
                  return left.agent_id < right.agent_id;
              });

    for (const agent_descriptor &candidate : candidates)
    {
        ::dsn::rpc_address address;
        if (!candidate.endpoint_uri.empty())
        {
            address = ::dsn::url_host_address(candidate.endpoint_uri.c_str());
        }
        else if (!candidate.host.empty() && candidate.port > 0 &&
                 candidate.port <= (std::numeric_limits<uint16_t>::max)())
        {
            address.assign_ipv4(candidate.host.c_str(),
                                static_cast<uint16_t>(candidate.port));
        }
        if (!address.is_invalid())
        {
            result.found = true;
            result.address = address;
            result.source = source;
            return result;
        }
    }

    result.ok = false;
    result.error = "registry capability '" + preferred_identity +
                   "' returned no valid endpoint";
    return result;
}

bool core_service_registration_enabled()
{
    return ::dsn_config_get_value_bool(
        "rasn.registry",
        "dynamic_registration",
        true,
        "Register built-in rASN services through registry RPC");
}

std::chrono::milliseconds core_service_registration_timeout()
{
    return std::chrono::milliseconds(::dsn_config_get_value_uint64(
        "rasn.registry",
        "registration_timeout_ms",
        1000,
        "rASN registry register/heartbeat RPC timeout"));
}

uint64_t core_service_heartbeat_ms()
{
    return ::dsn_config_get_value_uint64(
        "rasn.registry",
        "heartbeat_ms",
        2000,
        "rASN registry heartbeat interval in milliseconds");
}

endpoint_snapshot snapshot_for_resolution(const endpoint_resolution &resolution,
                                          uint64_t generation,
                                          bool refreshable)
{
    endpoint_snapshot snapshot;
    snapshot.ok =
        resolution.ok && resolution.found && !resolution.address.is_invalid();
    snapshot.address = resolution.address;
    snapshot.source = resolution.source;
    snapshot.error = resolution.error;
    snapshot.generation = generation;
    snapshot.refreshable = refreshable;
    return snapshot;
}

endpoint_refresh_metric
terminal_refresh_metric(endpoint_refresh_outcome outcome) noexcept
{
    switch (outcome)
    {
    case endpoint_refresh_outcome::rebound:
        return endpoint_refresh_metric::rebound;
    case endpoint_refresh_outcome::unchanged:
        return endpoint_refresh_metric::unchanged;
    case endpoint_refresh_outcome::failed:
        return endpoint_refresh_metric::failed;
    case endpoint_refresh_outcome::superseded:
        return endpoint_refresh_metric::superseded;
    }
    return endpoint_refresh_metric::exception;
}

void report_endpoint_refresh_diagnostic_drop() noexcept
{
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    if (!reported.test_and_set(std::memory_order_relaxed))
    {
        std::fputs(
            "rASN endpoint refresh diagnostic dropped after formatting/logging exception\n",
            stderr);
    }
}

void log_endpoint_refresh_result_noexcept(
    const std::string &identity,
    bool initial,
    const endpoint_refresh_result &result) noexcept
{
    try
    {
        switch (result.outcome)
        {
        case endpoint_refresh_outcome::failed:
            dwarn("rASN endpoint refresh failed identity=%s generation=%llu: %s",
                  identity.c_str(),
                  static_cast<unsigned long long>(
                      result.endpoint.generation),
                  result.endpoint.error.c_str());
            break;
        case endpoint_refresh_outcome::rebound:
            dinfo("rASN endpoint rebound identity=%s generation=%llu endpoint=%s source=%s",
                  identity.c_str(),
                  static_cast<unsigned long long>(
                      result.endpoint.generation),
                  result.endpoint.address.to_string(),
                  result.endpoint.source.c_str());
            break;
        case endpoint_refresh_outcome::unchanged:
            if (!initial)
            {
                dinfo("rASN endpoint refresh unchanged identity=%s generation=%llu endpoint=%s source=%s",
                      identity.c_str(),
                      static_cast<unsigned long long>(
                          result.endpoint.generation),
                      result.endpoint.address.to_string(),
                      result.endpoint.source.c_str());
            }
            break;
        case endpoint_refresh_outcome::superseded:
            break;
        }
    }
    catch (...)
    {
        report_endpoint_refresh_diagnostic_drop();
    }
}

} // namespace

struct rasn_core_service_registration::impl
{
    std::mutex lock;
    agent_descriptor descriptor;
    ::dsn::rpc_address registry;
    ::dsn::task_ptr timer;
    bool active = false;

    void publish(bool heartbeat)
    {
        agent_descriptor current;
        ::dsn::rpc_address registry_address;
        {
            std::lock_guard<std::mutex> guard(lock);
            if (!active)
            {
                return;
            }
            current = descriptor;
            registry_address = registry;
        }

        std::string local_error;
        if (heartbeat)
        {
            if (!global_agent_registry().heartbeat(current, &local_error))
            {
                (void)global_agent_registry().register_agent(
                    current, &local_error, true);
            }
        }
        else
        {
            (void)global_agent_registry().register_agent(
                current, &local_error, true);
        }
        if (!local_error.empty())
        {
            dwarn("rASN core service registry publication failed identity=%s: %s",
                  current.role.c_str(),
                  local_error.c_str());
        }

        rasn_registry_client client(registry_address);
        ::dsn::error_code err;
        agent_response response;
        if (heartbeat)
        {
            std::tie(err, response) = client.heartbeat_sync(
                current, core_service_registration_timeout());
            if (err == ::dsn::ERR_OK &&
                registry_response_is_agent_not_found(response))
            {
                std::tie(err, response) = client.register_sync(
                    current, core_service_registration_timeout());
            }
        }
        else
        {
            std::tie(err, response) = client.register_sync(
                current, core_service_registration_timeout());
        }
        if (err != ::dsn::ERR_OK)
        {
            dwarn("rASN core service registry RPC failed identity=%s: %s",
                  current.role.c_str(),
                  err.to_string());
        }
        else if (!response.ok)
        {
            dwarn("rASN core service registry rejected identity=%s: %s",
                  current.role.c_str(),
                  response.error.message.c_str());
        }
    }
};

rasn_core_service_registration::rasn_core_service_registration()
    : _impl(new impl())
{
}

rasn_core_service_registration::~rasn_core_service_registration()
{
    stop();
}

void rasn_core_service_registration::start(
    const std::string &identity,
    const std::string &capability,
    const ::dsn::rpc_address &endpoint,
    const std::string &app_name)
{
    agent_descriptor descriptor;
    descriptor.agent_id = identity;
    descriptor.role = identity;
    descriptor.version = "prototype";
    descriptor.health = "healthy";
    start(descriptor, capability, endpoint, app_name);
}

void rasn_core_service_registration::start(
    const agent_descriptor &hosted_agent,
    const std::string &service_capability,
    const ::dsn::rpc_address &endpoint,
    const std::string &app_name)
{
    stop();
    if (!core_service_registration_enabled() || endpoint.is_invalid() ||
        hosted_agent.agent_id.empty())
    {
        return;
    }

    agent_descriptor descriptor = hosted_agent;
    descriptor.agent_id += "@" + endpoint.to_std_string();
    descriptor.app_name = app_name;
    descriptor.host.clear();
    descriptor.port = endpoint.type() == HOST_TYPE_IPV4 ? endpoint.port() : 0;
    descriptor.endpoint_uri = endpoint.to_std_string();
    descriptor.health = "healthy";
    const auto service_capability_it =
        std::find_if(descriptor.capabilities.begin(),
                     descriptor.capabilities.end(),
                     [&service_capability](const agent_capability &capability) {
                         return capability.name == service_capability;
                     });
    if (service_capability_it == descriptor.capabilities.end())
    {
        descriptor.capabilities.push_back(make_capability(
            service_capability,
            "service_request",
            "service_response",
            "service_endpoint"));
    }

    {
        std::lock_guard<std::mutex> guard(_impl->lock);
        _impl->descriptor = descriptor;
        _impl->registry = configured_rasn_registry_address();
        _impl->active = true;
    }
    _impl->publish(false);

    const uint64_t heartbeat_ms = core_service_heartbeat_ms();
    if (heartbeat_ms == 0)
    {
        return;
    }
    const std::chrono::milliseconds interval(heartbeat_ms);
    ::dsn::task_ptr timer = ::dsn::tasking::enqueue_timer(
        LPC_RASN_REGISTRY_HEARTBEAT_TIMER,
        nullptr,
        [this]() { _impl->publish(true); },
        interval,
        0,
        interval);
    bool cancel_timer = false;
    {
        std::lock_guard<std::mutex> guard(_impl->lock);
        if (_impl->active)
        {
            _impl->timer = timer;
        }
        else if (timer != nullptr)
        {
            cancel_timer = true;
        }
    }
    if (cancel_timer)
    {
        timer->cancel(true);
    }
}

void rasn_core_service_registration::stop()
{
    ::dsn::task_ptr timer;
    agent_descriptor descriptor;
    ::dsn::rpc_address registry;
    bool active = false;
    {
        std::lock_guard<std::mutex> guard(_impl->lock);
        active = _impl->active;
        _impl->active = false;
        timer = _impl->timer;
        _impl->timer = nullptr;
        descriptor = _impl->descriptor;
        registry = _impl->registry;
    }
    if (timer != nullptr)
    {
        timer->cancel(true);
    }
    if (!active || descriptor.agent_id.empty())
    {
        return;
    }

    global_agent_registry().unregister_agent(descriptor.agent_id);
    rasn_registry_client client(registry);
    const std::pair< ::dsn::error_code, agent_response> response =
        client.unregister_sync(
            descriptor.agent_id, core_service_registration_timeout());
    if (response.first != ::dsn::ERR_OK)
    {
        dwarn("rASN core service registry unregister failed identity=%s: %s",
              descriptor.role.c_str(),
              response.first.to_string());
    }
}

class refreshable_endpoint_binding::refresh_flight_guard
{
public:
    refresh_flight_guard(refreshable_endpoint_binding &owner,
                         uint64_t expected_generation) noexcept
        : _owner(owner),
          _expected_generation(expected_generation),
          _metrics(nullptr),
          _owns_flight(true),
          _notify_pending(false),
          _attempt_recorded(false),
          _terminal_staged(false),
          _terminal_metric(endpoint_refresh_metric::exception)
    {
    }

    ~refresh_flight_guard() noexcept
    {
        if (_owns_flight &&
            _owner.abandon_resolution(_expected_generation))
        {
            _owns_flight = false;
            _notify_pending = true;
        }
        notify();
        if (_attempt_recorded)
        {
            _metrics->on_endpoint_refresh(
                _terminal_staged ? _terminal_metric
                                 : endpoint_refresh_metric::exception);
        }
    }

    void record_attempt()
    {
        metrics_registry &metrics = metrics_registry::instance();
        _metrics = &metrics;
        _metrics->on_endpoint_refresh(endpoint_refresh_metric::attempt);
        _attempt_recorded = true;
    }

    bool finish_locked(endpoint_resolution *next_current,
                       uint64_t next_generation,
                       endpoint_refresh_result *joiner_result) noexcept
    {
        if (_owns_flight &&
            _owner.finish_resolution_locked(_expected_generation,
                                            next_current,
                                            next_generation,
                                            joiner_result))
        {
            _owns_flight = false;
            _notify_pending = true;
            return true;
        }
        return false;
    }

    void notify() noexcept
    {
        if (_notify_pending)
        {
            _notify_pending = false;
            _owner._refresh_done.notify_all();
        }
    }

    void stage_terminal(endpoint_refresh_metric metric) noexcept
    {
        _terminal_metric = metric;
        _terminal_staged = true;
    }

private:
    refresh_flight_guard(const refresh_flight_guard &) = delete;
    refresh_flight_guard &operator=(const refresh_flight_guard &) = delete;

    refreshable_endpoint_binding &_owner;
    uint64_t _expected_generation;
    metrics_registry *_metrics;
    bool _owns_flight;
    bool _notify_pending;
    bool _attempt_recorded;
    bool _terminal_staged;
    endpoint_refresh_metric _terminal_metric;
};

static_assert(std::is_trivially_copyable<dsn_address_t>::value,
              "endpoint address storage must remain scalar");
static_assert(std::is_nothrow_copy_constructible<dsn_address_t>::value,
              "endpoint address scalar copies must be no-throw");
static_assert(std::is_nothrow_copy_assignable<dsn_address_t>::value,
              "endpoint address scalar publication must be no-throw");
static_assert(sizeof(endpoint_address) == sizeof(dsn_address_t),
              "endpoint address must add no state");
static_assert(alignof(endpoint_address) == alignof(dsn_address_t),
              "endpoint address must retain scalar alignment");
static_assert(std::is_standard_layout<endpoint_address>::value,
              "endpoint address must remain a scalar value wrapper");
static_assert(std::is_trivially_copyable<endpoint_address>::value,
              "endpoint address copies must remain scalar");
static_assert(std::is_nothrow_move_constructible<endpoint_address>::value,
              "endpoint address transfer must be no-throw");
static_assert(std::is_nothrow_move_assignable<endpoint_address>::value,
              "endpoint address publication must be no-throw");
static_assert(
    std::is_constructible<endpoint_address,
                          const ::dsn::rpc_address &>::value,
    "rpc addresses must support intentional endpoint storage");
static_assert(
    !std::is_convertible<const ::dsn::rpc_address &, endpoint_address>::value,
    "rpc addresses must not implicitly enter endpoint storage");
static_assert(
    std::is_assignable<endpoint_address &,
                       const ::dsn::rpc_address &>::value,
    "rpc addresses must remain assignable at explicit storage boundaries");
static_assert(
    std::is_convertible<const endpoint_address &,
                        ::dsn::rpc_address>::value,
    "stored endpoints must remain compatible with rpc client boundaries");
static_assert(std::is_nothrow_move_constructible<std::string>::value,
              "endpoint string transfer must be no-throw");
static_assert(std::is_nothrow_move_assignable<std::string>::value,
              "endpoint string publication must be no-throw");
static_assert(std::is_nothrow_move_assignable<endpoint_resolution>::value,
              "endpoint resolution publication must be no-throw");
static_assert(std::is_nothrow_move_constructible<endpoint_resolution>::value,
              "endpoint resolution transfer must be no-throw");
static_assert(std::is_nothrow_move_assignable<endpoint_refresh_result>::value,
              "endpoint refresh result publication must be no-throw");
static_assert(std::is_nothrow_move_constructible<endpoint_refresh_result>::value,
              "endpoint refresh result return must be no-throw");
static_assert(std::is_nothrow_move_assignable<endpoint_snapshot>::value,
              "endpoint snapshot publication must be no-throw");
static_assert(std::is_nothrow_move_constructible<endpoint_snapshot>::value,
              "endpoint snapshot return must be no-throw");
static_assert(
    std::is_nothrow_destructible<endpoint_resolver>::value,
    "resolver destruction must not throw during refresh unwinding");
static_assert(
    std::is_nothrow_destructible<std::lock_guard<std::mutex>>::value,
    "refresh mutex guards must release without throwing");
static_assert(
    std::is_nothrow_destructible<std::unique_lock<std::mutex>>::value,
    "refresh wait locks must release without throwing");
static_assert(std::is_nothrow_destructible<std::mutex>::value,
              "refresh mutex destruction must not throw");
static_assert(
    std::is_nothrow_destructible<std::condition_variable>::value,
    "refresh condition-variable destruction must not throw");
static_assert(
    noexcept(std::declval<std::condition_variable &>().notify_all()),
    "refresh waiter notification must not throw");
static_assert(
    noexcept(std::declval<metrics_registry &>().on_endpoint_refresh(
        endpoint_refresh_metric::attempt)),
    "refresh lifecycle accounting must not throw");
static_assert(noexcept(std::declval<std::string &>().clear()),
              "allocation-free refresh failure fallback must not throw");

refreshable_endpoint_binding::refreshable_endpoint_binding(
    const std::string &identity,
    const ::dsn::rpc_address &fallback,
    const std::string &fallback_source,
    bool resolve_on_first_use,
    endpoint_resolver resolver)
    : _identity(identity),
      _resolver(std::move(resolver)),
      _generation(1),
      _refresh_generation(0),
      _refresh_sequence(0),
      _initialized(!resolve_on_first_use || !_resolver),
      _refreshing(false)
{
    _fallback.ok = !fallback.is_invalid();
    _fallback.found = _fallback.ok;
    _fallback.address = fallback;
    _fallback.source = fallback_source;
    _current = _fallback;
    _last_refresh.endpoint = snapshot_locked();
}

endpoint_resolution refreshable_endpoint_binding::run_resolver() const
{
    endpoint_resolver resolver;
    {
        std::lock_guard<std::mutex> guard(_lock);
        resolver = _resolver;
    }
    if (!resolver)
    {
        endpoint_resolution result;
        result.ok = true;
        return result;
    }
    return resolver();
}

endpoint_snapshot refreshable_endpoint_binding::snapshot_locked() const
{
    endpoint_snapshot snapshot;
    snapshot.ok = _current.ok && _current.found && !_current.address.is_invalid();
    snapshot.address = _current.address;
    snapshot.source = _current.source;
    snapshot.error = _current.error;
    snapshot.generation = _generation;
    snapshot.refreshable = static_cast<bool>(_resolver);
    return snapshot;
}

endpoint_refresh_result refreshable_endpoint_binding::complete_resolution(
    refresh_flight_guard &flight,
    uint64_t expected_generation,
    bool initial,
    endpoint_resolution resolved)
{
    const bool usable =
        resolved.ok && resolved.found && !resolved.address.is_invalid();
    if (!usable)
    {
        resolved.ok = false;
        resolved.found = false;
        if (resolved.error.empty())
        {
            resolved.error = "endpoint resolver returned no usable endpoint";
        }
    }

    endpoint_refresh_result result;
    endpoint_refresh_result joiner_result;
    endpoint_resolution next_current(std::move(resolved));
    bool published = false;
    {
        std::lock_guard<std::mutex> guard(_lock);
        if (_generation != expected_generation)
        {
            result.outcome = endpoint_refresh_outcome::superseded;
            result.endpoint = snapshot_locked();
            published = flight.finish_locked(nullptr, 0, nullptr);
        }
        else
        {
            uint64_t next_generation = expected_generation;
            if (!usable)
            {
                if (!initial)
                {
                    ++next_generation;
                }
                result.outcome = endpoint_refresh_outcome::failed;
            }
            else
            {
                const bool endpoint_changed =
                    !_current.found ||
                    _current.address != next_current.address;
                if (!initial && endpoint_changed)
                {
                    ++next_generation;
                    result.outcome = endpoint_refresh_outcome::rebound;
                }
                else
                {
                    result.outcome = endpoint_refresh_outcome::unchanged;
                }
            }

            result.endpoint = snapshot_for_resolution(
                next_current,
                next_generation,
                static_cast<bool>(_resolver));
            joiner_result = result;
            published = flight.finish_locked(
                &next_current, next_generation, &joiner_result);
        }
    }
    flight.notify();
    if (published)
    {
        flight.stage_terminal(terminal_refresh_metric(result.outcome));
    }
    return result;
}

bool refreshable_endpoint_binding::finish_resolution_locked(
    uint64_t expected_generation,
    endpoint_resolution *next_current,
    uint64_t next_generation,
    endpoint_refresh_result *joiner_result) noexcept
{
    if (!_refreshing || _refresh_generation != expected_generation)
    {
        return false;
    }
    if (_generation == expected_generation &&
        next_current != nullptr &&
        joiner_result != nullptr)
    {
        _current = std::move(*next_current);
        _generation = next_generation;
        _initialized = true;
        _last_refresh = std::move(*joiner_result);
    }
    _refreshing = false;
    _refresh_generation = 0;
    ++_refresh_sequence;
    return true;
}

void refreshable_endpoint_binding::publish_exception_result_locked() noexcept
{
    try
    {
        endpoint_refresh_result failure;
        failure.outcome = endpoint_refresh_outcome::failed;
        failure.endpoint = snapshot_locked();
        failure.endpoint.ok = false;
        failure.endpoint.error = "endpoint refresh threw an exception";
        _last_refresh = std::move(failure);
        return;
    }
    catch (...)
    {
        // Preserve the original exception. This scalar-only fallback requires no
        // allocation and still gives joiners an unusable matching-generation
        // completion if detailed diagnostics cannot be constructed.
        _last_refresh.outcome = endpoint_refresh_outcome::failed;
        _last_refresh.endpoint.ok = false;
        _last_refresh.endpoint.address = ::dsn::rpc_address();
        _last_refresh.endpoint.source.clear();
        _last_refresh.endpoint.error.clear();
        _last_refresh.endpoint.generation = _generation;
        _last_refresh.endpoint.refreshable =
            static_cast<bool>(_resolver);
    }
}

bool refreshable_endpoint_binding::abandon_resolution(
    uint64_t expected_generation) noexcept
{
    std::lock_guard<std::mutex> guard(_lock);
    if (!_refreshing || _refresh_generation != expected_generation)
    {
        return false;
    }
    if (_generation == expected_generation)
    {
        publish_exception_result_locked();
    }
    _refreshing = false;
    _refresh_generation = 0;
    ++_refresh_sequence;
    return true;
}

endpoint_refresh_result refreshable_endpoint_binding::resolve_claimed(
    refresh_flight_guard &flight,
    uint64_t expected_generation,
    bool initial)
{
    flight.record_attempt();
    endpoint_resolution resolved = run_resolver();
    endpoint_refresh_result result = complete_resolution(
        flight, expected_generation, initial, std::move(resolved));
    log_endpoint_refresh_result_noexcept(_identity, initial, result);
    return result;
}

endpoint_snapshot refreshable_endpoint_binding::current()
{
    uint64_t expected_generation = 0;
    for (;;)
    {
        std::unique_lock<std::mutex> guard(_lock);
        if (_initialized)
        {
            return snapshot_locked();
        }
        if (_refreshing)
        {
            const uint64_t observed_sequence = _refresh_sequence;
            _refresh_done.wait(guard, [this, observed_sequence] {
                return !_refreshing || _refresh_sequence != observed_sequence;
            });
            continue;
        }
        _refreshing = true;
        _refresh_generation = _generation;
        expected_generation = _generation;
        break;
    }

    refresh_flight_guard flight(*this, expected_generation);
    endpoint_refresh_result completed =
        resolve_claimed(flight, expected_generation, true);
    return std::move(completed.endpoint);
}

endpoint_refresh_result
refreshable_endpoint_binding::refresh(uint64_t expected_generation)
{
    for (;;)
    {
        std::unique_lock<std::mutex> guard(_lock);
        if (!_resolver)
        {
            endpoint_refresh_result result;
            result.outcome = endpoint_refresh_outcome::unchanged;
            result.endpoint = snapshot_locked();
            return result;
        }
        if (_generation != expected_generation)
        {
            endpoint_refresh_result result;
            result.outcome = endpoint_refresh_outcome::superseded;
            result.endpoint = snapshot_locked();
            return result;
        }
        if (_refreshing)
        {
            const uint64_t joined_generation = _refresh_generation;
            const uint64_t observed_sequence = _refresh_sequence;
            _refresh_done.wait(guard, [this, observed_sequence] {
                return !_refreshing || _refresh_sequence != observed_sequence;
            });
            if (!_refreshing && joined_generation == expected_generation)
            {
                return _last_refresh;
            }
            continue;
        }
        _refreshing = true;
        _refresh_generation = expected_generation;
        break;
    }

    refresh_flight_guard flight(*this, expected_generation);
    return resolve_claimed(flight, expected_generation, false);
}

void refreshable_endpoint_binding::reset(const ::dsn::rpc_address &fallback,
                                         const std::string &fallback_source,
                                         bool resolve_on_first_use,
                                         endpoint_resolver resolver)
{
    endpoint_resolver previous_resolver;
    {
        std::lock_guard<std::mutex> guard(_lock);
        ++_generation;
        _fallback = endpoint_resolution();
        _fallback.ok = !fallback.is_invalid();
        _fallback.found = _fallback.ok;
        _fallback.address = fallback;
        _fallback.source = fallback_source;
        _current = _fallback;
        previous_resolver.swap(_resolver);
        _resolver.swap(resolver);
        _initialized = !resolve_on_first_use || !_resolver;
        // The matching owner guard remains responsible for clearing an active
        // flight and notifying its waiters; reset only supersedes its generation.
        ++_refresh_sequence;
        _last_refresh.outcome = endpoint_refresh_outcome::superseded;
        _last_refresh.endpoint = snapshot_locked();
    }
    _refresh_done.notify_all();
}

std::string refreshable_endpoint_binding::identity() const
{
    return _identity;
}

std::string refreshable_endpoint_binding::describe() const
{
    std::lock_guard<std::mutex> guard(_lock);
    const endpoint_snapshot snapshot = snapshot_locked();
    std::ostringstream out;
    out << _identity << " generation=" << snapshot.generation
        << " refreshable=" << (snapshot.refreshable ? "true" : "false");
    if (snapshot.ok)
    {
        out << " endpoint=" << snapshot.address.to_string()
            << " source=" << snapshot.source;
    }
    else
    {
        out << " unavailable=" << snapshot.error;
    }
    return out.str();
}

bool refreshable_endpoint_binding::refreshable() const
{
    std::lock_guard<std::mutex> guard(_lock);
    return static_cast<bool>(_resolver);
}

void refreshable_endpoint_binding::record_exhausted() const
{
    metrics_registry::instance().on_endpoint_refresh(
        endpoint_refresh_metric::exhausted);
    dwarn("rASN endpoint refresh exhausted identity=%s", _identity.c_str());
}

endpoint_resolution resolve_registry_endpoint(
    const std::string &capability,
    const std::string &preferred_identity,
    const ::dsn::rpc_address &registry,
    const ::dsn::rpc_address &fallback,
    const std::string &fallback_source,
    std::chrono::milliseconds timeout)
{
    std::vector<agent_descriptor> local;
    std::string local_error;
    if (!global_agent_registry().query_by_capability(
            capability, true, &local, &local_error))
    {
        endpoint_resolution failed;
        failed.error = "local registry query failed for capability '" +
                       capability + "': " + local_error;
        return failed;
    }
    endpoint_resolution selected =
        select_registry_candidate(preferred_identity, local, "registry:local");
    if (!selected.ok || selected.found)
    {
        return selected;
    }

    if (registry.is_invalid())
    {
        endpoint_resolution failed;
        failed.error = "configured registry endpoint is invalid for capability '" +
                       capability + "'";
        return failed;
    }

    registry_query_request request;
    request.capability = capability;
    request.healthy_only = true;
    rasn_registry_client client(registry);
    ::dsn::error_code err;
    registry_query_response response;
    std::tie(err, response) = client.query_sync(request, timeout);
    if (err != ::dsn::ERR_OK)
    {
        endpoint_resolution failed;
        failed.error = "registry query transport failure for capability '" +
                       capability + "': " + err.to_string();
        return failed;
    }
    if (!response.ok)
    {
        endpoint_resolution failed;
        failed.error = "registry query failed for capability '" + capability +
                       "': " + response.error;
        return failed;
    }

    selected =
        select_registry_candidate(preferred_identity, response.agents, "registry");
    if (!selected.ok || selected.found)
    {
        return selected;
    }

    if (!fallback.is_invalid())
    {
        endpoint_resolution result;
        result.ok = true;
        result.found = true;
        result.address = fallback;
        result.source = fallback_source;
        return result;
    }
    selected.error = "registry returned no live endpoint for capability '" +
                     capability + "'";
    return selected;
}

} // namespace rasn
} // namespace dsn
