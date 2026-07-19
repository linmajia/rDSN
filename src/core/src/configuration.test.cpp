/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 * 
 * -=- Robust Distributed System Nucleus (rDSN) -=- 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * Description:
 *     Unit-test for configuration.
 *
 * Revision history:
 *     Nov., 2015, @qinzuoyan (Zuoyan Qin), first version
 *     xxxx-xx-xx, author, fix bug about xxx
 */

# include <dsn/utility/configuration.h>
# include <dsn/cpp/test_output_utils.h>
# include <dsn/cpp/utils.h>
# include <gtest/gtest.h>
# include <algorithm>
# include <fstream>
# include <cstring>
# include <atomic>
# include <sstream>
# include <string>
# include <thread>
# include <vector>

using namespace ::dsn;

TEST(core, configuration)
{
    scoped_test_stderr stderr_capture;
    configuration_ptr c;

    fprintf(stdout, "load not_exist_config_file\n");
    c.reset(new configuration());
    ASSERT_FALSE(c->load("not_exist_config_file"));

    fprintf(stdout, "load config-empty.ini\n");
    c.reset(new configuration());
    ASSERT_FALSE(c->load("config-empty.ini"));

    fprintf(stdout, "load config-sample.ini with bad arguments\n");
    c.reset(new configuration());
    ASSERT_FALSE(c->load("config-sample.ini", "a="));

    fprintf(stdout, "load config-no-section.ini\n");
    c.reset(new configuration());
    ASSERT_FALSE(c->load("config-no-section.ini"));

    fprintf(stdout, "load config-null-section.ini\n");
    c.reset(new configuration());
    ASSERT_FALSE(c->load("config-null-section.ini"));

    fprintf(stdout, "load config-dup-section.ini\n");
    c.reset(new configuration());
    // we now allow duplicated section (as for include and overwrite)
    ASSERT_TRUE(c->load("config-dup-section.ini"));

    fprintf(stdout, "load config-unmatch-section.ini\n");
    c.reset(new configuration());
    ASSERT_FALSE(c->load("config-unmatch-section.ini"));

    fprintf(stdout, "load config-bad-section.ini\n");
    c.reset(new configuration());
    ASSERT_FALSE(c->load("config-bad-section.ini"));

    fprintf(stdout, "load config-no-key.ini\n");
    c.reset(new configuration());
    ASSERT_FALSE(c->load("config-no-key.ini"));

    fprintf(stdout, "load config-dup-key.ini\n");
    c.reset(new configuration());
    ASSERT_TRUE(c->load("config-dup-key.ini"));

    fprintf(stdout, "load config-sample.ini\n");
    c.reset(new configuration());
    ASSERT_TRUE(c->load("config-sample.ini", "replace=replace_value"));
    bool old = c->set_warning(true);
    ASSERT_FALSE(old);

    std::vector<std::string> sections;
    c->get_all_sections(sections);
    ASSERT_EQ(5u, sections.size());
    std::sort(sections.begin(), sections.end());
    ASSERT_EQ("apps..default", sections[0]);
    ASSERT_EQ("apps.client", sections[1]);
    ASSERT_EQ("apps.server", sections[2]);
    ASSERT_EQ("modules", sections[3]);
    ASSERT_EQ("test", sections[4]);

    std::vector<const char*> keys;
    c->get_all_keys("apps..default", keys);
    ASSERT_EQ(2u, keys.size());
    std::sort(keys.begin(), keys.end(), [](const char* l, const char* r){ return strcmp(l,r) < 0; });
    ASSERT_STREQ("count", keys[0]);
    ASSERT_STREQ("run", keys[1]);

    c->get_all_keys("test", keys);
    ASSERT_EQ(0u, keys.size());

    auto v = c->get_string_value("apps.server", "replace_data", "unknown", "for test replace");
    ASSERT_STREQ("replace_value", v);

    v = c->get_string_value("apps.server", "shift_data", "unknown", "for test shift");
    ASSERT_STREQ("head#middle;tail", v);

    // add [apps..default] my_key
    v = c->get_string_value("apps..default", "my_key", "my_value", "my key and value");
    ASSERT_STREQ("my_value", v);
    v = c->get_string_value("apps..default", "my_key", "my_value", "my key and value again");
    ASSERT_STREQ("my_value", v);
    c->get_all_keys("apps..default", keys);
    ASSERT_EQ(3u, keys.size());
    std::sort(keys.begin(), keys.end(), [](const char* l, const char* r){ return strcmp(l,r) < 0; });
    ASSERT_STREQ("count", keys[0]);
    ASSERT_STREQ("my_key", keys[1]);
    ASSERT_STREQ("run", keys[2]);

    // add [my_section] my_key
    v = c->get_string_value("my_section", "my_key", "my_value", "my key and value");
    ASSERT_STREQ("my_value", v);
    c->get_all_sections(sections);
    ASSERT_EQ(6u, sections.size());
    std::sort(sections.begin(), sections.end());
    ASSERT_EQ("apps..default", sections[0]);
    ASSERT_EQ("apps.client", sections[1]);
    ASSERT_EQ("apps.server", sections[2]);    
    ASSERT_EQ("modules", sections[3]);
    ASSERT_EQ("my_section", sections[4]);
    ASSERT_EQ("test", sections[5]);
    c->get_all_keys("my_section", keys);
    ASSERT_EQ(1u, keys.size());
    ASSERT_STREQ("my_key", keys[0]);

    std::list<std::string> l = c->get_string_value_list("apps.client", "pools", ',', "thread pools");
    ASSERT_EQ(2u, l.size());
    ASSERT_STREQ("THREAD_POOL_DEFAULT", l.begin()->c_str());
    ASSERT_STREQ("THREAD_POOL_TEST_SERVER", (++l.begin())->c_str());

    l = c->get_string_value_list("apps.client", "my_list", ',', "my list");
    ASSERT_EQ(0u, l.size());

    ASSERT_TRUE(c->has_section("test"));
    ASSERT_FALSE(c->has_section("unexist_section"));

    ASSERT_TRUE(c->has_key("apps..default", "run"));
    ASSERT_FALSE(c->has_key("apps..default", "unexist_key"));
    ASSERT_FALSE(c->has_key("unexist_section", "unexist_key"));

    ASSERT_STREQ("config-sample.ini", c->get_file_name());

    ASSERT_EQ("unexist_value", c->get_value<std::string>("apps.client", "unexist_key", "unexist_value", ""));
    ASSERT_EQ(1.0, c->get_value<double>("apps.client", "count", 2.0, "client count"));
    ASSERT_EQ(2.0, c->get_value<double>("apps.client", "unexist_double_key", 2.0, ""));
    ASSERT_EQ(1, c->get_value<long long>("apps.client", "count", 100, "client count"));
    ASSERT_EQ(100, c->get_value<long long>("apps.client", "unexist_long_long_key", 100, ""));
    ASSERT_EQ(0xdead, c->get_value<long long>("apps.server", "hex_data", 100, ""));
    ASSERT_EQ(1u, c->get_value<unsigned long long>("apps.client", "count", 100, "client count"));
    ASSERT_EQ(100u, c->get_value<unsigned long long>("apps.client", "unexist_unsigned_long_long_key", 100, ""));
    ASSERT_EQ(1, c->get_value<long>("apps.client", "count", 100, "client count"));
    ASSERT_EQ(100, c->get_value<long>("apps.client", "unexist_long_key", 100, ""));
    ASSERT_EQ(0xdead, c->get_value<long>("apps.server", "hex_data", 100, ""));
    ASSERT_EQ(1u, c->get_value<unsigned long>("apps.client", "count", 100, "client count"));
    ASSERT_EQ(100u, c->get_value<unsigned long>("apps.client", "unexist_unsigned_long_key", 100, ""));
    ASSERT_EQ(1, c->get_value<int>("apps.client", "count", 100, "client count"));
    ASSERT_EQ(100, c->get_value<int>("apps.client", "unexist_int_key", 100, ""));
    ASSERT_EQ(1u, c->get_value<unsigned int>("apps.client", "count", 100, "client count"));
    ASSERT_EQ(100u, c->get_value<unsigned int>("apps.client", "unexist_unsigned_int_key", 100, ""));
    ASSERT_EQ(1, c->get_value<short>("apps.client", "count", 100, "client count"));
    ASSERT_EQ(100, c->get_value<short>("apps.client", "unexist_short_key", 100, ""));
    ASSERT_EQ(1u, c->get_value<unsigned short>("apps.client", "count", 100, "client count"));
    ASSERT_EQ(100u, c->get_value<unsigned short>("apps.client", "unexist_unsigned_short_key", 100, ""));
    ASSERT_TRUE(c->get_value<bool>("apps.client", "run", false, "client run"));
    ASSERT_FALSE(c->get_value<bool>("apps.client", "unexist_bool_key", false, ""));

    ASSERT_TRUE(::dsn::utils::test::prepare_test_tmp_dir("dsn.core.configuration"));
    const std::string dump_file =
        ::dsn::utils::test::test_tmp_path("dsn.core.configuration", "config-sample-dump.ini");
    std::fstream out;
    out.open(dump_file.c_str(), std::ios::out);
    c->dump(out);
    out.close();

    fprintf(stdout, "load config-sample-dump.ini\n");
    c.reset(new configuration());
    ASSERT_TRUE(c->load(dump_file.c_str()));
    c->get_all_sections(sections);
    ASSERT_EQ(6u, sections.size());
    std::sort(sections.begin(), sections.end());
    ASSERT_EQ("apps..default", sections[0]);
    ASSERT_EQ("apps.client", sections[1]);
    ASSERT_EQ("apps.server", sections[2]);        
    ASSERT_EQ("modules", sections[3]);
    ASSERT_EQ("my_section", sections[4]);
    ASSERT_EQ("test", sections[5]);

    // configuration set test
    ASSERT_TRUE(!c->has_key("not-exsit", "not-exsit"));
    c->set("not-exsit", "not-exsit", "exsit", "kaka");
    ASSERT_EQ(std::string("exsit"), std::string(c->get_string_value("not-exsit", "not-exsit", "", "")));
    c->set("not-exsit", "not-exsit", "exsit2", "kaka");
    ASSERT_EQ(std::string("exsit2"), std::string(c->get_string_value("not-exsit", "not-exsit", "", "")));
}

