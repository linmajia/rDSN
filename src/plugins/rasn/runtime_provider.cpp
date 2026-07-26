#include <rasn/runtime_provider.h>
#include <rasn/runtime_provider_internal.h>

#include <rasn/agent_services.h>
#include <rasn/circuit_breaker.h>
#include <rasn/coordination_breaker.h>
#include <rasn/metrics.h>
#include <rasn/rasn_core.h>
#include <rasn/rpc_resilience.h>
#include <rasn/state_service.h>

#include <dsn/cpp/utils.h>
#include <dsn/cpp/zlocks.h>
#include <dsn/tool-api/task.h>
#include <dsn/utility/configuration.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <future>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>
#include <utility>

namespace dsn {
namespace rasn {

// Thread-local ambient trace id backing rasn_runtime_trace_scope (declared in the
// header). Internal linkage but visible throughout this translation unit, so the
// generated request metadata initialization can stamp it onto each module call.
static std::string &rasn_runtime_ambient_trace_id()
{
    static thread_local std::string ambient;
    return ambient;
}

std::string current_rasn_runtime_trace_id() { return rasn_runtime_ambient_trace_id(); }

rasn_runtime_trace_scope::rasn_runtime_trace_scope(const std::string &trace_id)
    : _previous(rasn_runtime_ambient_trace_id()), _changed(!trace_id.empty())
{
    if (_changed)
    {
        rasn_runtime_ambient_trace_id() = trace_id;
    }
}

rasn_runtime_trace_scope::~rasn_runtime_trace_scope()
{
    if (_changed)
    {
        rasn_runtime_ambient_trace_id() = _previous;
    }
}

bool rasn_runtime_module_is_sharded(const std::string &module)
{
    return module == "agent_message_bus" || module == "resource_budget" ||
           module == "blackboard" || module == "human_interaction";
}

namespace {

std::string config_string(const char *key, const char *fallback, const char *description)
{
    const char *value = ::dsn_config_get_value_string("rasn.runtime", key, fallback, description);
    return value == nullptr ? fallback : value;
}

std::string lower_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string normalize_rasn_runtime_provider_name(const std::string &name)
{
    const std::string provider = lower_ascii(trim(name.empty() ? "local" : name));
    if (provider == "embedded" || provider == "in-process" || provider == "inprocess")
    {
        return "local";
    }
    if (provider == "rdsn" || provider == "rdsn-state" || provider == "remote")
    {
        return "distributed";
    }
    if (provider == "hybrid" || provider == "mixed" || provider == "per-module" || provider == "per_module")
    {
        return "hybrid";
    }
    return provider == "distributed" ? "distributed" : "local";
}

std::string state_key_component(const std::string &value)
{
    std::ostringstream output;
    for (const unsigned char c : value)
    {
        if (std::isalnum(c) || c == '.' || c == '-' || c == '_')
        {
            output << static_cast<char>(c);
        }
        else
        {
            output << '_' << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(c)
                   << std::dec << std::setfill('0');
        }
    }
    return output.str().empty() ? "_" : output.str();
}

std::string join_strings(const std::vector<std::string> &values, const std::string &delimiter)
{
    std::ostringstream output;
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (i != 0)
        {
            output << delimiter;
        }
        output << values[i];
    }
    return output.str();
}

std::string describe_sandbox_profile(const sandbox_profile &profile)
{
    std::ostringstream output;
    output << "profile=" << profile.name
           << "\nallow_filesystem_read=" << (profile.allow_filesystem_read ? "true" : "false")
           << "\nallow_filesystem_write=" << (profile.allow_filesystem_write ? "true" : "false")
           << "\nallow_network=" << (profile.allow_network ? "true" : "false")
           << "\nallow_process_spawn=" << (profile.allow_process_spawn ? "true" : "false")
           << "\nallowed_roots=" << join_strings(profile.allowed_roots, ",")
           << "\ndenied_paths=" << join_strings(profile.denied_paths, ",");
    return output.str();
}

uint16_t config_service_port(const std::string &key, uint16_t default_port)
{
    const uint64_t configured =
        ::dsn_config_get_value_uint64("rasn.service", key.c_str(), default_port, "rASN runtime module RPC port");
    if (configured > (std::numeric_limits<uint16_t>::max)())
    {
        dwarn("rasn.service.%s=%llu exceeds uint16_t port range; using default %u",
              key.c_str(),
              static_cast<unsigned long long>(configured),
              static_cast<unsigned int>(default_port));
        return default_port;
    }
    return static_cast<uint16_t>(configured);
}

std::string config_service_string(const std::string &key, const std::string &fallback, const std::string &description)
{
    const char *value = ::dsn_config_get_value_string("rasn.service", key.c_str(), fallback.c_str(), description.c_str());
    return value == nullptr ? fallback : value;
}

std::string config_service_override(const std::string &key,
                                    const std::string &fallback,
                                    const std::string &description,
                                    bool *configured)
{
    static const std::string unset = "__rasn_config_value_not_set__";
    const std::string value = config_service_string(key, unset, description);
    const bool present = value != unset;
    if (configured != nullptr)
    {
        *configured = present;
    }
    return present ? value : fallback;
}

uint16_t config_service_port_override(const std::string &key,
                                      uint16_t fallback,
                                      bool *configured)
{
    const uint64_t unset = (std::numeric_limits<uint64_t>::max)();
    const uint64_t value = ::dsn_config_get_value_uint64(
        "rasn.service", key.c_str(), unset, "rASN runtime module RPC port override");
    const bool present = value != unset;
    if (configured != nullptr)
    {
        *configured = present;
    }
    if (!present)
    {
        return fallback;
    }
    if (value == 0 || value > (std::numeric_limits<uint16_t>::max)())
    {
        dwarn("rasn.service.%s=%llu is outside the valid RPC port range; using inherited port %u",
              key.c_str(),
              static_cast<unsigned long long>(value),
              static_cast<unsigned int>(fallback));
        return fallback;
    }
    return static_cast<uint16_t>(value);
}

uint64_t config_service_uint64(const std::string &key, uint64_t fallback, const std::string &description)
{
    return ::dsn_config_get_value_uint64("rasn.service", key.c_str(), fallback, description.c_str());
}

bool config_service_bool(const std::string &key, bool fallback, const std::string &description)
{
    return ::dsn_config_get_value_bool("rasn.service", key.c_str(), fallback, description.c_str());
}

std::string module_service_key(const std::string &module)
{
    return state_key_component(module);
}

std::string rasn_runtime_state_key(const std::string &state_prefix,
                                   const std::string &module,
                                   const std::string &kind,
                                   const std::string &key)
{
    return state_prefix + "/" + module + "/" + kind + "/" + state_key_component(key);
}

std::string rasn_runtime_state_prefix(const std::string &state_prefix)
{
    return state_prefix + "/";
}

bool has_module(const std::vector<std::string> &modules, const std::string &module)
{
    return std::find(modules.begin(), modules.end(), module) != modules.end();
}

struct runtime_endpoint
{
    bool ok = true;
    ::dsn::rpc_address address;
    std::string source;
    std::string error;
    uint64_t generation = 0;
    uint32_t partition_index = 0;
    uint32_t partition_count = 1;
    // True when the operator explicitly declared this endpoint (a URI, host, or
    // port override) in [rasn.service] rather than falling through to the localhost
    // default. An explicitly declared endpoint is authoritative: the app was told
    // exactly where the runtime lives, so it routes there directly instead of
    // second-guessing through registry discovery (which may advertise a
    // primary_address() -- an auto-selected, possibly unreachable NIC).
    bool explicit_config = false;
};

struct runtime_registry_lookup
{
    bool ok = true;
    bool found = false;
    runtime_endpoint endpoint;
    std::string error;
};

void warn_resolver_partition_contract_once(const std::string &module,
                                           const std::string &uri,
                                           uint32_t partition_count)
{
    static std::mutex lock;
    static std::set<std::string> warned;
    const std::string warning_key =
        module + "\x1f" + uri + "\x1f" + std::to_string(partition_count);
    {
        std::lock_guard<std::mutex> guard(lock);
        if (!warned.insert(warning_key).second)
        {
            return;
        }
    }
    dwarn("runtime module '%s' uses shared resolver URI '%s' across %u configured "
          "shards; rDSN does not expose the table partition count for startup "
          "validation, so operators must ensure the counts match",
          module.c_str(),
          uri.c_str(),
          static_cast<unsigned int>(partition_count));
}

std::string rasn_runtime_breaker_key(const std::string &module,
                                     const runtime_endpoint &endpoint)
{
    // URI calls expose only the logical table URI. rDSN invalidates and retries a
    // failed physical replica internally, so rASN deliberately keys the breaker
    // to the logical partition after that resolver path returns a terminal failure.
    return module + "#" + std::to_string(endpoint.partition_index) + "@" +
           std::string(endpoint.address.to_string());
}

uint64_t fnv1a64(const std::string &value)
{
    uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char c : value)
    {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

template <typename Request>
uint64_t rasn_runtime_partition_hash_impl(const Request &request)
{
    if (request.metadata.__isset.route_partition)
    {
        return static_cast<uint64_t>(request.metadata.route_partition);
    }
    const std::string module = runtime_module_name(request);
    const std::string key = runtime_request_key(request);
    // Sharded modules historically hashed even the empty natural key. Preserve
    // that corner-case mapping; unsharded keyless control calls keep hash zero.
    return key.empty() && !rasn_runtime_module_is_sharded(module) ? 0 : fnv1a64(key);
}

uint32_t rasn_runtime_partition_count(const std::string &module)
{
    if (!rasn_runtime_module_is_sharded(module))
    {
        return 1;
    }
    const std::string key = module_service_key(module) + "_shard_count";
    const uint64_t configured =
        config_service_uint64(key, 1, "rASN sharded runtime module partition count");
    if (configured == 0)
    {
        return 1;
    }
    if (configured > (std::numeric_limits<uint32_t>::max)())
    {
        dwarn("rasn.service.%s=%llu exceeds uint32_t range; using maximum partition count",
              key.c_str(),
              static_cast<unsigned long long>(configured));
        return (std::numeric_limits<uint32_t>::max)();
    }
    return static_cast<uint32_t>(configured);
}

bool parse_uint32_token(const std::string &value, uint32_t *result)
{
    if (value.empty())
    {
        return false;
    }
    uint64_t parsed = 0;
    for (const char c : value)
    {
        if (c < '0' || c > '9')
        {
            return false;
        }
        parsed = parsed * 10 + static_cast<uint64_t>(c - '0');
        if (parsed > (std::numeric_limits<uint32_t>::max)())
        {
            return false;
        }
    }
    if (result != nullptr)
    {
        *result = static_cast<uint32_t>(parsed);
    }
    return true;
}

void add_hosted_shard_token(const std::string &module,
                            const std::string &token,
                            uint32_t partition_count,
                            std::set<uint32_t> *hosted)
{
    const std::string normalized = lower_ascii(trim(token));
    if (normalized.empty() || hosted == nullptr)
    {
        return;
    }
    if (normalized == "all" || normalized == "*")
    {
        for (uint32_t i = 0; i < partition_count; ++i)
        {
            hosted->insert(i);
        }
        return;
    }

    uint32_t shard = 0;
    if (!parse_uint32_token(normalized, &shard) || shard >= partition_count)
    {
        dwarn("ignoring invalid hosted shard '%s' for rASN runtime module %s partition_count=%u",
              token.c_str(),
              module.c_str(),
              partition_count);
        return;
    }
    hosted->insert(shard);
}

std::vector<uint32_t> rasn_runtime_hosted_shards(const std::string &module)
{
    const uint32_t partition_count = rasn_runtime_partition_count(module);
    if (!rasn_runtime_module_is_sharded(module) || partition_count <= 1)
    {
        return std::vector<uint32_t>();
    }

    const std::string module_key = module_service_key(module);
    std::string configured = trim(config_service_string(module_key + "_hosted_shards",
                                                        "",
                                                        "Comma-separated rASN runtime shard indexes hosted by this service"));
    if (configured.empty())
    {
        configured = trim(config_service_string(module_key + "_shard_index",
                                                "",
                                                "Single rASN runtime shard index hosted by this service"));
    }
    if (configured.empty())
    {
        return std::vector<uint32_t>();
    }

    std::set<uint32_t> hosted;
    std::string token;
    for (size_t i = 0; i <= configured.size(); ++i)
    {
        const bool separator = i == configured.size() || configured[i] == ',' || configured[i] == ';' ||
                               std::isspace(static_cast<unsigned char>(configured[i]));
        if (separator)
        {
            add_hosted_shard_token(module, token, partition_count, &hosted);
            token.clear();
        }
        else
        {
            token.push_back(configured[i]);
        }
    }
    return std::vector<uint32_t>(hosted.begin(), hosted.end());
}

uint32_t rasn_runtime_partition_for_key(const std::string &module, const std::string &key)
{
    const uint32_t count = rasn_runtime_partition_count(module);
    if (count <= 1)
    {
        return 0;
    }
    return static_cast<uint32_t>(fnv1a64(key) % count);
}

template <typename Request>
uint32_t rasn_runtime_partition_for_request(const Request &request)
{
    const std::string module = runtime_module_name(request);
    const uint32_t count = rasn_runtime_partition_count(module);
    if (count <= 1)
    {
        return 0;
    }
    if (request.metadata.__isset.route_partition)
    {
        return static_cast<uint32_t>(request.metadata.route_partition) % count;
    }
    return rasn_runtime_partition_for_key(module, runtime_request_key(request));
}

::dsn::rpc_address rasn_service_address(const std::string &service_name, uint16_t default_port)
{
    if (service_name == "registry")
    {
        return configured_rasn_registry_address();
    }
    const std::string uri = config_service_string(service_name + "_uri", "", "rASN service URI");
    if (!uri.empty())
    {
        return ::dsn::url_host_address(uri.c_str());
    }
    const std::string default_host = config_service_string("host", "localhost", "default rASN service RPC host");
    std::string host = config_service_string(service_name + "_host", default_host, "rASN service RPC host");
    if (host.empty())
    {
        host = "localhost";
    }
    ::dsn::rpc_address address;
    address.assign_ipv4(host.c_str(), config_service_port(service_name + "_port", default_port));
    return address;
}

std::string rasn_runtime_module_capability(const std::string &module)
{
    return "rasn.runtime." + module;
}

std::string rasn_runtime_module_shard_capability(const std::string &module, uint32_t partition_index)
{
    return rasn_runtime_module_capability(module) + ".shard." + std::to_string(partition_index);
}

bool parse_rasn_runtime_record_kind(const std::string &record_kind, std::string *module, std::string *kind)
{
    const std::string prefix = "rasn.runtime.";
    if (record_kind.find(prefix) != 0)
    {
        return false;
    }
    const std::string rest = record_kind.substr(prefix.size());
    const size_t dot = rest.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= rest.size())
    {
        return false;
    }
    if (module != nullptr)
    {
        *module = rest.substr(0, dot);
    }
    if (kind != nullptr)
    {
        *kind = rest.substr(dot + 1);
    }
    return true;
}

bool runtime_hydration_supported(const std::string &module, const std::string &kind)
{
    return (module == "agent_control_plane" && kind == "agent") ||
           (module == "agent_message_bus" && kind == "message") ||
           (module == "task_orchestration_kernel" && kind == "task") ||
           (module == "determinism_ledger" && kind == "choice") ||
           (module == "capability_directory" && kind == "provider") ||
           (module == "resource_budget" && (kind == "quota" || kind == "usage")) ||
           (module == "recovery_supervisor" && (kind == "policy" || kind == "failure")) ||
           (module == "blackboard" && kind == "entry") ||
           (module == "contract_verifier" && kind == "contract") ||
           (module == "human_interaction" && kind == "request") ||
           (module == "sandbox_runtime" && kind == "profile");
}

bool rasn_runtime_registry_discovery_enabled()
{
    return config_service_bool(
        "rasn_runtime_registry_discovery_enabled", true, "Discover rASN runtime module endpoints through rasn.registry");
}

bool rasn_runtime_registry_registration_enabled()
{
    return config_service_bool(
        "rasn_runtime_registry_registration_enabled", true, "Register rASN runtime module services with rasn.registry");
}

std::chrono::milliseconds rasn_runtime_registry_timeout()
{
    const uint64_t timeout_ms = ::dsn_config_get_value_uint64(
        "rasn.registry", "registration_timeout_ms", 1000, "rASN registry register/query/heartbeat RPC timeout");
    return std::chrono::milliseconds(timeout_ms);
}

uint64_t rasn_runtime_registry_heartbeat_ms()
{
    return ::dsn_config_get_value_uint64(
        "rasn.registry", "heartbeat_ms", 2000, "rASN runtime registry heartbeat interval in milliseconds");
}

bool rasn_runtime_state_hydration_enabled()
{
    return config_service_bool(
        "rasn_runtime_state_hydration_enabled", true, "Hydrate rASN runtime module services from mirrored state");
}

bool rasn_runtime_state_mirroring_enabled()
{
    return config_service_bool(
        "rasn_runtime_state_mirroring_enabled",
        true,
        "Mirror successful remote runtime mutations into rasn.state (disable for native replicated modules)");
}

bool rasn_runtime_module_uses_native_replication(const std::string &module)
{
    const bool common = config_service_bool(
        "rasn_runtime_native_replication_enabled",
        false,
        "Runtime module endpoints are native rDSN type-1 replica groups");
    return config_service_bool(module_service_key(module) + "_native_replication",
                               common,
                               "Per-module native rDSN type-1 replication override");
}

bool rasn_runtime_ownership_gate_enabled()
{
    return config_service_bool(
        "rasn_runtime_ownership_gate_enabled",
        false,
        "Acquire single-writer ownership of hosted runtime module shards (via [rasn.coordination]) "
        "before opening RPC handlers; fail closed on contention");
}

std::chrono::milliseconds rasn_runtime_request_drain_timeout()
{
    const uint64_t configured = config_service_uint64(
        "rasn_runtime_request_drain_timeout_ms",
        30000,
        "Maximum time to drain in-flight runtime module RPCs before releasing ownership");
    const uint64_t chrono_max =
        static_cast<uint64_t>((std::numeric_limits<int64_t>::max)());
    return std::chrono::milliseconds(
        static_cast<int64_t>((std::min)(configured, chrono_max)));
}

std::chrono::milliseconds rasn_runtime_state_hydration_timeout()
{
    const uint64_t timeout_ms = config_service_uint64(
        "rasn_runtime_state_hydration_timeout_ms", 500, "rASN runtime state hydration query timeout in milliseconds");
    return std::chrono::milliseconds(timeout_ms);
}

uint32_t rasn_runtime_state_hydration_max_attempts()
{
    const uint64_t attempts = config_service_uint64(
        "rasn_runtime_state_hydration_max_attempts",
        20,
        "Max attempts for the rASN runtime state hydration query while the co-located state service "
        "becomes ready at startup");
    if (attempts == 0)
    {
        return 1;
    }
    // Honor the operator's configured readiness budget as-is: clamp only to the
    // uint32_t return width to avoid truncation (a type-safety guard, not a policy
    // cap), so a deployment can raise the budget for an unusually long cold start.
    if (attempts > std::numeric_limits<uint32_t>::max())
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(attempts);
}

std::chrono::milliseconds rasn_runtime_state_hydration_retry_backoff()
{
    const uint64_t backoff_ms = config_service_uint64(
        "rasn_runtime_state_hydration_retry_backoff_ms",
        250,
        "Backoff between rASN runtime state hydration retries in milliseconds");
    return std::chrono::milliseconds(backoff_ms);
}

uint32_t rasn_runtime_ownership_acquire_max_attempts()
{
    const uint64_t attempts = config_service_uint64(
        "rasn_runtime_ownership_acquire_max_attempts",
        1,
        "Max attempts the single-writer ownership gate makes to acquire a contended resource before "
        "failing closed; raise it to let a standby ride out a brief ownership handover during failover");
    if (attempts == 0)
    {
        return 1;
    }
    // Honor the configured attempt count as-is: clamp only to the uint32_t return
    // width to avoid truncation (a type-safety guard, not a policy cap).
    if (attempts > std::numeric_limits<uint32_t>::max())
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(attempts);
}

std::chrono::milliseconds rasn_runtime_ownership_acquire_retry_backoff()
{
    const uint64_t backoff_ms = config_service_uint64(
        "rasn_runtime_ownership_acquire_retry_backoff_ms",
        1000,
        "Backoff between single-writer ownership acquisition retries in milliseconds");
    return std::chrono::milliseconds(backoff_ms);
}

bool rasn_runtime_state_watermark_enabled()
{
    return config_service_bool(
        "rasn_runtime_state_watermark_enabled", true, "Write runtime state mirror watermarks after mirrored mutations");
}

bool rasn_runtime_state_watermark_verify_enabled()
{
    return config_service_bool(
        "rasn_runtime_state_watermark_verify_enabled", true, "Verify runtime state mirror watermarks during hydration");
}

bool rasn_runtime_rpc_auth_enabled()
{
    return config_service_bool(
        "rasn_runtime_auth_enabled", false, "Require a shared token on distributed rASN runtime module RPCs");
}

std::string rasn_runtime_rpc_auth_token()
{
    return config_service_string(
        "rasn_runtime_auth_token", "", "Shared token for distributed rASN runtime module RPC authentication");
}

bool constant_time_equal(const std::string &lhs, const std::string &rhs)
{
    const size_t max_size = (std::max)(lhs.size(), rhs.size());
    size_t diff = lhs.size() ^ rhs.size();
    for (size_t i = 0; i < max_size; ++i)
    {
        const unsigned char lhs_byte = i < lhs.size() ? static_cast<unsigned char>(lhs[i]) : 0;
        const unsigned char rhs_byte = i < rhs.size() ? static_cast<unsigned char>(rhs[i]) : 0;
        diff |= static_cast<size_t>(lhs_byte ^ rhs_byte);
    }
    return diff == 0;
}

template <typename Request>
bool prepare_rasn_runtime_rpc_request(Request *request, std::string *error)
{
    if (request == nullptr)
    {
        if (error != nullptr)
        {
            *error = "runtime module RPC request is null";
        }
        return false;
    }
    if (!rasn_runtime_rpc_auth_enabled())
    {
        request->metadata.__set_auth_token(std::string());
        return true;
    }
    const std::string token = rasn_runtime_rpc_auth_token();
    if (token.empty())
    {
        if (error != nullptr)
        {
            *error = "runtime module RPC auth enabled but rasn_runtime_auth_token is empty";
        }
        return false;
    }
    request->metadata.__set_auth_token(token);
    if (error != nullptr)
    {
        error->clear();
    }
    return true;
}

template <typename Request>
bool authenticate_rasn_runtime_rpc_request(const Request &request, std::string *error)
{
    if (!rasn_runtime_rpc_auth_enabled())
    {
        return true;
    }
    const std::string expected = rasn_runtime_rpc_auth_token();
    if (expected.empty())
    {
        if (error != nullptr)
        {
            *error = "runtime module RPC auth enabled but rasn_runtime_auth_token is empty on service";
        }
        return false;
    }
    if (!request.metadata.__isset.auth_token || request.metadata.auth_token.empty() ||
        !constant_time_equal(request.metadata.auth_token, expected))
    {
        if (error != nullptr)
        {
            *error = "runtime module RPC auth failed";
        }
        return false;
    }
    if (error != nullptr)
    {
        error->clear();
    }
    return true;
}

runtime_endpoint static_rasn_runtime_endpoint(const std::string &module, uint32_t partition_index = 0)
{
    const std::string module_key = module_service_key(module);
    const uint32_t partition_count = rasn_runtime_partition_count(module);
    const bool sharded = partition_count > 1;
    const std::string shard_key = module_key + "_shard_" + std::to_string(partition_index);
    const std::string common_uri = config_service_string("rasn_runtime_uri", "", "rASN runtime module service URI");
    const std::string module_uri = config_service_override(
        module_key + "_uri", common_uri, "rASN per-module service URI", nullptr);
    bool shard_uri_configured = false;
    const std::string uri =
        sharded ? config_service_override(shard_key + "_uri",
                                          module_uri,
                                          "rASN per-shard module service URI",
                                          &shard_uri_configured)
                : module_uri;
    if (!uri.empty())
    {
        runtime_endpoint endpoint;
        endpoint.address = ::dsn::url_host_address(uri.c_str());
        endpoint.source =
            endpoint.address.type() == HOST_TYPE_URI
                ? (sharded ? "resolver:shard" : "resolver")
                : (sharded ? "static:shard" : "static");
        if (endpoint.address.type() == HOST_TYPE_URI && sharded &&
            !shard_uri_configured)
        {
            warn_resolver_partition_contract_once(module, uri, partition_count);
        }
        endpoint.partition_index = partition_index;
        endpoint.partition_count = partition_count;
        // A URI is only ever set by the operator, so it is always authoritative.
        endpoint.explicit_config = true;
        return endpoint;
    }
    const std::string default_host = config_service_string("host", "localhost", "default rASN service RPC host");
    bool common_host_configured = false;
    const std::string common_host = config_service_override(
        "rasn_runtime_host", default_host, "rASN runtime module service host", &common_host_configured);
    bool module_host_configured = false;
    const std::string module_host = config_service_override(
        module_key + "_host", common_host, "rASN per-module service host", &module_host_configured);
    bool shard_host_configured = false;
    std::string host =
        sharded ? config_service_override(shard_key + "_host",
                                          module_host,
                                          "rASN per-shard module service host",
                                          &shard_host_configured)
                : module_host;
    const bool host_explicit = common_host_configured || module_host_configured ||
                               (sharded && shard_host_configured);
    if (host.empty())
    {
        host = "localhost";
    }
    bool common_port_configured = false;
    const uint16_t common_port =
        config_service_port_override("rasn_runtime_port", 27107, &common_port_configured);
    bool module_port_configured = false;
    const uint16_t module_port = config_service_port_override(
        module_key + "_port", common_port, &module_port_configured);
    bool shard_port_configured = false;
    const uint16_t port =
        sharded ? config_service_port_override(
                      shard_key + "_port", module_port, &shard_port_configured)
                : module_port;
    ::dsn::rpc_address address;
    address.assign_ipv4(host.c_str(), port);
    runtime_endpoint endpoint;
    endpoint.address = address;
    endpoint.source = sharded ? "static:shard" : "static";
    endpoint.partition_index = partition_index;
    endpoint.partition_count = partition_count;
    endpoint.explicit_config = host_explicit || common_port_configured ||
                               module_port_configured ||
                               (sharded && shard_port_configured);
    return endpoint;
}

bool endpoint_from_descriptor(const agent_descriptor &descriptor, ::dsn::rpc_address *address)
{
    if (address == nullptr)
    {
        return false;
    }
    if (!descriptor.endpoint_uri.empty())
    {
        *address = ::dsn::url_host_address(descriptor.endpoint_uri.c_str());
        return !address->is_invalid();
    }
    if (!descriptor.host.empty() && descriptor.port > 0 &&
        descriptor.port <= (std::numeric_limits<uint16_t>::max)())
    {
        address->assign_ipv4(descriptor.host.c_str(), static_cast<uint16_t>(descriptor.port));
        return !address->is_invalid();
    }
    return false;
}

bool choose_registry_endpoint(const std::string &module,
                              std::vector<agent_descriptor> agents,
                              uint32_t partition_index,
                              const std::string &source,
                              runtime_endpoint *endpoint)
{
    if (endpoint == nullptr || agents.empty())
    {
        return false;
    }
    const std::string preferred_role = rasn_runtime_module_app_role(module);
    const auto choose = [&preferred_role](const agent_descriptor &lhs, const agent_descriptor &rhs) {
        const bool lhs_preferred = lhs.role == preferred_role || lhs.app_name == preferred_role;
        const bool rhs_preferred = rhs.role == preferred_role || rhs.app_name == preferred_role;
        if (lhs_preferred != rhs_preferred)
        {
            return lhs_preferred;
        }
        return lhs.agent_id < rhs.agent_id;
    };
    std::sort(agents.begin(), agents.end(), choose);

    for (size_t offset = 0; offset < agents.size(); ++offset)
    {
        const size_t index = (static_cast<size_t>(partition_index) + offset) % agents.size();
        ::dsn::rpc_address address;
        if (endpoint_from_descriptor(agents[index], &address))
        {
            endpoint->address = address;
            endpoint->source = source;
            endpoint->partition_index = partition_index;
            endpoint->partition_count = rasn_runtime_partition_count(module);
            return true;
        }
    }
    return false;
}

runtime_registry_lookup lookup_rasn_runtime_endpoint_capability(
    const std::string &module,
    uint32_t partition_index,
    const std::string &capability,
    const std::string &source)
{
    runtime_registry_lookup lookup;
    if (!rasn_runtime_registry_discovery_enabled())
    {
        return lookup;
    }

    std::vector<agent_descriptor> local_agents;
    if (!global_agent_registry().query_by_capability(
            capability, true, &local_agents, &lookup.error))
    {
        lookup.ok = false;
        lookup.error = "local registry query failed for capability '" +
                       capability + "': " + lookup.error;
        return lookup;
    }
    if (choose_registry_endpoint(
            module, local_agents, partition_index, source, &lookup.endpoint))
    {
        lookup.found = true;
        return lookup;
    }

    registry_query_request request;
    request.capability = capability;
    request.healthy_only = true;

    rasn_registry_client registry(rasn_service_address("registry", 27100));
    ::dsn::error_code err;
    registry_query_response response;
    std::tie(err, response) = registry.query_sync(request, rasn_runtime_registry_timeout());
    if (err != ::dsn::ERR_OK)
    {
        lookup.ok = false;
        lookup.error = "registry query transport failure for capability '" +
                       capability + "': " + err.to_string();
        return lookup;
    }
    if (!response.ok)
    {
        lookup.ok = false;
        lookup.error = "registry query failed for capability '" + capability +
                       "': " + response.error;
        return lookup;
    }
    lookup.found = choose_registry_endpoint(
        module, response.agents, partition_index, source, &lookup.endpoint);
    return lookup;
}

runtime_registry_lookup
lookup_rasn_runtime_endpoint_in_registry(const std::string &module,
                                         uint32_t partition_index)
{
    const uint32_t partition_count = rasn_runtime_partition_count(module);
    if (partition_count > 1)
    {
        runtime_registry_lookup shard =
            lookup_rasn_runtime_endpoint_capability(
                module,
                partition_index,
                rasn_runtime_module_shard_capability(module, partition_index),
                "registry:shard");
        if (!shard.ok || shard.found)
        {
            return shard;
        }
    }
    return lookup_rasn_runtime_endpoint_capability(
        module,
        partition_index,
        rasn_runtime_module_capability(module),
        "registry");
}

endpoint_resolution resolve_rasn_runtime_partition_endpoint_once(
    const std::string &module,
    uint32_t partition_index,
    const runtime_endpoint &static_endpoint)
{
    endpoint_resolution resolved;
    const runtime_registry_lookup registry =
        lookup_rasn_runtime_endpoint_in_registry(module, partition_index);
    if (!registry.ok)
    {
        resolved.error = registry.error;
        return resolved;
    }
    resolved.ok = true;
    resolved.found = true;
    if (registry.found)
    {
        resolved.address = registry.endpoint.address;
        resolved.source = registry.endpoint.source;
        return resolved;
    }
    resolved.address = static_endpoint.address;
    resolved.source = static_endpoint.source + ":fallback";
    return resolved;
}

std::shared_ptr<refreshable_endpoint_binding>
rasn_runtime_partition_binding(const std::string &module,
                               uint32_t partition_index)
{
    static std::mutex lock;
    static std::map<std::string, std::shared_ptr<refreshable_endpoint_binding>>
        bindings;
    const std::string key =
        module + "#" + std::to_string(partition_index);
    std::lock_guard<std::mutex> guard(lock);
    const auto existing = bindings.find(key);
    if (existing != bindings.end())
    {
        return existing->second;
    }

    const runtime_endpoint fallback =
        static_rasn_runtime_endpoint(module, partition_index);
    endpoint_resolver resolver;
    const bool refreshable =
        fallback.address.type() != HOST_TYPE_URI &&
        rasn_runtime_registry_discovery_enabled();
    if (refreshable)
    {
        resolver = [module, partition_index, fallback]() {
            return resolve_rasn_runtime_partition_endpoint_once(
                module, partition_index, fallback);
        };
    }
    std::shared_ptr<refreshable_endpoint_binding> binding =
        std::make_shared<refreshable_endpoint_binding>(
            key,
            fallback.address,
            fallback.source,
            /*resolve_on_first_use=*/refreshable && !fallback.explicit_config,
            resolver);
    bindings[key] = binding;
    return binding;
}

runtime_endpoint
resolve_rasn_runtime_partition_endpoint(const std::string &module,
                                        uint32_t partition_index)
{
    const runtime_endpoint configured =
        static_rasn_runtime_endpoint(module, partition_index);
    const endpoint_snapshot snapshot =
        rasn_runtime_partition_binding(module, partition_index)->current();
    runtime_endpoint endpoint;
    endpoint.ok = snapshot.ok;
    endpoint.address = snapshot.address;
    endpoint.source = snapshot.source;
    endpoint.error = snapshot.error;
    endpoint.generation = snapshot.generation;
    endpoint.partition_index = partition_index;
    endpoint.partition_count = rasn_runtime_partition_count(module);
    endpoint.explicit_config = configured.explicit_config;
    return endpoint;
}

runtime_endpoint resolve_rasn_runtime_endpoint(const std::string &module, const std::string &key = "")
{
    return resolve_rasn_runtime_partition_endpoint(module, rasn_runtime_partition_for_key(module, key));
}

template <typename Request>
runtime_endpoint resolve_rasn_runtime_endpoint(const Request &request)
{
    return resolve_rasn_runtime_partition_endpoint(runtime_module_name(request),
                                                   rasn_runtime_partition_for_request(request));
}

::dsn::rpc_address rasn_runtime_address(const std::string &module)
{
    return resolve_rasn_runtime_endpoint(module).address;
}

std::string runtime_endpoint_label(const runtime_endpoint &endpoint)
{
    std::ostringstream output;
    if (!endpoint.ok)
    {
        output << "unavailable:" << endpoint.error;
        return output.str();
    }
    output << endpoint.source << ":" << endpoint.address.to_string()
           << "#generation=" << endpoint.generation;
    if (endpoint.partition_count > 1)
    {
        output << "#shard=" << endpoint.partition_index << "/" << endpoint.partition_count;
    }
    return output.str();
}

std::string rasn_runtime_module_endpoint_summary(const std::string &module)
{
    const uint32_t partition_count = rasn_runtime_partition_count(module);
    if (partition_count <= 1)
    {
        return runtime_endpoint_label(resolve_rasn_runtime_endpoint(module));
    }

    std::ostringstream output;
    for (uint32_t i = 0; i < partition_count; ++i)
    {
        if (i != 0)
        {
            output << ",";
        }
        output << "shard" << i << "=" << runtime_endpoint_label(resolve_rasn_runtime_partition_endpoint(module, i));
    }
    return output.str();
}

::dsn::task_code rpc_code_for_module(const std::string &module, bool write = false)
{
    if (module == "agent_control_plane") return write ? RPC_RASN_AGENT_CONTROL_WRITE : RPC_RASN_AGENT_CONTROL;
    if (module == "agent_message_bus") return write ? RPC_RASN_MESSAGE_BUS_WRITE : RPC_RASN_MESSAGE_BUS;
    if (module == "task_orchestration_kernel")
        return write ? RPC_RASN_TASK_ORCHESTRATION_WRITE : RPC_RASN_TASK_ORCHESTRATION;
    if (module == "determinism_ledger")
        return write ? RPC_RASN_DETERMINISM_LEDGER_WRITE : RPC_RASN_DETERMINISM_LEDGER;
    if (module == "capability_directory")
        return write ? RPC_RASN_CAPABILITY_DIRECTORY_WRITE : RPC_RASN_CAPABILITY_DIRECTORY;
    if (module == "resource_budget") return write ? RPC_RASN_RESOURCE_BUDGET_WRITE : RPC_RASN_RESOURCE_BUDGET;
    if (module == "recovery_supervisor")
        return write ? RPC_RASN_RECOVERY_SUPERVISOR_WRITE : RPC_RASN_RECOVERY_SUPERVISOR;
    if (module == "blackboard") return write ? RPC_RASN_BLACKBOARD_WRITE : RPC_RASN_BLACKBOARD;
    if (module == "contract_verifier")
        return write ? RPC_RASN_CONTRACT_VERIFIER_WRITE : RPC_RASN_CONTRACT_VERIFIER;
    if (module == "human_interaction")
        return write ? RPC_RASN_HUMAN_INTERACTION_WRITE : RPC_RASN_HUMAN_INTERACTION;
    if (module == "sandbox_runtime") return write ? RPC_RASN_SANDBOX_RUNTIME_WRITE : RPC_RASN_SANDBOX_RUNTIME;
    return write ? RPC_RASN_AGENT_CONTROL_WRITE : RPC_RASN_AGENT_CONTROL;
}

::dsn::task_code lpc_code_for_module(const std::string &module)
{
    if (module == "agent_control_plane") return LPC_RASN_AGENT_CONTROL;
    if (module == "agent_message_bus") return LPC_RASN_MESSAGE_BUS;
    if (module == "task_orchestration_kernel") return LPC_RASN_TASK_ORCHESTRATION;
    if (module == "determinism_ledger") return LPC_RASN_DETERMINISM_LEDGER;
    if (module == "capability_directory") return LPC_RASN_CAPABILITY_DIRECTORY;
    if (module == "resource_budget") return LPC_RASN_RESOURCE_BUDGET;
    if (module == "recovery_supervisor") return LPC_RASN_RECOVERY_SUPERVISOR;
    if (module == "blackboard") return LPC_RASN_BLACKBOARD;
    if (module == "contract_verifier") return LPC_RASN_CONTRACT_VERIFIER;
    if (module == "human_interaction") return LPC_RASN_HUMAN_INTERACTION;
    if (module == "sandbox_runtime") return LPC_RASN_SANDBOX_RUNTIME;
    return LPC_RASN_AGENT_CONTROL;
}

// Resolve a numeric [rasn.service] knob with a per-module override: the shared
// "rasn_runtime_<suffix>" value provides the default, and an optional
// "<module>_<suffix>" key overrides it so a module hosted on a slower or
// busier remote node can be tuned independently.
uint64_t rasn_runtime_scoped_uint64(const std::string &module,
                                     const std::string &suffix,
                                     uint64_t base_default,
                                     const std::string &description)
{
    const std::string module_key = module_service_key(module);
    const uint64_t common_value = config_service_uint64("rasn_runtime_" + suffix, base_default, description);
    return config_service_uint64(module_key + "_" + suffix, common_value, description);
}

std::chrono::milliseconds rasn_runtime_rpc_timeout(const std::string &module)
{
    const uint64_t base_default =
        ::dsn_config_get_value_uint64("rasn.rpc", "timeout_ms", 5000, "Default rASN RPC timeout in milliseconds");
    const uint64_t timeout_ms = rasn_runtime_scoped_uint64(
        module, "timeout_ms", base_default, "rASN runtime module RPC timeout in milliseconds");
    return std::chrono::milliseconds(timeout_ms == 0 ? base_default : timeout_ms);
}

std::chrono::milliseconds rasn_runtime_ping_timeout(const std::string &module)
{
    const uint64_t timeout_ms = rasn_runtime_scoped_uint64(
        module, "ping_timeout_ms", 1000, "rASN runtime module health ping timeout in milliseconds");
    return std::chrono::milliseconds(timeout_ms == 0 ? 1000 : timeout_ms);
}

uint32_t rasn_runtime_rpc_max_attempts(const std::string &module)
{
    const uint64_t retries =
        rasn_runtime_scoped_uint64(module, "retries", 2, "rASN runtime module RPC retries for transient errors");
    const uint64_t capped = retries > 16 ? 16 : retries;
    return static_cast<uint32_t>(capped) + 1;
}

uint64_t rasn_runtime_rpc_backoff_ms(const std::string &module)
{
    return rasn_runtime_scoped_uint64(
        module, "retry_backoff_ms", 50, "rASN runtime module RPC retry backoff in milliseconds");
}

bool is_retryable_rasn_runtime_error(::dsn::error_code code)
{
    return code == ::dsn::ERR_TIMEOUT || code == ::dsn::ERR_NETWORK_FAILURE ||
           code == ::dsn::ERR_NETWORK_INIT_FAILED || code == ::dsn::ERR_BUSY ||
           code == ::dsn::ERR_CAPACITY_EXCEEDED || code == ::dsn::ERR_TRY_AGAIN;
}

typedef std::map<std::string, std::vector<std::string>> field_map;

void append_field(std::ostringstream &output, const std::string &key, const std::string &value)
{
    output << key << "=" << value.size() << ":" << value << "\n";
}

std::string encode_fields(const std::vector<std::pair<std::string, std::string>> &fields)
{
    std::ostringstream output;
    for (const std::pair<std::string, std::string> &field : fields)
    {
        append_field(output, field.first, field.second);
    }
    return output.str();
}

bool parse_size_text(const std::string &text, size_t *value)
{
    if (value == nullptr || text.empty())
    {
        return false;
    }
    size_t result = 0;
    for (const char ch : text)
    {
        if (ch < '0' || ch > '9')
        {
            return false;
        }
        const size_t digit = static_cast<size_t>(ch - '0');
        if (result > ((std::numeric_limits<size_t>::max)() - digit) / 10)
        {
            return false;
        }
        result = result * 10 + digit;
    }
    *value = result;
    return true;
}

bool decode_fields(const std::string &payload, field_map *fields, std::string *error)
{
    if (fields == nullptr)
    {
        if (error != nullptr)
        {
            *error = "field map output is null";
        }
        return false;
    }
    fields->clear();
    size_t offset = 0;
    while (offset < payload.size())
    {
        const size_t equal = payload.find('=', offset);
        if (equal == std::string::npos)
        {
            if (error != nullptr)
            {
                *error = "malformed runtime module payload: missing '='";
            }
            return false;
        }
        const size_t colon = payload.find(':', equal + 1);
        if (colon == std::string::npos)
        {
            if (error != nullptr)
            {
                *error = "malformed runtime module payload: missing ':'";
            }
            return false;
        }
        const std::string key = payload.substr(offset, equal - offset);
        size_t length = 0;
        if (!parse_size_text(payload.substr(equal + 1, colon - equal - 1), &length))
        {
            if (error != nullptr)
            {
                *error = "malformed runtime module payload: invalid length";
            }
            return false;
        }
        const size_t value_begin = colon + 1;
        if (value_begin > payload.size() || length > payload.size() - value_begin)
        {
            if (error != nullptr)
            {
                *error = "malformed runtime module payload: truncated value";
            }
            return false;
        }
        (*fields)[key].push_back(payload.substr(value_begin, length));
        offset = value_begin + length;
        if (offset < payload.size())
        {
            if (payload[offset] != '\n')
            {
                if (error != nullptr)
                {
                    *error = "malformed runtime module payload: missing field terminator";
                }
                return false;
            }
            ++offset;
        }
    }
    if (error != nullptr)
    {
        error->clear();
    }
    return true;
}

std::vector<std::string> field_values(const field_map &fields, const std::string &key)
{
    const field_map::const_iterator it = fields.find(key);
    return it == fields.end() ? std::vector<std::string>() : it->second;
}

std::string field_string(const field_map &fields, const std::string &key, const std::string &fallback = "")
{
    const field_map::const_iterator it = fields.find(key);
    return it == fields.end() || it->second.empty() ? fallback : it->second.front();
}

bool field_bool(const field_map &fields, const std::string &key, bool fallback = false)
{
    const std::string value = lower_ascii(field_string(fields, key, fallback ? "true" : "false"));
    return value == "true" || value == "1" || value == "yes";
}

uint64_t field_uint64(const field_map &fields, const std::string &key, uint64_t fallback = 0)
{
    const std::string value = field_string(fields, key, "");
    if (value.empty())
    {
        return fallback;
    }
    uint64_t result = 0;
    for (const char ch : value)
    {
        if (ch < '0' || ch > '9')
        {
            return fallback;
        }
        const uint64_t digit = static_cast<uint64_t>(ch - '0');
        if (result > ((std::numeric_limits<uint64_t>::max)() - digit) / 10)
        {
            return fallback;
        }
        result = result * 10 + digit;
    }
    return result;
}

uint32_t field_uint32(const field_map &fields, const std::string &key, uint32_t fallback = 0)
{
    const uint64_t value = field_uint64(fields, key, fallback);
    return value > (std::numeric_limits<uint32_t>::max)() ? fallback : static_cast<uint32_t>(value);
}

size_t field_size(const field_map &fields, const std::string &key, size_t fallback = 0)
{
    const uint64_t value = field_uint64(fields, key, static_cast<uint64_t>(fallback));
    return value > (std::numeric_limits<size_t>::max)() ? fallback : static_cast<size_t>(value);
}

bool parse_payload(const std::string &payload, field_map *fields, std::string *error)
{
    if (!decode_fields(payload, fields, error))
    {
        return false;
    }
    return true;
}

struct runtime_state_watermark
{
    std::string module;
    std::string state_prefix;
    uint64_t last_record_sequence = 0;
    uint64_t last_state_sequence = 0;
    uint64_t updated_at_ms = 0;
};

std::string rasn_runtime_watermark_key(const std::string &state_prefix, const std::string &module)
{
    return state_prefix + "/" + module + "/_meta/watermark";
}

std::string encode_runtime_state_watermark_payload(const runtime_state_watermark &watermark)
{
    return encode_fields({{"schema_version", std::to_string(RASN_AGENT_SCHEMA_VERSION)},
                          {"module", watermark.module},
                          {"state_prefix", watermark.state_prefix},
                          {"last_record_sequence", std::to_string(watermark.last_record_sequence)},
                          {"last_state_sequence", std::to_string(watermark.last_state_sequence)},
                          {"updated_at_ms", std::to_string(watermark.updated_at_ms)}});
}

bool decode_runtime_state_watermark_payload(const std::string &payload,
                                            runtime_state_watermark *watermark,
                                            std::string *error)
{
    if (watermark == nullptr)
    {
        if (error != nullptr)
        {
            *error = "runtime state watermark output is null";
        }
        return false;
    }
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    watermark->module = field_string(fields, "module");
    watermark->state_prefix = field_string(fields, "state_prefix");
    watermark->last_record_sequence = field_uint64(fields, "last_record_sequence");
    watermark->last_state_sequence = field_uint64(fields, "last_state_sequence");
    watermark->updated_at_ms = field_uint64(fields, "updated_at_ms");
    if (watermark->module.empty() || watermark->last_record_sequence == 0)
    {
        if (error != nullptr)
        {
            *error = "runtime state watermark missing module or last record sequence";
        }
        return false;
    }
    if (watermark->last_state_sequence != 0 && watermark->last_state_sequence < watermark->last_record_sequence)
    {
        if (error != nullptr)
        {
            *error = "runtime state watermark state sequence is older than record sequence";
        }
        return false;
    }
    if (error != nullptr)
    {
        error->clear();
    }
    return true;
}

state_record make_runtime_state_record(const std::string &state_prefix,
                                       const std::string &module,
                                       const std::string &kind,
                                       const std::string &key,
                                       const std::string &value)
{
    state_record record;
    record.key = rasn_runtime_state_key(state_prefix, module, kind, key);
    record.kind = "rasn.runtime." + module + "." + kind;
    record.scope = "rasn.runtime";
    record.value = value;
    return record;
}

std::string replicated_runtime_key_component(const std::string &value)
{
    std::ostringstream output;
    output << value.size() << '-';
    for (const unsigned char c : value)
    {
        output << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(c);
    }
    return output.str();
}

state_record make_replicated_runtime_state_record(const std::string &module,
                                                  const std::string &kind,
                                                  const std::string &key,
                                                  const std::string &value)
{
    state_record record = make_runtime_state_record("replica", module, kind, key, value);
    record.key = "replica/" + replicated_runtime_key_component(module) + "/" +
                 replicated_runtime_key_component(kind) + "/" +
                 replicated_runtime_key_component(key);
    return record;
}

bool put_runtime_state_mirror(rasn_service_graph &services,
                              const std::string &state_prefix,
                              const std::string &module,
                              const std::string &kind,
                              const std::string &key,
                              const std::string &value,
                              std::string *error)
{
    const state_response response = services.put_state(make_runtime_state_record(state_prefix, module, kind, key, value));
    if (!response.ok)
    {
        if (error != nullptr)
        {
            *error = response.error.empty() ? "failed to write rASN runtime state" : response.error;
        }
        return false;
    }
    if (!rasn_runtime_state_watermark_enabled())
    {
        if (error != nullptr)
        {
            error->clear();
        }
        return true;
    }

    runtime_state_watermark watermark;
    watermark.module = module;
    watermark.state_prefix = state_prefix;
    watermark.last_record_sequence = response.record.sequence;
    watermark.last_state_sequence = response.last_sequence;
    watermark.updated_at_ms = ::dsn_now_ms();

    state_record watermark_record;
    watermark_record.key = rasn_runtime_watermark_key(state_prefix, module);
    watermark_record.kind = "rasn.runtime." + module + ".watermark";
    watermark_record.scope = "rasn.runtime";
    watermark_record.value = encode_runtime_state_watermark_payload(watermark);
    const state_response watermark_response = services.put_state(watermark_record);
    if (!watermark_response.ok)
    {
        if (error != nullptr)
        {
            *error = watermark_response.error.empty() ? "failed to write rASN runtime state watermark"
                                                     : watermark_response.error;
        }
        return false;
    }
    if (error != nullptr)
    {
        error->clear();
    }
    return true;
}

bool verify_runtime_state_watermarks(const std::vector<state_record> &records,
                                     const std::vector<std::string> &hosted_modules,
                                     const std::string &state_prefix,
                                     std::string *error)
{
    std::map<std::string, uint64_t> max_record_sequence;
    std::map<std::string, runtime_state_watermark> watermarks;
    std::set<std::string> modules_with_data;
    for (const state_record &record : records)
    {
        std::string module;
        std::string kind;
        if (!parse_rasn_runtime_record_kind(record.kind, &module, &kind) || !has_module(hosted_modules, module))
        {
            continue;
        }
        if (kind == "watermark")
        {
            runtime_state_watermark watermark;
            std::string decode_error;
            if (!decode_runtime_state_watermark_payload(record.value, &watermark, &decode_error))
            {
                if (error != nullptr)
                {
                    *error = "invalid runtime state watermark for " + module + ": " + decode_error;
                }
                return false;
            }
            if (watermark.module != module)
            {
                if (error != nullptr)
                {
                    *error = "runtime state watermark module mismatch: key module " + module + " payload module " +
                             watermark.module;
                }
                return false;
            }
            if (!watermark.state_prefix.empty() && watermark.state_prefix != state_prefix)
            {
                if (error != nullptr)
                {
                    *error = "runtime state watermark prefix mismatch for " + module;
                }
                return false;
            }
            const std::map<std::string, runtime_state_watermark>::const_iterator existing = watermarks.find(module);
            if (existing == watermarks.end() ||
                watermark.last_record_sequence > existing->second.last_record_sequence)
            {
                watermarks[module] = watermark;
            }
            continue;
        }
        if (!runtime_hydration_supported(module, kind))
        {
            continue;
        }
        uint64_t &max_sequence = max_record_sequence[module];
        max_sequence = (std::max)(max_sequence, record.sequence);
        modules_with_data.insert(module);
    }

    for (const std::string &module : modules_with_data)
    {
        if (watermarks.count(module) == 0)
        {
            if (error != nullptr)
            {
                *error = "missing runtime state watermark for module " + module;
            }
            return false;
        }
    }

    for (const std::map<std::string, runtime_state_watermark>::value_type &entry : watermarks)
    {
        const uint64_t max_sequence = max_record_sequence[entry.first];
        if (max_sequence < entry.second.last_record_sequence)
        {
            if (error != nullptr)
            {
                *error = "runtime state mirror for " + entry.first + " is behind watermark " +
                         std::to_string(entry.second.last_record_sequence) + " (max record sequence " +
                         std::to_string(max_sequence) + ")";
            }
            return false;
        }
    }
    if (error != nullptr)
    {
        error->clear();
    }
    return true;
}

std::string encode_capability_payload(const agent_capability &capability)
{
    return encode_fields({{"schema_version", std::to_string(capability.schema_version)},
                          {"name", capability.name},
                          {"input_type", capability.input_type},
                          {"output_type", capability.output_type},
                          {"side_effect_class", capability.side_effect_class},
                          {"cost_hint", std::to_string(capability.cost_hint)},
                          {"latency_hint_ms", std::to_string(capability.latency_hint_ms)},
                          {"reliability_hint", std::to_string(capability.reliability_hint)}});
}

bool decode_capability_payload(const std::string &payload, agent_capability *capability, std::string *error)
{
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    capability->schema_version = field_uint32(fields, "schema_version", RASN_AGENT_SCHEMA_VERSION);
    capability->name = field_string(fields, "name");
    capability->input_type = field_string(fields, "input_type");
    capability->output_type = field_string(fields, "output_type");
    capability->side_effect_class = field_string(fields, "side_effect_class");
    capability->cost_hint = field_uint32(fields, "cost_hint");
    capability->latency_hint_ms = field_uint32(fields, "latency_hint_ms");
    capability->reliability_hint = field_uint32(fields, "reliability_hint");
    return true;
}

std::string encode_descriptor_payload(const agent_descriptor &descriptor)
{
    std::vector<std::pair<std::string, std::string>> fields;
    fields.push_back({"schema_version", std::to_string(descriptor.schema_version)});
    fields.push_back({"agent_id", descriptor.agent_id});
    fields.push_back({"role", descriptor.role});
    fields.push_back({"app_name", descriptor.app_name});
    fields.push_back({"host", descriptor.host});
    fields.push_back({"port", std::to_string(descriptor.port)});
    fields.push_back({"endpoint_uri", descriptor.endpoint_uri});
    fields.push_back({"version", descriptor.version});
    fields.push_back({"health", descriptor.health});
    for (const agent_capability &capability : descriptor.capabilities)
    {
        fields.push_back({"capability", encode_capability_payload(capability)});
    }
    return encode_fields(fields);
}

bool decode_descriptor_payload(const std::string &payload, agent_descriptor *descriptor, std::string *error)
{
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    descriptor->schema_version = field_uint32(fields, "schema_version", RASN_AGENT_SCHEMA_VERSION);
    descriptor->agent_id = field_string(fields, "agent_id");
    descriptor->role = field_string(fields, "role");
    descriptor->app_name = field_string(fields, "app_name");
    descriptor->host = field_string(fields, "host");
    descriptor->port = field_uint32(fields, "port");
    descriptor->endpoint_uri = field_string(fields, "endpoint_uri");
    descriptor->version = field_string(fields, "version");
    descriptor->health = field_string(fields, "health");
    descriptor->capabilities.clear();
    const std::vector<std::string> capabilities = field_values(fields, "capability");
    for (const std::string &encoded : capabilities)
    {
        agent_capability capability;
        if (!decode_capability_payload(encoded, &capability, error))
        {
            return false;
        }
        descriptor->capabilities.push_back(capability);
    }
    return true;
}

std::string encode_agent_control_payload(const agent_control_record &record)
{
    return encode_fields({{"descriptor", encode_descriptor_payload(record.descriptor)},
                          {"state", record.state},
                          {"placement", record.placement},
                          {"owner", record.owner},
                          {"restart_policy", record.restart_policy},
                          {"last_error", record.last_error},
                          {"generation", std::to_string(record.generation)},
                          {"last_heartbeat_ms", std::to_string(record.last_heartbeat_ms)},
                          {"lease_expires_ms", std::to_string(record.lease_expires_ms)}});
}

bool decode_agent_control_payload(const std::string &payload, agent_control_record *record, std::string *error)
{
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    if (!decode_descriptor_payload(field_string(fields, "descriptor"), &record->descriptor, error))
    {
        return false;
    }
    record->state = field_string(fields, "state", "starting");
    record->placement = field_string(fields, "placement");
    record->owner = field_string(fields, "owner");
    record->restart_policy = field_string(fields, "restart_policy", "never");
    record->last_error = field_string(fields, "last_error");
    record->generation = field_uint64(fields, "generation");
    record->last_heartbeat_ms = field_uint64(fields, "last_heartbeat_ms");
    record->lease_expires_ms = field_uint64(fields, "lease_expires_ms");
    return true;
}

std::string encode_lease_payload(const agent_control_lease &lease)
{
    return encode_fields({{"ok", lease.ok ? "true" : "false"},
                          {"agent_id", lease.agent_id},
                          {"owner", lease.owner},
                          {"generation", std::to_string(lease.generation)},
                          {"expires_ms", std::to_string(lease.expires_ms)},
                          {"error", lease.error}});
}

bool decode_lease_payload(const std::string &payload, agent_control_lease *lease, std::string *error)
{
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    lease->ok = field_bool(fields, "ok");
    lease->agent_id = field_string(fields, "agent_id");
    lease->owner = field_string(fields, "owner");
    lease->generation = field_uint64(fields, "generation");
    lease->expires_ms = field_uint64(fields, "expires_ms");
    lease->error = field_string(fields, "error");
    return true;
}

std::string encode_message_payload(const agent_message &message)
{
    return encode_fields({{"message_id", message.message_id},
                          {"correlation_id", message.correlation_id},
                          {"sender", message.sender},
                          {"receiver", message.receiver},
                          {"type", message.type},
                          {"payload", message.payload},
                          {"state", message.state},
                          {"error", message.error},
                          {"attempt", std::to_string(message.attempt)},
                          {"deadline_ms", std::to_string(message.deadline_ms)},
                          {"available_at_ms", std::to_string(message.available_at_ms)},
                          {"created_at_ms", std::to_string(message.created_at_ms)},
                          {"updated_at_ms", std::to_string(message.updated_at_ms)}});
}

bool decode_message_payload(const std::string &payload, agent_message *message, std::string *error)
{
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    message->message_id = field_string(fields, "message_id");
    message->correlation_id = field_string(fields, "correlation_id");
    message->sender = field_string(fields, "sender");
    message->receiver = field_string(fields, "receiver");
    message->type = field_string(fields, "type");
    message->payload = field_string(fields, "payload");
    message->state = field_string(fields, "state", "queued");
    message->error = field_string(fields, "error");
    message->attempt = field_uint32(fields, "attempt");
    message->deadline_ms = field_uint64(fields, "deadline_ms");
    message->available_at_ms = field_uint64(fields, "available_at_ms");
    message->created_at_ms = field_uint64(fields, "created_at_ms");
    message->updated_at_ms = field_uint64(fields, "updated_at_ms");
    return true;
}

std::string encode_task_payload(const orchestration_task &task)
{
    std::vector<std::pair<std::string, std::string>> fields;
    fields.push_back({"task_id", task.task_id});
    fields.push_back({"parent_task_id", task.parent_task_id});
    fields.push_back({"owner_agent", task.owner_agent});
    fields.push_back({"state", task.state});
    fields.push_back({"input", task.input});
    fields.push_back({"output", task.output});
    fields.push_back({"error", task.error});
    fields.push_back({"compensation", task.compensation});
    fields.push_back({"deadline_ms", std::to_string(task.deadline_ms)});
    fields.push_back({"generation", std::to_string(task.generation)});
    for (const std::string &dependency : task.depends_on)
    {
        fields.push_back({"depends_on", dependency});
    }
    return encode_fields(fields);
}

bool decode_task_payload(const std::string &payload, orchestration_task *task, std::string *error)
{
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    task->task_id = field_string(fields, "task_id");
    task->parent_task_id = field_string(fields, "parent_task_id");
    task->owner_agent = field_string(fields, "owner_agent");
    task->state = field_string(fields, "state", "pending");
    task->input = field_string(fields, "input");
    task->output = field_string(fields, "output");
    task->error = field_string(fields, "error");
    task->compensation = field_string(fields, "compensation");
    task->deadline_ms = field_uint64(fields, "deadline_ms");
    task->generation = field_uint64(fields, "generation");
    task->depends_on = field_values(fields, "depends_on");
    return true;
}

std::string encode_choice_payload(const deterministic_choice &choice)
{
    return encode_fields({{"sequence", std::to_string(choice.sequence)},
                          {"task_id", choice.task_id},
                          {"key", choice.key},
                          {"source", choice.source},
                          {"value", choice.value}});
}

bool decode_choice_payload(const std::string &payload, deterministic_choice *choice, std::string *error)
{
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    choice->sequence = field_uint64(fields, "sequence");
    choice->task_id = field_string(fields, "task_id");
    choice->key = field_string(fields, "key");
    choice->source = field_string(fields, "source");
    choice->value = field_string(fields, "value");
    return true;
}

std::string encode_capability_provider_payload(const capability_provider &provider)
{
    std::vector<std::pair<std::string, std::string>> fields;
    fields.push_back({"descriptor", encode_descriptor_payload(provider.descriptor)});
    fields.push_back({"state", provider.state});
    fields.push_back({"placement", provider.placement});
    fields.push_back({"load", std::to_string(provider.load)});
    fields.push_back({"last_seen_ms", std::to_string(provider.last_seen_ms)});
    for (const std::string &label : provider.labels)
    {
        fields.push_back({"label", label});
    }
    return encode_fields(fields);
}

bool decode_capability_provider_payload(const std::string &payload, capability_provider *provider, std::string *error)
{
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    if (!decode_descriptor_payload(field_string(fields, "descriptor"), &provider->descriptor, error))
    {
        return false;
    }
    provider->state = field_string(fields, "state", "running");
    provider->placement = field_string(fields, "placement");
    provider->load = field_uint32(fields, "load");
    provider->last_seen_ms = field_uint64(fields, "last_seen_ms");
    provider->labels = field_values(fields, "label");
    return true;
}

std::string encode_quota_payload(const resource_quota &quota)
{
    return encode_fields({{"scope", quota.scope},
                          {"max_cost_units", std::to_string(quota.max_cost_units)},
                          {"max_latency_ms", std::to_string(quota.max_latency_ms)},
                          {"max_tokens", std::to_string(quota.max_tokens)},
                          {"max_tool_calls", std::to_string(quota.max_tool_calls)}});
}

bool decode_quota_payload(const std::string &payload, resource_quota *quota, std::string *error)
{
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    quota->scope = field_string(fields, "scope");
    quota->max_cost_units = field_uint64(fields, "max_cost_units");
    quota->max_latency_ms = field_uint64(fields, "max_latency_ms");
    quota->max_tokens = field_uint64(fields, "max_tokens");
    quota->max_tool_calls = field_uint64(fields, "max_tool_calls");
    return true;
}

std::string encode_request_payload(const resource_request &request)
{
    return encode_fields({{"scope", request.scope},
                          {"cost_units", std::to_string(request.cost_units)},
                          {"latency_ms", std::to_string(request.latency_ms)},
                          {"tokens", std::to_string(request.tokens)},
                          {"tool_calls", std::to_string(request.tool_calls)},
                          {"reason", request.reason}});
}

bool decode_request_payload(const std::string &payload, resource_request *request, std::string *error)
{
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    request->scope = field_string(fields, "scope");
    request->cost_units = field_uint64(fields, "cost_units");
    request->latency_ms = field_uint64(fields, "latency_ms");
    request->tokens = field_uint64(fields, "tokens");
    request->tool_calls = field_uint64(fields, "tool_calls");
    request->reason = field_string(fields, "reason");
    return true;
}

std::string encode_usage_payload(const resource_usage &usage)
{
    return encode_fields({{"scope", usage.scope},
                          {"cost_units", std::to_string(usage.cost_units)},
                          {"latency_ms", std::to_string(usage.latency_ms)},
                          {"tokens", std::to_string(usage.tokens)},
                          {"tool_calls", std::to_string(usage.tool_calls)}});
}

bool decode_usage_payload(const std::string &payload, resource_usage *usage, std::string *error)
{
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    usage->scope = field_string(fields, "scope");
    usage->cost_units = field_uint64(fields, "cost_units");
    usage->latency_ms = field_uint64(fields, "latency_ms");
    usage->tokens = field_uint64(fields, "tokens");
    usage->tool_calls = field_uint64(fields, "tool_calls");
    return true;
}

std::string encode_decision_payload(const resource_budget_decision &decision)
{
    return encode_fields({{"allowed", decision.allowed ? "true" : "false"},
                          {"scope", decision.scope},
                          {"reason", decision.reason},
                          {"usage_after", encode_usage_payload(decision.usage_after)},
                          {"quota", encode_quota_payload(decision.quota)}});
}

bool decode_decision_payload(const std::string &payload, resource_budget_decision *decision, std::string *error)
{
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    decision->allowed = field_bool(fields, "allowed");
    decision->scope = field_string(fields, "scope");
    decision->reason = field_string(fields, "reason");
    if (!decode_usage_payload(field_string(fields, "usage_after"), &decision->usage_after, error))
    {
        return false;
    }
    if (!decode_quota_payload(field_string(fields, "quota"), &decision->quota, error))
    {
        return false;
    }
    return true;
}

std::string encode_recovery_policy_payload(const recovery_policy &policy)
{
    return encode_fields({{"failure_class", policy.failure_class},
                          {"max_attempts", std::to_string(policy.max_attempts)},
                          {"retry_delay_ms", std::to_string(policy.retry_delay_ms)},
                          {"escalate_after_attempts", std::to_string(policy.escalate_after_attempts)},
                          {"retryable", policy.retryable ? "true" : "false"},
                          {"compensation", policy.compensation}});
}

bool decode_recovery_policy_payload(const std::string &payload, recovery_policy *policy, std::string *error)
{
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    policy->failure_class = field_string(fields, "failure_class");
    policy->max_attempts = field_uint32(fields, "max_attempts", 1);
    policy->retry_delay_ms = field_uint64(fields, "retry_delay_ms");
    policy->escalate_after_attempts = field_uint32(fields, "escalate_after_attempts");
    policy->retryable = field_bool(fields, "retryable");
    policy->compensation = field_string(fields, "compensation");
    return true;
}

std::string encode_failure_payload(const failure_observation &failure)
{
    return encode_fields({{"task_id", failure.task_id},
                          {"component", failure.component},
                          {"failure_class", failure.failure_class},
                          {"code", failure.code},
                          {"message", failure.message},
                          {"attempt", std::to_string(failure.attempt)},
                          {"retryable", failure.retryable ? "true" : "false"},
                          {"time_ms", std::to_string(failure.time_ms)}});
}

bool decode_failure_payload(const std::string &payload, failure_observation *failure, std::string *error)
{
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    failure->task_id = field_string(fields, "task_id");
    failure->component = field_string(fields, "component");
    failure->failure_class = field_string(fields, "failure_class");
    failure->code = field_string(fields, "code");
    failure->message = field_string(fields, "message");
    failure->attempt = field_uint32(fields, "attempt");
    failure->retryable = field_bool(fields, "retryable");
    failure->time_ms = field_uint64(fields, "time_ms");
    return true;
}

std::string encode_recovery_action_payload(const recovery_action &action)
{
    std::vector<std::pair<std::string, std::string>> fields;
    fields.push_back({"handled", action.handled ? "true" : "false"});
    fields.push_back({"action", action.action});
    fields.push_back({"delay_ms", std::to_string(action.delay_ms)});
    fields.push_back({"reason", action.reason});
    for (const std::string &label : action.labels)
    {
        fields.push_back({"label", label});
    }
    return encode_fields(fields);
}

bool decode_recovery_action_payload(const std::string &payload, recovery_action *action, std::string *error)
{
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    action->handled = field_bool(fields, "handled");
    action->action = field_string(fields, "action");
    action->delay_ms = field_uint64(fields, "delay_ms");
    action->reason = field_string(fields, "reason");
    action->labels = field_values(fields, "label");
    return true;
}

std::string encode_blackboard_payload(const blackboard_entry &entry)
{
    std::vector<std::pair<std::string, std::string>> fields;
    fields.push_back({"key", entry.key});
    fields.push_back({"kind", entry.kind});
    fields.push_back({"owner", entry.owner});
    fields.push_back({"value", entry.value});
    fields.push_back({"generation", std::to_string(entry.generation)});
    fields.push_back({"created_at_ms", std::to_string(entry.created_at_ms)});
    fields.push_back({"updated_at_ms", std::to_string(entry.updated_at_ms)});
    fields.push_back({"expires_at_ms", std::to_string(entry.expires_at_ms)});
    for (const std::string &tag : entry.tags)
    {
        fields.push_back({"tag", tag});
    }
    return encode_fields(fields);
}

bool decode_blackboard_payload(const std::string &payload, blackboard_entry *entry, std::string *error)
{
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    entry->key = field_string(fields, "key");
    entry->kind = field_string(fields, "kind");
    entry->owner = field_string(fields, "owner");
    entry->value = field_string(fields, "value");
    entry->generation = field_uint64(fields, "generation");
    entry->created_at_ms = field_uint64(fields, "created_at_ms");
    entry->updated_at_ms = field_uint64(fields, "updated_at_ms");
    entry->expires_at_ms = field_uint64(fields, "expires_at_ms");
    entry->tags = field_values(fields, "tag");
    return true;
}

std::string encode_contract_payload(const agent_contract &contract)
{
    std::vector<std::pair<std::string, std::string>> fields;
    fields.push_back({"contract_id", contract.contract_id});
    fields.push_back({"require_input_non_empty", contract.require_input_non_empty ? "true" : "false"});
    fields.push_back({"require_output_non_empty", contract.require_output_non_empty ? "true" : "false"});
    fields.push_back({"max_output_bytes", std::to_string(contract.max_output_bytes)});
    for (const std::string &fragment : contract.required_input_fragments) fields.push_back({"required_input", fragment});
    for (const std::string &fragment : contract.required_output_fragments) fields.push_back({"required_output", fragment});
    for (const std::string &fragment : contract.forbidden_output_fragments) fields.push_back({"forbidden_output", fragment});
    for (const std::string &label : contract.required_policy_labels) fields.push_back({"required_label", label});
    return encode_fields(fields);
}

bool decode_contract_payload(const std::string &payload, agent_contract *contract, std::string *error)
{
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    contract->contract_id = field_string(fields, "contract_id");
    contract->require_input_non_empty = field_bool(fields, "require_input_non_empty");
    contract->require_output_non_empty = field_bool(fields, "require_output_non_empty", true);
    contract->max_output_bytes = field_size(fields, "max_output_bytes");
    contract->required_input_fragments = field_values(fields, "required_input");
    contract->required_output_fragments = field_values(fields, "required_output");
    contract->forbidden_output_fragments = field_values(fields, "forbidden_output");
    contract->required_policy_labels = field_values(fields, "required_label");
    return true;
}

std::string encode_contract_evaluation_payload(const contract_evaluation &evaluation)
{
    std::vector<std::pair<std::string, std::string>> fields;
    fields.push_back({"ok", evaluation.ok ? "true" : "false"});
    fields.push_back({"contract_id", evaluation.contract_id});
    for (const std::string &violation : evaluation.violations) fields.push_back({"violation", violation});
    for (const std::string &warning : evaluation.warnings) fields.push_back({"warning", warning});
    return encode_fields(fields);
}

bool decode_contract_evaluation_payload(const std::string &payload, contract_evaluation *evaluation, std::string *error)
{
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    evaluation->ok = field_bool(fields, "ok", true);
    evaluation->contract_id = field_string(fields, "contract_id");
    evaluation->violations = field_values(fields, "violation");
    evaluation->warnings = field_values(fields, "warning");
    return true;
}

std::string encode_human_payload(const human_interaction_request &request)
{
    std::vector<std::pair<std::string, std::string>> fields;
    fields.push_back({"request_id", request.request_id});
    fields.push_back({"task_id", request.task_id});
    fields.push_back({"kind", request.kind});
    fields.push_back({"requester", request.requester});
    fields.push_back({"prompt", request.prompt});
    fields.push_back({"state", request.state});
    fields.push_back({"answer", request.answer});
    fields.push_back({"created_at_ms", std::to_string(request.created_at_ms)});
    fields.push_back({"updated_at_ms", std::to_string(request.updated_at_ms)});
    fields.push_back({"deadline_ms", std::to_string(request.deadline_ms)});
    for (const std::string &choice : request.choices) fields.push_back({"choice", choice});
    return encode_fields(fields);
}

bool decode_human_payload(const std::string &payload, human_interaction_request *request, std::string *error)
{
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    request->request_id = field_string(fields, "request_id");
    request->task_id = field_string(fields, "task_id");
    request->kind = field_string(fields, "kind", "approval");
    request->requester = field_string(fields, "requester");
    request->prompt = field_string(fields, "prompt");
    request->state = field_string(fields, "state", "pending");
    request->answer = field_string(fields, "answer");
    request->created_at_ms = field_uint64(fields, "created_at_ms");
    request->updated_at_ms = field_uint64(fields, "updated_at_ms");
    request->deadline_ms = field_uint64(fields, "deadline_ms");
    request->choices = field_values(fields, "choice");
    return true;
}

std::string encode_sandbox_profile_payload(const sandbox_profile &profile)
{
    std::vector<std::pair<std::string, std::string>> fields;
    fields.push_back({"name", profile.name});
    fields.push_back({"allow_filesystem_read", profile.allow_filesystem_read ? "true" : "false"});
    fields.push_back({"allow_filesystem_write", profile.allow_filesystem_write ? "true" : "false"});
    fields.push_back({"allow_network", profile.allow_network ? "true" : "false"});
    fields.push_back({"allow_process_spawn", profile.allow_process_spawn ? "true" : "false"});
    fields.push_back({"max_cpu_ms", std::to_string(profile.max_cpu_ms)});
    fields.push_back({"max_memory_bytes", std::to_string(profile.max_memory_bytes)});
    for (const std::string &value : profile.allowed_roots) fields.push_back({"allowed_root", value});
    for (const std::string &value : profile.denied_paths) fields.push_back({"denied_path", value});
    for (const std::string &value : profile.allowed_network_hosts) fields.push_back({"allowed_network_host", value});
    for (const std::string &value : profile.allowed_commands) fields.push_back({"allowed_command", value});
    return encode_fields(fields);
}

bool decode_sandbox_profile_payload(const std::string &payload, sandbox_profile *profile, std::string *error)
{
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    profile->name = field_string(fields, "name", "read-only");
    profile->allow_filesystem_read = field_bool(fields, "allow_filesystem_read", true);
    profile->allow_filesystem_write = field_bool(fields, "allow_filesystem_write");
    profile->allow_network = field_bool(fields, "allow_network");
    profile->allow_process_spawn = field_bool(fields, "allow_process_spawn");
    profile->max_cpu_ms = field_uint64(fields, "max_cpu_ms");
    profile->max_memory_bytes = field_uint64(fields, "max_memory_bytes");
    profile->allowed_roots = field_values(fields, "allowed_root");
    profile->denied_paths = field_values(fields, "denied_path");
    profile->allowed_network_hosts = field_values(fields, "allowed_network_host");
    profile->allowed_commands = field_values(fields, "allowed_command");
    return true;
}

std::string encode_sandbox_request_payload(const sandbox_request &request)
{
    return encode_fields({{"operation", request.operation},
                          {"path", request.path},
                          {"network_host", request.network_host},
                          {"command", request.command}});
}

bool decode_sandbox_request_payload(const std::string &payload, sandbox_request *request, std::string *error)
{
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    request->operation = field_string(fields, "operation");
    request->path = field_string(fields, "path");
    request->network_host = field_string(fields, "network_host");
    request->command = field_string(fields, "command");
    return true;
}

std::string encode_sandbox_decision_payload(const sandbox_decision &decision)
{
    return encode_fields({{"allowed", decision.allowed ? "true" : "false"},
                          {"profile", decision.profile},
                          {"reason", decision.reason},
                          {"max_cpu_ms", std::to_string(decision.max_cpu_ms)},
                          {"max_memory_bytes", std::to_string(decision.max_memory_bytes)}});
}

bool decode_sandbox_decision_payload(const std::string &payload, sandbox_decision *decision, std::string *error)
{
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    decision->allowed = field_bool(fields, "allowed");
    decision->profile = field_string(fields, "profile");
    decision->reason = field_string(fields, "reason");
    decision->max_cpu_ms = field_uint64(fields, "max_cpu_ms");
    decision->max_memory_bytes = field_uint64(fields, "max_memory_bytes");
    return true;
}

template <typename T, typename Encoder>
std::string encode_items(const std::vector<T> &items, Encoder encoder)
{
    std::vector<std::pair<std::string, std::string>> fields;
    for (const T &item : items)
    {
        fields.push_back({"item", encoder(item)});
    }
    return encode_fields(fields);
}

template <typename T, typename Decoder>
bool decode_items(const std::string &payload, std::vector<T> *items, Decoder decoder, std::string *error)
{
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    items->clear();
    const std::vector<std::string> encoded_items = field_values(fields, "item");
    for (const std::string &encoded : encoded_items)
    {
        T item;
        if (!decoder(encoded, &item, error))
        {
            return false;
        }
        items->push_back(item);
    }
    return true;
}

const char kTypedRuntimeStatePrefix[] = "rasn.thrift.v1:";

template <typename Domain>
std::string encode_typed_runtime_state(const Domain &value)
{
    return std::string(kTypedRuntimeStatePrefix) +
           serialize_runtime_rpc_value(to_wire(value));
}

template <typename Wire, typename Domain, typename LegacyDecoder>
bool decode_runtime_state(const std::string &encoded,
                          Domain *value,
                          std::string *error,
                          LegacyDecoder legacy_decoder)
{
    const size_t prefix_size = sizeof(kTypedRuntimeStatePrefix) - 1;
    if (encoded.compare(0, prefix_size, kTypedRuntimeStatePrefix) != 0)
    {
        return legacy_decoder(encoded, value, error);
    }
    Wire wire;
    if (!deserialize_runtime_rpc_value(encoded.substr(prefix_size), &wire, error))
    {
        return false;
    }
    return from_wire(wire, value, error);
}

class rasn_runtime_service_store
{
public:
    explicit rasn_runtime_service_store(std::string allowed_module = std::string())
        : _allowed_module(std::move(allowed_module))
    {
    }

#define RASN_STORE_DISPATCH(request_type, response_type)                                                \
    ::dsn::rasn::rpc::response_type dispatch(                                                           \
        const ::dsn::rasn::rpc::request_type &request, bool local_dedup = true)                         \
    {                                                                                                   \
        return dispatch_typed<::dsn::rasn::rpc::request_type, ::dsn::rasn::rpc::response_type>(         \
            request, local_dedup);                                                                      \
    }

