#pragma once

// rASN model-gateway cost/token budget.
//
// The model rate limiter (rate_limiter.h) governs request COUNT: it paces how
// many completions per minute a provider will admit. That cannot bound token
// throughput or metered spend, because one large prompt may cost many times a
// small one. This header adds the missing cost dimension: a deterministic
// estimator that converts a request's prompt size into an estimated token
// charge, plus a mapping from operator-facing cost tunables to the generic
// token-bucket config (rate_limit_config) that meters those charges.
//
// The cost engine itself is just a rate_limiter fed a per-request cost instead of
// 1, so it inherits the same rDSN-aligned properties: an injected millisecond
// clock (dsn_now_ms via the pluggable environment provider), deterministic refill
// under replay, and perf-counter/observability export. Keeping the estimator here
// -- dependency-light, pulling in no rDSN headers -- makes it unit-testable
// without a live service node, matching rate_limiter.h, circuit_breaker.h, and
// admission_gate.h.
//
// The estimate is intentionally a function of the prompt size only (not of the
// provider response), so charging a request is deterministic and replay-safe and
// never depends on provider-side nondeterminism.

#include <cstddef>
#include <cstdint>

#include "rate_limiter.h"

namespace dsn {
namespace rasn {

// Default prompt characters per estimated token when chars_per_token is 0.
// Roughly one token per four characters of English text, the common heuristic
// for OpenAI-style byte-pair tokenizers.
constexpr uint32_t k_default_chars_per_token = 4;

// Default completion allowance (percent of estimated input tokens) when
// completion_percent is 0. 150 charges +50% over the input estimate to
// approximate the completion the model will generate in addition to the prompt.
constexpr uint32_t k_default_completion_percent = 150;

// Operator-facing tunables for the model-gateway cost/token budget.
struct model_cost_config
{
    bool enabled = true;
    // Sustained token budget per provider in estimated tokens/minute. 0 disables
    // the budget (every request passes through). This is the bucket refill rate.
    uint32_t tokens_per_min = 0;
    // Bucket capacity in estimated tokens: the maximum burst above the sustained
    // budget. 0 selects a default of one second of the sustained budget (at least
    // one token). Size this >= the largest single-request estimate you intend to
    // admit, otherwise one oversized prompt can never fit within max_wait_ms.
    uint32_t burst_tokens = 0;
    // Upper bound on how long a request may be paced waiting for budget before it
    // is rejected. 0 means reject immediately when the budget is exhausted.
    uint32_t max_wait_ms = 1000;
    // Prompt characters per estimated token (0 => k_default_chars_per_token).
    uint32_t chars_per_token = 0;
    // Completion allowance as a percent of estimated input tokens added to the
    // charge (0 => k_default_completion_percent; 100 => charge input only).
    uint32_t completion_percent = 0;
};

// Estimate the token charge for a request whose prompt is prompt_chars characters
// (system + user + joined context). Returns
//   max(1, ceil(prompt_chars / chars_per_token) * completion_percent / 100).
// The floor of 1 ensures every request costs at least one token so an empty
// prompt still draws from the budget. Deterministic in its inputs.
double estimate_prompt_cost_tokens(size_t prompt_chars, const model_cost_config &config);

// Project the cost tunables onto the generic token-bucket config so a
// rate_limiter / rate_limiter_registry can meter estimated token charges. The
// bucket's "requests_per_min" therefore holds tokens/minute and "burst" holds
// burst tokens; callers pass the estimated cost to try_acquire()/refund().
rate_limit_config to_rate_limit_config(const model_cost_config &config);

} // namespace rasn
} // namespace dsn