TEST(core, configuration_set_promotes_cached_default)
{
    configuration config;
    const char* section = "explicit_override";
    const char* key = "cached_default";

    ASSERT_STREQ("fallback-a", config.get_string_value(section, key, "fallback-a", ""));
    config.set(section, key, "explicit-b", "");

    EXPECT_TRUE(config.has_key(section, key));
    EXPECT_STREQ("explicit-b", config.get_string_value(section, key, "fallback-a", ""));
    EXPECT_STREQ("explicit-b", config.get_string_value(section, key, "fallback-c", ""));
}

TEST(core, configuration_snapshots_restore_exact_value_state)
{
    configuration config;

    const configuration_value_snapshot missing =
        config.query_value("snapshot_missing", "value");
    EXPECT_EQ(configuration_value_state::missing, missing.state);
    EXPECT_FALSE(missing.section_existed);
    EXPECT_TRUE(missing.value.empty());

    const configuration_value_snapshot missing_before_override =
        config.set_with_snapshot("snapshot_missing", "value", "override", "");
    EXPECT_EQ(configuration_value_state::missing, missing_before_override.state);
    EXPECT_EQ(configuration_value_state::explicit_value,
              config.query_value("snapshot_missing", "value").state);
    config.restore_snapshot("snapshot_missing", "value", missing_before_override);
    const configuration_value_snapshot missing_after_restore =
        config.query_value("snapshot_missing", "value");
    EXPECT_EQ(configuration_value_state::missing, missing_after_restore.state);
    EXPECT_FALSE(missing_after_restore.section_existed);

    ASSERT_STREQ("cached-a",
                 config.get_string_value("snapshot_cached", "value", "cached-a", ""));
    const configuration_value_snapshot cached =
        config.query_value("snapshot_cached", "value");
    EXPECT_EQ(configuration_value_state::defaulted, cached.state);
    EXPECT_TRUE(cached.section_existed);
    EXPECT_EQ("cached-a", cached.value);
    const configuration_value_snapshot cached_before_override =
        config.set_with_snapshot("snapshot_cached", "value", "override", "");
    EXPECT_EQ(configuration_value_state::defaulted, cached_before_override.state);
    config.restore_snapshot("snapshot_cached", "value", cached_before_override);
    const configuration_value_snapshot cached_after_restore =
        config.query_value("snapshot_cached", "value");
    EXPECT_EQ(configuration_value_state::defaulted, cached_after_restore.state);
    EXPECT_EQ("cached-a", cached_after_restore.value);

    config.set("snapshot_explicit", "value", "explicit-a", "");
    const configuration_value_snapshot explicit_before_override =
        config.set_with_snapshot("snapshot_explicit", "value", "explicit-b", "");
    EXPECT_EQ(configuration_value_state::explicit_value, explicit_before_override.state);
    EXPECT_EQ("explicit-a", explicit_before_override.value);
    EXPECT_EQ("explicit-b", config.query_value("snapshot_explicit", "value").value);
    config.restore_snapshot("snapshot_explicit", "value", explicit_before_override);
    const configuration_value_snapshot explicit_after_restore =
        config.query_value("snapshot_explicit", "value");
    EXPECT_EQ(configuration_value_state::explicit_value, explicit_after_restore.state);
    EXPECT_EQ("explicit-a", explicit_after_restore.value);
}

