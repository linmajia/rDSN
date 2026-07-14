#include <rasn/coordination_service.h>

#include <dsn/c/app_model.h>
#include <dsn/service_api_cpp.h>
#include <dsn/cpp/utils.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef RASN_HAS_DIST_COORDINATION
#include <dsn/dist/distributed_lock_service.h>
#include <dsn/dist/meta_state_service.h>
// ERR_NODE_ALREADY_EXIST and other dist-service error codes are declared by the
// rDSN.dist.service plugin, not core rDSN (dsn/cpp/auto_codes.h).
#include <dsn/dist/error_code.h>

// Concrete providers live in the rDSN.dist.service submodule's private headers
// (reached via include paths added in the rASN CMake files).
#include "distributed_lock_service_simple.h"
#include "meta_state_service_simple.h"
#include "distributed_lock_service_zookeeper.h"
#include "meta_state_service_zookeeper.h"
#endif

namespace dsn {
namespace rasn {

rasn_coordination_config load_rasn_coordination_config()
{
    rasn_coordination_config cfg;
    cfg.provider = ::dsn_config_get_value_string(
        "rasn.coordination", "provider", cfg.provider.c_str(),
        "coordination backend: inproc | simple | zookeeper");
    cfg.lock_namespace = ::dsn_config_get_value_string(
        "rasn.coordination", "lock_namespace", cfg.lock_namespace.c_str(),
        "root path for ownership/leader-election locks");
    cfg.state_namespace = ::dsn_config_get_value_string(
        "rasn.coordination", "state_namespace", cfg.state_namespace.c_str(),
        "root path for cluster-shared state");
    cfg.acquire_timeout_ms = static_cast<int>(::dsn_config_get_value_uint64(
        "rasn.coordination", "acquire_timeout_ms",
        static_cast<uint64_t>(cfg.acquire_timeout_ms),
        "default timeout (ms) for blocking ownership acquisition"));
    cfg.operation_timeout_ms = static_cast<int>(::dsn_config_get_value_uint64(
        "rasn.coordination", "operation_timeout_ms",
        static_cast<uint64_t>(cfg.operation_timeout_ms),
        "timeout (ms) for coordination state I/O and ownership cleanup"));
    cfg.state_work_dir = ::dsn_config_get_value_string(
        "rasn.coordination", "state_work_dir", cfg.state_work_dir.c_str(),
        "directory for the 'simple' meta-state backend durable log");
    return cfg;
}

::dsn::error_code rasn_coordination_service::acquire_ownership(const std::string &resource_id,
                                                              const std::string &owner_id)
{
    return acquire_ownership(
        resource_id, owner_id, _default_acquire_timeout_ms, nullptr, ownership_lost_callback());
}

::dsn::error_code rasn_coordination_service::acquire_ownership(
    const std::string &resource_id, const std::string &owner_id, int timeout_ms)
{
    return acquire_ownership(resource_id, owner_id, timeout_ms, nullptr, ownership_lost_callback());
}

::dsn::error_code rasn_coordination_service::acquire_ownership(
    const std::string &resource_id,
    const std::string &owner_id,
    int timeout_ms,
    uint64_t *fencing_token)
{
    return acquire_ownership(
        resource_id, owner_id, timeout_ms, fencing_token, ownership_lost_callback());
}

::dsn::error_code rasn_coordination_service::acquire_ownership(
    const std::string &resource_id,
    const std::string &owner_id,
    const ownership_lost_callback &on_lost)
{
    return acquire_ownership(
        resource_id, owner_id, _default_acquire_timeout_ms, nullptr, on_lost);
}

::dsn::error_code rasn_coordination_service::release_ownership(
    const std::string &resource_id, const std::string &owner_id)
{
    return release_ownership(resource_id, owner_id, true);
}

bool rasn_coordination_service::query_owner(const std::string &resource_id,
                                            std::string &owner_id)
{
    return query_owner(resource_id, owner_id, nullptr);
}

#ifdef RASN_HAS_DIST_COORDINATION
// This facade delivers its OWN completion callbacks (grant/lease) on
// THREAD_POOL_META_SERVER via LPC_RASN_COORDINATION, so every app running the
// simple|zookeeper backend must declare THREAD_POOL_META_SERVER. The two dist
// backends additionally run provider-internal work on DIFFERENT pools, and the
// hosting app must declare whichever pool its backend uses or rDSN aborts at
// startup ("pool <NAME> not ready ... not designated in [apps.<app>] pools"):
//   * simple    -> distributed_lock_service_simple runs its lease timer
//                  (LPC_DIST_LOCK_SVC_RANDOM_EXPIRE) on THREAD_POOL_META_SERVER,
//                  so THREAD_POOL_META_SERVER alone suffices.
//   * zookeeper -> distributed_lock_service_zookeeper runs its lock tasks
//                  (TASK_CODE_DLOCK) on THREAD_POOL_DLOCK (declare it
//                  partitioned = true; the provider asserts single-thread access
//                  per lock). So a zookeeper-backed app must declare BOTH
//                  THREAD_POOL_META_SERVER and THREAD_POOL_DLOCK.
// rASN never processes requests on either pool (rASN handlers run on
// THREAD_POOL_DEFAULT / THREAD_POOL_RASN_WORKFLOW), so the blocking facade calls
// below use bounded waits for callbacks on a pool distinct from the caller's and
// cannot self-deadlock. The default 'inproc' backend uses no dist provider and
// never enqueues LPC_RASN_COORDINATION, so these pools are declared in the rASN
// runtime deployment config only when provider = simple|zookeeper. Declaring them
// under the default config would fail config parsing ("invalid enum configuration")
// because the providers that register the pools are not loaded.
//
// Defined here in the named dsn::rasn namespace (not the anonymous namespace
// below): the DEFINE_TASK_CODE macro emits a weak symbol, which must have
// external linkage. Placing it in the anonymous namespace gives it internal
// linkage, which clang rejects ("weak declaration cannot have internal linkage").
DEFINE_TASK_CODE(LPC_RASN_COORDINATION, TASK_PRIORITY_COMMON, THREAD_POOL_META_SERVER)
#endif

namespace {

// Join a namespace root with a relative key into a single znode-style path,
// collapsing redundant slashes ("/rasn/state" + "a/b" -> "/rasn/state/a/b").
std::string join_path(const std::string &base, const std::string &key)
{
    std::string b = base;
    while (b.size() > 1 && b.back() == '/')
        b.pop_back();
    if (key.empty())
        return b;
    std::string k = key;
    while (!k.empty() && k.front() == '/')
        k.erase(k.begin());
    while (!k.empty() && k.back() == '/')
        k.pop_back();
    if (k.empty())
        return b;
    return b + "/" + k;
}

// ---------------------------------------------------------------------------
// In-process, single-node fallback. Always available so rASN builds and runs
// even without the dist.service plugin, and so "inproc" is a valid provider.
// ---------------------------------------------------------------------------
class inproc_coordination_service : public rasn_coordination_service
{
public:
    explicit inproc_coordination_service(const rasn_coordination_config &cfg)
        : _state_namespace(cfg.state_namespace)
    {
        _default_acquire_timeout_ms = cfg.acquire_timeout_ms;
    }

