// Unit tests for the rASN distributed coordination facade
// (rasn/coordination_service.h). The in-process fallback is always exercised.
// When the rDSN.dist.service plugin is compiled in (RASN_HAS_DIST_COORDINATION),
// the "simple" distributed provider is exercised too, proving rASN reuses
// rDSN's distributed_lock_service / meta_state_service for ownership election
// and cluster-shared state without a ZooKeeper cluster.
#include <gtest/gtest.h>

#include <rasn/coordination_breaker.h>
#include <rasn/coordination_service.h>
#include <dsn/cpp/utils.h>

#include <atomic>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace dsn;
using namespace dsn::rasn;

namespace {

// Unique state namespace per test so the durable "simple" meta-state provider
// (which replays a shared on-disk log within one process) never leaks nodes
// between test cases.
std::string unique_state_ns()
{
    static std::atomic<int> counter{0};
    return std::string("/rasn_test/state/") + std::to_string(counter.fetch_add(1));
}

// Fresh, wiped work directory per simple-provider test. The "simple" meta-state
// backend keeps a durable on-disk log; wiping the directory guarantees each test
// (and each re-run of the binary) starts from empty state, so assertions like
// "get is NOT_FOUND initially" are deterministic.
std::string fresh_work_dir()
{
    static std::atomic<int> counter{0};
    std::string dir =
        std::string("./rasn_coord_test_") + std::to_string(counter.fetch_add(1));
    dsn::utils::filesystem::remove_path(dir);
    return dir;
}

void run_ownership_contract(rasn_coordination_service *svc)
{
    // node-1 acquires exclusive ownership.
    ASSERT_EQ(ERR_OK, svc->acquire_ownership("resource-A", "node-1", 2000));

    // Re-acquiring by the same owner is idempotent.
    EXPECT_EQ(ERR_OK, svc->acquire_ownership("resource-A", "node-1", 2000));

    // A different owner cannot acquire while node-1 holds it.
    EXPECT_EQ(ERR_TIMEOUT, svc->acquire_ownership("resource-A", "node-2", 300));

    // After node-1 releases, node-2 can take ownership (single-writer handoff).
    ASSERT_EQ(ERR_OK, svc->release_ownership("resource-A", "node-1"));
    EXPECT_EQ(ERR_OK, svc->acquire_ownership("resource-A", "node-2", 2000));

    std::string owner;
    EXPECT_TRUE(svc->query_owner("resource-A", owner));
    EXPECT_EQ(std::string("node-2"), owner);

    EXPECT_EQ(ERR_OK, svc->release_ownership("resource-A", "node-2"));
}

void run_shared_state_contract(rasn_coordination_service *svc)
{
    std::string value;
    EXPECT_EQ(ERR_OBJECT_NOT_FOUND, svc->get_state("registry/agent-1", value));

    EXPECT_EQ(ERR_OK, svc->put_state("registry/agent-1", "endpoint-1"));
    EXPECT_EQ(ERR_OK, svc->put_state("registry/agent-2", "endpoint-2"));

    ASSERT_EQ(ERR_OK, svc->get_state("registry/agent-1", value));
    EXPECT_EQ(std::string("endpoint-1"), value);

    // Overwrite is allowed.
    EXPECT_EQ(ERR_OK, svc->put_state("registry/agent-1", "endpoint-1b"));
    ASSERT_EQ(ERR_OK, svc->get_state("registry/agent-1", value));
    EXPECT_EQ(std::string("endpoint-1b"), value);

    std::vector<std::string> children;
    ASSERT_EQ(ERR_OK, svc->list_state("registry", children));
    EXPECT_EQ(static_cast<size_t>(2), children.size());

    EXPECT_EQ(ERR_OK, svc->delete_state("registry/agent-1"));
    EXPECT_EQ(ERR_OBJECT_NOT_FOUND, svc->get_state("registry/agent-1", value));

    // Deleting an absent key is idempotent.
    EXPECT_EQ(ERR_OK, svc->delete_state("registry/agent-1"));
}

std::shared_ptr<rasn_coordination_context> make_inproc_context()
{
    rasn_coordination_config cfg;
    cfg.provider = "inproc";
    cfg.lock_namespace = unique_state_ns() + "/locks";
    cfg.state_namespace = unique_state_ns();
    return std::shared_ptr<rasn_coordination_context>(
        new rasn_coordination_context(cfg));
}

rasn_shared_breaker_config shared_breaker_test_config()
{
    rasn_shared_breaker_config config;
    config.enabled = true;
    config.state_prefix = "test/breakers";
    config.lock_timeout_ms = 100;
    config.probe_lease_ms = 10;
    config.max_probe_lease_ms = 50;
    config.clock_skew_ms = 0;
    return config;
}

} // namespace

