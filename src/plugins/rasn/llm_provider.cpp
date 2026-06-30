#include "llm_provider.h"

#include "redaction.h"

#include <dsn/cpp/utils.h>
#include <dsn/service_api_cpp.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace dsn {
namespace rasn {

namespace {

std::string getenv_or(const std::string &name, const std::string &fallback)
{
    const char *value = std::getenv(name.c_str());
    const std::string result = value == nullptr ? "" : value;
    if (result.empty())
    {
        return fallback;
    }
    return result;
}

std::string config_string(const std::string &section,
                          const std::string &key,
                          const std::string &fallback,
                          const std::string &help)
{
    const char *value = ::dsn_config_get_value_string(section.c_str(), key.c_str(), fallback.c_str(), help.c_str());
    const std::string result = value == nullptr ? "" : value;
    if (result.empty())
    {
        return fallback;
    }
    return result;
}

std::string llm_config(const std::string &key, const std::string &fallback, const std::string &help)
{
    const std::string model_value = config_string("rasn.model", key, "", help);
    if (!model_value.empty())
    {
        return model_value;
    }
    return config_string("rasn.llm", key, fallback, help);
}

uint64_t llm_config_uint64(const std::string &key, uint64_t fallback, const std::string &help)
{
    return ::dsn_config_get_value_uint64("rasn.model", key.c_str(), fallback, help.c_str());
}

std::string seconds_from_milliseconds(uint64_t timeout_ms)
{
    if (timeout_ms % 1000 == 0)
    {
        return std::to_string(timeout_ms / 1000);
    }
    std::ostringstream output;
    output << std::fixed << std::setprecision(3) << (static_cast<double>(timeout_ms) / 1000.0);
    return output.str();
}

std::string effective_request_timeout_sec(uint32_t request_timeout_ms)
{
    const uint64_t configured_sec = llm_config_uint64("request_timeout_sec", 120, "provider request timeout");
    if (request_timeout_ms == 0)
    {
        return std::to_string(configured_sec);
    }

    uint64_t effective_ms = request_timeout_ms;
    if (configured_sec != 0)
    {
        const uint64_t configured_ms = configured_sec > (std::numeric_limits<uint64_t>::max() / 1000)
                                           ? std::numeric_limits<uint64_t>::max()
                                           : configured_sec * 1000;
        effective_ms = std::min(effective_ms, configured_ms);
    }
    return seconds_from_milliseconds(effective_ms);
}

std::string provider_config(const std::string &provider,
                            const std::string &key,
                            const std::string &fallback,
                            const std::string &help)
{
    std::string normalized = provider;
    for (char &c : normalized)
    {
        if (c == '.' || c == '-')
        {
            c = '_';
        }
    }

    const std::string provider_key = normalized + "_" + key;
    const std::string specific = llm_config(provider_key, "", help);
    if (!specific.empty())
    {
        return specific;
    }
    return llm_config(key, fallback, help);
}

std::string command_quote(const std::string &value)
{
    std::string quoted = "\"";
    for (const char c : value)
    {
        if (c == '"')
        {
            quoted += "\\\"";
        }
        else
        {
            quoted.push_back(c);
        }
    }
    quoted.push_back('"');
    return quoted;
}

std::string process_id_string()
{
#if defined(_WIN32)
    return std::to_string(_getpid());
#else
    return std::to_string(getpid());
#endif
}

std::string os_temp_root()
{
    const std::string configured = config_string("rasn.runtime", "temp_dir", "", "temporary directory for rASN provider payloads");
    if (!configured.empty() && configured != ".")
    {
        return configured;
    }

    const char *rasn_temp = std::getenv("RASN_TEMP_DIR");
    if (rasn_temp != nullptr && rasn_temp[0] != '\0')
    {
        return rasn_temp;
    }

#if defined(_WIN32)
    const char *tmp = std::getenv("TEMP");
    if (tmp == nullptr || tmp[0] == '\0')
    {
        tmp = std::getenv("TMP");
    }
#else
    const char *tmp = std::getenv("TMPDIR");
#endif
    if (tmp != nullptr && tmp[0] != '\0')
    {
        return tmp;
    }
    return ".";
}

std::string provider_temp_dir()
{
    const std::string root = os_temp_root();
    const std::string dir = ::dsn::utils::filesystem::path_combine(root, "rasn-provider");
    if (!::dsn::utils::filesystem::directory_exists(dir))
    {
        ::dsn::utils::filesystem::create_directory(dir);
    }
    return dir;
}

std::string temp_path(const std::string &prefix, const std::string &extension)
{
    static std::atomic<unsigned long long> counter(0);
    std::ostringstream oss;
    oss << prefix << "-"
        << process_id_string() << "-"
        << std::chrono::high_resolution_clock::now().time_since_epoch().count() << "-"
        << counter.fetch_add(1) << extension;
    return ::dsn::utils::filesystem::path_combine(provider_temp_dir(), oss.str());
}

std::string temp_request_path()
{
    return temp_path("rasn-request", ".json");
}

std::string temp_curl_config_path()
{
    return temp_path("rasn-curl", ".cfg");
}

std::string run_command_capture_stdout(const std::string &command, int *exit_code)
{
    std::array<char, 4096> buffer;
    std::string output;

#if defined(_WIN32)
    FILE *pipe = _popen(command.c_str(), "r");
#else
    FILE *pipe = popen(command.c_str(), "r");
#endif

    if (pipe == nullptr)
    {
        if (exit_code != nullptr)
        {
            *exit_code = -1;
        }
        return "";
    }

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
    {
        output += buffer.data();
    }

#if defined(_WIN32)
    const int rc = _pclose(pipe);
#else
    const int rc = pclose(pipe);
#endif
    if (exit_code != nullptr)
    {
        *exit_code = rc;
    }
    return output;
}

std::string curl_config_escape(const std::string &value)
{
    std::string escaped;
    for (const char c : value)
    {
        if (c == '\\' || c == '"')
        {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }
    return escaped;
}

bool write_curl_config(const std::string &path,
                       const std::string &endpoint,
                       const std::string &payload_path,
                       const std::string &token,
                       const std::vector<std::string> &headers,
                       const std::string &request_timeout_sec,
                       std::string *error)
{
    std::ofstream config(path.c_str(), std::ios::binary);
    if (!config)
    {
        if (error != nullptr)
        {
            *error = "cannot write temporary curl config";
        }
        return false;
    }

    config << "silent\n";
    config << "show-error\n";
    config << "fail-with-body\n";
    config << "location\n";
    config << "connect-timeout = \"" << llm_config_uint64("connect_timeout_sec", 10, "provider connect timeout") << "\"\n";
    config << "max-time = \"" << request_timeout_sec << "\"\n";
    config << "request = \"POST\"\n";
    config << "url = \"" << curl_config_escape(endpoint) << "\"\n";
    config << "header = \"Content-Type: application/json\"\n";
    config << "header = \"User-Agent: rASN-CodePilot\"\n";
    if (!token.empty())
    {
        config << "header = \"Authorization: Bearer " << curl_config_escape(token) << "\"\n";
    }
    for (const std::string &header : headers)
    {
        if (!header.empty())
        {
            config << "header = \"" << curl_config_escape(header) << "\"\n";
        }
    }
    config << "data-binary = \"@" << curl_config_escape(payload_path) << "\"\n";
    return true;
}

std::vector<std::string> split_env_names(const std::string &value)
{
    std::vector<std::string> names;
    std::string current;
    for (const char c : value)
    {
        if (c == ',' || c == ';')
        {
            const std::string item = trim(current);
            if (!item.empty())
            {
                names.push_back(item);
            }
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    const std::string item = trim(current);
    if (!item.empty())
    {
        names.push_back(item);
    }
    return names;
}

std::string token_from_command(const std::string &command)
{
    if (command.empty())
    {
        return "";
    }

    int exit_code = 0;
    const std::string output = run_command_capture_stdout(command, &exit_code);
    if (exit_code != 0)
    {
        return "";
    }
    return trim(output);
}

std::string token_from_file(const std::string &path)
{
    if (path.empty())
    {
        return "";
    }

    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input)
    {
        return "";
    }

    std::ostringstream content;
    content << input.rdbuf();
    return trim(content.str());
}

bool starts_with(const std::string &value, const std::string &prefix)
{
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::string load_bearer_token(const std::string &token_env)
{
    if (token_env.empty())
    {
        return "";
    }

    const std::vector<std::string> env_names = split_env_names(token_env);
    for (const std::string &env_name : env_names)
    {
        const char *token_value = std::getenv(env_name.c_str());
        const std::string token = token_value == nullptr ? "" : token_value;
        if (!token.empty())
        {
            return token;
        }
    }

    for (const std::string &env_name : env_names)
    {
        const std::string specific_command_env = env_name + "_COMMAND";
        std::string command = llm_config(specific_command_env, "", "provider-specific token command");
        if (command.empty())
        {
            command = getenv_or(specific_command_env, "");
        }
        const std::string token = token_from_command(command);
        if (!token.empty())
        {
            return token;
        }
    }

    std::string command = llm_config("token_command", "", "command that prints a provider token");
    if (command.empty())
    {
        command = getenv_or("RASN_TOKEN_COMMAND", "");
    }
    return token_from_command(command);
}

std::string load_bearer_token_ref(const std::string &credential_ref, const std::string &token_env)
{
    const std::string ref = trim(credential_ref);
    if (ref.empty())
    {
        return load_bearer_token(token_env);
    }

    if (starts_with(ref, "env:"))
    {
        return load_bearer_token(trim(ref.substr(4)));
    }
    if (starts_with(ref, "file:"))
    {
        return token_from_file(trim(ref.substr(5)));
    }
    if (starts_with(ref, "cmd:"))
    {
        return token_from_command(trim(ref.substr(4)));
    }
    if (starts_with(ref, "command:"))
    {
        return token_from_command(trim(ref.substr(8)));
    }

    return "";
}

std::string credential_ref_descriptor(const std::string &credential_ref, const std::string &token_env)
{
    const std::string ref = trim(credential_ref);
    if (ref.empty())
    {
        return token_env.empty() ? "" : "env:" + token_env;
    }
    if (starts_with(ref, "cmd:") || starts_with(ref, "command:"))
    {
        return "cmd:<configured>";
    }
    if (starts_with(ref, "env:") || starts_with(ref, "file:"))
    {
        return ref;
    }
    return "<unsupported>";
}

std::string token_command_descriptor(const std::string &credential_ref, const std::string &token_env)
{
    const std::string ref = trim(credential_ref);
    if (starts_with(ref, "cmd:") || starts_with(ref, "command:"))
    {
        return "token_ref:cmd";
    }
    return token_env.empty() ? "" : token_env + "_COMMAND";
}

std::string join_context(const std::vector<std::string> &context)
{
    std::ostringstream oss;
    for (size_t i = 0; i < context.size(); ++i)
    {
        oss << "\n\n--- context " << (i + 1) << " ---\n" << context[i];
    }
    return oss.str();
}

std::string make_chat_payload(const std::string &model, const llm_request &request)
{
    std::ostringstream oss;
    oss << "{\"model\":\"" << json_escape(model)
        << "\",\"messages\":[{\"role\":\"system\",\"content\":\"" << json_escape(request.system_prompt)
        << "\"},{\"role\":\"user\",\"content\":\""
        << json_escape(request.user_prompt + join_context(request.context))
        << "\"}],\"temperature\":0.2,\"stream\":false}";
    return oss.str();
}

std::string make_ollama_payload(const std::string &model, const llm_request &request)
{
    std::ostringstream oss;
    oss << "{\"model\":\"" << json_escape(model)
        << "\",\"prompt\":\"" << json_escape(request.system_prompt + "\n\n" + request.user_prompt + join_context(request.context))
        << "\",\"stream\":false}";
    return oss.str();
}

std::string make_ollama_chat_payload(const std::string &model, const llm_request &request)
{
    std::ostringstream oss;
    oss << "{\"model\":\"" << json_escape(model)
        << "\",\"messages\":[{\"role\":\"system\",\"content\":\"" << json_escape(request.system_prompt)
        << "\"},{\"role\":\"user\",\"content\":\""
        << json_escape(request.user_prompt + join_context(request.context))
        << "\"}],\"stream\":false}";
    return oss.str();
}

std::string extract_json_string(const std::string &json, const std::string &field)
{
    const std::string needle = "\"" + field + "\"";
    const std::string::size_type pos = json.find(needle);
    if (pos == std::string::npos)
    {
        return "";
    }

    const std::string::size_type colon = json.find(':', pos + needle.size());
    if (colon == std::string::npos)
    {
        return "";
    }

    std::string::size_type quote = json.find('"', colon + 1);
    if (quote == std::string::npos)
    {
        return "";
    }

    std::string result;
    bool escaping = false;
    for (++quote; quote < json.size(); ++quote)
    {
        const char c = json[quote];
        if (escaping)
        {
            switch (c)
            {
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            default:
                result.push_back(c);
                break;
            }
            escaping = false;
            continue;
        }
        if (c == '\\')
        {
            escaping = true;
            continue;
        }
        if (c == '"')
        {
            break;
        }
        result.push_back(c);
    }
    return result;
}

class simulator_provider : public llm_provider
{
public:
    simulator_provider() : _model("rasn-random-simulator") {}

    std::string name() const override { return "simulator"; }
    std::string model() const override { return _model; }
    model_provider_descriptor describe() const override
    {
        model_provider_descriptor descriptor;
        descriptor.provider = name();
        descriptor.model = model();
        descriptor.endpoint = "local";
        descriptor.payload_format = "simulator";
        descriptor.local = true;
        descriptor.streaming = true;
        descriptor.health = "healthy";
        return descriptor;
    }

    llm_response complete(const llm_request &request, nucleus_runtime &runtime) override
    {
        agent_task task;
        task.id = request.task_id;
        task.name = "llm.simulator";
        task.input = request.user_prompt;

        for (const std::string &context : request.context)
        {
            if (context.find("Tool ") != std::string::npos)
            {
                llm_response response;
                response.ok = true;
                response.text = "Observed the tool result and used it to continue the coding task.\n\n" +
                                context.substr(0, 360);
                return response;
            }
        }

        const std::string trigger = "force_tool_call:";
        const std::string::size_type trigger_pos = request.user_prompt.find(trigger);
        if (trigger_pos != std::string::npos)
        {
            llm_response response;
            response.ok = true;
            response.text = "RASN_TOOL " + trim(request.user_prompt.substr(trigger_pos + trigger.size()));
            return response;
        }

        static const std::string templates[] = {
            "Plan:\n1. Inspect the relevant files.\n2. Make a small, reversible change.\n3. Build and test the touched surface.",
            "I would split this into runtime, orchestration, state, and observability tasks before editing code.",
            "Suggested patch strategy: capture inputs, record nondeterministic choices, then replay the trace to debug failures.",
            "The safest next step is to produce a minimal implementation, run a targeted build, and keep the event log enabled."};

        const std::string choice = runtime.resolve_nondeterminism(task, "simulator.response", "local-random", []() {
            static std::mt19937 rng(static_cast<unsigned int>(std::time(nullptr)));
            std::uniform_int_distribution<int> dist(0, 3);
            return std::to_string(dist(rng));
        });

        int index = std::atoi(choice.c_str());
        if (index < 0 || index > 3)
        {
            index = 0;
        }

        llm_response response;
        response.ok = true;
        response.text = std::string(templates[index]) + "\n\nPrompt digest: " + request.user_prompt.substr(0, 180);
        return response;
    }

private:
    std::string _model;
};

class curl_chat_provider : public llm_provider
{
public:
    curl_chat_provider(const std::string &provider_name,
                       const std::string &endpoint,
                       const std::string &model,
                       const std::string &token_env,
                       const std::string &credential_ref,
                       const std::string &payload_format,
                       bool local,
                       const std::vector<std::string> &headers)
        : _provider_name(provider_name),
          _endpoint(endpoint),
          _model(model),
          _token_env(token_env),
          _credential_ref(credential_ref),
          _payload_format(payload_format),
          _local(local),
          _headers(headers)
    {
    }

    std::string name() const override { return _provider_name; }
    std::string model() const override { return _model; }
    model_provider_descriptor describe() const override
    {
        model_provider_descriptor descriptor;
        descriptor.provider = _provider_name;
        descriptor.model = _model;
        descriptor.endpoint = _endpoint;
        descriptor.payload_format = _payload_format;
        descriptor.token_env = _token_env;
        descriptor.token_command_ref = token_command_descriptor(_credential_ref, _token_env);
        descriptor.credential_ref = credential_ref_descriptor(_credential_ref, _token_env);
        descriptor.token_required = !_token_env.empty() || !_credential_ref.empty();
        descriptor.local = _local;
        descriptor.health = "configured";
        return descriptor;
    }

    llm_response complete(const llm_request &request, nucleus_runtime &runtime) override
    {
        agent_task task;
        task.id = request.task_id;
        task.name = "llm." + _provider_name;
        task.input = request.user_prompt;
        runtime.record_llm_request(task, _provider_name, _model);

        std::string payload;
        if (_payload_format == "ollama.generate")
        {
            payload = make_ollama_payload(_model, request);
        }
        else if (_payload_format == "ollama.chat")
        {
            payload = make_ollama_chat_payload(_model, request);
        }
        else
        {
            payload = make_chat_payload(_model, request);
        }
        const std::string path = temp_request_path();

        {
            std::ofstream file(path.c_str(), std::ios::binary);
            if (!file)
            {
                llm_response response;
                response.ok = false;
                response.error = "cannot write temporary request payload: " + path;
                return response;
            }
            file << payload;
        }

        const std::string config_path = temp_curl_config_path();
        const std::string request_timeout_sec = effective_request_timeout_sec(request.timeout_ms);
        const std::string token = load_bearer_token_ref(_credential_ref, _token_env);
        if (!_credential_ref.empty() && token.empty())
        {
            ::dsn::utils::filesystem::remove_path(path);
            llm_response response;
            response.ok = false;
            response.error = "configured credential_ref did not produce a bearer token";
            runtime.record_failure(task, "provider", "llm_provider_credential_error", response.error, false, _provider_name);
            return response;
        }
        std::string config_error;
        if (!write_curl_config(
                config_path, _endpoint, path, token, _headers, request_timeout_sec, &config_error))
        {
            ::dsn::utils::filesystem::remove_path(path);
            llm_response response;
            response.ok = false;
            response.error = config_error;
            return response;
        }

        std::ostringstream command;
        command << "curl --config " << command_quote(config_path);

        int exit_code = 0;
        const std::string output = run_command_capture_stdout(command.str(), &exit_code);
        ::dsn::utils::filesystem::remove_path(config_path);
        ::dsn::utils::filesystem::remove_path(path);

        llm_response response;
        if (exit_code != 0)
        {
            const bool timed_out = exit_code == 28;
            response.ok = false;
            response.error = timed_out
                                 ? "llm provider request timed out after " + request_timeout_sec + " seconds: " + output
                                 : "curl failed with exit code " + std::to_string(exit_code) + ": " + output;
            runtime.record_failure(task,
                                   "provider",
                                   timed_out ? "llm_provider_timeout" : "llm_provider_error",
                                   response.error,
                                   true,
                                   _provider_name);
            return response;
        }

        std::string text = _payload_format == "ollama.generate" ? extract_json_string(output, "response")
                                                                 : extract_json_string(output, "content");
        if (text.empty())
        {
            text = output;
        }

        response.ok = true;
        response.text = trim(text);
        runtime.record_llm_response(task, _provider_name, response.text);
        return response;
    }

private:
    std::string _provider_name;
    std::string _endpoint;
    std::string _model;
    std::string _token_env;
    std::string _credential_ref;
    std::string _payload_format;
    bool _local;
    std::vector<std::string> _headers;
};

struct provider_profile
{
    std::string name;
    std::string endpoint;
    std::string model;
    std::string token_env;
    std::string credential_ref;
    std::string payload_format;
    bool local;
    std::vector<std::string> headers;
};

std::unique_ptr<llm_provider> make_profile_provider(const provider_profile &profile)
{
    return std::unique_ptr<llm_provider>(new curl_chat_provider(profile.name,
                                                                profile.endpoint,
                                                                profile.model,
                                                                profile.token_env,
                                                                profile.credential_ref,
                                                                profile.payload_format,
                                                                profile.local,
                                                                profile.headers));
}

} // namespace

void emit_llm_stream_chunks(const agent_task &task,
                            const std::string &provider,
                            const std::string &text,
                            nucleus_runtime &runtime,
                            const llm_stream_callback &on_chunk,
                            size_t chunk_bytes)
{
    const size_t configured_chunk_bytes =
        static_cast<size_t>(llm_config_uint64("stream_chunk_bytes", 96, "model response stream chunk size"));
    const size_t effective_chunk_bytes = chunk_bytes == 0 ? std::max<size_t>(1, configured_chunk_bytes)
                                                         : std::max<size_t>(1, chunk_bytes);
    if (text.empty())
    {
        return;
    }

    size_t chunk_index = 0;
    for (size_t offset = 0; offset < text.size(); offset += effective_chunk_bytes)
    {
        const std::string redacted_chunk = redact_sensitive_text(text.substr(offset, effective_chunk_bytes));
        runtime.record_llm_response_chunk(task, provider, chunk_index, redacted_chunk);
        if (on_chunk)
        {
            on_chunk(redacted_chunk);
        }
        ++chunk_index;
    }
}

llm_response
llm_provider::complete_streaming(const llm_request &request, nucleus_runtime &runtime, const llm_stream_callback &on_chunk)
{
    const llm_response response = complete(request, runtime);
    if (response.ok)
    {
        agent_task task;
        task.id = request.task_id;
        task.name = "llm.stream";
        task.input = request.user_prompt;
        emit_llm_stream_chunks(task, name(), response.text, runtime, on_chunk);
    }
    return response;
}

std::unique_ptr<llm_provider> create_provider_from_environment()
{
    return create_provider(llm_config("provider", "simulator", "LLM provider name"));
}

std::unique_ptr<llm_provider> create_provider(const std::string &provider_name)
{
    const std::string provider = trim(provider_name.empty() ? "simulator" : provider_name);

    if (provider == "simulator" || provider == "mock" || provider == "random")
    {
        return std::unique_ptr<llm_provider>(new simulator_provider());
    }

    if (provider == "ollama")
    {
        provider_profile profile;
        profile.name = "ollama";
        profile.endpoint = provider_config("ollama", "endpoint", "http://localhost:11434/api/chat", "Ollama endpoint");
        profile.model = provider_config("ollama", "model", "llama3.1", "Ollama model name");
        profile.payload_format = provider_config("ollama", "payload", "ollama.chat", "Ollama payload format");
        profile.local = true;
        return make_profile_provider(profile);
    }

    if (provider == "llamacpp" || provider == "llama.cpp")
    {
        provider_profile profile;
        profile.name = "llama.cpp";
        profile.endpoint = provider_config("llama_cpp", "endpoint", "http://localhost:8080/v1/chat/completions", "llama.cpp endpoint");
        profile.model = provider_config("llama_cpp", "model", "local-model", "llama.cpp model name");
        profile.payload_format = "openai.chat";
        profile.local = true;
        return make_profile_provider(profile);
    }

    if (provider == "lmstudio" || provider == "lm-studio")
    {
        provider_profile profile;
        profile.name = "lmstudio";
        profile.endpoint = provider_config("lmstudio", "endpoint", "http://localhost:1234/v1/chat/completions", "LM Studio endpoint");
        profile.model = provider_config("lmstudio", "model", "local-model", "LM Studio model name");
        profile.payload_format = "openai.chat";
        profile.local = true;
        return make_profile_provider(profile);
    }

    if (provider == "copilot" || provider == "github-copilot")
    {
        provider_profile profile;
        profile.name = "copilot";
        profile.endpoint = provider_config("copilot", "endpoint", "https://api.githubcopilot.com/chat/completions", "GitHub Copilot-compatible endpoint");
        profile.model = provider_config("copilot", "model", "gpt-4o-copilot", "GitHub Copilot-compatible model name");
        profile.credential_ref = provider_config(
            "copilot", "token_ref", "", "Copilot credential reference such as env:RASN_COPILOT_TOKEN or file:path");
        profile.token_env = provider_config(
            "copilot", "token_env", "RASN_COPILOT_TOKEN,GITHUB_COPILOT_TOKEN,COPILOT_TOKEN,GH_TOKEN", "Copilot token environment variables");
        profile.payload_format = "openai.chat";
        profile.local = false;
        profile.headers.push_back("Copilot-Integration-Id: rasn-codepilot");
        profile.headers.push_back("Editor-Version: rASN-CodePilot/0.1");
        profile.headers.push_back("OpenAI-Intent: conversation-panel");
        return make_profile_provider(profile);
    }

    provider_profile profile;
    profile.name = provider;
    profile.endpoint = provider_config(provider, "endpoint", "http://localhost:8000/v1/chat/completions", "OpenAI-compatible endpoint");
    profile.model = provider_config(provider, "model", "local-model", "OpenAI-compatible model name");
    profile.credential_ref = provider_config(provider, "token_ref", "", "credential reference such as env:RASN_API_KEY or file:path");
    profile.token_env = provider_config(provider, "token_env", "RASN_API_KEY", "environment variable containing the provider token");
    profile.payload_format = "openai.chat";
    profile.local = false;
    return make_profile_provider(profile);
}

std::string describe_provider_environment()
{
    return "[rasn.model] provider=simulator|copilot|ollama|llamacpp|lmstudio|openai-compatible "
           "(or compatibility alias [rasn.llm]), endpoint=<url>, model=<model>, "
           "token_ref=env:<env[,env2]>|file:<path>|cmd:<command> or token_env=<env[,env2]>; provider-specific keys such as copilot_endpoint, "
           "ollama_model, llama_cpp_endpoint, and lmstudio_model override shared defaults; "
           "use token_ref, token_env, or token_command for secrets instead of storing tokens in config files";
}

} // namespace rasn
} // namespace dsn
