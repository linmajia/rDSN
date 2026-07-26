#include <dsn/c/api_layer1.h>
#include <dsn/c/app_model.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::string config_path_from_args(int argc, char **argv, std::vector<char *> *gtest_args)
{
    std::string config_path;
    gtest_args->push_back(argv[0]);
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i] == nullptr ? "" : argv[i];
        const std::string prefix = "--rasn_dsn_config=";
        if (arg.compare(0, prefix.size(), prefix) == 0)
        {
            config_path = arg.substr(prefix.size());
        }
        else
        {
            gtest_args->push_back(argv[i]);
        }
    }
    return config_path;
}

std::string default_config_path()
{
    const char *tmp = std::getenv("TEMP");
    if (tmp == nullptr || *tmp == '\0')
    {
        tmp = std::getenv("TMPDIR");
    }
    const std::string directory = (tmp == nullptr || *tmp == '\0') ? "." : tmp;
#if defined(_WIN32)
    return directory + "\\rasn-unit-test.config.ini";
#else
    return directory + "/rasn-unit-test.config.ini";
#endif
}

bool write_default_config(const std::string &path)
{
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!output)
    {
        return false;
    }

    output << "[modules]\n"
           << "dsn.tools.common\n"
           << "dsn.tools.emulator\n"
           << "dsn.tools.nfs\n"
           << "\n"
           << "[apps..default]\n"
           << "run = false\n"
           << "count = 1\n"
           << "network.client.RPC_CHANNEL_TCP = dsn::tools::asio_network_provider, 65536\n"
           << "network.client.RPC_CHANNEL_UDP = dsn::tools::asio_udp_provider, 65536\n"
           << "network.server.0.RPC_CHANNEL_TCP = dsn::tools::asio_network_provider, 65536\n"
           << "network.server.0.RPC_CHANNEL_UDP = dsn::tools::asio_udp_provider, 65536\n"
           << "\n"
           << "[apps.mimic]\n"
           << "type = dsn.app.mimic\n"
           << "run = true\n"
           << "count = 1\n"
           << "pools = THREAD_POOL_DEFAULT, THREAD_POOL_META_SERVER\n"
           << "\n"
           << "[core]\n"
           << "tool = nativerun\n"
           << "toollets = tracer, profiler\n"
           << "pause_on_start = false\n"
           << "cli_local = false\n"
           << "cli_remote = false\n"
           << "enable_default_app_mimic = true\n"
           << "logging_start_level = LOG_LEVEL_WARNING\n"
           << "logging_factory_name = dsn::tools::simple_logger\n"
           << "gtest = false\n"
           << "\n"
           << "[tools.simple_logger]\n"
           << "fast_flush = true\n"
           << "short_header = false\n"
           << "stderr_start_level = LOG_LEVEL_FATAL\n"
           << "\n"
           << "[tools.emulator]\n"
           << "random_seed = 0\n"
           << "\n"
           << "[rasn.workflow]\n"
           << "execution_lease_ms = 200\n"
           << "execution_lease_renew_ms = 50\n"
           << "\n"
           << "[rasn.state]\n"
           << "checkpoint_dir = rasn/state\n"
           << "\n"
           << "[rasn.service]\n"
           << "agent_message_bus_shard_count = 1\n"
           << "resource_budget_shard_count = 1\n"
           << "blackboard_shard_count = 1\n"
           << "human_interaction_shard_count = 1\n"
           << "\n"
           << "[network]\n"
           << "io_service_worker_count = 1\n"
           << "\n"
           << "[task..default]\n"
           << "is_trace = false\n"
           << "is_profile = false\n"
           << "allow_inline = false\n"
           << "rpc_call_channel = RPC_CHANNEL_TCP\n"
           << "rpc_message_header_format = dsn\n"
           << "rpc_timeout_milliseconds = 1000\n"
           << "\n"
           << "[threadpool..default]\n"
           << "\n"
           << "[threadpool.THREAD_POOL_DEFAULT]\n"
           << "partitioned = false\n"
           << "worker_count = 1\n"
           << "worker_priority = THREAD_xPRIORITY_NORMAL\n"
           << "\n"
           << "[threadpool.THREAD_POOL_META_SERVER]\n"
           << "partitioned = false\n"
           << "worker_count = 2\n"
           << "worker_priority = THREAD_xPRIORITY_NORMAL\n";
    return output.good();
}

} // namespace

int main(int argc, char **argv)
{
    std::vector<char *> gtest_args;
    std::string config_path = config_path_from_args(argc, argv, &gtest_args);
    if (config_path.empty())
    {
        config_path = default_config_path();
        if (!write_default_config(config_path))
        {
            std::fprintf(stderr, "failed to write rASN unit test config: %s\n", config_path.c_str());
            return 1;
        }
    }

    if (!::dsn_run_config(config_path.c_str(), false))
    {
        std::fprintf(stderr, "failed to initialize rDSN for rASN unit tests: %s\n", config_path.c_str());
        return 1;
    }

    int gtest_argc = static_cast<int>(gtest_args.size());
    ::testing::InitGoogleTest(&gtest_argc, gtest_args.data());
    const int result = RUN_ALL_TESTS();
    ::dsn_exit(result);
}