    RASN_STORE_DISPATCH(agent_control_request, agent_control_response)
    RASN_STORE_DISPATCH(message_bus_request, message_bus_response)
    RASN_STORE_DISPATCH(task_orchestration_request, task_orchestration_response)
    RASN_STORE_DISPATCH(determinism_request, determinism_response)
    RASN_STORE_DISPATCH(capability_directory_request, capability_directory_response)
    RASN_STORE_DISPATCH(resource_budget_request, resource_budget_response)
    RASN_STORE_DISPATCH(recovery_supervisor_request, recovery_supervisor_response)
    RASN_STORE_DISPATCH(blackboard_request, blackboard_response)
    RASN_STORE_DISPATCH(contract_verifier_request, contract_verifier_response)
    RASN_STORE_DISPATCH(human_interaction_rpc_request, human_interaction_rpc_response)
    RASN_STORE_DISPATCH(sandbox_runtime_request, sandbox_runtime_response)

#undef RASN_STORE_DISPATCH

    std::vector<state_record> checkpoint_records(const std::string &module) const
    {
        std::vector<state_record> records;
        if (module == "agent_control_plane")
        {
            for (const agent_control_record &record : _agent_control.list())
            {
                records.push_back(make_replicated_runtime_state_record(
                    module,
                    "agent",
                    record.descriptor.agent_id,
                    encode_typed_runtime_state(record)));
            }
        }
        else if (module == "agent_message_bus")
        {
            for (const agent_message &message : _message_bus.snapshot())
            {
                records.push_back(make_replicated_runtime_state_record(
                    module, "message", message.message_id, encode_typed_runtime_state(message)));
            }
        }
        else if (module == "task_orchestration_kernel")
        {
            for (const orchestration_task &task : _orchestration.snapshot())
            {
                records.push_back(
                    make_replicated_runtime_state_record(
                        module, "task", task.task_id, encode_typed_runtime_state(task)));
            }
        }
        else if (module == "determinism_ledger")
        {
            for (const deterministic_choice &choice : _determinism.snapshot())
            {
                records.push_back(make_replicated_runtime_state_record(
                    module,
                    "choice",
                    std::to_string(choice.sequence),
                    encode_typed_runtime_state(choice)));
            }
        }
        else if (module == "capability_directory")
        {
            for (const capability_provider &provider : _capabilities.snapshot())
            {
                records.push_back(make_replicated_runtime_state_record(
                    module,
                    "provider",
                    provider.descriptor.agent_id,
                    encode_typed_runtime_state(provider)));
            }
        }
        else if (module == "resource_budget")
        {
            for (const resource_quota &quota : _budgets.quota_snapshot())
            {
                records.push_back(
                    make_replicated_runtime_state_record(
                        module, "quota", quota.scope, encode_typed_runtime_state(quota)));
            }
            for (const resource_usage &usage : _budgets.snapshot())
            {
                records.push_back(
                    make_replicated_runtime_state_record(
                        module, "usage", usage.scope, encode_typed_runtime_state(usage)));
            }
        }
        else if (module == "recovery_supervisor")
        {
            for (const recovery_policy &policy : _recovery.policy_snapshot())
            {
                records.push_back(make_replicated_runtime_state_record(
                    module,
                    "policy",
                    policy.failure_class,
                    encode_typed_runtime_state(policy)));
            }
            size_t index = 0;
            for (const failure_observation &failure : _recovery.history())
            {
                records.push_back(make_replicated_runtime_state_record(
                    module,
                    "failure",
                    std::to_string(index++),
                    encode_typed_runtime_state(failure)));
            }
        }
        else if (module == "blackboard")
        {
            for (const blackboard_entry &entry : _blackboard.snapshot())
            {
                records.push_back(
                    make_replicated_runtime_state_record(
                        module, "entry", entry.key, encode_typed_runtime_state(entry)));
            }
        }
        else if (module == "contract_verifier")
        {
            for (const agent_contract &contract : _contracts.list_contracts())
            {
                records.push_back(make_replicated_runtime_state_record(
                    module,
                    "contract",
                    contract.contract_id,
                    encode_typed_runtime_state(contract)));
            }
        }
        else if (module == "human_interaction")
        {
            for (const human_interaction_request &request : _human.snapshot())
            {
                records.push_back(make_replicated_runtime_state_record(
                    module,
                    "request",
                    request.request_id,
                    encode_typed_runtime_state(request)));
            }
        }
        else if (module == "sandbox_runtime")
        {
            ::dsn::service::zauto_lock guard(_sandbox_lock);
            records.push_back(make_replicated_runtime_state_record(
                module,
                "profile",
                "default",
                encode_typed_runtime_state(_sandbox_profile)));
        }
        return records;
    }

