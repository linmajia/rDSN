#pragma once

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

bool wait_for_cli_service_dependencies(const rasn_service_graph &services,
                                       const rasn_cli_service_readiness_options &options,
                                       std::string *error);

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
    virtual const char *repl_title() const = 0;
    virtual const char *repl_prompt() const = 0;
    virtual const char *repl_plain_text_behavior() const = 0;
    virtual int run_command(const std::vector<std::string> &args, bool interactive_mode = false) = 0;
    virtual void handle_plain_text(const std::string &line) = 0;
    virtual int handle_empty_args();
    virtual void on_startup_context(const cli_startup_context &startup);
    virtual size_t max_context_bytes() const;
    virtual cli_workspace_context_options workspace_context_options() const;
    virtual std::string provider_summary() const;

    rasn_service_graph &_services;

private:
    std::atomic<bool> _shutdown_requested{false};
};

} // namespace rasn
} // namespace dsn