    ::dsn::error_code start() override { return ::dsn::ERR_OK; }
    void stop() override {}

    ::dsn::error_code acquire_ownership(const std::string &resource_id,
                                        const std::string &owner_id,
                                        int /*timeout_ms*/,
                                        uint64_t *fencing_token,
                                        const ownership_lost_callback & /*on_lost*/) override
    {
        std::lock_guard<std::mutex> guard(_mu);
        auto it = _owners.find(resource_id);
        if (it != _owners.end() && it->second.owner == owner_id)
        {
            if (fencing_token != nullptr)
            {
                *fencing_token = it->second.version;
            }
            return ::dsn::ERR_OK;
        }
        if (it == _owners.end())
        {
            uint64_t &version = _versions[resource_id];
            if (version == (std::numeric_limits<uint64_t>::max)())
            {
                return ::dsn::ERR_CAPACITY_EXCEEDED;
            }
            ++version;
            inproc_hold held;
            held.owner = owner_id;
            held.version = version;
            _owners[resource_id] = held;
            if (fencing_token != nullptr)
            {
                *fencing_token = version;
            }
            return ::dsn::ERR_OK;
        }
        // Held by a different owner within this process.
        return ::dsn::ERR_TIMEOUT;
    }

    ::dsn::error_code release_ownership(const std::string &resource_id,
                                        const std::string &owner_id,
                                        bool destroy) override
    {
        std::lock_guard<std::mutex> guard(_mu);
        auto it = _owners.find(resource_id);
        const bool held_by_owner =
            it != _owners.end() && it->second.owner == owner_id;
        if (held_by_owner)
            _owners.erase(it);
        if (destroy && _owners.find(resource_id) == _owners.end())
            _versions.erase(resource_id);
        return ::dsn::ERR_OK;
    }

    ::dsn::error_code verify_ownership_fence(const std::string &resource_id,
                                             uint64_t fencing_token) override
    {
        std::lock_guard<std::mutex> guard(_mu);
        const auto it = _versions.find(resource_id);
        return it == _versions.end() || it->second <= fencing_token
                   ? ::dsn::ERR_OK
                   : ::dsn::ERR_INVALID_STATE;
    }

    bool query_owner(const std::string &resource_id,
                     std::string &owner_id,
                     uint64_t *fencing_token) override
    {
        std::lock_guard<std::mutex> guard(_mu);
        auto it = _owners.find(resource_id);
        if (it == _owners.end())
            return false;
        owner_id = it->second.owner;
        if (fencing_token != nullptr)
            *fencing_token = it->second.version;
        return true;
    }

    ::dsn::error_code put_state(const std::string &key, const std::string &value) override
    {
        std::lock_guard<std::mutex> guard(_mu);
        _state[join_path(_state_namespace, key)] = value;
        return ::dsn::ERR_OK;
    }

    ::dsn::error_code get_state(const std::string &key, std::string &value) override
    {
        std::lock_guard<std::mutex> guard(_mu);
        auto it = _state.find(join_path(_state_namespace, key));
        if (it == _state.end())
            return ::dsn::ERR_OBJECT_NOT_FOUND;
        value = it->second;
        return ::dsn::ERR_OK;
    }

    ::dsn::error_code delete_state(const std::string &key) override
    {
        std::lock_guard<std::mutex> guard(_mu);
        const std::string node = join_path(_state_namespace, key);
        _state.erase(node);
        const std::string prefix = node + "/";
        for (auto it = _state.begin(); it != _state.end();) {
            if (it->first.compare(0, prefix.size(), prefix) == 0)
                it = _state.erase(it);
            else
                ++it;
        }
        return ::dsn::ERR_OK;
    }