    bool hydrate_from_state(const std::vector<state_record> &records,
                            const std::vector<std::string> &hosted_modules,
                            size_t *applied,
                            std::string *error,
                            const std::map<std::string, std::vector<uint32_t>> *hosted_shards = nullptr)
    {
        if (applied != nullptr)
        {
            *applied = 0;
        }
        if (error != nullptr)
        {
            error->clear();
        }

        std::vector<state_record> sorted = records;
        std::sort(sorted.begin(), sorted.end(), [](const state_record &left, const state_record &right) {
            if (left.sequence != right.sequence)
            {
                return left.sequence < right.sequence;
            }
            return left.key < right.key;
        });

        bool ok = true;
        for (const state_record &record : sorted)
        {
            std::string module;
            std::string kind;
            if (!parse_rasn_runtime_record_kind(record.kind, &module, &kind) || !has_module(hosted_modules, module))
            {
                continue;
            }
            if (!runtime_hydration_supported(module, kind))
            {
                continue;
            }

            const std::vector<uint32_t> *record_hosted_shards = nullptr;
            if (hosted_shards != nullptr)
            {
                const auto hosted = hosted_shards->find(module);
                if (hosted != hosted_shards->end())
                {
                    record_hosted_shards = &hosted->second;
                }
            }

            std::string hydrate_error;
            const hydration_record_result result =
                hydrate_record(module, kind, record, record_hosted_shards, &hydrate_error);
            if (result == hydration_record_result::skipped)
            {
                continue;
            }
            if (result == hydration_record_result::applied)
            {
                if (applied != nullptr)
                {
                    ++(*applied);
                }
            }
            else
            {
                ok = false;
                if (error != nullptr && error->empty())
                {
                    *error = hydrate_error.empty() ? "runtime hydration record failed" : hydrate_error;
                }
                dwarn("failed to hydrate runtime module %s record %s: %s",
                      module.c_str(),
                      record.key.c_str(),
                      hydrate_error.c_str());
            }
        }
        return ok;
    }

private:
    enum class hydration_record_result
    {
        applied,
        skipped,
        failed
    };

    template <typename Wire,
              typename Domain,
              typename LegacyDecoder,
              typename KeySelector,
              typename Apply>
    hydration_record_result decode_route_apply_sharded(
        const std::string &module,
        const state_record &record,
        const std::vector<uint32_t> *hosted_shards,
        std::string *error,
        LegacyDecoder legacy_decoder,
        KeySelector key_selector,
        Apply apply)
    {
        Domain value;
        if (!decode_runtime_state<Wire>(record.value, &value, error, legacy_decoder))
        {
            return hydration_record_result::failed;
        }
        if (hosted_shards != nullptr && !hosted_shards->empty())
        {
            const uint32_t partition =
                rasn_runtime_partition_for_key(module, key_selector(value));
            if (std::find(hosted_shards->begin(), hosted_shards->end(), partition) ==
                hosted_shards->end())
            {
                return hydration_record_result::skipped;
            }
        }
        return apply(value, error) ? hydration_record_result::applied
                                   : hydration_record_result::failed;
    }

    template <typename Request, typename Response>
    Response error_response(const Request &request,
                            ::dsn::rasn::rpc::runtime_error_code::type code,
                            const std::string &error,
                            bool retryable = false) const
    {
        Response response = make_runtime_response<Request, Response>(request);
        set_runtime_error(&response, code, error, retryable);
        return response;
    }

    template <typename Request, typename Response>
    Response dispatch_typed(const Request &request, bool local_dedup)
    {
        const std::string module = runtime_module_name(request);
        if (!_allowed_module.empty() && module != _allowed_module)
        {
            return error_response<Request, Response>(
                request,
                ::dsn::rasn::rpc::runtime_error_code::misrouted,
                "replica for " + _allowed_module + " cannot dispatch module " + module);
        }
        std::string validation_error;
        if (!validate_runtime_request(request, &validation_error))
        {
            const auto code = runtime_validation_error_code(validation_error);
            return error_response<Request, Response>(request, code, validation_error);
        }

        if (!local_dedup || !runtime_request_is_mutating(request))
        {
            return route_module_request(request);
        }

        std::string cached;
        std::string dedup_key_value;
        const dedup_begin_result dedup = begin_dedup(request, &cached, &dedup_key_value);
        if (dedup == dedup_begin_result::cached)
        {
            Response response;
            if (deserialize_runtime_rpc_value(cached, &response, &validation_error))
            {
                return response;
            }
            abort_dedup(dedup_key_value);
            return error_response<Request, Response>(
                request,
                ::dsn::rasn::rpc::runtime_error_code::internal,
                validation_error);
        }

        bool finished = false;
        dedup_completion_guard guard(
            this, dedup_key_value, dedup == dedup_begin_result::owner, &finished);
        Response response;
        try
        {
            response = route_module_request(request);
        }
        catch (const std::exception &ex)
        {
            response = error_response<Request, Response>(
                request,
                ::dsn::rasn::rpc::runtime_error_code::internal,
                std::string("runtime module dispatch threw: ") + ex.what());
        }
        catch (...)
        {
            response = error_response<Request, Response>(
                request,
                ::dsn::rasn::rpc::runtime_error_code::internal,
                "runtime module dispatch threw an unknown exception");
        }
        if (dedup == dedup_begin_result::owner)
        {
            finish_dedup(
                dedup_key_value, serialize_runtime_rpc_value(response), response.status.ok);
            finished = true;
        }
        return response;
    }

    hydration_record_result hydrate_record(const std::string &module,
                                           const std::string &kind,
                                           const state_record &record,
                                           const std::vector<uint32_t> *hosted_shards,
                                           std::string *error)
    {
        if (module == "agent_control_plane" && kind == "agent")
        {
            agent_control_record value;
            return decode_runtime_state<::dsn::rasn::rpc::wire_agent_control_record>(
                       record.value, &value, error, decode_agent_control_payload) &&
                           _agent_control.hydrate_agent(value, error)
                       ? hydration_record_result::applied
                       : hydration_record_result::failed;
        }
        if (module == "agent_message_bus" && kind == "message")
        {
            return decode_route_apply_sharded<::dsn::rasn::rpc::wire_agent_message,
                                              agent_message>(
                module,
                record,
                hosted_shards,
                error,
                decode_message_payload,
                [](const agent_message &value) { return value.message_id; },
                [this](const agent_message &value, std::string *apply_error) {
                    return _message_bus.hydrate_message(value, apply_error);
                });
        }
        if (module == "task_orchestration_kernel" && kind == "task")
        {
            orchestration_task value;
            return decode_runtime_state<::dsn::rasn::rpc::wire_orchestration_task>(
                       record.value, &value, error, decode_task_payload) &&
                           _orchestration.hydrate_task(value, error)
                       ? hydration_record_result::applied
                       : hydration_record_result::failed;
        }
        if (module == "determinism_ledger" && kind == "choice")
        {
            deterministic_choice value;
            return decode_runtime_state<::dsn::rasn::rpc::wire_deterministic_choice>(
                       record.value, &value, error, decode_choice_payload) &&
                           _determinism.hydrate_choice(value, error)
                       ? hydration_record_result::applied
                       : hydration_record_result::failed;
        }
        if (module == "capability_directory" && kind == "provider")
        {
            capability_provider value;
            return decode_runtime_state<::dsn::rasn::rpc::wire_capability_provider>(
                       record.value, &value, error, decode_capability_provider_payload) &&
                           _capabilities.upsert_provider(value, error)
                       ? hydration_record_result::applied
                       : hydration_record_result::failed;
        }
        if (module == "resource_budget" && kind == "quota")
        {
            return decode_route_apply_sharded<::dsn::rasn::rpc::wire_resource_quota,
                                              resource_quota>(
                module,
                record,
                hosted_shards,
                error,
                decode_quota_payload,
                [](const resource_quota &value) { return value.scope; },
                [this](const resource_quota &value, std::string *apply_error) {
                    return _budgets.configure(value, apply_error);
                });
        }
        if (module == "resource_budget" && kind == "usage")
        {
            return decode_route_apply_sharded<::dsn::rasn::rpc::wire_resource_usage,
                                              resource_usage>(
                module,
                record,
                hosted_shards,
                error,
                decode_usage_payload,
                [](const resource_usage &value) { return value.scope; },
                [this](const resource_usage &value, std::string *apply_error) {
                    return _budgets.hydrate_usage(value, apply_error);
                });
        }
        if (module == "recovery_supervisor" && kind == "policy")
        {
            recovery_policy value;
            return decode_runtime_state<::dsn::rasn::rpc::wire_recovery_policy>(
                       record.value, &value, error, decode_recovery_policy_payload) &&
                           _recovery.set_policy(value, error)
                       ? hydration_record_result::applied
                       : hydration_record_result::failed;
        }
        if (module == "recovery_supervisor" && kind == "failure")
        {
            failure_observation value;
            return decode_runtime_state<::dsn::rasn::rpc::wire_failure_observation>(
                       record.value, &value, error, decode_failure_payload) &&
                           _recovery.hydrate_failure(value, error)
                       ? hydration_record_result::applied
                       : hydration_record_result::failed;
        }
        if (module == "blackboard" && kind == "entry")
        {
            return decode_route_apply_sharded<::dsn::rasn::rpc::wire_blackboard_entry,
                                              blackboard_entry>(
                module,
                record,
                hosted_shards,
                error,
                decode_blackboard_payload,
                [](const blackboard_entry &value) { return value.key; },
                [this](const blackboard_entry &value, std::string *apply_error) {
                    return _blackboard.hydrate_entry(value, apply_error);
                });
        }
        if (module == "contract_verifier" && kind == "contract")
        {
            agent_contract value;
            return decode_runtime_state<::dsn::rasn::rpc::wire_agent_contract>(
                       record.value, &value, error, decode_contract_payload) &&
                           _contracts.register_contract(value, error)
                       ? hydration_record_result::applied
                       : hydration_record_result::failed;
        }
        if (module == "human_interaction" && kind == "request")
        {
            return decode_route_apply_sharded<::dsn::rasn::rpc::wire_human_interaction_request,
                                              human_interaction_request>(
                module,
                record,
                hosted_shards,
                error,
                decode_human_payload,
                [](const human_interaction_request &value) { return value.request_id; },
                [this](const human_interaction_request &value, std::string *apply_error) {
                    return _human.hydrate_request(value, apply_error);
                });
        }
        if (module == "sandbox_runtime" && kind == "profile")
        {
            sandbox_profile value;
            if (!decode_runtime_state<::dsn::rasn::rpc::wire_sandbox_profile>(
                    record.value, &value, error, decode_sandbox_profile_payload))
            {
                return hydration_record_result::failed;
            }
            ::dsn::service::zauto_lock guard(_sandbox_lock);
            _sandbox_profile = value;
            return hydration_record_result::applied;
        }
        if (error != nullptr)
        {
            *error = "unsupported runtime mirror kind: " + module + "/" + kind;
        }
        return hydration_record_result::failed;
    }

    struct dedup_entry
    {
        bool in_flight = false;
        std::string response;
        uint64_t expires_at_ms = 0;
        uint32_t waiters = 0;
    };

    enum class dedup_begin_result
    {
        owner,
        cached,
        uncached_retry
    };

    class dedup_completion_guard
    {
    public:
        dedup_completion_guard(rasn_runtime_service_store *store,
                               std::string key,
                               bool active,
                               bool *finished)
            : _store(store), _key(std::move(key)), _active(active), _finished(finished)
        {
        }

        ~dedup_completion_guard()
        {
            if (_active && _finished != nullptr && !*_finished && _store != nullptr)
            {
                try
                {
                    _store->abort_dedup(_key);
                }
                catch (...)
                {
                }
                *_finished = true;
            }
        }

    private:
        rasn_runtime_service_store *_store;
        std::string _key;
        bool _active;
        bool *_finished;
    };

    static size_t dedup_capacity()
    {
        const uint64_t configured = ::dsn_config_get_value_uint64(
            "rasn.service",
            "rasn_runtime_dedup_capacity",
            8192,
            "rASN runtime module idempotency cache capacity (0 disables dedup)");
        if (configured > (std::numeric_limits<size_t>::max)())
        {
            dwarn("rasn.service.rasn_runtime_dedup_capacity=%llu exceeds size_t range; using maximum capacity",
                  static_cast<unsigned long long>(configured));
            return (std::numeric_limits<size_t>::max)();
        }
        return static_cast<size_t>(configured);
    }

    static uint64_t dedup_ttl_ms()
    {
        return ::dsn_config_get_value_uint64("rasn.service",
                                             "rasn_runtime_dedup_ttl_ms",
                                             300000,
                                             "rASN runtime module idempotency cache TTL in milliseconds (0 disables TTL expiry)");
    }

    static uint64_t dedup_wait_timeout_ms()
    {
        return ::dsn_config_get_value_uint64("rasn.service",
                                             "rasn_runtime_dedup_wait_timeout_ms",
                                             5000,
                                             "Maximum time a duplicate runtime request waits for an in-flight idempotency response");
    }

    template <typename Request>
    static std::string dedup_key(const Request &request)
    {
        return std::string(runtime_module_name(request)) + "\x1f" +
               serialize_runtime_rpc_value(request);
    }

    void record_dedup_metric(const std::string &event) const
    {
        metrics_registry::instance().on_event("runtime.dedup." + event, "");
    }

    void rebuild_dedup_order_locked()
    {
        std::deque<std::string> kept;
        for (const std::string &key : _dedup_order)
        {
            if (_dedup_index.find(key) != _dedup_index.end())
            {
                kept.push_back(key);
            }
        }
        _dedup_order.swap(kept);
    }

    void prune_expired_dedup_locked(uint64_t now_ms)
    {
        const uint64_t ttl_ms = dedup_ttl_ms();
        if (ttl_ms == 0)
        {
            return;
        }
        bool pruned = false;
        for (std::map<std::string, dedup_entry>::iterator it = _dedup_index.begin(); it != _dedup_index.end();)
        {
            if (!it->second.in_flight && it->second.waiters == 0 && it->second.expires_at_ms != 0 &&
                it->second.expires_at_ms <= now_ms)
            {
                it = _dedup_index.erase(it);
                pruned = true;
                record_dedup_metric("expired");
            }
            else
            {
                ++it;
            }
        }
        if (pruned)
        {
            rebuild_dedup_order_locked();
            _dedup_cv.notify_all();
        }
    }

    void enforce_dedup_capacity_locked()
    {
        const size_t capacity = dedup_capacity();
        if (capacity == 0)
        {
            _dedup_index.clear();
            _dedup_order.clear();
            return;
        }

        size_t scanned = 0;
        const size_t initial_size = _dedup_order.size();
        while (_dedup_index.size() > capacity && !_dedup_order.empty() && scanned < initial_size)
        {
            const std::string key = _dedup_order.front();
            _dedup_order.pop_front();
            ++scanned;

            std::map<std::string, dedup_entry>::iterator it = _dedup_index.find(key);
            if (it == _dedup_index.end())
            {
                continue;
            }
            if (it->second.in_flight || it->second.waiters != 0)
            {
                _dedup_order.push_back(key);
                continue;
            }
            _dedup_index.erase(it);
            record_dedup_metric("evicted");
        }
    }

    template <typename Request>
    dedup_begin_result begin_dedup(const Request &request,
                                   std::string *cached,
                                   std::string *dedup_key_value)
    {
        const std::string key = dedup_key(request);
        if (dedup_key_value != nullptr)
        {
            *dedup_key_value = key;
        }
        std::unique_lock<std::mutex> guard(_dedup_lock);
        while (true)
        {
            const uint64_t now_ms = ::dsn_now_ms();
            prune_expired_dedup_locked(now_ms);
            std::map<std::string, dedup_entry>::iterator it = _dedup_index.find(key);
            if (it == _dedup_index.end())
            {
                dedup_entry entry;
                entry.in_flight = true;
                _dedup_index[key] = entry;
                _dedup_order.push_back(key);
                record_dedup_metric("miss");
                enforce_dedup_capacity_locked();
                return dedup_begin_result::owner;
            }

            if (!it->second.in_flight)
            {
                if (cached != nullptr)
                {
                    *cached = it->second.response;
                }
                record_dedup_metric("hit");
                return dedup_begin_result::cached;
            }

            ++it->second.waiters;
            record_dedup_metric("wait");
            const bool ready = _dedup_cv.wait_for(guard, std::chrono::milliseconds(dedup_wait_timeout_ms()), [&]() {
                const std::map<std::string, dedup_entry>::const_iterator current = _dedup_index.find(key);
                return current == _dedup_index.end() || !current->second.in_flight;
            });
            it = _dedup_index.find(key);
            if (it != _dedup_index.end() && it->second.waiters != 0)
            {
                --it->second.waiters;
            }
            if (!ready)
            {
                return dedup_begin_result::uncached_retry;
            }
            if (it != _dedup_index.end() && !it->second.in_flight)
            {
                if (cached != nullptr)
                {
                    *cached = it->second.response;
                }
                record_dedup_metric("hit");
                return dedup_begin_result::cached;
            }
        }
    }

    void abort_dedup(const std::string &key)
    {
        std::lock_guard<std::mutex> guard(_dedup_lock);
        _dedup_index.erase(key);
        for (std::deque<std::string>::iterator it = _dedup_order.begin(); it != _dedup_order.end();)
        {
            if (*it == key)
            {
                it = _dedup_order.erase(it);
            }
            else
            {
                ++it;
            }
        }
        _dedup_cv.notify_all();
    }

    void finish_dedup(const std::string &key, const std::string &response, bool ok)
    {
        const uint64_t now_ms = ::dsn_now_ms();
        const uint64_t ttl_ms = dedup_ttl_ms();
        std::lock_guard<std::mutex> guard(_dedup_lock);
        if (!ok)
        {
            _dedup_index.erase(key);
            rebuild_dedup_order_locked();
            _dedup_cv.notify_all();
            return;
        }
        std::map<std::string, dedup_entry>::iterator it = _dedup_index.find(key);
        if (it == _dedup_index.end())
        {
            // The in-flight entry was already removed (aborted/evicted) before
            // completion. Do NOT recreate it via operator[]: that would insert a
            // key absent from _dedup_order, leaving an orphan that capacity
            // enforcement can never evict (a slow memory leak when ttl==0).
            _dedup_cv.notify_all();
            return;
        }
        dedup_entry &entry = it->second;
        entry.in_flight = false;
        entry.response = response;
        entry.expires_at_ms =
            ttl_ms == 0 ? 0
                        : (now_ms > (std::numeric_limits<uint64_t>::max)() - ttl_ms
                               ? (std::numeric_limits<uint64_t>::max)()
                               : now_ms + ttl_ms);
        enforce_dedup_capacity_locked();
        _dedup_cv.notify_all();
    }

    ::dsn::rasn::rpc::agent_control_response route_module_request(
        const ::dsn::rasn::rpc::agent_control_request &request)
    {
        using operation = ::dsn::rasn::rpc::agent_control_operation;
        auto response =
            make_runtime_response<::dsn::rasn::rpc::agent_control_request,
                                  ::dsn::rasn::rpc::agent_control_response>(request);
        std::string error;
        if (request.operation == operation::ping)
        {
            response.__set_description("module=agent_control_plane status=ok");
        }
        else if (request.operation == operation::upsert_agent)
        {
            agent_control_record record;
            if (!from_wire(request.upsert_agent, &record, &error) ||
                !_agent_control.upsert_agent(record, &error))
                set_runtime_error(&response, ::dsn::rasn::rpc::runtime_error_code::conflict, error);
        }
        else if (request.operation == operation::acquire_lease)
        {
            const agent_control_lease lease = _agent_control.acquire_lease(
                request.acquire_lease.agent_id,
                request.acquire_lease.owner,
                static_cast<uint64_t>(request.acquire_lease.now_ms),
                static_cast<uint64_t>(request.acquire_lease.lease_ms));
            response.__set_lease(to_wire(lease));
        }
        else if (request.operation == operation::heartbeat)
        {
            if (!_agent_control.heartbeat(request.heartbeat.agent_id,
                                          static_cast<uint64_t>(request.heartbeat.now_ms),
                                          &error))
                set_runtime_error(&response, ::dsn::rasn::rpc::runtime_error_code::not_found, error);
        }
        else if (request.operation == operation::find)
        {
            agent_control_record record;
            if (!_agent_control.find(request.find.agent_id, &record))
                set_runtime_error(&response,
                                  ::dsn::rasn::rpc::runtime_error_code::not_found,
                                  "agent not found: " + request.find.agent_id);
            else
                response.__set_agent(to_wire(record));
        }
        else if (request.operation == operation::expire_leases)
        {
            response.__set_count(static_cast<int64_t>(
                _agent_control.expire_leases(static_cast<uint64_t>(request.expire_leases.now_ms))));
        }
        else if (request.operation == operation::list_agents)
        {
            std::vector<::dsn::rasn::rpc::wire_agent_control_record> records;
            for (const agent_control_record &record :
                 _agent_control.list(request.list_agents.include_expired,
                                     static_cast<uint64_t>(request.list_agents.now_ms)))
                records.push_back(to_wire(record));
            response.__set_agents(records);
        }
        else if (request.operation == operation::describe)
        {
            response.__set_description(
                _agent_control.describe(static_cast<uint64_t>(request.describe.now_ms)));
        }
        return response;
    }

    ::dsn::rasn::rpc::message_bus_response route_module_request(
        const ::dsn::rasn::rpc::message_bus_request &request)
    {
        using operation = ::dsn::rasn::rpc::message_bus_operation;
        auto response = make_runtime_response<::dsn::rasn::rpc::message_bus_request,
                                              ::dsn::rasn::rpc::message_bus_response>(request);
        std::string error;
        if (request.operation == operation::ping)
            response.__set_description("module=agent_message_bus status=ok");
        else if (request.operation == operation::publish)
        {
            agent_message message;
            agent_message stored;
            if (!from_wire(request.publish, &message, &error) ||
                !_message_bus.publish(message, &stored, &error))
                set_runtime_error(&response, ::dsn::rasn::rpc::runtime_error_code::conflict, error);
            else
                response.__set_message(to_wire(stored));
        }
        else if (request.operation == operation::acknowledge)
        {
            if (!_message_bus.ack(request.acknowledge.message_id,
                                  &error,
                                  static_cast<uint64_t>(request.acknowledge.now_ms)))
                set_runtime_error(&response, ::dsn::rasn::rpc::runtime_error_code::not_found, error);
        }
        else if (request.operation == operation::dead_letter)
        {
            if (!_message_bus.dead_letter(request.dead_letter.message_id,
                                          request.dead_letter.reason,
                                          &error,
                                          static_cast<uint64_t>(request.dead_letter.now_ms)))
                set_runtime_error(&response, ::dsn::rasn::rpc::runtime_error_code::not_found, error);
        }
        else if (request.operation == operation::find)
        {
            agent_message message;
            if (!_message_bus.find(request.find.message_id, &message))
                set_runtime_error(&response,
                                  ::dsn::rasn::rpc::runtime_error_code::not_found,
                                  "message not found: " + request.find.message_id);
            else
                response.__set_message(to_wire(message));
        }
        else if (request.operation == operation::snapshot)
        {
            std::vector<::dsn::rasn::rpc::wire_agent_message> messages;
            for (const agent_message &message : _message_bus.snapshot())
                messages.push_back(to_wire(message));
            response.__set_messages(messages);
        }
        else if (request.operation == operation::describe)
            response.__set_description("messages=" + std::to_string(_message_bus.snapshot().size()));
        return response;
    }

    ::dsn::rasn::rpc::task_orchestration_response route_module_request(
        const ::dsn::rasn::rpc::task_orchestration_request &request)
    {
        using operation = ::dsn::rasn::rpc::task_orchestration_operation;
        auto response =
            make_runtime_response<::dsn::rasn::rpc::task_orchestration_request,
                                  ::dsn::rasn::rpc::task_orchestration_response>(request);
        std::string error;
        if (request.operation == operation::ping)
            response.__set_description("module=task_orchestration_kernel status=ok");
        else if (request.operation == operation::add_task)
        {
            orchestration_task task;
            if (!from_wire(request.add_task, &task, &error) ||
                !_orchestration.add_task(task, &error))
                set_runtime_error(&response, ::dsn::rasn::rpc::runtime_error_code::conflict, error);
        }
        else if (request.operation == operation::start)
        {
            if (!_orchestration.start(request.start.task_id, request.start.owner_agent, &error))
                set_runtime_error(&response, ::dsn::rasn::rpc::runtime_error_code::conflict, error);
        }
        else if (request.operation == operation::complete)
        {
            if (!_orchestration.complete(request.complete.task_id, request.complete.output, &error))
                set_runtime_error(&response, ::dsn::rasn::rpc::runtime_error_code::conflict, error);
        }
        else if (request.operation == operation::fail)
        {
            if (!_orchestration.fail(
                    request.fail.task_id, request.fail.error, request.fail.retryable, &error))
                set_runtime_error(&response, ::dsn::rasn::rpc::runtime_error_code::conflict, error);
        }
        else if (request.operation == operation::find)
        {
            orchestration_task task;
            if (!_orchestration.find(request.find.task_id, &task))
                set_runtime_error(&response,
                                  ::dsn::rasn::rpc::runtime_error_code::not_found,
                                  "task not found: " + request.find.task_id);
            else
                response.__set_task(to_wire(task));
        }
        else if (request.operation == operation::snapshot ||
                 request.operation == operation::ready ||
                 request.operation == operation::blocked)
        {
            const std::vector<orchestration_task> source =
                request.operation == operation::snapshot
                    ? _orchestration.snapshot()
                    : (request.operation == operation::ready
                           ? _orchestration.ready_tasks(static_cast<uint64_t>(request.ready.now_ms))
                           : _orchestration.blocked_tasks());
            std::vector<::dsn::rasn::rpc::wire_orchestration_task> tasks;
            for (const orchestration_task &task : source)
                tasks.push_back(to_wire(task));
            response.__set_tasks(tasks);
        }
        else if (request.operation == operation::describe)
            response.__set_description("tasks=" + std::to_string(_orchestration.snapshot().size()));
        return response;
    }

    ::dsn::rasn::rpc::determinism_response route_module_request(
        const ::dsn::rasn::rpc::determinism_request &request)
    {
        using operation = ::dsn::rasn::rpc::determinism_operation;
        auto response = make_runtime_response<::dsn::rasn::rpc::determinism_request,
                                              ::dsn::rasn::rpc::determinism_response>(request);
        std::string error;
        if (request.operation == operation::ping)
            response.__set_description("module=determinism_ledger status=ok");
        else if (request.operation == operation::record)
        {
            deterministic_choice choice;
            if (!_determinism.record(request.record.task_id,
                                     request.record.key,
                                     request.record.source,
                                     request.record.value,
                                     &choice,
                                     &error))
                set_runtime_error(&response, ::dsn::rasn::rpc::runtime_error_code::conflict, error);
            else
                response.__set_choice(to_wire(choice));
        }
        else if (request.operation == operation::snapshot)
        {
            std::vector<::dsn::rasn::rpc::wire_deterministic_choice> choices;
            for (const deterministic_choice &choice : _determinism.snapshot())
                choices.push_back(to_wire(choice));
            response.__set_choices(choices);
        }
        else if (request.operation == operation::describe)
            response.__set_description("choices=" + std::to_string(_determinism.snapshot().size()));
        return response;
    }

    ::dsn::rasn::rpc::capability_directory_response route_module_request(
        const ::dsn::rasn::rpc::capability_directory_request &request)
    {
        using operation = ::dsn::rasn::rpc::capability_directory_operation;
        auto response =
            make_runtime_response<::dsn::rasn::rpc::capability_directory_request,
                                  ::dsn::rasn::rpc::capability_directory_response>(request);
        std::string error;
        if (request.operation == operation::ping)
            response.__set_description("module=capability_directory status=ok");
        else if (request.operation == operation::upsert_provider)
        {
            capability_provider provider;
            if (!from_wire(request.upsert_provider, &provider, &error) ||
                !_capabilities.upsert_provider(provider, &error))
                set_runtime_error(&response, ::dsn::rasn::rpc::runtime_error_code::conflict, error);
        }
        else if (request.operation == operation::describe)
            response.__set_description(_capabilities.describe());
        return response;
    }

    ::dsn::rasn::rpc::resource_budget_response route_module_request(
        const ::dsn::rasn::rpc::resource_budget_request &request)
    {
        using operation = ::dsn::rasn::rpc::resource_budget_operation;
        auto response = make_runtime_response<::dsn::rasn::rpc::resource_budget_request,
                                              ::dsn::rasn::rpc::resource_budget_response>(request);
        std::string error;
        if (request.operation == operation::ping)
            response.__set_description("module=resource_budget status=ok");
        else if (request.operation == operation::configure)
        {
            resource_quota quota;
            if (!from_wire(request.configure, &quota, &error) || !_budgets.configure(quota, &error))
                set_runtime_error(&response, ::dsn::rasn::rpc::runtime_error_code::conflict, error);
        }
        else if (request.operation == operation::reserve)
        {
            resource_request value;
            if (!from_wire(request.reserve, &value, &error))
                set_runtime_error(&response, ::dsn::rasn::rpc::runtime_error_code::invalid_request, error);
            else
                response.__set_decision(to_wire(_budgets.reserve(value)));
        }
        else if (request.operation == operation::release)
        {
            resource_request value;
            if (!from_wire(request.release, &value, &error) || !_budgets.release(value, &error))
                set_runtime_error(&response, ::dsn::rasn::rpc::runtime_error_code::conflict, error);
        }
        else if (request.operation == operation::usage)
        {
            resource_usage usage;
            if (!_budgets.usage(request.usage.scope, &usage))
                set_runtime_error(&response,
                                  ::dsn::rasn::rpc::runtime_error_code::not_found,
                                  "budget usage not found: " + request.usage.scope);
            else
                response.__set_usage(to_wire(usage));
        }
        else if (request.operation == operation::describe)
            response.__set_description(_budgets.describe());
        return response;
    }

    ::dsn::rasn::rpc::recovery_supervisor_response route_module_request(
        const ::dsn::rasn::rpc::recovery_supervisor_request &request)
    {
        using operation = ::dsn::rasn::rpc::recovery_supervisor_operation;
        auto response =
            make_runtime_response<::dsn::rasn::rpc::recovery_supervisor_request,
                                  ::dsn::rasn::rpc::recovery_supervisor_response>(request);
        std::string error;
        if (request.operation == operation::ping)
            response.__set_description("module=recovery_supervisor status=ok");
        else if (request.operation == operation::set_policy)
        {
            recovery_policy policy;
            if (!from_wire(request.set_policy, &policy, &error) ||
                !_recovery.set_policy(policy, &error))
                set_runtime_error(&response, ::dsn::rasn::rpc::runtime_error_code::conflict, error);
        }
        else if (request.operation == operation::observe_failure)
        {
            failure_observation failure;
            if (!from_wire(request.observe_failure, &failure, &error))
                set_runtime_error(&response, ::dsn::rasn::rpc::runtime_error_code::invalid_request, error);
            else
                response.__set_action(to_wire(_recovery.observe(failure)));
        }
        else if (request.operation == operation::describe)
            response.__set_description(_recovery.describe());
        return response;
    }

    ::dsn::rasn::rpc::blackboard_response route_module_request(
        const ::dsn::rasn::rpc::blackboard_request &request)
    {
        using operation = ::dsn::rasn::rpc::blackboard_operation;
        auto response = make_runtime_response<::dsn::rasn::rpc::blackboard_request,
                                              ::dsn::rasn::rpc::blackboard_response>(request);
        std::string error;
        if (request.operation == operation::ping)
            response.__set_description("module=blackboard status=ok");
        else if (request.operation == operation::put)
        {
            blackboard_entry entry;
            blackboard_entry stored;
            if (!from_wire(request.put, &entry, &error) ||
                !_blackboard.put(entry, &stored, &error))
                set_runtime_error(&response, ::dsn::rasn::rpc::runtime_error_code::conflict, error);
            else
                response.__set_entry(to_wire(stored));
        }
        else if (request.operation == operation::get)
        {
            blackboard_entry entry;
            if (!_blackboard.get(request.get.key, &entry))
                set_runtime_error(&response,
                                  ::dsn::rasn::rpc::runtime_error_code::not_found,
                                  "blackboard entry not found: " + request.get.key);
            else
                response.__set_entry(to_wire(entry));
        }
        else if (request.operation == operation::snapshot)
        {
            std::vector<::dsn::rasn::rpc::wire_blackboard_entry> entries;
            for (const blackboard_entry &entry :
                 _blackboard.snapshot(request.snapshot.include_expired,
                                      static_cast<uint64_t>(request.snapshot.now_ms)))
                entries.push_back(to_wire(entry));
            response.__set_entries(entries);
        }
        else if (request.operation == operation::describe)
            response.__set_description(_blackboard.describe());
        return response;
    }

    ::dsn::rasn::rpc::contract_verifier_response route_module_request(
        const ::dsn::rasn::rpc::contract_verifier_request &request)
    {
        using operation = ::dsn::rasn::rpc::contract_verifier_operation;
        auto response =
            make_runtime_response<::dsn::rasn::rpc::contract_verifier_request,
                                  ::dsn::rasn::rpc::contract_verifier_response>(request);
        std::string error;
        if (request.operation == operation::ping)
            response.__set_description("module=contract_verifier status=ok");
        else if (request.operation == operation::register_contract)
        {
            agent_contract contract;
            if (!from_wire(request.register_contract, &contract, &error) ||
                !_contracts.register_contract(contract, &error))
                set_runtime_error(&response, ::dsn::rasn::rpc::runtime_error_code::conflict, error);
        }
        else if (request.operation == operation::evaluate_input)
        {
            response.__set_evaluation(to_wire(_contracts.evaluate_input(
                request.evaluate_input.contract_id, request.evaluate_input.input)));
        }
        else if (request.operation == operation::evaluate_output)
        {
            response.__set_evaluation(to_wire(_contracts.evaluate_output(
                request.evaluate_output.contract_id,
                request.evaluate_output.output,
                request.evaluate_output.policy_labels)));
        }
        else if (request.operation == operation::describe)
            response.__set_description(_contracts.describe());
        return response;
    }

