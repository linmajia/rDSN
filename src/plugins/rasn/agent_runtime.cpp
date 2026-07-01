#include "agent_runtime.h"

namespace dsn {
namespace rasn {

namespace {

const size_t kMaxCancelledRequestTombstones = 1024;

std::string cancel_target_id(const agent_request &request)
{
    // In-flight requests and cancellation tombstones are keyed strictly by
    // request_id (validate_agent_request forces it non-empty before a request
    // can begin), so cancellation must use the same key. A task.id fallback
    // could never match an in-flight entry and would silently drop the cancel.
    return request.request_id;
}

} // namespace

agent_runtime::agent_runtime(const std::string &role, const std::string &agent_id) : _started(false)
{
    _descriptor.agent_id = agent_id;
    _descriptor.role = role;
    _descriptor.app_name = role;
    _descriptor.version = "prototype";
    _descriptor.health = "created";
}

::dsn::error_code agent_runtime::start()
{
    {
        ::dsn::service::zauto_lock guard(_lock);
        if (_started)
        {
            return ::dsn::ERR_OK;
        }
        _descriptor.health = "starting";
    }

    const ::dsn::error_code err = on_start();

    ::dsn::service::zauto_lock guard(_lock);
    if (err == ::dsn::ERR_OK)
    {
        _started = true;
        _descriptor.health = "healthy";
        dinfo("agent %s role %s started", _descriptor.agent_id.c_str(), _descriptor.role.c_str());
    }
    else
    {
        _descriptor.health = "failed";
        derror("agent %s role %s failed to start: %s",
               _descriptor.agent_id.c_str(),
               _descriptor.role.c_str(),
               err.to_string());
    }
    return err;
}

::dsn::error_code agent_runtime::stop()
{
    {
        ::dsn::service::zauto_lock guard(_lock);
        if (!_started)
        {
            return ::dsn::ERR_OK;
        }
        _descriptor.health = "stopping";
    }

    const ::dsn::error_code err = on_stop();

    ::dsn::service::zauto_lock guard(_lock);
    if (err == ::dsn::ERR_OK)
    {
        _started = false;
        _descriptor.health = "stopped";
        dinfo("agent %s role %s stopped", _descriptor.agent_id.c_str(), _descriptor.role.c_str());
    }
    else
    {
        _descriptor.health = "stop_failed";
        derror("agent %s role %s failed to stop: %s",
               _descriptor.agent_id.c_str(),
               _descriptor.role.c_str(),
               err.to_string());
    }
    return err;
}

bool agent_runtime::is_started() const
{
    ::dsn::service::zauto_lock guard(_lock);
    return _started;
}

agent_descriptor agent_runtime::descriptor() const
{
    ::dsn::service::zauto_lock guard(_lock);
    return _descriptor;
}

void agent_runtime::set_endpoint(const std::string &host, uint32_t port)
{
    set_endpoint(host, port, std::string());
}

void agent_runtime::set_endpoint(const std::string &host, uint32_t port, const std::string &endpoint_uri)
{
    ::dsn::service::zauto_lock guard(_lock);
    _descriptor.host = host;
    _descriptor.port = port;
    _descriptor.endpoint_uri = endpoint_uri;
}

void agent_runtime::set_endpoint_uri(const std::string &endpoint_uri)
{
    ::dsn::service::zauto_lock guard(_lock);
    _descriptor.endpoint_uri = endpoint_uri;
}

void agent_runtime::set_health(const std::string &health)
{
    ::dsn::service::zauto_lock guard(_lock);
    _descriptor.health = health;
}

void agent_runtime::add_capability(const agent_capability &capability)
{
    if (capability.name.empty())
    {
        dwarn("agent %s ignored empty capability", _descriptor.agent_id.c_str());
        return;
    }

    ::dsn::service::zauto_lock guard(_lock);
    for (const agent_capability &existing : _descriptor.capabilities)
    {
        if (existing.name == capability.name)
        {
            return;
        }
    }
    _descriptor.capabilities.push_back(capability);
}

bool agent_runtime::validate(const agent_request &request, agent_response *rejection) const
{
    std::string error;
    if (validate_agent_request(request, &error))
    {
        return true;
    }

    if (rejection != nullptr)
    {
        *rejection = reject(request, "validation", "invalid_request", error, false);
    }
    return false;
}

bool agent_runtime::begin_request(const agent_request &request, agent_response *rejection) const
{
    if (!validate(request, rejection))
    {
        return false;
    }

    bool already_cancelled = false;
    bool already_inflight = false;
    {
        ::dsn::service::zauto_lock guard(_lock);
        already_cancelled = _cancelled_requests.find(request.request_id) != _cancelled_requests.end();
        if (!already_cancelled)
        {
            already_inflight = !_inflight_requests.insert(request.request_id).second;
        }
    }

    if (already_cancelled)
    {
        if (rejection != nullptr)
        {
            *rejection = cancelled_response(request);
        }
        return false;
    }
    if (already_inflight)
    {
        if (rejection != nullptr)
        {
            *rejection =
                reject(request, "lifecycle", "duplicate_inflight_request", "request is already in flight", false);
        }
        return false;
    }
    return true;
}

void agent_runtime::finish_request(const agent_request &request) const
{
    if (request.request_id.empty())
    {
        return;
    }
    ::dsn::service::zauto_lock guard(_lock);
    _inflight_requests.erase(request.request_id);
    trim_cancelled_request_tombstones_locked();
}

agent_response agent_runtime::cancel_request(const agent_request &request) const
{
    const std::string request_id = cancel_target_id(request);
    agent_response response;
    response.request_id = request_id;
    response.trace_id = request.trace_id;

    if (request_id.empty())
    {
        response.ok = false;
        response.error = make_agent_error("validation",
                                          "invalid_cancel_request",
                                          "cancel request is missing a request id",
                                          false,
                                          descriptor().agent_id);
        return response;
    }

    bool in_flight = false;
    bool already_cancelled = false;
    {
        ::dsn::service::zauto_lock guard(_lock);
        in_flight = _inflight_requests.find(request_id) != _inflight_requests.end();
        already_cancelled = _cancelled_requests.find(request_id) != _cancelled_requests.end();
        if (in_flight && !already_cancelled)
        {
            remember_cancelled_request_locked(request_id);
        }
    }

    if (in_flight || already_cancelled)
    {
        response.ok = true;
        response.output = already_cancelled ? "cancel already requested for: " + request_id
                                            : "cancel requested for: " + request_id;
        return response;
    }

    response.ok = false;
    response.error = make_agent_error("lifecycle",
                                      "cancel_not_found",
                                      "no in-flight cancellable request: " + request_id,
                                      false,
                                      descriptor().agent_id);
    return response;
}

bool agent_runtime::is_cancelled(const std::string &request_id) const
{
    if (request_id.empty())
    {
        return false;
    }
    ::dsn::service::zauto_lock guard(_lock);
    return _cancelled_requests.find(request_id) != _cancelled_requests.end();
}

agent_response agent_runtime::cancelled_response(const agent_request &request) const
{
    agent_response response;
    response.request_id = request.request_id;
    response.trace_id = request.trace_id;
    response.ok = false;
    response.error = make_agent_error("lifecycle",
                                      "request_cancelled",
                                      "request cancelled: " + request.request_id,
                                      false,
                                      descriptor().agent_id);
    return response;
}

agent_response agent_runtime::reject(const agent_request &request,
                                     const std::string &failure_class,
                                     const std::string &code,
                                     const std::string &message,
                                     bool retryable) const
{
    agent_response response;
    response.request_id = request.request_id;
    response.trace_id = request.trace_id;
    response.ok = false;
    response.error = make_agent_error(failure_class, code, message, retryable, descriptor().agent_id);
    dwarn("agent %s rejected request %s: %s",
          response.error.source.c_str(),
          request.request_id.c_str(),
          message.c_str());
    return response;
}

void agent_runtime::remember_cancelled_request_locked(const std::string &request_id) const
{
    if (_cancelled_requests.insert(request_id).second)
    {
        _cancelled_order.push_back(request_id);
    }

    trim_cancelled_request_tombstones_locked();
}

void agent_runtime::trim_cancelled_request_tombstones_locked() const
{
    size_t inspected = 0;
    while (_cancelled_order.size() > kMaxCancelledRequestTombstones && inspected < _cancelled_order.size())
    {
        const std::string request_id = _cancelled_order.front();
        _cancelled_order.pop_front();
        if (_inflight_requests.find(request_id) != _inflight_requests.end())
        {
            _cancelled_order.push_back(request_id);
            ++inspected;
            continue;
        }

        _cancelled_requests.erase(request_id);
        inspected = 0;
    }
}

::dsn::error_code agent_runtime::on_start()
{
    return ::dsn::ERR_OK;
}

::dsn::error_code agent_runtime::on_stop()
{
    return ::dsn::ERR_OK;
}

agent_capability make_capability(const std::string &name,
                                 const std::string &input_type,
                                 const std::string &output_type,
                                 const std::string &side_effect_class)
{
    agent_capability capability;
    capability.name = name;
    capability.input_type = input_type;
    capability.output_type = output_type;
    capability.side_effect_class = side_effect_class;
    return capability;
}

} // namespace rasn
} // namespace dsn
