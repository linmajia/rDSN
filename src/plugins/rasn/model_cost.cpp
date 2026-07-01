// rASN model-gateway cost/token budget implementation.
//
// See model_cost.h. The estimator is a pure function of the prompt size; the
// mapping simply reinterprets the token tunables as a token-denominated
// rate_limit_config so the existing token bucket can meter estimated token
// charges instead of request count.

#include "model_cost.h"

#include <algorithm>
#include <cmath>

namespace dsn {
namespace rasn {

double estimate_prompt_cost_tokens(size_t prompt_chars, const model_cost_config &config)
{
    const uint32_t chars_per_token =
        config.chars_per_token == 0 ? k_default_chars_per_token : config.chars_per_token;
    const uint32_t completion_percent =
        config.completion_percent == 0 ? k_default_completion_percent : config.completion_percent;

    // Input tokens: ceil(prompt_chars / chars_per_token). chars_per_token is at
    // least 1 here (0 mapped to the default above), so the division is safe.
    const double input_tokens =
        std::ceil(static_cast<double>(prompt_chars) / static_cast<double>(chars_per_token));
    // Add the completion allowance and floor at a single token so every request
    // draws from the budget.
    const double charge = input_tokens * static_cast<double>(completion_percent) / 100.0;
    return charge < 1.0 ? 1.0 : charge;
}

rate_limit_config to_rate_limit_config(const model_cost_config &config)
{
    rate_limit_config cfg;
    cfg.enabled = config.enabled;
    // The token bucket is unit-agnostic: it meters whatever weight try_acquire()
    // is handed. Here that weight is estimated tokens, so requests_per_min holds
    // the tokens/minute budget and burst holds burst tokens.
    cfg.requests_per_min = config.tokens_per_min;
    cfg.burst = config.burst_tokens;
    cfg.max_wait_ms = config.max_wait_ms;
    return cfg;
}

} // namespace rasn
} // namespace dsn
