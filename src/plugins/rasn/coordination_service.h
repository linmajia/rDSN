#pragma once

// rASN distributed coordination facade.
//
// Provides two production primitives that rASN's distributed runtime modules
// need but previously lacked (see docs/DISTRIBUTED_RUNTIME.md findings 1.1 and
// 1.5): single-writer ownership / leader election, and cluster-shared state.
//
// Rather than reinventing consensus, this facade reuses rDSN's existing
// distributed facilities from the rDSN.dist.service submodule:
//   * distributed_lock_service  -> single-writer ownership / leader election
//   * meta_state_service        -> hierarchical cluster-shared state (registry)
//
// A backend is selected by configuration ([rasn.coordination] provider):
//   * "inproc"    : single-node in-memory fallback (always available). Keeps
//                   the app self-contained for local/dev runs and for builds
//                   that do not include the dist.service plugin.
//   * "simple"    : rDSN in-process provider. Coordinates only *within a single
//                   coordination-service instance in one process* -- the provider
//                   keeps its lock/state maps in per-instance members, so two
//                   facade instances (or two processes) do NOT see each other's
//                   locks or state. Use it for unit tests and single-writer dev,
//                   where exactly one facade instance is the coordinator. For
//                   real cross-process / multi-app coordination on one box or many,
//                   use "zookeeper".
//   * "zookeeper" : rDSN ZooKeeper-backed provider (production, HA). The only
//                   backend that coordinates across independent processes/apps.
//
// The "simple"/"zookeeper" backends are only compiled when the dist.service
// plugin is present in the build (RASN_HAS_DIST_COORDINATION). When it is not,
// the factory transparently falls back to the in-process backend so that rASN
// still builds and runs as a single node.

#include <dsn/service_api_cpp.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

// Backend selection + connection parameters, normally loaded from
// [rasn.coordination] in the rDSN config.
struct rasn_coordination_config
{
    // "inproc" | "simple" | "zookeeper"
    std::string provider = "inproc";
    // Root znode-style path for ownership locks, e.g. "/rasn/locks".
    std::string lock_namespace = "/rasn/locks";
    // Root znode-style path for shared state, e.g. "/rasn/state".
    std::string state_namespace = "/rasn/state";
    // Lock acquisition timeout used by the blocking acquire_ownership() helper.
    int acquire_timeout_ms = 5000;
    // Timeout for every other blocking provider operation, including state I/O,
    // cancellation, unlock, and shutdown cleanup.
    int operation_timeout_ms = 5000;
    // Filesystem directory used by the "simple" meta-state backend for its
    // durable log. Ignored by the "inproc" and "zookeeper" backends. Defaults
    // to the current working directory so it is never null (the underlying
    // provider otherwise falls back to dsn_get_app_data_dir(), which is null in
    // hosts that do not configure an app data dir, e.g. unit-test mimic hosts).
    std::string state_work_dir = ".";
};

// Load [rasn.coordination] into a config struct (with the defaults above).
rasn_coordination_config load_rasn_coordination_config();

// Abstract coordination facade. All methods are synchronous: the dist-backed
// implementation drives rDSN's async APIs internally and blocks the caller.
//
// Threading contract: the blocking methods must NOT be invoked from a
// THREAD_POOL_META_SERVER worker, because the dist backend delivers its completion
// callbacks on that pool. rASN's runtime never processes requests on
// THREAD_POOL_META_SERVER, so ordinary rASN call sites are safe. Every rASN app that
// may run the simple|zookeeper backend must declare THREAD_POOL_META_SERVER in
// config; a zookeeper-backed app must ALSO declare THREAD_POOL_DLOCK
// (partitioned = true), the pool the zookeeper lock provider runs its own lock/lease
// tasks on. See config.rasn.ini and DISTRIBUTED_RUNTIME.md for the full per-backend
// pool wiring.
class rasn_coordination_service
{
public:
    using ownership_lost_callback =
        std::function<void(const std::string &resource_id, uint64_t fencing_token)>;

    virtual ~rasn_coordination_service() {}

    // Connect / initialize the backend. Must be called before any other method.
    virtual ::dsn::error_code start() = 0;
    // Release backend resources. Safe to call multiple times.
    virtual void stop() = 0;

    // --- Single-writer ownership / leader election -----------------------
    //
    // Blocks until ownership of resource_id is granted to owner_id, or until
    // timeout_ms elapses. Returns ERR_OK on grant, ERR_TIMEOUT if another owner
    // holds it past the deadline, or a backend error. Re-acquiring a resource
    // already owned by owner_id is idempotent and returns ERR_OK. On success,
    // fencing_token receives the backend's grant version; preserve the lock object
    // on release when that token must remain monotonic across future owners.
    virtual ::dsn::error_code acquire_ownership(const std::string &resource_id,
                                                const std::string &owner_id,
                                                int timeout_ms,
                                                /*out*/ uint64_t *fencing_token,
                                                const ownership_lost_callback &on_lost) = 0;