TEST(core, configuration_set_preserves_empty_inputs_and_rejects_nulls)
{
    scoped_test_stderr stderr_capture;
    configuration config;

    config.set("", "key", "empty-section", "empty section");
    EXPECT_EQ("empty-section", config.query_value("", "key").value);
    config.set("section", "", "empty-key", "empty key");
    EXPECT_EQ("empty-key", config.query_value("section", "").value);
    config.set("section", "empty-value", "", "empty value");
    const configuration_value_snapshot empty_value =
        config.query_value("section", "empty-value");
    EXPECT_EQ(configuration_value_state::explicit_value, empty_value.state);
    EXPECT_TRUE(empty_value.value.empty());

    const configuration_value_snapshot empty_names =
        config.set_with_snapshot("", "", "", "empty section and key");
    EXPECT_EQ(configuration_value_state::missing, empty_names.state);
    EXPECT_EQ(configuration_value_state::explicit_value, config.query_value("", "").state);
    EXPECT_TRUE(config.query_value("", "").value.empty());
    config.restore_snapshot("", "", empty_names);
    EXPECT_EQ(configuration_value_state::missing, config.query_value("", "").state);
    EXPECT_EQ("empty-section", config.query_value("", "key").value);

    ASSERT_TRUE(::dsn::utils::test::prepare_test_tmp_dir("dsn.core.configuration.empty"));
    const std::string config_path = ::dsn::utils::test::test_tmp_path(
        "dsn.core.configuration.empty", "empty-overwrite.ini");
    std::fstream output(config_path.c_str(), std::ios::out | std::ios::trunc);
    ASSERT_TRUE(output.good());
    output << "[seed]\nvalue = original\n";
    output.close();

    configuration overwritten;
    ASSERT_TRUE(overwritten.load(config_path.c_str(), nullptr, ".foo=x;foo.=y"));
    EXPECT_EQ("x", overwritten.query_value("", "foo").value);
    EXPECT_EQ("y", overwritten.query_value("foo", "").value);

    config.set(nullptr, "key", "value", "");
    config.set("section", nullptr, "value", "");
    config.set("section", "null-value", nullptr, "");
    EXPECT_EQ(configuration_value_state::missing,
              config.query_value("section", "null-value").state);
    EXPECT_EQ(configuration_value_state::missing,
              config.set_with_snapshot(nullptr, "key", "value", "").state);
    EXPECT_EQ(configuration_value_state::missing,
              config.set_with_snapshot("section", nullptr, "value", "").state);
    EXPECT_EQ(configuration_value_state::missing,
              config.set_with_snapshot("section", "null-snapshot-value", nullptr, "").state);
    EXPECT_EQ(configuration_value_state::missing, config.query_value(nullptr, "key").state);
    EXPECT_EQ(configuration_value_state::missing, config.query_value("section", nullptr).state);
    config.restore_snapshot(nullptr, "key", empty_names);
    config.restore_snapshot("section", nullptr, empty_names);
    EXPECT_EQ("empty-key", config.query_value("section", "").value);
}