    ::dsn::rasn::rpc::human_interaction_rpc_response route_module_request(
        const ::dsn::rasn::rpc::human_interaction_rpc_request &request)
    {
        using operation = ::dsn::rasn::rpc::human_interaction_operation;
        auto response =
            make_runtime_response<::dsn::rasn::rpc::human_interaction_rpc_request,
                                  ::dsn::rasn::rpc::human_interaction_rpc_response>(request);
        std::string error;
        if (request.operation == operation::ping)
            response.__set_description("module=human_interaction status=ok");
        else if (request.operation == operation::open)
        {
            human_interaction_request human_request;
            if (!from_wire(request.open, &human_request, &error))
                set_runtime_error(&response, ::dsn::rasn::rpc::runtime_error_code::invalid_request, error);
            else
            {
                const human_interaction_result result =
                    _human.open(human_request, human_request.updated_at_ms);
                if (!result.ok)
                    set_runtime_error(&response,
                                      ::dsn::rasn::rpc::runtime_error_code::conflict,
                                      result.error);
                else
                    response.__set_request(to_wire(result.request));
            }
        }
        else if (request.operation == operation::answer || request.operation == operation::cancel)
        {
            const human_interaction_result result =
                request.operation == operation::answer
                    ? _human.answer(request.answer.request_id,
                                    request.answer.answer,
                                    static_cast<uint64_t>(request.answer.updated_at_ms))
                    : _human.cancel(request.cancel.request_id,
                                    request.cancel.reason,
                                    static_cast<uint64_t>(request.cancel.updated_at_ms));
            if (!result.ok)
                set_runtime_error(&response,
                                  ::dsn::rasn::rpc::runtime_error_code::conflict,
                                  result.error);
            else
                response.__set_request(to_wire(result.request));
        }
        else if (request.operation == operation::find)
        {
            human_interaction_request human_request;
            if (!_human.find(request.find.request_id, &human_request))
                set_runtime_error(&response,
                                  ::dsn::rasn::rpc::runtime_error_code::not_found,
                                  "human interaction request not found: " +
                                      request.find.request_id);
            else
                response.__set_request(to_wire(human_request));
        }
        else if (request.operation == operation::expire)
        {
            response.__set_count(
                static_cast<int64_t>(_human.expire(static_cast<uint64_t>(request.expire.now_ms))));
        }
        else if (request.operation == operation::snapshot || request.operation == operation::pending)
        {
            const std::vector<human_interaction_request> source =
                request.operation == operation::snapshot
                    ? _human.snapshot()
                    : _human.pending(request.pending.requester);
            std::vector<::dsn::rasn::rpc::wire_human_interaction_request> requests;
            for (const human_interaction_request &item : source)
                requests.push_back(to_wire(item));
            response.__set_requests(requests);
        }
        else if (request.operation == operation::describe)
            response.__set_description(_human.describe());
        return response;
    }

    ::dsn::rasn::rpc::sandbox_runtime_response route_module_request(
        const ::dsn::rasn::rpc::sandbox_runtime_request &request)
    {
        using operation = ::dsn::rasn::rpc::sandbox_runtime_operation;
        auto response = make_runtime_response<::dsn::rasn::rpc::sandbox_runtime_request,
                                              ::dsn::rasn::rpc::sandbox_runtime_response>(request);
        ::dsn::service::zauto_lock guard(_sandbox_lock);
        std::string error;
        if (request.operation == operation::ping)
            response.__set_description("module=sandbox_runtime status=ok");
        else if (request.operation == operation::set_profile)
        {
            sandbox_profile profile;
            if (!from_wire(request.set_profile, &profile, &error))
                set_runtime_error(&response, ::dsn::rasn::rpc::runtime_error_code::invalid_request, error);
            else
            {
                _sandbox_profile = profile;
                response.__set_profile(to_wire(_sandbox_profile));
            }
        }
        else if (request.operation == operation::get_profile)
            response.__set_profile(to_wire(_sandbox_profile));
        else if (request.operation == operation::evaluate)
        {
            sandbox_request value;
            if (!from_wire(request.evaluate, &value, &error))
                set_runtime_error(&response, ::dsn::rasn::rpc::runtime_error_code::invalid_request, error);
            else
                response.__set_decision(to_wire(evaluate_sandbox_request(_sandbox_profile, value)));
        }
        else if (request.operation == operation::describe)
            response.__set_description(describe_sandbox_profile(_sandbox_profile));
        return response;
    }

    agent_control_plane _agent_control;
    agent_message_bus _message_bus;
    task_orchestration_kernel _orchestration;
    determinism_ledger _determinism;
    capability_directory _capabilities;
    resource_budget_manager _budgets;
    recovery_supervisor _recovery;
    shared_blackboard _blackboard;
    contract_verifier _contracts;
    human_interaction_queue _human;
    sandbox_profile _sandbox_profile = default_read_only_sandbox_profile();
    mutable ::dsn::service::zlock _sandbox_lock;

    // Bounded idempotency cache: maps a request signature plus request_id to the
    // first response, with a FIFO eviction order so the cache stays within
    // rasn_runtime_dedup_capacity.
    std::map<std::string, dedup_entry> _dedup_index;
    std::deque<std::string> _dedup_order;
    mutable std::mutex _dedup_lock;
    std::condition_variable _dedup_cv;
    std::string _allowed_module;
};

rasn_runtime_service_store &global_rasn_runtime_store()
{
    static rasn_runtime_service_store store;
    return store;
}

template <typename Response>
bool runtime_status_ok(const Response &status)
{
    return status.ok && status.code == ::dsn::rasn::rpc::runtime_error_code::none;
}

template <typename Response>
void set_response_error(const Response &response, std::string *error)
{
    if (error != nullptr)
    {
        *error = response.status.message.empty() ? "runtime module API request failed"
                                                 : response.status.message;
    }
}

void clear_error(std::string *error)
{
    if (error != nullptr)
    {
        error->clear();
    }
}

template <typename Response>
bool response_bool(const Response &response, std::string *error)
{
    if (!runtime_status_ok(response.status))
    {
        set_response_error(response, error);
        return false;
    }
    clear_error(error);
    return true;
}

template <typename Wire, typename Domain>
std::vector<Domain> from_wire_list(const std::vector<Wire> &values)
{
    std::vector<Domain> results;
    results.reserve(values.size());
    for (const Wire &value : values)
    {
        Domain result;
        std::string error;
        if (!from_wire(value, &result, &error))
        {
            dwarn("typed runtime response conversion failed: %s", error.c_str());
            continue;
        }
        results.push_back(result);
    }
    return results;
}

template <typename Response>
uint32_t response_partition(const Response &response)
{
    return response.metadata.__isset.route_partition
               ? static_cast<uint32_t>(response.metadata.route_partition)
               : 0;
}

human_interaction_result human_result_from_response(
    const ::dsn::rasn::rpc::human_interaction_rpc_response &response)
{
    human_interaction_result result;
    if (!runtime_status_ok(response.status))
    {
        result.error = response.status.message.empty() ? "runtime module API request failed"
                                                        : response.status.message;
        return result;
    }
    if (!response.__isset.request ||
        !from_wire(response.request, &result.request, &result.error))
    {
        return result;
    }
    result.ok = true;
    return result;
}

contract_evaluation failed_contract_evaluation(const std::string &contract_id, const std::string &error)
{
    contract_evaluation evaluation;
    evaluation.ok = false;
    evaluation.contract_id = contract_id;
    evaluation.violations.push_back(error.empty() ? "runtime module API request failed" : error);
    return evaluation;
}

sandbox_decision denied_sandbox_decision(const std::string &reason)
{
    sandbox_decision decision;
    decision.allowed = false;
    decision.profile = "unavailable";
    decision.reason = reason.empty() ? "runtime module API request failed" : reason;
    return decision;
}

// ---------------------------------------------------------------------------
// Shared module invocation helpers used by the local, distributed, and hybrid
// runtime providers. Keeping the transport logic here (rather than duplicated in
// each provider) means the circuit breaker, retry/backoff, and idempotency policy
// are defined once and applied consistently wherever a module is reached remotely.
// ---------------------------------------------------------------------------

std::string generate_rasn_runtime_request_id()
{
    static const std::string process_prefix = make_trace_id();
    static std::atomic<uint64_t> counter(0);
    const uint64_t seq = counter.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream output;
    output << process_prefix << "-" << std::hex << ::dsn_now_ns() << "-" << seq;
    return output.str();
}

bool rasn_runtime_idempotency_enabled()
{
    return ::dsn_config_get_value_bool("rasn.service",
                                       "rasn_runtime_idempotency_enabled",
                                       true,
                                       "generate idempotency ids for rASN runtime module RPC retries");
}

bool rasn_runtime_breaker_enabled()
{
    return ::dsn_config_get_value_bool(
        "rasn.service", "rasn_runtime_breaker_enabled", true, "enable the rASN runtime module circuit breaker");
}

breaker_config read_rasn_runtime_breaker_config()
{
    breaker_config cfg;
    cfg.enabled = rasn_runtime_breaker_enabled();
    const uint64_t failure_threshold = config_service_uint64(
        "rasn_runtime_breaker_failures",
        5,
        "consecutive runtime module RPC failures before the circuit breaker opens");
    cfg.failure_threshold =
        failure_threshold > (std::numeric_limits<uint32_t>::max)()
            ? (std::numeric_limits<uint32_t>::max)()
            : static_cast<uint32_t>(failure_threshold);
    cfg.open_ms = config_service_uint64(
        "rasn_runtime_breaker_open_ms",
        30000,
        "cooldown in ms before an open runtime module circuit breaker admits a half-open probe");
    return cfg;
}

circuit_breaker_registry &global_rasn_runtime_breakers()
{
    static circuit_breaker_registry registry;
    return registry;
}

void ensure_rasn_runtime_breaker_config()
{
    static std::once_flag once;
    std::call_once(once, [] {
        circuit_breaker_registry &registry = global_rasn_runtime_breakers();
        registry.set_config(read_rasn_runtime_breaker_config());
        configure_rasn_shared_breaker_registry(registry, "runtime_module");
    });
}

// A remote module call issues an rDSN RPC, which the core only permits from a
// thread attached to a service node. A thin app in `distributed` mode running an
// ordinary CLI command executes on a plain CLI/io thread with no node attached, so
// the core would otherwise fail-stop the whole process ("tasks without explicit
// service node can only be created inside threads which is attached to specific
// node"). Detect the missing node context here and surface a graceful error,
// mirroring invoke_local_module's no-node fallback, instead of letting the core
// assert.
bool rasn_runtime_rpc_context_available()
{
    return ::dsn::task::get_current_node2() != nullptr;
}

std::string rasn_runtime_no_node_context_error(const std::string &module)
{
    return std::string("distributed runtime module '") + module +
           "' is only reachable over RPC from an rDSN service node context; configure the app for "
           "distributed/hybrid placement so its entry point attaches a client node, or embed it in "
           "a hosted runtime node";
}

template <typename Request>
struct runtime_response_for;

#define RASN_RUNTIME_RESPONSE_FOR(request_type, response_type)                                          \
    template <>                                                                                         \
    struct runtime_response_for<::dsn::rasn::rpc::request_type>                                         \
    {                                                                                                   \
        typedef ::dsn::rasn::rpc::response_type type;                                                   \
    }

RASN_RUNTIME_RESPONSE_FOR(agent_control_request, agent_control_response);
RASN_RUNTIME_RESPONSE_FOR(message_bus_request, message_bus_response);
RASN_RUNTIME_RESPONSE_FOR(task_orchestration_request, task_orchestration_response);
RASN_RUNTIME_RESPONSE_FOR(determinism_request, determinism_response);
RASN_RUNTIME_RESPONSE_FOR(capability_directory_request, capability_directory_response);
RASN_RUNTIME_RESPONSE_FOR(resource_budget_request, resource_budget_response);
RASN_RUNTIME_RESPONSE_FOR(recovery_supervisor_request, recovery_supervisor_response);
RASN_RUNTIME_RESPONSE_FOR(blackboard_request, blackboard_response);
RASN_RUNTIME_RESPONSE_FOR(contract_verifier_request, contract_verifier_response);
RASN_RUNTIME_RESPONSE_FOR(human_interaction_rpc_request, human_interaction_rpc_response);
RASN_RUNTIME_RESPONSE_FOR(sandbox_runtime_request, sandbox_runtime_response);

#undef RASN_RUNTIME_RESPONSE_FOR

template <typename Request>
void initialize_runtime_request(Request *request)
{
    if (request->metadata.wire_version == 0)
    {
        request->__set_metadata(make_runtime_request_metadata());
    }
    if (!request->metadata.__isset.trace_id && !current_rasn_runtime_trace_id().empty())
    {
        request->metadata.__set_trace_id(current_rasn_runtime_trace_id());
    }
    if (runtime_request_is_mutating(*request) && !request->metadata.__isset.request_id)
    {
        static std::once_flag idempotency_notice;
        if (!rasn_runtime_idempotency_enabled())
        {
            std::call_once(idempotency_notice, [] {
                dwarn("rasn_runtime_idempotency_enabled=false cannot disable request ids for "
                      "typed runtime mutations; the setting remains accepted for configuration "
                      "compatibility");
            });
        }
        request->metadata.__set_request_id(generate_rasn_runtime_request_id());
    }
}

template <typename Request>
typename runtime_response_for<Request>::type
make_runtime_transport_error(const Request &request, const std::string &message)
{
    typename runtime_response_for<Request>::type response =
        make_runtime_response<Request, typename runtime_response_for<Request>::type>(request);
    set_runtime_error(&response, ::dsn::rasn::rpc::runtime_error_code::unavailable, message);
    return response;
}

// In-process invocation dispatches directly without an active rDSN node and
// otherwise hops onto the module LPC queue.
template <typename Request>
typename runtime_response_for<Request>::type invoke_local_module(const Request &input)
{
    typedef typename runtime_response_for<Request>::type response_type;
    Request request = input;
    initialize_runtime_request(&request);
    if (::dsn::task::get_current_node2() == nullptr)
    {
        return dispatch_rasn_runtime_request(request);
    }
    std::shared_ptr<std::promise<response_type>> promise(new std::promise<response_type>());
    std::future<response_type> future = promise->get_future();
    const std::string module = runtime_module_name(request);
    ::dsn::task_ptr task = ::dsn::tasking::enqueue(
        lpc_code_for_module(module),
        nullptr,
        [request, promise]() { promise->set_value(dispatch_rasn_runtime_request(request)); });
    if (task == nullptr)
    {
        return make_runtime_transport_error(request, "failed to enqueue runtime module LPC request");
    }
    return future.get();
}

// Remote invocation shares one resilience policy across every generated module
// request/response pair.
template <typename Request>
typename runtime_response_for<Request>::type invoke_remote_module(const Request &input)
{
    typedef typename runtime_response_for<Request>::type response_type;
    const std::string module = runtime_module_name(input);
    if (!rasn_runtime_rpc_context_available())
    {
        return make_runtime_transport_error(input, rasn_runtime_no_node_context_error(module));
    }
    Request sending = input;
    initialize_runtime_request(&sending);
    const uint32_t partition_index =
        rasn_runtime_partition_for_request(sending);
    const uint32_t partition_count =
        rasn_runtime_partition_count(module);
    const std::shared_ptr<refreshable_endpoint_binding> binding =
        rasn_runtime_partition_binding(module, partition_index);
    const uint64_t partition_hash = rasn_runtime_partition_hash_impl(sending);
    if (rasn_runtime_module_is_sharded(module) && partition_count > 1)
    {
        sending.metadata.__set_route_partition(partition_index);
    }
    std::string auth_error;
    if (!prepare_rasn_runtime_rpc_request(&sending, &auth_error))
    {
        return make_runtime_transport_error(input, auth_error);
    }

    const std::chrono::milliseconds timeout = rasn_runtime_rpc_timeout(module);
    const uint32_t max_attempts = rasn_runtime_rpc_max_attempts(module);
    const uint64_t backoff_ms = rasn_runtime_rpc_backoff_ms(module);
    rpc_resilience_options lease_options;
    lease_options.max_attempts = max_attempts;
    lease_options.backoff_ms = backoff_ms;
    breaker_decision admission;
    const bool breaker_enabled = rasn_runtime_breaker_enabled();
    std::string admitted_key;
    bool admitted = false;
    if (breaker_enabled)
        ensure_rasn_runtime_breaker_config();

    const auto endpoint_from_snapshot =
        [partition_index, partition_count](const endpoint_snapshot &snapshot) {
            runtime_endpoint endpoint;
            endpoint.ok = snapshot.ok;
            endpoint.address = snapshot.address;
            endpoint.source = snapshot.source;
            endpoint.error = snapshot.error;
            endpoint.generation = snapshot.generation;
            endpoint.partition_index = partition_index;
            endpoint.partition_count = partition_count;
            return endpoint;
        };
    runtime_endpoint endpoint = endpoint_from_snapshot(binding->current());

    const auto report_breaker = [&](bool ok) {
        if (!breaker_enabled || !admitted)
            return;
        const breaker_report reported =
            global_rasn_runtime_breakers().report(
                admitted_key, admission, ok, ::dsn_now_ms());
        if (!reported.available)
        {
            dwarn("runtime module '%s' endpoint breaker report failed for key '%s': %s",
                  module.c_str(),
                  admitted_key.c_str(),
                  reported.error.c_str());
        }
        else if (reported.opened)
        {
            dwarn("runtime module '%s' endpoint breaker opened for key '%s' after %u consecutive failures",
                  module.c_str(),
                  admitted_key.c_str(),
                  static_cast<unsigned int>(reported.consecutive_failures));
        }
    };

    for (uint32_t attempt = 1; attempt <= max_attempts; ++attempt)
    {
        if (!endpoint.ok)
        {
            if (admitted)
            {
                report_breaker(false);
                admitted = false;
            }
            if (attempt >= max_attempts || !binding->refreshable())
            {
                binding->record_exhausted();
                return make_runtime_transport_error(
                    input,
                    "runtime module endpoint resolution failed for " + module +
                        ": " + endpoint.error);
            }
            endpoint = endpoint_from_snapshot(
                binding->refresh(endpoint.generation).endpoint);
            if (backoff_ms > 0)
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(backoff_ms * attempt));
            continue;
        }

        const std::string breaker_key =
            rasn_runtime_breaker_key(module, endpoint);
        if (admitted && admitted_key != breaker_key)
        {
            report_breaker(false);
            admitted = false;
        }
        if (!admitted || admitted_key != breaker_key)
        {
            admitted_key = breaker_key;
            if (breaker_enabled)
            {
                admission = global_rasn_runtime_breakers().allow(
                    breaker_key,
                    ::dsn_now_ms(),
                    rpc_breaker_probe_lease_hint(lease_options, timeout));
                if (!admission.allowed)
                {
                    dwarn("runtime module '%s' endpoint '%s' circuit breaker %s; refreshing binding%s%s",
                          module.c_str(),
                          endpoint.address.to_string(),
                          to_string(admission.state),
                          admission.error.empty() ? "" : ": ",
                          admission.error.c_str());
                    if (attempt >= max_attempts || !binding->refreshable())
                    {
                        binding->record_exhausted();
                        return make_runtime_transport_error(
                            input,
                            std::string("runtime module circuit breaker ") +
                                to_string(admission.state) +
                                (admission.error.empty()
                                     ? ""
                                     : ": " + admission.error));
                    }
                    endpoint = endpoint_from_snapshot(
                        binding->refresh(endpoint.generation).endpoint);
                    if (backoff_ms > 0)
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(backoff_ms * attempt));
                    continue;
                }
                admitted = true;
            }
        }

        rasn_runtime_client client(endpoint.address);
        const std::pair< ::dsn::error_code, response_type> result =
            client.call_sync(sending, timeout, 0, partition_hash);
        if (result.first == ::dsn::ERR_OK)
        {
            const bool stale_topology =
                !runtime_status_ok(result.second.status) &&
                result.second.status.retryable &&
                result.second.status.code ==
                    ::dsn::rasn::rpc::runtime_error_code::misrouted;
            if (!stale_topology)
            {
                report_breaker(true);
                return result.second;
            }

            // A misroute is a typed pre-dispatch topology rejection. It is safe to
            // retry even for mutations, using the same request/auth/trace/dedup id.
            report_breaker(true);
            admitted = false;
            endpoint = endpoint_from_snapshot(
                binding->refresh(endpoint.generation).endpoint);
            if (attempt >= max_attempts)
            {
                binding->record_exhausted();
                return result.second;
            }
            dwarn("runtime module '%s' RPC attempt %u/%u was misrouted; refreshing shard %u binding",
                  module.c_str(),
                  static_cast<unsigned int>(attempt),
                  static_cast<unsigned int>(max_attempts),
                  static_cast<unsigned int>(partition_index));
            if (backoff_ms > 0)
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(backoff_ms * attempt));
            continue;
        }

        const bool transport_retryable =
            is_retryable_rasn_runtime_error(result.first);
        const bool ambiguous_mutation =
            runtime_request_is_mutating(sending) &&
            rpc_error_is_ambiguous_transient(result.first) &&
            !rasn_runtime_module_uses_native_replication(module);
        const bool retryable = transport_retryable && !ambiguous_mutation;
        const runtime_endpoint failed_endpoint = endpoint;
        if (transport_retryable && binding->refreshable())
        {
            endpoint = endpoint_from_snapshot(
                binding->refresh(endpoint.generation).endpoint);
            if (endpoint.ok &&
                endpoint.address != failed_endpoint.address)
            {
                report_breaker(false);
                admitted = false;
            }
        }
        if (attempt >= max_attempts || !retryable)
        {
            report_breaker(false);
            if (retryable && attempt >= max_attempts)
                binding->record_exhausted();
            return make_runtime_transport_error(
                input, std::string("runtime module RPC failed: ") + result.first.to_string());
        }
        dwarn("runtime module '%s' RPC attempt %u/%u failed (%s); retrying",
              module.c_str(),
              static_cast<unsigned int>(attempt),
              static_cast<unsigned int>(max_attempts),
              result.first.to_string());
        if (backoff_ms > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms * attempt));
        }
    }
    // Unreachable: max_attempts >= 1 guarantees a return inside the loop. Keeping a
    // bare fallback avoids parking an unread error_code across the hot success
    // return, which rDSN's TRACK_ERROR_CODE would otherwise report as a dropped
    // error on every successful module RPC.
    return make_runtime_transport_error(input, std::string("runtime module RPC failed"));
}

// Health ping over RPC: uses a dedicated short timeout and a single attempt, and
// respects the circuit breaker (skipping the probe while open) so a multi-node
// readiness sweep stays fast when an endpoint is down.
template <typename Request>
bool ping_remote_module_type(const std::string &module, std::string *error)
{
    if (!rasn_runtime_rpc_context_available())
    {
        if (error != nullptr)
        {
            *error = rasn_runtime_no_node_context_error(module);
        }
        return false;
    }
    const uint32_t partition_count = rasn_runtime_partition_count(module);
    bool all_ok = true;
    std::string first_error;
    for (uint32_t partition_index = 0; partition_index < partition_count; ++partition_index)
    {
        const std::shared_ptr<refreshable_endpoint_binding> binding =
            rasn_runtime_partition_binding(module, partition_index);
        runtime_endpoint resolved_endpoint =
            resolve_rasn_runtime_partition_endpoint(module, partition_index);
        if (!resolved_endpoint.ok && binding->refreshable())
        {
            (void)binding->refresh(resolved_endpoint.generation);
            resolved_endpoint =
                resolve_rasn_runtime_partition_endpoint(module, partition_index);
        }
        if (!resolved_endpoint.ok)
        {
            all_ok = false;
            if (first_error.empty())
            {
                first_error = "runtime module endpoint resolution failed: " +
                              resolved_endpoint.error;
            }
            continue;
        }
        const ::dsn::rpc_address address = resolved_endpoint.address;
        const std::string endpoint = std::string(address.to_string());
        const std::string breaker_key =
            rasn_runtime_breaker_key(module, resolved_endpoint);
        const bool breaker_enabled = rasn_runtime_breaker_enabled();
        const std::chrono::milliseconds ping_timeout = rasn_runtime_ping_timeout(module);
        Request ping;
        typedef decltype(ping.operation) operation_type;
        ping.__set_operation(operation_type::ping);
        initialize_runtime_request(&ping);
        ping.metadata.__set_route_partition(resolved_endpoint.partition_index);
        // Explicit fan-out/probe routes use the partition index itself as the
        // canonical hash. Keyed calls retain the full FNV hash; modulo the same
        // configured/meta partition count, both select this partition.
        const uint64_t partition_hash = rasn_runtime_partition_hash(ping);
        std::string auth_error;
        if (!prepare_rasn_runtime_rpc_request(&ping, &auth_error))
        {
            all_ok = false;
            if (first_error.empty())
            {
                first_error = auth_error;
            }
            continue;
        }
        breaker_decision admission;
        if (breaker_enabled)
        {
            ensure_rasn_runtime_breaker_config();
            admission = global_rasn_runtime_breakers().allow(
                breaker_key, ::dsn_now_ms(), static_cast<uint64_t>(ping_timeout.count()));
            if (!admission.allowed)
            {
                if (binding->refreshable())
                {
                    (void)binding->refresh(resolved_endpoint.generation);
                }
                all_ok = false;
                if (first_error.empty())
                {
                    first_error = admission.error.empty()
                                      ? "runtime module circuit breaker open"
                                      : "runtime module circuit breaker unavailable: " +
                                            admission.error;
                }
                continue;
            }
        }
        rasn_runtime_client client(address);
        const std::pair<::dsn::error_code, typename runtime_response_for<Request>::type> result =
            client.call_sync(ping, ping_timeout, 0, partition_hash);
        if (result.first != ::dsn::ERR_OK)
        {
            if (is_retryable_rasn_runtime_error(result.first) &&
                binding->refreshable())
            {
                (void)binding->refresh(resolved_endpoint.generation);
            }
            if (breaker_enabled)
            {
                const breaker_report reported = global_rasn_runtime_breakers().report(
                    breaker_key, admission, false, ::dsn_now_ms());
                if (!reported.available && first_error.empty())
                {
                    first_error = "runtime module circuit breaker report failed: " +
                                  reported.error;
                }
            }
            all_ok = false;
            if (first_error.empty())
            {
                first_error = std::string("runtime module ping RPC failed: ") + result.first.to_string();
            }
            continue;
        }
        if (breaker_enabled)
        {
            const breaker_report reported = global_rasn_runtime_breakers().report(
                breaker_key, admission, true, ::dsn_now_ms());
            if (!reported.available && first_error.empty())
            {
                first_error =
                    "runtime module circuit breaker report failed: " + reported.error;
                all_ok = false;
            }
        }
        std::string response_error;
        if (!response_bool(result.second, &response_error))
        {
            if (result.second.status.retryable &&
                result.second.status.code ==
                    ::dsn::rasn::rpc::runtime_error_code::misrouted &&
                binding->refreshable())
            {
                (void)binding->refresh(resolved_endpoint.generation);
            }
            all_ok = false;
            if (first_error.empty())
            {
                first_error = response_error;
            }
        }
    }
    if (!all_ok && error != nullptr)
    {
        *error = first_error.empty() ? "runtime module ping failed" : first_error;
    }
    if (all_ok && error != nullptr)
    {
        error->clear();
    }
    return all_ok;
}

bool ping_remote_module(const std::string &module, std::string *error)
{
#define RASN_PING_REMOTE(module_name, request_type)                                                      \
    if (module == module_name)                                                                          \
        return ping_remote_module_type<::dsn::rasn::rpc::request_type>(module, error)

    RASN_PING_REMOTE("agent_control_plane", agent_control_request);
    RASN_PING_REMOTE("agent_message_bus", message_bus_request);
    RASN_PING_REMOTE("task_orchestration_kernel", task_orchestration_request);
    RASN_PING_REMOTE("determinism_ledger", determinism_request);
    RASN_PING_REMOTE("capability_directory", capability_directory_request);
    RASN_PING_REMOTE("resource_budget", resource_budget_request);
    RASN_PING_REMOTE("recovery_supervisor", recovery_supervisor_request);
    RASN_PING_REMOTE("blackboard", blackboard_request);
    RASN_PING_REMOTE("contract_verifier", contract_verifier_request);
    RASN_PING_REMOTE("human_interaction", human_interaction_rpc_request);
    RASN_PING_REMOTE("sandbox_runtime", sandbox_runtime_request);

#undef RASN_PING_REMOTE
    if (error != nullptr)
    {
        *error = "unknown runtime module: " + module;
    }
    return false;
}

template <typename Request>
bool ping_local_module_type(std::string *error)
{
    Request request;
    typedef decltype(request.operation) operation_type;
    request.__set_operation(operation_type::ping);
    return response_bool(invoke_local_module(request), error);
}

bool ping_local_module(const std::string &module, std::string *error)
{
#define RASN_PING_LOCAL(module_name, request_type)                                                       \
    if (module == module_name)                                                                          \
        return ping_local_module_type<::dsn::rasn::rpc::request_type>(error)

    RASN_PING_LOCAL("agent_control_plane", agent_control_request);
    RASN_PING_LOCAL("agent_message_bus", message_bus_request);
    RASN_PING_LOCAL("task_orchestration_kernel", task_orchestration_request);
    RASN_PING_LOCAL("determinism_ledger", determinism_request);
    RASN_PING_LOCAL("capability_directory", capability_directory_request);
    RASN_PING_LOCAL("resource_budget", resource_budget_request);
    RASN_PING_LOCAL("recovery_supervisor", recovery_supervisor_request);
    RASN_PING_LOCAL("blackboard", blackboard_request);
    RASN_PING_LOCAL("contract_verifier", contract_verifier_request);
    RASN_PING_LOCAL("human_interaction", human_interaction_rpc_request);
    RASN_PING_LOCAL("sandbox_runtime", sandbox_runtime_request);

#undef RASN_PING_LOCAL
    if (error != nullptr)
    {
        *error = "unknown runtime module: " + module;
    }
    return false;
}

class rasn_local_runtime_provider : public rasn_runtime_provider
{
public:
    explicit rasn_local_runtime_provider(const rasn_runtime_config &config)
        : rasn_runtime_provider(config)
    {
    }

    std::string provider_name() const override { return "local"; }
    bool distributed() const override { return false; }

protected:
#define RASN_LOCAL_PROVIDER_CALL(request_type, response_type)                                            \
    ::dsn::rasn::rpc::response_type call_module_api(                                                     \
        const ::dsn::rasn::rpc::request_type &request) const override                                   \
    {                                                                                                   \
        return invoke_local_module(request);                                                            \
    }

    RASN_LOCAL_PROVIDER_CALL(agent_control_request, agent_control_response);
    RASN_LOCAL_PROVIDER_CALL(message_bus_request, message_bus_response);
    RASN_LOCAL_PROVIDER_CALL(task_orchestration_request, task_orchestration_response);
    RASN_LOCAL_PROVIDER_CALL(determinism_request, determinism_response);
    RASN_LOCAL_PROVIDER_CALL(capability_directory_request, capability_directory_response);
    RASN_LOCAL_PROVIDER_CALL(resource_budget_request, resource_budget_response);
    RASN_LOCAL_PROVIDER_CALL(recovery_supervisor_request, recovery_supervisor_response);
    RASN_LOCAL_PROVIDER_CALL(blackboard_request, blackboard_response);
    RASN_LOCAL_PROVIDER_CALL(contract_verifier_request, contract_verifier_response);
    RASN_LOCAL_PROVIDER_CALL(human_interaction_rpc_request, human_interaction_rpc_response);
    RASN_LOCAL_PROVIDER_CALL(sandbox_runtime_request, sandbox_runtime_response);

#undef RASN_LOCAL_PROVIDER_CALL

    bool write_state(const std::string &module,
                     const std::string &kind,
                     const std::string &key,
                     const std::string &value,
                     std::string *error) override
    {
        (void)module;
        (void)kind;
        (void)key;
        (void)value;
        clear_error(error);
        return true;
    }
};

class rasn_distributed_runtime_provider : public rasn_runtime_provider
{
public:
    rasn_distributed_runtime_provider(rasn_service_graph &services, const rasn_runtime_config &config)
        : rasn_runtime_provider(config), _services(services)
    {
    }

    std::string provider_name() const override { return "distributed"; }
    bool distributed() const override { return true; }

    bool ping_module(const std::string &module, std::string *error) const override
    {
        return ping_remote_module(module, error);
    }

protected:
#define RASN_REMOTE_PROVIDER_CALL(request_type, response_type)                                           \
    ::dsn::rasn::rpc::response_type call_module_api(                                                     \
        const ::dsn::rasn::rpc::request_type &request) const override                                   \
    {                                                                                                   \
        return invoke_remote_module(request);                                                           \
    }

    RASN_REMOTE_PROVIDER_CALL(agent_control_request, agent_control_response);
    RASN_REMOTE_PROVIDER_CALL(message_bus_request, message_bus_response);
    RASN_REMOTE_PROVIDER_CALL(task_orchestration_request, task_orchestration_response);
    RASN_REMOTE_PROVIDER_CALL(determinism_request, determinism_response);
    RASN_REMOTE_PROVIDER_CALL(capability_directory_request, capability_directory_response);
    RASN_REMOTE_PROVIDER_CALL(resource_budget_request, resource_budget_response);
    RASN_REMOTE_PROVIDER_CALL(recovery_supervisor_request, recovery_supervisor_response);
    RASN_REMOTE_PROVIDER_CALL(blackboard_request, blackboard_response);
    RASN_REMOTE_PROVIDER_CALL(contract_verifier_request, contract_verifier_response);
    RASN_REMOTE_PROVIDER_CALL(human_interaction_rpc_request, human_interaction_rpc_response);
    RASN_REMOTE_PROVIDER_CALL(sandbox_runtime_request, sandbox_runtime_response);

#undef RASN_REMOTE_PROVIDER_CALL

    std::string module_endpoint(const std::string &module) const override
    {
        return rasn_runtime_module_endpoint_summary(module);
    }

    bool write_state(const std::string &module,
                     const std::string &kind,
                     const std::string &key,
                     const std::string &value,
                     std::string *error) override
    {
        if (!rasn_runtime_state_mirroring_enabled())
        {
            clear_error(error);
            return true;
        }

        std::string state_error;
        if (!put_runtime_state_mirror(_services, state_prefix(), module, kind, key, value, &state_error))
        {
            if (error != nullptr)
            {
                *error = state_error;
            }
            return false;
        }
        clear_error(error);
        return true;
    }

private:
    rasn_service_graph &_services;
};

// Per-module routing provider: each runtime module is independently placed either
// in-process (local) or on a remote service (distributed) based on config, so an
// operator can co-locate hot/latency-sensitive modules with the app while pushing
// shared, stateful modules (e.g. blackboard, resource_budget) onto their own nodes.
// A module is routed remotely when "[rasn.service] <module>_mode" (falling back to
// rasn_runtime_default_mode) is remote/distributed/rpc.
class rasn_hybrid_runtime_provider : public rasn_runtime_provider
{
public:
    rasn_hybrid_runtime_provider(rasn_service_graph &services, const rasn_runtime_config &config)
        : rasn_runtime_provider(config), _services(services)
    {
    }

    std::string provider_name() const override { return "hybrid"; }
    bool distributed() const override { return true; }

    bool ping_module(const std::string &module, std::string *error) const override
    {
        if (module_is_remote(module))
        {
            return ping_remote_module(module, error);
        }
        return ping_local_module(module, error);
    }

protected:
#define RASN_HYBRID_PROVIDER_CALL(request_type, response_type)                                           \
    ::dsn::rasn::rpc::response_type call_module_api(                                                     \
        const ::dsn::rasn::rpc::request_type &request) const override                                   \
    {                                                                                                   \
        return module_is_remote(runtime_module_name(request)) ? invoke_remote_module(request)            \
                                                               : invoke_local_module(request);           \
    }

    RASN_HYBRID_PROVIDER_CALL(agent_control_request, agent_control_response);
    RASN_HYBRID_PROVIDER_CALL(message_bus_request, message_bus_response);
    RASN_HYBRID_PROVIDER_CALL(task_orchestration_request, task_orchestration_response);
    RASN_HYBRID_PROVIDER_CALL(determinism_request, determinism_response);
    RASN_HYBRID_PROVIDER_CALL(capability_directory_request, capability_directory_response);
    RASN_HYBRID_PROVIDER_CALL(resource_budget_request, resource_budget_response);
    RASN_HYBRID_PROVIDER_CALL(recovery_supervisor_request, recovery_supervisor_response);
    RASN_HYBRID_PROVIDER_CALL(blackboard_request, blackboard_response);
    RASN_HYBRID_PROVIDER_CALL(contract_verifier_request, contract_verifier_response);
    RASN_HYBRID_PROVIDER_CALL(human_interaction_rpc_request, human_interaction_rpc_response);
    RASN_HYBRID_PROVIDER_CALL(sandbox_runtime_request, sandbox_runtime_response);

#undef RASN_HYBRID_PROVIDER_CALL

    bool module_routed_remote(const std::string &module) const override { return module_is_remote(module); }

    std::string module_endpoint(const std::string &module) const override
    {
        if (!module_is_remote(module))
        {
            return "in-process";
        }
        const runtime_endpoint endpoint = resolve_rasn_runtime_endpoint(module);
        return rasn_runtime_module_endpoint_summary(module);
    }

    bool write_state(const std::string &module,
                     const std::string &kind,
                     const std::string &key,
                     const std::string &value,
                     std::string *error) override
    {
        const bool remote = module_is_remote(module);
        if (remote && !rasn_runtime_state_mirroring_enabled())
        {
            if (error != nullptr)
            {
                error->clear();
            }
            return true;
        }
        // Locally-routed modules keep state in-process (parity with the local
        // provider); remotely-routed modules also mirror to the shared state
        // service so the value survives a module service restart.
        if (!remote)
        {
            clear_error(error);
            return true;
        }

        std::string state_error;
        if (!put_runtime_state_mirror(_services, state_prefix(), module, kind, key, value, &state_error))
        {
            if (error != nullptr)
            {
                *error = state_error;
            }
            return false;
        }
        clear_error(error);
        return true;
    }

private:
    bool module_is_remote(const std::string &module) const
    {
        const std::string module_key = module_service_key(module);
        const std::string default_mode = lower_ascii(trim(config_service_string(
            "rasn_runtime_default_mode", "local", "default rASN hybrid module routing: local or remote")));
        const std::string mode = lower_ascii(trim(config_service_string(
            module_key + "_mode", default_mode, "per-module rASN hybrid routing: local or remote")));
        return mode == "remote" || mode == "distributed" || mode == "rpc";
    }

    rasn_service_graph &_services;
};

} // namespace

