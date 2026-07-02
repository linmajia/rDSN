// rASN admission control implementation.
//
// The engine state is dependency-light, but the graceful-backpressure curve is
// computed by reusing rDSN's dsn::exp_delay utility -- the same primitive rDSN
// uses to throttle traffic into its task thread pools ("delay for admission
// control"). We scale rDSN's default delay shape so its peak equals the
// configured max_backpressure_ms, keeping the curve rDSN-native while making its
// magnitude tunable.

#include <rasn/admission_gate.h>

#include <dsn/utility/exp_delay.h>

#include <algorithm>
#include <climits>
#include <vector>

namespace dsn {
namespace rasn {

namespace {

// Map an in-flight level to a backpressure delay using rDSN's exp_delay curve.
// exp_delay returns 0 below the threshold and ramps up to its peak as the value
// reaches ~2x the threshold. We seed it with rDSN's default shape scaled so the
// peak equals max_backpressure_ms.
uint32_t backpressure_delay_ms(uint32_t in_flight, const admission_config &cfg)
{
    if (cfg.soft_concurrency == 0 || cfg.max_backpressure_ms == 0 || in_flight < cfg.soft_concurrency)
    {
        return 0;
    }

    std::vector<int> delays(DELAY_COUNT, 0);
    // exp_delay operates entirely in signed int. Clamp the configured peak to a
    // bound where scaling it by the s_default_delay shape (max factor 10) cannot
    // overflow int, so an out-of-range config value that was clamped to uint32_t's
    // maximum degrades to a large-but-valid delay instead of narrowing to a
    // negative/garbage value and corrupting the curve.
    static const uint32_t k_max_peak_ms = static_cast<uint32_t>(INT_MAX) / 10;
    const int peak = static_cast<int>((std::min)(cfg.max_backpressure_ms, k_max_peak_ms));
    for (int i = 0; i < DELAY_COUNT; ++i)
    {
        // s_default_delay peaks at 10ms; rescale so the last point equals the
        // configured maximum backpressure.
        delays[i] = ::dsn::s_default_delay[i] * peak / 10;
    }

    ::dsn::exp_delay curve;
    curve.initialize(delays, static_cast<int>(cfg.soft_concurrency));
    const int ms = curve.delay(static_cast<int>(in_flight));
    if (ms <= 0)
    {
        return 0;
    }
    return static_cast<uint32_t>((std::min)(ms, peak));
}

} // namespace

admission_slot::admission_slot(admission_gate *gate, const admission_decision &decision)
    : _gate(gate), _decision(decision)
{
}

admission_slot::admission_slot(admission_slot &&other) noexcept
    : _gate(other._gate), _decision(other._decision)
{
    other._gate = nullptr;
}

admission_slot &admission_slot::operator=(admission_slot &&other) noexcept
{
    if (this != &other)
    {
        release();
        _gate = other._gate;
        _decision = other._decision;
        other._gate = nullptr;
    }
    return *this;
}

admission_slot::~admission_slot() { release(); }

void admission_slot::release()
{
    if (_gate != nullptr)
    {
        _gate->release();
        _gate = nullptr;
    }
}

admission_gate::admission_gate(const admission_config &config) : _config(config)
{
    // Backpressure must begin no later than the hard cap, otherwise it could
    // never engage. Clamp soft to the cap for well-defined behavior.
    if (_config.max_concurrency != 0 && _config.soft_concurrency > _config.max_concurrency)
    {
        _config.soft_concurrency = _config.max_concurrency;
    }
}

admission_slot admission_gate::try_admit()
{
    std::lock_guard<std::mutex> guard(_lock);

    admission_decision decision;
    decision.limit = _config.max_concurrency;

    if (!_config.enabled)
    {
        // Passthrough: admit without reserving capacity or applying backpressure.
        decision.admitted = true;
        decision.in_flight = _in_flight;
        return admission_slot(nullptr, decision);
    }

    if (_config.max_concurrency != 0 && _in_flight >= _config.max_concurrency)
    {
        // Hard cap reached: reject without reserving capacity.
        decision.admitted = false;
        decision.in_flight = _in_flight;
        return admission_slot(nullptr, decision);
    }

    ++_in_flight;
    decision.admitted = true;
    decision.in_flight = _in_flight;
    decision.delay_ms = backpressure_delay_ms(_in_flight, _config);
    return admission_slot(this, decision);
}

void admission_gate::release()
{
    std::lock_guard<std::mutex> guard(_lock);
    if (_in_flight > 0)
    {
        --_in_flight;
    }
}

uint32_t admission_gate::in_flight() const
{
    std::lock_guard<std::mutex> guard(_lock);
    return _in_flight;
}

admission_gate_registry::admission_gate_registry(const admission_config &config) : _config(config) {}

admission_gate &admission_gate_registry::get(const std::string &key)
{
    std::lock_guard<std::mutex> guard(_lock);
    std::unique_ptr<admission_gate> &slot = _gates[key];
    if (slot == nullptr)
    {
        slot.reset(new admission_gate(_config));
    }
    return *slot;
}

std::vector<admission_gate_registry::entry> admission_gate_registry::snapshot() const
{
    std::lock_guard<std::mutex> guard(_lock);
    std::vector<entry> result;
    result.reserve(_gates.size());
    for (const auto &kv : _gates)
    {
        entry e;
        e.key = kv.first;
        e.in_flight = kv.second->in_flight();
        e.max_concurrency = kv.second->config().max_concurrency;
        e.soft_concurrency = kv.second->config().soft_concurrency;
        result.push_back(e);
    }
    return result;
}

void admission_gate_registry::set_config(const admission_config &config)
{
    std::lock_guard<std::mutex> guard(_lock);
    _config = config;
}

} // namespace rasn
} // namespace dsn