    ::dsn::error_code
    acquire_ownership(const std::string &resource_id, const std::string &owner_id, int timeout_ms);
    ::dsn::error_code acquire_ownership(const std::string &resource_id,
                                        const std::string &owner_id,
                                        int timeout_ms,
                                        /*out*/ uint64_t *fencing_token);
    ::dsn::error_code acquire_ownership(const std::string &resource_id,
                                        const std::string &owner_id,
                                        const ownership_lost_callback &on_lost);

    // Convenience overload using the configured acquire_timeout_ms.
    ::dsn::error_code acquire_ownership(const std::string &resource_id, const std::string &owner_id);

    // Release ownership previously granted to owner_id. Returns ERR_OK even if
    // owner_id was not the holder (idempotent release).
    // destroy=false preserves the lock object and its monotonically increasing
    // grant version for fencing. Runtime ownership uses the default destroy=true;
    // coordinated read/modify/write adapters use false.
    virtual ::dsn::error_code release_ownership(const std::string &resource_id,
                                                const std::string &owner_id,
                                                bool destroy) = 0;
    ::dsn::error_code
    release_ownership(const std::string &resource_id, const std::string &owner_id);

    // Post-release fencing barrier. ERR_OK means no newer grant was already
    // active when the barrier was observed. A caller that persists a fenced
    // transition must pass this before exposing the transition's side effect.
    virtual ::dsn::error_code verify_ownership_fence(const std::string &resource_id,
                                                     uint64_t fencing_token) = 0;

    // Non-blocking best-effort query of the current owner. Returns true and sets
    // owner_id if an owner is known, false otherwise.
    virtual bool query_owner(const std::string &resource_id,
                             /*out*/ std::string &owner_id,
                             /*out*/ uint64_t *fencing_token) = 0;
    bool query_owner(const std::string &resource_id, /*out*/ std::string &owner_id);

    // --- Cluster-shared state (HA registry / global state) ---------------
    //
    // Keys are relative to state_namespace; slashes create hierarchy.

    // Create or overwrite key with value.
    virtual ::dsn::error_code put_state(const std::string &key, const std::string &value) = 0;
    // Read key into value. Returns ERR_OBJECT_NOT_FOUND if absent.
    virtual ::dsn::error_code get_state(const std::string &key, /*out*/ std::string &value) = 0;
    // Delete key (and its children). Absent key returns ERR_OK.
    virtual ::dsn::error_code delete_state(const std::string &key) = 0;
    // List immediate child names under key.
    virtual ::dsn::error_code
    list_state(const std::string &key, /*out*/ std::vector<std::string> &children) = 0;

    // True when backed by a real distributed provider (simple/zookeeper),
    // false for the in-process single-node fallback.
    virtual bool is_distributed() const = 0;

    // Human-readable backend name ("inproc" | "simple" | "zookeeper").
    virtual const char *provider_name() const = 0;

protected:
    // Timeout used by the 2-arg acquire_ownership() convenience overload; each
    // implementation seeds it from rasn_coordination_config::acquire_timeout_ms.
    int _default_acquire_timeout_ms = 5000;
};

// Build a coordination service for the given config. When provider selects a
// distributed backend that is not compiled into this build, the factory logs a
// warning and returns the in-process fallback so callers still get a usable,
// single-node service.
std::unique_ptr<rasn_coordination_service>
create_rasn_coordination_service(const rasn_coordination_config &cfg);

// Per-rDSN-app lifecycle wrapper. ZooKeeper callbacks are bound to the app that
// initializes the provider, so a context must never be used from a different app
// role/index in the same process.
class rasn_coordination_context
{
public:
    explicit rasn_coordination_context(const rasn_coordination_config &cfg);
    ~rasn_coordination_context();

    ::dsn::error_code start();
    rasn_coordination_service *service() const { return _service.get(); }
    const char *provider_name() const;

private:
    mutable std::mutex _lock;
    std::unique_ptr<rasn_coordination_service> _service;
    bool _started = false;
};

// Returns the current rDSN app's context, creating it from
// load_rasn_coordination_config() when needed. Contexts are keyed by app identity
// to preserve the ZooKeeper provider's app/thread affinity.
std::shared_ptr<rasn_coordination_context> shared_rasn_coordination_context();

} // namespace rasn
} // namespace dsn
