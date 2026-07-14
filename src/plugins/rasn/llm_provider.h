#pragma once

#include <rasn/model_agent.h>
#include <rasn/rasn_core.h>

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

// Outcome of interpreting a raw provider HTTP response body.
struct chat_completion_parse
{
    // True when a usable completion was extracted, or a non-JSON body was passed
    // through best-effort. False when the body carried no usable completion, e.g.
    // a JSON error envelope or a well-formed but empty message.
    bool ok = false;
    std::string text;         // Completion text; valid when ok.
    std::string error_detail; // Provider-supplied diagnostic; set when !ok.
};

typedef std::function<void(const std::string &chunk)> llm_stream_callback;

// Resolves a request override against [rasn.model] request_timeout_sec using
// the same rules as the network provider.
uint64_t effective_llm_request_timeout_ms(uint32_t request_timeout_ms);

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

// Interpret a raw provider response body (OpenAI/Ollama/llama.cpp-compatible)
// into a completion or a failure. Prefers the standard content/response field,
// then falls back to "reasoning_content" for reasoning models that leave
// "content" empty. A JSON body with no usable completion (an error envelope or
// an empty message) is reported as !ok instead of being surfaced as the model's
// answer, which would otherwise mask provider failures and corrupt downstream
// state. Non-JSON bodies are passed through unchanged for unknown/plain-text
// providers. Exposed for unit testing of the extraction logic.
chat_completion_parse parse_chat_completion(const std::string &output, const std::string &payload_format);

} // namespace rasn
} // namespace dsn