    ::dsn::error_code list_state(const std::string &key,
                                 std::vector<std::string> &children) override
    {
        std::lock_guard<std::mutex> guard(_mu);
        const std::string prefix = join_path(_state_namespace, key) + "/";
        std::set<std::string> uniq;
        for (const auto &kv : _state) {
            if (kv.first.compare(0, prefix.size(), prefix) != 0)
                continue;
            const std::string rest = kv.first.substr(prefix.size());
            const size_t slash = rest.find('/');
            const std::string child = (slash == std::string::npos) ? rest : rest.substr(0, slash);
            if (!child.empty())
                uniq.insert(child);
        }
        children.assign(uniq.begin(), uniq.end());
        return ::dsn::ERR_OK;
    }

    bool is_distributed() const override { return false; }
    const char *provider_name() const override { return "inproc"; }

private:
    struct inproc_hold
    {
        std::string owner;
        uint64_t version = 0;
    };

    std::mutex _mu;
    std::string _state_namespace;
    std::map<std::string, inproc_hold> _owners;
    std::map<std::string, uint64_t> _versions;
    std::map<std::string, std::string> _state;
};

#ifdef RASN_HAS_DIST_COORDINATION

// Wrap a std::string in an owning blob (copies the bytes).
::dsn::blob string_to_blob(const std::string &s)
{
    std::shared_ptr<char> buffer(new char[s.size() + 1], std::default_delete<char[]>());
    memcpy(buffer.get(), s.data(), s.size());
    buffer.get()[s.size()] = '\0';
    return ::dsn::blob(buffer, static_cast<unsigned int>(s.size()));
}

std::string blob_to_string(const ::dsn::blob &b)
{
    return b.length() == 0 ? std::string() : std::string(b.data(), b.length());
}

struct coordination_async_result
{
    std::atomic<int> error{static_cast<int>(::dsn::ERR_TIMEOUT.get())};
    std::string value;
    std::vector<std::string> children;
    uint64_t version = 0;
};

::dsn::error_code async_error(const std::shared_ptr<coordination_async_result> &result)
{
    return ::dsn::error_code(static_cast<dsn_error_t>(result->error.load()));
}

// ---------------------------------------------------------------------------
// Distributed backend reusing rDSN's distributed_lock_service (ownership /
// leader election) and meta_state_service (cluster-shared state). Provider is
// "simple" (rDSN in-process provider; coordinates within one facade instance
// only) or "zookeeper" (cross-process HA). The async rDSN APIs are driven
// synchronously here so the rASN facade stays blocking.
// ---------------------------------------------------------------------------
class dist_coordination_service : public rasn_coordination_service
{
public:
    explicit dist_coordination_service(const rasn_coordination_config &cfg) : _cfg(cfg)
    {
        _default_acquire_timeout_ms = cfg.acquire_timeout_ms;
    }

    ~dist_coordination_service() override { stop(); }

    ::dsn::error_code start() override
    {
        if (_operation_timed_out.load())
        {
            derror("rasn.coordination provider was quarantined after an unresolved "
                   "operation and cannot be restarted safely");
            return ::dsn::ERR_INVALID_STATE;
        }
        const bool zk = _cfg.provider == "zookeeper";
        if (zk) {
            const std::string hosts =
                ::dsn_config_get_value_string("zookeeper", "hosts_list", "", "");
            if (hosts.empty()) {
                derror("rasn.coordination provider=zookeeper requires [zookeeper] hosts_list "
                       "to be set");
                return ::dsn::ERR_INVALID_PARAMETERS;
            }
            _lock = new ::dsn::dist::distributed_lock_service_zookeeper();
            _state_svc = new ::dsn::dist::meta_state_service_zookeeper();
        } else {
            // "simple" backend: rDSN's in-process providers. Their lock table and
            // state tree live in per-instance members of the objects created here,
            // so coordination is scoped to THIS facade instance only -- a second
            // dist_coordination_service (another instance in this process, or any
            // other process) constructs its own providers and shares nothing.
            // Adequate for unit tests and single-writer dev; use "zookeeper" for
            // real cross-process / multi-app coordination.
            _lock = new ::dsn::dist::distributed_lock_service_simple();
            _state_svc = new ::dsn::dist::meta_state_service_simple();
        }

        std::vector<std::string> lock_args;
        if (zk)
            lock_args.push_back(_cfg.lock_namespace);
        ::dsn::error_code ec = _lock->initialize(lock_args);
        if (ec != ::dsn::ERR_OK) {
            derror("rasn.coordination lock service initialize failed: %s", ec.to_string());
            return ec;
        }
        _lock_initialized = true;

        std::vector<std::string> state_args;
        if (!zk) {
            // The "simple" meta-state backend keeps a durable log under a work
            // directory. When no arg is given it falls back to
            // dsn_get_app_data_dir(), which is null in hosts without an app data
            // dir (e.g. unit-test mimic hosts) and would crash. Always pass an
            // explicit, existing directory.
            std::string work_dir = _cfg.state_work_dir.empty() ? "." : _cfg.state_work_dir;
            if (!::dsn::utils::filesystem::directory_exists(work_dir) &&
                !::dsn::utils::filesystem::create_directory(work_dir)) {
                derror("rasn.coordination failed to create state_work_dir '%s'",
                       work_dir.c_str());
                return ::dsn::ERR_FILE_OPERATION_FAILED;
            }
            state_args.push_back(work_dir);
        }
        ec = _state_svc->initialize(state_args);
        if (ec != ::dsn::ERR_OK) {
            derror("rasn.coordination state service initialize failed: %s", ec.to_string());
            return ec;
        }
        _state_initialized = true;

        // Materialize the shared-state root before first use. Fail closed if the
        // namespace cannot be created: a missing root means every later put/get
        // would fail confusingly, so surface the error at start() instead.
        ::dsn::error_code path_ec = ensure_path(_cfg.state_namespace);
        if (path_ec != ::dsn::ERR_OK) {
            derror("rasn.coordination failed to create state namespace '%s': %s",
                   _cfg.state_namespace.c_str(), path_ec.to_string());
            return path_ec;
        }
        return ::dsn::ERR_OK;
    }