namespace {

const size_t kReplicatedRuntimeDedupCapacity = 8192;
const uint64_t kReplicatedRuntimeCheckpointVersion = 2;

size_t configured_replicated_runtime_dedup_capacity()
{
    const uint64_t configured = ::dsn_config_get_value_uint64(
        "rasn.service",
        "rasn_runtime_dedup_capacity",
        8192,
        "rASN runtime module idempotency cache capacity (0 disables dedup)");
    return configured > (std::numeric_limits<size_t>::max)()
               ? (std::numeric_limits<size_t>::max)()
               : static_cast<size_t>(configured);
}

struct replicated_dedup_entry
{
    std::string module;
    std::string encoded_response;
};

template <typename Request>
std::string replicated_runtime_request_signature(const Request &request)
{
    return std::string(runtime_module_name(request)) + "\x1f" +
           serialize_runtime_rpc_value(request);
}

std::string encode_replicated_dedup_payload(const std::string &signature,
                                            const replicated_dedup_entry &entry)
{
    return encode_fields({{"schema_version", std::to_string(RASN_RUNTIME_WIRE_VERSION)},
                          {"signature", signature},
                          {"module", entry.module},
                          {"typed_response", entry.encoded_response}});
}

uint64_t replicated_runtime_records_digest(std::vector<state_record> records)
{
    std::sort(records.begin(), records.end(), [](const state_record &left, const state_record &right) {
        return std::tie(left.key, left.kind, left.scope, left.value) <
               std::tie(right.key, right.kind, right.scope, right.value);
    });
    std::ostringstream canonical;
    for (const state_record &record : records)
    {
        const auto append = [&canonical](const std::string &value) {
            canonical << value.size() << ':' << value;
        };
        append(std::to_string(record.schema_version));
        append(record.key);
        append(record.kind);
        append(record.scope);
        append(record.value);
    }
    return fnv1a64(canonical.str());
}

std::string encode_replicated_runtime_manifest(const std::string &module,
                                               size_t dedup_capacity,
                                               ::dsn_gpid gpid,
                                               int64_t decree,
                                               size_t record_count,
                                               uint64_t content_digest)
{
    return encode_fields({{"schema_version", std::to_string(kReplicatedRuntimeCheckpointVersion)},
                          {"module", module},
                          {"dedup_capacity", std::to_string(dedup_capacity)},
                          {"app_id", std::to_string(gpid.u.app_id)},
                          {"partition_index", std::to_string(gpid.u.partition_index)},
                          {"decree", std::to_string(decree)},
                          {"record_count", std::to_string(record_count)},
                          {"content_digest", std::to_string(content_digest)}});
}

bool validate_replicated_runtime_manifest(const std::string &payload,
                                          const std::string &module,
                                          size_t dedup_capacity,
                                          ::dsn_gpid gpid,
                                          int64_t decree,
                                          size_t record_count,
                                          uint64_t content_digest,
                                          std::string *error)
{
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    const char *required_fields[] = {"schema_version",
                                     "module",
                                     "dedup_capacity",
                                     "app_id",
                                     "partition_index",
                                     "decree",
                                     "record_count",
                                     "content_digest"};
    for (const char *field : required_fields)
    {
        if (fields.find(field) == fields.end())
        {
            if (error != nullptr)
            {
                *error = "replicated runtime checkpoint manifest is incomplete";
            }
            return false;
        }
    }
    if (gpid.u.app_id <= 0 || gpid.u.partition_index < 0 || decree < 0 ||
        (field_uint64(fields, "schema_version") < 1 ||
         field_uint64(fields, "schema_version") > kReplicatedRuntimeCheckpointVersion) ||
        field_string(fields, "module") != module ||
        field_uint64(fields, "dedup_capacity") != dedup_capacity ||
        field_uint64(fields, "app_id") != static_cast<uint64_t>(gpid.u.app_id) ||
        field_uint64(fields, "partition_index") != static_cast<uint64_t>(gpid.u.partition_index) ||
        field_uint64(fields, "decree") != static_cast<uint64_t>(decree) ||
        field_uint64(fields, "record_count") != static_cast<uint64_t>(record_count) ||
        field_uint64(fields, "content_digest") != content_digest)
    {
        if (error != nullptr)
        {
            *error = "replicated runtime checkpoint manifest does not match the local replica";
        }
        return false;
    }
    return true;
}

bool decode_replicated_dedup_payload(const std::string &payload,
                                     std::string *signature,
                                     replicated_dedup_entry *entry,
                                     std::string *error)
{
    if (signature == nullptr || entry == nullptr)
    {
        if (error != nullptr)
        {
            *error = "replicated runtime dedup output is null";
        }
        return false;
    }
    field_map fields;
    if (!parse_payload(payload, &fields, error))
    {
        return false;
    }
    if (field_uint64(fields, "schema_version") != RASN_RUNTIME_WIRE_VERSION)
    {
        if (error != nullptr)
        {
            *error = "replicated runtime dedup record has unsupported schema version";
        }
        return false;
    }
    *signature = field_string(fields, "signature");
    entry->module = field_string(fields, "module");
    entry->encoded_response = field_string(fields, "typed_response");
    if (!signature->empty() && !entry->module.empty() &&
        entry->encoded_response.empty() &&
        fields.find("response_schema_version") != fields.end())
    {
        // A pre-fd5 dedup record cannot be replayed as a typed response. Accept
        // the checkpoint and discard only its bounded retry cache entry.
        return true;
    }
    if (signature->empty() || entry->module.empty() || entry->encoded_response.empty())
    {
        if (error != nullptr)
        {
            *error = "replicated runtime dedup record is incomplete";
        }
        return false;
    }
    return true;
}

template <typename Request>
bool replicated_request_has_deterministic_values(const Request &, std::string *)
{
    return true;
}

bool replicated_request_has_deterministic_values(
    const ::dsn::rasn::rpc::agent_control_request &request, std::string *error)
{
    using operation = ::dsn::rasn::rpc::agent_control_operation;
    const bool missing_timestamp =
        (request.operation == operation::upsert_agent &&
         request.upsert_agent.last_heartbeat_ms == 0) ||
        (request.operation == operation::acquire_lease &&
         request.acquire_lease.now_ms == 0) ||
        (request.operation == operation::heartbeat && request.heartbeat.now_ms == 0) ||
        (request.operation == operation::expire_leases &&
         request.expire_leases.now_ms == 0);
    if (missing_timestamp)
    {
        if (error != nullptr)
        {
            *error = "replicated agent-control mutation requires an explicit timestamp";
        }
        return false;
    }
    return true;
}

bool replicated_request_has_deterministic_values(
    const ::dsn::rasn::rpc::message_bus_request &request, std::string *error)
{
    const bool invalid_publish =
        request.operation == ::dsn::rasn::rpc::message_bus_operation::publish &&
        (request.publish.created_at_ms == 0 || request.publish.updated_at_ms == 0);
    const bool invalid_ack =
        request.operation == ::dsn::rasn::rpc::message_bus_operation::acknowledge &&
        request.acknowledge.now_ms == 0;
    const bool invalid_dead_letter =
        request.operation == ::dsn::rasn::rpc::message_bus_operation::dead_letter &&
        request.dead_letter.now_ms == 0;
    if (invalid_publish || invalid_ack || invalid_dead_letter)
    {
        if (error != nullptr)
            *error = "replicated message mutation requires explicit timestamps";
        return false;
    }
    return true;
}

bool replicated_request_has_deterministic_values(
    const ::dsn::rasn::rpc::capability_directory_request &request, std::string *error)
{
    if (request.operation ==
            ::dsn::rasn::rpc::capability_directory_operation::upsert_provider &&
        request.upsert_provider.last_seen_ms == 0)
    {
        if (error != nullptr)
            *error = "replicated capability upsert requires last_seen_ms";
        return false;
    }
    return true;
}

bool replicated_request_has_deterministic_values(
    const ::dsn::rasn::rpc::recovery_supervisor_request &request, std::string *error)
{
    if (request.operation ==
            ::dsn::rasn::rpc::recovery_supervisor_operation::observe_failure &&
        request.observe_failure.time_ms == 0)
    {
        if (error != nullptr)
            *error = "replicated failure observation requires time_ms";
        return false;
    }
    return true;
}

bool replicated_request_has_deterministic_values(
    const ::dsn::rasn::rpc::blackboard_request &request, std::string *error)
{
    if (request.operation == ::dsn::rasn::rpc::blackboard_operation::put &&
        request.put.updated_at_ms == 0)
    {
        if (error != nullptr)
            *error = "replicated blackboard put requires updated_at_ms";
        return false;
    }
    return true;
}

bool replicated_request_has_deterministic_values(
    const ::dsn::rasn::rpc::human_interaction_rpc_request &request, std::string *error)
{
    const bool invalid_open =
        request.operation == ::dsn::rasn::rpc::human_interaction_operation::open &&
        (request.open.created_at_ms == 0 || request.open.updated_at_ms == 0);
    const bool invalid_answer =
        request.operation == ::dsn::rasn::rpc::human_interaction_operation::answer &&
        request.answer.updated_at_ms == 0;
    const bool invalid_cancel =
        request.operation == ::dsn::rasn::rpc::human_interaction_operation::cancel &&
        request.cancel.updated_at_ms == 0;
    const bool invalid_expire =
        request.operation == ::dsn::rasn::rpc::human_interaction_operation::expire &&
        request.expire.now_ms == 0;
    if (invalid_open || invalid_answer || invalid_cancel || invalid_expire)
    {
        if (error != nullptr)
            *error = "replicated human interaction mutation requires explicit timestamps";
        return false;
    }
    return true;
}

template <typename Request>
bool replicated_runtime_request_is_deterministic(const Request &request, std::string *error)
{
    if (!runtime_request_is_mutating(request))
    {
        return true;
    }
    if (!request.metadata.__isset.request_id || request.metadata.request_id.empty())
    {
        if (error != nullptr)
        {
            *error = "replicated runtime mutation requires a stable request_id";
        }
        return false;
    }
    if (!validate_runtime_request(request, error))
    {
        return false;
    }
    return replicated_request_has_deterministic_values(request, error);
}

bool build_replicated_runtime_store(const std::string &module,
                                    const std::vector<state_record> &records,
                                    size_t dedup_capacity,
                                    ::dsn_gpid gpid,
                                    int64_t decree,
                                    std::unique_ptr<rasn_runtime_service_store> *store,
                                    std::map<std::string, replicated_dedup_entry> *dedup,
                                    std::deque<std::string> *order,
                                    std::string *error)
{
    if (store == nullptr || dedup == nullptr || order == nullptr)
    {
        if (error != nullptr)
        {
            *error = "replicated runtime checkpoint output is null";
        }
        return false;
    }
    if (error != nullptr)
    {
        error->clear();
    }

    std::vector<state_record> sorted = records;
    std::sort(sorted.begin(), sorted.end(), [](const state_record &left, const state_record &right) {
        if (left.sequence != right.sequence)
        {
            return left.sequence < right.sequence;
        }
        return left.key < right.key;
    });
    dedup->clear();
    order->clear();
    size_t manifest_count = 0;
    std::string manifest_payload;
    std::vector<state_record> content_records;
    std::set<std::string> record_keys;
    for (const state_record &record : sorted)
    {
        if (!record_keys.insert(record.key).second)
        {
            if (error != nullptr)
            {
                *error = "replicated runtime checkpoint contains duplicate record keys";
            }
            return false;
        }
        std::string record_module;
        std::string kind;
        if (record.scope != "rasn.runtime" ||
            !parse_rasn_runtime_record_kind(record.kind, &record_module, &kind) ||
            record_module != module)
        {
            if (error != nullptr)
            {
                *error = "replicated runtime checkpoint contains a record for the wrong module";
            }
            return false;
        }
        if (kind == "manifest")
        {
            ++manifest_count;
            if (manifest_count != 1)
            {
                if (error != nullptr)
                {
                    *error = "replicated runtime checkpoint contains duplicate manifests";
                }
                return false;
            }
            manifest_payload = record.value;
            continue;
        }
        content_records.push_back(record);
        if (kind != "dedup")
        {
            if (!runtime_hydration_supported(module, kind))
            {
                if (error != nullptr)
                {
                    *error = "replicated runtime checkpoint contains an unsupported record kind";
                }
                return false;
            }
            continue;
        }
        std::string signature;
        replicated_dedup_entry entry;
        if (!decode_replicated_dedup_payload(record.value, &signature, &entry, error))
        {
            return false;
        }
        if (entry.encoded_response.empty())
        {
            continue;
        }
        if (dedup->find(signature) != dedup->end())
        {
            if (error != nullptr)
            {
                *error = "replicated runtime checkpoint contains duplicate dedup signature";
            }
            return false;
        }
        (*dedup)[signature] = entry;
        order->push_back(signature);
    }
    if (manifest_count != 1)
    {
        if (error != nullptr)
        {
            *error = "replicated runtime checkpoint is missing its manifest";
        }
        return false;
    }
    if (!validate_replicated_runtime_manifest(manifest_payload,
                                              module,
                                              dedup_capacity,
                                              gpid,
                                              decree,
                                              content_records.size(),
                                              replicated_runtime_records_digest(content_records),
                                              error))
    {
        return false;
    }

    while (order->size() > dedup_capacity)
    {
        dedup->erase(order->front());
        order->pop_front();
    }
    std::unique_ptr<rasn_runtime_service_store> restored(new rasn_runtime_service_store(module));
    size_t applied = 0;
    if (!restored->hydrate_from_state(records, std::vector<std::string>{module}, &applied, error))
    {
        return false;
    }
    *store = std::move(restored);
    if (error != nullptr)
    {
        error->clear();
    }
    return true;
}

} // namespace

struct rasn_runtime_replica_store::impl
{
    explicit impl(std::string module_name)
        : module(std::move(module_name)),
          store(new rasn_runtime_service_store(module)),
          dedup_capacity(kReplicatedRuntimeDedupCapacity)
    {
    }

    std::string module;
    std::unique_ptr<rasn_runtime_service_store> store;
    const size_t dedup_capacity;
    std::map<std::string, replicated_dedup_entry> dedup;
    std::deque<std::string> order;
    mutable std::mutex lock;
};

rasn_runtime_replica_store::rasn_runtime_replica_store(std::string module)
    : _impl(new impl(std::move(module)))
{
}

rasn_runtime_replica_store::~rasn_runtime_replica_store() = default;

template <typename Request, typename Response>
Response rasn_runtime_replica_store::dispatch_typed(const Request &request)
{
    std::lock_guard<std::mutex> guard(_impl->lock);
    if (runtime_module_name(request) != _impl->module)
    {
        return make_runtime_transport_error(
            request,
            "replica for " + _impl->module + " cannot dispatch module " +
                runtime_module_name(request));
    }

    const bool mutating = runtime_request_is_mutating(request);
    std::string signature;
    if (mutating)
    {
        std::string error;
        if (!replicated_runtime_request_is_deterministic(request, &error))
        {
            Response response = make_runtime_response<Request, Response>(request);
            // Replicated ingress must classify a rejected mutation the same way
            // the standalone path does: an unsupported wire version tells the
            // peer to change build, not payload.
            set_runtime_error(&response, runtime_validation_error_code(error), error);
            return response;
        }
        signature = replicated_runtime_request_signature(request);
        const std::map<std::string, replicated_dedup_entry>::const_iterator cached =
            _impl->dedup.find(signature);
        if (cached != _impl->dedup.end())
        {
            metrics_registry::instance().on_event("runtime.dedup.replica_hit", _impl->module);
            Response response;
            std::string error;
            if (cached->second.module == _impl->module &&
                deserialize_runtime_rpc_value(cached->second.encoded_response, &response, &error))
            {
                return response;
            }
            Response invalid = make_runtime_response<Request, Response>(request);
            set_runtime_error(&invalid,
                              ::dsn::rasn::rpc::runtime_error_code::internal,
                              error.empty() ? "replicated runtime dedup record is invalid" : error);
            return invalid;
        }
    }

    Response response;
    try
    {
        response = _impl->store->dispatch(request, false);
    }
    catch (const std::exception &ex)
    {
        response = make_runtime_response<Request, Response>(request);
        set_runtime_error(&response,
                          ::dsn::rasn::rpc::runtime_error_code::internal,
                          std::string("replicated runtime dispatch threw: ") + ex.what());
    }
    catch (...)
    {
        response = make_runtime_response<Request, Response>(request);
        set_runtime_error(&response,
                          ::dsn::rasn::rpc::runtime_error_code::internal,
                          "replicated runtime dispatch threw an unknown exception");
    }
    if (mutating)
    {
        if (_impl->dedup_capacity != 0)
        {
            replicated_dedup_entry entry;
            entry.module = _impl->module;
            entry.encoded_response = serialize_runtime_rpc_value(response);
            _impl->dedup[signature] = entry;
            _impl->order.push_back(signature);
            while (_impl->order.size() > _impl->dedup_capacity)
            {
                _impl->dedup.erase(_impl->order.front());
                _impl->order.pop_front();
                metrics_registry::instance().on_event("runtime.dedup.replica_evicted", _impl->module);
            }
        }
    }
    return response;
}

#define RASN_DEFINE_REPLICA_DISPATCH(request_type, response_type)                                       \
    ::dsn::rasn::rpc::response_type rasn_runtime_replica_store::dispatch(                               \
        const ::dsn::rasn::rpc::request_type &request)                                                  \
    {                                                                                                   \
        return dispatch_typed<::dsn::rasn::rpc::request_type, ::dsn::rasn::rpc::response_type>(request); \
    }

RASN_DEFINE_REPLICA_DISPATCH(agent_control_request, agent_control_response)
RASN_DEFINE_REPLICA_DISPATCH(message_bus_request, message_bus_response)
RASN_DEFINE_REPLICA_DISPATCH(task_orchestration_request, task_orchestration_response)
RASN_DEFINE_REPLICA_DISPATCH(determinism_request, determinism_response)
RASN_DEFINE_REPLICA_DISPATCH(capability_directory_request, capability_directory_response)
RASN_DEFINE_REPLICA_DISPATCH(resource_budget_request, resource_budget_response)
RASN_DEFINE_REPLICA_DISPATCH(recovery_supervisor_request, recovery_supervisor_response)
RASN_DEFINE_REPLICA_DISPATCH(blackboard_request, blackboard_response)
RASN_DEFINE_REPLICA_DISPATCH(contract_verifier_request, contract_verifier_response)
RASN_DEFINE_REPLICA_DISPATCH(human_interaction_rpc_request, human_interaction_rpc_response)
RASN_DEFINE_REPLICA_DISPATCH(sandbox_runtime_request, sandbox_runtime_response)

#undef RASN_DEFINE_REPLICA_DISPATCH

bool rasn_runtime_replica_store::checkpoint_records(::dsn_gpid gpid,
                                                    int64_t decree,
                                                    std::vector<state_record> *records,
                                                    std::string *error) const
{
    if (records == nullptr || gpid.u.app_id <= 0 || gpid.u.partition_index < 0 || decree < 0)
    {
        if (error != nullptr)
        {
            *error = "replicated runtime checkpoint context is invalid";
        }
        return false;
    }
    std::lock_guard<std::mutex> guard(_impl->lock);
    *records = _impl->store->checkpoint_records(_impl->module);
    size_t index = 0;
    for (const std::string &signature : _impl->order)
    {
        const std::map<std::string, replicated_dedup_entry>::const_iterator response =
            _impl->dedup.find(signature);
        if (response == _impl->dedup.end())
        {
            continue;
        }
        std::ostringstream key;
        key << std::setw(20) << std::setfill('0') << index++;
        records->push_back(make_replicated_runtime_state_record(
            _impl->module,
            "dedup",
            key.str(),
            encode_replicated_dedup_payload(signature, response->second)));
    }
    std::set<std::string> keys;
    for (const state_record &record : *records)
    {
        if (!keys.insert(record.key).second)
        {
            if (error != nullptr)
            {
                *error = "replicated runtime checkpoint contains colliding record keys";
            }
            return false;
        }
    }
    const uint64_t digest = replicated_runtime_records_digest(*records);
    records->push_back(make_replicated_runtime_state_record(
        _impl->module,
        "manifest",
        "checkpoint",
        encode_replicated_runtime_manifest(
            _impl->module, _impl->dedup_capacity, gpid, decree, records->size(), digest)));
    if (error != nullptr)
    {
        error->clear();
    }
    return true;
}

bool rasn_runtime_replica_store::validate_checkpoint_records(const std::vector<state_record> &records,
                                                             ::dsn_gpid gpid,
                                                             int64_t decree,
                                                             std::string *error) const
{
    std::unique_ptr<rasn_runtime_service_store> store;
    std::map<std::string, replicated_dedup_entry> dedup;
    std::deque<std::string> order;
    return build_replicated_runtime_store(
        _impl->module, records, _impl->dedup_capacity, gpid, decree, &store, &dedup, &order, error);
}

bool rasn_runtime_replica_store::replace_checkpoint_records(const std::vector<state_record> &records,
                                                            ::dsn_gpid gpid,
                                                            int64_t decree,
                                                            std::string *error)
{
    std::unique_ptr<rasn_runtime_service_store> store;
    std::map<std::string, replicated_dedup_entry> dedup;
    std::deque<std::string> order;
    if (!build_replicated_runtime_store(
            _impl->module,
            records,
            _impl->dedup_capacity,
            gpid,
            decree,
            &store,
            &dedup,
            &order,
            error))
    {
        return false;
    }
    std::lock_guard<std::mutex> guard(_impl->lock);
    _impl->store.swap(store);
    _impl->dedup.swap(dedup);
    _impl->order.swap(order);
    return true;
}

bool runtime_provider_internal::replica_store_test_accessor::replace_mirrored_state_records(
    rasn_runtime_replica_store &replica_store,
    const std::vector<state_record> &records,
    const std::vector<uint32_t> &hosted_shards,
    size_t *applied,
    std::string *error)
{
    std::unique_ptr<rasn_runtime_service_store> store(
        new rasn_runtime_service_store(replica_store._impl->module));
    const std::map<std::string, std::vector<uint32_t>> shard_ownership = {
        {replica_store._impl->module, hosted_shards}};
    if (!store->hydrate_from_state(
            records,
            std::vector<std::string>{replica_store._impl->module},
            applied,
            error,
            &shard_ownership))
    {
        return false;
    }
    std::lock_guard<std::mutex> guard(replica_store._impl->lock);
    replica_store._impl->store.swap(store);
    replica_store._impl->dedup.clear();
    replica_store._impl->order.clear();
    return true;
}

uint32_t runtime_provider_internal::replica_store_test_accessor::partition_for_key(
    const std::string &module, const std::string &key)
{
    return rasn_runtime_partition_for_key(module, key);
}

const std::string &rasn_runtime_replica_store::module() const { return _impl->module; }

size_t rasn_runtime_replica_store::dedup_capacity() const { return _impl->dedup_capacity; }

#define RASN_DEFINE_RUNTIME_ROUTING(request_type)                                                        \
    uint64_t rasn_runtime_partition_hash(const ::dsn::rasn::rpc::request_type &request)                  \
    {                                                                                                   \
        return rasn_runtime_partition_hash_impl(request);                                               \
    }                                                                                                   \
    ::dsn::task_code rasn_runtime_rpc_code_for_request(                                                 \
        const ::dsn::rasn::rpc::request_type &request)                                                  \
    {                                                                                                   \
        return rpc_code_for_module(runtime_module_name(request), runtime_request_is_mutating(request)); \
    }

RASN_DEFINE_RUNTIME_ROUTING(agent_control_request)
RASN_DEFINE_RUNTIME_ROUTING(message_bus_request)
RASN_DEFINE_RUNTIME_ROUTING(task_orchestration_request)
RASN_DEFINE_RUNTIME_ROUTING(determinism_request)
RASN_DEFINE_RUNTIME_ROUTING(capability_directory_request)
RASN_DEFINE_RUNTIME_ROUTING(resource_budget_request)
RASN_DEFINE_RUNTIME_ROUTING(recovery_supervisor_request)
RASN_DEFINE_RUNTIME_ROUTING(blackboard_request)
RASN_DEFINE_RUNTIME_ROUTING(contract_verifier_request)
RASN_DEFINE_RUNTIME_ROUTING(human_interaction_rpc_request)
RASN_DEFINE_RUNTIME_ROUTING(sandbox_runtime_request)

#undef RASN_DEFINE_RUNTIME_ROUTING

std::vector<std::string> rasn_runtime_module_names()
{
    return std::vector<std::string>{"agent_control_plane",
                                    "agent_message_bus",
                                    "task_orchestration_kernel",
                                    "determinism_ledger",
                                    "capability_directory",
                                    "resource_budget",
                                    "recovery_supervisor",
                                    "blackboard",
                                    "contract_verifier",
                                    "human_interaction",
                                    "sandbox_runtime"};
}

std::vector<std::string> rasn_runtime_module_ownership_resources_for(const std::string &module,
                                                                     const std::vector<uint32_t> &hosted_shards,
                                                                     bool sharded,
                                                                     uint32_t partition_count)
{
    std::vector<std::string> resources;
    if (!hosted_shards.empty())
    {
        // Explicit hosted-shard subset: lock exactly those shards.
        for (const uint32_t shard : hosted_shards)
        {
            resources.push_back(rasn_runtime_module_shard_capability(module, shard));
        }
        return resources;
    }

    if (sharded && partition_count > 1)
    {
        // Whole-module host of a sharded module (no explicit hosted subset): it
        // serves EVERY shard, so it must single-writer own every shard resource.
        // Locking only the unqualified rasn.runtime.<module> resource would not
        // contend with a peer that locks rasn.runtime.<module>.shard.N, letting a
        // whole-module owner and a shard-N owner both mutate shard N (split brain).
        for (uint32_t shard = 0; shard < partition_count; ++shard)
        {
            resources.push_back(rasn_runtime_module_shard_capability(module, shard));
        }
        return resources;
    }

    // Unsharded module (a single logical partition): one module-level lock.
    resources.push_back(rasn_runtime_module_capability(module));
    return resources;
}

std::vector<std::string> rasn_runtime_module_ownership_resources(const std::vector<std::string> &modules)
{
    std::vector<std::string> resources;
    for (const std::string &module : modules)
    {
        const std::vector<std::string> module_resources =
            rasn_runtime_module_ownership_resources_for(module,
                                                        rasn_runtime_hosted_shards(module),
                                                        rasn_runtime_module_is_sharded(module),
                                                        rasn_runtime_partition_count(module));
        resources.insert(resources.end(), module_resources.begin(), module_resources.end());
    }
    return resources;
}

// Defined at dsn::rasn scope (not the anonymous namespace above) so the unit
// tests can link against it via the declaration in runtime_provider.h. It still
// calls the anonymous-namespace partition helpers, which remain visible in the
// enclosing namespace within this translation unit.
#define RASN_DEFINE_HOSTS_REQUEST(request_type)                                                          \
    bool rasn_runtime_service_hosts_request(const ::dsn::rasn::rpc::request_type &request,              \
                                            const std::vector<uint32_t> &hosted_shards)                  \
    {                                                                                                   \
        if (hosted_shards.empty())                                                                      \
        {                                                                                               \
            return true;                                                                                \
        }                                                                                               \
        const uint32_t partition = rasn_runtime_partition_for_request(request);                         \
        return std::find(hosted_shards.begin(), hosted_shards.end(), partition) != hosted_shards.end(); \
    }

RASN_DEFINE_HOSTS_REQUEST(agent_control_request)
RASN_DEFINE_HOSTS_REQUEST(message_bus_request)
RASN_DEFINE_HOSTS_REQUEST(task_orchestration_request)
RASN_DEFINE_HOSTS_REQUEST(determinism_request)
RASN_DEFINE_HOSTS_REQUEST(capability_directory_request)
RASN_DEFINE_HOSTS_REQUEST(resource_budget_request)
RASN_DEFINE_HOSTS_REQUEST(recovery_supervisor_request)
RASN_DEFINE_HOSTS_REQUEST(blackboard_request)
RASN_DEFINE_HOSTS_REQUEST(contract_verifier_request)
RASN_DEFINE_HOSTS_REQUEST(human_interaction_rpc_request)
RASN_DEFINE_HOSTS_REQUEST(sandbox_runtime_request)

#undef RASN_DEFINE_HOSTS_REQUEST

std::vector<rasn_runtime_descriptor> rasn_runtime_module_descriptors()
{
    std::vector<rasn_runtime_descriptor> descriptors;
    const auto add = [&descriptors](const char *name, const char *consistency, const char *summary) {
        rasn_runtime_descriptor descriptor;
        descriptor.name = name;
        descriptor.role = rasn_runtime_module_app_role(name);
        descriptor.consistency = consistency;
        descriptor.stateful = true;
        descriptor.summary = summary;
        descriptors.push_back(descriptor);
    };
    add("agent_control_plane", "replicated", "agent registration, ownership leases, and heartbeats");
    add("agent_message_bus", "sharded", "inter-agent messaging with ack/dead-letter delivery");
    add("task_orchestration_kernel", "replicated", "task graph scheduling and lifecycle");
    add("determinism_ledger", "replicated", "append-only record of deterministic choices for replay");
    add("capability_directory", "replicated", "catalog of capability providers");
    add("resource_budget", "sharded", "per-scope resource quotas and reservations");
    add("recovery_supervisor", "replicated", "failure policies and recovery actions");
    add("blackboard", "sharded", "shared key/value coordination state with TTLs");
    add("contract_verifier", "replicated", "input/output contract registration and evaluation");
    add("human_interaction", "sharded", "human-in-the-loop request queue partitioned by request id");
    add("sandbox_runtime", "singleton", "sandbox profile and access decisions");
    return descriptors;
}

std::string rasn_runtime_module_app_role(const std::string &module_or_role)
{
    const std::string value = lower_ascii(trim(module_or_role));
    if (value == "rasn.runtime.agent_control" || value == "rasn.common.agent_control" || value == "agent_control" ||
        value == "agent_control_plane")
    {
        return "rasn.runtime.agent_control";
    }
    if (value == "rasn.runtime.message_bus" || value == "rasn.common.message_bus" || value == "message_bus" ||
        value == "agent_message_bus")
    {
        return "rasn.runtime.message_bus";
    }
    if (value == "rasn.runtime.task_kernel" || value == "rasn.common.task_kernel" || value == "task_kernel" ||
        value == "task_orchestration" || value == "task_orchestration_kernel")
    {
        return "rasn.runtime.task_kernel";
    }
    if (value == "rasn.runtime.determinism" || value == "rasn.common.determinism" || value == "determinism" ||
        value == "determinism_ledger")
    {
        return "rasn.runtime.determinism";
    }
    if (value == "rasn.runtime.capability" || value == "rasn.common.capability" || value == "capability" ||
        value == "capability_directory")
    {
        return "rasn.runtime.capability";
    }
    if (value == "rasn.runtime.budget" || value == "rasn.common.budget" || value == "budget" || value == "resource_budget")
    {
        return "rasn.runtime.budget";
    }
    if (value == "rasn.runtime.recovery" || value == "rasn.common.recovery" || value == "recovery" ||
        value == "recovery_supervisor")
    {
        return "rasn.runtime.recovery";
    }
    if (value == "rasn.runtime.blackboard" || value == "rasn.common.blackboard" || value == "blackboard")
    {
        return "rasn.runtime.blackboard";
    }
    if (value == "rasn.runtime.contract" || value == "rasn.common.contract" || value == "contract" ||
        value == "contract_verifier")
    {
        return "rasn.runtime.contract";
    }
    if (value == "rasn.runtime.human_interaction" || value == "rasn.common.human_interaction" ||
        value == "human_interaction")
    {
        return "rasn.runtime.human_interaction";
    }
    if (value == "rasn.runtime.sandbox_runtime" || value == "rasn.common.sandbox_runtime" || value == "sandbox" ||
        value == "sandbox_runtime")
    {
        return "rasn.runtime.sandbox_runtime";
    }
    if (value == "rasn.runtime" || value == "rasn.common.modules" || value == "modules")
    {
        return "rasn.runtime";
    }
    return "";
}

std::string normalize_rasn_runtime_app_list(const std::string &app_list)
{
    std::vector<std::string> normalized;
    std::string token;
    for (size_t i = 0; i <= app_list.size(); ++i)
    {
        if (i == app_list.size() || app_list[i] == ';' || app_list[i] == ',')
        {
            const std::string trimmed = trim(token);
            if (!trimmed.empty())
            {
                const size_t at = trimmed.find('@');
                const std::string app = at == std::string::npos ? trimmed : trimmed.substr(0, at);
                const std::string suffix = at == std::string::npos ? "" : trimmed.substr(at);
                const std::string role = rasn_runtime_module_app_role(app);
                normalized.push_back((role.empty() ? app : role) + suffix);
            }
            token.clear();
        }
        else
        {
            token.push_back(app_list[i]);
        }
    }
    return join_strings(normalized, ";");
}

rasn_runtime_state_compaction_report compact_rasn_runtime_state_mirror(rasn_service_graph &services,
                                                                       const std::string &checkpoint_path,
                                                                       const std::string &state_prefix)
{
    rasn_runtime_state_compaction_report report;
    report.state_prefix = state_prefix.empty() ? load_rasn_runtime_config().state_prefix : state_prefix;

    state_query_request query;
    query.key_prefix = rasn_runtime_state_prefix(report.state_prefix);
    const state_response queried = services.query_state(query);
    if (!queried.ok)
    {
        report.ok = false;
        report.error = queried.error.empty() ? "failed to query rASN runtime state mirror" : queried.error;
        return report;
    }

    report.queried_records = queried.records.size();
    const std::vector<std::string> modules = rasn_runtime_module_names();
    for (const state_record &record : queried.records)
    {
        std::string module;
        std::string kind;
        if (!parse_rasn_runtime_record_kind(record.kind, &module, &kind) || !has_module(modules, module))
        {
            continue;
        }
        if (kind == "watermark")
        {
            ++report.watermark_records;
        }
        else
        {
            ++report.runtime_records;
        }
    }

    std::string watermark_error;
    if (!verify_runtime_state_watermarks(queried.records, modules, report.state_prefix, &watermark_error))
    {
        report.ok = false;
        report.error = watermark_error.empty() ? "runtime state watermark verification failed" : watermark_error;
        return report;
    }

    state_checkpoint_request checkpoint;
    checkpoint.path = checkpoint_path;
    const state_checkpoint_result checkpointed =
        services.checkpoint_state_detailed(checkpoint);
    if (!checkpointed.response.ok)
    {
        report.ok = false;
        report.error = checkpointed.response.error.empty()
                           ? "failed to checkpoint rASN state mirror"
                           : checkpointed.response.error;
        return report;
    }

    report.checkpointed_records = checkpointed.response.records.size();
    report.last_sequence = checkpointed.response.last_sequence;
    report.checkpoint_path = checkpointed.checkpoint_path;
    report.recovery_journal_compacted = checkpointed.journal_compacted;
    report.compaction_details_available = checkpointed.details_available;
    return report;
}

rasn_runtime::rasn_runtime(std::unique_ptr<rasn_runtime_provider> provider) : _provider(std::move(provider)) {}

rasn_runtime::~rasn_runtime() {}

std::string rasn_runtime::provider_name() const
{
    return _provider->provider_name();
}

bool rasn_runtime::distributed() const
{
    return _provider->distributed();
}

bool rasn_runtime::strict() const
{
    return _provider->strict();
}

const std::string &rasn_runtime::state_prefix() const
{
    return _provider->state_prefix();
}

std::string rasn_runtime::summary_header() const
{
    return _provider->summary_header();
}

bool rasn_runtime::upsert_agent(const agent_control_record &record, std::string *error)
{
    return _provider->upsert_agent(record, error);
}

agent_control_lease rasn_runtime::acquire_agent_lease(const std::string &agent_id,
                                                        const std::string &owner,
                                                        uint64_t now_ms,
                                                        uint64_t lease_ms)
{
    return _provider->acquire_agent_lease(agent_id, owner, now_ms, lease_ms);
}

bool rasn_runtime::heartbeat_agent(const std::string &agent_id, uint64_t now_ms, std::string *error)
{
    return _provider->heartbeat_agent(agent_id, now_ms, error);
}

bool rasn_runtime::find_agent(const std::string &agent_id, agent_control_record *record) const
{
    return _provider->find_agent(agent_id, record);
}

size_t rasn_runtime::expire_agent_leases(uint64_t now_ms)
{
    return _provider->expire_agent_leases(now_ms);
}

std::vector<agent_control_record> rasn_runtime::list_agents(bool include_expired, uint64_t now_ms) const
{
    return _provider->list_agents(include_expired, now_ms);
}

std::string rasn_runtime::describe_agents(uint64_t now_ms) const
{
    return _provider->describe_agents(now_ms);
}

bool rasn_runtime::publish_message(const agent_message &message, agent_message *stored, std::string *error)
{
    return _provider->publish_message(message, stored, error);
}

bool rasn_runtime::ack_message(const std::string &message_id, std::string *error)
{
    return _provider->ack_message(message_id, error);
}

bool rasn_runtime::dead_letter_message(const std::string &message_id, const std::string &error_text, std::string *error)
{
    return _provider->dead_letter_message(message_id, error_text, error);
}

bool rasn_runtime::find_message(const std::string &message_id, agent_message *message) const
{
    return _provider->find_message(message_id, message);
}

std::vector<agent_message> rasn_runtime::message_snapshot() const
{
    return _provider->message_snapshot();
}

bool rasn_runtime::add_task(const orchestration_task &task, std::string *error)
{
    return _provider->add_task(task, error);
}

bool rasn_runtime::start_task(const std::string &task_id, const std::string &owner_agent, std::string *error)
{
    return _provider->start_task(task_id, owner_agent, error);
}

bool rasn_runtime::complete_task(const std::string &task_id, const std::string &output, std::string *error)
{
    return _provider->complete_task(task_id, output, error);
}

bool rasn_runtime::fail_task(const std::string &task_id, const std::string &error_text, bool retryable, std::string *error)
{
    return _provider->fail_task(task_id, error_text, retryable, error);
}

bool rasn_runtime::find_task(const std::string &task_id, orchestration_task *task) const
{
    return _provider->find_task(task_id, task);
}

std::vector<orchestration_task> rasn_runtime::task_snapshot() const
{
    return _provider->task_snapshot();
}

std::vector<orchestration_task> rasn_runtime::ready_tasks(uint64_t now_ms) const
{
    return _provider->ready_tasks(now_ms);
}

std::vector<orchestration_task> rasn_runtime::blocked_tasks() const
{
    return _provider->blocked_tasks();
}

bool rasn_runtime::record_choice(const std::string &task_id,
                                   const std::string &key,
                                   const std::string &source,
                                   const std::string &value,
                                   deterministic_choice *choice,
                                   std::string *error)
{
    return _provider->record_choice(task_id, key, source, value, choice, error);
}

std::vector<deterministic_choice> rasn_runtime::choice_snapshot() const
{
    return _provider->choice_snapshot();
}

bool rasn_runtime::upsert_capability_provider(const capability_provider &provider, std::string *error)
{
    return _provider->upsert_capability_provider(provider, error);
}

std::string rasn_runtime::describe_capabilities() const
{
    return _provider->describe_capabilities();
}

bool rasn_runtime::configure_budget(const resource_quota &quota, std::string *error)
{
    return _provider->configure_budget(quota, error);
}

resource_budget_decision rasn_runtime::reserve_budget(const resource_request &request)
{
    return _provider->reserve_budget(request);
}

bool rasn_runtime::release_budget(const resource_request &request, std::string *error)
{
    return _provider->release_budget(request, error);
}

bool rasn_runtime::budget_usage(const std::string &scope, resource_usage *usage) const
{
    return _provider->budget_usage(scope, usage);
}

std::string rasn_runtime::describe_budgets() const
{
    return _provider->describe_budgets();
}