TEST(core, configuration_snapshot_restores_metadata_after_deletion)
{
    ASSERT_TRUE(::dsn::utils::test::prepare_test_tmp_dir("dsn.core.configuration.metadata"));
    const std::string config_path = ::dsn::utils::test::test_tmp_path(
        "dsn.core.configuration.metadata", "metadata.ini");
    std::fstream output(config_path.c_str(), std::ios::out | std::ios::trunc);
    ASSERT_TRUE(output.good());
    output << "[metadata]\nvalue = original\n";
    output.close();

    configuration config;
    ASSERT_TRUE(config.load(config_path.c_str()));
    EXPECT_STREQ(
        "original", config.get_string_value("metadata", "value", "", "original description"));
    const configuration_value_snapshot original =
        config.query_value("metadata", "value");
    ASSERT_EQ(configuration_value_state::explicit_value, original.state);
    EXPECT_EQ("original", original.value);
    EXPECT_EQ("original description", original.description);
    EXPECT_GT(original.line, 0);

    const configuration_value_snapshot missing_in_section =
        config.query_value("metadata", "missing");
    ASSERT_EQ(configuration_value_state::missing, missing_in_section.state);
    ASSERT_TRUE(missing_in_section.section_existed);
    config.restore_snapshot("metadata", "value", missing_in_section);
    EXPECT_EQ(configuration_value_state::missing,
              config.query_value("metadata", "value").state);

    config.restore_snapshot("metadata", "value", original);
    const configuration_value_snapshot restored =
        config.query_value("metadata", "value");
    EXPECT_EQ(original.state, restored.state);
    EXPECT_EQ(original.value, restored.value);
    EXPECT_EQ(original.description, restored.description);
    EXPECT_EQ(original.line, restored.line);

    std::ostringstream dump;
    config.dump(dump);
    EXPECT_NE(std::string::npos, dump.str().find("; original description\n"));
    EXPECT_NE(std::string::npos, dump.str().find("value = original\n"));
}