    void stop() override
    {
        std::map<std::string, hold> held;
        {
            std::lock_guard<std::mutex> guard(_ownership->mu);
            held.swap(_ownership->holds);
        }
        for (auto &kv : held) {
            if (_lock != nullptr) {
                auto result = std::make_shared<coordination_async_result>();
                ::dsn::task_ptr task =
                    _lock->unlock(kv.first,
                                  kv.second.owner,
                                  kv.second.destroy_on_stop,
                                  LPC_RASN_COORDINATION,
                                  [result](::dsn::error_code ec) {
                                      result->error.store(static_cast<int>(ec.get()));
                                  });
                wait_for_operation(task);
            }
            if (kv.second.lease_task != nullptr)
                cancel_and_wait(kv.second.lease_task);
        }
        if (_operation_timed_out.load()) {
            dwarn("rasn.coordination shutdown left a provider operation unresolved; "
                  "retaining provider objects to keep late callbacks memory-safe");
            _lock = nullptr;
            _state_svc = nullptr;
            _lock_initialized = false;
            _state_initialized = false;
            return;
        }
        if (_lock != nullptr) {
            if (_lock_initialized) {
                _lock->finalize();
                if (_cfg.provider != "zookeeper")
                    delete _lock;
            } else {
                delete _lock;
            }
            _lock = nullptr;
            _lock_initialized = false;
        }
        if (_state_svc != nullptr) {
            if (_state_initialized) {
                _state_svc->finalize();
                if (_cfg.provider != "zookeeper")
                    delete _state_svc;
            } else {
                delete _state_svc;
            }
            _state_svc = nullptr;
            _state_initialized = false;
        }
    }

    ::dsn::error_code acquire_ownership(const std::string &resource_id,
                                        const std::string &owner_id,
                                        int timeout_ms,
                                        uint64_t *fencing_token,
                                        const ownership_lost_callback &on_lost) override
    {
        {
            std::lock_guard<std::mutex> guard(_ownership->mu);
            auto it = _ownership->holds.find(resource_id);
            if (it != _ownership->holds.end()) {
                if (it->second.owner != owner_id)
                    return ::ERR_HOLD_BY_OTHERS;
                if (!it->second.usable)
                    return ::dsn::ERR_INVALID_STATE;
                if (fencing_token != nullptr)
                    *fencing_token = it->second.version;
                return ::dsn::ERR_OK; // idempotent re-acquire
            }
        }

        ::dsn::dist::distributed_lock_service::lock_options opt = {true, true};
        auto granted = std::make_shared<std::atomic<int>>(static_cast<int>(::dsn::ERR_TIMEOUT.get()));
        auto granted_version = std::make_shared<std::atomic<uint64_t>>(0);
        std::shared_ptr<ownership_registry> ownership = _ownership;
        auto cleanup_requested = std::make_shared<std::atomic<bool>>(false);
        auto cleanup_started = std::make_shared<std::atomic<bool>>(false);
        ::dsn::dist::distributed_lock_service *lock_provider = _lock;

        std::pair< ::dsn::task_ptr, ::dsn::task_ptr> tasks = _lock->lock(
            resource_id, owner_id, LPC_RASN_COORDINATION,
            [granted,
             granted_version,
             cleanup_requested,
             cleanup_started,
             lock_provider,
             ownership,
             resource_id,
             owner_id](::dsn::error_code ec,
                       const std::string &,
                       uint64_t version) {
                granted_version->store(version);
                granted->store(static_cast<int>(ec.get()));
                if (ec == ::dsn::ERR_OK && cleanup_requested->load())
                {
                    schedule_uncertain_grant_cleanup(lock_provider,
                                                     ownership,
                                                     resource_id,
                                                     owner_id,
                                                     cleanup_started);
                }
            },
            LPC_RASN_COORDINATION,
            [ownership, resource_id, owner_id, on_lost](::dsn::error_code ec,
                                                        const std::string &,
                                                        uint64_t version) {
                ec.end_tracking();
                on_lease_lost(ownership, resource_id, owner_id, version, on_lost);
            },
            opt);

        const bool completed = tasks.first->wait((std::max)(1, timeout_ms));
        if (!completed) {
            // Timed out while (apparently) still pending. Cancel the attempt, but
            // the grant may have won the race between our wait() deadline and the
            // cancel: rDSN's cancel_pending_lock reports ERR_OBJECT_NOT_FOUND and
            // the *current owner* when the caller is no longer in the pending list
            // because it already acquired the lock (distributed_lock_service.h).
            // If that owner is us -- or the grant callback already stored ERR_OK --
            // we actually hold the lock and must not leak it: record the hold (with
            // its live lease task) and return success, matching the documented
            // contract (ERR_OK == granted). Only a genuine timeout unwinds the lease.
            auto cancel = std::make_shared<coordination_async_result>();
            ::dsn::task_ptr cancel_task = _lock->cancel_pending_lock(
                resource_id,
                owner_id,
                LPC_RASN_COORDINATION,
                [cancel](::dsn::error_code ec,
                         const std::string &owner,
                         uint64_t version) {
                    cancel->error.store(static_cast<int>(ec.get()));
                    cancel->value = owner;
                    cancel->version = version;
                });
            if (!wait_for_operation(cancel_task)) {
                {
                    std::lock_guard<std::mutex> guard(_ownership->mu);
                    hold h;
                    h.owner = owner_id;
                    h.lease_task = tasks.second;
                    h.version = granted_version->load();
                    h.usable = false;
                    _ownership->holds[resource_id] = h;
                }
                cleanup_requested->store(true);
                if (::dsn::error_code(static_cast<dsn_error_t>(granted->load())) ==
                    ::dsn::ERR_OK)
                {
                    schedule_uncertain_grant_cleanup(_lock,
                                                     _ownership,
                                                     resource_id,
                                                     owner_id,
                                                     cleanup_started);
                }
                return ::dsn::ERR_TIMEOUT;
            }

            const bool grant_won =
                ::dsn::error_code(static_cast<dsn_error_t>(granted->load())) == ::dsn::ERR_OK;
            const bool cancel_saw_us =
                async_error(cancel) == ::dsn::ERR_OBJECT_NOT_FOUND &&
                cancel->value == owner_id;
            if (grant_won || cancel_saw_us) {
                std::lock_guard<std::mutex> guard(_ownership->mu);
                hold h;
                h.owner = owner_id;
                h.lease_task = tasks.second;
                h.version =
                    grant_won ? granted_version->load() : cancel->version;
                _ownership->holds[resource_id] = h;
                remember_latest_version(resource_id, h.version);
                if (fencing_token != nullptr)
                    *fencing_token = h.version;
                return ::dsn::ERR_OK;
            }

            if (tasks.second != nullptr)
                cancel_and_wait(tasks.second);
            cancel_and_wait(tasks.first);
            return ::dsn::ERR_TIMEOUT;
        }

        ::dsn::error_code ec(static_cast<dsn_error_t>(granted->load()));
        if (ec == ::dsn::ERR_OK) {
            std::lock_guard<std::mutex> guard(_ownership->mu);
            hold h;
            h.owner = owner_id;
            h.lease_task = tasks.second;
            h.version = granted_version->load();
            _ownership->holds[resource_id] = h;
            remember_latest_version(resource_id, h.version);
            if (fencing_token != nullptr)
                *fencing_token = h.version;
        } else if (tasks.second != nullptr) {
            cancel_and_wait(tasks.second);
        }
        return ec;
    }