bool rasn_runtime::set_recovery_policy(const recovery_policy &policy, std::string *error)
{
    return _provider->set_recovery_policy(policy, error);
}

recovery_action rasn_runtime::observe_failure(const failure_observation &failure)
{
    return _provider->observe_failure(failure);
}

std::string rasn_runtime::describe_recovery() const
{
    return _provider->describe_recovery();
}

bool rasn_runtime::put_blackboard(const blackboard_entry &entry, blackboard_entry *stored, std::string *error)
{
    return _provider->put_blackboard(entry, stored, error);
}

bool rasn_runtime::get_blackboard(const std::string &key, blackboard_entry *entry) const
{
    return _provider->get_blackboard(key, entry);
}

std::vector<blackboard_entry> rasn_runtime::blackboard_snapshot(bool include_expired, uint64_t now_ms) const
{
    return _provider->blackboard_snapshot(include_expired, now_ms);
}

bool rasn_runtime::register_contract(const agent_contract &contract, std::string *error)
{
    return _provider->register_contract(contract, error);
}

contract_evaluation rasn_runtime::evaluate_input(const std::string &contract_id, const std::string &input) const
{
    return _provider->evaluate_input(contract_id, input);
}

contract_evaluation rasn_runtime::evaluate_output(const std::string &contract_id,
                                                    const std::string &output,
                                                    const std::vector<std::string> &policy_labels) const
{
    return _provider->evaluate_output(contract_id, output, policy_labels);
}

std::string rasn_runtime::describe_contracts() const
{
    return _provider->describe_contracts();
}

human_interaction_result
rasn_runtime::open_human_interaction(const human_interaction_request &request)
{
    return _provider->open_human_interaction(request);
}

human_interaction_result rasn_runtime::answer_human_interaction(const std::string &request_id,
                                                                const std::string &answer)
{
    return _provider->answer_human_interaction(request_id, answer);
}

human_interaction_result rasn_runtime::cancel_human_interaction(const std::string &request_id,
                                                                const std::string &reason)
{
    return _provider->cancel_human_interaction(request_id, reason);
}

bool rasn_runtime::find_human_interaction(const std::string &request_id,
                                          human_interaction_request *request) const
{
    return _provider->find_human_interaction(request_id, request);
}

size_t rasn_runtime::expire_human_interactions(uint64_t now_ms)
{
    return _provider->expire_human_interactions(now_ms);
}

std::vector<human_interaction_request> rasn_runtime::human_snapshot() const
{
    return _provider->human_snapshot();
}

std::vector<human_interaction_request>
rasn_runtime::pending_human(const std::string &requester) const
{
    return _provider->pending_human(requester);
}

void rasn_runtime::set_sandbox_profile(const sandbox_profile &profile)
{
    _provider->set_sandbox_profile(profile);
}

sandbox_decision rasn_runtime::evaluate_sandbox(const sandbox_request &request) const
{
    return _provider->evaluate_sandbox(request);
}

sandbox_profile rasn_runtime::sandbox() const
{
    return _provider->sandbox();
}

bool rasn_runtime::mirror_state(const std::string &module,
                                  const std::string &kind,
                                  const std::string &key,
                                  const std::string &value,
                                  std::string *error)
{
    return _provider->mirror_state(module, kind, key, value, error);
}

bool rasn_runtime::ping_module(const std::string &module, std::string *error) const
{
    return _provider->ping_module(module, error);
}

std::vector<std::pair<std::string, bool>> rasn_runtime::module_health() const
{
    std::vector<std::pair<std::string, bool>> health;
    const std::vector<std::string> modules = rasn_runtime_module_names();
    health.reserve(modules.size());
    for (const std::string &module : modules)
    {
        std::string error;
        health.push_back(std::make_pair(module, _provider->ping_module(module, &error)));
    }
    return health;
}

std::string rasn_runtime::describe_module_health() const
{
    const std::vector<std::pair<std::string, bool>> health = module_health();
    size_t reachable = 0;
    for (const std::pair<std::string, bool> &entry : health)
    {
        if (entry.second)
        {
            ++reachable;
        }
    }
    std::ostringstream output;
    output << "module_health: provider=" << provider_name() << " mode=" << (distributed() ? "distributed" : "local")
           << " reachable=" << reachable << "/" << health.size();
    for (const std::pair<std::string, bool> &entry : health)
    {
        output << "\n  " << entry.first << "=" << (entry.second ? "ok" : "unreachable");
    }
    return output.str();
}

std::string rasn_runtime::describe_topology() const
{
    return _provider->describe_topology();
}

rasn_runtime_provider::rasn_runtime_provider(rasn_runtime_config config)
    : _config(std::move(config))
{
    if (_config.state_prefix.empty())
    {
        _config.state_prefix = "rasn/runtime";
    }
}

std::string rasn_runtime_provider::summary_header() const
{
    std::ostringstream output;
    output << "runtime_provider: provider=" << provider_name()
           << " mode=" << provider_name()
           << " module_api=" << (distributed() ? "rpc" : "lpc")
           << " state_service=" << (distributed() ? "enabled" : "disabled")
           << " state_prefix=" << _config.state_prefix
           << " strict=" << (_config.strict ? "yes" : "no");
    return output.str();
}

std::string rasn_runtime_provider::describe_topology() const
{
    const std::vector<rasn_runtime_descriptor> descriptors = rasn_runtime_module_descriptors();
    size_t remote = 0;
    for (const rasn_runtime_descriptor &descriptor : descriptors)
    {
        if (module_routed_remote(descriptor.name))
        {
            ++remote;
        }
    }
    std::ostringstream output;
    output << "runtime_topology: provider=" << provider_name() << " modules=" << descriptors.size()
           << " remote=" << remote << " local=" << (descriptors.size() - remote);
    for (const rasn_runtime_descriptor &descriptor : descriptors)
    {
        const bool routed_remote = module_routed_remote(descriptor.name);
        output << "\n  " << descriptor.name << " routing=" << (routed_remote ? "remote" : "local")
               << " endpoint=" << module_endpoint(descriptor.name) << " role=" << descriptor.role
               << " consistency=" << descriptor.consistency << "(intended)"
               << " actual="
               << (routed_remote && rasn_runtime_module_uses_native_replication(descriptor.name)
                       ? "rdsn_type1_replica_group"
                       : "single_writer_in_memory")
               << " stateful=" << (descriptor.stateful ? "yes" : "no");
    }
    return output.str();
}

bool rasn_runtime_provider::mirror_state(const std::string &module,
                                           const std::string &kind,
                                           const std::string &key,
                                           const std::string &value,
                                           std::string *error)
{
    return write_state(module, kind, key, value, error);
}

bool rasn_runtime_provider::mirror_state_after_success(const std::string &module,
                                                       const std::string &kind,
                                                       const std::string &key,
                                                       const std::string &value,
                                                       std::string *error)
{
    std::string write_error;
    if (!write_state(module, kind, key, value, &write_error))
    {
        dwarn("%s runtime module state mirror failed for %s/%s/%s: %s",
              strict() ? "strict" : "distributed",
              module.c_str(),
              kind.c_str(),
              key.c_str(),
              write_error.c_str());
        if (strict())
        {
            if (error != nullptr)
            {
                *error = write_error;
            }
            return false;
        }
    }
    clear_error(error);
    return true;
}

bool rasn_runtime_provider::ping_module(const std::string &module, std::string *error) const
{
    return ping_local_module(module, error);
}

#define RASN_DEFINE_PROVIDER_SHARDS(request_type, response_type)                                        \
    std::vector<::dsn::rasn::rpc::response_type> rasn_runtime_provider::call_module_api_shards(         \
        const ::dsn::rasn::rpc::request_type &request) const                                            \
    {                                                                                                   \
        const std::string module = runtime_module_name(request);                                        \
        const uint32_t partition_count = rasn_runtime_partition_count(module);                          \
        if (!module_routed_remote(module) || partition_count <= 1)                                      \
        {                                                                                               \
            return std::vector<::dsn::rasn::rpc::response_type>{call_module_api(request)};              \
        }                                                                                               \
        std::vector<::dsn::rasn::rpc::response_type> responses;                                        \
        responses.reserve(partition_count);                                                             \
        std::set<std::string> queried_endpoints;                                                        \
        for (uint32_t i = 0; i < partition_count; ++i)                                                  \
        {                                                                                               \
            const runtime_endpoint endpoint = resolve_rasn_runtime_partition_endpoint(module, i);       \
            if (endpoint.address.type() != HOST_TYPE_URI &&                                             \
                !queried_endpoints.insert(std::string(endpoint.address.to_string())).second)            \
            {                                                                                           \
                continue;                                                                               \
            }                                                                                           \
            ::dsn::rasn::rpc::request_type shard_request = request;                                    \
            initialize_runtime_request(&shard_request);                                                 \
            shard_request.metadata.__set_route_partition(endpoint.partition_index);                     \
            ::dsn::rasn::rpc::response_type response = call_module_api(shard_request);                  \
            response.metadata.__set_route_partition(endpoint.partition_index);                          \
            responses.push_back(response);                                                              \
        }                                                                                               \
        return responses;                                                                               \
    }

RASN_DEFINE_PROVIDER_SHARDS(message_bus_request, message_bus_response)
RASN_DEFINE_PROVIDER_SHARDS(resource_budget_request, resource_budget_response)
RASN_DEFINE_PROVIDER_SHARDS(blackboard_request, blackboard_response)
RASN_DEFINE_PROVIDER_SHARDS(human_interaction_rpc_request, human_interaction_rpc_response)

#undef RASN_DEFINE_PROVIDER_SHARDS

std::string rasn_runtime_provider::state_key(const std::string &module,
                                               const std::string &kind,
                                               const std::string &key) const
{
    return rasn_runtime_state_key(_config.state_prefix, module, kind, key);
}

void rasn_runtime_provider::set_sandbox_profile(const sandbox_profile &profile)
{
    ::dsn::rasn::rpc::sandbox_runtime_request request;
    request.__set_operation(::dsn::rasn::rpc::sandbox_runtime_operation::set_profile);
    request.__set_set_profile(to_wire(profile));
    const ::dsn::rasn::rpc::sandbox_runtime_response response = call_module_api(request);
    if (!runtime_status_ok(response.status))
    {
        dwarn("failed to set sandbox runtime profile through module API: %s",
              runtime_error_message(response).c_str());
        return;
    }
    mirror_state_after_success("sandbox_runtime", "profile", profile.name, encode_sandbox_profile_payload(profile));
}

sandbox_decision rasn_runtime_provider::evaluate_sandbox(const sandbox_request &request) const
{
    ::dsn::rasn::rpc::sandbox_runtime_request rpc_request;
    rpc_request.__set_operation(::dsn::rasn::rpc::sandbox_runtime_operation::evaluate);
    rpc_request.__set_evaluate(to_wire(request));
    const ::dsn::rasn::rpc::sandbox_runtime_response response = call_module_api(rpc_request);
    if (!runtime_status_ok(response.status))
    {
        return denied_sandbox_decision(runtime_error_message(response));
    }
    sandbox_decision decision;
    std::string error;
    if (!response.__isset.decision || !from_wire(response.decision, &decision, &error))
    {
        return denied_sandbox_decision(error);
    }
    return decision;
}

sandbox_profile rasn_runtime_provider::sandbox() const
{
    ::dsn::rasn::rpc::sandbox_runtime_request request;
    request.__set_operation(::dsn::rasn::rpc::sandbox_runtime_operation::get_profile);
    const ::dsn::rasn::rpc::sandbox_runtime_response response = call_module_api(request);
    sandbox_profile profile = default_read_only_sandbox_profile();
    if (!runtime_status_ok(response.status))
    {
        profile.name = "unavailable";
        return profile;
    }
    std::string error;
    if (!response.__isset.profile || !from_wire(response.profile, &profile, &error))
    {
        profile = default_read_only_sandbox_profile();
        profile.name = "unavailable";
    }
    return profile;
}

bool rasn_runtime_provider::upsert_agent(const agent_control_record &record, std::string *error)
{
    agent_control_record stored_record = record;
    if (stored_record.last_heartbeat_ms == 0)
    {
        stored_record.last_heartbeat_ms = ::dsn_now_ms();
    }
    ::dsn::rasn::rpc::agent_control_request request;
    request.__set_operation(::dsn::rasn::rpc::agent_control_operation::upsert_agent);
    request.__set_upsert_agent(to_wire(stored_record));
    const ::dsn::rasn::rpc::agent_control_response response = call_module_api(request);
    if (!response_bool(response, error))
    {
        return false;
    }
    agent_control_record stored;
    if (find_agent(record.descriptor.agent_id, &stored))
    {
        return mirror_state_after_success(
            "agent_control_plane", "agent", stored.descriptor.agent_id, encode_agent_control_payload(stored), error);
    }
    return true;
}

agent_control_lease rasn_runtime_provider::acquire_agent_lease(const std::string &agent_id,
                                                                 const std::string &owner,
                                                                 uint64_t now_ms,
                                                                 uint64_t lease_ms)
{
    ::dsn::rasn::rpc::agent_control_acquire_lease_request body;
    body.agent_id = agent_id;
    body.owner = owner;
    body.now_ms = encode_runtime_unsigned<int64_t>(now_ms);
    body.lease_ms = encode_runtime_unsigned<int64_t>(lease_ms);
    ::dsn::rasn::rpc::agent_control_request request;
    request.__set_operation(::dsn::rasn::rpc::agent_control_operation::acquire_lease);
    request.__set_acquire_lease(body);
    const ::dsn::rasn::rpc::agent_control_response response = call_module_api(request);
    agent_control_lease lease;
    lease.agent_id = agent_id;
    lease.owner = owner;
    if (!runtime_status_ok(response.status))
    {
        lease.error = runtime_error_message(response);
        return lease;
    }
    std::string error;
    if (!response.__isset.lease || !from_wire(response.lease, &lease, &error))
    {
        lease.ok = false;
        lease.error = error;
    }
    if (lease.ok)
    {
        agent_control_record stored;
        if (find_agent(agent_id, &stored))
        {
            std::string mirror_error;
            if (!mirror_state_after_success("agent_control_plane",
                                            "agent",
                                            stored.descriptor.agent_id,
                                            encode_agent_control_payload(stored),
                                            &mirror_error))
            {
                lease.ok = false;
                lease.error = mirror_error;
            }
        }
    }
    return lease;
}

bool rasn_runtime_provider::heartbeat_agent(const std::string &agent_id, uint64_t now_ms, std::string *error)
{
    ::dsn::rasn::rpc::agent_control_heartbeat_request body;
    body.agent_id = agent_id;
    body.now_ms = encode_runtime_unsigned<int64_t>(now_ms);
    ::dsn::rasn::rpc::agent_control_request request;
    request.__set_operation(::dsn::rasn::rpc::agent_control_operation::heartbeat);
    request.__set_heartbeat(body);
    const ::dsn::rasn::rpc::agent_control_response response = call_module_api(request);
    if (!response_bool(response, error))
    {
        return false;
    }
    agent_control_record stored;
    if (find_agent(agent_id, &stored))
    {
        return mirror_state_after_success(
            "agent_control_plane", "agent", stored.descriptor.agent_id, encode_agent_control_payload(stored), error);
    }
    return true;
}

bool rasn_runtime_provider::find_agent(const std::string &agent_id, agent_control_record *record) const
{
    ::dsn::rasn::rpc::agent_control_find_request body;
    body.agent_id = agent_id;
    ::dsn::rasn::rpc::agent_control_request request;
    request.__set_operation(::dsn::rasn::rpc::agent_control_operation::find);
    request.__set_find(body);
    const ::dsn::rasn::rpc::agent_control_response response = call_module_api(request);
    if (!runtime_status_ok(response.status) || !response.__isset.agent)
    {
        return false;
    }
    std::string error;
    return from_wire(response.agent, record, &error);
}

size_t rasn_runtime_provider::expire_agent_leases(uint64_t now_ms)
{
    ::dsn::rasn::rpc::agent_control_expire_request body;
    body.now_ms = encode_runtime_unsigned<int64_t>(now_ms);
    ::dsn::rasn::rpc::agent_control_request request;
    request.__set_operation(::dsn::rasn::rpc::agent_control_operation::expire_leases);
    request.__set_expire_leases(body);
    const ::dsn::rasn::rpc::agent_control_response response = call_module_api(request);
    if (!runtime_status_ok(response.status))
    {
        return 0;
    }
    const size_t expired = response.__isset.count ? static_cast<size_t>(response.count) : 0;
    if (expired > 0)
    {
        const std::vector<agent_control_record> records = list_agents(false, now_ms);
        for (const agent_control_record &record : records)
        {
            mirror_state_after_success("agent_control_plane", "agent", record.descriptor.agent_id, encode_agent_control_payload(record));
        }
    }
    return expired;
}

std::vector<agent_control_record> rasn_runtime_provider::list_agents(bool include_expired, uint64_t now_ms) const
{
    ::dsn::rasn::rpc::agent_control_list_request body;
    body.include_expired = include_expired;
    body.now_ms = encode_runtime_unsigned<int64_t>(now_ms);
    ::dsn::rasn::rpc::agent_control_request request;
    request.__set_operation(::dsn::rasn::rpc::agent_control_operation::list_agents);
    request.__set_list_agents(body);
    const ::dsn::rasn::rpc::agent_control_response response = call_module_api(request);
    return runtime_status_ok(response.status) && response.__isset.agents
               ? from_wire_list<::dsn::rasn::rpc::wire_agent_control_record, agent_control_record>(
                     response.agents)
               : std::vector<agent_control_record>();
}

std::string rasn_runtime_provider::describe_agents(uint64_t now_ms) const
{
    ::dsn::rasn::rpc::agent_control_describe_request body;
    body.now_ms = encode_runtime_unsigned<int64_t>(now_ms);
    ::dsn::rasn::rpc::agent_control_request request;
    request.__set_operation(::dsn::rasn::rpc::agent_control_operation::describe);
    request.__set_describe(body);
    const ::dsn::rasn::rpc::agent_control_response response = call_module_api(request);
    return runtime_status_ok(response.status) && response.__isset.description
               ? response.description
               : runtime_error_message(response);
}

bool rasn_runtime_provider::publish_message(const agent_message &message, agent_message *stored, std::string *error)
{
    agent_message submitted = message;
    if (submitted.message_id.empty())
    {
        submitted.message_id = generate_rasn_runtime_request_id();
    }
    const uint64_t now_ms = ::dsn_now_ms();
    if (submitted.created_at_ms == 0)
    {
        submitted.created_at_ms = now_ms;
    }
    if (submitted.updated_at_ms == 0)
    {
        submitted.updated_at_ms = now_ms;
    }
    ::dsn::rasn::rpc::message_bus_request request;
    request.__set_operation(::dsn::rasn::rpc::message_bus_operation::publish);
    request.__set_publish(to_wire(submitted));
    const ::dsn::rasn::rpc::message_bus_response response = call_module_api(request);
    if (!runtime_status_ok(response.status))
    {
        set_response_error(response, error);
        return false;
    }
    agent_message stored_message;
    std::string decode_error;
    if (!response.__isset.message || !from_wire(response.message, &stored_message, &decode_error))
    {
        if (error != nullptr)
        {
            *error = decode_error;
        }
        return false;
    }
    if (stored != nullptr)
    {
        *stored = stored_message;
    }
    return mirror_state_after_success(
        "agent_message_bus", "message", stored_message.message_id, encode_message_payload(stored_message), error);
}

bool rasn_runtime_provider::ack_message(const std::string &message_id, std::string *error)
{
    ::dsn::rasn::rpc::message_ack_request body;
    body.message_id = message_id;
    body.now_ms = encode_runtime_unsigned<int64_t>(::dsn_now_ms());
    ::dsn::rasn::rpc::message_bus_request request;
    request.__set_operation(::dsn::rasn::rpc::message_bus_operation::acknowledge);
    request.__set_acknowledge(body);
    const ::dsn::rasn::rpc::message_bus_response response = call_module_api(request);
    if (!response_bool(response, error))
    {
        return false;
    }
    agent_message stored;
    if (find_message(message_id, &stored))
    {
        return mirror_state_after_success(
            "agent_message_bus", "message", stored.message_id, encode_message_payload(stored), error);
    }
    return true;
}

bool rasn_runtime_provider::dead_letter_message(const std::string &message_id,
                                                  const std::string &error_text,
                                                  std::string *error)
{
    ::dsn::rasn::rpc::message_dead_letter_request body;
    body.message_id = message_id;
    body.reason = error_text;
    body.now_ms = encode_runtime_unsigned<int64_t>(::dsn_now_ms());
    ::dsn::rasn::rpc::message_bus_request request;
    request.__set_operation(::dsn::rasn::rpc::message_bus_operation::dead_letter);
    request.__set_dead_letter(body);
    const ::dsn::rasn::rpc::message_bus_response response = call_module_api(request);
    if (!response_bool(response, error))
    {
        return false;
    }
    agent_message stored;
    if (find_message(message_id, &stored))
    {
        return mirror_state_after_success(
            "agent_message_bus", "message", stored.message_id, encode_message_payload(stored), error);
    }
    return true;
}

bool rasn_runtime_provider::find_message(const std::string &message_id, agent_message *message) const
{
    ::dsn::rasn::rpc::message_find_request body;
    body.message_id = message_id;
    ::dsn::rasn::rpc::message_bus_request request;
    request.__set_operation(::dsn::rasn::rpc::message_bus_operation::find);
    request.__set_find(body);
    const ::dsn::rasn::rpc::message_bus_response response = call_module_api(request);
    if (!runtime_status_ok(response.status) || !response.__isset.message)
    {
        return false;
    }
    std::string error;
    return from_wire(response.message, message, &error);
}

std::vector<agent_message> rasn_runtime_provider::message_snapshot() const
{
    std::vector<agent_message> messages;
    ::dsn::rasn::rpc::message_bus_request request;
    request.__set_operation(::dsn::rasn::rpc::message_bus_operation::snapshot);
    for (const ::dsn::rasn::rpc::message_bus_response &response : call_module_api_shards(request))
    {
        if (!runtime_status_ok(response.status))
        {
            dwarn("agent_message_bus snapshot shard %u failed: %s",
                  response_partition(response),
                  runtime_error_message(response).c_str());
            continue;
        }
        if (response.__isset.messages)
        {
            const std::vector<agent_message> shard_messages =
                from_wire_list<::dsn::rasn::rpc::wire_agent_message, agent_message>(
                    response.messages);
            messages.insert(messages.end(), shard_messages.begin(), shard_messages.end());
        }
    }
    return messages;
}

bool rasn_runtime_provider::add_task(const orchestration_task &task, std::string *error)
{
    ::dsn::rasn::rpc::task_orchestration_request request;
    request.__set_operation(::dsn::rasn::rpc::task_orchestration_operation::add_task);
    request.__set_add_task(to_wire(task));
    const ::dsn::rasn::rpc::task_orchestration_response response = call_module_api(request);
    if (!response_bool(response, error))
    {
        return false;
    }
    orchestration_task stored;
    if (find_task(task.task_id, &stored))
    {
        return mirror_state_after_success(
            "task_orchestration_kernel", "task", stored.task_id, encode_task_payload(stored), error);
    }
    return true;
}

bool rasn_runtime_provider::start_task(const std::string &task_id, const std::string &owner_agent, std::string *error)
{
    ::dsn::rasn::rpc::task_start_request body;
    body.task_id = task_id;
    body.owner_agent = owner_agent;
    ::dsn::rasn::rpc::task_orchestration_request request;
    request.__set_operation(::dsn::rasn::rpc::task_orchestration_operation::start);
    request.__set_start(body);
    const ::dsn::rasn::rpc::task_orchestration_response response = call_module_api(request);
    if (!response_bool(response, error))
    {
        return false;
    }
    orchestration_task stored;
    if (find_task(task_id, &stored))
    {
        return mirror_state_after_success(
            "task_orchestration_kernel", "task", stored.task_id, encode_task_payload(stored), error);
    }
    return true;
}

bool rasn_runtime_provider::complete_task(const std::string &task_id, const std::string &output, std::string *error)
{
    ::dsn::rasn::rpc::task_complete_request body;
    body.task_id = task_id;
    body.output = output;
    ::dsn::rasn::rpc::task_orchestration_request request;
    request.__set_operation(::dsn::rasn::rpc::task_orchestration_operation::complete);
    request.__set_complete(body);
    const ::dsn::rasn::rpc::task_orchestration_response response = call_module_api(request);
    if (!response_bool(response, error))
    {
        return false;
    }
    orchestration_task stored;
    if (find_task(task_id, &stored))
    {
        return mirror_state_after_success(
            "task_orchestration_kernel", "task", stored.task_id, encode_task_payload(stored), error);
    }
    return true;
}

bool rasn_runtime_provider::fail_task(const std::string &task_id,
                                        const std::string &error_text,
                                        bool retryable,
                                        std::string *error)
{
    ::dsn::rasn::rpc::task_fail_request body;
    body.task_id = task_id;
    body.error = error_text;
    body.retryable = retryable;
    ::dsn::rasn::rpc::task_orchestration_request request;
    request.__set_operation(::dsn::rasn::rpc::task_orchestration_operation::fail);
    request.__set_fail(body);
    const ::dsn::rasn::rpc::task_orchestration_response response = call_module_api(request);
    if (!response_bool(response, error))
    {
        return false;
    }
    orchestration_task stored;
    if (find_task(task_id, &stored))
    {
        return mirror_state_after_success(
            "task_orchestration_kernel", "task", stored.task_id, encode_task_payload(stored), error);
    }
    return true;
}

bool rasn_runtime_provider::find_task(const std::string &task_id, orchestration_task *task) const
{
    ::dsn::rasn::rpc::task_find_request body;
    body.task_id = task_id;
    ::dsn::rasn::rpc::task_orchestration_request request;
    request.__set_operation(::dsn::rasn::rpc::task_orchestration_operation::find);
    request.__set_find(body);
    const ::dsn::rasn::rpc::task_orchestration_response response = call_module_api(request);
    if (!runtime_status_ok(response.status) || !response.__isset.task)
    {
        return false;
    }
    std::string error;
    return from_wire(response.task, task, &error);
}

std::vector<orchestration_task> rasn_runtime_provider::task_snapshot() const
{
    ::dsn::rasn::rpc::task_orchestration_request request;
    request.__set_operation(::dsn::rasn::rpc::task_orchestration_operation::snapshot);
    const ::dsn::rasn::rpc::task_orchestration_response response = call_module_api(request);
    return runtime_status_ok(response.status) && response.__isset.tasks
               ? from_wire_list<::dsn::rasn::rpc::wire_orchestration_task, orchestration_task>(
                     response.tasks)
               : std::vector<orchestration_task>();
}

std::vector<orchestration_task> rasn_runtime_provider::ready_tasks(uint64_t now_ms) const
{
    ::dsn::rasn::rpc::task_ready_request body;
    body.now_ms = encode_runtime_unsigned<int64_t>(now_ms);
    ::dsn::rasn::rpc::task_orchestration_request request;
    request.__set_operation(::dsn::rasn::rpc::task_orchestration_operation::ready);
    request.__set_ready(body);
    const ::dsn::rasn::rpc::task_orchestration_response response = call_module_api(request);
    return runtime_status_ok(response.status) && response.__isset.tasks
               ? from_wire_list<::dsn::rasn::rpc::wire_orchestration_task, orchestration_task>(
                     response.tasks)
               : std::vector<orchestration_task>();
}

std::vector<orchestration_task> rasn_runtime_provider::blocked_tasks() const
{
    ::dsn::rasn::rpc::task_orchestration_request request;
    request.__set_operation(::dsn::rasn::rpc::task_orchestration_operation::blocked);
    const ::dsn::rasn::rpc::task_orchestration_response response = call_module_api(request);
    return runtime_status_ok(response.status) && response.__isset.tasks
               ? from_wire_list<::dsn::rasn::rpc::wire_orchestration_task, orchestration_task>(
                     response.tasks)
               : std::vector<orchestration_task>();
}

bool rasn_runtime_provider::record_choice(const std::string &task_id,
                                            const std::string &key,
                                            const std::string &source,
                                            const std::string &value,
                                            deterministic_choice *choice,
                                            std::string *error)
{
    ::dsn::rasn::rpc::determinism_record_request body;
    body.task_id = task_id;
    body.key = key;
    body.source = source;
    body.value = value;
    ::dsn::rasn::rpc::determinism_request request;
    request.__set_operation(::dsn::rasn::rpc::determinism_operation::record);
    request.__set_record(body);
    const ::dsn::rasn::rpc::determinism_response response = call_module_api(request);
    if (!runtime_status_ok(response.status))
    {
        set_response_error(response, error);
        return false;
    }
    deterministic_choice stored_choice;
    std::string decode_error;
    if (!response.__isset.choice || !from_wire(response.choice, &stored_choice, &decode_error))
    {
        if (error != nullptr)
        {
            *error = decode_error;
        }
        return false;
    }
    if (choice != nullptr)
    {
        *choice = stored_choice;
    }
    return mirror_state_after_success("determinism_ledger",
                                      "choice",
                                      stored_choice.task_id + "/" + stored_choice.key,
                                      encode_choice_payload(stored_choice),
                                      error);
}

std::vector<deterministic_choice> rasn_runtime_provider::choice_snapshot() const
{
    ::dsn::rasn::rpc::determinism_request request;
    request.__set_operation(::dsn::rasn::rpc::determinism_operation::snapshot);
    const ::dsn::rasn::rpc::determinism_response response = call_module_api(request);
    return runtime_status_ok(response.status) && response.__isset.choices
               ? from_wire_list<::dsn::rasn::rpc::wire_deterministic_choice,
                                deterministic_choice>(response.choices)
               : std::vector<deterministic_choice>();
}

bool rasn_runtime_provider::upsert_capability_provider(const capability_provider &provider, std::string *error)
{
    capability_provider stored = provider;
    if (stored.last_seen_ms == 0)
    {
        stored.last_seen_ms = ::dsn_now_ms();
    }
    ::dsn::rasn::rpc::capability_directory_request request;
    request.__set_operation(
        ::dsn::rasn::rpc::capability_directory_operation::upsert_provider);
    request.__set_upsert_provider(to_wire(stored));
    const ::dsn::rasn::rpc::capability_directory_response response =
        call_module_api(request);
    if (!response_bool(response, error))
    {
        return false;
    }
    return mirror_state_after_success(
        "capability_directory", "provider", stored.descriptor.agent_id, encode_capability_provider_payload(stored), error);
}

std::string rasn_runtime_provider::describe_capabilities() const
{
    ::dsn::rasn::rpc::capability_directory_request request;
    request.__set_operation(::dsn::rasn::rpc::capability_directory_operation::describe);
    const ::dsn::rasn::rpc::capability_directory_response response =
        call_module_api(request);
    return runtime_status_ok(response.status) && response.__isset.description
               ? response.description
               : runtime_error_message(response);
}

bool rasn_runtime_provider::configure_budget(const resource_quota &quota, std::string *error)
{
    ::dsn::rasn::rpc::resource_budget_request request;
    request.__set_operation(::dsn::rasn::rpc::resource_budget_operation::configure);
    request.__set_configure(to_wire(quota));
    const ::dsn::rasn::rpc::resource_budget_response response = call_module_api(request);
    if (!response_bool(response, error))
    {
        return false;
    }
    if (!mirror_state_after_success("resource_budget", "quota", quota.scope, encode_quota_payload(quota), error))
    {
        return false;
    }
    resource_usage usage;
    if (budget_usage(quota.scope, &usage))
    {
        return mirror_state_after_success("resource_budget", "usage", usage.scope, encode_usage_payload(usage), error);
    }
    return true;
}

resource_budget_decision rasn_runtime_provider::reserve_budget(const resource_request &request_value)
{
    ::dsn::rasn::rpc::resource_budget_request request;
    request.__set_operation(::dsn::rasn::rpc::resource_budget_operation::reserve);
    request.__set_reserve(to_wire(request_value));
    const ::dsn::rasn::rpc::resource_budget_response response = call_module_api(request);
    resource_budget_decision decision;
    decision.allowed = false;
    decision.scope = request_value.scope;
    if (!runtime_status_ok(response.status))
    {
        decision.reason = runtime_error_message(response);
        return decision;
    }
    std::string error;
    if (!response.__isset.decision || !from_wire(response.decision, &decision, &error))
    {
        decision.allowed = false;
        decision.reason = error;
    }
    if (decision.allowed)
    {
        std::string mirror_error;
        if (!mirror_state_after_success(
                "resource_budget", "usage", decision.usage_after.scope, encode_usage_payload(decision.usage_after), &mirror_error))
        {
            decision.allowed = false;
            decision.reason = mirror_error;
        }
    }
    return decision;
}

bool rasn_runtime_provider::release_budget(const resource_request &request_value, std::string *error)
{
    ::dsn::rasn::rpc::resource_budget_request request;
    request.__set_operation(::dsn::rasn::rpc::resource_budget_operation::release);
    request.__set_release(to_wire(request_value));
    const ::dsn::rasn::rpc::resource_budget_response response = call_module_api(request);
    if (!response_bool(response, error))
    {
        return false;
    }
    resource_usage usage;
    if (budget_usage(request_value.scope, &usage))
    {
        return mirror_state_after_success("resource_budget", "usage", usage.scope, encode_usage_payload(usage), error);
    }
    return true;
}

bool rasn_runtime_provider::budget_usage(const std::string &scope, resource_usage *usage) const
{
    ::dsn::rasn::rpc::resource_usage_request body;
    body.scope = scope;
    ::dsn::rasn::rpc::resource_budget_request request;
    request.__set_operation(::dsn::rasn::rpc::resource_budget_operation::usage);
    request.__set_usage(body);
    const ::dsn::rasn::rpc::resource_budget_response response = call_module_api(request);
    if (!runtime_status_ok(response.status) || !response.__isset.usage)
    {
        return false;
    }
    std::string error;
    return from_wire(response.usage, usage, &error);
}

std::string rasn_runtime_provider::describe_budgets() const
{
    ::dsn::rasn::rpc::resource_budget_request request;
    request.__set_operation(::dsn::rasn::rpc::resource_budget_operation::describe);
    const std::vector<::dsn::rasn::rpc::resource_budget_response> responses =
        call_module_api_shards(request);
    const uint32_t partition_count = rasn_runtime_partition_count("resource_budget");
    if (responses.size() <= 1 && (!module_routed_remote("resource_budget") || partition_count <= 1))
    {
        return responses.empty()
                   ? ""
                   : (runtime_status_ok(responses[0].status) &&
                              responses[0].__isset.description
                          ? responses[0].description
                          : runtime_error_message(responses[0]));
    }
    std::ostringstream output;
    for (size_t i = 0; i < responses.size(); ++i)
    {
        if (i != 0)
        {
            output << "\n";
        }
        output << "shard" << response_partition(responses[i]) << ": "
               << (runtime_status_ok(responses[i].status) &&
                           responses[i].__isset.description
                       ? responses[i].description
                       : runtime_error_message(responses[i]));
    }
    return output.str();
}

bool rasn_runtime_provider::set_recovery_policy(const recovery_policy &policy, std::string *error)
{
    ::dsn::rasn::rpc::recovery_supervisor_request request;
    request.__set_operation(::dsn::rasn::rpc::recovery_supervisor_operation::set_policy);
    request.__set_set_policy(to_wire(policy));
    const ::dsn::rasn::rpc::recovery_supervisor_response response =
        call_module_api(request);
    if (!response_bool(response, error))
    {
        return false;
    }
    return mirror_state_after_success(
        "recovery_supervisor", "policy", policy.failure_class, encode_recovery_policy_payload(policy), error);
}

recovery_action rasn_runtime_provider::observe_failure(const failure_observation &failure)
{
    failure_observation stored_failure = failure;
    if (stored_failure.time_ms == 0)
    {
        stored_failure.time_ms = ::dsn_now_ms();
    }
    ::dsn::rasn::rpc::recovery_supervisor_request request;
    request.__set_operation(
        ::dsn::rasn::rpc::recovery_supervisor_operation::observe_failure);
    request.__set_observe_failure(to_wire(stored_failure));
    const ::dsn::rasn::rpc::recovery_supervisor_response response =
        call_module_api(request);
    recovery_action action;
    if (!runtime_status_ok(response.status))
    {
        action.reason = runtime_error_message(response);
        return action;
    }
    std::string error;
    if (!response.__isset.action || !from_wire(response.action, &action, &error))
    {
        action.reason = error;
    }
    else
    {
        const std::string key = stored_failure.task_id + "/" + stored_failure.component + "/" +
                                stored_failure.failure_class + "/" + stored_failure.code + "/" +
                                std::to_string(stored_failure.attempt) + "/" +
                                std::to_string(stored_failure.time_ms);
        std::string mirror_error;
        if (!mirror_state_after_success("recovery_supervisor", "failure", key, encode_failure_payload(stored_failure), &mirror_error))
        {
            action.reason = mirror_error;
        }
    }
    return action;
}

std::string rasn_runtime_provider::describe_recovery() const
{
    ::dsn::rasn::rpc::recovery_supervisor_request request;
    request.__set_operation(::dsn::rasn::rpc::recovery_supervisor_operation::describe);
    const ::dsn::rasn::rpc::recovery_supervisor_response response =
        call_module_api(request);
    return runtime_status_ok(response.status) && response.__isset.description
               ? response.description
               : runtime_error_message(response);
}

bool rasn_runtime_provider::put_blackboard(const blackboard_entry &entry, blackboard_entry *stored, std::string *error)
{
    blackboard_entry submitted = entry;
    const uint64_t now_ms = ::dsn_now_ms();
    submitted.updated_at_ms = now_ms;
    ::dsn::rasn::rpc::blackboard_request request;
    request.__set_operation(::dsn::rasn::rpc::blackboard_operation::put);
    request.__set_put(to_wire(submitted));
    const ::dsn::rasn::rpc::blackboard_response response = call_module_api(request);
    if (!runtime_status_ok(response.status))
    {
        set_response_error(response, error);
        return false;
    }
    blackboard_entry stored_entry;
    std::string decode_error;
    if (!response.__isset.entry || !from_wire(response.entry, &stored_entry, &decode_error))
    {
        if (error != nullptr)
        {
            *error = decode_error;
        }
        return false;
    }
    if (stored != nullptr)
    {
        *stored = stored_entry;
    }
    return mirror_state_after_success(
        "blackboard", "entry", stored_entry.key, encode_blackboard_payload(stored_entry), error);
}

bool rasn_runtime_provider::get_blackboard(const std::string &key, blackboard_entry *entry) const
{
    ::dsn::rasn::rpc::blackboard_get_request body;
    body.key = key;
    ::dsn::rasn::rpc::blackboard_request request;
    request.__set_operation(::dsn::rasn::rpc::blackboard_operation::get);
    request.__set_get(body);
    const ::dsn::rasn::rpc::blackboard_response response = call_module_api(request);
    if (!runtime_status_ok(response.status) || !response.__isset.entry)
    {
        return false;
    }
    std::string error;
    return from_wire(response.entry, entry, &error);
}

