#pragma once

#include "../../agent_services.h"
#include "../../rasn.code.definition.h"

#include <dsn/cpp/task_helper.h>
#include <dsn/service_api_cpp.h>

#include <atomic>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

class srepilot_cli
{
public:
    srepilot_cli();

    int run(const std::vector<std::string> &args);
    int repl();

    void request_shutdown() { _shutdown_requested.store(true); }

private:
    int run_command(const std::vector<std::string> &args);
    int diagnose(const std::vector<std::string> &args);
    int runbook(const std::vector<std::string> &args);
    int status();
    int observe(const std::vector<std::string> &args);
    int selftest();
    int set_provider(const std::vector<std::string> &args);
    bool persist_response(const std::string &kind,
                          const agent_task &task,
                          const std::string &input,
                          const std::string &output,
                          std::string *stored_key);
    void print_help() const;

    rasn_service_graph &_services;
    std::atomic<bool> _shutdown_requested{false};
};

class srepilot_app : public ::dsn::service_app
{
public:
    explicit srepilot_app(::dsn_gpid gpid) : ::dsn::service_app(gpid) {}

    virtual ::dsn::error_code start(int argc, char **argv);
    virtual ::dsn::error_code stop(bool cleanup = false);

private:
    void run_cli_task();

    srepilot_cli _cli;
    std::vector<std::string> _args;
    ::dsn::task_ptr _cli_task;
};

} // namespace rasn
} // namespace dsn