TEST(rasn_coordination, inproc_ownership_and_state)
{
    rasn_coordination_config cfg;
    cfg.provider = "inproc";
    cfg.state_namespace = unique_state_ns();
    auto svc = create_rasn_coordination_service(cfg);
    ASSERT_NE(nullptr, svc.get());
    EXPECT_FALSE(svc->is_distributed());
    EXPECT_STREQ("inproc", svc->provider_name());
    ASSERT_EQ(ERR_OK, svc->start());

    run_ownership_contract(svc.get());
    run_shared_state_contract(svc.get());

    svc->stop();
}

TEST(rasn_coordination, config_defaults)
{
    // The loader falls back to inproc with the documented namespaces.
    rasn_coordination_config cfg = load_rasn_coordination_config();
    EXPECT_FALSE(cfg.provider.empty());
    EXPECT_FALSE(cfg.lock_namespace.empty());
    EXPECT_FALSE(cfg.state_namespace.empty());
    EXPECT_GT(cfg.acquire_timeout_ms, 0);
    EXPECT_GT(cfg.operation_timeout_ms, 0);
}

TEST(rasn_coordination, preserved_ownership_versions_are_monotonic)
{
    rasn_coordination_config cfg;
    cfg.provider = "inproc";
    cfg.state_namespace = unique_state_ns();
    auto svc = create_rasn_coordination_service(cfg);
    ASSERT_NE(nullptr, svc.get());
    ASSERT_EQ(ERR_OK, svc->start());

    uint64_t first = 0;
    uint64_t second = 0;
    ASSERT_EQ(ERR_OK,
              svc->acquire_ownership("flat-lock-id", "node-1", 100, &first));
    ASSERT_EQ(ERR_OK,
              svc->release_ownership("flat-lock-id", "node-1", false));
    ASSERT_EQ(ERR_OK,
              svc->acquire_ownership("flat-lock-id", "node-2", 100, &second));
    EXPECT_GT(second, first);
    EXPECT_EQ(ERR_OK,
              svc->release_ownership("flat-lock-id", "node-2", false));
    svc->stop();
}

TEST(rasn_coordination, post_release_fence_rejects_newer_grants)
{
    rasn_coordination_config cfg;
    cfg.provider = "inproc";
    cfg.state_namespace = unique_state_ns();
    auto svc = create_rasn_coordination_service(cfg);
    ASSERT_NE(nullptr, svc.get());
    ASSERT_EQ(ERR_OK, svc->start());

    uint64_t first = 0;
    uint64_t second = 0;
    ASSERT_EQ(ERR_OK,
              svc->acquire_ownership("barrier-resource", "node-1", 100, &first));
    ASSERT_EQ(ERR_OK,
              svc->release_ownership("barrier-resource", "node-1", false));
    EXPECT_EQ(ERR_OK, svc->verify_ownership_fence("barrier-resource", first));

    ASSERT_EQ(ERR_OK,
              svc->acquire_ownership("barrier-resource", "node-2", 100, &second));
    ASSERT_GT(second, first);
    EXPECT_NE(ERR_OK, svc->verify_ownership_fence("barrier-resource", first));
    ASSERT_EQ(ERR_OK,
              svc->release_ownership("barrier-resource", "node-2", false));
    EXPECT_NE(ERR_OK, svc->verify_ownership_fence("barrier-resource", first));
    EXPECT_EQ(ERR_OK, svc->verify_ownership_fence("barrier-resource", second));
    svc->stop();
}

