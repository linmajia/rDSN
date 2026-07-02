#pragma once

#include "model_agent.h"
#include "rasn_core.h"

#include <memory>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

struct llm_request
{
    std::string task_id;
    std::string system_prompt;
    std::string user_prompt;
    std::vector<std::string> context;
    uint32_t timeout_ms = 0;
    uint32_t retry_budget = 0;
    std::vector<std::string> policy_labels;
};

struct llm_response
{
    bool ok;
    std::string text;
    std::string error;
};

typedef std::function<void(const std::string &chunk)> llm_stream_callback;

class llm_provider
{
public:
    virtual ~llm_provider() {}
    virtual std::string name() const = 0;
    virtual std::string model() const = 0;
    virtual model_provider_descriptor describe() const = 0;
    // True when the provider runs entirely in this process and performs no
    // network I/O (e.g. the deterministic simulator, the workflow service-graph
    // bridge, or test fakes). Such providers cannot hang on a remote endpoint, so
    // the model-gateway circuit breaker exempts them. Network-backed providers
    // (anything issuing HTTP, including loopback Ollama/llama.cpp/LM Studio
    // endpoints) override this to return false so they are breaker-guarded. This
    // is deliberately distinct from descriptor.local, which only conveys whether
    // the endpoint is loopback for display/health, not whether it does I/O.
    virtual bool in_process() const { return true; }
    virtual llm_response complete(const llm_request &request, nucleus_runtime &runtime) = 0;
    virtual llm_response
    complete_streaming(const llm_request &request, nucleus_runtime &runtime, const llm_stream_callback &on_chunk);
};

void emit_llm_stream_chunks(const agent_task &task,
                            const std::string &provider,
                            const std::string &text,
                            nucleus_runtime &runtime,
                            const llm_stream_callback &on_chunk,
                            size_t chunk_bytes = 0);

std::unique_ptr<llm_provider> create_provider_from_environment();
std::unique_ptr<llm_provider> create_provider(const std::string &provider_name);
std::unique_ptr<llm_provider> create_provider(const std::string &provider_name, const std::string &model_name);
std::string describe_provider_environment();

} // namespace rasn
} // namespace dsn
