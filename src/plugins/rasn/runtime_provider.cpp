#include <rasn/runtime_provider.h>

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
// file-local make_module_request below can stamp it onto each envelope.
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
    ::dsn::rpc_address address;
    std::string source;
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

bool rasn_runtime_module_is_sharded(const std::string &module)
{
    return module == "agent_message_bus" || module == "resource_budget" || module == "blackboard";
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

uint64_t rasn_runtime_partition_hash_impl(const rasn_runtime_request &request)
{
    if (request.route_partition != (std::numeric_limits<uint32_t>::max)())
    {
        return request.route_partition;
    }
    // Sharded modules historically hashed even the empty natural key. Preserve
    // that corner-case mapping; unsharded keyless control calls keep hash zero.
    return request.key.empty() && !rasn_runtime_module_is_sharded(request.module)
               ? 0
               : fnv1a64(request.key);
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
    rasn_runtime_request request;
    request.module = module;
    request.key = key;
    return static_cast<uint32_t>(rasn_runtime_partition_hash_impl(request) % count);
}

uint32_t rasn_runtime_partition_for_request(const rasn_runtime_request &request)
{
    const uint32_t count = rasn_runtime_partition_count(request.module);
    if (count <= 1)
    {
        return 0;
    }
    if (request.route_partition != (std::numeric_limits<uint32_t>::max)())
    {
        return request.route_partition % count;
    }
    return rasn_runtime_partition_for_key(request.module, request.key);
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

bool runtime_operation_is_mutating(const std::string &operation)
{
    if (operation.empty() || operation == "ping" || operation == "describe" || operation == "find" ||
        operation == "list" || operation == "snapshot" || operation == "ready_tasks" ||
        operation == "blocked_tasks" || operation == "usage" || operation == "get" ||
        operation == "profile" || operation == "evaluate" || operation == "evaluate_input" ||
        operation == "evaluate_output" || operation == "pending")
    {
        return false;
    }
    return true;
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

bool prepare_rasn_runtime_rpc_request(rasn_runtime_request *request, std::string *error)
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
        request->auth_token.clear();
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
    request->auth_token = token;
    if (error != nullptr)
    {
        error->clear();
    }
    return true;
}

bool authenticate_rasn_runtime_rpc_request(const rasn_runtime_request &request, std::string *error)
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
    if (request.auth_token.empty() || !constant_time_equal(request.auth_token, expected))
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

bool lookup_rasn_runtime_endpoint_capability(const std::string &module,
                                             uint32_t partition_index,
                                             const std::string &capability,
                                             const std::string &source,
                                             runtime_endpoint *endpoint)
{
    if (endpoint == nullptr || !rasn_runtime_registry_discovery_enabled())
    {
        return false;
    }

    std::vector<agent_descriptor> local_agents = global_agent_registry().query_by_capability(capability, true);
    if (choose_registry_endpoint(module, local_agents, partition_index, source, endpoint))
    {
        return true;
    }

    registry_query_request request;
    request.capability = capability;
    request.healthy_only = true;

    rasn_registry_client registry(rasn_service_address("registry", 27100));
    ::dsn::error_code err;
    registry_query_response response;
    std::tie(err, response) = registry.query_sync(request, rasn_runtime_registry_timeout());
    if (err != ::dsn::ERR_OK || !response.ok || response.agents.empty())
    {
        return false;
    }

    return choose_registry_endpoint(module, response.agents, partition_index, source, endpoint);
}

bool lookup_rasn_runtime_endpoint_in_registry(const std::string &module, uint32_t partition_index, runtime_endpoint *endpoint)
{
    const uint32_t partition_count = rasn_runtime_partition_count(module);
    if (partition_count > 1 &&
        lookup_rasn_runtime_endpoint_capability(module,
                                                partition_index,
                                                rasn_runtime_module_shard_capability(module, partition_index),
                                                "registry:shard",
                                                endpoint))
    {
        return true;
    }
    return lookup_rasn_runtime_endpoint_capability(
        module, partition_index, rasn_runtime_module_capability(module), "registry", endpoint);
}

runtime_endpoint resolve_rasn_runtime_partition_endpoint(const std::string &module, uint32_t partition_index)
{
    // An operator-declared endpoint wins over registry discovery. When the app was
    // configured with an explicit runtime address ([rasn.service] rasn_runtime_host
    // / _uri, or the per-module / per-shard variants) it must be honored verbatim:
    // the app is meant to reach the runtime at exactly that address and should not
    // fall back to a discovered primary_address() that a co-located or
    // differently-homed client may be unable to reach. Discovery remains the
    // mechanism for modules whose placement is left unconfigured (dynamic/sharded
    // fleets), and the same static endpoint is the final localhost fallback.
    const runtime_endpoint static_endpoint = static_rasn_runtime_endpoint(module, partition_index);
    if (static_endpoint.explicit_config)
    {
        return static_endpoint;
    }
    runtime_endpoint endpoint;
    if (lookup_rasn_runtime_endpoint_in_registry(module, partition_index, &endpoint))
    {
        return endpoint;
    }
    return static_endpoint;
}

runtime_endpoint resolve_rasn_runtime_endpoint(const std::string &module, const std::string &key = "")
{
    return resolve_rasn_runtime_partition_endpoint(module, rasn_runtime_partition_for_key(module, key));
}

runtime_endpoint resolve_rasn_runtime_endpoint(const rasn_runtime_request &request)
{
    return resolve_rasn_runtime_partition_endpoint(request.module, rasn_runtime_partition_for_request(request));
}

::dsn::rpc_address rasn_runtime_address(const std::string &module)
{
    return resolve_rasn_runtime_endpoint(module).address;
}

std::string runtime_endpoint_label(const runtime_endpoint &endpoint)
{
    std::ostringstream output;
    output << endpoint.source << ":" << endpoint.address.to_string();
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

::dsn::task_code rpc_code_for_module(const std::string &module)
{
    if (module == "agent_control_plane") return RPC_RASN_AGENT_CONTROL;
    if (module == "agent_message_bus") return RPC_RASN_MESSAGE_BUS;
    if (module == "task_orchestration_kernel") return RPC_RASN_TASK_ORCHESTRATION;
    if (module == "determinism_ledger") return RPC_RASN_DETERMINISM_LEDGER;
    if (module == "capability_directory") return RPC_RASN_CAPABILITY_DIRECTORY;
    if (module == "resource_budget") return RPC_RASN_RESOURCE_BUDGET;
    if (module == "recovery_supervisor") return RPC_RASN_RECOVERY_SUPERVISOR;
    if (module == "blackboard") return RPC_RASN_BLACKBOARD;
    if (module == "contract_verifier") return RPC_RASN_CONTRACT_VERIFIER;
    if (module == "human_interaction") return RPC_RASN_HUMAN_INTERACTION;
    if (module == "sandbox_runtime") return RPC_RASN_SANDBOX_RUNTIME;
    return RPC_RASN_AGENT_CONTROL;
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

rasn_runtime_response make_rasn_runtime_response(const rasn_runtime_request &request)
{
    rasn_runtime_response response;
    response.module = request.module;
    response.operation = request.operation;
    response.key = request.key;
    response.payload = request.payload;
    response.route_partition = request.route_partition;
    response.trace_id = request.trace_id;
    return response;
}

rasn_runtime_response make_rasn_runtime_error(const rasn_runtime_request &request, const std::string &error)
{
    rasn_runtime_response response = make_rasn_runtime_response(request);
    response.ok = false;
    response.error = error;
    return response;
}

void force_module(rasn_runtime_request *request, const std::string &module)
{
    if (request != nullptr)
    {
        request->module = module;
    }
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

rasn_runtime_response success_response(const rasn_runtime_request &request, const std::string &payload = "")
{
    rasn_runtime_response response = make_rasn_runtime_response(request);
    response.payload = payload;
    return response;
}

rasn_runtime_response bool_response(const rasn_runtime_request &request, bool ok, const std::string &error = "")
{
    rasn_runtime_response response = make_rasn_runtime_response(request);
    response.ok = ok;
    response.error = ok ? "" : error;
    return response;
}

class rasn_runtime_service_store
{
public:
    rasn_runtime_response dispatch(const rasn_runtime_request &request)
    {
        if (request.operation.find("mirror_state:") == 0)
        {
            return success_response(request);
        }
        if (request.operation == "ping")
        {
            if (!has_module(rasn_runtime_module_names(), request.module))
            {
                return make_rasn_runtime_error(request, "unknown runtime module: " + request.module);
            }
            return success_response(request, encode_fields({{"module", request.module}, {"status", "ok"}}));
        }
        // Idempotency: mutating requests with an id install an in-flight placeholder
        // before applying the module operation. A duplicate with the same
        // module/operation/key/payload/id waits for that first response, then returns
        // it instead of applying the mutation twice. Read-only operations bypass this
        // cache so observation calls are never retained solely because a client sent
        // a request id.
        if (idempotency_dedup_enabled(request))
        {
            rasn_runtime_response cached;
            std::string dedup_key_value;
            const dedup_begin_result dedup = begin_dedup(request, &cached, &dedup_key_value);
            if (dedup == dedup_begin_result::cached)
            {
                return cached;
            }
            bool finished = false;
            dedup_completion_guard guard(this, dedup_key_value, dedup == dedup_begin_result::owner, &finished);
            rasn_runtime_response response;
            try
            {
                response = route_module_request(request);
            }
            catch (const std::exception &ex)
            {
                response = make_rasn_runtime_error(request, std::string("runtime module dispatch threw: ") + ex.what());
            }
            catch (...)
            {
                response = make_rasn_runtime_error(request, "runtime module dispatch threw an unknown exception");
            }
            if (dedup == dedup_begin_result::owner)
            {
                finish_dedup(dedup_key_value, response);
                finished = true;
            }
            return response;
        }
        return route_module_request(request);
    }

    bool hydrate_from_state(const std::vector<state_record> &records,
                            const std::vector<std::string> &hosted_modules,
                            size_t *applied,
                            std::string *error)
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

            const rasn_runtime_response response = hydrate_record(module, kind, record);
            if (response.ok)
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
                    *error = response.error.empty() ? "runtime hydration record failed" : response.error;
                }
                dwarn("failed to hydrate runtime module %s record %s: %s",
                      module.c_str(),
                      record.key.c_str(),
                      response.error.c_str());
            }
        }
        return ok;
    }

private:
    rasn_runtime_response route_module_request(const rasn_runtime_request &request)
    {
        if (request.module == "agent_control_plane") return dispatch_agent_control(request);
        if (request.module == "agent_message_bus") return dispatch_message_bus(request);
        if (request.module == "task_orchestration_kernel") return dispatch_orchestration(request);
        if (request.module == "determinism_ledger") return dispatch_determinism(request);
        if (request.module == "capability_directory") return dispatch_capabilities(request);
        if (request.module == "resource_budget") return dispatch_budget(request);
        if (request.module == "recovery_supervisor") return dispatch_recovery(request);
        if (request.module == "blackboard") return dispatch_blackboard(request);
        if (request.module == "contract_verifier") return dispatch_contracts(request);
        if (request.module == "human_interaction") return dispatch_human(request);
        if (request.module == "sandbox_runtime") return dispatch_sandbox(request);
        return make_rasn_runtime_error(request, "unknown runtime module: " + request.module);
    }

    rasn_runtime_response hydrate_record(const std::string &module, const std::string &kind, const state_record &record)
    {
        rasn_runtime_request request;
        request.module = module;
        request.key = record.key;
        request.payload = record.value;
        if (module == "agent_control_plane" && kind == "agent")
        {
            request.operation = "hydrate_agent";
        }
        else if (module == "agent_message_bus" && kind == "message")
        {
            request.operation = "hydrate_message";
        }
        else if (module == "task_orchestration_kernel" && kind == "task")
        {
            request.operation = "hydrate_task";
        }
        else if (module == "determinism_ledger" && kind == "choice")
        {
            request.operation = "hydrate_choice";
        }
        else if (module == "capability_directory" && kind == "provider")
        {
            request.operation = "upsert_provider";
        }
        else if (module == "resource_budget" && kind == "quota")
        {
            request.operation = "configure";
        }
        else if (module == "resource_budget" && kind == "usage")
        {
            request.operation = "hydrate_usage";
        }
        else if (module == "recovery_supervisor" && kind == "policy")
        {
            request.operation = "set_policy";
        }
        else if (module == "recovery_supervisor" && kind == "failure")
        {
            request.operation = "hydrate_failure";
        }
        else if (module == "blackboard" && kind == "entry")
        {
            request.operation = "hydrate_entry";
        }
        else if (module == "contract_verifier" && kind == "contract")
        {
            request.operation = "register";
        }
        else if (module == "human_interaction" && kind == "request")
        {
            request.operation = "hydrate_request";
        }
        else if (module == "sandbox_runtime" && kind == "profile")
        {
            request.operation = "set_profile";
        }
        else
        {
            return make_rasn_runtime_error(request, "unsupported runtime mirror kind: " + module + "/" + kind);
        }
        return route_module_request(request);
    }

    struct dedup_entry
    {
        bool in_flight = false;
        rasn_runtime_response response;
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

    static bool idempotency_dedup_enabled(const rasn_runtime_request &request)
    {
        return !request.request_id.empty() && runtime_operation_is_mutating(request.operation) && dedup_capacity() != 0;
    }

    static std::string dedup_key(const rasn_runtime_request &request)
    {
        return request.module + "\x1f" + request.operation + "\x1f" + request.key + "\x1f" + request.payload + "\x1f" +
               request.request_id + "\x1f" + std::to_string(request.route_partition);
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

    dedup_begin_result begin_dedup(const rasn_runtime_request &request,
                                   rasn_runtime_response *cached,
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

    void finish_dedup(const std::string &key, const rasn_runtime_response &response)
    {
        const uint64_t now_ms = ::dsn_now_ms();
        const uint64_t ttl_ms = dedup_ttl_ms();
        std::lock_guard<std::mutex> guard(_dedup_lock);
        if (!response.ok)
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

    rasn_runtime_response dispatch_agent_control(const rasn_runtime_request &request)
    {
        std::string error;
        if (request.operation == "hydrate_agent")
        {
            agent_control_record record;
            if (!decode_agent_control_payload(request.payload, &record, &error)) return make_rasn_runtime_error(request, error);
            return bool_response(request, _agent_control.hydrate_agent(record, &error), error);
        }
        if (request.operation == "upsert_agent")
        {
            agent_control_record record;
            if (!decode_agent_control_payload(request.payload, &record, &error)) return make_rasn_runtime_error(request, error);
            return bool_response(request, _agent_control.upsert_agent(record, &error), error);
        }
        if (request.operation == "acquire_lease")
        {
            field_map fields;
            if (!parse_payload(request.payload, &fields, &error)) return make_rasn_runtime_error(request, error);
            const agent_control_lease lease = _agent_control.acquire_lease(
                request.key, field_string(fields, "owner"), field_uint64(fields, "now_ms"), field_uint64(fields, "lease_ms"));
            return success_response(request, encode_lease_payload(lease));
        }
        if (request.operation == "heartbeat")
        {
            return bool_response(request, _agent_control.heartbeat(request.key, field_uint64_payload(request.payload), &error), error);
        }
        if (request.operation == "find")
        {
            agent_control_record record;
            if (!_agent_control.find(request.key, &record)) return make_rasn_runtime_error(request, "agent not found: " + request.key);
            return success_response(request, encode_agent_control_payload(record));
        }
        if (request.operation == "expire_leases")
        {
            const size_t expired = _agent_control.expire_leases(field_uint64_payload(request.payload));
            return success_response(request, encode_fields({{"count", std::to_string(expired)}}));
        }
        if (request.operation == "list")
        {
            field_map fields;
            if (!parse_payload(request.payload, &fields, &error)) return make_rasn_runtime_error(request, error);
            return success_response(request,
                                    encode_items(_agent_control.list(field_bool(fields, "include_expired"),
                                                                     field_uint64(fields, "now_ms")),
                                                 encode_agent_control_payload));
        }
        if (request.operation == "describe")
        {
            return success_response(request, _agent_control.describe(field_uint64_payload(request.payload)));
        }
        return make_rasn_runtime_error(request, "unsupported agent_control_plane operation: " + request.operation);
    }

    uint64_t field_uint64_payload(const std::string &payload) const
    {
        field_map fields;
        std::string error;
        if (parse_payload(payload, &fields, &error))
        {
            return field_uint64(fields, "value");
        }
        return 0;
    }

    rasn_runtime_response dispatch_message_bus(const rasn_runtime_request &request)
    {
        std::string error;
        if (request.operation == "hydrate_message")
        {
            agent_message message;
            if (!decode_message_payload(request.payload, &message, &error)) return make_rasn_runtime_error(request, error);
            return bool_response(request, _message_bus.hydrate_message(message, &error), error);
        }
        if (request.operation == "publish")
        {
            agent_message message;
            if (!decode_message_payload(request.payload, &message, &error)) return make_rasn_runtime_error(request, error);
            agent_message stored;
            if (!_message_bus.publish(message, &stored, &error)) return make_rasn_runtime_error(request, error);
            return success_response(request, encode_message_payload(stored));
        }
        if (request.operation == "ack") return bool_response(request, _message_bus.ack(request.key, &error), error);
        if (request.operation == "dead_letter") return bool_response(request, _message_bus.dead_letter(request.key, request.payload, &error), error);
        if (request.operation == "find")
        {
            agent_message message;
            if (!_message_bus.find(request.key, &message)) return make_rasn_runtime_error(request, "message not found: " + request.key);
            return success_response(request, encode_message_payload(message));
        }
        if (request.operation == "snapshot") return success_response(request, encode_items(_message_bus.snapshot(), encode_message_payload));
        if (request.operation == "describe") return success_response(request, "messages=" + std::to_string(_message_bus.snapshot().size()));
        return make_rasn_runtime_error(request, "unsupported agent_message_bus operation: " + request.operation);
    }

    rasn_runtime_response dispatch_orchestration(const rasn_runtime_request &request)
    {
        std::string error;
        if (request.operation == "hydrate_task")
        {
            orchestration_task task;
            if (!decode_task_payload(request.payload, &task, &error)) return make_rasn_runtime_error(request, error);
            return bool_response(request, _orchestration.hydrate_task(task, &error), error);
        }
        if (request.operation == "add_task")
        {
            orchestration_task task;
            if (!decode_task_payload(request.payload, &task, &error)) return make_rasn_runtime_error(request, error);
            return bool_response(request, _orchestration.add_task(task, &error), error);
        }
        if (request.operation == "start") return bool_response(request, _orchestration.start(request.key, request.payload, &error), error);
        if (request.operation == "complete") return bool_response(request, _orchestration.complete(request.key, request.payload, &error), error);
        if (request.operation == "fail")
        {
            field_map fields;
            if (!parse_payload(request.payload, &fields, &error)) return make_rasn_runtime_error(request, error);
            return bool_response(
                request, _orchestration.fail(request.key, field_string(fields, "error"), field_bool(fields, "retryable"), &error), error);
        }
        if (request.operation == "find")
        {
            orchestration_task task;
            if (!_orchestration.find(request.key, &task)) return make_rasn_runtime_error(request, "task not found: " + request.key);
            return success_response(request, encode_task_payload(task));
        }
        if (request.operation == "snapshot") return success_response(request, encode_items(_orchestration.snapshot(), encode_task_payload));
        if (request.operation == "ready_tasks") return success_response(request, encode_items(_orchestration.ready_tasks(field_uint64_payload(request.payload)), encode_task_payload));
        if (request.operation == "blocked_tasks") return success_response(request, encode_items(_orchestration.blocked_tasks(), encode_task_payload));
        if (request.operation == "describe") return success_response(request, "tasks=" + std::to_string(_orchestration.snapshot().size()));
        return make_rasn_runtime_error(request, "unsupported task_orchestration_kernel operation: " + request.operation);
    }

    rasn_runtime_response dispatch_determinism(const rasn_runtime_request &request)
    {
        std::string error;
        if (request.operation == "hydrate_choice")
        {
            deterministic_choice choice;
            if (!decode_choice_payload(request.payload, &choice, &error)) return make_rasn_runtime_error(request, error);
            return bool_response(request, _determinism.hydrate_choice(choice, &error), error);
        }
        if (request.operation == "record")
        {
            field_map fields;
            if (!parse_payload(request.payload, &fields, &error)) return make_rasn_runtime_error(request, error);
            deterministic_choice choice;
            if (!_determinism.record(field_string(fields, "task_id"),
                                     field_string(fields, "key"),
                                     field_string(fields, "source"),
                                     field_string(fields, "value"),
                                     &choice,
                                     &error))
            {
                return make_rasn_runtime_error(request, error);
            }
            return success_response(request, encode_choice_payload(choice));
        }
        if (request.operation == "snapshot") return success_response(request, encode_items(_determinism.snapshot(), encode_choice_payload));
        if (request.operation == "describe") return success_response(request, "choices=" + std::to_string(_determinism.snapshot().size()));
        return make_rasn_runtime_error(request, "unsupported determinism_ledger operation: " + request.operation);
    }

    rasn_runtime_response dispatch_capabilities(const rasn_runtime_request &request)
    {
        std::string error;
        if (request.operation == "upsert_provider")
        {
            capability_provider provider;
            if (!decode_capability_provider_payload(request.payload, &provider, &error)) return make_rasn_runtime_error(request, error);
            return bool_response(request, _capabilities.upsert_provider(provider, &error), error);
        }
        if (request.operation == "describe") return success_response(request, _capabilities.describe());
        return make_rasn_runtime_error(request, "unsupported capability_directory operation: " + request.operation);
    }

    rasn_runtime_response dispatch_budget(const rasn_runtime_request &request)
    {
        std::string error;
        if (request.operation == "hydrate_usage")
        {
            resource_usage usage;
            if (!decode_usage_payload(request.payload, &usage, &error)) return make_rasn_runtime_error(request, error);
            return bool_response(request, _budgets.hydrate_usage(usage, &error), error);
        }
        if (request.operation == "configure")
        {
            resource_quota quota;
            if (!decode_quota_payload(request.payload, &quota, &error)) return make_rasn_runtime_error(request, error);
            return bool_response(request, _budgets.configure(quota, &error), error);
        }
        if (request.operation == "reserve")
        {
            resource_request resource_request_value;
            if (!decode_request_payload(request.payload, &resource_request_value, &error)) return make_rasn_runtime_error(request, error);
            return success_response(request, encode_decision_payload(_budgets.reserve(resource_request_value)));
        }
        if (request.operation == "release")
        {
            resource_request resource_request_value;
            if (!decode_request_payload(request.payload, &resource_request_value, &error)) return make_rasn_runtime_error(request, error);
            return bool_response(request, _budgets.release(resource_request_value, &error), error);
        }
        if (request.operation == "usage")
        {
            resource_usage usage;
            if (!_budgets.usage(request.key, &usage)) return make_rasn_runtime_error(request, "budget usage not found: " + request.key);
            return success_response(request, encode_usage_payload(usage));
        }
        if (request.operation == "describe") return success_response(request, _budgets.describe());
        return make_rasn_runtime_error(request, "unsupported resource_budget operation: " + request.operation);
    }

    rasn_runtime_response dispatch_recovery(const rasn_runtime_request &request)
    {
        std::string error;
        if (request.operation == "hydrate_failure")
        {
            failure_observation failure;
            if (!decode_failure_payload(request.payload, &failure, &error)) return make_rasn_runtime_error(request, error);
            return bool_response(request, _recovery.hydrate_failure(failure, &error), error);
        }
        if (request.operation == "set_policy")
        {
            recovery_policy policy;
            if (!decode_recovery_policy_payload(request.payload, &policy, &error)) return make_rasn_runtime_error(request, error);
            return bool_response(request, _recovery.set_policy(policy, &error), error);
        }
        if (request.operation == "observe")
        {
            failure_observation failure;
            if (!decode_failure_payload(request.payload, &failure, &error)) return make_rasn_runtime_error(request, error);
            return success_response(request, encode_recovery_action_payload(_recovery.observe(failure)));
        }
        if (request.operation == "describe") return success_response(request, _recovery.describe());
        return make_rasn_runtime_error(request, "unsupported recovery_supervisor operation: " + request.operation);
    }

    rasn_runtime_response dispatch_blackboard(const rasn_runtime_request &request)
    {
        std::string error;
        if (request.operation == "hydrate_entry")
        {
            blackboard_entry entry;
            if (!decode_blackboard_payload(request.payload, &entry, &error)) return make_rasn_runtime_error(request, error);
            return bool_response(request, _blackboard.hydrate_entry(entry, &error), error);
        }
        if (request.operation == "put")
        {
            blackboard_entry entry;
            if (!decode_blackboard_payload(request.payload, &entry, &error)) return make_rasn_runtime_error(request, error);
            blackboard_entry stored;
            if (!_blackboard.put(entry, &stored, &error)) return make_rasn_runtime_error(request, error);
            return success_response(request, encode_blackboard_payload(stored));
        }
        if (request.operation == "get")
        {
            blackboard_entry entry;
            if (!_blackboard.get(request.key, &entry)) return make_rasn_runtime_error(request, "blackboard entry not found: " + request.key);
            return success_response(request, encode_blackboard_payload(entry));
        }
        if (request.operation == "snapshot")
        {
            field_map fields;
            if (!parse_payload(request.payload, &fields, &error)) return make_rasn_runtime_error(request, error);
            return success_response(
                request,
                encode_items(_blackboard.snapshot(field_bool(fields, "include_expired", true), field_uint64(fields, "now_ms")),
                             encode_blackboard_payload));
        }
        if (request.operation == "describe") return success_response(request, _blackboard.describe());
        return make_rasn_runtime_error(request, "unsupported blackboard operation: " + request.operation);
    }

    rasn_runtime_response dispatch_contracts(const rasn_runtime_request &request)
    {
        std::string error;
        if (request.operation == "register")
        {
            agent_contract contract;
            if (!decode_contract_payload(request.payload, &contract, &error)) return make_rasn_runtime_error(request, error);
            return bool_response(request, _contracts.register_contract(contract, &error), error);
        }
        if (request.operation == "evaluate_input")
        {
            field_map fields;
            if (!parse_payload(request.payload, &fields, &error)) return make_rasn_runtime_error(request, error);
            return success_response(request,
                                    encode_contract_evaluation_payload(_contracts.evaluate_input(field_string(fields, "contract_id"),
                                                                                                field_string(fields, "input"))));
        }
        if (request.operation == "evaluate_output")
        {
            field_map fields;
            if (!parse_payload(request.payload, &fields, &error)) return make_rasn_runtime_error(request, error);
            return success_response(
                request,
                encode_contract_evaluation_payload(_contracts.evaluate_output(field_string(fields, "contract_id"),
                                                                              field_string(fields, "output"),
                                                                              field_values(fields, "policy_label"))));
        }
        if (request.operation == "describe") return success_response(request, _contracts.describe());
        return make_rasn_runtime_error(request, "unsupported contract_verifier operation: " + request.operation);
    }

    rasn_runtime_response dispatch_human(const rasn_runtime_request &request)
    {
        std::string error;
        if (request.operation == "hydrate_request")
        {
            human_interaction_request human_request;
            if (!decode_human_payload(request.payload, &human_request, &error)) return make_rasn_runtime_error(request, error);
            return bool_response(request, _human.hydrate_request(human_request, &error), error);
        }
        if (request.operation == "snapshot") return success_response(request, encode_items(_human.snapshot(), encode_human_payload));
        if (request.operation == "pending") return success_response(request, encode_items(_human.pending(request.key), encode_human_payload));
        if (request.operation == "describe") return success_response(request, _human.describe());
        return make_rasn_runtime_error(request, "unsupported human_interaction operation: " + request.operation);
    }

    rasn_runtime_response dispatch_sandbox(const rasn_runtime_request &request)
    {
        ::dsn::service::zauto_lock guard(_sandbox_lock);
        std::string error;
        if (request.operation == "set_profile")
        {
            sandbox_profile profile;
            if (!decode_sandbox_profile_payload(request.payload, &profile, &error)) return make_rasn_runtime_error(request, error);
            _sandbox_profile = profile;
            return success_response(request, encode_sandbox_profile_payload(_sandbox_profile));
        }
        if (request.operation == "profile") return success_response(request, encode_sandbox_profile_payload(_sandbox_profile));
        if (request.operation == "evaluate")
        {
            sandbox_request sandbox_request_value;
            if (!decode_sandbox_request_payload(request.payload, &sandbox_request_value, &error)) return make_rasn_runtime_error(request, error);
            return success_response(request, encode_sandbox_decision_payload(evaluate_sandbox_request(_sandbox_profile, sandbox_request_value)));
        }
        if (request.operation == "describe") return success_response(request, describe_sandbox_profile(_sandbox_profile));
        return make_rasn_runtime_error(request, "unsupported sandbox_runtime operation: " + request.operation);
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
};

rasn_runtime_service_store &global_rasn_runtime_store()
{
    static rasn_runtime_service_store store;
    return store;
}

rasn_runtime_request make_module_request(const std::string &module,
                                          const std::string &operation,
                                          const std::string &key = "",
                                          const std::string &payload = "")
{
    rasn_runtime_request request;
    request.module = module;
    request.operation = operation;
    request.key = key;
    request.payload = payload;
    // Propagate the ambient trace id (installed by the operation origin or the
    // server RPC ingress) so this module request is correlated with its cause.
    request.trace_id = current_rasn_runtime_trace_id();
    return request;
}

std::string scalar_payload(uint64_t value)
{
    return encode_fields({{"value", std::to_string(value)}});
}

void set_response_error(const rasn_runtime_response &response, std::string *error)
{
    if (error != nullptr)
    {
        *error = response.error.empty() ? "runtime module API request failed" : response.error;
    }
}

void clear_error(std::string *error)
{
    if (error != nullptr)
    {
        error->clear();
    }
}

bool response_bool(const rasn_runtime_response &response, std::string *error)
{
    if (!response.ok)
    {
        set_response_error(response, error);
        return false;
    }
    clear_error(error);
    return true;
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

// In-process invocation: dispatch directly when no rDSN node is active (CLI
// bootstrap), otherwise hop onto the module's LPC queue so the call is serialized
// with the rest of that module's work.
rasn_runtime_response invoke_local_module(const rasn_runtime_request &request)
{
    if (::dsn::task::get_current_node2() == nullptr)
    {
        return dispatch_rasn_runtime_request(request);
    }
    std::shared_ptr<std::promise<rasn_runtime_response>> promise(new std::promise<rasn_runtime_response>());
    std::future<rasn_runtime_response> future = promise->get_future();
    ::dsn::task_ptr task = ::dsn::tasking::enqueue(
        lpc_code_for_module(request.module),
        nullptr,
        [request, promise]() { promise->set_value(dispatch_rasn_runtime_request(request)); });
    if (task == nullptr)
    {
        return make_rasn_runtime_error(request, "failed to enqueue runtime module LPC request");
    }
    return future.get();
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

// Remote invocation with the runtime-owned resilience policy: a per-endpoint
// circuit breaker fast-fails while a module endpoint is unhealthy, an idempotency
// id makes retries safe against lost replies, and transient transport errors are
// retried with linear backoff before the call is surfaced as an error to the
// facade.
rasn_runtime_response invoke_remote_module(const rasn_runtime_request &request)
{
    const std::string &module = request.module;
    if (!rasn_runtime_rpc_context_available())
    {
        return make_rasn_runtime_error(request, rasn_runtime_no_node_context_error(module));
    }
    const runtime_endpoint resolved_endpoint = resolve_rasn_runtime_endpoint(request);
    const ::dsn::rpc_address address = resolved_endpoint.address;
    const uint64_t partition_hash = rasn_runtime_partition_hash(request);
    const std::string endpoint = std::string(address.to_string());
    const std::string breaker_key =
        rasn_runtime_breaker_key(module, resolved_endpoint);
    rasn_runtime_request sending = request;
    if (rasn_runtime_module_is_sharded(module) &&
        resolved_endpoint.partition_count > 1)
    {
        // Keep the wire-level ingress hint aligned with the partition selected
        // from the stable key hash. The RPC header still carries the full hash so
        // URI addresses let rDSN resolve the authoritative replica-group endpoint.
        sending.route_partition = resolved_endpoint.partition_index;
    }
    if (sending.request_id.empty() && rasn_runtime_idempotency_enabled())
    {
        sending.request_id = generate_rasn_runtime_request_id();
    }
    std::string auth_error;
    if (!prepare_rasn_runtime_rpc_request(&sending, &auth_error))
    {
        return make_rasn_runtime_error(request, auth_error);
    }

    const std::chrono::milliseconds timeout = rasn_runtime_rpc_timeout(module);
    const uint32_t max_attempts = rasn_runtime_rpc_max_attempts(module);
    const uint64_t backoff_ms = rasn_runtime_rpc_backoff_ms(module);
    rpc_resilience_options lease_options;
    lease_options.max_attempts = max_attempts;
    lease_options.backoff_ms = backoff_ms;
    breaker_decision admission;
    const bool breaker_enabled = rasn_runtime_breaker_enabled();
    if (breaker_enabled)
    {
        ensure_rasn_runtime_breaker_config();
        admission = global_rasn_runtime_breakers().allow(
            breaker_key,
            ::dsn_now_ms(),
            rpc_breaker_probe_lease_hint(lease_options, timeout));
        if (!admission.allowed)
        {
            dwarn("runtime module '%s' endpoint '%s' circuit breaker %s; short-circuiting RPC%s%s",
                  module.c_str(),
                  endpoint.c_str(),
                  to_string(admission.state),
                  admission.error.empty() ? "" : ": ",
                  admission.error.c_str());
            return make_rasn_runtime_error(
                request,
                std::string("runtime module circuit breaker ") + to_string(admission.state) +
                    (admission.error.empty() ? "" : ": " + admission.error));
        }
    }

    for (uint32_t attempt = 1; attempt <= max_attempts; ++attempt)
    {
        rasn_runtime_client client(address);
        const std::pair< ::dsn::error_code, rasn_runtime_response> result =
            client.call_sync(sending, timeout, 0, partition_hash);
        if (result.first == ::dsn::ERR_OK)
        {
            if (breaker_enabled)
            {
                const breaker_report reported = global_rasn_runtime_breakers().report(
                    breaker_key, admission, true, ::dsn_now_ms());
                if (!reported.available)
                {
                    dwarn("runtime module '%s' endpoint '%s' circuit breaker report failed: %s",
                          module.c_str(),
                          endpoint.c_str(),
                          reported.error.c_str());
                }
            }
            return result.second;
        }
        if (attempt >= max_attempts || !is_retryable_rasn_runtime_error(result.first))
        {
            if (breaker_enabled)
            {
                const breaker_report reported = global_rasn_runtime_breakers().report(
                    breaker_key, admission, false, ::dsn_now_ms());
                if (!reported.available)
                {
                    dwarn("runtime module '%s' endpoint '%s' circuit breaker report failed: %s",
                          module.c_str(),
                          endpoint.c_str(),
                          reported.error.c_str());
                }
                else if (reported.opened)
                {
                    dwarn("runtime module '%s' endpoint '%s' circuit breaker opened after %u consecutive failures",
                          module.c_str(),
                          endpoint.c_str(),
                          static_cast<unsigned int>(reported.consecutive_failures));
                }
            }
            return make_rasn_runtime_error(
                request, std::string("runtime module RPC failed: ") + result.first.to_string());
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
    return make_rasn_runtime_error(request, std::string("runtime module RPC failed"));
}

// Health ping over RPC: uses a dedicated short timeout and a single attempt, and
// respects the circuit breaker (skipping the probe while open) so a multi-node
// readiness sweep stays fast when an endpoint is down.
bool ping_remote_module(const std::string &module, std::string *error)
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
        const runtime_endpoint resolved_endpoint = resolve_rasn_runtime_partition_endpoint(module, partition_index);
        const ::dsn::rpc_address address = resolved_endpoint.address;
        const std::string endpoint = std::string(address.to_string());
        const std::string breaker_key =
            rasn_runtime_breaker_key(module, resolved_endpoint);
        const bool breaker_enabled = rasn_runtime_breaker_enabled();
        const std::chrono::milliseconds ping_timeout = rasn_runtime_ping_timeout(module);
        rasn_runtime_request ping = make_module_request(module, "ping");
        ping.route_partition = resolved_endpoint.partition_index;
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
        const std::pair< ::dsn::error_code, rasn_runtime_response> result =
            client.call_sync(ping, ping_timeout, 0, partition_hash);
        if (result.first != ::dsn::ERR_OK)
        {
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
    rasn_runtime_response call_module_api(const rasn_runtime_request &request) const override
    {
        return invoke_local_module(request);
    }

    bool write_state(const std::string &module,
                     const std::string &kind,
                     const std::string &key,
                     const std::string &value,
                     std::string *error) override
    {
        rasn_runtime_request request;
        request.module = module;
        request.operation = "mirror_state:" + kind;
        request.key = key;
        request.payload = value;
        const rasn_runtime_response response = call_module_api(request);
        if (!response.ok)
        {
            if (error != nullptr)
            {
                *error = response.error.empty() ? "runtime module LPC request failed" : response.error;
            }
            return false;
        }
        if (error != nullptr)
        {
            error->clear();
        }
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
    rasn_runtime_response call_module_api(const rasn_runtime_request &request) const override
    {
        return invoke_remote_module(request);
    }

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
        rasn_runtime_request request;
        request.module = module;
        request.operation = "mirror_state:" + kind;
        request.key = key;
        request.payload = value;
        const rasn_runtime_response module_response = call_module_api(request);
        if (!module_response.ok)
        {
            if (error != nullptr)
            {
                *error = module_response.error.empty() ? "runtime module RPC request failed" : module_response.error;
            }
            return false;
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
        if (error != nullptr)
        {
            error->clear();
        }
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
        return response_bool(invoke_local_module(make_module_request(module, "ping")), error);
    }

protected:
    rasn_runtime_response call_module_api(const rasn_runtime_request &request) const override
    {
        if (module_is_remote(request.module))
        {
            return invoke_remote_module(request);
        }
        return invoke_local_module(request);
    }

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
        rasn_runtime_request request;
        request.module = module;
        request.operation = "mirror_state:" + kind;
        request.key = key;
        request.payload = value;
        const rasn_runtime_response module_response = call_module_api(request);
        if (!module_response.ok)
        {
            if (error != nullptr)
            {
                *error = module_response.error.empty() ? "runtime module request failed" : module_response.error;
            }
            return false;
        }

        // Locally-routed modules keep state in-process (parity with the local
        // provider); remotely-routed modules also mirror to the shared state
        // service so the value survives a module service restart.
        if (!remote)
        {
            if (error != nullptr)
            {
                error->clear();
            }
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
        if (error != nullptr)
        {
            error->clear();
        }
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

uint64_t rasn_runtime_partition_hash(const rasn_runtime_request &request)
{
    return rasn_runtime_partition_hash_impl(request);
}

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
bool rasn_runtime_service_hosts_request(const rasn_runtime_request &request,
                                        const std::vector<uint32_t> &hosted_shards)
{
    // An empty hosted set means the service owns the whole module (or the module
    // is unsharded), so it serves every partition. Otherwise the request must
    // route to one of the shards this service actually hosts; a request for any
    // other shard is a misroute (stale registry entry, static endpoint, or a
    // direct client) and must not be allowed to mutate state this node does not own.
    if (hosted_shards.empty())
    {
        return true;
    }
    const uint32_t partition = rasn_runtime_partition_for_request(request);
    return std::find(hosted_shards.begin(), hosted_shards.end(), partition) != hosted_shards.end();
}

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
    add("human_interaction", "singleton", "human-in-the-loop request queue");
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
    report.checkpoint_path = checkpoint_path;

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
    const state_response checkpointed = services.checkpoint_state(checkpoint);
    if (!checkpointed.ok)
    {
        report.ok = false;
        report.error = checkpointed.error.empty() ? "failed to checkpoint rASN state mirror" : checkpointed.error;
        return report;
    }

    report.checkpointed_records = checkpointed.records.size();
    report.last_sequence = checkpointed.last_sequence;
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

std::vector<human_interaction_request> rasn_runtime::human_snapshot() const
{
    return _provider->human_snapshot();
}

std::vector<human_interaction_request> rasn_runtime::pending_human() const
{
    return _provider->pending_human();
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
               << " actual=single_writer_in_memory"
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
    return response_bool(call_module_api(make_module_request(module, "ping")), error);
}

std::vector<rasn_runtime_response>
rasn_runtime_provider::call_module_api_shards(const rasn_runtime_request &request) const
{
    const uint32_t partition_count = rasn_runtime_partition_count(request.module);
    if (!module_routed_remote(request.module) || partition_count <= 1)
    {
        return std::vector<rasn_runtime_response>{call_module_api(request)};
    }

    std::vector<rasn_runtime_response> responses;
    responses.reserve(partition_count);
    std::set<std::string> queried_endpoints;
    for (uint32_t i = 0; i < partition_count; ++i)
    {
        const runtime_endpoint endpoint = resolve_rasn_runtime_partition_endpoint(request.module, i);
        if (endpoint.address.type() != HOST_TYPE_URI &&
            !queried_endpoints.insert(std::string(endpoint.address.to_string())).second)
        {
            continue;
        }
        rasn_runtime_request shard_request = request;
        shard_request.route_partition = endpoint.partition_index;
        rasn_runtime_response response = call_module_api(shard_request);
        response.route_partition = endpoint.partition_index;
        responses.push_back(response);
    }
    return responses;
}

std::string rasn_runtime_provider::state_key(const std::string &module,
                                               const std::string &kind,
                                               const std::string &key) const
{
    return rasn_runtime_state_key(_config.state_prefix, module, kind, key);
}

void rasn_runtime_provider::set_sandbox_profile(const sandbox_profile &profile)
{
    const rasn_runtime_response response =
        call_module_api(make_module_request("sandbox_runtime", "set_profile", profile.name, encode_sandbox_profile_payload(profile)));
    if (!response.ok)
    {
        dwarn("failed to set sandbox runtime profile through module API: %s", response.error.c_str());
        return;
    }
    mirror_state_after_success("sandbox_runtime", "profile", profile.name, encode_sandbox_profile_payload(profile));
}

sandbox_decision rasn_runtime_provider::evaluate_sandbox(const sandbox_request &request) const
{
    const rasn_runtime_response response =
        call_module_api(make_module_request("sandbox_runtime", "evaluate", "", encode_sandbox_request_payload(request)));
    if (!response.ok)
    {
        return denied_sandbox_decision(response.error);
    }
    sandbox_decision decision;
    std::string error;
    if (!decode_sandbox_decision_payload(response.payload, &decision, &error))
    {
        return denied_sandbox_decision(error);
    }
    return decision;
}

sandbox_profile rasn_runtime_provider::sandbox() const
{
    const rasn_runtime_response response = call_module_api(make_module_request("sandbox_runtime", "profile"));
    sandbox_profile profile = default_read_only_sandbox_profile();
    if (!response.ok)
    {
        profile.name = "unavailable";
        return profile;
    }
    std::string error;
    if (!decode_sandbox_profile_payload(response.payload, &profile, &error))
    {
        profile = default_read_only_sandbox_profile();
        profile.name = "unavailable";
    }
    return profile;
}

bool rasn_runtime_provider::upsert_agent(const agent_control_record &record, std::string *error)
{
    const rasn_runtime_response response = call_module_api(make_module_request("agent_control_plane",
                                                                               "upsert_agent",
                                                                               record.descriptor.agent_id,
                                                                               encode_agent_control_payload(record)));
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
    const rasn_runtime_response response = call_module_api(make_module_request(
        "agent_control_plane",
        "acquire_lease",
        agent_id,
        encode_fields({{"owner", owner}, {"now_ms", std::to_string(now_ms)}, {"lease_ms", std::to_string(lease_ms)}})));
    agent_control_lease lease;
    lease.agent_id = agent_id;
    lease.owner = owner;
    if (!response.ok)
    {
        lease.error = response.error;
        return lease;
    }
    std::string error;
    if (!decode_lease_payload(response.payload, &lease, &error))
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
    const rasn_runtime_response response =
        call_module_api(make_module_request("agent_control_plane", "heartbeat", agent_id, scalar_payload(now_ms)));
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
    const rasn_runtime_response response = call_module_api(make_module_request("agent_control_plane", "find", agent_id));
    if (!response.ok)
    {
        return false;
    }
    std::string error;
    return decode_agent_control_payload(response.payload, record, &error);
}

size_t rasn_runtime_provider::expire_agent_leases(uint64_t now_ms)
{
    const rasn_runtime_response response =
        call_module_api(make_module_request("agent_control_plane", "expire_leases", "*", scalar_payload(now_ms)));
    if (!response.ok)
    {
        return 0;
    }
    field_map fields;
    std::string error;
    const size_t expired = decode_fields(response.payload, &fields, &error) ? field_size(fields, "count") : 0;
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
    const rasn_runtime_response response = call_module_api(make_module_request(
        "agent_control_plane",
        "list",
        "",
        encode_fields({{"include_expired", include_expired ? "true" : "false"}, {"now_ms", std::to_string(now_ms)}})));
    std::vector<agent_control_record> records;
    std::string error;
    if (response.ok)
    {
        (void)decode_items(response.payload, &records, decode_agent_control_payload, &error);
    }
    return records;
}

std::string rasn_runtime_provider::describe_agents(uint64_t now_ms) const
{
    const rasn_runtime_response response =
        call_module_api(make_module_request("agent_control_plane", "describe", "", scalar_payload(now_ms)));
    return response.ok ? response.payload : response.error;
}

bool rasn_runtime_provider::publish_message(const agent_message &message, agent_message *stored, std::string *error)
{
    const rasn_runtime_response response =
        call_module_api(make_module_request("agent_message_bus", "publish", message.message_id, encode_message_payload(message)));
    if (!response.ok)
    {
        set_response_error(response, error);
        return false;
    }
    agent_message stored_message;
    std::string decode_error;
    if (!decode_message_payload(response.payload, &stored_message, &decode_error))
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
    const rasn_runtime_response response = call_module_api(make_module_request("agent_message_bus", "ack", message_id));
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
    const rasn_runtime_response response =
        call_module_api(make_module_request("agent_message_bus", "dead_letter", message_id, error_text));
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
    const rasn_runtime_response response = call_module_api(make_module_request("agent_message_bus", "find", message_id));
    if (!response.ok)
    {
        return false;
    }
    std::string error;
    return decode_message_payload(response.payload, message, &error);
}

std::vector<agent_message> rasn_runtime_provider::message_snapshot() const
{
    std::vector<agent_message> messages;
    for (const rasn_runtime_response &response :
         call_module_api_shards(make_module_request("agent_message_bus", "snapshot")))
    {
        std::vector<agent_message> shard_messages;
        std::string error;
        if (!response.ok)
        {
            dwarn("agent_message_bus snapshot shard %u failed: %s",
                  static_cast<unsigned int>(response.route_partition),
                  response.error.c_str());
            continue;
        }
        if (decode_items(response.payload, &shard_messages, decode_message_payload, &error))
        {
            messages.insert(messages.end(), shard_messages.begin(), shard_messages.end());
        }
        else
        {
            dwarn("agent_message_bus snapshot shard %u decode failed: %s",
                  static_cast<unsigned int>(response.route_partition),
                  error.c_str());
        }
    }
    return messages;
}

bool rasn_runtime_provider::add_task(const orchestration_task &task, std::string *error)
{
    const rasn_runtime_response response =
        call_module_api(make_module_request("task_orchestration_kernel", "add_task", task.task_id, encode_task_payload(task)));
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
    const rasn_runtime_response response =
        call_module_api(make_module_request("task_orchestration_kernel", "start", task_id, owner_agent));
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
    const rasn_runtime_response response =
        call_module_api(make_module_request("task_orchestration_kernel", "complete", task_id, output));
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
    const rasn_runtime_response response = call_module_api(make_module_request(
        "task_orchestration_kernel",
        "fail",
        task_id,
        encode_fields({{"error", error_text}, {"retryable", retryable ? "true" : "false"}})));
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
    const rasn_runtime_response response = call_module_api(make_module_request("task_orchestration_kernel", "find", task_id));
    if (!response.ok)
    {
        return false;
    }
    std::string error;
    return decode_task_payload(response.payload, task, &error);
}

std::vector<orchestration_task> rasn_runtime_provider::task_snapshot() const
{
    const rasn_runtime_response response = call_module_api(make_module_request("task_orchestration_kernel", "snapshot"));
    std::vector<orchestration_task> tasks;
    std::string error;
    if (response.ok)
    {
        (void)decode_items(response.payload, &tasks, decode_task_payload, &error);
    }
    return tasks;
}

std::vector<orchestration_task> rasn_runtime_provider::ready_tasks(uint64_t now_ms) const
{
    const rasn_runtime_response response =
        call_module_api(make_module_request("task_orchestration_kernel", "ready_tasks", "", scalar_payload(now_ms)));
    std::vector<orchestration_task> tasks;
    std::string error;
    if (response.ok)
    {
        (void)decode_items(response.payload, &tasks, decode_task_payload, &error);
    }
    return tasks;
}

std::vector<orchestration_task> rasn_runtime_provider::blocked_tasks() const
{
    const rasn_runtime_response response = call_module_api(make_module_request("task_orchestration_kernel", "blocked_tasks"));
    std::vector<orchestration_task> tasks;
    std::string error;
    if (response.ok)
    {
        (void)decode_items(response.payload, &tasks, decode_task_payload, &error);
    }
    return tasks;
}

bool rasn_runtime_provider::record_choice(const std::string &task_id,
                                            const std::string &key,
                                            const std::string &source,
                                            const std::string &value,
                                            deterministic_choice *choice,
                                            std::string *error)
{
    const rasn_runtime_response response = call_module_api(make_module_request(
        "determinism_ledger",
        "record",
        task_id + "/" + key,
        encode_fields({{"task_id", task_id}, {"key", key}, {"source", source}, {"value", value}})));
    if (!response.ok)
    {
        set_response_error(response, error);
        return false;
    }
    deterministic_choice stored_choice;
    std::string decode_error;
    if (!decode_choice_payload(response.payload, &stored_choice, &decode_error))
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
    const rasn_runtime_response response = call_module_api(make_module_request("determinism_ledger", "snapshot"));
    std::vector<deterministic_choice> choices;
    std::string error;
    if (response.ok)
    {
        (void)decode_items(response.payload, &choices, decode_choice_payload, &error);
    }
    return choices;
}

bool rasn_runtime_provider::upsert_capability_provider(const capability_provider &provider, std::string *error)
{
    capability_provider stored = provider;
    if (stored.last_seen_ms == 0)
    {
        stored.last_seen_ms = ::dsn_now_ms();
    }
    const rasn_runtime_response response = call_module_api(make_module_request("capability_directory",
                                                                               "upsert_provider",
                                                                               stored.descriptor.agent_id,
                                                                               encode_capability_provider_payload(stored)));
    if (!response_bool(response, error))
    {
        return false;
    }
    return mirror_state_after_success(
        "capability_directory", "provider", stored.descriptor.agent_id, encode_capability_provider_payload(stored), error);
}

std::string rasn_runtime_provider::describe_capabilities() const
{
    const rasn_runtime_response response = call_module_api(make_module_request("capability_directory", "describe"));
    return response.ok ? response.payload : response.error;
}

bool rasn_runtime_provider::configure_budget(const resource_quota &quota, std::string *error)
{
    const rasn_runtime_response response =
        call_module_api(make_module_request("resource_budget", "configure", quota.scope, encode_quota_payload(quota)));
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
    const rasn_runtime_response response =
        call_module_api(make_module_request("resource_budget", "reserve", request_value.scope, encode_request_payload(request_value)));
    resource_budget_decision decision;
    decision.allowed = false;
    decision.scope = request_value.scope;
    if (!response.ok)
    {
        decision.reason = response.error;
        return decision;
    }
    std::string error;
    if (!decode_decision_payload(response.payload, &decision, &error))
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
    const rasn_runtime_response response =
        call_module_api(make_module_request("resource_budget", "release", request_value.scope, encode_request_payload(request_value)));
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
    const rasn_runtime_response response = call_module_api(make_module_request("resource_budget", "usage", scope));
    if (!response.ok)
    {
        return false;
    }
    std::string error;
    return decode_usage_payload(response.payload, usage, &error);
}

std::string rasn_runtime_provider::describe_budgets() const
{
    const std::vector<rasn_runtime_response> responses =
        call_module_api_shards(make_module_request("resource_budget", "describe"));
    const uint32_t partition_count = rasn_runtime_partition_count("resource_budget");
    if (responses.size() <= 1 && (!module_routed_remote("resource_budget") || partition_count <= 1))
    {
        return responses.empty() ? "" : (responses[0].ok ? responses[0].payload : responses[0].error);
    }
    std::ostringstream output;
    for (size_t i = 0; i < responses.size(); ++i)
    {
        if (i != 0)
        {
            output << "\n";
        }
        output << "shard" << responses[i].route_partition << ": "
               << (responses[i].ok ? responses[i].payload : responses[i].error);
    }
    return output.str();
}

bool rasn_runtime_provider::set_recovery_policy(const recovery_policy &policy, std::string *error)
{
    const rasn_runtime_response response = call_module_api(make_module_request("recovery_supervisor",
                                                                               "set_policy",
                                                                               policy.failure_class,
                                                                               encode_recovery_policy_payload(policy)));
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
    const rasn_runtime_response response =
        call_module_api(make_module_request("recovery_supervisor", "observe", stored_failure.task_id, encode_failure_payload(stored_failure)));
    recovery_action action;
    if (!response.ok)
    {
        action.reason = response.error;
        return action;
    }
    std::string error;
    if (!decode_recovery_action_payload(response.payload, &action, &error))
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
    const rasn_runtime_response response = call_module_api(make_module_request("recovery_supervisor", "describe"));
    return response.ok ? response.payload : response.error;
}

bool rasn_runtime_provider::put_blackboard(const blackboard_entry &entry, blackboard_entry *stored, std::string *error)
{
    const rasn_runtime_response response =
        call_module_api(make_module_request("blackboard", "put", entry.key, encode_blackboard_payload(entry)));
    if (!response.ok)
    {
        set_response_error(response, error);
        return false;
    }
    blackboard_entry stored_entry;
    std::string decode_error;
    if (!decode_blackboard_payload(response.payload, &stored_entry, &decode_error))
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
    const rasn_runtime_response response = call_module_api(make_module_request("blackboard", "get", key));
    if (!response.ok)
    {
        return false;
    }
    std::string error;
    return decode_blackboard_payload(response.payload, entry, &error);
}

std::vector<blackboard_entry> rasn_runtime_provider::blackboard_snapshot(bool include_expired, uint64_t now_ms) const
{
    std::vector<blackboard_entry> entries;
    const rasn_runtime_request request = make_module_request(
        "blackboard",
        "snapshot",
        "",
        encode_fields({{"include_expired", include_expired ? "true" : "false"}, {"now_ms", std::to_string(now_ms)}}));
    for (const rasn_runtime_response &response : call_module_api_shards(request))
    {
        std::vector<blackboard_entry> shard_entries;
        std::string error;
        if (!response.ok)
        {
            dwarn("blackboard snapshot shard %u failed: %s",
                  static_cast<unsigned int>(response.route_partition),
                  response.error.c_str());
            continue;
        }
        if (decode_items(response.payload, &shard_entries, decode_blackboard_payload, &error))
        {
            entries.insert(entries.end(), shard_entries.begin(), shard_entries.end());
        }
        else
        {
            dwarn("blackboard snapshot shard %u decode failed: %s",
                  static_cast<unsigned int>(response.route_partition),
                  error.c_str());
        }
    }
    return entries;
}

bool rasn_runtime_provider::register_contract(const agent_contract &contract, std::string *error)
{
    const rasn_runtime_response response =
        call_module_api(make_module_request("contract_verifier", "register", contract.contract_id, encode_contract_payload(contract)));
    if (!response_bool(response, error))
    {
        return false;
    }
    return mirror_state_after_success(
        "contract_verifier", "contract", contract.contract_id, encode_contract_payload(contract), error);
}

contract_evaluation rasn_runtime_provider::evaluate_input(const std::string &contract_id, const std::string &input) const
{
    const rasn_runtime_response response = call_module_api(make_module_request(
        "contract_verifier", "evaluate_input", contract_id, encode_fields({{"contract_id", contract_id}, {"input", input}})));
    if (!response.ok)
    {
        return failed_contract_evaluation(contract_id, response.error);
    }
    contract_evaluation evaluation;
    std::string error;
    return decode_contract_evaluation_payload(response.payload, &evaluation, &error)
               ? evaluation
               : failed_contract_evaluation(contract_id, error);
}

contract_evaluation rasn_runtime_provider::evaluate_output(const std::string &contract_id,
                                                             const std::string &output,
                                                             const std::vector<std::string> &policy_labels) const
{
    std::vector<std::pair<std::string, std::string>> fields;
    fields.push_back({"contract_id", contract_id});
    fields.push_back({"output", output});
    for (const std::string &label : policy_labels)
    {
        fields.push_back({"policy_label", label});
    }
    const rasn_runtime_response response =
        call_module_api(make_module_request("contract_verifier", "evaluate_output", contract_id, encode_fields(fields)));
    if (!response.ok)
    {
        return failed_contract_evaluation(contract_id, response.error);
    }
    contract_evaluation evaluation;
    std::string error;
    return decode_contract_evaluation_payload(response.payload, &evaluation, &error)
               ? evaluation
               : failed_contract_evaluation(contract_id, error);
}

std::string rasn_runtime_provider::describe_contracts() const
{
    const rasn_runtime_response response = call_module_api(make_module_request("contract_verifier", "describe"));
    return response.ok ? response.payload : response.error;
}

std::vector<human_interaction_request> rasn_runtime_provider::human_snapshot() const
{
    const rasn_runtime_response response = call_module_api(make_module_request("human_interaction", "snapshot"));
    std::vector<human_interaction_request> requests;
    std::string error;
    if (response.ok)
    {
        (void)decode_items(response.payload, &requests, decode_human_payload, &error);
    }
    return requests;
}

std::vector<human_interaction_request> rasn_runtime_provider::pending_human() const
{
    const rasn_runtime_response response = call_module_api(make_module_request("human_interaction", "pending"));
    std::vector<human_interaction_request> requests;
    std::string error;
    if (response.ok)
    {
        (void)decode_items(response.payload, &requests, decode_human_payload, &error);
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

rasn_runtime_response dispatch_rasn_runtime_request(const rasn_runtime_request &request)
{
    if (request.module.empty())
    {
        return make_rasn_runtime_error(request, "rASN runtime request missing module name");
    }
    if (request.operation.empty())
    {
        return make_rasn_runtime_error(request, "rASN runtime request missing operation");
    }
    return global_rasn_runtime_store().dispatch(request);
}

rasn_runtime_rpc_service::rasn_runtime_rpc_service(std::vector<std::string> modules)
    : ::dsn::serverlet<rasn_runtime_rpc_service>("rasn.runtime"),
      _modules(modules.empty() ? rasn_runtime_module_names() : std::move(modules))
{
    // Cache the hosted-shard set for any sharded module that hosts only a subset
    // of partitions so the ingress guard can reject misrouted requests without a
    // per-request config lookup. Modules that host the whole module (the common
    // single-process case) contribute nothing and are admitted unconditionally.
    for (const std::string &module : _modules)
    {
        std::vector<uint32_t> shards = rasn_runtime_hosted_shards(module);
        if (!shards.empty())
        {
            _hosted_shards.emplace(module, std::move(shards));
        }
    }
}

void rasn_runtime_rpc_service::open_service()
{
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
    bool registered = false;
    if (module == "agent_control_plane")
    {
        registered = this->register_async_rpc_handler(
            RPC_RASN_AGENT_CONTROL, "agent_control", &rasn_runtime_rpc_service::on_agent_control);
    }
    else if (module == "agent_message_bus")
    {
        registered = this->register_async_rpc_handler(
            RPC_RASN_MESSAGE_BUS, "message_bus", &rasn_runtime_rpc_service::on_message_bus);
    }
    else if (module == "task_orchestration_kernel")
    {
        registered = this->register_async_rpc_handler(
            RPC_RASN_TASK_ORCHESTRATION, "task_orchestration", &rasn_runtime_rpc_service::on_task_orchestration);
    }
    else if (module == "determinism_ledger")
    {
        registered = this->register_async_rpc_handler(
            RPC_RASN_DETERMINISM_LEDGER, "determinism_ledger", &rasn_runtime_rpc_service::on_determinism_ledger);
    }
    else if (module == "capability_directory")
    {
        registered = this->register_async_rpc_handler(
            RPC_RASN_CAPABILITY_DIRECTORY, "capability_directory", &rasn_runtime_rpc_service::on_capability_directory);
    }
    else if (module == "resource_budget")
    {
        registered = this->register_async_rpc_handler(
            RPC_RASN_RESOURCE_BUDGET, "resource_budget", &rasn_runtime_rpc_service::on_resource_budget);
    }
    else if (module == "recovery_supervisor")
    {
        registered = this->register_async_rpc_handler(
            RPC_RASN_RECOVERY_SUPERVISOR, "recovery_supervisor", &rasn_runtime_rpc_service::on_recovery_supervisor);
    }
    else if (module == "blackboard")
    {
        registered =
            this->register_async_rpc_handler(RPC_RASN_BLACKBOARD, "blackboard", &rasn_runtime_rpc_service::on_blackboard);
    }
    else if (module == "contract_verifier")
    {
        registered = this->register_async_rpc_handler(
            RPC_RASN_CONTRACT_VERIFIER, "contract_verifier", &rasn_runtime_rpc_service::on_contract_verifier);
    }
    else if (module == "human_interaction")
    {
        registered = this->register_async_rpc_handler(
            RPC_RASN_HUMAN_INTERACTION, "human_interaction", &rasn_runtime_rpc_service::on_human_interaction);
    }
    else if (module == "sandbox_runtime")
    {
        registered = this->register_async_rpc_handler(
            RPC_RASN_SANDBOX_RUNTIME, "sandbox_runtime", &rasn_runtime_rpc_service::on_sandbox_runtime);
    }
    else
    {
        dwarn("unknown runtime module API '%s' is not registered", module.c_str());
        return false;
    }
    return registered;
}

void rasn_runtime_rpc_service::unregister_module_handler(const std::string &module)
{
    if (!has_module(rasn_runtime_module_names(), module))
    {
        return;
    }
    this->unregister_rpc_handler(rpc_code_for_module(module));
}

void rasn_runtime_rpc_service::reply_module_request(const std::string &module,
                                                   const rasn_runtime_request &request,
                                                   ::dsn::rpc_replier<rasn_runtime_response> &reply)
{
    rasn_runtime_request copy = request;
    force_module(&copy, module);
    request_guard guard(this);
    if (!guard.active())
    {
        reply(make_rasn_runtime_error(copy, "rasn runtime service is shutting down"));
        return;
    }
    std::string auth_error;
    if (!authenticate_rasn_runtime_rpc_request(copy, &auth_error))
    {
        metrics_registry::instance().on_event("runtime.auth.rejected", module);
        dwarn("runtime module RPC auth rejected for module '%s'", module.c_str());
        reply(make_rasn_runtime_error(copy, auth_error));
        return;
    }
    copy.auth_token.clear();

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
        reply(make_rasn_runtime_error(
            copy, "rasn runtime service does not host shard " + std::to_string(partition) + " of module " + module));
        return;
    }

    // Adopt the incoming trace id for the duration of dispatch so server-side
    // logs and any nested module requests share the originating operation's trace.
    rasn_runtime_trace_scope trace(copy.trace_id);
    reply(dispatch_rasn_runtime_request(copy));
}

void rasn_runtime_rpc_service::on_agent_control(const rasn_runtime_request &request,
                                                      ::dsn::rpc_replier<rasn_runtime_response> &reply)
{
    reply_module_request("agent_control_plane", request, reply);
}

void rasn_runtime_rpc_service::on_message_bus(const rasn_runtime_request &request,
                                                    ::dsn::rpc_replier<rasn_runtime_response> &reply)
{
    reply_module_request("agent_message_bus", request, reply);
}

void rasn_runtime_rpc_service::on_task_orchestration(const rasn_runtime_request &request,
                                                           ::dsn::rpc_replier<rasn_runtime_response> &reply)
{
    reply_module_request("task_orchestration_kernel", request, reply);
}

void rasn_runtime_rpc_service::on_determinism_ledger(const rasn_runtime_request &request,
                                                           ::dsn::rpc_replier<rasn_runtime_response> &reply)
{
    reply_module_request("determinism_ledger", request, reply);
}

void rasn_runtime_rpc_service::on_capability_directory(const rasn_runtime_request &request,
                                                             ::dsn::rpc_replier<rasn_runtime_response> &reply)
{
    reply_module_request("capability_directory", request, reply);
}

void rasn_runtime_rpc_service::on_resource_budget(const rasn_runtime_request &request,
                                                        ::dsn::rpc_replier<rasn_runtime_response> &reply)
{
    reply_module_request("resource_budget", request, reply);
}

void rasn_runtime_rpc_service::on_recovery_supervisor(const rasn_runtime_request &request,
                                                            ::dsn::rpc_replier<rasn_runtime_response> &reply)
{
    reply_module_request("recovery_supervisor", request, reply);
}

void rasn_runtime_rpc_service::on_blackboard(const rasn_runtime_request &request,
                                                   ::dsn::rpc_replier<rasn_runtime_response> &reply)
{
    reply_module_request("blackboard", request, reply);
}

void rasn_runtime_rpc_service::on_contract_verifier(const rasn_runtime_request &request,
                                                          ::dsn::rpc_replier<rasn_runtime_response> &reply)
{
    reply_module_request("contract_verifier", request, reply);
}

void rasn_runtime_rpc_service::on_human_interaction(const rasn_runtime_request &request,
                                                          ::dsn::rpc_replier<rasn_runtime_response> &reply)
{
    reply_module_request("human_interaction", request, reply);
}

void rasn_runtime_rpc_service::on_sandbox_runtime(const rasn_runtime_request &request,
                                                        ::dsn::rpc_replier<rasn_runtime_response> &reply)
{
    reply_module_request("sandbox_runtime", request, reply);
}

std::pair< ::dsn::error_code, rasn_runtime_response>
rasn_runtime_client::call_sync(const rasn_runtime_request &request,
                                     std::chrono::milliseconds timeout,
                                     int thread_hash,
                                     uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<rasn_runtime_response>(::dsn::rpc::call(
        _server, rpc_code_for_module(request.module), request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

namespace {

agent_capability make_rasn_runtime_capability(const std::string &name)
{
    agent_capability capability;
    capability.name = name;
    capability.input_type = "rasn_runtime_request";
    capability.output_type = "rasn_runtime_response";
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

    rasn_state_client state(rasn_service_address("state", 27104));
    const uint32_t max_attempts = rasn_runtime_state_hydration_max_attempts();
    const std::chrono::milliseconds retry_backoff = rasn_runtime_state_hydration_retry_backoff();
    ::dsn::error_code err = ::dsn::ERR_OK;
    state_response response;
    for (uint32_t attempt = 1; attempt <= max_attempts; ++attempt)
    {
        std::tie(err, response) = state.query_sync(request, rasn_runtime_state_hydration_timeout());
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
    if (!global_rasn_runtime_store().hydrate_from_state(response.records, _rpc.modules(), &applied, &error))
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
