#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

struct schema_field_descriptor
{
    std::string name;
    std::string type;
    bool required;
    std::string notes;
};

struct schema_type_descriptor
{
    std::string name;
    uint32_t version;
    std::string purpose;
    std::vector<schema_field_descriptor> fields;
};

struct rpc_operation_descriptor
{
    std::string service;
    std::string client_class;
    std::string method;
    std::string task_code;
    std::string request_type;
    std::string response_type;
};

std::vector<schema_type_descriptor> rasn_schema_manifest();
std::vector<rpc_operation_descriptor> rasn_rpc_operation_manifest();
std::string rasn_schema_manifest_text();
std::string rasn_schema_manifest_json();
std::string rasn_schema_manifest_idl();
std::string rasn_schema_manifest_cpp_header();
std::string rasn_schema_manifest_cpp_clients();
std::string rasn_schema_manifest_typescript();
std::string rasn_schema_manifest_typescript_clients();
std::string rasn_schema_manifest_python();
std::string rasn_schema_manifest_python_clients();

} // namespace rasn
} // namespace dsn