std::vector<blackboard_entry> rasn_runtime_provider::blackboard_snapshot(bool include_expired, uint64_t now_ms) const
{
    std::vector<blackboard_entry> entries;
    ::dsn::rasn::rpc::blackboard_snapshot_request body;
    body.include_expired = include_expired;
    body.now_ms = encode_runtime_unsigned<int64_t>(now_ms);
    ::dsn::rasn::rpc::blackboard_request request;
    request.__set_operation(::dsn::rasn::rpc::blackboard_operation::snapshot);
    request.__set_snapshot(body);
    for (const ::dsn::rasn::rpc::blackboard_response &response :
         call_module_api_shards(request))
    {
        if (!runtime_status_ok(response.status))
        {
            dwarn("blackboard snapshot shard %u failed: %s",
                  response_partition(response),
                  runtime_error_message(response).c_str());
            continue;
        }
        if (response.__isset.entries)
        {
            const std::vector<blackboard_entry> shard_entries =
                from_wire_list<::dsn::rasn::rpc::wire_blackboard_entry, blackboard_entry>(
                    response.entries);
            entries.insert(entries.end(), shard_entries.begin(), shard_entries.end());
        }
    }
    return entries;
}

bool rasn_runtime_provider::register_contract(const agent_contract &contract, std::string *error)
{
    ::dsn::rasn::rpc::contract_verifier_request request;
    request.__set_operation(
        ::dsn::rasn::rpc::contract_verifier_operation::register_contract);
    request.__set_register_contract(to_wire(contract));
    const ::dsn::rasn::rpc::contract_verifier_response response =
        call_module_api(request);
    if (!response_bool(response, error))
    {
        return false;
    }
    return mirror_state_after_success(
        "contract_verifier", "contract", contract.contract_id, encode_contract_payload(contract), error);
}

contract_evaluation rasn_runtime_provider::evaluate_input(const std::string &contract_id, const std::string &input) const
{
    ::dsn::rasn::rpc::contract_evaluate_input_request body;
    body.contract_id = contract_id;
    body.input = input;
    ::dsn::rasn::rpc::contract_verifier_request request;
    request.__set_operation(
        ::dsn::rasn::rpc::contract_verifier_operation::evaluate_input);
    request.__set_evaluate_input(body);
    const ::dsn::rasn::rpc::contract_verifier_response response =
        call_module_api(request);
    if (!runtime_status_ok(response.status))
    {
        return failed_contract_evaluation(contract_id, runtime_error_message(response));
    }
    contract_evaluation evaluation;
    std::string error;
    return response.__isset.evaluation &&
                   from_wire(response.evaluation, &evaluation, &error)
               ? evaluation
               : failed_contract_evaluation(contract_id, error);
}

contract_evaluation rasn_runtime_provider::evaluate_output(const std::string &contract_id,
                                                             const std::string &output,
                                                             const std::vector<std::string> &policy_labels) const
{
    ::dsn::rasn::rpc::contract_evaluate_output_request body;
    body.contract_id = contract_id;
    body.output = output;
    body.policy_labels = policy_labels;
    ::dsn::rasn::rpc::contract_verifier_request request;
    request.__set_operation(
        ::dsn::rasn::rpc::contract_verifier_operation::evaluate_output);
    request.__set_evaluate_output(body);
    const ::dsn::rasn::rpc::contract_verifier_response response =
        call_module_api(request);
    if (!runtime_status_ok(response.status))
    {
        return failed_contract_evaluation(contract_id, runtime_error_message(response));
    }
    contract_evaluation evaluation;
    std::string error;
    return response.__isset.evaluation &&
                   from_wire(response.evaluation, &evaluation, &error)
               ? evaluation
               : failed_contract_evaluation(contract_id, error);
}

std::string rasn_runtime_provider::describe_contracts() const
{
    ::dsn::rasn::rpc::contract_verifier_request request;
    request.__set_operation(::dsn::rasn::rpc::contract_verifier_operation::describe);
    const ::dsn::rasn::rpc::contract_verifier_response response =
        call_module_api(request);
    return runtime_status_ok(response.status) && response.__isset.description
               ? response.description
               : runtime_error_message(response);
}

human_interaction_result
rasn_runtime_provider::open_human_interaction(const human_interaction_request &request)
{
    human_interaction_request submitted = request;
    if (submitted.request_id.empty())
    {
        submitted.request_id = make_trace_id();
    }
    if (submitted.state.empty())
    {
        submitted.state = "pending";
    }
    const uint64_t now_ms = ::dsn_now_ms();
    if (submitted.created_at_ms == 0)
    {
        submitted.created_at_ms = now_ms;
    }
    submitted.updated_at_ms = now_ms;

    ::dsn::rasn::rpc::human_interaction_rpc_request rpc_request;
    rpc_request.__set_operation(::dsn::rasn::rpc::human_interaction_operation::open);
    rpc_request.__set_open(to_wire(submitted));
    human_interaction_result result =
        human_result_from_response(call_module_api(rpc_request));
    if (result.ok)
    {
        std::string mirror_error;
        if (!mirror_state_after_success("human_interaction",
                                        "request",
                                        result.request.request_id,
                                        encode_human_payload(result.request),
                                        &mirror_error))
        {
            result.ok = false;
            result.error = mirror_error;
        }
    }
    return result;
}

human_interaction_result
rasn_runtime_provider::answer_human_interaction(const std::string &request_id,
                                                const std::string &answer)
{
    ::dsn::rasn::rpc::human_answer_request body;
    body.request_id = request_id;
    body.answer = answer;
    body.updated_at_ms = encode_runtime_unsigned<int64_t>(::dsn_now_ms());
    ::dsn::rasn::rpc::human_interaction_rpc_request request;
    request.__set_operation(::dsn::rasn::rpc::human_interaction_operation::answer);
    request.__set_answer(body);
    human_interaction_result result = human_result_from_response(call_module_api(request));
    if (result.ok)
    {
        std::string mirror_error;
        if (!mirror_state_after_success("human_interaction",
                                        "request",
                                        result.request.request_id,
                                        encode_human_payload(result.request),
                                        &mirror_error))
        {
            result.ok = false;
            result.error = mirror_error;
        }
    }
    return result;
}

human_interaction_result
rasn_runtime_provider::cancel_human_interaction(const std::string &request_id,
                                                const std::string &reason)
{
    ::dsn::rasn::rpc::human_cancel_request body;
    body.request_id = request_id;
    body.reason = reason;
    body.updated_at_ms = encode_runtime_unsigned<int64_t>(::dsn_now_ms());
    ::dsn::rasn::rpc::human_interaction_rpc_request request;
    request.__set_operation(::dsn::rasn::rpc::human_interaction_operation::cancel);
    request.__set_cancel(body);
    human_interaction_result result = human_result_from_response(call_module_api(request));
    if (result.ok)
    {
        std::string mirror_error;
        if (!mirror_state_after_success("human_interaction",
                                        "request",
                                        result.request.request_id,
                                        encode_human_payload(result.request),
                                        &mirror_error))
        {
            result.ok = false;
            result.error = mirror_error;
        }
    }
    return result;
}

bool rasn_runtime_provider::find_human_interaction(const std::string &request_id,
                                                   human_interaction_request *request) const
{
    ::dsn::rasn::rpc::human_find_request body;
    body.request_id = request_id;
    ::dsn::rasn::rpc::human_interaction_rpc_request rpc_request;
    rpc_request.__set_operation(::dsn::rasn::rpc::human_interaction_operation::find);
    rpc_request.__set_find(body);
    const ::dsn::rasn::rpc::human_interaction_rpc_response response =
        call_module_api(rpc_request);
    if (!runtime_status_ok(response.status) || !response.__isset.request)
    {
        return false;
    }
    std::string error;
    return from_wire(response.request, request, &error);
}

size_t rasn_runtime_provider::expire_human_interactions(uint64_t now_ms)
{
    ::dsn::rasn::rpc::human_expire_request body;
    body.now_ms = encode_runtime_unsigned<int64_t>(now_ms);
    ::dsn::rasn::rpc::human_interaction_rpc_request request;
    request.__set_operation(::dsn::rasn::rpc::human_interaction_operation::expire);
    request.__set_expire(body);
    size_t expired = 0;
    for (const ::dsn::rasn::rpc::human_interaction_rpc_response &response :
         call_module_api_shards(request))
    {
        if (!runtime_status_ok(response.status))
        {
            dwarn("human interaction expiry partition %u failed: %s",
                  response_partition(response),
                  runtime_error_message(response).c_str());
            continue;
        }
        expired += response.__isset.count ? static_cast<size_t>(response.count) : 0;
    }
    if (expired > 0)
    {
        for (const human_interaction_request &request : human_snapshot())
        {
            mirror_state_after_success("human_interaction",
                                       "request",
                                       request.request_id,
                                       encode_human_payload(request));
        }
    }
    return expired;
}

std::vector<human_interaction_request> rasn_runtime_provider::human_snapshot() const
{
    std::vector<human_interaction_request> requests;
    ::dsn::rasn::rpc::human_interaction_rpc_request request;
    request.__set_operation(::dsn::rasn::rpc::human_interaction_operation::snapshot);
    for (const ::dsn::rasn::rpc::human_interaction_rpc_response &response :
         call_module_api_shards(request))
    {
        if (!runtime_status_ok(response.status))
        {
            dwarn("human interaction snapshot partition %u failed: %s",
                  response_partition(response),
                  runtime_error_message(response).c_str());
            continue;
        }
        if (response.__isset.requests)
        {
            const std::vector<human_interaction_request> partition_requests =
                from_wire_list<::dsn::rasn::rpc::wire_human_interaction_request,
                               human_interaction_request>(response.requests);
            requests.insert(requests.end(), partition_requests.begin(), partition_requests.end());
        }
    }
    return requests;
}

std::vector<human_interaction_request>
rasn_runtime_provider::pending_human(const std::string &requester) const
{
    std::vector<human_interaction_request> requests;
    ::dsn::rasn::rpc::human_pending_request body;
    body.requester = requester;
    ::dsn::rasn::rpc::human_interaction_rpc_request request;
    request.__set_operation(::dsn::rasn::rpc::human_interaction_operation::pending);
    request.__set_pending(body);
    for (const ::dsn::rasn::rpc::human_interaction_rpc_response &response :
         call_module_api_shards(request))
    {
        if (!runtime_status_ok(response.status))
        {
            dwarn("pending human interaction partition %u failed: %s",
                  response_partition(response),
                  runtime_error_message(response).c_str());
            continue;
        }
        if (response.__isset.requests)
        {
            const std::vector<human_interaction_request> partition_requests =
                from_wire_list<::dsn::rasn::rpc::wire_human_interaction_request,
                               human_interaction_request>(response.requests);
            requests.insert(requests.end(), partition_requests.begin(), partition_requests.end());
        }
    }
    return requests;
}

bool rasn_runtime_config_file_selects_remote(const std::string &config_path)
{
    // Parse placement with rDSN's configuration implementation BEFORE dsn_run(),
    // so includes, inline comments, escaping, and last-write-wins semantics match
    // the actual runtime config. A one-shot CLI needs this answer early to decide
    // whether to launch a lightweight client service node for remote/hybrid RPC.
    // Defaults to local (false) on any read or parse failure.
    if (config_path.empty())
    {
        return false;
    }
    ::dsn::configuration config;
    config.set_warning(false);
    if (!config.load(config_path.c_str()))
    {
        return false;
    }
    const std::string provider = config.get_value<std::string>(
        "rasn.runtime", "rasn_runtime_provider", "", "rASN runtime module provider");
    const std::string mode = config.get_value<std::string>(
        "rasn.runtime", "rasn_runtime_mode", "local", "rASN runtime module mode");
    const std::string effective = provider.empty() ? mode : provider;
    const std::string normalized = normalize_rasn_runtime_provider_name(effective);
    return normalized == "distributed" || normalized == "hybrid";
}

std::vector<std::string>
rasn_runtime_unstartable_host_apps(const std::vector<rasn_runtime_host_app_spec> &apps,
                                   const std::string &app_list,
                                   size_t *matched,
                                   std::vector<std::string> *invalid)
{
    std::vector<std::string> unstartable;
    if (invalid != nullptr)
    {
        invalid->clear();
    }
    size_t matched_count = 0;
    std::set<std::string> selected_apps;
    std::string token;
    for (size_t i = 0; i <= app_list.size(); ++i)
    {
        if (i == app_list.size() || app_list[i] == ';' || app_list[i] == ',')
        {
            const std::string trimmed = trim(token);
            if (!trimmed.empty())
            {
                std::vector<std::string> selector;
                ::dsn::utils::split_args(trimmed.c_str(), selector, '@');
                if (selector.empty())
                {
                    if (invalid != nullptr)
                    {
                        invalid->push_back(trimmed);
                    }
                }
                else
                {
                    const std::vector<rasn_runtime_host_app_spec>::const_iterator app =
                        std::find_if(apps.begin(),
                                     apps.end(),
                                     [&selector](const rasn_runtime_host_app_spec &candidate) {
                                         return candidate.name == selector.front();
                                     });
                    if (app == apps.end() || !app->run || app->count <= 0)
                    {
                        unstartable.push_back(trimmed);
                    }
                    else if (!selected_apps.insert(app->name).second)
                    {
                        // rDSN scans app-list tokens for each app instance and
                        // stops at the first token whose name matches. Any later
                        // selector for the same app is therefore unreachable.
                        unstartable.push_back(trimmed);
                    }
                    else if (selector.size() < 2)
                    {
                        matched_count += static_cast<size_t>(app->count);
                    }
                    else
                    {
                        int index = 0;
                        if (!::dsn::utils::lexical_cast_integer<int>(selector.back(), index))
                        {
                            if (invalid != nullptr)
                            {
                                invalid->push_back(trimmed);
                            }
                        }
                        else if (index >= 1 && index <= app->count)
                        {
                            ++matched_count;
                        }
                        else
                        {
                            unstartable.push_back(trimmed);
                        }
                    }
                }
            }
            token.clear();
        }
        else
        {
            token.push_back(app_list[i]);
        }
    }
    if (matched != nullptr)
    {
        *matched = matched_count;
    }
    return unstartable;
}

rasn_runtime_host_app_list_check
rasn_runtime_check_host_app_list(const std::string &config_path, const std::string &app_list)
{
    // A `serve <config> <app_list>` override selects which [apps.*] sections the
    // runtime host starts. rDSN matches each token (its name part before '@')
    // against a config section by exact equality ("apps." + token == section), so
    // a token that names no section starts nothing; a fully-typo'd override brings
    // up a host that binds no services and then sleeps forever -- contrary to the
    // command's fail-clearly contract. Surface that here BEFORE dsn_run(), using
    // the same rDSN configuration parser (so @includes/comments/last-write-wins
    // match the real runtime), and let the caller reject a zero-match override.
    rasn_runtime_host_app_list_check result;
    if (config_path.empty())
    {
        return result;
    }

    ::dsn::configuration config;
    config.set_warning(false);
    if (!config.load(config_path.c_str()))
    {
        // Config unreadable/unparsable: leave config_loaded=false so the caller
        // does not reject on an unknown section set (rDSN surfaces the load error).
        return result;
    }
    result.config_loaded = true;

    const bool default_run =
        config.get_value<bool>("apps..default", "run", true, "whether to run the app instances or not");
    const int default_count = static_cast<int>(config.get_value<unsigned long long>(
        "apps..default", "count", 1, "count of app instances for this type"));

    std::vector<std::string> sections;
    config.get_all_sections(sections);
    const std::string prefix = "apps.";
    for (const std::string &section : sections)
    {
        if (section.size() <= prefix.size() || section.compare(0, prefix.size(), prefix) != 0)
        {
            continue;
        }
        const std::string name = section.substr(prefix.size());
        // Skip the "[apps..default]" inheritance template (its name part is empty
        // or begins with '.'); it is never a startable app.
        if (name.empty() || name[0] == '.')
        {
            continue;
        }
        rasn_runtime_host_app_spec app;
        app.name = name;
        app.run = config.get_value<bool>(
            section.c_str(), "run", default_run, "whether to run the app instances or not");
        app.count = static_cast<int>(config.get_value<unsigned long long>(
            section.c_str(), "count", static_cast<unsigned long long>(default_count), "count of app instances"));
        result.apps.push_back(app);
    }

    const bool mimic_enabled = config.get_value<bool>(
        "core", "enable_default_app_mimic", false, "whether to start a default service app");
    const bool mimic_defined =
        std::find_if(result.apps.begin(),
                     result.apps.end(),
                     [](const rasn_runtime_host_app_spec &app) { return app.name == "mimic"; }) !=
        result.apps.end();
    if (mimic_enabled && !mimic_defined)
    {
        // service_spec::init_app_specs() synthesizes this section before it
        // applies -app_list, inheriting the [apps..default] run/count values.
        rasn_runtime_host_app_spec mimic;
        mimic.name = "mimic";
        mimic.run = default_run;
        mimic.count = default_count;
        result.apps.push_back(mimic);
    }

    result.unstartable =
        rasn_runtime_unstartable_host_apps(result.apps, app_list, &result.matched, &result.invalid);
    return result;
}

rasn_runtime_config load_rasn_runtime_config()
{
    rasn_runtime_config config;
    const std::string provider =
        config_string("rasn_runtime_provider", "", "rASN runtime module provider: local, distributed, or hybrid");
    const std::string legacy_mode =
        config_string("rasn_runtime_mode", "local", "legacy rASN runtime module mode: local, distributed, or hybrid (embedded = alias for local)");
    config.provider = normalize_rasn_runtime_provider_name(provider.empty() ? legacy_mode : provider);
    config.state_prefix = trim(config_string(
        "rasn_runtime_state_prefix", "rasn/runtime", "State key prefix for distributed rASN runtime modules"));
    if (config.state_prefix.empty())
    {
        config.state_prefix = "rasn/runtime";
    }
    config.strict = ::dsn_config_get_value_bool("rasn.runtime",
                                                "rasn_runtime_strict",
                                                false,
                                                "Treat distributed runtime module provider failures as strict warnings");
    return config;
}

std::unique_ptr<rasn_runtime> create_rasn_runtime(rasn_service_graph &services, const rasn_runtime_config &config)
{
    std::unique_ptr<rasn_runtime_provider> provider;
    const std::string provider_name = normalize_rasn_runtime_provider_name(config.provider);
    if (provider_name == "distributed")
    {
        provider.reset(new rasn_distributed_runtime_provider(services, config));
    }
    else if (provider_name == "hybrid")
    {
        provider.reset(new rasn_hybrid_runtime_provider(services, config));
    }
    else
    {
        provider.reset(new rasn_local_runtime_provider(config));
    }
    return std::unique_ptr<rasn_runtime>(new rasn_runtime(std::move(provider)));
}

#define RASN_DEFINE_RUNTIME_DISPATCH(request_type, response_type)                                       \
    ::dsn::rasn::rpc::response_type dispatch_rasn_runtime_request(                                      \
        const ::dsn::rasn::rpc::request_type &request)                                                  \
    {                                                                                                   \
        return global_rasn_runtime_store().dispatch(request);                                           \
    }

RASN_DEFINE_RUNTIME_DISPATCH(agent_control_request, agent_control_response)
RASN_DEFINE_RUNTIME_DISPATCH(message_bus_request, message_bus_response)
RASN_DEFINE_RUNTIME_DISPATCH(task_orchestration_request, task_orchestration_response)
RASN_DEFINE_RUNTIME_DISPATCH(determinism_request, determinism_response)
RASN_DEFINE_RUNTIME_DISPATCH(capability_directory_request, capability_directory_response)
RASN_DEFINE_RUNTIME_DISPATCH(resource_budget_request, resource_budget_response)
RASN_DEFINE_RUNTIME_DISPATCH(recovery_supervisor_request, recovery_supervisor_response)
RASN_DEFINE_RUNTIME_DISPATCH(blackboard_request, blackboard_response)
RASN_DEFINE_RUNTIME_DISPATCH(contract_verifier_request, contract_verifier_response)
RASN_DEFINE_RUNTIME_DISPATCH(human_interaction_rpc_request, human_interaction_rpc_response)
RASN_DEFINE_RUNTIME_DISPATCH(sandbox_runtime_request, sandbox_runtime_response)

#undef RASN_DEFINE_RUNTIME_DISPATCH

rasn_runtime_rpc_service::rasn_runtime_rpc_service(std::vector<std::string> modules,
                                                   rasn_runtime_replica_store *replica_store)
    : ::dsn::serverlet<rasn_runtime_rpc_service>("rasn.runtime"),
      _modules(modules.empty() ? rasn_runtime_module_names() : std::move(modules)),
      _replica_store(replica_store)
{
    // Cache the hosted-shard set for any sharded module that hosts only a subset
    // of partitions so the ingress guard can reject misrouted requests without a
    // per-request config lookup. Modules that host the whole module (the common
    // single-process case) contribute nothing and are admitted unconditionally.
    for (const std::string &module : _modules)
    {
        std::vector<uint32_t> shards =
            _replica_store != nullptr ? std::vector<uint32_t>() : rasn_runtime_hosted_shards(module);
        if (!shards.empty())
        {
            _hosted_shards.emplace(module, std::move(shards));
        }
    }
}

void rasn_runtime_rpc_service::open_service(::dsn_gpid gpid)
{
    _gpid = gpid;
    {
        std::lock_guard<std::mutex> guard(_request_lock);
        _accepting_requests = true;
    }
    dinfo("opening rasn.runtime serverlet with module API(s): %s", join_strings(_modules, ",").c_str());
    for (const std::string &module : _modules)
    {
        if (!register_module_handler(module))
        {
            dwarn("failed to register runtime module API handler for '%s'", module.c_str());
        }
    }
}

bool rasn_runtime_rpc_service::close_service(std::chrono::milliseconds timeout)
{
    {
        std::lock_guard<std::mutex> guard(_request_lock);
        _accepting_requests = false;
    }
    dinfo("closing rasn.runtime serverlet with %d module API(s)", static_cast<int>(_modules.size()));
    for (const std::string &module : _modules)
    {
        unregister_module_handler(module);
    }
    std::unique_lock<std::mutex> guard(_request_lock);
    return _requests_drained.wait_for(
        guard, timeout, [this] { return _active_requests == 0; });
}

bool rasn_runtime_rpc_service::begin_request()
{
    std::lock_guard<std::mutex> guard(_request_lock);
    if (!_accepting_requests)
        return false;
    ++_active_requests;
    return true;
}

void rasn_runtime_rpc_service::finish_request()
{
    std::lock_guard<std::mutex> guard(_request_lock);
    dassert(_active_requests > 0, "runtime RPC active-request counter underflow");
    --_active_requests;
    if (_active_requests == 0)
        _requests_drained.notify_all();
}

bool rasn_runtime_rpc_service::register_module_handler(const std::string &module)
{
#define RASN_REGISTER_TYPED_MODULE(module_name, handler_name)                                           \
    if (module == module_name)                                                                          \
    {                                                                                                   \
        const bool read_registered = this->register_async_rpc_handler(                                  \
            rpc_code_for_module(module, false),                                                         \
            #handler_name,                                                                              \
            &rasn_runtime_rpc_service::on_##handler_name,                                               \
            _gpid);                                                                                     \
        const bool write_registered = this->register_async_rpc_handler(                                 \
            rpc_code_for_module(module, true),                                                          \
            #handler_name ".write",                                                                     \
            &rasn_runtime_rpc_service::on_##handler_name##_write,                                       \
            _gpid);                                                                                     \
        return read_registered && write_registered;                                                     \
    }

    RASN_REGISTER_TYPED_MODULE("agent_control_plane", agent_control)
    RASN_REGISTER_TYPED_MODULE("agent_message_bus", message_bus)
    RASN_REGISTER_TYPED_MODULE("task_orchestration_kernel", task_orchestration)
    RASN_REGISTER_TYPED_MODULE("determinism_ledger", determinism_ledger)
    RASN_REGISTER_TYPED_MODULE("capability_directory", capability_directory)
    RASN_REGISTER_TYPED_MODULE("resource_budget", resource_budget)
    RASN_REGISTER_TYPED_MODULE("recovery_supervisor", recovery_supervisor)
    RASN_REGISTER_TYPED_MODULE("blackboard", blackboard)
    RASN_REGISTER_TYPED_MODULE("contract_verifier", contract_verifier)
    RASN_REGISTER_TYPED_MODULE("human_interaction", human_interaction)
    RASN_REGISTER_TYPED_MODULE("sandbox_runtime", sandbox_runtime)

#undef RASN_REGISTER_TYPED_MODULE
    dwarn("unknown runtime module API '%s' is not registered", module.c_str());
    return false;
}

void rasn_runtime_rpc_service::unregister_module_handler(const std::string &module)
{
    if (!has_module(rasn_runtime_module_names(), module))
    {
        return;
    }
    this->unregister_rpc_handler(rpc_code_for_module(module, false), _gpid);
    this->unregister_rpc_handler(rpc_code_for_module(module, true), _gpid);
}

template <typename Request, typename Response>
void rasn_runtime_rpc_service::reply_module_request_typed(
    const Request &request, ::dsn::rpc_replier<Response> &reply, bool write_channel)
{
    const std::string module = runtime_module_name(request);
    Request copy = request;
    const auto reply_error = [&copy, &reply](::dsn::rasn::rpc::runtime_error_code::type code,
                                             const std::string &message) {
        Response response = make_runtime_response<Request, Response>(copy);
        set_runtime_error(&response, code, message);
        reply(response);
    };
    request_guard guard(this);
    if (!guard.active())
    {
        reply_error(::dsn::rasn::rpc::runtime_error_code::unavailable,
                    "rasn runtime service is shutting down");
        return;
    }
    std::string auth_error;
    if (!authenticate_rasn_runtime_rpc_request(copy, &auth_error))
    {
        metrics_registry::instance().on_event("runtime.auth.rejected", module);
        dwarn("runtime module RPC auth rejected for module '%s'", module.c_str());
        reply_error(::dsn::rasn::rpc::runtime_error_code::unauthorized, auth_error);
        return;
    }
    copy.metadata.auth_token.clear();
    std::string validation_error;
    if (!validate_runtime_request(copy, &validation_error))
    {
        reply_error(runtime_validation_error_code(validation_error), validation_error);
        return;
    }

    if (_replica_store != nullptr)
    {
        const bool mutating = runtime_request_is_mutating(copy);
        if (write_channel != mutating)
        {
            metrics_registry::instance().on_event("runtime.replica.channel_rejected", module);
            reply_error(::dsn::rasn::rpc::runtime_error_code::misrouted,
                        mutating ? "replicated runtime mutation arrived on the read RPC"
                                 : "replicated runtime read arrived on the write RPC");
            return;
        }
        const uint32_t partition_count = rasn_runtime_partition_count(module);
        uint32_t request_partition = rasn_runtime_partition_for_request(copy);
        if (mutating && partition_count > 1)
        {
            if (runtime_request_is_partition_fanout(copy))
            {
                if (!copy.metadata.__isset.route_partition)
                {
                    metrics_registry::instance().on_event("runtime.replica.partition_rejected", module);
                    reply_error(::dsn::rasn::rpc::runtime_error_code::misrouted,
                                "replicated partition-fanout mutation requires an explicit route");
                    return;
                }
                request_partition =
                    static_cast<uint32_t>(copy.metadata.route_partition) % partition_count;
            }
            else
            {
                request_partition =
                    rasn_runtime_partition_for_key(module, runtime_request_key(copy));
                if (copy.metadata.__isset.route_partition &&
                    static_cast<uint32_t>(copy.metadata.route_partition) % partition_count !=
                        request_partition)
                {
                    metrics_registry::instance().on_event("runtime.replica.partition_rejected", module);
                    reply_error(::dsn::rasn::rpc::runtime_error_code::misrouted,
                                "replicated runtime mutation route does not match its request key");
                    return;
                }
            }
        }
        if (partition_count > 1 &&
            request_partition != static_cast<uint32_t>(_gpid.u.partition_index))
        {
            metrics_registry::instance().on_event("runtime.replica.partition_rejected", module);
            reply_error(::dsn::rasn::rpc::runtime_error_code::misrouted,
                        "replicated runtime request does not belong to partition " +
                            std::to_string(_gpid.u.partition_index));
            return;
        }
    }

    // Shard-ownership ingress guard (review finding 2): when this service hosts
    // only a subset of a sharded module's partitions, refuse a request that routes
    // to a shard we do not host so a stale registry entry, a static endpoint, or a
    // direct client cannot mutate state owned by another node. No-op for unsharded
    // modules and for services that host the whole module.
    const auto hosted = _hosted_shards.find(module);
    if (hosted != _hosted_shards.end() && !rasn_runtime_service_hosts_request(copy, hosted->second))
    {
        metrics_registry::instance().on_event("runtime.shard.misrouted", module);
        const uint32_t partition = rasn_runtime_partition_for_request(copy);
        dwarn("runtime module '%s' rejected request routed to non-hosted shard %u",
              module.c_str(),
              static_cast<unsigned int>(partition));
        reply_error(::dsn::rasn::rpc::runtime_error_code::misrouted,
                    "rasn runtime service does not host shard " +
                        std::to_string(partition) + " of module " + module);
        return;
    }

    // Adopt the incoming trace id for the duration of dispatch so server-side
    // logs and any nested module requests share the originating operation's trace.
    rasn_runtime_trace_scope trace(
        copy.metadata.__isset.trace_id ? copy.metadata.trace_id : std::string());
    reply(_replica_store != nullptr ? _replica_store->dispatch(copy)
                                    : dispatch_rasn_runtime_request(copy));
}

#define RASN_DEFINE_RPC_HANDLERS(name, request_type, response_type)                                     \
    void rasn_runtime_rpc_service::on_##name(                                                           \
        const ::dsn::rasn::rpc::request_type &request,                                                  \
        ::dsn::rpc_replier<::dsn::rasn::rpc::response_type> &reply)                                    \
    {                                                                                                   \
        reply_module_request_typed(request, reply, false);                                              \
    }                                                                                                   \
    void rasn_runtime_rpc_service::on_##name##_write(                                                   \
        const ::dsn::rasn::rpc::request_type &request,                                                  \
        ::dsn::rpc_replier<::dsn::rasn::rpc::response_type> &reply)                                    \
    {                                                                                                   \
        reply_module_request_typed(request, reply, true);                                               \
    }

RASN_DEFINE_RPC_HANDLERS(agent_control, agent_control_request, agent_control_response)
RASN_DEFINE_RPC_HANDLERS(message_bus, message_bus_request, message_bus_response)
RASN_DEFINE_RPC_HANDLERS(task_orchestration,
                         task_orchestration_request,
                         task_orchestration_response)
RASN_DEFINE_RPC_HANDLERS(determinism_ledger, determinism_request, determinism_response)
RASN_DEFINE_RPC_HANDLERS(capability_directory,
                         capability_directory_request,
                         capability_directory_response)
RASN_DEFINE_RPC_HANDLERS(resource_budget, resource_budget_request, resource_budget_response)
RASN_DEFINE_RPC_HANDLERS(recovery_supervisor,
                         recovery_supervisor_request,
                         recovery_supervisor_response)
RASN_DEFINE_RPC_HANDLERS(blackboard, blackboard_request, blackboard_response)
RASN_DEFINE_RPC_HANDLERS(contract_verifier,
                         contract_verifier_request,
                         contract_verifier_response)
RASN_DEFINE_RPC_HANDLERS(human_interaction,
                         human_interaction_rpc_request,
                         human_interaction_rpc_response)
RASN_DEFINE_RPC_HANDLERS(sandbox_runtime, sandbox_runtime_request, sandbox_runtime_response)

#undef RASN_DEFINE_RPC_HANDLERS

#define RASN_DEFINE_CLIENT_CALL(request_type, response_type)                                            \
    std::pair<::dsn::error_code, ::dsn::rasn::rpc::response_type> rasn_runtime_client::call_sync(       \
        const ::dsn::rasn::rpc::request_type &request,                                                  \
        std::chrono::milliseconds timeout,                                                              \
        int thread_hash,                                                                                \
        uint64_t partition_hash)                                                                        \
    {                                                                                                   \
        return ::dsn::rpc::wait_and_unwrap<::dsn::rasn::rpc::response_type>(::dsn::rpc::call(          \
            _server,                                                                                   \
            rpc_code_for_module(runtime_module_name(request), runtime_request_is_mutating(request)),   \
            request,                                                                                   \
            nullptr,                                                                                   \
            empty_callback,                                                                            \
            timeout,                                                                                   \
            thread_hash,                                                                               \
            partition_hash));                                                                          \
    }

RASN_DEFINE_CLIENT_CALL(agent_control_request, agent_control_response)
RASN_DEFINE_CLIENT_CALL(message_bus_request, message_bus_response)
RASN_DEFINE_CLIENT_CALL(task_orchestration_request, task_orchestration_response)
RASN_DEFINE_CLIENT_CALL(determinism_request, determinism_response)
RASN_DEFINE_CLIENT_CALL(capability_directory_request, capability_directory_response)
RASN_DEFINE_CLIENT_CALL(resource_budget_request, resource_budget_response)
RASN_DEFINE_CLIENT_CALL(recovery_supervisor_request, recovery_supervisor_response)
RASN_DEFINE_CLIENT_CALL(blackboard_request, blackboard_response)
RASN_DEFINE_CLIENT_CALL(contract_verifier_request, contract_verifier_response)
RASN_DEFINE_CLIENT_CALL(human_interaction_rpc_request, human_interaction_rpc_response)
RASN_DEFINE_CLIENT_CALL(sandbox_runtime_request, sandbox_runtime_response)

#undef RASN_DEFINE_CLIENT_CALL

namespace {

agent_capability make_rasn_runtime_capability(const std::string &name)
{
    agent_capability capability;
    capability.name = name;
    const std::vector<std::tuple<std::string, std::string, std::string>> schemas = {
        {"agent_control_plane", "agent_control_request.v1", "agent_control_response.v1"},
        {"agent_message_bus", "message_bus_request.v1", "message_bus_response.v1"},
        {"task_orchestration_kernel",
         "task_orchestration_request.v1",
         "task_orchestration_response.v1"},
        {"determinism_ledger", "determinism_request.v1", "determinism_response.v1"},
        {"capability_directory",
         "capability_directory_request.v1",
         "capability_directory_response.v1"},
        {"resource_budget", "resource_budget_request.v1", "resource_budget_response.v1"},
        {"recovery_supervisor",
         "recovery_supervisor_request.v1",
         "recovery_supervisor_response.v1"},
        {"blackboard", "blackboard_request.v1", "blackboard_response.v1"},
        {"contract_verifier",
         "contract_verifier_request.v1",
         "contract_verifier_response.v1"},
        {"human_interaction",
         "human_interaction_rpc_request.v1",
         "human_interaction_rpc_response.v1"},
        {"sandbox_runtime", "sandbox_runtime_request.v1", "sandbox_runtime_response.v1"}};
    for (const auto &schema : schemas)
    {
        if (name.find(std::get<0>(schema)) != std::string::npos)
        {
            capability.input_type = std::get<1>(schema);
            capability.output_type = std::get<2>(schema);
            break;
        }
    }
    capability.side_effect_class = "stateful";
    return capability;
}

// Resolve the address a runtime host publishes to the registry for a module. By
// default this is the node's primary_address(), but rDSN derives that from the
// first non-loopback NIC, which on multi-homed hosts (or hosts with virtual/bridge
// interfaces) can be an address that clients cannot reach. Operators can override it
// with [rasn.service] rasn_runtime_advertise_host (or a per-module
// <module>_advertise_host): set 127.0.0.1 for a co-located runtime that only serves
// same-host clients, or the routable IP / DNS name for a multi-machine deployment.
// Only the host part is overridden; the advertised port stays the node's real listen
// port. When unset, behavior is unchanged (primary_address()). An invalid or
// unresolvable override returns an invalid address so it is never published as a
// healthy registry endpoint.
::dsn::rpc_address rasn_runtime_registry_advertise_address(const std::string &module,
                                                           const ::dsn::rpc_address &primary)
{
    const std::string module_key = module_service_key(module);
    const std::string common_host =
        config_service_string("rasn_runtime_advertise_host", "", "rASN runtime registry advertise host");
    const std::string host =
        config_service_string(module_key + "_advertise_host", common_host, "rASN per-module registry advertise host");
    if (host.empty())
    {
        return primary;
    }
    ::dsn::rpc_address address;
    address.assign_ipv4(host.c_str(), primary.port());
    if (address.is_invalid() || address.ip() == 0 || address.port() == 0)
    {
        derror("invalid registry advertise host '%s' for runtime module %s; "
               "skipping registry publication",
               host.c_str(),
               module.c_str());
        address.set_invalid();
    }
    return address;
}

agent_descriptor make_rasn_runtime_module_descriptor(const std::string &module,
                                                    const ::dsn::rpc_address &endpoint,
                                                    const std::string &app_name)
{
    const std::string endpoint_uri = endpoint.to_std_string();
    const std::string base_capability = rasn_runtime_module_capability(module);

    agent_descriptor descriptor;
    descriptor.agent_id = base_capability + "@" + endpoint_uri;
    descriptor.role = rasn_runtime_module_app_role(module);
    descriptor.app_name = app_name.empty() ? "rasn.runtime" : app_name;
    descriptor.port = endpoint.type() == HOST_TYPE_IPV4 ? endpoint.port() : 0;
    descriptor.endpoint_uri = endpoint_uri;
    descriptor.version = "prototype";
    descriptor.health = "healthy";
    descriptor.capabilities.push_back(make_rasn_runtime_capability(base_capability));
    const std::vector<uint32_t> hosted_shards = rasn_runtime_hosted_shards(module);
    for (const uint32_t shard : hosted_shards)
    {
        descriptor.capabilities.push_back(make_rasn_runtime_capability(rasn_runtime_module_shard_capability(module, shard)));
    }
    return descriptor;
}

} // namespace

namespace {

const char kReplicatedRuntimeCheckpointPrefix[] = "rasn-runtime-checkpoint.";

bool parse_replicated_runtime_checkpoint_decree(const std::string &file_name, int64_t *decree)
{
    if (decree == nullptr)
    {
        return false;
    }
    const size_t prefix_length = sizeof(kReplicatedRuntimeCheckpointPrefix) - 1;
    if (file_name.size() <= prefix_length ||
        file_name.compare(0, prefix_length, kReplicatedRuntimeCheckpointPrefix) != 0)
    {
        return false;
    }
    return ::dsn::utils::lexical_cast_integer<int64_t>(file_name.substr(prefix_length), *decree) && *decree >= 0;
}

} // namespace

rasn_replicated_runtime_app::rasn_replicated_runtime_app(::dsn_gpid gpid, std::string module)
    : ::dsn::replicated_service_app_type_1(gpid),
      _module(std::move(module)),
      _checkpoint_lock(true),
      _store(_module),
      _rpc(std::vector<std::string>{_module}, &_store)
{
}

