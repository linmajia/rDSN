#pragma once

#include <rasn/agent_control_plane.h>
#include <rasn/agent_executor.h>
#include <rasn/agent_message_bus.h>
#include <rasn/agent_services.h>
#include <rasn/blackboard.h>
#include <rasn/capability_directory.h>
#include <rasn/cli_support.h>
#include <rasn/runtime_provider.h>
#include <rasn/contract_verifier.h>
#include <rasn/determinism_ledger.h>
#include <rasn/human_interaction.h>
#include <rasn/recovery_supervisor.h>
#include <rasn/resource_budget.h>
#include <rasn/sandbox_runtime.h>
#include <rasn/task_orchestration.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

int run_rasn_state_command(rasn_service_graph &services,
                           const std::vector<std::string> &args);

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
void install_rasn_cli_out_of_memory_handler();
void run_dsn_with_cli_args(const std::vector<std::string> &args, bool sleep_after_init);
// Resolve a binplaced config beside the actual executable (or its parent for
// multi-config layouts such as bin/<app>/Debug). This resolves argv[0] through
// PATH when needed and never implicitly trusts an unrelated process CWD.
// Returns empty when the executable or requested config cannot be resolved.
std::string find_rasn_cli_config_file(const std::string &program, const std::string &filename);
// Attach the current one-shot CLI thread to the auto-started no-op [apps.mimic]
// node so it has a lightweight rDSN service-node context. Remote/hybrid runtime
// module RPC requires a node context; a bare CLI thread has none. Call this after
// run_dsn_with_cli_args(..., false) started the mimic app (i.e. only when the
// app's [rasn.runtime] placement is distributed/hybrid). Local placement needs no
// node (modules run in-process). Returns false with a warning if attach fails.
bool attach_cli_runtime_client_node();
// Make a runtime host config's `@include config.rasn.defaults.ini` resolve beside
// the selected config rather than against an unrelated launch directory. rDSN
// opens @include paths relative to the process working directory, so when
// config.rasn.ini is auto-detected next to the binary this switches into its
// directory and returns the absolute config path to hand to run_dsn_with_cli_args.
// Intended for the `serve` (runtime host) path; a no-op-equivalent when already
// launched from that directory. Returns the input unchanged if it cannot resolve.
std::string align_working_directory_to_runtime_config(const std::string &config_path);

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
    virtual void on_compat_prompt_start(const rasn_cli_compat_options &options);
    virtual void on_compat_prompt_finish(const rasn_cli_compat_options &options);
    virtual int handle_empty_args();
    virtual void on_cli_workspace_changed(const std::string &workspace);
    virtual void on_startup_context(const cli_startup_context &startup);
    virtual size_t max_context_bytes() const;
    virtual cli_workspace_context_options workspace_context_options() const;
    virtual std::string provider_summary() const;
    virtual std::string cli_agent_id() const;
    virtual std::string cli_agent_role() const;
    virtual std::string cli_agent_app_name() const;
    virtual std::vector<agent_capability> cli_agent_capabilities() const;
    int run_agent_plan(const rasn_cli_agent_plan &plan,
                       const agent_plan_executor::model_callback &model,
                       const agent_plan_executor::approval_callback &approve,
                       const agent_plan_executor::tool_callback &tool);
    std::string runtime_modules_summary() const;
    std::string runtime_modules_topology() const;
    bool runtime_modules_ready(std::string *detail = nullptr) const;
    deterministic_replay_result record_runtime_choice(const std::string &task_id,
                                                      const std::string &key,
                                                      const std::string &source,
                                                      const std::string &value);
    sandbox_decision evaluate_cli_sandbox_request(const sandbox_request &request) const;
    void set_cli_sandbox_profile(const sandbox_profile &profile);

    rasn_service_graph &_services;

private:
    struct runtime_execution
    {
        bool active = false;
        bool budget_reserved = false;
        std::string task_id;
        std::string message_id;
        resource_request budget;
    };

    void initialize_runtime_modules();
    void heartbeat_runtime_modules();
    void configure_runtime_module_mode() const;
    int run_tracked_command(const std::vector<std::string> &args, bool interactive_mode);
    int run_tracked_compat_prompt(const rasn_cli_compat_options &options);
    runtime_execution begin_runtime_execution(const std::string &kind,
                                              const std::string &name,
                                              const std::string &input,
                                              const std::string &receiver,
                                              const std::string &message_type);
    void finish_runtime_execution(const runtime_execution &execution, int exit_code, const std::string &detail);
    void warn_runtime_module_failure(const std::string &module, const std::string &error) const;

    std::atomic<bool> _shutdown_requested{false};
    mutable std::unique_ptr<rasn_runtime> _rasn_runtime;
    bool _runtime_modules_initialized = false;
};

} // namespace rasn
} // namespace dsn
