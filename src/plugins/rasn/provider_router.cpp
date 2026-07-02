#include "provider_router.h"

#include "rasn_core.h"

#include <dsn/service_api_cpp.h>

#include <algorithm>
#include <cctype>

namespace dsn {
namespace rasn {

namespace {

std::string config_string(const std::string &section,
                          const std::string &key,
                          const std::string &fallback,
                          const std::string &help)
{
    const char *value = ::dsn_config_get_value_string(section.c_str(), key.c_str(), fallback.c_str(), help.c_str());
    const std::string result = value == nullptr ? "" : value;
    return result.empty() ? fallback : result;
}

std::string lower_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

void apply_model_override(model_provider_profile *profile, const std::string &model_override)
{
    const std::string model = trim(model_override);
    if (!model.empty())
    {
        profile->model = model;
    }
}

} // namespace

std::string normalize_model_provider_name(const std::string &provider_name)
{
    const std::string trimmed = trim(provider_name.empty() ? "simulator" : provider_name);
    const std::string lower = lower_ascii(trimmed);
    if (lower == "mock" || lower == "random")
    {
        return "simulator";
    }
    if (lower == "github-copilot")
    {
        return "copilot";
    }
    if (lower == "llamacpp" || lower == "llama-cpp" || lower == "llama.cpp")
    {
        return "llama.cpp";
    }
    if (lower == "lm-studio")
    {
        return "lmstudio";
    }
    return trimmed.empty() ? "simulator" : trimmed;
}

std::string model_provider_config_key(const std::string &provider_name, const std::string &key)
{
    std::string normalized = provider_name;
    for (char &c : normalized)
    {
        if (c == '.' || c == '-')
        {
            c = '_';
        }
    }
    return normalized + "_" + key;
}

std::string rasn_model_config_string(const std::string &key,
                                     const std::string &fallback,
                                     const std::string &help)
{
    const std::string model_value = config_string("rasn.model", key, "", help);
    if (!model_value.empty())
    {
        return model_value;
    }
    const std::string legacy_value = config_string("rasn.llm", key, "", help);
    if (!legacy_value.empty())
    {
        return legacy_value;
    }
    return fallback;
}

std::string model_provider_config_string(const std::string &provider_name,
                                         const std::string &key,
                                         const std::string &fallback,
                                         const std::string &help)
{
    const std::string specific = rasn_model_config_string(model_provider_config_key(provider_name, key), "", help);
    return specific.empty() ? rasn_model_config_string(key, fallback, help) : specific;
}

model_provider_profile resolve_model_provider_profile(const std::string &provider_name,
                                                      const std::string &model_override)
{
    const std::string provider = normalize_model_provider_name(provider_name);
    model_provider_profile profile;

    if (provider == "simulator")
    {
        profile.name = "simulator";
        profile.endpoint = "local";
        profile.model = "rasn-random-simulator";
        profile.payload_format = "simulator";
        profile.local = true;
        profile.in_process = true;
        apply_model_override(&profile, model_override);
        return profile;
    }

    if (provider == "ollama")
    {
        profile.name = "ollama";
        profile.endpoint = model_provider_config_string("ollama", "endpoint", "http://localhost:11434/api/chat", "Ollama endpoint");
        profile.model = model_provider_config_string("ollama", "model", "llama3.1", "Ollama model name");
        profile.payload_format = model_provider_config_string("ollama", "payload", "ollama.chat", "Ollama payload format");
        profile.local = true;
        apply_model_override(&profile, model_override);
        return profile;
    }

    if (provider == "llama.cpp")
    {
        profile.name = "llama.cpp";
        profile.endpoint =
            model_provider_config_string("llama_cpp", "endpoint", "http://localhost:8080/v1/chat/completions", "llama.cpp endpoint");
        profile.model = model_provider_config_string("llama_cpp", "model", "local-model", "llama.cpp model name");
        profile.payload_format = "openai.chat";
        profile.local = true;
        apply_model_override(&profile, model_override);
        return profile;
    }

    if (provider == "lmstudio")
    {
        profile.name = "lmstudio";
        profile.endpoint =
            model_provider_config_string("lmstudio", "endpoint", "http://localhost:1234/v1/chat/completions", "LM Studio endpoint");
        profile.model = model_provider_config_string("lmstudio", "model", "local-model", "LM Studio model name");
        profile.payload_format = "openai.chat";
        profile.local = true;
        apply_model_override(&profile, model_override);
        return profile;
    }

    if (provider == "copilot")
    {
        profile.name = "copilot";
        profile.endpoint = model_provider_config_string(
            "copilot", "endpoint", "https://api.githubcopilot.com/chat/completions", "GitHub Copilot-compatible endpoint");
        profile.model = model_provider_config_string("copilot", "model", "gpt-4o-copilot", "GitHub Copilot-compatible model name");
        profile.credential_ref = model_provider_config_string(
            "copilot", "token_ref", "", "Copilot credential reference such as env:RASN_COPILOT_TOKEN or file:path");
        profile.token_env = model_provider_config_string(
            "copilot", "token_env", "RASN_COPILOT_TOKEN,GITHUB_COPILOT_TOKEN,COPILOT_TOKEN,GH_TOKEN", "Copilot token environment variables");
        profile.payload_format = "openai.chat";
        profile.local = false;
        profile.headers.push_back("Copilot-Integration-Id: rasn-codepilot");
        profile.headers.push_back("Editor-Version: rASN-CodePilot/0.1");
        profile.headers.push_back("OpenAI-Intent: conversation-panel");
        apply_model_override(&profile, model_override);
        return profile;
    }

    profile.name = provider;
    profile.endpoint =
        model_provider_config_string(provider, "endpoint", "http://localhost:8000/v1/chat/completions", "OpenAI-compatible endpoint");
    profile.model = model_provider_config_string(provider, "model", "local-model", "OpenAI-compatible model name");
    profile.credential_ref =
        model_provider_config_string(provider, "token_ref", "", "credential reference such as env:RASN_API_KEY or file:path");
    profile.token_env = model_provider_config_string(provider, "token_env", "RASN_API_KEY", "environment variable containing the provider token");
    profile.payload_format = "openai.chat";
    profile.local = false;
    apply_model_override(&profile, model_override);
    return profile;
}

std::string describe_model_provider_routing()
{
    return "[rasn.model] provider=simulator|copilot|ollama|llamacpp|lmstudio|openai-compatible "
           "(or compatibility alias [rasn.llm]), endpoint=<url>, model=<model>, "
           "token_ref=env:<env[,env2]>|file:<path>|cmd:<command> or token_env=<env[,env2]>; provider-specific keys such as copilot_endpoint, "
           "ollama_model, llama_cpp_endpoint, and lmstudio_model override shared defaults; "
           "use token_ref, token_env, or token_command for secrets instead of storing tokens in config files";
}

} // namespace rasn
} // namespace dsn
