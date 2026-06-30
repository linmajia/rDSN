#pragma once

#include "agent_types.h"

#include <dsn/cpp/serialization.h>

#include <cstdint>
#include <string>

namespace dsn {
namespace rasn {

struct model_provider_descriptor
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    std::string provider;
    std::string model;
    std::string endpoint;
    std::string payload_format;
    std::string token_env;
    std::string token_command_ref;
    std::string credential_ref;
    bool token_required = false;
    bool local = true;
    bool streaming = false;
    std::string health;
};

struct model_provider_request
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    std::string provider;
};

struct model_gateway_response
{
    uint32_t schema_version = RASN_AGENT_SCHEMA_VERSION;
    bool ok = true;
    std::string error;
    model_provider_descriptor provider;
};

inline void marshall(::dsn::binary_writer &writer, const model_provider_descriptor &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.provider);
    writer.write(value.model);
    writer.write(value.endpoint);
    writer.write(value.payload_format);
    writer.write(value.token_env);
    writer.write(value.token_command_ref);
    writer.write(value.credential_ref);
    writer.write(value.token_required);
    writer.write(value.local);
    writer.write(value.streaming);
    writer.write(value.health);
}

inline void unmarshall(::dsn::binary_reader &reader, model_provider_descriptor &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.provider);
    reader.read(value.model);
    reader.read(value.endpoint);
    reader.read(value.payload_format);
    reader.read(value.token_env);
    reader.read(value.token_command_ref);
    reader.read(value.credential_ref);
    reader.read(value.token_required);
    reader.read(value.local);
    reader.read(value.streaming);
    reader.read(value.health);
}

inline void marshall(::dsn::binary_writer &writer, const model_provider_request &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.provider);
}

inline void unmarshall(::dsn::binary_reader &reader, model_provider_request &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.provider);
}

inline void marshall(::dsn::binary_writer &writer, const model_gateway_response &value, ::dsn_msg_serialize_format fmt)
{
    writer.write(value.schema_version);
    writer.write(value.ok);
    writer.write(value.error);
    marshall(writer, value.provider, fmt);
}

inline void unmarshall(::dsn::binary_reader &reader, model_gateway_response &value, ::dsn_msg_serialize_format fmt)
{
    reader.read(value.schema_version);
    reader.read(value.ok);
    reader.read(value.error);
    unmarshall(reader, value.provider, fmt);
}

} // namespace rasn
} // namespace dsn
