#pragma once

#include <string>
#include <vector>

namespace dsn {
namespace rasn {

struct model_provider_profile
{
    std::string name;
    std::string endpoint;
    std::string model;
    std::string token_env;
    std::string credential_ref;
    std::string payload_format;
    bool local = true;
    bool in_process = false;
    std::vector<std::string> headers;
};

std::string normalize_model_provider_name(const std::string &provider_name);
std::string model_provider_config_key(const std::string &provider_name, const std::string &key);
std::string rasn_model_config_string(const std::string &key,
                                     const std::string &fallback,
                                     const std::string &help);
std::string model_provider_config_string(const std::string &provider_name,
                                         const std::string &key,
                                         const std::string &fallback,
                                         const std::string &help);
model_provider_profile resolve_model_provider_profile(const std::string &provider_name,
                                                      const std::string &model_override = "");
std::string describe_model_provider_routing();

} // namespace rasn
} // namespace dsn