TEST(rasn_coordination, shared_breaker_is_authoritative_across_registries)
{
    const std::shared_ptr<rasn_coordination_context> context =
        make_inproc_context();
    const rasn_shared_breaker_config shared = shared_breaker_test_config();

    breaker_config breaker;
    breaker.failure_threshold = 1;
    breaker.open_ms = 100;
    circuit_breaker_registry first(breaker);
    circuit_breaker_registry second(breaker);
    first.set_backend(
        create_rasn_shared_breaker_backend("test_scope", shared, context));
    second.set_backend(
        create_rasn_shared_breaker_backend("test_scope", shared, context));

    // Keep one ordinary admission outstanding while another request opens the
    // shared breaker. Its late success must not resolve a later half-open probe.
    const breaker_decision stale_closed = first.allow("dependency", 0);
    ASSERT_TRUE(stale_closed.allowed);
    const breaker_report opened =
        second.report("dependency", second.allow("dependency", 0), false, 0);
    ASSERT_TRUE(opened.available);
    EXPECT_TRUE(opened.opened);

    const breaker_status visible = first.inspect("dependency", 50);
    EXPECT_TRUE(visible.available);
    EXPECT_TRUE(visible.open);
    EXPECT_EQ(breaker_state::open, visible.state);
    EXPECT_FALSE(first.allow("dependency", 50).allowed);

    // Cooldown expiry admits exactly one cluster-wide probe.
    const breaker_decision first_probe = first.allow("dependency", 100);
    ASSERT_TRUE(first_probe.allowed);
    ASSERT_TRUE(first_probe.half_open_probe);
    EXPECT_FALSE(second.allow("dependency", 100).allowed);

    const breaker_report stale =
        second.report("dependency", stale_closed, true, 100);
    EXPECT_TRUE(stale.available);
    EXPECT_FALSE(stale.applied);
    EXPECT_EQ(breaker_state::half_open, stale.state);
    EXPECT_FALSE(second.allow("dependency", 100).allowed);

    // A crashed probe cannot strand the cluster forever: after its lease, a new
    // token replaces it. The superseded token's report is ignored.
    const breaker_decision replacement = second.allow("dependency", 110);
    ASSERT_TRUE(replacement.allowed);
    ASSERT_TRUE(replacement.half_open_probe);
    EXPECT_NE(first_probe.probe_token, replacement.probe_token);
    const breaker_report superseded =
        first.report("dependency", first_probe, true, 110);
    EXPECT_FALSE(superseded.applied);
    EXPECT_EQ(breaker_state::half_open, superseded.state);

    const breaker_report recovered =
        second.report("dependency", replacement, true, 110);
    EXPECT_TRUE(recovered.available);
    EXPECT_TRUE(recovered.applied);
    EXPECT_EQ(breaker_state::closed, recovered.state);
    EXPECT_EQ(0u, recovered.consecutive_failures);

    const breaker_report stale_after_recovery =
        first.report("dependency", stale_closed, false, 111);
    EXPECT_TRUE(stale_after_recovery.available);
    EXPECT_FALSE(stale_after_recovery.applied);
    EXPECT_EQ(breaker_state::closed, stale_after_recovery.state);

    const std::vector<circuit_breaker_registry::entry> snapshot =
        first.snapshot();
    ASSERT_EQ(1u, snapshot.size());
    EXPECT_EQ("dependency", snapshot[0].key);
    EXPECT_TRUE(snapshot[0].shared);
    EXPECT_TRUE(snapshot[0].available);
    EXPECT_EQ(breaker_state::closed, snapshot[0].state);
    EXPECT_GT(snapshot[0].revision, 0u);
}

TEST(rasn_coordination, shared_breaker_caps_probe_lease_hints)
{
    const std::shared_ptr<rasn_coordination_context> context =
        make_inproc_context();
    rasn_shared_breaker_config shared = shared_breaker_test_config();
    shared.max_probe_lease_ms = 20;

    breaker_config config;
    config.failure_threshold = 1;
    config.open_ms = 100;
    circuit_breaker_registry first(config);
    circuit_breaker_registry second(config);
    first.set_backend(
        create_rasn_shared_breaker_backend("lease_scope", shared, context));
    second.set_backend(
        create_rasn_shared_breaker_backend("lease_scope", shared, context));

    const breaker_decision admitted = first.allow("dependency", 0);
    ASSERT_TRUE(first.report("dependency", admitted, false, 0).opened);
    const breaker_decision first_probe =
        first.allow("dependency", 100, (std::numeric_limits<uint64_t>::max)());
    ASSERT_TRUE(first_probe.half_open_probe);

    const breaker_decision replacement = second.allow("dependency", 120);
    EXPECT_TRUE(replacement.allowed);
    EXPECT_TRUE(replacement.half_open_probe);
    EXPECT_NE(first_probe.probe_token, replacement.probe_token);
}

TEST(rasn_coordination, shared_breaker_rejects_invalid_probe_lease_bounds)
{
    rasn_shared_breaker_config shared = shared_breaker_test_config();
    shared.max_probe_lease_ms = shared.probe_lease_ms - 1;

    circuit_breaker_registry registry;
    registry.set_backend(create_rasn_shared_breaker_backend(
        "invalid_lease_scope", shared, make_inproc_context()));
    const breaker_decision rejected = registry.allow("dependency", 0);
    EXPECT_FALSE(rejected.allowed);
    EXPECT_FALSE(rejected.available);
    EXPECT_NE(std::string::npos,
              rejected.error.find("max_probe_lease_ms"));
}

