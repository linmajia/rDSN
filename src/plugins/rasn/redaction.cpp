#include "redaction.h"

#include "rasn_core.h"

#include <dsn/service_api_cpp.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <sstream>

namespace dsn {
namespace rasn {

namespace {

const char kRedactedSecret[] = "<redacted-secret>";

bool config_bool_compat(const std::string &key, bool fallback)
{
    const bool compat_value =
        ::dsn_config_get_value_bool("rasn.codepilot.tools", key.c_str(), fallback, "CodePilot compatibility redaction setting");
    return ::dsn_config_get_value_bool("rasn.policy", key.c_str(), compat_value, "rASN policy redaction setting");
}

uint64_t config_uint64_compat(const std::string &key, uint64_t fallback)
{
    const uint64_t compat_value = ::dsn_config_get_value_uint64(
        "rasn.codepilot.tools", key.c_str(), fallback, "CodePilot compatibility redaction size setting");
    return ::dsn_config_get_value_uint64("rasn.policy", key.c_str(), compat_value, "rASN policy redaction size setting");
}

std::string config_string_compat(const std::string &key, const std::string &fallback)
{
    const char *compat_value = ::dsn_config_get_value_string(
        "rasn.codepilot.tools", key.c_str(), fallback.c_str(), "CodePilot compatibility redaction string setting");
    const std::string compat = compat_value == nullptr ? "" : compat_value;
    const char *policy_value = ::dsn_config_get_value_string(
        "rasn.policy", key.c_str(), compat.empty() ? fallback.c_str() : compat.c_str(), "rASN policy redaction string setting");
    const std::string policy = policy_value == nullptr ? "" : policy_value;
    return policy.empty() ? fallback : policy;
}

std::vector<std::string> split_config_list(const std::string &value)
{
    std::vector<std::string> items;
    std::string current;
    for (const char c : value)
    {
        if (c == ',' || c == ';' || c == '\n' || c == '\r')
        {
            const std::string item = trim(current);
            if (!item.empty())
            {
                items.push_back(item);
            }
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    const std::string item = trim(current);
    if (!item.empty())
    {
        items.push_back(item);
    }
    return items;
}

std::string lower_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool has_suffix(const std::string &value, const std::string &suffix)
{
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_key_char(char c)
{
    const unsigned char ch = static_cast<unsigned char>(c);
    return std::isalnum(ch) || c == '_' || c == '-' || c == '.';
}

std::string normalized_secret_key(std::string key)
{
    key = lower_ascii(key);
    for (char &c : key)
    {
        if (c == '-' || c == '.')
        {
            c = '_';
        }
    }
    return key;
}

bool is_sensitive_key(const std::string &key)
{
    const std::string normalized = normalized_secret_key(key);
    if (normalized == "password" || normalized == "passwd" || normalized == "pwd" ||
        normalized == "secret" || normalized == "api_key" || normalized == "apikey" ||
        normalized == "access_key" || normalized == "private_key" || normalized == "authorization" ||
        normalized == "auth_token" || normalized == "token")
    {
        return true;
    }
    return has_suffix(normalized, "_token") || has_suffix(normalized, "_secret") ||
           has_suffix(normalized, "_password") || has_suffix(normalized, "_api_key");
}

bool is_token_terminator(char c)
{
    const unsigned char ch = static_cast<unsigned char>(c);
    return std::isspace(ch) || c == '"' || c == '\'' || c == ',' || c == ';' || c == '&' ||
           c == '|' || c == '<' || c == '>' || c == '}' || c == ']';
}

bool starts_with_case_insensitive(const std::string &value, size_t pos, const std::string &prefix)
{
    if (pos + prefix.size() > value.size())
    {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(value[pos + i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i])))
        {
            return false;
        }
    }
    return true;
}

// Length of a recognized HTTP auth-scheme keyword at `pos` when it is immediately
// followed by whitespace, else 0. Used so the credential after the scheme is
// redacted instead of just the scheme word -- without this,
// "Authorization: Basic <base64>" would only scrub "Basic" and leak the secret.
size_t auth_scheme_keyword_length(const std::string &text, size_t pos)
{
    static const std::string kSchemes[] = {
        "bearer", "basic", "digest", "negotiate", "ntlm", "token", "apikey"};
    for (const std::string &scheme : kSchemes)
    {
        if (starts_with_case_insensitive(text, pos, scheme))
        {
            const size_t after = pos + scheme.size();
            if (after < text.size() && std::isspace(static_cast<unsigned char>(text[after])))
            {
                return scheme.size();
            }
        }
    }
    return 0;
}

// Process-wide registry of secret values resolved at runtime (e.g. bearer tokens
// loaded from a file: or cmd: credential_ref) that never appear in an environment
// variable. Populated by the provider at token-load time so redaction stays
// consistent regardless of where a secret came from.
std::mutex &runtime_secret_mutex()
{
    static std::mutex mutex;
    return mutex;
}

std::vector<std::string> &runtime_secret_registry()
{
    static std::vector<std::string> values;
    return values;
}

void replace_all(std::string *text, const std::string &needle, const std::string &replacement)
{
    if (needle.empty())
    {
        return;
    }
    size_t pos = 0;
    while ((pos = text->find(needle, pos)) != std::string::npos)
    {
        text->replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
}

void append_unique(std::vector<std::string> *values, const std::string &value)
{
    if (value.empty() || std::find(values->begin(), values->end(), value) != values->end())
    {
        return;
    }
    values->push_back(value);
}

std::vector<std::string> configured_secret_values(size_t min_secret_length)
{
    std::vector<std::string> values;
    for (const std::string &literal : split_config_list(config_string_compat("redact_literal_values", "")))
    {
        if (literal.size() >= min_secret_length)
        {
            append_unique(&values, literal);
        }
    }

    const std::string default_envs =
        "RASN_COPILOT_TOKEN,GITHUB_COPILOT_TOKEN,COPILOT_TOKEN,GH_TOKEN,"
        "OPENAI_API_KEY,ANTHROPIC_API_KEY,AZURE_OPENAI_API_KEY";
    std::vector<std::string> env_names = split_config_list(config_string_compat("redact_env_names", default_envs));
    const std::vector<std::string> model_envs = split_config_list(config_string_compat("token_env", ""));
    env_names.insert(env_names.end(), model_envs.begin(), model_envs.end());

    const char *model_token_env = ::dsn_config_get_value_string(
        "rasn.model", "token_env", "", "rASN model token environment variable names for redaction");
    const std::vector<std::string> canonical_model_envs =
        split_config_list(model_token_env == nullptr ? "" : model_token_env);
    env_names.insert(env_names.end(), canonical_model_envs.begin(), canonical_model_envs.end());

    const char *legacy_token_env = ::dsn_config_get_value_string(
        "rasn.llm", "token_env", "", "rASN legacy model token environment variable names for redaction");
    const std::vector<std::string> legacy_envs = split_config_list(legacy_token_env == nullptr ? "" : legacy_token_env);
    env_names.insert(env_names.end(), legacy_envs.begin(), legacy_envs.end());

    for (const std::string &env_name : env_names)
    {
        const char *env_value = std::getenv(env_name.c_str());
        if (env_value != nullptr)
        {
            const std::string value = env_value;
            if (value.size() >= min_secret_length)
            {
                append_unique(&values, value);
            }
        }
    }

    {
        std::lock_guard<std::mutex> guard(runtime_secret_mutex());
        for (const std::string &secret : runtime_secret_registry())
        {
            if (secret.size() >= min_secret_length)
            {
                append_unique(&values, secret);
            }
        }
    }
    return values;
}

void redact_bearer_tokens(std::string *text)
{
    size_t pos = 0;
    while (pos < text->size())
    {
        if (!starts_with_case_insensitive(*text, pos, "bearer"))
        {
            ++pos;
            continue;
        }
        const bool left_boundary = pos == 0 || !std::isalnum(static_cast<unsigned char>((*text)[pos - 1]));
        size_t value_begin = pos + 6;
        if (!left_boundary || value_begin >= text->size() ||
            !std::isspace(static_cast<unsigned char>((*text)[value_begin])))
        {
            ++pos;
            continue;
        }
        while (value_begin < text->size() && std::isspace(static_cast<unsigned char>((*text)[value_begin])))
        {
            ++value_begin;
        }
        size_t value_end = value_begin;
        while (value_end < text->size() && !is_token_terminator((*text)[value_end]))
        {
            ++value_end;
        }
        if (value_end > value_begin)
        {
            text->replace(value_begin, value_end - value_begin, kRedactedSecret);
            pos = value_begin + sizeof(kRedactedSecret) - 1;
        }
        else
        {
            ++pos;
        }
    }
}

void redact_key_value_secrets(std::string *text)
{
    size_t pos = 0;
    while (pos < text->size())
    {
        const char c = (*text)[pos];
        if (c != '=' && c != ':')
        {
            ++pos;
            continue;
        }

        size_t key_end = pos;
        while (key_end > 0 && std::isspace(static_cast<unsigned char>((*text)[key_end - 1])))
        {
            --key_end;
        }
        if (key_end > 0 && ((*text)[key_end - 1] == '"' || (*text)[key_end - 1] == '\''))
        {
            --key_end;
        }
        size_t key_begin = key_end;
        while (key_begin > 0 && is_key_char((*text)[key_begin - 1]))
        {
            --key_begin;
        }
        const std::string key = text->substr(key_begin, key_end - key_begin);
        if (key.empty() || !is_sensitive_key(key))
        {
            ++pos;
            continue;
        }

        size_t value_begin = pos + 1;
        while (value_begin < text->size() && std::isspace(static_cast<unsigned char>((*text)[value_begin])))
        {
            ++value_begin;
        }

        char quote = 0;
        if (value_begin < text->size() && ((*text)[value_begin] == '"' || (*text)[value_begin] == '\''))
        {
            quote = (*text)[value_begin];
            ++value_begin;
        }

        bool auth_scheme_value = false;
        const size_t scheme_len = auth_scheme_keyword_length(*text, value_begin);
        if (scheme_len != 0)
        {
            size_t scheme_end = value_begin + scheme_len;
            while (scheme_end < text->size() && std::isspace(static_cast<unsigned char>((*text)[scheme_end])))
            {
                ++scheme_end;
            }
            if (scheme_end > value_begin + scheme_len)
            {
                value_begin = scheme_end;
                auth_scheme_value = true;
            }
        }

        size_t value_end = value_begin;
        if (quote != 0)
        {
            // Honor backslash escapes so a value like "ab\"cd" is redacted whole
            // instead of stopping at the escaped quote and leaking the tail.
            while (value_end < text->size() && (*text)[value_end] != quote)
            {
                if ((*text)[value_end] == '\\' && value_end + 1 < text->size())
                {
                    ++value_end;
                }
                ++value_end;
            }
        }
        else if (auth_scheme_value)
        {
            // Authorization-style scheme credentials (Basic/Digest/...) run to the
            // end of the line and may contain spaces, commas, or '=' padding, so
            // redact the whole credential rather than stopping at the first space.
            while (value_end < text->size() && (*text)[value_end] != '\n' && (*text)[value_end] != '\r')
            {
                ++value_end;
            }
        }
        else
        {
            while (value_end < text->size() && !is_token_terminator((*text)[value_end]))
            {
                ++value_end;
            }
        }

        if (value_end > value_begin)
        {
            text->replace(value_begin, value_end - value_begin, kRedactedSecret);
            pos = value_begin + sizeof(kRedactedSecret) - 1;
        }
        else
        {
            ++pos;
        }
    }
}

std::string redact_sensitive_text_internal(const std::string &text,
                                           const std::vector<std::string> &secret_values,
                                           size_t min_secret_length)
{
    std::string redacted = text;
    for (const std::string &secret : secret_values)
    {
        if (secret.size() >= min_secret_length)
        {
            replace_all(&redacted, secret, kRedactedSecret);
        }
    }
    redact_bearer_tokens(&redacted);
    redact_key_value_secrets(&redacted);
    return redacted;
}

} // namespace

std::string redact_sensitive_text(const std::string &text)
{
    if (!config_bool_compat("redaction_enabled", true))
    {
        return text;
    }
    const size_t min_secret_length =
        static_cast<size_t>(config_uint64_compat("redact_min_secret_length", 8));
    return redact_sensitive_text_internal(text, configured_secret_values(min_secret_length), min_secret_length);
}

std::string redact_sensitive_text(const std::string &text, const std::vector<std::string> &secret_values)
{
    return redact_sensitive_text_internal(text, secret_values, 1);
}

void register_runtime_secret(const std::string &secret)
{
    if (secret.empty())
    {
        return;
    }
    std::lock_guard<std::mutex> guard(runtime_secret_mutex());
    std::vector<std::string> &registry = runtime_secret_registry();
    if (std::find(registry.begin(), registry.end(), secret) == registry.end())
    {
        registry.push_back(secret);
    }
}

} // namespace rasn
} // namespace dsn
