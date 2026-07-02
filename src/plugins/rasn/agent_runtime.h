#pragma once

#include <rasn/agent_types.h>

#include <dsn/cpp/zlocks.h>
#include <dsn/service_api_cpp.h>

#include <deque>
#include <set>
#include <string>

namespace dsn {
namespace rasn {

class agent_runtime
{
public:
    agent_runtime(const std::string &role, const std::string &agent_id);
    virtual ~agent_runtime() {}

    ::dsn::error_code start();
    ::dsn::error_code stop();

    bool is_started() const;
    agent_descriptor descriptor() const;
    void set_endpoint(const std::string &host, uint32_t port);
    void set_endpoint(const std::string &host, uint32_t port, const std::string &endpoint_uri);
    void set_endpoint_uri(const std::string &endpoint_uri);
    void set_health(const std::string &health);
    void add_capability(const agent_capability &capability);

    bool validate(const agent_request &request, agent_response *rejection) const;
    bool begin_request(const agent_request &request, agent_response *rejection) const;
    void finish_request(const agent_request &request) const;
    agent_response cancel_request(const agent_request &request) const;
    bool is_cancelled(const std::string &request_id) const;
    agent_response cancelled_response(const agent_request &request) const;
    agent_response reject(const agent_request &request,
                          const std::string &failure_class,
                          const std::string &code,
                          const std::string &message,
                          bool retryable) const;

protected:
    virtual ::dsn::error_code on_start();
    virtual ::dsn::error_code on_stop();

private:
    void remember_cancelled_request_locked(const std::string &request_id) const;
    void trim_cancelled_request_tombstones_locked() const;

    mutable ::dsn::service::zlock _lock;
    agent_descriptor _descriptor;
    bool _started;
    mutable std::set<std::string> _inflight_requests;
    mutable std::set<std::string> _cancelled_requests;
    mutable std::deque<std::string> _cancelled_order;
};

agent_capability make_capability(const std::string &name,
                                 const std::string &input_type,
                                 const std::string &output_type,
                                 const std::string &side_effect_class);

} // namespace rasn
} // namespace dsn
