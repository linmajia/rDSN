#pragma once

#include "agent_types.h"
#include "agent_tools.h"
#include "state_service.h"

#include <dsn/cpp/serialization.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

enum class tool_side_effect
{
    read_only,
    write,
    shell,
    network,
    unknown
};

struct policy_request
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    std::string tool_name;
    std::vector<std::string> args;
    std::string side_effect;
    std::string target;
    std::string actor;
    std::vector<std::string> policy_labels;
};

struct policy_decision
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    bool allowed = false;
    std::string reason;
    std::string side_effect;
    std::string policy_name;
};

std::string to_string(tool_side_effect side_effect);
tool_side_effect classify_tool_side_effect(const std::string &tool_name);
std::string human_approval_policy_label(tool_side_effect side_effect);
bool policy_labels_include_human_approval(const std::vector<std::string> &policy_labels,
                                         tool_side_effect side_effect);
bool policy_target_within_workspace(const std::string &target,
                                   const std::string &workspace_root,
                                   std::string *normalized_target,
                                   std::string *normalized_root);
policy_request make_policy_request(const std::string &tool_name,
                                   const std::vector<std::string> &args,
                                   const agent_task &task,
                                   const std::vector<std::string> &policy_labels = std::vector<std::string>());

typedef state_response (*policy_state_writer)(const state_record &record);
void set_policy_state_writer(policy_state_writer writer);
void reset_policy_state_writer();

class policy_manager
{
public:
    policy_decision evaluate(const policy_request &request) const;
    tool_result apply_tool_output_bounds(const std::string &tool_name,
                                         const agent_task &task,
                                         const tool_result &result) const;

private:
    bool config_bool(const std::string &key, bool fallback) const;
    size_t config_size(const std::string &key, size_t fallback) const;
    std::string config_string(const std::string &key, const std::string &fallback) const;
    std::string artifact_dir() const;
    std::string effective_workspace_root() const;
};

policy_manager &global_policy_manager();

inline void marshall_policy_string_vector(::dsn::binary_writer &writer, const std::vector<std::string> &values)
{
    writer.write(static_cast<uint32_t>(values.size()));
    for (const std::string &value : values)
    {
        writer.write(value);
    }
}

inline void unmarshall_policy_string_vector(::dsn::binary_reader &reader, std::vector<std::string> &values)
{
    uint32_t count = 0;
    reader.read(count);
    values.clear();
    values.reserve(rasn_bounded_reserve_count<std::string>(count));
    for (uint32_t i = 0; i < count; ++i)
    {
        std::string value;
        reader.read(value);
        values.push_back(value);
    }
}

inline void marshall(::dsn::binary_writer &writer, const policy_request &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.tool_name);
    marshall_policy_string_vector(writer, value.args);
    writer.write(value.side_effect);
    writer.write(value.target);
    writer.write(value.actor);
    marshall_policy_string_vector(writer, value.policy_labels);
}

inline void unmarshall(::dsn::binary_reader &reader, policy_request &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.tool_name);
    unmarshall_policy_string_vector(reader, value.args);
    reader.read(value.side_effect);
    reader.read(value.target);
    reader.read(value.actor);
    unmarshall_policy_string_vector(reader, value.policy_labels);
}

inline void marshall(::dsn::binary_writer &writer, const policy_decision &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.allowed);
    writer.write(value.reason);
    writer.write(value.side_effect);
    writer.write(value.policy_name);
}

inline void unmarshall(::dsn::binary_reader &reader, policy_decision &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.allowed);
    reader.read(value.reason);
    reader.read(value.side_effect);
    reader.read(value.policy_name);
}

} // namespace rasn
} // namespace dsn
