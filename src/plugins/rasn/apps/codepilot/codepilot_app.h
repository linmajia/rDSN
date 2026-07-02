#pragma once

#include "../../cli_app.h"
#include "../../rasn.code.definition.h"
#include "../../workflow.h"
#include "skills.h"

#include <dsn/service_api_cpp.h>
#include <dsn/cpp/task_helper.h>

#include <memory>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

class codepilot_cli : public rasn_cli_app_base
{
public:
    codepilot_cli();

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
    std::string compat_dry_run_message() const override;
    bool handle_compat_resume(const rasn_cli_compat_options &options, int *exit_code) override;
    bool supports_compat_safety_options() const override;
    void print_compat_provider(const model_gateway_response &response) const override;
    void on_startup_context(const cli_startup_context &startup) override;
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

    std::vector<std::string> _context;
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
