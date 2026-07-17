#pragma once

#include <dsn/tool-api/admission_controller.h>
#include <dsn/tool_api.h>

extern const dsn::threadpool_code THREAD_POOL_FOR_TEST_1;
extern const dsn::threadpool_code THREAD_POOL_FOR_TEST_2;

class admission_controller_for_test : public dsn::admission_controller
{
public:
    admission_controller_for_test(dsn::task_queue *queue, std::vector<std::string> &arguments)
        : dsn::admission_controller(queue, arguments), _arguments(arguments)
    {
    }

    bool is_task_accepted(dsn::task *) override { return true; }

    const std::vector<std::string> &arguments() const { return _arguments; }

private:
    std::vector<std::string> _arguments;
};

inline void register_core_test_components()
{
    dsn::tools::register_component_provider<admission_controller_for_test>(
        "dsn::tools::admission_controller_for_test");
}