    ::dsn::error_code release_ownership(const std::string &resource_id,
                                        const std::string &owner_id,
                                        bool destroy) override
    {
        ::dsn::task_ptr lease;
        uint64_t version = 0;
        {
            std::lock_guard<std::mutex> guard(_ownership->mu);
            auto it = _ownership->holds.find(resource_id);
            if (it == _ownership->holds.end() || it->second.owner != owner_id)
                return ::dsn::ERR_OK; // not held by us: idempotent
            lease = it->second.lease_task;
            version = it->second.version;
            // Once unlock starts, an ambiguous outcome must never be reused as
            // an idempotent acquisition with this now-potentially-stale fence.
            it->second.usable = false;
            it->second.destroy_on_stop = destroy;
        }

        auto result = std::make_shared<coordination_async_result>();
        ::dsn::task_ptr task =
            _lock->unlock(resource_id,
                          owner_id,
                          destroy,
                          LPC_RASN_COORDINATION,
                          [result](::dsn::error_code ec) {
                              result->error.store(static_cast<int>(ec.get()));
                          });
        if (!wait_for_operation(task))
            return ::dsn::ERR_TIMEOUT;
        const ::dsn::error_code out = async_error(result);
        const bool definitively_released =
            out == ::dsn::ERR_OK || out == ::ERR_HOLD_BY_OTHERS ||
            out == ::dsn::ERR_OBJECT_NOT_FOUND || out == ::ERR_NO_OWNER;
        if (definitively_released)
        {
            {
                std::lock_guard<std::mutex> guard(_ownership->mu);
                auto it = _ownership->holds.find(resource_id);
                if (it != _ownership->holds.end() && it->second.owner == owner_id &&
                    it->second.version == version)
                {
                    _ownership->holds.erase(it);
                }
                if (destroy && _cfg.provider != "zookeeper" &&
                    _ownership->holds.find(resource_id) == _ownership->holds.end())
                {
                    const auto latest = _ownership->latest_versions.find(resource_id);
                    if (latest != _ownership->latest_versions.end() &&
                        latest->second <= version)
                    {
                        _ownership->latest_versions.erase(latest);
                    }
                }
            }
            if (lease != nullptr)
                cancel_and_wait(lease);
            // Idempotent release treats every outcome that proves this owner no
            // longer holds the lock as success. Only uncertain outcomes escape.
            return ::dsn::ERR_OK;
        }
        return out;
    }

