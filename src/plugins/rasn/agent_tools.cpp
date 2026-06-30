#include "agent_tools.h"

#include <dsn/cpp/zlocks.h>
#include <dsn/service_api_cpp.h>

namespace dsn {
namespace rasn {

namespace {

class unconfigured_tool_provider : public agent_tool_provider
{
public:
    std::string describe_tools() const override
    {
        return "no application tool provider registered";
    }

    tool_result run(const std::string &name,
                    const std::vector<std::string> &args,
                    nucleus_runtime &runtime,
                    const agent_task &task) const override
    {
        tool_result result;
        result.ok = false;
        result.error = "no application tool provider registered";
        return result;
    }
};

::dsn::service::zlock &provider_factory_lock()
{
    static ::dsn::service::zlock lock;
    return lock;
}

agent_tool_provider_factory &provider_factory()
{
    static agent_tool_provider_factory factory = &create_unconfigured_tool_provider;
    return factory;
}

} // namespace

std::unique_ptr<agent_tool_provider> create_unconfigured_tool_provider()
{
    return std::unique_ptr<agent_tool_provider>(new unconfigured_tool_provider());
}

void register_default_tool_provider(agent_tool_provider_factory factory)
{
    dassert(factory != nullptr, "rASN tool provider factory cannot be null");
    ::dsn::service::zauto_lock guard(provider_factory_lock());
    provider_factory() = factory;
    dinfo("registered default rASN tool provider factory");
}

std::unique_ptr<agent_tool_provider> create_default_tool_provider()
{
    agent_tool_provider_factory factory = nullptr;
    {
        ::dsn::service::zauto_lock guard(provider_factory_lock());
        factory = provider_factory();
    }
    return factory();
}

} // namespace rasn
} // namespace dsn
