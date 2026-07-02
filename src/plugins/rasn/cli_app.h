#pragma once

#include "agent_executor.h"
#include "agent_services.h"
#include "cli_support.h"

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

class rasn_service_graph_lifecycle_scope
{
public:
    explicit rasn_service_graph_lifecycle_scope(rasn_service_graph &services);
    ~rasn_service_graph_lifecycle_scope();

    rasn_service_graph_lifecycle_scope(const rasn_service_graph_lifecycle_scope &) = delete;
    rasn_service_graph_lifecycle_scope &operator=(const rasn_service_graph_lifecycle_scope &) = delete;

private:
    rasn_service_graph &_services;
};

struct rasn_cli_service_readiness_options
{
    std::string state_probe_key = "__rasn_readiness_probe__";
    std::string workflow_id = "readiness";
    std::string workflow_source_name = "<readiness>";
    std::string dependency_error = "rASN service dependencies are not ready";
};

struct rasn_cli_compat_options
{
    bool help = false;
    bool version = false;
    bool print = false;
    bool prompt_set = false;
    bool stream = false;
    bool no_interactive = false;
    bool yes = false;
    bool dry_run = false;
    bool continue_latest = false;
    bool resume_set = false;
    bool provider_set = false;
    bool model_set = false;
    bool workspace_set = false;
    bool approval_set = false;
    bool sandbox_set = false;
    std::string prompt;
    std::string resume_id;
    std::string provider;
    std::string model;
    std::string workspace;
    std::string approval;
    std::string sandbox;
};

struct rasn_cli_agent_plan
{
    agent_task task;
    std::string prompt;
    std::string system_prompt;
    std::vector<std::string> context;
    agent_executor_options executor_options;
    std::string approval_failure_source = "rasn.cli";
    std::string approval_failure_category = "policy";
    std::string approval_failure_code = "tool_approval_denied";
};

bool wait_for_cli_service_dependencies(const rasn_service_graph &services,
                                       const rasn_cli_service_readiness_options &options,
                                       std::string *error);

std::vector<std::string> cli_args_from_argv(int argc, char **argv, int begin = 0);
void run_dsn_with_cli_args(const std::vector<std::string> &args, bool sleep_after_init);

class rasn_cli_app_base
{
public:
    explicit rasn_cli_app_base(rasn_service_graph &services);
    virtual ~rasn_cli_app_base();

    int run(const std::vector<std::string> &args);
    int repl();
    void request_shutdown() { _shutdown_requested.store(true); }

protected:
    virtual std::vector<std::string> commands() const = 0;
    virtual std::string repl_title() const = 0;
    virtual std::string repl_prompt() const = 0;
    virtual std::string repl_plain_text_behavior() const = 0;
    virtual int run_command(const std::vector<std::string> &args, bool interactive_mode = false) = 0;
    virtual void handle_plain_text(const std::string &line) = 0;
    virtual int run_compat_prompt(const std::string &prompt, bool stream) = 0;
    virtual bool handle_compat_options(const rasn_cli_compat_options &options, int *exit_code);
    virtual void print_compat_help() const;
    virtual std::string version_string() const;
    virtual std::string compat_prompt_usage() const;
    virtual std::string compat_dry_run_message() const;
    virtual std::string compat_resume_continue_message() const;
    virtual bool handle_compat_resume(const rasn_cli_compat_options &options, int *exit_code);
    virtual bool supports_compat_safety_options() const;
    virtual void print_compat_provider(const model_gateway_response &response) const;
    virtual int handle_empty_args();
    virtual void on_startup_context(const cli_startup_context &startup);
    virtual size_t max_context_bytes() const;
    virtual cli_workspace_context_options workspace_context_options() const;
    virtual std::string provider_summary() const;
    int run_agent_plan(const rasn_cli_agent_plan &plan,
                       const agent_plan_executor::model_callback &model,
                       const agent_plan_executor::approval_callback &approve,
                       const agent_plan_executor::tool_callback &tool);

    rasn_service_graph &_services;

private:
    std::atomic<bool> _shutdown_requested{false};
};

} // namespace rasn
} // namespace dsn
