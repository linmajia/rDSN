#include <rasn/coordination_breaker.h>

#include <dsn/c/api_layer1.h>
#include <dsn/service_api_cpp.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

namespace dsn {
namespace rasn {

namespace {

const char *kBreakerRecordMagic = "rasn-breaker-v1";
const size_t kRetainedBreakerVersions = 8;

struct stored_breaker_state
{
    uint64_t revision = 0;
    uint64_t fencing_token = 0;
    uint64_t generation = 0;
    breaker_state state = breaker_state::closed;
    uint32_t consecutive_failures = 0;
    uint64_t opened_at_ms = 0;
    uint64_t probe_token = 0;
    uint64_t probe_deadline_ms = 0;
    uint32_t failure_threshold = 1;
    uint64_t open_ms = 0;
    uint64_t probe_lease_ms = 0;
    uint64_t max_probe_lease_ms = 0;
    uint64_t clock_skew_ms = 0;
};

uint64_t saturating_add(uint64_t lhs, uint64_t rhs)
{
    const uint64_t max = (std::numeric_limits<uint64_t>::max)();
    return lhs > max - rhs ? max : lhs + rhs;
}

std::string hex_encode(const std::string &value)
{
    static const char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(value.size() * 2);
    for (unsigned char ch : value)
    {
        encoded.push_back(digits[ch >> 4]);
        encoded.push_back(digits[ch & 0x0f]);
    }
    return encoded;
}

uint64_t fnv1a64(const std::string &value)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned char ch : value)
    {
        hash ^= ch;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

std::string flat_lock_id(const std::string &prefix,
                         const std::string &scope,
                         const std::string &key)
{
    std::ostringstream material;
    material << prefix << '\0' << scope << '\0' << key;
    std::ostringstream output;
    output << "rasn-breaker-v1-" << std::hex << std::setw(16)
           << std::setfill('0') << fnv1a64(material.str());
    return output.str();
}

int hex_digit(char value)
{
    if (value >= '0' && value <= '9')
    {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f')
    {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F')
    {
        return value - 'A' + 10;
    }
    return -1;
}

bool hex_decode(const std::string &encoded, std::string *value)
{
    if (value == nullptr || encoded.size() % 2 != 0)
    {
        return false;
    }
    std::string decoded;
    decoded.reserve(encoded.size() / 2);
    for (size_t i = 0; i < encoded.size(); i += 2)
    {
        const int high = hex_digit(encoded[i]);
        const int low = hex_digit(encoded[i + 1]);
        if (high < 0 || low < 0)
        {
            return false;
        }
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    *value = decoded;
    return true;
}

std::vector<std::string> split_fields(const std::string &value)
{
    std::vector<std::string> fields;
    std::string::size_type start = 0;
    while (start <= value.size())
    {
        const std::string::size_type separator = value.find('|', start);
        if (separator == std::string::npos)
        {
            fields.push_back(value.substr(start));
            break;
        }
        fields.push_back(value.substr(start, separator - start));
        start = separator + 1;
    }
    return fields;
}

bool parse_uint64(const std::string &value, uint64_t *parsed)
{
    if (parsed == nullptr || value.empty() || value[0] == '-')
    {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long long converted = std::strtoull(value.c_str(), &end, 10);
    if (errno == ERANGE || end == value.c_str() || end == nullptr || *end != '\0')
    {
        return false;
    }
    *parsed = static_cast<uint64_t>(converted);
    return true;
}

std::string serialize_state(const stored_breaker_state &state)
{
    std::ostringstream output;
    output << kBreakerRecordMagic << "|" << state.revision << "|"
           << state.fencing_token << "|" << state.generation << "|"
           << static_cast<unsigned int>(state.state) << "|"
           << state.consecutive_failures << "|" << state.opened_at_ms << "|"
           << state.probe_token << "|" << state.probe_deadline_ms << "|"
           << state.failure_threshold << "|" << state.open_ms << "|"
           << state.probe_lease_ms << "|" << state.max_probe_lease_ms << "|"
           << state.clock_skew_ms;
    return output.str();
}

bool parse_state(const std::string &value, stored_breaker_state *state, std::string *error)
{
    if (state == nullptr)
    {
        if (error != nullptr)
        {
            *error = "breaker state output is null";
        }
        return false;
    }
    const std::vector<std::string> fields = split_fields(value);
    if (fields.size() != 14 || fields[0] != kBreakerRecordMagic)
    {
        if (error != nullptr)
        {
            *error = "unsupported or malformed breaker state record";
        }
        return false;
    }

    uint64_t numeric[13] = {};
    for (size_t i = 1; i < fields.size(); ++i)
    {
        if (!parse_uint64(fields[i], &numeric[i - 1]))
        {
            if (error != nullptr)
            {
                *error = "breaker state contains a non-numeric field";
            }
            return false;
        }
    }
    if (numeric[3] > static_cast<uint64_t>(breaker_state::half_open) ||
        numeric[4] > (std::numeric_limits<uint32_t>::max)() ||
        numeric[8] == 0 || numeric[8] > (std::numeric_limits<uint32_t>::max)())
    {
        if (error != nullptr)
        {
            *error = "breaker state contains an out-of-range field";
        }
        return false;
    }

    state->revision = numeric[0];
    state->fencing_token = numeric[1];
    state->generation = numeric[2];
    state->state = static_cast<breaker_state>(numeric[3]);
    state->consecutive_failures = static_cast<uint32_t>(numeric[4]);
    state->opened_at_ms = numeric[5];
    state->probe_token = numeric[6];
    state->probe_deadline_ms = numeric[7];
    state->failure_threshold = static_cast<uint32_t>(numeric[8]);
    state->open_ms = numeric[9];
    state->probe_lease_ms = numeric[10];
    state->max_probe_lease_ms = numeric[11];
    state->clock_skew_ms = numeric[12];

    if ((state->state == breaker_state::half_open) != (state->probe_token != 0) ||
        (state->state == breaker_state::half_open) != (state->probe_deadline_ms != 0))
    {
        if (error != nullptr)
        {
            *error = "breaker state has inconsistent half-open probe fields";
        }
        return false;
    }
    return true;
}

stored_breaker_state initial_state(const breaker_config &breaker,
                                   const rasn_shared_breaker_config &shared)
{
    const breaker_config normalized = normalize_breaker_config(breaker);
    stored_breaker_state state;
    state.failure_threshold = normalized.failure_threshold;
    state.open_ms = normalized.open_ms;
    state.probe_lease_ms = shared.probe_lease_ms;
    state.max_probe_lease_ms = shared.max_probe_lease_ms;
    state.clock_skew_ms = shared.clock_skew_ms;
    return state;
}

bool config_matches(const stored_breaker_state &state,
                    const breaker_config &breaker,
                    const rasn_shared_breaker_config &shared,
                    std::string *error)
{
    const breaker_config normalized = normalize_breaker_config(breaker);
    if (state.failure_threshold == normalized.failure_threshold &&
        state.open_ms == normalized.open_ms &&
        state.probe_lease_ms == shared.probe_lease_ms &&
        state.max_probe_lease_ms == shared.max_probe_lease_ms &&
        state.clock_skew_ms == shared.clock_skew_ms)
    {
        return true;
    }
    if (error != nullptr)
    {
        std::ostringstream message;
        message << "shared breaker configuration mismatch: stored(threshold="
                << state.failure_threshold << ",open_ms=" << state.open_ms
                << ",probe_lease_ms=" << state.probe_lease_ms
                << ",max_probe_lease_ms=" << state.max_probe_lease_ms
                << ",clock_skew_ms=" << state.clock_skew_ms << ") local(threshold="
                << normalized.failure_threshold << ",open_ms=" << normalized.open_ms
                << ",probe_lease_ms=" << shared.probe_lease_ms
                << ",max_probe_lease_ms=" << shared.max_probe_lease_ms
                << ",clock_skew_ms=" << shared.clock_skew_ms << ")";
        *error = message.str();
    }
    return false;
}

bool next_revision(stored_breaker_state *state, std::string *error)
{
    if (state->revision == (std::numeric_limits<uint64_t>::max)())
    {
        if (error != nullptr)
        {
            *error = "shared breaker revision exhausted";
        }
        return false;
    }
    ++state->revision;
    return true;
}

bool cooldown_elapsed(const stored_breaker_state &state, uint64_t now_ms)
{
    const uint64_t cooldown = saturating_add(state.open_ms, state.clock_skew_ms);
    return now_ms >= state.opened_at_ms && (now_ms - state.opened_at_ms) >= cooldown;
}

uint64_t make_probe_token()
{
    static std::atomic<uint64_t> sequence(0);
    uint64_t token = ::dsn_random64(1, (std::numeric_limits<uint64_t>::max)());
    token ^= sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    return token == 0 ? 1 : token;
}

std::string make_owner_id(const std::string &scope)
{
    static std::atomic<uint64_t> sequence(0);
    std::ostringstream output;
    output << "rasn.breaker." << hex_encode(scope) << "." << std::hex
           << ::dsn_random64(0, (std::numeric_limits<uint64_t>::max)()) << "."
           << sequence.fetch_add(1, std::memory_order_relaxed);
    return output.str();
}

breaker_decision unavailable_decision(const std::string &error)
{
    breaker_decision decision;
    decision.allowed = false;
    decision.state = breaker_state::open;
    decision.available = false;
    decision.error = error;
    return decision;
}

breaker_status unavailable_status(const std::string &error)
{
    breaker_status status;
    status.open = true;
    status.state = breaker_state::open;
    status.available = false;
    status.error = error;
    return status;
}

breaker_report unavailable_report(const std::string &error)
{
    breaker_report report;
    report.applied = false;
    report.state = breaker_state::open;
    report.available = false;
    report.error = error;
    return report;
}

class coordinated_breaker_backend : public circuit_breaker_registry_backend
{
public:
    coordinated_breaker_backend(
        std::string scope,
        rasn_shared_breaker_config config,
        std::shared_ptr<rasn_coordination_context> coordination)
        : _scope(std::move(scope)),
          _encoded_scope(hex_encode(_scope)),
          _config(std::move(config)),
          _coordination(std::move(coordination)),
          _owner_id(make_owner_id(_scope))
    {
        if (_scope.empty())
        {
            _configuration_error = "shared breaker scope must not be empty";
        }
        else if (_config.state_prefix.empty() || _config.state_prefix.front() == '/' ||
            _config.state_prefix.back() == '/' ||
            _config.state_prefix.find("//") != std::string::npos)
        {
            _configuration_error =
                "shared_breaker_state_prefix must be a non-empty relative state path";
        }
        else if (_config.max_probe_lease_ms == 0 ||
                 _config.max_probe_lease_ms < _config.probe_lease_ms)
        {
            _configuration_error =
                "shared_breaker_max_probe_lease_ms must be at least "
                "shared_breaker_probe_lease_ms";
        }
    }

    breaker_decision allow(const std::string &key,
                           const breaker_config &config,
                           uint64_t now_ms,
                           uint64_t probe_lease_hint_ms) override
    {
        if (!config.enabled)
        {
            return breaker_decision();
        }
        if (key.empty())
        {
            return unavailable_decision("shared breaker key must not be empty");
        }
        rasn_coordination_service *service = nullptr;
        std::string owner_id;
        std::string error;
        if (!service_started(&service, &owner_id, &error))
        {
            return unavailable_decision(error);
        }

        stored_breaker_state state;
        bool found = false;
        if (!read_state(service, key, &state, &found, &error))
        {
            return unavailable_decision(error);
        }
        if (!found)
        {
            return breaker_decision();
        }
        if (!config_matches(state, config, _config, &error))
        {
            return unavailable_decision(error);
        }
        if (state.state == breaker_state::closed)
        {
            breaker_decision decision;
            decision.generation = state.generation;
            return decision;
        }
        if (state.state == breaker_state::open && !cooldown_elapsed(state, now_ms))
        {
            breaker_decision decision;
            decision.allowed = false;
            decision.state = breaker_state::open;
            decision.generation = state.generation;
            return decision;
        }
        if (state.state == breaker_state::half_open &&
            (now_ms < state.probe_deadline_ms || now_ms < state.opened_at_ms))
        {
            breaker_decision decision;
            decision.allowed = false;
            decision.state = breaker_state::half_open;
            decision.generation = state.generation;
            return decision;
        }
        return claim_probe(
            service, owner_id, key, config, now_ms, probe_lease_hint_ms);
    }

    breaker_status
    inspect(const std::string &key, const breaker_config &config, uint64_t now_ms) override
    {
        if (!config.enabled)
        {
            return breaker_status();
        }
        if (key.empty())
        {
            return unavailable_status("shared breaker key must not be empty");
        }
        rasn_coordination_service *service = nullptr;
        std::string error;
        if (!service_started(&service, nullptr, &error))
        {
            return unavailable_status(error);
        }
        stored_breaker_state state;
        bool found = false;
        if (!read_state(service, key, &state, &found, &error))
        {
            return unavailable_status(error);
        }
        if (!found)
        {
            return breaker_status();
        }
        if (!config_matches(state, config, _config, &error))
        {
            return unavailable_status(error);
        }
        breaker_status status;
        status.state = state.state;
        status.consecutive_failures = state.consecutive_failures;
        status.open = state.state == breaker_state::open && !cooldown_elapsed(state, now_ms);
        return status;
    }

    breaker_report report(const std::string &key,
                          const breaker_config &config,
                          const breaker_decision &admission,
                          bool ok,
                          uint64_t now_ms) override
    {
        if (!config.enabled || !admission.allowed)
        {
            breaker_report result;
            result.applied = false;
            return result;
        }
        if (key.empty())
        {
            return unavailable_report("shared breaker key must not be empty");
        }
        rasn_coordination_service *service = nullptr;
        std::string owner_id;
        std::string error;
        if (!service_started(&service, &owner_id, &error))
        {
            return unavailable_report(error);
        }

        const std::shared_ptr<std::mutex> local = key_lock(key);
        std::lock_guard<std::mutex> local_guard(*local);
        const std::string resource = lock_key(key);
        uint64_t fencing_token = 0;
        ::dsn::error_code err = service->acquire_ownership(
            resource, owner_id, _config.lock_timeout_ms, &fencing_token);
        if (err != ::dsn::ERR_OK)
        {
            return unavailable_report("failed to acquire shared breaker lock: " +
                                      std::string(err.to_string()));
        }

        breaker_report result;
        stored_breaker_state state;
        bool found = false;
        bool write = false;
        if (!read_state(service, key, &state, &found, &error))
        {
            result = unavailable_report(error);
        }
        else if (found && state.fencing_token > fencing_token)
        {
            result = unavailable_report("shared breaker mutation was rejected by a newer fence");
        }
        else if (!found && admission.half_open_probe)
        {
            result.applied = false;
            result.state = breaker_state::open;
        }
        else
        {
            if (!found)
            {
                state = initial_state(config, _config);
                write = true;
            }
            if (!config_matches(state, config, _config, &error))
            {
                result = unavailable_report(error);
            }
            else
            {
                apply_report(state, admission, ok, now_ms, &write, &result);
                if (write && result.available && !next_revision(&state, &error))
                {
                    result = unavailable_report(error);
                }
                else if (write && result.available)
                {
                    if (!persist_state(service, key, fencing_token, &state, &error))
                    {
                        result = unavailable_report(error);
                    }
                }
                if (result.available)
                {
                    result.state = state.state;
                    result.consecutive_failures = state.consecutive_failures;
                }
            }
        }

        // Reports only commit fenced state; unlike probe claims, they do not
        // authorize a side effect and therefore need no post-release barrier.
        err = service->release_ownership(resource, owner_id, false);
        if (err != ::dsn::ERR_OK)
        {
            const std::string release_error =
                "failed to release shared breaker lock: " +
                std::string(err.to_string());
            if (result.available)
            {
                dwarn("%s; the breaker report was already committed",
                      release_error.c_str());
                return result;
            }
            if (!result.error.empty())
            {
                result.error += "; ";
            }
            result.error += release_error;
        }
        return result;
    }

    std::vector<breaker_registry_entry> snapshot() const override
    {
        std::vector<breaker_registry_entry> result;
        rasn_coordination_service *service = nullptr;
        std::string error;
        if (!service_started(&service, nullptr, &error))
        {
            result.push_back(error_entry("*", error));
            return result;
        }

        std::vector<std::string> children;
        const ::dsn::error_code listed = service->list_state(scope_key(), children);
        if (listed == ::dsn::ERR_OBJECT_NOT_FOUND || listed == ::dsn::ERR_PATH_NOT_FOUND)
        {
            return result;
        }
        if (listed != ::dsn::ERR_OK)
        {
            result.push_back(error_entry(
                "*", "failed to list shared breaker state: " + std::string(listed.to_string())));
            return result;
        }
        for (const std::string &child : children)
        {
            std::string key;
            if (!hex_decode(child, &key))
            {
                result.push_back(error_entry(child, "shared breaker key is not valid hex"));
                continue;
            }
            stored_breaker_state state;
            bool found = false;
            if (!read_state(service, key, &state, &found, &error))
            {
                result.push_back(error_entry(key, error));
                continue;
            }
            if (!found)
            {
                continue;
            }
            breaker_registry_entry entry;
            entry.key = key;
            entry.state = state.state;
            entry.consecutive_failures = state.consecutive_failures;
            entry.shared = true;
            entry.revision = state.revision;
            result.push_back(entry);
        }
        std::sort(result.begin(), result.end(), [](const breaker_registry_entry &lhs,
                                                   const breaker_registry_entry &rhs) {
            return lhs.key < rhs.key;
        });
        return result;
    }

    const char *name() const override { return "coordination"; }

private:
    bool service_started(rasn_coordination_service **service,
                         std::string *owner_id,
                         std::string *error) const
    {
        if (!_configuration_error.empty())
        {
            if (error != nullptr)
            {
                *error = _configuration_error;
            }
            return false;
        }
        const std::shared_ptr<rasn_coordination_context> coordination =
            _coordination != nullptr ? _coordination : shared_rasn_coordination_context();
        if (service == nullptr || coordination == nullptr)
        {
            if (error != nullptr)
            {
                *error = "shared breaker coordination context is unavailable";
            }
            return false;
        }
        const ::dsn::error_code started = coordination->start();
        if (started != ::dsn::ERR_OK)
        {
            if (error != nullptr)
            {
                *error = "failed to start shared breaker coordination backend: " +
                         std::string(started.to_string());
            }
            return false;
        }
        *service = coordination->service();
        if (*service == nullptr)
        {
            if (error != nullptr)
            {
                *error = "shared breaker coordination service is unavailable";
            }
            return false;
        }
        if (owner_id != nullptr)
        {
            uint64_t operation = _operation_sequence.load();
            while (operation != (std::numeric_limits<uint64_t>::max)() &&
                   !_operation_sequence.compare_exchange_weak(operation,
                                                              operation + 1))
            {
            }
            if (operation == (std::numeric_limits<uint64_t>::max)())
            {
                if (error != nullptr)
                {
                    *error = "shared breaker owner sequence is exhausted";
                }
                return false;
            }
            std::ostringstream identity;
            identity << _owner_id << "." << std::hex
                     << reinterpret_cast<uintptr_t>(coordination.get()) << "."
                     << operation;
            *owner_id = identity.str();
        }
        return true;
    }

    std::string scope_key() const
    {
        return _config.state_prefix + "/v1/" + _encoded_scope;
    }

    std::string record_key(const std::string &key) const
    {
        return scope_key() + "/" + hex_encode(key);
    }

    std::string version_key(const std::string &key, uint64_t fencing_token) const
    {
        return record_key(key) + "/" + std::to_string(fencing_token);
    }

    std::string lock_key(const std::string &key) const
    {
        return flat_lock_id(_config.state_prefix, _scope, key);
    }

    std::shared_ptr<std::mutex> key_lock(const std::string &key)
    {
        std::lock_guard<std::mutex> guard(_key_locks_lock);
        std::shared_ptr<std::mutex> &slot = _key_locks[key];
        if (slot == nullptr)
        {
            slot.reset(new std::mutex());
        }
        return slot;
    }

    bool read_state(rasn_coordination_service *service,
                    const std::string &key,
                    stored_breaker_state *state,
                    bool *found,
                    std::string *error) const
    {
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            std::vector<std::string> versions;
            const ::dsn::error_code listed =
                service->list_state(record_key(key), versions);
            if (listed == ::dsn::ERR_OBJECT_NOT_FOUND ||
                listed == ::dsn::ERR_PATH_NOT_FOUND)
            {
                *found = false;
                return true;
            }
            if (listed != ::dsn::ERR_OK)
            {
                if (error != nullptr)
                {
                    *error = "failed to list shared breaker versions: " +
                             std::string(listed.to_string());
                }
                return false;
            }
            uint64_t latest = 0;
            bool have_version = false;
            for (const std::string &version : versions)
            {
                uint64_t parsed = 0;
                if (!parse_uint64(version, &parsed))
                {
                    if (error != nullptr)
                    {
                        *error =
                            "shared breaker version is not a valid fencing token";
                    }
                    return false;
                }
                if (!have_version || parsed > latest)
                {
                    latest = parsed;
                    have_version = true;
                }
            }
            if (!have_version)
            {
                *found = false;
                return true;
            }
            std::string value;
            const ::dsn::error_code loaded =
                service->get_state(version_key(key, latest), value);
            if (loaded == ::dsn::ERR_OBJECT_NOT_FOUND ||
                loaded == ::dsn::ERR_PATH_NOT_FOUND)
            {
                // A newer writer may have pruned the version between list and
                // get. Relist instead of surfacing a transient false outage.
                continue;
            }
            if (loaded != ::dsn::ERR_OK)
            {
                if (error != nullptr)
                {
                    *error = "failed to read latest shared breaker version: " +
                             std::string(loaded.to_string());
                }
                return false;
            }
            *found = true;
            if (!parse_state(value, state, error))
            {
                return false;
            }
            if (state->fencing_token != latest)
            {
                if (error != nullptr)
                {
                    *error =
                        "shared breaker record fencing token does not match its version";
                }
                return false;
            }
            return true;
        }
        if (error != nullptr)
        {
            *error = "shared breaker versions changed repeatedly while reading";
        }
        return false;
    }

    bool persist_state(rasn_coordination_service *service,
                       const std::string &key,
                       uint64_t fencing_token,
                       stored_breaker_state *state,
                       std::string *error) const
    {
        state->fencing_token = fencing_token;
        ::dsn::error_code err =
            service->put_state(version_key(key, fencing_token), serialize_state(*state));
        if (err != ::dsn::ERR_OK)
        {
            if (error != nullptr)
            {
                *error = "failed to persist fenced shared breaker state: " +
                         std::string(err.to_string());
            }
            return false;
        }

        std::vector<std::string> versions;
        err = service->list_state(record_key(key), versions);
        if (err != ::dsn::ERR_OK)
        {
            if (error != nullptr)
            {
                *error = "failed to verify fenced shared breaker state: " +
                         std::string(err.to_string());
            }
            return false;
        }
        uint64_t latest = 0;
        bool have_version = false;
        std::vector<std::pair<uint64_t, std::string>> parsed_versions;
        parsed_versions.reserve(versions.size());
        for (const std::string &version : versions)
        {
            uint64_t parsed = 0;
            if (!parse_uint64(version, &parsed))
            {
                if (error != nullptr)
                {
                    *error = "shared breaker version is not a valid fencing token";
                }
                return false;
            }
            latest = (std::max)(latest, parsed);
            have_version = true;
            parsed_versions.push_back(std::make_pair(parsed, version));
        }
        if (latest > fencing_token)
        {
            if (error != nullptr)
            {
                *error = "shared breaker mutation was fenced by a newer owner";
            }
            return false;
        }
        if (!have_version || latest != fencing_token)
        {
            if (error != nullptr)
            {
                *error = "fenced shared breaker write is not visible";
            }
            return false;
        }

        // Older versions are no longer authoritative. Retain a short tail so an
        // unlocked reader that listed the previous latest record can still fetch
        // it while writes are busy; correctness still comes from selecting the
        // greatest fence.
        std::sort(parsed_versions.begin(), parsed_versions.end());
        const size_t prune_count =
            parsed_versions.size() > kRetainedBreakerVersions
                ? parsed_versions.size() - kRetainedBreakerVersions
                : 0;
        for (size_t i = 0; i < prune_count; ++i)
        {
            if (parsed_versions[i].first < fencing_token)
            {
                const ::dsn::error_code pruned =
                    service->delete_state(record_key(key) + "/" +
                                          parsed_versions[i].second);
                if (pruned != ::dsn::ERR_OK)
                {
                    dwarn("failed to prune obsolete shared breaker fence %s: %s",
                          parsed_versions[i].second.c_str(),
                          pruned.to_string());
                }
                pruned.end_tracking();
            }
        }
        return true;
    }

    breaker_decision claim_probe(rasn_coordination_service *service,
                                 const std::string &owner_id,
                                 const std::string &key,
                                 const breaker_config &config,
                                 uint64_t now_ms,
                                 uint64_t probe_lease_hint_ms)
    {
        const std::shared_ptr<std::mutex> local = key_lock(key);
        std::lock_guard<std::mutex> local_guard(*local);
        const std::string resource = lock_key(key);
        uint64_t fencing_token = 0;
        ::dsn::error_code err = service->acquire_ownership(
            resource, owner_id, _config.lock_timeout_ms, &fencing_token);
        if (err != ::dsn::ERR_OK)
        {
            return unavailable_decision("failed to acquire shared breaker lock: " +
                                        std::string(err.to_string()));
        }

        breaker_decision decision;
        stored_breaker_state state;
        bool found = false;
        bool write = false;
        std::string error;
        if (!read_state(service, key, &state, &found, &error))
        {
            decision = unavailable_decision(error);
        }
        else if (found && state.fencing_token > fencing_token)
        {
            decision = unavailable_decision(
                "shared breaker probe claim was rejected by a newer fence");
        }
        else if (found && !config_matches(state, config, _config, &error))
        {
            decision = unavailable_decision(error);
        }
        else if (!found || state.state == breaker_state::closed)
        {
            decision.allowed = true;
            decision.state = breaker_state::closed;
            decision.generation = found ? state.generation : 0;
        }
        else if (state.state == breaker_state::open && !cooldown_elapsed(state, now_ms))
        {
            decision.allowed = false;
            decision.state = breaker_state::open;
            decision.generation = state.generation;
        }
        else if (state.state == breaker_state::half_open &&
                 (now_ms < state.probe_deadline_ms || now_ms < state.opened_at_ms))
        {
            decision.allowed = false;
            decision.state = breaker_state::half_open;
            decision.generation = state.generation;
        }
        else
        {
            state.state = breaker_state::half_open;
            state.probe_token = make_probe_token();
            const uint64_t requested_lease = (std::min)(
                _config.max_probe_lease_ms,
                (std::max)(_config.probe_lease_ms, probe_lease_hint_ms));
            state.probe_deadline_ms =
                saturating_add(now_ms, saturating_add(requested_lease, _config.clock_skew_ms));
            if (!next_revision(&state, &error))
            {
                decision = unavailable_decision(error);
            }
            else
            {
                write = true;
                decision.allowed = true;
                decision.state = breaker_state::half_open;
                decision.half_open_probe = true;
                decision.probe_token = state.probe_token;
                decision.generation = state.generation;
            }
        }

        if (write)
        {
            if (!persist_state(service, key, fencing_token, &state, &error))
            {
                decision = unavailable_decision(error);
            }
        }
        err = service->release_ownership(resource, owner_id, false);
        if (err != ::dsn::ERR_OK)
        {
            return unavailable_decision("failed to release shared breaker lock: " +
                                        std::string(err.to_string()));
        }
        if (write && decision.available)
        {
            err = service->verify_ownership_fence(resource, fencing_token);
            if (err != ::dsn::ERR_OK)
            {
                return unavailable_decision(
                    "shared breaker probe claim failed its post-release fence barrier: " +
                    std::string(err.to_string()));
            }
            stored_breaker_state committed;
            bool committed_found = false;
            if (!read_state(service, key, &committed, &committed_found, &error))
            {
                return unavailable_decision(error);
            }
            if (!committed_found || committed.fencing_token != fencing_token ||
                committed.revision != state.revision ||
                committed.probe_token != state.probe_token)
            {
                return unavailable_decision(
                    "shared breaker probe claim was superseded after release");
            }
        }
        return decision;
    }

    static void apply_report(stored_breaker_state &state,
                             const breaker_decision &admission,
                             bool ok,
                             uint64_t now_ms,
                             bool *write,
                             breaker_report *result)
    {
        const bool valid_probe =
            admission.half_open_probe && admission.probe_token != 0 &&
            state.state == breaker_state::half_open &&
            admission.probe_token == state.probe_token &&
            admission.generation == state.generation;
        if ((admission.half_open_probe && !valid_probe) ||
            (!admission.half_open_probe &&
             (state.state == breaker_state::half_open ||
              admission.generation != state.generation)))
        {
            result->applied = false;
            return;
        }

        switch (state.state)
        {
        case breaker_state::closed:
            if (ok)
            {
                if (state.consecutive_failures != 0)
                {
                    state.consecutive_failures = 0;
                    *write = true;
                }
            }
            else
            {
                if (state.consecutive_failures < (std::numeric_limits<uint32_t>::max)())
                {
                    ++state.consecutive_failures;
                }
                *write = true;
                if (state.consecutive_failures >= state.failure_threshold)
                {
                    if (state.generation == (std::numeric_limits<uint64_t>::max)())
                    {
                        *write = false;
                        result->available = false;
                        result->error = "shared breaker generation overflow";
                        return;
                    }
                    ++state.generation;
                    state.state = breaker_state::open;
                    state.opened_at_ms = now_ms;
                    state.probe_token = 0;
                    state.probe_deadline_ms = 0;
                    result->opened = true;
                }
            }
            break;
        case breaker_state::half_open:
            state.probe_token = 0;
            state.probe_deadline_ms = 0;
            *write = true;
            if (ok)
            {
                state.state = breaker_state::closed;
                state.consecutive_failures = 0;
                state.opened_at_ms = 0;
            }
            else
            {
                if (state.generation == (std::numeric_limits<uint64_t>::max)())
                {
                    *write = false;
                    result->available = false;
                    result->error = "shared breaker generation overflow";
                    return;
                }
                ++state.generation;
                state.state = breaker_state::open;
                state.opened_at_ms = now_ms;
                result->opened = true;
            }
            break;
        case breaker_state::open:
            result->applied = false;
            break;
        }
    }

    static breaker_registry_entry error_entry(const std::string &key,
                                              const std::string &error)
    {
        breaker_registry_entry entry;
        entry.key = key;
        entry.state = breaker_state::open;
        entry.shared = true;
        entry.available = false;
        entry.error = error;
        return entry;
    }

    std::string _scope;
    std::string _encoded_scope;
    rasn_shared_breaker_config _config;
    std::shared_ptr<rasn_coordination_context> _coordination;
    std::string _owner_id;
    mutable std::atomic<uint64_t> _operation_sequence{1};
    std::string _configuration_error;
    std::mutex _key_locks_lock;
    std::map<std::string, std::shared_ptr<std::mutex>> _key_locks;
};

} // namespace

rasn_shared_breaker_config load_rasn_shared_breaker_config()
{
    rasn_shared_breaker_config config;
    config.enabled = ::dsn_config_get_value_bool(
        "rasn.coordination",
        "shared_breaker_enabled",
        config.enabled,
        "store circuit-breaker state in the coordination backend");
    config.state_prefix = ::dsn_config_get_value_string(
        "rasn.coordination",
        "shared_breaker_state_prefix",
        config.state_prefix.c_str(),
        "relative coordination state prefix for shared circuit breakers");
    const uint64_t lock_timeout = ::dsn_config_get_value_uint64(
        "rasn.coordination",
        "shared_breaker_lock_timeout_ms",
        static_cast<uint64_t>(config.lock_timeout_ms),
        "maximum wait for a shared circuit-breaker mutation lock");
    config.lock_timeout_ms = static_cast<int>(
        (std::min)(lock_timeout, static_cast<uint64_t>((std::numeric_limits<int>::max)())));
    if (config.lock_timeout_ms <= 0)
    {
        config.lock_timeout_ms = 1;
    }
    config.probe_lease_ms = ::dsn_config_get_value_uint64(
        "rasn.coordination",
        "shared_breaker_probe_lease_ms",
        config.probe_lease_ms,
        "minimum lease for a cluster-wide half-open breaker probe");
    if (config.probe_lease_ms == 0)
    {
        config.probe_lease_ms = 1;
    }
    config.max_probe_lease_ms = ::dsn_config_get_value_uint64(
        "rasn.coordination",
        "shared_breaker_max_probe_lease_ms",
        config.max_probe_lease_ms,
        "maximum lease granted to a cluster-wide half-open breaker probe");
    config.clock_skew_ms = ::dsn_config_get_value_uint64(
        "rasn.coordination",
        "shared_breaker_clock_skew_ms",
        config.clock_skew_ms,
        "clock-skew safety margin for shared breaker cooldowns and probe leases");
    return config;
}

std::shared_ptr<circuit_breaker_registry_backend>
create_rasn_shared_breaker_backend(
    const std::string &scope,
    const rasn_shared_breaker_config &config,
    const std::shared_ptr<rasn_coordination_context> &coordination)
{
    return std::shared_ptr<circuit_breaker_registry_backend>(
        new coordinated_breaker_backend(scope, config, coordination));
}

void configure_rasn_shared_breaker_registry(circuit_breaker_registry &registry,
                                            const std::string &scope)
{
    const rasn_shared_breaker_config config = load_rasn_shared_breaker_config();
    if (!config.enabled)
    {
        return;
    }
    const std::shared_ptr<rasn_coordination_context> coordination =
        shared_rasn_coordination_context();
    // The backend resolves this context again for every operation because rDSN
    // coordination providers are bound to the current app's callback pools.
    registry.set_backend(create_rasn_shared_breaker_backend(scope, config, nullptr));
    dinfo("enabled coordination-backed circuit breakers for scope=%s via provider=%s",
          scope.c_str(),
          coordination->provider_name());
    if (std::string(coordination->provider_name()) != "zookeeper")
    {
        dwarn("coordination-backed circuit breakers for scope=%s use provider=%s; "
              "protection is not cross-process unless provider=zookeeper",
              scope.c_str(),
              coordination->provider_name());
    }
}

} // namespace rasn
} // namespace dsn