    ::dsn::error_code verify_ownership_fence(const std::string &resource_id,
                                             uint64_t fencing_token) override
    {
        if (_cfg.provider != "zookeeper")
        {
            std::lock_guard<std::mutex> guard(_ownership->mu);
            const auto it = _ownership->latest_versions.find(resource_id);
            return it == _ownership->latest_versions.end() ||
                           it->second <= fencing_token
                       ? ::dsn::ERR_OK
                       : ::dsn::ERR_INVALID_STATE;
        }

        // The provider has no public queued-grant query, so this barrier pins
        // its private <lock-root>/<lock-id>/LOCKNODE<sequence> layout. Because
        // rASN releases fenced locks non-destructively, the directory must still
        // exist; missing or malformed layout is a fail-closed provider mismatch.
        std::vector<std::string> children;
        const ::dsn::error_code listed =
            list_children_sync(join_path(_cfg.lock_namespace, resource_id), children);
        if (listed == ::dsn::ERR_OBJECT_NOT_FOUND ||
            listed == ::dsn::ERR_PATH_NOT_FOUND)
        {
            derror("rasn.coordination expected the ZooKeeper lock directory for '%s' "
                   "to survive non-destructive release; refusing to weaken the "
                   "fencing barrier",
                   resource_id.c_str());
            return ::dsn::ERR_INVALID_STATE;
        }
        if (listed != ::dsn::ERR_OK)
            return listed;
        if (children.empty())
            return ::dsn::ERR_OK;

        for (const std::string &child : children)
        {
            uint64_t version = 0;
            if (!parse_lock_version(child, &version))
            {
                derror("rasn.coordination expected ZooKeeper lock child '%s' to use "
                       "the provider's LOCKNODE<sequence> layout; refusing to weaken "
                       "the fencing barrier",
                       child.c_str());
                return ::dsn::ERR_INVALID_STATE;
            }
            if (version <= fencing_token)
                return ::dsn::ERR_INVALID_STATE;
        }
        // This method is called after release. Any remaining lock node means a
        // peer was already queued or active, so the completed transition cannot
        // safely expose a side effect based only on its older fence.
        return ::ERR_HOLD_BY_OTHERS;
    }

    bool query_owner(const std::string &resource_id,
                     std::string &owner_id,
                     uint64_t *fencing_token) override
    {
        uint64_t version = 0;
        ::dsn::error_code ec = _lock->query_cache(resource_id, owner_id, version);
        if (ec == ::dsn::ERR_OK && fencing_token != nullptr)
            *fencing_token = version;
        return ec == ::dsn::ERR_OK && !owner_id.empty();
    }

    ::dsn::error_code put_state(const std::string &key, const std::string &value) override
    {
        const std::string node = join_path(_cfg.state_namespace, key);
        ::dsn::error_code ec = ensure_path(parent_path(node));
        if (ec != ::dsn::ERR_OK)
            return ec;

        bool exists = false;
        ec = node_exist_sync(node, exists);
        if (ec != ::dsn::ERR_OK)
            return ec;
        if (exists)
            return set_data_sync(node, value);
        // node_exist-then-create is not atomic: a concurrent writer may create the
        // node in between. create_node then returns ERR_NODE_ALREADY_EXIST; fall
        // back to set_data so our value still lands (last-writer-wins), exactly as
        // the exists branch above would have done had we observed it first.
        ec = create_node_sync(node, value);
        if (ec == ::ERR_NODE_ALREADY_EXIST)
            return set_data_sync(node, value);
        return ec;
    }

    ::dsn::error_code get_state(const std::string &key, std::string &value) override
    {
        return get_data_sync(join_path(_cfg.state_namespace, key), value);
    }

    ::dsn::error_code delete_state(const std::string &key) override
    {
        auto result = std::make_shared<coordination_async_result>();
        ::dsn::task_ptr task = _state_svc->delete_node(
            join_path(_cfg.state_namespace, key),
            true,
            LPC_RASN_COORDINATION,
            [result](::dsn::error_code ec) {
                result->error.store(static_cast<int>(ec.get()));
            });
        if (!wait_for_operation(task))
            return ::dsn::ERR_TIMEOUT;
        const ::dsn::error_code out = async_error(result);
        if (out == ::dsn::ERR_OBJECT_NOT_FOUND || out == ::dsn::ERR_PATH_NOT_FOUND)
            return ::dsn::ERR_OK;
        return out;
    }

    ::dsn::error_code list_state(const std::string &key,
                                 std::vector<std::string> &children) override
    {
        return list_children_sync(join_path(_cfg.state_namespace, key), children);
    }

    bool is_distributed() const override { return true; }
    const char *provider_name() const override
    {
        return _cfg.provider == "zookeeper" ? "zookeeper" : "simple";
    }

private:
    struct hold
    {
        std::string owner;
        ::dsn::task_ptr lease_task;
        uint64_t version = 0;
        bool usable = true;
        bool destroy_on_stop = true;
    };

    struct ownership_registry
    {
        std::mutex mu;
        std::map<std::string, hold> holds;
        // The simple provider has no public post-release grant query, so retain
        // each resource's latest fence for delayed barrier checks. ZooKeeper
        // verifies live sequential children instead and never populates this map.
        std::map<std::string, uint64_t> latest_versions;
    };

    void remember_latest_version(const std::string &resource_id, uint64_t version)
    {
        if (_cfg.provider == "zookeeper")
        {
            return;
        }
        _ownership->latest_versions[resource_id] =
            (std::max)(_ownership->latest_versions[resource_id], version);
    }

    bool wait_for_operation(const ::dsn::task_ptr &task)
    {
        const int timeout_ms = (std::max)(1, _cfg.operation_timeout_ms);
        if (task != nullptr && task->wait(timeout_ms))
            return true;
        _operation_timed_out.store(true);
        if (task != nullptr)
            task->cancel(false);
        return false;
    }

    void cancel_and_wait(const ::dsn::task_ptr &task)
    {
        if (task == nullptr)
            return;
        task->cancel(false);
        wait_for_operation(task);
    }