TEST(rasn_coordination, shared_breaker_fails_closed_on_config_mismatch)
{
    const std::shared_ptr<rasn_coordination_context> context =
        make_inproc_context();
    const rasn_shared_breaker_config shared = shared_breaker_test_config();

    breaker_config first_config;
    first_config.failure_threshold = 1;
    first_config.open_ms = 100;
    circuit_breaker_registry first(first_config);
    first.set_backend(
        create_rasn_shared_breaker_backend("mismatch_scope", shared, context));
    const breaker_decision admitted = first.allow("dependency", 0);
    ASSERT_TRUE(admitted.allowed);
    ASSERT_TRUE(first.report("dependency", admitted, false, 0).opened);

    breaker_config second_config = first_config;
    second_config.failure_threshold = 2;
    circuit_breaker_registry second(second_config);
    second.set_backend(
        create_rasn_shared_breaker_backend("mismatch_scope", shared, context));
    const breaker_decision rejected = second.allow("dependency", 0);
    EXPECT_FALSE(rejected.allowed);
    EXPECT_FALSE(rejected.available);
    EXPECT_NE(std::string::npos,
              rejected.error.find("configuration mismatch"));
}

#ifdef RASN_HAS_DIST_COORDINATION

TEST(rasn_coordination, simple_provider_ownership)
{
    rasn_coordination_config cfg;
    cfg.provider = "simple";
    cfg.lock_namespace = "/rasn_test/locks";
    cfg.state_namespace = unique_state_ns();
    cfg.state_work_dir = fresh_work_dir();
    auto svc = create_rasn_coordination_service(cfg);
    ASSERT_NE(nullptr, svc.get());
    EXPECT_TRUE(svc->is_distributed());
    EXPECT_STREQ("simple", svc->provider_name());
    ASSERT_EQ(ERR_OK, svc->start());

    run_ownership_contract(svc.get());
    uint64_t first = 0;
    uint64_t second = 0;
    ASSERT_EQ(ERR_OK,
              svc->acquire_ownership("fenced-resource", "node-1", 2000, &first));
    ASSERT_EQ(ERR_OK,
              svc->release_ownership("fenced-resource", "node-1", false));
    EXPECT_EQ(ERR_OK,
              svc->verify_ownership_fence("fenced-resource", first));
    ASSERT_EQ(ERR_OK,
              svc->acquire_ownership("fenced-resource", "node-2", 2000, &second));
    EXPECT_GT(second, first);
    EXPECT_NE(ERR_OK,
              svc->verify_ownership_fence("fenced-resource", first));
    EXPECT_EQ(ERR_OK,
              svc->release_ownership("fenced-resource", "node-2", false));
    EXPECT_EQ(ERR_OK,
              svc->verify_ownership_fence("fenced-resource", second));

    svc->stop();
}

TEST(rasn_coordination, simple_provider_shared_state)
{
    rasn_coordination_config cfg;
    cfg.provider = "simple";
    cfg.lock_namespace = "/rasn_test/locks";
    cfg.state_namespace = unique_state_ns();
    cfg.state_work_dir = fresh_work_dir();
    auto svc = create_rasn_coordination_service(cfg);
    ASSERT_NE(nullptr, svc.get());
    ASSERT_EQ(ERR_OK, svc->start());

    run_shared_state_contract(svc.get());

    svc->stop();
}

TEST(rasn_coordination, simple_provider_concurrent_put_state_is_lww)
{
    rasn_coordination_config cfg;
    cfg.provider = "simple";
    cfg.lock_namespace = "/rasn_test/locks";
    cfg.state_namespace = unique_state_ns();
    cfg.state_work_dir = fresh_work_dir();
    auto svc = create_rasn_coordination_service(cfg);
    ASSERT_NE(nullptr, svc.get());
    ASSERT_EQ(ERR_OK, svc->start());

    // Many writers race to create the SAME previously-absent key. put_state does a
    // non-atomic node_exist-then-create, so all but one create_node call come back
    // ERR_NODE_ALREADY_EXIST; the facade must fall back to set_data so every writer
    // observes success (last-writer-wins), never a spurious ERR_NODE_ALREADY_EXIST.
    const int kWriters = 8;
    std::vector<error_code> results(kWriters, ERR_UNKNOWN);
    std::vector<std::thread> threads;
    threads.reserve(kWriters);
    for (int i = 0; i < kWriters; ++i) {
        threads.emplace_back([&svc, &results, i]() {
            results[i] = svc->put_state("contended/key",
                                        std::string("writer-") + std::to_string(i));
        });
    }
    for (auto &t : threads)
        t.join();

    for (int i = 0; i < kWriters; ++i)
        EXPECT_EQ(ERR_OK, results[i]) << "writer " << i << " should succeed via LWW fallback";

    // Exactly one of the raced writes must be visible afterwards.
    std::string value;
    ASSERT_EQ(ERR_OK, svc->get_state("contended/key", value));
    EXPECT_EQ(std::string("writer-"), value.substr(0, 7));

    svc->stop();
}

#endif // RASN_HAS_DIST_COORDINATION
