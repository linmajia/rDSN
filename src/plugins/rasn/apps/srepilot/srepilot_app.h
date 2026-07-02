#pragma once

#include "../../cli_app.h"
#include "../../rasn.code.definition.h"

#include <dsn/cpp/task_helper.h>
#include <dsn/service_api_cpp.h>

#include <string>
#include <vector>

namespace dsn {
namespace rasn {

class srepilot_cli : public rasn_cli_app_base
{
public:
    srepilot_cli();

private:
    std::vector<std::string> commands() const override;
    std::string repl_title() const override;
    std::string repl_prompt() const override;
    std::string repl_plain_text_behavior() const override;
    int run_command(const std::vector<std::string> &args, bool interactive_mode = false) override;
    void handle_plain_text(const std::string &line) override;
    int run_compat_prompt(const std::string &prompt, bool stream) override;
    void print_compat_help() const override;
    std::string version_string() const override;
    std::string compat_prompt_usage() const override;
    std::string compat_dry_run_message() const override;
    std::string compat_resume_continue_message() const override;
    int handle_empty_args() override;
    void on_startup_context(const cli_startup_context &startup) override;
    int diagnose(const std::vector<std::string> &args);
    int runbook(const std::vector<std::string> &args);
    int status();
    int observe(const std::vector<std::string> &args);
    int selftest();
    int set_provider(const std::vector<std::string> &args);
    std::string startup_context_block() const;
    bool recover_state_for_persist(std::string *error);
    bool persist_response(const std::string &kind,
                          const agent_task &task,
                          const std::string &input,
                          const std::string &output,
                          std::string *stored_key);
    void print_help(bool interactive_mode) const;

    std::vector<std::string> _startup_context;
    bool _state_recovered_for_persist = false;
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
