#include <rasn/coordination_service.h>

#include <dsn/service_api_cpp.h>
#include <dsn/cpp/utils.h>

#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
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
    cfg.state_work_dir = ::dsn_config_get_value_string(
        "rasn.coordination", "state_work_dir", cfg.state_work_dir.c_str(),
        "directory for the 'simple' meta-state backend durable log");
    return cfg;
}

::dsn::error_code rasn_coordination_service::acquire_ownership(const std::string &resource_id,
                                                              const std::string &owner_id)
{
    return acquire_ownership(resource_id, owner_id, _default_acquire_timeout_ms);
}

#ifdef RASN_HAS_DIST_COORDINATION
// Coordination callbacks are delivered on THREAD_POOL_META_SERVER. This is the
// pool the rDSN dist providers themselves require: distributed_lock_service_simple
// (and the zookeeper provider) enqueue their own internal work -- notably the
// LPC_DIST_LOCK_SVC_RANDOM_EXPIRE lease timer -- on THREAD_POOL_META_SERVER, so any
// app running the simple/zookeeper backend must declare that pool regardless. rASN
// never processes requests on it (rASN handlers run on THREAD_POOL_DEFAULT /
// THREAD_POOL_RASN_WORKFLOW), so the blocking facade calls below wait() for their
// completion callbacks on a pool distinct from the caller's and cannot self-deadlock.
// The default 'inproc' backend uses no dist provider and never enqueues
// LPC_RASN_COORDINATION, so THREAD_POOL_META_SERVER is declared in the rASN
// runtime deployment config (the single shared src/plugins/rasn/config.rasn.ini)
// only when provider = simple|zookeeper.
// Declaring it under the default config would fail config parsing ("invalid enum
// configuration") because the providers that register the pool are not loaded.
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
                                        int /*timeout_ms*/) override
    {
        std::lock_guard<std::mutex> guard(_mu);
        auto it = _owners.find(resource_id);
        if (it == _owners.end() || it->second == owner_id) {
            _owners[resource_id] = owner_id;
            return ::dsn::ERR_OK;
        }
        // Held by a different owner within this process.
        return ::dsn::ERR_TIMEOUT;
    }

    ::dsn::error_code release_ownership(const std::string &resource_id,
                                        const std::string &owner_id) override
    {
        std::lock_guard<std::mutex> guard(_mu);
        auto it = _owners.find(resource_id);
        if (it != _owners.end() && it->second == owner_id)
            _owners.erase(it);
        return ::dsn::ERR_OK;
    }

    bool query_owner(const std::string &resource_id, std::string &owner_id) override
    {
        std::lock_guard<std::mutex> guard(_mu);
        auto it = _owners.find(resource_id);
        if (it == _owners.end())
            return false;
        owner_id = it->second;
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
    std::mutex _mu;
    std::string _state_namespace;
    std::map<std::string, std::string> _owners;
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
            std::lock_guard<std::mutex> guard(_mu);
            held.swap(_holds);
        }
        for (auto &kv : held) {
            if (_lock != nullptr) {
                _lock->unlock(kv.first, kv.second.owner, true, LPC_RASN_COORDINATION,
                              [](::dsn::error_code ec) { ec.end_tracking(); })
                    ->wait();
            }
            if (kv.second.lease_task != nullptr)
                kv.second.lease_task->cancel(false);
        }
        if (_lock != nullptr) {
            _lock->finalize();
            delete _lock;
            _lock = nullptr;
        }
        if (_state_svc != nullptr) {
            _state_svc->finalize();
            delete _state_svc;
            _state_svc = nullptr;
        }
    }

    ::dsn::error_code acquire_ownership(const std::string &resource_id,
                                        const std::string &owner_id,
                                        int timeout_ms) override
    {
        {
            std::lock_guard<std::mutex> guard(_mu);
            auto it = _holds.find(resource_id);
            if (it != _holds.end() && it->second.owner == owner_id)
                return ::dsn::ERR_OK; // idempotent re-acquire
        }

        ::dsn::dist::distributed_lock_service::lock_options opt = {true, true};
        auto granted = std::make_shared<std::atomic<int>>(static_cast<int>(::dsn::ERR_TIMEOUT.get()));

        std::pair<::dsn::task_ptr, ::dsn::task_ptr> tasks = _lock->lock(
            resource_id, owner_id, LPC_RASN_COORDINATION,
            [granted](::dsn::error_code ec, const std::string &, uint64_t) {
                granted->store(static_cast<int>(ec.get()));
            },
            LPC_RASN_COORDINATION,
            [this, resource_id](::dsn::error_code ec, const std::string &, uint64_t) {
                ec.end_tracking();
                on_lease_lost(resource_id);
            },
            opt);

        const bool completed = tasks.first->wait(timeout_ms);
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
            auto cancel_ec = std::make_shared<std::atomic<int>>(
                static_cast<int>(::dsn::ERR_TIMEOUT.get()));
            auto cancel_owner_ptr = std::make_shared<std::string>();
            _lock->cancel_pending_lock(
                      resource_id, owner_id, LPC_RASN_COORDINATION,
                      [cancel_ec, cancel_owner_ptr](::dsn::error_code ec,
                                                    const std::string &owner, uint64_t) {
                          cancel_ec->store(static_cast<int>(ec.get()));
                          *cancel_owner_ptr = owner;
                      })
                ->wait();

            const bool grant_won =
                ::dsn::error_code(static_cast<dsn_error_t>(granted->load())) == ::dsn::ERR_OK;
            const bool cancel_saw_us =
                ::dsn::error_code(static_cast<dsn_error_t>(cancel_ec->load())) ==
                    ::dsn::ERR_OBJECT_NOT_FOUND &&
                *cancel_owner_ptr == owner_id;
            if (grant_won || cancel_saw_us) {
                std::lock_guard<std::mutex> guard(_mu);
                hold h;
                h.owner = owner_id;
                h.lease_task = tasks.second;
                _holds[resource_id] = h;
                return ::dsn::ERR_OK;
            }

            if (tasks.second != nullptr)
                tasks.second->cancel(false);
            return ::dsn::ERR_TIMEOUT;
        }

        ::dsn::error_code ec(static_cast<dsn_error_t>(granted->load()));
        if (ec == ::dsn::ERR_OK) {
            std::lock_guard<std::mutex> guard(_mu);
            hold h;
            h.owner = owner_id;
            h.lease_task = tasks.second;
            _holds[resource_id] = h;
        } else if (tasks.second != nullptr) {
            tasks.second->cancel(false);
        }
        return ec;
    }

    ::dsn::error_code release_ownership(const std::string &resource_id,
                                        const std::string &owner_id) override
    {
        ::dsn::task_ptr lease;
        {
            std::lock_guard<std::mutex> guard(_mu);
            auto it = _holds.find(resource_id);
            if (it == _holds.end() || it->second.owner != owner_id)
                return ::dsn::ERR_OK; // not held by us: idempotent
            lease = it->second.lease_task;
            _holds.erase(it);
        }

        ::dsn::error_code out(::dsn::ERR_TIMEOUT.get());
        _lock->unlock(resource_id, owner_id, true, LPC_RASN_COORDINATION,
                      [&out](::dsn::error_code ec) { out = ec; })
            ->wait();
        if (lease != nullptr)
            lease->cancel(false);
        return out;
    }

    bool query_owner(const std::string &resource_id, std::string &owner_id) override
    {
        uint64_t version = 0;
        ::dsn::error_code ec = _lock->query_cache(resource_id, owner_id, version);
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
        ::dsn::error_code out(::dsn::ERR_TIMEOUT.get());
        _state_svc->delete_node(join_path(_cfg.state_namespace, key), true, LPC_RASN_COORDINATION,
                                [&out](::dsn::error_code ec) { out = ec; })
            ->wait();
        if (out == ::dsn::ERR_OBJECT_NOT_FOUND || out == ::dsn::ERR_PATH_NOT_FOUND)
            return ::dsn::ERR_OK;
        return out;
    }

    ::dsn::error_code list_state(const std::string &key,
                                 std::vector<std::string> &children) override
    {
        ::dsn::error_code out(::dsn::ERR_TIMEOUT.get());
        _state_svc->get_children(
            join_path(_cfg.state_namespace, key), LPC_RASN_COORDINATION,
            [&out, &children](::dsn::error_code ec, const std::vector<std::string> &ret) {
                out = ec;
                if (ec == ::dsn::ERR_OK)
                    children = ret;
            })
            ->wait();
        return out;
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
    };

    void on_lease_lost(const std::string &resource_id)
    {
        std::lock_guard<std::mutex> guard(_mu);
        _holds.erase(resource_id);
    }

    static std::string parent_path(const std::string &node)
    {
        const size_t slash = node.find_last_of('/');
        if (slash == std::string::npos || slash == 0)
            return "/";
        return node.substr(0, slash);
    }

    ::dsn::error_code create_node_sync(const std::string &node, const std::string &value)
    {
        ::dsn::error_code out(::dsn::ERR_TIMEOUT.get());
        _state_svc->create_node(node, LPC_RASN_COORDINATION,
                                [&out](::dsn::error_code ec) { out = ec; },
                                string_to_blob(value), nullptr)
            ->wait();
        return out;
    }

    ::dsn::error_code set_data_sync(const std::string &node, const std::string &value)
    {
        ::dsn::error_code out(::dsn::ERR_TIMEOUT.get());
        _state_svc->set_data(node, string_to_blob(value), LPC_RASN_COORDINATION,
                             [&out](::dsn::error_code ec) { out = ec; }, nullptr)
            ->wait();
        return out;
    }

    ::dsn::error_code get_data_sync(const std::string &node, std::string &value)
    {
        ::dsn::error_code out(::dsn::ERR_TIMEOUT.get());
        _state_svc->get_data(node, LPC_RASN_COORDINATION,
                             [&out, &value](::dsn::error_code ec, const ::dsn::blob &val) {
                                 out = ec;
                                 if (ec == ::dsn::ERR_OK)
                                     value = blob_to_string(val);
                             },
                             nullptr)
            ->wait();
        return out;
    }

    ::dsn::error_code node_exist_sync(const std::string &node, bool &exists)
    {
        ::dsn::error_code out(::dsn::ERR_TIMEOUT.get());
        _state_svc->node_exist(node, LPC_RASN_COORDINATION,
                               [&out](::dsn::error_code ec) { out = ec; }, nullptr)
            ->wait();
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
    std::mutex _mu;
    std::map<std::string, hold> _holds;
    ::dsn::dist::distributed_lock_service *_lock = nullptr;
    ::dsn::dist::meta_state_service *_state_svc = nullptr;
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

} // namespace rasn
} // namespace dsn
