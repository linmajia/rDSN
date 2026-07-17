#include "component_test_runner.h"
#include "core_test_environment.h"

#include <dsn/cpp/address.h>
#include <dsn/cpp/test_utils.h>
#include <dsn/cpp/utils.h>
#include <dsn/service_api_c.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

DEFINE_THREAD_POOL_CODE(THREAD_POOL_FOR_TEST_1)
DEFINE_THREAD_POOL_CODE(THREAD_POOL_FOR_TEST_2)

namespace dsn {
namespace tests {
namespace {

[[noreturn]] void run_registered_tests()
{
    const char *arguments = dsn_config_get_value_string(
        "core", "gtest_arguments", "", "arguments passed to GoogleTest");
    std::vector<std::string> argument_storage;
    ::dsn::utils::split_args(arguments, argument_storage);

    std::vector<char *> argument_pointers;
    char program_name[] = "rdsn.gtest";
    argument_pointers.push_back(program_name);
    for (auto &argument : argument_storage)
    {
        argument_pointers.push_back(&argument[0]);
    }

    int argument_count = static_cast<int>(argument_pointers.size());
    ::testing::InitGoogleTest(&argument_count, argument_pointers.data());
    const int result = RUN_ALL_TESTS();

#ifndef ENABLE_GCOV
    dsn_exit(result);
#else
    dsn_exit(0);
#endif
}

class component_test_app : public ::dsn::serverlet<component_test_app>,
                           public ::dsn::service_app
{
public:
    explicit component_test_app(dsn_gpid gpid)
        : ::dsn::serverlet<component_test_app>("test-server", 7), ::dsn::service_app(gpid)
    {
    }

    void on_rpc_test(const std::string &, ::dsn::rpc_replier<std::string> &replier)
    {
        replier(::dsn::task::get_current_node_name());
    }

    void on_rpc_string_test(dsn_message_t message)
    {
        std::string command;
        ::dsn::unmarshall(message, command);

        if (command == "expect_talk_to_others")
        {
            ::dsn::rpc_address next_address = ::dsn::service_app::primary_address();
            if (next_address.port() != TEST_PORT_END)
            {
                next_address.assign_ipv4(next_address.ip(), next_address.port() + 1);
                auto error = dsn_rpc_forward(message, next_address.c_addr());
                dassert(error == ERR_OK, "dsn_rpc_forward failed: %s", error_code(error).to_string());
            }
            else
            {
                reply(message, next_address.to_std_string());
            }
        }
        else if (command == "expect_no_reply")
        {
            if (::dsn::service_app::primary_address().port() == TEST_PORT_END)
            {
                reply(message, ::dsn::service_app::primary_address().to_std_string());
            }
        }
        else if (command.compare(0, 5, "echo ") == 0)
        {
            reply(message, command.substr(5));
        }
        else
        {
            derror("unknown command");
        }
    }

    ::dsn::error_code start(int argc, char **) override
    {
        if (argc == 1)
        {
            register_async_rpc_handler(
                RPC_TEST_HASH, "rpc.test.hash", &component_test_app::on_rpc_test);
            register_async_rpc_handler(
                RPC_TEST_HASH1, "rpc.test.hash1", &component_test_app::on_rpc_test);
            register_async_rpc_handler(
                RPC_TEST_HASH2, "rpc.test.hash2", &component_test_app::on_rpc_test);
            register_async_rpc_handler(
                RPC_TEST_HASH3, "rpc.test.hash3", &component_test_app::on_rpc_test);
            register_async_rpc_handler(
                RPC_TEST_HASH4, "rpc.test.hash4", &component_test_app::on_rpc_test);
            register_async_rpc_handler(
                RPC_TEST_HASH5, "rpc.test.hash5", &component_test_app::on_rpc_test);
            register_async_rpc_handler(
                RPC_TEST_HASH6, "rpc.test.hash6", &component_test_app::on_rpc_test);
            register_rpc_handler(RPC_TEST_STRING_COMMAND,
                                 "rpc.test.string.command",
                                 &component_test_app::on_rpc_string_test);
        }
        else
        {
            run_registered_tests();
        }

        return ::dsn::ERR_OK;
    }

    ::dsn::error_code stop(bool) override { return ::dsn::ERR_OK; }
};

} // namespace

int run_component_tests(int argc,
                        char **argv,
                        prepare_test_environment prepare_environment)
{
    register_core_test_components();
    if (prepare_environment != nullptr)
    {
        prepare_environment();
    }

    dassert(::dsn::register_app<component_test_app>("test"), "register test app failed");
    dsn_run(argc, argv, true);
    return 1;
}

} // namespace tests
} // namespace dsn