    static void schedule_uncertain_grant_cleanup(
        ::dsn::dist::distributed_lock_service *lock_provider,
        const std::shared_ptr<ownership_registry> &ownership,
        const std::string &resource_id,
        const std::string &owner_id,
        const std::shared_ptr<std::atomic<bool>> &cleanup_started)
    {
        if (cleanup_started->exchange(true))
            return;
        lock_provider->unlock(
            resource_id,
            owner_id,
            false,
            LPC_RASN_COORDINATION,
            [ownership, resource_id, owner_id](::dsn::error_code ec) {
                const bool released =
                    ec == ::dsn::ERR_OK || ec == ::ERR_HOLD_BY_OTHERS ||
                    ec == ::dsn::ERR_OBJECT_NOT_FOUND || ec == ::ERR_NO_OWNER;
                if (!released)
                {
                    dwarn("rasn.coordination could not clean up an ambiguously granted "
                          "lock '%s' for owner '%s': %s",
                          resource_id.c_str(),
                          owner_id.c_str(),
                          ec.to_string());
                    return;
                }
                std::lock_guard<std::mutex> guard(ownership->mu);
                auto it = ownership->holds.find(resource_id);
                if (it != ownership->holds.end() &&
                    it->second.owner == owner_id && !it->second.usable)
                {
                    if (it->second.lease_task != nullptr)
                        it->second.lease_task->cancel(false);
                    ownership->holds.erase(it);
                }
            });
    }

    ::dsn::error_code list_children_sync(
        const std::string &node, std::vector<std::string> &children)
    {
        auto result = std::make_shared<coordination_async_result>();
        ::dsn::task_ptr task = _state_svc->get_children(
            node, LPC_RASN_COORDINATION,
            [result](::dsn::error_code ec, const std::vector<std::string> &ret) {
                result->error.store(static_cast<int>(ec.get()));
                if (ec == ::dsn::ERR_OK)
                    result->children = ret;
            });
        if (!wait_for_operation(task))
            return ::dsn::ERR_TIMEOUT;
        const ::dsn::error_code out = async_error(result);
        if (out == ::dsn::ERR_OK)
            children = result->children;
        return out;
    }

    static void on_lease_lost(const std::shared_ptr<ownership_registry> &ownership,
                              const std::string &resource_id,
                              const std::string &owner_id,
                              uint64_t version,
                              const ownership_lost_callback &callback)
    {
        bool removed = false;
        {
            std::lock_guard<std::mutex> guard(ownership->mu);
            auto it = ownership->holds.find(resource_id);
            if (it != ownership->holds.end() && it->second.owner == owner_id &&
                it->second.version == version)
            {
                ownership->holds.erase(it);
                removed = true;
            }
        }
        if (removed && callback)
            callback(resource_id, version);
    }

    static std::string parent_path(const std::string &node)
    {
        const size_t slash = node.find_last_of('/');
        if (slash == std::string::npos || slash == 0)
            return "/";
        return node.substr(0, slash);
    }

    static bool parse_lock_version(const std::string &node, uint64_t *version)
    {
        // distributed_lock_service_zookeeper currently creates private
        // ephemeral-sequential children named LOCKNODE<sequence>. Its public
        // API exposes no equivalent queued-grant fence, so pin that internal
        // layout explicitly and fail closed if the provider ever changes it.
        static const char kLockNodePrefix[] = "LOCKNODE";
        const size_t prefix_length = sizeof(kLockNodePrefix) - 1;
        if (node.size() <= prefix_length ||
            node.compare(0, prefix_length, kLockNodePrefix) != 0)
        {
            return false;
        }
        uint64_t parsed = 0;
        for (size_t i = prefix_length; i < node.size(); ++i)
        {
            if (node[i] < '0' || node[i] > '9' ||
                parsed > ((std::numeric_limits<uint64_t>::max)() -
                          static_cast<uint64_t>(node[i] - '0')) /
                             10)
            {
                return false;
            }
            parsed = parsed * 10 + static_cast<uint64_t>(node[i] - '0');
        }
        *version = parsed;
        return true;
    }

    ::dsn::error_code create_node_sync(const std::string &node, const std::string &value)
    {
        auto result = std::make_shared<coordination_async_result>();
        ::dsn::task_ptr task = _state_svc->create_node(
            node,
            LPC_RASN_COORDINATION,
            [result](::dsn::error_code ec) {
                result->error.store(static_cast<int>(ec.get()));
            },
            string_to_blob(value),
            nullptr);
        return wait_for_operation(task) ? async_error(result) : ::dsn::ERR_TIMEOUT;
    }

    ::dsn::error_code set_data_sync(const std::string &node, const std::string &value)
    {
        auto result = std::make_shared<coordination_async_result>();
        ::dsn::task_ptr task = _state_svc->set_data(
            node,
            string_to_blob(value),
            LPC_RASN_COORDINATION,
            [result](::dsn::error_code ec) {
                result->error.store(static_cast<int>(ec.get()));
            },
            nullptr);
        return wait_for_operation(task) ? async_error(result) : ::dsn::ERR_TIMEOUT;
    }

    ::dsn::error_code get_data_sync(const std::string &node, std::string &value)
    {
        auto result = std::make_shared<coordination_async_result>();
        ::dsn::task_ptr task = _state_svc->get_data(
            node,
            LPC_RASN_COORDINATION,
            [result](::dsn::error_code ec, const ::dsn::blob &val) {
                result->error.store(static_cast<int>(ec.get()));
                if (ec == ::dsn::ERR_OK)
                    result->value = blob_to_string(val);
            },
            nullptr);
        if (!wait_for_operation(task))
            return ::dsn::ERR_TIMEOUT;
        const ::dsn::error_code out = async_error(result);
        if (out == ::dsn::ERR_OK)
            value = result->value;
        return out;
    }