::dsn::error_code rasn_replicated_runtime_app::start(int argc, char **argv)
{
    const size_t configured_capacity = configured_replicated_runtime_dedup_capacity();
    if (configured_capacity != _store.dedup_capacity())
    {
        derror("replicated runtime module %s requires rasn_runtime_dedup_capacity=%llu "
               "(the native replication protocol constant), got %llu",
               _module.c_str(),
               static_cast<unsigned long long>(_store.dedup_capacity()),
               static_cast<unsigned long long>(configured_capacity));
        return ::dsn::ERR_INVALID_PARAMETERS;
    }
    if (rasn_runtime_rpc_auth_enabled())
    {
        derror("replicated runtime module %s cannot use shared-token RPC auth because "
               "type-1 writes are authenticated after quorum commit",
               _module.c_str());
        return ::dsn::ERR_INVALID_PARAMETERS;
    }
    const char *data_dir = ::dsn_get_app_data_dir(get_gpid());
    if (data_dir == nullptr || data_dir[0] == '\0')
    {
        derror("replicated runtime module %s partition has no app data directory", _module.c_str());
        return ::dsn::ERR_INVALID_PARAMETERS;
    }
    _data_dir = data_dir;
    const ::dsn::error_code recovered = recover_latest_checkpoint();
    if (recovered != ::dsn::ERR_OK)
    {
        return recovered;
    }
    _rpc.open_service(get_gpid());
    dinfo("opened quorum-replicated runtime module=%s partition=%d checkpoint_decree=%lld",
          _module.c_str(),
          get_gpid().u.partition_index,
          static_cast<long long>(_last_durable_decree.load()));
    return ::dsn::ERR_OK;
}

::dsn::error_code rasn_replicated_runtime_app::stop(bool cleanup)
{
    if (!_rpc.close_service(rasn_runtime_request_drain_timeout()))
    {
        derror("timed out draining replicated runtime module=%s partition=%d",
               _module.c_str(),
               get_gpid().u.partition_index);
        return ::dsn::ERR_TIMEOUT;
    }
    if (cleanup && !_data_dir.empty() && ::dsn::utils::filesystem::directory_exists(_data_dir) &&
        !::dsn::utils::filesystem::remove_path(_data_dir))
    {
        derror("failed to remove replicated runtime data directory: %s", _data_dir.c_str());
        return ::dsn::ERR_FILE_OPERATION_FAILED;
    }
    return ::dsn::ERR_OK;
}

::dsn::error_code rasn_replicated_runtime_app::sync_checkpoint(int64_t last_commit)
{
    if (last_commit < 0)
    {
        return ::dsn::ERR_INVALID_PARAMETERS;
    }
    ::dsn::service::zauto_lock guard(_checkpoint_lock);
    std::vector<state_record> records;
    std::string error;
    if (!_store.checkpoint_records(get_gpid(), last_commit, &records, &error))
    {
        derror("failed to snapshot replicated runtime module=%s decree=%lld: %s",
               _module.c_str(),
               static_cast<long long>(last_commit),
               error.c_str());
        return ::dsn::ERR_CHECKPOINT_FAILED;
    }

    state_store snapshot(false, 0);
    for (const state_record &record : records)
    {
        const state_response written = snapshot.put(record);
        if (!written.ok)
        {
            derror("failed to stage replicated runtime checkpoint module=%s decree=%lld: %s",
                   _module.c_str(),
                   static_cast<long long>(last_commit),
                   written.error.c_str());
            return ::dsn::ERR_CHECKPOINT_FAILED;
        }
    }
    state_checkpoint_request request;
    request.path = checkpoint_path(last_commit);
    const state_response checkpointed = snapshot.checkpoint(request);
    if (!checkpointed.ok)
    {
        derror("failed to persist replicated runtime checkpoint module=%s decree=%lld: %s",
               _module.c_str(),
               static_cast<long long>(last_commit),
               checkpointed.error.c_str());
        return ::dsn::ERR_CHECKPOINT_FAILED;
    }
    _last_durable_decree.store(last_commit);
    return ::dsn::ERR_OK;
}

::dsn::error_code rasn_replicated_runtime_app::async_checkpoint(int64_t last_commit)
{
    (void)last_commit;
    return ::dsn::ERR_NOT_IMPLEMENTED;
}

int64_t rasn_replicated_runtime_app::get_last_checkpoint_decree()
{
    return _last_durable_decree.load();
}

::dsn::error_code rasn_replicated_runtime_app::get_checkpoint(int64_t learn_start,
                                                              int64_t local_commit,
                                                              void *learn_request,
                                                              int learn_request_size,
                                                              app_learn_state &state)
{
    (void)learn_start;
    (void)local_commit;
    (void)learn_request;
    (void)learn_request_size;
    const int64_t decree = _last_durable_decree.load();
    if (decree <= 0)
    {
        return ::dsn::ERR_OBJECT_NOT_FOUND;
    }
    const std::string path = checkpoint_path(decree);
    if (!::dsn::utils::filesystem::file_exists(path))
    {
        return ::dsn::ERR_OBJECT_NOT_FOUND;
    }
    state.from_decree_excluded = 0;
    state.to_decree_included = decree;
    state.files.clear();
    state.files.push_back(path);
    return ::dsn::ERR_OK;
}

::dsn::error_code rasn_replicated_runtime_app::apply_checkpoint(
    ::dsn_chkpt_apply_mode mode,
    int64_t local_commit,
    const ::dsn_app_learn_state &state)
{
    (void)local_commit;
    if ((mode != ::DSN_CHKPT_LEARN && mode != ::DSN_CHKPT_COPY) ||
        state.to_decree_included < 0 || state.file_state_count != 1 ||
        state.files == nullptr || state.files[0] == nullptr || state.files[0][0] == '\0')
    {
        return ::dsn::ERR_INVALID_PARAMETERS;
    }
    ::dsn::service::zauto_lock guard(_checkpoint_lock);
    if (mode == ::DSN_CHKPT_COPY && state.to_decree_included < _last_durable_decree.load())
    {
        return ::dsn::ERR_INVALID_STATE;
    }

    state_checkpoint_request request;
    request.path = state.files[0];
    state_store imported(false, 0);
    const state_response parsed = imported.copy_checkpoint(request, "");
    std::string error;
    if (!parsed.ok ||
        !_store.validate_checkpoint_records(
            parsed.records, get_gpid(), state.to_decree_included, &error))
    {
        derror("failed to validate replicated runtime checkpoint module=%s decree=%lld mode=%d: %s",
               _module.c_str(),
               static_cast<long long>(state.to_decree_included),
               static_cast<int>(mode),
               parsed.ok ? error.c_str() : parsed.error.c_str());
        return ::dsn::ERR_CHECKPOINT_FAILED;
    }

    const std::string durable_path = checkpoint_path(state.to_decree_included);
    const state_response persisted = imported.copy_checkpoint(request, durable_path);
    if (!persisted.ok)
    {
        derror("failed to persist replicated runtime checkpoint module=%s decree=%lld: %s",
               _module.c_str(),
               static_cast<long long>(state.to_decree_included),
               persisted.error.c_str());
        return ::dsn::ERR_CHECKPOINT_FAILED;
    }
    if (mode == ::DSN_CHKPT_LEARN &&
        !_store.replace_checkpoint_records(
            parsed.records, get_gpid(), state.to_decree_included, &error))
    {
        derror("failed to learn replicated runtime checkpoint module=%s decree=%lld: %s",
               _module.c_str(),
               static_cast<long long>(state.to_decree_included),
               error.c_str());
        return ::dsn::ERR_CHECKPOINT_FAILED;
    }
    _last_durable_decree.store(state.to_decree_included);
    return ::dsn::ERR_OK;
}

::dsn::error_code rasn_replicated_runtime_app::recover_latest_checkpoint()
{
    std::vector<std::string> files;
    if (!::dsn::utils::filesystem::get_subfiles(_data_dir, files, false))
    {
        derror("failed to enumerate replicated runtime data directory: %s", _data_dir.c_str());
        return ::dsn::ERR_FILE_OPERATION_FAILED;
    }
    std::vector<std::pair<int64_t, std::string>> checkpoints;
    for (const std::string &path : files)
    {
        int64_t decree = 0;
        if (parse_replicated_runtime_checkpoint_decree(
                ::dsn::utils::filesystem::get_file_name(path), &decree))
        {
            checkpoints.emplace_back(decree, path);
        }
    }
    if (checkpoints.empty())
    {
        return ::dsn::ERR_OK;
    }

    std::sort(checkpoints.rbegin(), checkpoints.rend());
    for (const std::pair<int64_t, std::string> &checkpoint : checkpoints)
    {
        state_checkpoint_request request;
        request.path = checkpoint.second;
        state_store imported(false, 0);
        const state_response parsed = imported.copy_checkpoint(request, "");
        std::string error;
        if (parsed.ok &&
            _store.replace_checkpoint_records(
                parsed.records, get_gpid(), checkpoint.first, &error))
        {
            _last_durable_decree.store(checkpoint.first);
            return ::dsn::ERR_OK;
        }
        dwarn("ignoring invalid replicated runtime checkpoint module=%s path=%s error=%s",
              _module.c_str(),
              checkpoint.second.c_str(),
              parsed.ok ? error.c_str() : parsed.error.c_str());
    }
    return ::dsn::ERR_CHECKPOINT_FAILED;
}

std::string rasn_replicated_runtime_app::checkpoint_path(int64_t decree) const
{
    return ::dsn::utils::filesystem::path_combine(
        _data_dir, std::string(kReplicatedRuntimeCheckpointPrefix) + std::to_string(decree));
}

::dsn::error_code rasn_runtime_app::start(int argc, char **argv)
{
    global_rasn_services().acquire();
    // Acquire single-writer ownership BEFORE hydrating (review finding 1). If a
    // standby hydrates first and then blocks waiting for the active owner to
    // release, the active can commit a newer snapshot in the meantime and the
    // standby would open handlers on stale in-memory state. Acquiring first means
    // that once we own the resource no other node serves writes for it, so the
    // subsequent hydration observes the latest committed snapshot and it stays
    // current through open_service(). The gate is default-off, in which case
    // acquire/release are no-ops and ordering is immaterial.
    const ::dsn::error_code ownership_error = acquire_module_ownership();
    if (ownership_error != ::dsn::ERR_OK)
    {
        global_rasn_services().release();
        return ownership_error;
    }
    const ::dsn::error_code hydration_error = hydrate_modules_from_state();
    if (hydration_error != ::dsn::ERR_OK)
    {
        // Do not keep ownership we are not going to serve: release it so another
        // node can take over instead of parking the resource behind an aborting start.
        release_module_ownership();
        global_rasn_services().release();
        return hydration_error;
    }
    if (!_owned_resources.empty())
    {
        _ownership_state->serving.store(true);
        if (_ownership_state->lost.load())
        {
            _ownership_state->serving.store(false);
            release_module_ownership();
            global_rasn_services().release();
            return ::dsn::ERR_INVALID_STATE;
        }
    }
    _rpc.open_service();
    register_modules_with_registry();
    start_registry_heartbeat_timer();
    return ::dsn::ERR_OK;
}

rasn_runtime_app::rasn_runtime_app(::dsn_gpid gpid)
    : ::dsn::service_app(gpid), _rpc(rasn_runtime_module_names())
{
}

rasn_runtime_app::rasn_runtime_app(::dsn_gpid gpid, std::vector<std::string> modules)
    : ::dsn::service_app(gpid), _rpc(std::move(modules))
{
}

::dsn::error_code rasn_runtime_app::hydrate_modules_from_state()
{
    if (!rasn_runtime_state_hydration_enabled())
    {
        return ::dsn::ERR_OK;
    }

    const rasn_runtime_config config = load_rasn_runtime_config();
    state_query_request request;
    request.key_prefix = rasn_runtime_state_prefix(config.state_prefix);

    const std::shared_ptr<refreshable_endpoint_binding> state_binding =
        global_rasn_services().service_endpoint_binding("state");
    if (state_binding == nullptr)
    {
        derror("runtime state hydration cannot find the state endpoint binding");
        return ::dsn::ERR_INVALID_PARAMETERS;
    }
    const uint32_t max_attempts = rasn_runtime_state_hydration_max_attempts();
    const std::chrono::milliseconds retry_backoff = rasn_runtime_state_hydration_retry_backoff();
    ::dsn::error_code err = ::dsn::ERR_OK;
    state_response response;
    for (uint32_t attempt = 1; attempt <= max_attempts; ++attempt)
    {
        endpoint_snapshot endpoint = state_binding->current();
        if (!endpoint.ok && endpoint.refreshable)
        {
            endpoint = state_binding->refresh(endpoint.generation).endpoint;
        }
        if (!endpoint.ok)
        {
            err = ::dsn::ERR_SERVICE_NOT_FOUND;
        }
        else
        {
            rasn_state_client state(endpoint.address);
            std::tie(err, response) =
                state.query_sync(request, rasn_runtime_state_hydration_timeout());
            if (err != ::dsn::ERR_OK &&
                is_retryable_rasn_runtime_error(err) &&
                endpoint.refreshable)
            {
                (void)state_binding->refresh(endpoint.generation);
            }
        }
        if (err == ::dsn::ERR_OK)
        {
            break;
        }
        // A co-located state service may still be registering its RPC handlers
        // when this runtime service starts, so the first hydration query can hit
        // a transient ERR_TIMEOUT (the state node logs "unknown rpc name" and the
        // client times out). Wait for the dependency to become ready instead of
        // aborting on the first miss, mirroring recover_workflow_state_after_start().
        // Fail closed only after the readiness budget is exhausted or on a
        // non-transient error.
        if (attempt >= max_attempts || !is_retryable_rasn_runtime_error(err))
        {
            dwarn("runtime state hydration RPC query failed (%s) after %u attempt(s); refusing to open module APIs",
                  err.to_string(),
                  static_cast<unsigned int>(attempt));
            return err;
        }
        dinfo("runtime state hydration RPC not ready yet (%s); retry %u/%u",
              err.to_string(),
              static_cast<unsigned int>(attempt),
              static_cast<unsigned int>(max_attempts));
        std::this_thread::sleep_for(retry_backoff);
    }
    if (!response.ok)
    {
        dwarn("runtime state hydration query failed: %s", response.error.c_str());
        return ::dsn::ERR_UNKNOWN;
    }
    if (rasn_runtime_state_watermark_verify_enabled())
    {
        std::string watermark_error;
        if (!verify_runtime_state_watermarks(response.records, _rpc.modules(), config.state_prefix, &watermark_error))
        {
            dwarn("runtime state hydration watermark verification failed: %s", watermark_error.c_str());
            return ::dsn::ERR_UNKNOWN;
        }
    }

    size_t applied = 0;
    std::string error;
    std::map<std::string, std::vector<uint32_t>> hosted_shards;
    for (const std::string &module : _rpc.modules())
    {
        std::vector<uint32_t> shards = rasn_runtime_hosted_shards(module);
        if (!shards.empty())
        {
            hosted_shards.emplace(module, std::move(shards));
        }
    }
    if (!global_rasn_runtime_store().hydrate_from_state(
            response.records, _rpc.modules(), &applied, &error, &hosted_shards))
    {
        dwarn("runtime state hydration replay failed: %s", error.c_str());
        return ::dsn::ERR_UNKNOWN;
    }
    if (applied > 0)
    {
        dinfo("hydrated %llu rASN runtime module record(s) for %s",
              static_cast<unsigned long long>(applied),
              join_strings(_rpc.modules(), ",").c_str());
    }
    return ::dsn::ERR_OK;
}

::dsn::error_code rasn_runtime_app::acquire_module_ownership()
{
    if (!rasn_runtime_ownership_gate_enabled())
    {
        return ::dsn::ERR_OK;
    }

    const ::dsn::rpc_address endpoint = primary_address();
    if (endpoint.is_invalid())
    {
        derror("rASN runtime app %s cannot acquire module ownership: primary address is invalid; "
               "refusing to open module APIs",
               name().c_str());
        return ::dsn::ERR_UNKNOWN;
    }
    static std::atomic<uint64_t> owner_sequence{0};
    std::ostringstream owner;
    owner << endpoint.to_std_string() << ".session." << std::hex
          << ::dsn_random64(0, (std::numeric_limits<uint64_t>::max)()) << "."
          << owner_sequence.fetch_add(1, std::memory_order_relaxed);
    _owner_id = owner.str();
    _ownership_state->lost.store(false);
    _ownership_state->serving.store(false);

    _coordination = create_rasn_coordination_service(load_rasn_coordination_config());
    const ::dsn::error_code start_err = _coordination->start();
    if (start_err != ::dsn::ERR_OK)
    {
        derror("rASN runtime app %s failed to start coordination backend: %s; refusing to open module APIs",
               name().c_str(),
               start_err.to_string());
        _coordination->stop();
        _coordination.reset();
        return start_err;
    }

    // Acquire single-writer ownership of every hosted module (or each hosted
    // shard for sharded modules) before opening RPC handlers, so at most one
    // runtime service serves writes for a given module/shard. Fail closed on
    // contention: a node that loses the race releases what it took and does not
    // open handlers for a resource another node owns.
    //
    // Each acquire_ownership() call already blocks up to the configured
    // acquire_timeout_ms for the current owner to release. On sustained contention
    // it returns an error and this app fails closed (rDSN aborts a non-OK start),
    // so an always-on active/standby pair relies on an external supervisor to
    // restart the loser, which then retries and wins once the active releases
    // (see DISTRIBUTED_RUNTIME.md §10). rasn_runtime_ownership_acquire_max_attempts
    // adds bounded in-process retries so a standby can also ride out a brief
    // ownership handover without a restart; it defaults to 1 (unchanged fail-closed
    // behavior).
    const std::vector<std::string> resources = rasn_runtime_module_ownership_resources(_rpc.modules());
    const uint32_t max_attempts = rasn_runtime_ownership_acquire_max_attempts();
    const std::chrono::milliseconds retry_backoff = rasn_runtime_ownership_acquire_retry_backoff();
    for (const std::string &resource : resources)
    {
        ::dsn::error_code acquired = ::dsn::ERR_OK;
        for (uint32_t attempt = 1; attempt <= max_attempts; ++attempt)
        {
            const std::shared_ptr<ownership_lease_state> lease_state =
                _ownership_state;
            const std::string app_name = name();
            acquired = _coordination->acquire_ownership(
                resource,
                _owner_id,
                [lease_state, app_name](const std::string &lost_resource,
                                        uint64_t fencing_token) {
                    lease_state->lost.store(true);
                    if (lease_state->serving.exchange(false))
                    {
                        derror("rASN runtime app %s lost ownership of %s at fence %llu "
                               "while serving; fail-stopping to prevent split-brain writes",
                               app_name.c_str(),
                               lost_resource.c_str(),
                               static_cast<unsigned long long>(fencing_token));
                        ::dsn_exit(1);
                    }
                });
            if (acquired == ::dsn::ERR_OK || attempt >= max_attempts)
            {
                break;
            }
            dwarn("rASN runtime app %s could not acquire ownership of %s yet (%s); retry %u/%u after backoff",
                  name().c_str(),
                  resource.c_str(),
                  acquired.to_string(),
                  static_cast<unsigned int>(attempt),
                  static_cast<unsigned int>(max_attempts));
            std::this_thread::sleep_for(retry_backoff);
        }
        if (acquired != ::dsn::ERR_OK)
        {
            derror("rASN runtime app %s failed to acquire ownership of %s: %s; refusing to open module APIs",
                   name().c_str(),
                   resource.c_str(),
                   acquired.to_string());
            release_module_ownership();
            return acquired;
        }
        _owned_resources.push_back(resource);
        if (_ownership_state->lost.load())
        {
            derror("rASN runtime app %s lost module ownership while starting; refusing to open module APIs",
                   name().c_str());
            release_module_ownership();
            return ::dsn::ERR_INVALID_STATE;
        }
    }

    if (!_owned_resources.empty())
    {
        dinfo("rASN runtime app %s acquired single-writer ownership of %llu module resource(s) via %s coordination",
              name().c_str(),
              static_cast<unsigned long long>(_owned_resources.size()),
              _coordination->provider_name());
    }
    return ::dsn::ERR_OK;
}

void rasn_runtime_app::release_module_ownership()
{
    _ownership_state->serving.store(false);
    if (!_coordination)
    {
        return;
    }
    bool ambiguous_release = false;
    for (const std::string &resource : _owned_resources)
    {
        const ::dsn::error_code released =
            _coordination->release_ownership(resource, _owner_id);
        if (released != ::dsn::ERR_OK)
        {
            ambiguous_release = true;
            derror("rASN runtime app %s cannot prove ownership release for %s: %s",
                   name().c_str(),
                   resource.c_str(),
                   released.to_string());
        }
    }
    _owned_resources.clear();
    _coordination->stop();
    _coordination.reset();
    if (ambiguous_release)
    {
        derror("rASN runtime ownership release is ambiguous; terminating the process so "
               "the shared ZooKeeper session cannot retain a ghost owner");
        ::dsn_exit(1);
    }
}

void rasn_runtime_app::register_modules_with_registry()
{
    if (!rasn_runtime_registry_registration_enabled())
    {
        return;
    }

    const ::dsn::rpc_address endpoint = primary_address();
    if (endpoint.is_invalid())
    {
        dwarn("rASN runtime app %s cannot register modules: primary address is invalid", name().c_str());
        return;
    }

    _registry_descriptors.clear();
    rasn_registry_client registry(rasn_service_address("registry", 27100));
    const std::chrono::milliseconds timeout = rasn_runtime_registry_timeout();
    bool registry_rpc_available = true;
    for (const std::string &module : _rpc.modules())
    {
        const ::dsn::rpc_address advertised = rasn_runtime_registry_advertise_address(module, endpoint);
        if (advertised.is_invalid() || advertised.ip() == 0 || advertised.port() == 0)
        {
            continue;
        }
        agent_descriptor descriptor = make_rasn_runtime_module_descriptor(module, advertised, name());
        std::string error;
        if (!global_agent_registry().register_agent(descriptor, &error, true))
        {
            dwarn("failed to register runtime module %s in local registry: %s", module.c_str(), error.c_str());
            continue;
        }
        _registry_descriptors.push_back(descriptor);

        if (!registry_rpc_available)
        {
            continue;
        }
        ::dsn::error_code err;
        agent_response response;
        std::tie(err, response) = registry.register_sync(descriptor, timeout);
        if (err != ::dsn::ERR_OK)
        {
            dwarn("failed to register runtime module %s through registry RPC: %s",
                  module.c_str(),
                  err.to_string());
            registry_rpc_available = false;
        }
        else if (!response.ok)
        {
            dwarn("registry rejected runtime module %s: %s", module.c_str(), response.error.message.c_str());
        }
    }
}

void rasn_runtime_app::heartbeat_modules_to_registry()
{
    if (_registry_descriptors.empty() || !rasn_runtime_registry_registration_enabled())
    {
        return;
    }

    rasn_registry_client registry(rasn_service_address("registry", 27100));
    const std::chrono::milliseconds timeout = rasn_runtime_registry_timeout();
    bool registry_rpc_available = true;
    for (const agent_descriptor &descriptor : _registry_descriptors)
    {
        std::string error;
        if (!global_agent_registry().heartbeat(descriptor, &error))
        {
            (void)global_agent_registry().register_agent(descriptor, &error, true);
        }

        if (!registry_rpc_available)
        {
            continue;
        }
        ::dsn::error_code err;
        agent_response response;
        std::tie(err, response) = registry.heartbeat_sync(descriptor, timeout);
        if (err != ::dsn::ERR_OK)
        {
            dwarn("failed to heartbeat runtime module %s through registry RPC: %s",
                  descriptor.agent_id.c_str(),
                  err.to_string());
            registry_rpc_available = false;
        }
        else if (registry_response_is_agent_not_found(response))
        {
            std::tie(err, response) = registry.register_sync(descriptor, timeout);
            if (err != ::dsn::ERR_OK)
            {
                dwarn("failed to re-register runtime module %s after heartbeat rejection: %s",
                      descriptor.agent_id.c_str(),
                      err.to_string());
            }
            else if (!response.ok)
            {
                dwarn("registry rejected runtime module %s after heartbeat rejection: %s",
                      descriptor.agent_id.c_str(),
                      response.error.message.c_str());
            }
        }
        else if (!response.ok)
        {
            dwarn("registry heartbeat rejected runtime module %s without a safe "
                  "re-registration: %s",
                  descriptor.agent_id.c_str(),
                  response.error.message.c_str());
        }
    }
}

void rasn_runtime_app::unregister_modules_from_registry()
{
    if (_registry_descriptors.empty())
    {
        return;
    }

    rasn_registry_client registry(rasn_service_address("registry", 27100));
    const std::chrono::milliseconds timeout = rasn_runtime_registry_timeout();
    bool registry_rpc_available = true;
    for (const agent_descriptor &descriptor : _registry_descriptors)
    {
        global_agent_registry().unregister_agent(descriptor.agent_id);
        if (registry_rpc_available && rasn_runtime_registry_registration_enabled())
        {
            ::dsn::error_code err;
            agent_response response;
            std::tie(err, response) = registry.unregister_sync(descriptor.agent_id, timeout);
            if (err != ::dsn::ERR_OK)
            {
                dwarn("failed to unregister runtime module %s through registry RPC: %s",
                      descriptor.agent_id.c_str(),
                      err.to_string());
                registry_rpc_available = false;
            }
        }
    }
    _registry_descriptors.clear();
}

void rasn_runtime_app::start_registry_heartbeat_timer()
{
    if (_registry_descriptors.empty() || _registry_heartbeat_timer != nullptr)
    {
        return;
    }

    const uint64_t interval_ms = rasn_runtime_registry_heartbeat_ms();
    if (interval_ms == 0)
    {
        return;
    }

    const std::chrono::milliseconds interval(interval_ms);
    _registry_heartbeat_timer = ::dsn::tasking::enqueue_timer(
        LPC_RASN_REGISTRY_HEARTBEAT_TIMER,
        nullptr,
        [this]() { heartbeat_modules_to_registry(); },
        interval,
        0,
        interval);
    if (_registry_heartbeat_timer == nullptr)
    {
        dwarn("failed to start rASN runtime registry heartbeat timer");
    }
}

void rasn_runtime_app::cancel_registry_heartbeat_timer()
{
    if (_registry_heartbeat_timer != nullptr)
    {
        _registry_heartbeat_timer->cancel(true);
        _registry_heartbeat_timer = nullptr;
    }
}

void register_rasn_runtime_apps()
{
    dassert(::dsn::register_app<rasn_runtime_app>("rasn.runtime"),
            "register rasn.runtime app failed");
    dassert(::dsn::register_app<rasn_agent_control_module_app>("rasn.runtime.agent_control"),
            "register rasn.runtime.agent_control app failed");
    dassert(::dsn::register_app<rasn_message_bus_module_app>("rasn.runtime.message_bus"),
            "register rasn.runtime.message_bus app failed");
    dassert(::dsn::register_app<rasn_task_orchestration_module_app>("rasn.runtime.task_kernel"),
            "register rasn.runtime.task_kernel app failed");
    dassert(::dsn::register_app<rasn_determinism_ledger_module_app>("rasn.runtime.determinism"),
            "register rasn.runtime.determinism app failed");
    dassert(::dsn::register_app<rasn_capability_directory_module_app>("rasn.runtime.capability"),
            "register rasn.runtime.capability app failed");
    dassert(::dsn::register_app<rasn_resource_budget_module_app>("rasn.runtime.budget"),
            "register rasn.runtime.budget app failed");
    dassert(::dsn::register_app<rasn_recovery_supervisor_module_app>("rasn.runtime.recovery"),
            "register rasn.runtime.recovery app failed");
    dassert(::dsn::register_app<rasn_blackboard_module_app>("rasn.runtime.blackboard"),
            "register rasn.runtime.blackboard app failed");
    dassert(::dsn::register_app<rasn_contract_verifier_module_app>("rasn.runtime.contract"),
            "register rasn.runtime.contract app failed");
    dassert(::dsn::register_app<rasn_human_interaction_module_app>("rasn.runtime.human_interaction"),
            "register rasn.runtime.human_interaction app failed");
    dassert(::dsn::register_app<rasn_sandbox_runtime_module_app>("rasn.runtime.sandbox_runtime"),
            "register rasn.runtime.sandbox_runtime app failed");
    dassert(::dsn::register_app_with_type_1_replication_support<rasn_replicated_agent_control_app>(
                "rasn.runtime.agent_control.replicated"),
            "register replicated agent-control runtime app failed");
    dassert(::dsn::register_app_with_type_1_replication_support<rasn_replicated_message_bus_app>(
                "rasn.runtime.message_bus.replicated"),
            "register replicated message-bus runtime app failed");
    dassert(::dsn::register_app_with_type_1_replication_support<rasn_replicated_task_orchestration_app>(
                "rasn.runtime.task_kernel.replicated"),
            "register replicated task-kernel runtime app failed");
    dassert(::dsn::register_app_with_type_1_replication_support<rasn_replicated_determinism_ledger_app>(
                "rasn.runtime.determinism.replicated"),
            "register replicated determinism runtime app failed");
    dassert(::dsn::register_app_with_type_1_replication_support<rasn_replicated_capability_directory_app>(
                "rasn.runtime.capability.replicated"),
            "register replicated capability runtime app failed");
    dassert(::dsn::register_app_with_type_1_replication_support<rasn_replicated_resource_budget_app>(
                "rasn.runtime.budget.replicated"),
            "register replicated budget runtime app failed");
    dassert(::dsn::register_app_with_type_1_replication_support<rasn_replicated_recovery_supervisor_app>(
                "rasn.runtime.recovery.replicated"),
            "register replicated recovery runtime app failed");
    dassert(::dsn::register_app_with_type_1_replication_support<rasn_replicated_blackboard_app>(
                "rasn.runtime.blackboard.replicated"),
            "register replicated blackboard runtime app failed");
    dassert(::dsn::register_app_with_type_1_replication_support<rasn_replicated_contract_verifier_app>(
                "rasn.runtime.contract.replicated"),
            "register replicated contract runtime app failed");
    dassert(::dsn::register_app_with_type_1_replication_support<rasn_replicated_human_interaction_app>(
                "rasn.runtime.human_interaction.replicated"),
            "register replicated human-interaction runtime app failed");
    dassert(::dsn::register_app_with_type_1_replication_support<rasn_replicated_sandbox_runtime_app>(
                "rasn.runtime.sandbox_runtime.replicated"),
            "register replicated sandbox runtime app failed");

    // Backward-compatible app type aliases for configs/scripts created before the
    // common-runtime rename. The public app-list normalizer rewrites these to the
    // rasn.runtime.* roles, but raw rDSN app lists may still reference them.
    dassert(::dsn::register_app<rasn_runtime_app>("rasn.common.modules"),
            "register rasn.common.modules app alias failed");
    dassert(::dsn::register_app<rasn_agent_control_module_app>("rasn.common.agent_control"),
            "register rasn.common.agent_control app alias failed");
    dassert(::dsn::register_app<rasn_message_bus_module_app>("rasn.common.message_bus"),
            "register rasn.common.message_bus app alias failed");
    dassert(::dsn::register_app<rasn_task_orchestration_module_app>("rasn.common.task_kernel"),
            "register rasn.common.task_kernel app alias failed");
    dassert(::dsn::register_app<rasn_determinism_ledger_module_app>("rasn.common.determinism"),
            "register rasn.common.determinism app alias failed");
    dassert(::dsn::register_app<rasn_capability_directory_module_app>("rasn.common.capability"),
            "register rasn.common.capability app alias failed");
    dassert(::dsn::register_app<rasn_resource_budget_module_app>("rasn.common.budget"),
            "register rasn.common.budget app alias failed");
    dassert(::dsn::register_app<rasn_recovery_supervisor_module_app>("rasn.common.recovery"),
            "register rasn.common.recovery app alias failed");
    dassert(::dsn::register_app<rasn_blackboard_module_app>("rasn.common.blackboard"),
            "register rasn.common.blackboard app alias failed");
    dassert(::dsn::register_app<rasn_contract_verifier_module_app>("rasn.common.contract"),
            "register rasn.common.contract app alias failed");
    dassert(::dsn::register_app<rasn_human_interaction_module_app>("rasn.common.human_interaction"),
            "register rasn.common.human_interaction app alias failed");
    dassert(::dsn::register_app<rasn_sandbox_runtime_module_app>("rasn.common.sandbox_runtime"),
            "register rasn.common.sandbox_runtime app alias failed");
}

::dsn::error_code rasn_runtime_app::stop(bool cleanup)
{
    cancel_registry_heartbeat_timer();
    unregister_modules_from_registry();
    if (!_rpc.close_service(rasn_runtime_request_drain_timeout()))
    {
        derror("rASN runtime app %s could not drain in-flight module RPCs before "
               "the ownership-release deadline; fail-stopping",
               name().c_str());
        ::dsn_exit(1);
        return ::dsn::ERR_TIMEOUT;
    }
    release_module_ownership();
    global_rasn_services().release();
    return ::dsn::ERR_OK;
}

std::string describe_agent_control_record(const agent_control_record &record)
{
    std::ostringstream output;
    output << "agent_id=" << record.descriptor.agent_id
           << "\nrole=" << record.descriptor.role
           << "\napp=" << record.descriptor.app_name
           << "\nstate=" << record.state
           << "\nplacement=" << record.placement
           << "\nowner=" << record.owner
           << "\nrestart_policy=" << record.restart_policy
           << "\nlast_error=" << record.last_error
           << "\ngeneration=" << record.generation
           << "\nlast_heartbeat_ms=" << record.last_heartbeat_ms
           << "\nlease_expires_ms=" << record.lease_expires_ms
           << "\ncapabilities=";
    for (size_t i = 0; i < record.descriptor.capabilities.size(); ++i)
    {
        if (i != 0)
        {
            output << ",";
        }
        output << record.descriptor.capabilities[i].name;
    }
    return output.str();
}

std::string describe_capability_provider_record(const capability_provider &provider)
{
    std::ostringstream output;
    output << "provider_id=" << provider.descriptor.agent_id
           << "\nstate=" << provider.state
           << "\nplacement=" << provider.placement
           << "\nlabels=" << join_strings(provider.labels, ",")
           << "\nload=" << provider.load
           << "\nlast_seen_ms=" << provider.last_seen_ms;
    return output.str();
}

std::string describe_resource_quota_record(const resource_quota &quota)
{
    std::ostringstream output;
    output << "scope=" << quota.scope
           << "\nmax_cost_units=" << quota.max_cost_units
           << "\nmax_latency_ms=" << quota.max_latency_ms
           << "\nmax_tokens=" << quota.max_tokens
           << "\nmax_tool_calls=" << quota.max_tool_calls;
    return output.str();
}

std::string describe_resource_decision_record(const resource_budget_decision &decision)
{
    std::ostringstream output;
    output << "allowed=" << (decision.allowed ? "true" : "false")
           << "\nscope=" << decision.scope
           << "\nreason=" << decision.reason
           << "\ncost_units=" << decision.usage_after.cost_units
           << "\nlatency_ms=" << decision.usage_after.latency_ms
           << "\ntokens=" << decision.usage_after.tokens
           << "\ntool_calls=" << decision.usage_after.tool_calls;
    return output.str();
}

std::string describe_resource_usage_record(const resource_usage &usage)
{
    std::ostringstream output;
    output << "scope=" << usage.scope
           << "\ncost_units=" << usage.cost_units
           << "\nlatency_ms=" << usage.latency_ms
           << "\ntokens=" << usage.tokens
           << "\ntool_calls=" << usage.tool_calls;
    return output.str();
}

std::string describe_recovery_policy_record(const recovery_policy &policy)
{
    std::ostringstream output;
    output << "failure_class=" << policy.failure_class
           << "\nmax_attempts=" << policy.max_attempts
           << "\nretry_delay_ms=" << policy.retry_delay_ms
           << "\nescalate_after_attempts=" << policy.escalate_after_attempts
           << "\nretryable=" << (policy.retryable ? "true" : "false")
           << "\ncompensation=" << policy.compensation;
    return output.str();
}

std::string describe_failure_observation_record(const failure_observation &failure)
{
    std::ostringstream output;
    output << "task_id=" << failure.task_id
           << "\ncomponent=" << failure.component
           << "\nfailure_class=" << failure.failure_class
           << "\ncode=" << failure.code
           << "\nmessage=" << failure.message
           << "\nattempt=" << failure.attempt
           << "\nretryable=" << (failure.retryable ? "true" : "false")
           << "\ntime_ms=" << failure.time_ms;
    return output.str();
}

std::string describe_contract_record(const agent_contract &contract)
{
    std::ostringstream output;
    output << "contract_id=" << contract.contract_id
           << "\nrequire_input_non_empty=" << (contract.require_input_non_empty ? "true" : "false")
           << "\nrequire_output_non_empty=" << (contract.require_output_non_empty ? "true" : "false")
           << "\nmax_output_bytes=" << contract.max_output_bytes
           << "\nrequired_input_fragments=" << join_strings(contract.required_input_fragments, ",")
           << "\nrequired_output_fragments=" << join_strings(contract.required_output_fragments, ",")
           << "\nforbidden_output_fragments=" << join_strings(contract.forbidden_output_fragments, ",")
           << "\nrequired_policy_labels=" << join_strings(contract.required_policy_labels, ",");
    return output.str();
}

std::string describe_orchestration_task_record(const orchestration_task &task)
{
    std::ostringstream output;
    output << "task_id=" << task.task_id
           << "\nparent_task_id=" << task.parent_task_id
           << "\nowner_agent=" << task.owner_agent
           << "\nstate=" << task.state
           << "\ninput=" << task.input
           << "\noutput=" << task.output
           << "\nerror=" << task.error
           << "\ndepends_on=" << join_strings(task.depends_on, ",")
           << "\ncompensation=" << task.compensation
           << "\ndeadline_ms=" << task.deadline_ms
           << "\ngeneration=" << task.generation;
    return output.str();
}

std::string describe_agent_message_record(const agent_message &message)
{
    std::ostringstream output;
    output << "message_id=" << message.message_id
           << "\ncorrelation_id=" << message.correlation_id
           << "\nsender=" << message.sender
           << "\nreceiver=" << message.receiver
           << "\ntype=" << message.type
           << "\npayload=" << message.payload
           << "\nstate=" << message.state
           << "\nerror=" << message.error
           << "\nattempt=" << message.attempt
           << "\ndeadline_ms=" << message.deadline_ms
           << "\navailable_at_ms=" << message.available_at_ms
           << "\ncreated_at_ms=" << message.created_at_ms
           << "\nupdated_at_ms=" << message.updated_at_ms;
    return output.str();
}

std::string describe_choice_record(const deterministic_choice &choice)
{
    std::ostringstream output;
    output << "sequence=" << choice.sequence
           << "\ntask_id=" << choice.task_id
           << "\nkey=" << choice.key
           << "\nsource=" << choice.source
           << "\nvalue=" << choice.value;
    return output.str();
}

std::string describe_blackboard_record(const blackboard_entry &entry)
{
    std::ostringstream output;
    output << "key=" << entry.key
           << "\nkind=" << entry.kind
           << "\nowner=" << entry.owner
           << "\nvalue=" << entry.value
           << "\ntags=" << join_strings(entry.tags, ",")
           << "\ngeneration=" << entry.generation
           << "\ncreated_at_ms=" << entry.created_at_ms
           << "\nupdated_at_ms=" << entry.updated_at_ms
           << "\nexpires_at_ms=" << entry.expires_at_ms;
    return output.str();
}

} // namespace rasn
} // namespace dsn
