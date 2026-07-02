#pragma once

#include "../../agent_services.h"
#include "../../rasn.code.definition.h"
#include "../../workflow.h"
#include "skills.h"

#include <dsn/service_api_cpp.h>
#include <dsn/cpp/task_helper.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

void debug_log(const std::string &message);

class codepilot_cli
{
public:
    codepilot_cli();

    int run(const std::vector<std::string> &args);
    int repl();

    // Ask a running interactive repl() to exit at its next loop iteration. Safe to
    // call from another thread (e.g. service_app::stop) so shutdown need not block
    // on the non-preemptible, stdin-parked CLI task.
    void request_shutdown() { _shutdown_requested.store(true); }

private:
    int run_command(const std::vector<std::string> &args, bool interactive_mode = false);
    int ask(const std::string &prompt, bool planning_mode);
    int stream(const std::string &prompt);
    int agent(const std::string &prompt);
    int run_workflow(const std::string &path, const std::string &run_id = "", bool resume = false);
    int run_tool(const std::vector<std::string> &args);
    int run_state(const std::vector<std::string> &args);
    int run_registry(const std::vector<std::string> &args);
    int run_agent_control(const std::vector<std::string> &args);
    int run_observe(const std::vector<std::string> &args);
    int run_skill(const std::vector<std::string> &args);
    int run_selftest(const std::vector<std::string> &args);
    int run_eval(const std::vector<std::string> &args);
    int enable_replay(const std::string &path);
    int set_provider(const std::string &provider_name);
    bool load_context_file(const std::string &path, std::string *error);
    bool approve_tool_invocation(const std::string &tool_name,
                                 const std::vector<std::string> &args,
                                 bool explicit_approval,
                                 std::vector<std::string> *policy_labels) const;
    void print_help(bool interactive_mode) const;
    std::string provider_summary() const;

    rasn_service_graph &_services;
    std::vector<std::string> _context;
    std::atomic<bool> _shutdown_requested{false};
};

class codepilot_app : public ::dsn::service_app
{
public:
    explicit codepilot_app(::dsn_gpid gpid) : ::dsn::service_app(gpid) {}

    virtual ::dsn::error_code start(int argc, char **argv);
    virtual ::dsn::error_code stop(bool cleanup = false);

private:
    void run_cli_task();

    codepilot_cli _cli;
    std::vector<std::string> _args;
    ::dsn::task_ptr _cli_task;
};

} // namespace rasn
} // namespace dsn