    ::dsn::error_code node_exist_sync(const std::string &node, bool &exists)
    {
        auto result = std::make_shared<coordination_async_result>();
        ::dsn::task_ptr task = _state_svc->node_exist(
            node,
            LPC_RASN_COORDINATION,
            [result](::dsn::error_code ec) {
                result->error.store(static_cast<int>(ec.get()));
            },
            nullptr);
        if (!wait_for_operation(task))
            return ::dsn::ERR_TIMEOUT;
        const ::dsn::error_code out = async_error(result);
        if (out == ::dsn::ERR_OK) {
            exists = true;
            return ::dsn::ERR_OK;
        }
        if (out == ::dsn::ERR_OBJECT_NOT_FOUND || out == ::dsn::ERR_PATH_NOT_FOUND) {
            exists = false;
            return ::dsn::ERR_OK;
        }
        return out;
    }

    // Create every ancestor node along an absolute path, ignoring nodes that
    // already exist. Used to materialize the shared-state namespace.
    ::dsn::error_code ensure_path(const std::string &path)
    {
        if (path.empty() || path == "/")
            return ::dsn::ERR_OK;
        size_t pos = (path[0] == '/') ? 1 : 0;
        while (pos <= path.size()) {
            const size_t slash = path.find('/', pos);
            const std::string prefix =
                (slash == std::string::npos) ? path : path.substr(0, slash);
            if (!prefix.empty()) {
                bool exists = false;
                ::dsn::error_code ec = node_exist_sync(prefix, exists);
                if (ec != ::dsn::ERR_OK)
                    return ec;
                if (!exists) {
                    ::dsn::error_code cec = create_node_sync(prefix, std::string());
                    // Tolerate only a concurrent creation of the same ancestor
                    // (ERR_NODE_ALREADY_EXIST). Any other failure is real -- e.g. the
                    // backend is unreachable -- and must abort path materialization
                    // rather than being swallowed, so callers don't proceed against a
                    // parent path that was never created.
                    if (cec != ::dsn::ERR_OK && cec != ::ERR_NODE_ALREADY_EXIST)
                        return cec;
                }
            }
            if (slash == std::string::npos)
                break;
            pos = slash + 1;
        }
        return ::dsn::ERR_OK;
    }

    rasn_coordination_config _cfg;
    std::shared_ptr<ownership_registry> _ownership =
        std::make_shared<ownership_registry>();
    std::atomic<bool> _operation_timed_out{false};
    ::dsn::dist::distributed_lock_service *_lock = nullptr;
    ::dsn::dist::meta_state_service *_state_svc = nullptr;
    bool _lock_initialized = false;
    bool _state_initialized = false;
};

#endif // RASN_HAS_DIST_COORDINATION

} // namespace

std::unique_ptr<rasn_coordination_service>
create_rasn_coordination_service(const rasn_coordination_config &cfg)
{
#ifdef RASN_HAS_DIST_COORDINATION
    if (cfg.provider == "simple" || cfg.provider == "zookeeper")
        return std::unique_ptr<rasn_coordination_service>(new dist_coordination_service(cfg));
#else
    if (cfg.provider == "simple" || cfg.provider == "zookeeper") {
        dwarn("rasn.coordination provider '%s' requested but the dist.service plugin is not "
              "compiled in; falling back to in-process coordination",
              cfg.provider.c_str());
    }
#endif
    return std::unique_ptr<rasn_coordination_service>(new inproc_coordination_service(cfg));
}

rasn_coordination_context::rasn_coordination_context(const rasn_coordination_config &cfg)
    : _service(create_rasn_coordination_service(cfg))
{
}

rasn_coordination_context::~rasn_coordination_context()
{
    std::lock_guard<std::mutex> guard(_lock);
    if (_service != nullptr && _started)
    {
        _service->stop();
        _started = false;
    }
}

::dsn::error_code rasn_coordination_context::start()
{
    std::lock_guard<std::mutex> guard(_lock);
    if (_started)
    {
        return ::dsn::ERR_OK;
    }
    if (_service == nullptr)
    {
        return ::dsn::ERR_UNKNOWN;
    }
    const ::dsn::error_code err = _service->start();
    if (err != ::dsn::ERR_OK)
    {
        _service->stop();
        return err;
    }
    _started = true;
    return ::dsn::ERR_OK;
}

const char *rasn_coordination_context::provider_name() const
{
    return _service == nullptr ? "unavailable" : _service->provider_name();
}

std::shared_ptr<rasn_coordination_context> shared_rasn_coordination_context()
{
    static std::mutex lock;
    // Providers schedule callbacks on the hosting app's pools, so retain one
    // context per rDSN app identity instead of sharing a process-global provider.
    static std::map<std::string, std::shared_ptr<rasn_coordination_context>> contexts;
    dsn_app_info app_info;
    std::ostringstream key;
    if (::dsn_get_current_app_info(&app_info))
    {
        key << app_info.app.app_context_ptr << ":" << app_info.app_id << ":"
            << app_info.role << ":" << app_info.index;
    }
    else
    {
        key << "unbound";
    }
    std::lock_guard<std::mutex> guard(lock);
    std::shared_ptr<rasn_coordination_context> &context = contexts[key.str()];
    if (context == nullptr)
    {
        context.reset(new rasn_coordination_context(load_rasn_coordination_config()));
    }
    return context;
}

} // namespace rasn
} // namespace dsn
