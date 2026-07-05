// Unit tests for the rASN distributed coordination facade
// (rasn/coordination_service.h). The in-process fallback is always exercised.
// When the rDSN.dist.service plugin is compiled in (RASN_HAS_DIST_COORDINATION),
// the "simple" distributed provider is exercised too, proving rASN reuses
// rDSN's distributed_lock_service / meta_state_service for ownership election
// and cluster-shared state without a ZooKeeper cluster.
#include <gtest/gtest.h>

#include <rasn/coordination_service.h>
#include <dsn/cpp/utils.h>

#include <atomic>
#include <string>
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

#endif // RASN_HAS_DIST_COORDINATION