TEST(core, configuration_snapshot_restores_section_existence)
{
    configuration config;

    const configuration_value_snapshot new_section =
        config.set_with_snapshot("new_section", "override", "value", "");
    ASSERT_EQ(configuration_value_state::missing, new_section.state);
    ASSERT_FALSE(new_section.section_existed);
    config.restore_snapshot("new_section", "override", new_section);
    EXPECT_FALSE(config.has_section("new_section"));

    config.set("existing_section", "seed", "value", "");
    const configuration_value_snapshot missing_in_existing =
        config.query_value("existing_section", "override");
    ASSERT_EQ(configuration_value_state::missing, missing_in_existing.state);
    ASSERT_TRUE(missing_in_existing.section_existed);
    config.restore_snapshot("existing_section", "seed", missing_in_existing);
    ASSERT_TRUE(config.has_section("existing_section"));
    std::vector<const char*> keys;
    config.get_all_keys("existing_section", keys);
    ASSERT_TRUE(keys.empty());

    const configuration_value_snapshot empty_section =
        config.set_with_snapshot("existing_section", "override", "value", "");
    ASSERT_EQ(configuration_value_state::missing, empty_section.state);
    ASSERT_TRUE(empty_section.section_existed);
    config.restore_snapshot("existing_section", "override", empty_section);
    EXPECT_TRUE(config.has_section("existing_section"));
    config.get_all_keys("existing_section", keys);
    EXPECT_TRUE(keys.empty());
    std::vector<std::string> sections;
    config.get_all_sections(sections);
    EXPECT_NE(sections.end(),
              std::find(sections.begin(), sections.end(), "existing_section"));

    const configuration_value_snapshot concurrent_section =
        config.set_with_snapshot("concurrent_section", "override", "value", "");
    config.set("concurrent_section", "unrelated", "keep", "");
    config.restore_snapshot("concurrent_section", "override", concurrent_section);
    EXPECT_TRUE(config.has_section("concurrent_section"));
    EXPECT_EQ("keep", config.query_value("concurrent_section", "unrelated").value);
    EXPECT_EQ(configuration_value_state::missing,
              config.query_value("concurrent_section", "override").state);
}

TEST(core, configuration_invalid_numeric_values)
{
    scoped_test_stderr stderr_capture;
    configuration c;
    const char* section = "invalid_numbers";

    c.set(section, "invalid_int", "abc", "");
    c.set(section, "partial_int", "123abc", "");
    c.set(section, "overflow_int", "999999999999999999999999999999999999", "");
    c.set(section, "invalid_unsigned_int", "-abc", "");
    c.set(section, "partial_unsigned_int", "456abc", "");
    c.set(section, "invalid_hex", "0xzz", "");
    c.set(section, "invalid_uppercase_hex", "0Xzz", "");
    c.set(section, "overflow_hex", "0xffffffffffffffff", "");

    ASSERT_EQ(11, c.get_value<int>(section, "invalid_int", 11, ""));
    ASSERT_EQ(12, c.get_value<long>(section, "partial_int", 12, ""));
    ASSERT_EQ(13, c.get_value<long long>(section, "overflow_int", 13, ""));
    ASSERT_EQ(16u, c.get_value<unsigned int>(section, "invalid_unsigned_int", 16, ""));
    ASSERT_EQ(17u, c.get_value<unsigned int>(section, "partial_unsigned_int", 17, ""));
    ASSERT_EQ(14, c.get_value<long>(section, "invalid_hex", 14, ""));
    ASSERT_EQ(15, c.get_value<long long>(section, "invalid_uppercase_hex", 15, ""));
    ASSERT_EQ(18, c.get_value<long long>(section, "overflow_hex", 18, ""));
}

TEST(core, configuration_circular_include)
{
    scoped_test_stderr stderr_capture;
    // A circular @include chain must be rejected gracefully (load returns false)
    // instead of recursing through load()/load_include() until the stack
    // overflows and the process crashes.

    // a.ini -> b.ini -> a.ini
    {
        configuration c;
        ASSERT_FALSE(c.load("config-include-cycle-a.ini"));
    }

    // self-include: a.ini -> a.ini
    {
        configuration c;
        ASSERT_FALSE(c.load("config-include-self.ini"));
    }

    // 3-node cycle: a.ini -> b.ini -> c.ini -> a.ini
    {
        configuration c;
        ASSERT_FALSE(c.load("config-include-cycle3-a.ini"));
    }

    // A diamond include (the same file reached via two different, non-cyclic
    // paths) is NOT a cycle and must still load successfully.
    {
        configuration c;
        ASSERT_TRUE(c.load("config-include-diamond-a.ini"));
        ASSERT_STREQ("dv", c.get_string_value("diamond_test", "dk", "unknown", ""));
    }

    // A deep (non-cyclic) include chain must load successfully.
    {
        configuration c;
        ASSERT_TRUE(c.load("config-include-deep-a.ini"));
        ASSERT_STREQ("dv", c.get_string_value("deep_test", "dk", "unknown", ""));
    }

    // After a rejected circular load, the per-thread include-tracking state must
    // be clean again, so a subsequent valid load on the same thread succeeds.
    {
        configuration c1;
        ASSERT_FALSE(c1.load("config-include-cycle-a.ini"));
        configuration c2;
        ASSERT_TRUE(c2.load("config-include-diamond-a.ini"));
        ASSERT_STREQ("dv", c2.get_string_value("diamond_test", "dk", "unknown", ""));
    }
}

TEST(core, configuration_concurrent_include)
{
    scoped_test_stderr stderr_capture;
    // The include-tracking set is thread_local, so loading configurations
    // concurrently on multiple threads is race-free: each thread tracks only its
    // own @include ancestry, with no shared mutable state and no cross-thread
    // false positive. No matter how the loads interleave, every valid (diamond)
    // load must succeed and every circular load must be rejected.
    const int thread_count = 8;
    // Static storage so the thread lambda below can read it *without* capturing
    // it: capturing this const int is flagged by clang (-Wunused-lambda-capture,
    // it's a constant expression) while NOT capturing an automatic one is
    // rejected by MSVC (C3493). A static local is never captured, which satisfies
    // clang, gcc and MSVC alike.
    static const int iterations = 50;

    std::atomic<int> diamond_ok(0);
    std::atomic<int> cycle_rejected(0);

    std::vector<std::thread> threads;
    for (int t = 0; t < thread_count; ++t)
    {
        threads.emplace_back([&diamond_ok, &cycle_rejected]() {
            for (int i = 0; i < iterations; ++i)
            {
                {
                    configuration c;
                    if (c.load("config-include-diamond-a.ini")
                        && std::string("dv")
                               == c.get_string_value("diamond_test", "dk", "unknown", ""))
                    {
                        diamond_ok.fetch_add(1);
                    }
                }
                {
                    configuration c;
                    if (!c.load("config-include-cycle-a.ini"))
                    {
                        cycle_rejected.fetch_add(1);
                    }
                }
            }
        });
    }

    for (auto& th : threads)
    {
        th.join();
    }

    ASSERT_EQ(thread_count * iterations, diamond_ok.load());
    ASSERT_EQ(thread_count * iterations, cycle_rejected.load());
}
