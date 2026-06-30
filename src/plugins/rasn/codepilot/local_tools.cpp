#include "local_tools.h"

#include "../policy_manager.h"

#include <dsn/cpp/utils.h>
#include <dsn/service_api_cpp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dsn {
namespace rasn {

namespace {

std::string join_tool_args(const std::vector<std::string> &args, size_t begin)
{
    std::ostringstream oss;
    for (size_t i = begin; i < args.size(); ++i)
    {
        if (i != begin)
        {
            oss << " ";
        }
        oss << args[i];
    }
    return oss.str();
}

bool parse_size(const std::string &value, size_t *parsed)
{
    char *end = nullptr;
    const unsigned long result = std::strtoul(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0')
    {
        return false;
    }
    *parsed = static_cast<size_t>(result);
    return true;
}

std::string config_string_compat(const std::string &key, const std::string &fallback)
{
    const char *compat_value = ::dsn_config_get_value_string(
        "rasn.codepilot.tools", key.c_str(), fallback.c_str(), "CodePilot compatibility tool setting");
    const std::string compat = compat_value == nullptr ? "" : compat_value;
    const char *policy_value =
        ::dsn_config_get_value_string("rasn.policy", key.c_str(), compat.empty() ? fallback.c_str() : compat.c_str(), "rASN policy tool setting");
    const std::string policy = policy_value == nullptr ? "" : policy_value;
    return policy.empty() ? fallback : policy;
}

uint64_t config_uint64_compat(const std::string &key, uint64_t fallback)
{
    const uint64_t compat_value = ::dsn_config_get_value_uint64(
        "rasn.codepilot.tools", key.c_str(), fallback, "CodePilot compatibility tool numeric setting");
    return ::dsn_config_get_value_uint64(
        "rasn.policy", key.c_str(), compat_value, "rASN policy tool numeric setting");
}

std::vector<std::string> split_config_list(const std::string &value)
{
    std::vector<std::string> items;
    std::string current;
    std::istringstream input(value);
    while (std::getline(input, current, ','))
    {
        current = trim(current);
        if (!current.empty())
        {
            items.push_back(current);
        }
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

std::string canonical_command_name(std::string command)
{
    command = normalize_platform_path(command);
    command = ::dsn::utils::filesystem::get_file_name(command);
#if defined(_WIN32)
    command = lower_ascii(command);
    if (has_suffix(command, ".exe") || has_suffix(command, ".cmd") || has_suffix(command, ".bat") ||
        has_suffix(command, ".ps1"))
    {
        command = command.substr(0, command.find_last_of('.'));
    }
#endif
    return command;
}

bool command_contains_shell_metacharacter(const std::string &command)
{
    return command.find_first_of("&|;<>`") != std::string::npos || command.find('\n') != std::string::npos ||
           command.find('\r') != std::string::npos;
}

std::string quote_shell_path(const std::string &path)
{
#if defined(_WIN32)
    std::string quoted = "\"";
    for (char c : path)
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
    quoted += "\"";
    return quoted;
#else
    std::string quoted = "'";
    for (char c : path)
    {
        if (c == '\'')
        {
            quoted += "'\\''";
        }
        else
        {
            quoted.push_back(c);
        }
    }
    quoted += "'";
    return quoted;
#endif
}

void replace_all(std::string *value, const std::string &placeholder, const std::string &replacement)
{
    std::string::size_type pos = 0;
    while ((pos = value->find(placeholder, pos)) != std::string::npos)
    {
        value->replace(pos, placeholder.size(), replacement);
        pos += replacement.size();
    }
}

std::string absolute_or_normalized_path(const std::string &path)
{
    std::string normalized = normalize_platform_path(path.empty() ? "." : path);
    std::string absolute;
    if (::dsn::utils::filesystem::get_absolute_path(normalized, absolute))
    {
        normalized = absolute;
    }
    return normalize_platform_path(normalized);
}

bool read_text_prefix(const std::string &path, size_t max_bytes, std::string *content, std::string *error)
{
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input)
    {
        if (error != nullptr)
        {
            *error = "cannot open file: " + path;
        }
        return false;
    }

    std::vector<char> buffer(max_bytes + 1);
    input.read(buffer.data(), static_cast<std::streamsize>(max_bytes));
    const std::streamsize count = input.gcount();
    content->assign(buffer.data(), static_cast<size_t>(count));
    if (input.peek() != EOF)
    {
        *content += "\n\n[truncated at " + std::to_string(max_bytes) + " bytes]";
    }
    return true;
}

bool should_skip_path(const std::string &path)
{
    std::string component;
    for (char ch : path)
    {
        if (ch == '\\' || ch == '/')
        {
            if (component == ".git" || component == "builder" || component == "builder-rasn")
            {
                return true;
            }
            component.clear();
            continue;
        }
        component.push_back(ch);
    }
    return component == ".git" || component == "builder" || component == "builder-rasn";
}

void list_directory_entries(const std::string &path, std::vector<std::string> *entries)
{
    std::vector<std::string> files;
    std::vector<std::string> directories;
    ::dsn::utils::filesystem::get_subfiles(path, files, false);
    ::dsn::utils::filesystem::get_subdirectories(path, directories, false);

    for (const std::string &file : files)
    {
        entries->push_back(::dsn::utils::filesystem::get_file_name(file));
    }
    for (const std::string &directory : directories)
    {
        entries->push_back(::dsn::utils::filesystem::get_file_name(directory) + "\\");
    }
}

void collect_files(const std::string &path, std::vector<std::string> *files)
{
    if (::dsn::utils::filesystem::file_exists(path))
    {
        files->push_back(path);
        return;
    }

    if (!::dsn::utils::filesystem::directory_exists(path))
    {
        return;
    }

    std::vector<std::string> discovered;
    if (!::dsn::utils::filesystem::get_subfiles(path, discovered, true))
    {
        return;
    }

    for (const std::string &file : discovered)
    {
        if (files->size() >= 10000)
        {
            break;
        }
        if (!should_skip_path(file))
        {
            files->push_back(file);
        }
    }
}

std::string filesystem_snapshot_path(const std::string &path)
{
    std::string normalized = normalize_platform_path(path.empty() ? "." : path);
    std::string absolute;
    if (::dsn::utils::filesystem::get_absolute_path(normalized, absolute))
    {
        normalized = absolute;
    }
    std::string canonical;
    if (::dsn::utils::filesystem::get_normalized_path(normalized, canonical) == 0 && !canonical.empty())
    {
        normalized = canonical;
    }
    return normalize_platform_path(normalized);
}

std::string file_content_fingerprint(const std::string &path)
{
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input)
    {
        return "missing";
    }

    uint64_t hash = 1469598103934665603ull;
    uint64_t bytes = 0;
    char c = 0;
    while (input.get(c))
    {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
        ++bytes;
    }

    std::ostringstream output;
    output << "file bytes=" << bytes << " fnv1a=0x"
           << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}

std::string immediate_directory_fingerprint(const std::string &path)
{
    if (!::dsn::utils::filesystem::directory_exists(path))
    {
        return "missing";
    }
    std::vector<std::string> entries;
    list_directory_entries(path, &entries);
    std::sort(entries.begin(), entries.end());

    std::ostringstream output;
    output << "directory entries=" << entries.size();
    for (const std::string &entry : entries)
    {
        output << "\n" << entry;
    }
    return output.str();
}

std::string recursive_search_fingerprint(const std::string &path)
{
    std::vector<std::string> files;
    collect_files(path, &files);
    std::sort(files.begin(), files.end());

    std::ostringstream output;
    output << "search files=" << files.size();
    for (const std::string &file : files)
    {
        output << "\n" << filesystem_snapshot_path(file) << " " << file_content_fingerprint(file);
    }
    return output.str();
}

std::string filesystem_snapshot_for_tool(const std::string &name, const std::vector<std::string> &args)
{
    if (name != "list" && name != "read" && name != "search")
    {
        return "";
    }

    const std::string path = filesystem_snapshot_path(args.empty() ? "." : args[0]);
    std::ostringstream output;
    output << "tool=" << name << "\npath=" << path << "\n";
    if (name == "list")
    {
        output << immediate_directory_fingerprint(path);
    }
    else if (name == "read")
    {
        output << file_content_fingerprint(path);
    }
    else
    {
        output << recursive_search_fingerprint(path);
    }
    return output.str();
}

std::string filesystem_snapshot_key_for_tool(const std::string &name, const std::vector<std::string> &args)
{
    if (name != "list" && name != "read" && name != "search")
    {
        return "";
    }
    return name + ":" + filesystem_snapshot_path(args.empty() ? "." : args[0]);
}

#if defined(_WIN32)
void drain_pipe(HANDLE read_pipe, std::string *output)
{
    std::array<char, 4096> buffer;
    for (;;)
    {
        DWORD available = 0;
        if (!PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0)
        {
            return;
        }

        DWORD read = 0;
        const DWORD to_read = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
        if (!ReadFile(read_pipe, buffer.data(), to_read, &read, nullptr) || read == 0)
        {
            return;
        }
        output->append(buffer.data(), static_cast<size_t>(read));
    }
}
#endif

std::string run_command_capture_stdout_and_stderr(const std::string &command,
                                                  uint64_t timeout_ms,
                                                  bool *timed_out,
                                                  int *exit_code)
{
    std::string output;
    if (timed_out != nullptr)
    {
        *timed_out = false;
    }

#if defined(_WIN32)
    SECURITY_ATTRIBUTES security;
    security.nLength = sizeof(security);
    security.lpSecurityDescriptor = nullptr;
    security.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0))
    {
        if (exit_code != nullptr)
        {
            *exit_code = -1;
        }
        return output;
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA startup;
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;

    PROCESS_INFORMATION process;
    ZeroMemory(&process, sizeof(process));

    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    if (job != nullptr)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
        ZeroMemory(&limits, sizeof(limits));
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        {
            CloseHandle(job);
            job = nullptr;
        }
    }

    std::string command_line = "cmd.exe /C " + command;
    const BOOL created = CreateProcessA(nullptr,
                                        &command_line[0],
                                        nullptr,
                                        nullptr,
                                        TRUE,
                                        CREATE_NO_WINDOW,
                                        nullptr,
                                        nullptr,
                                        &startup,
                                        &process);
    CloseHandle(write_pipe);

    if (!created)
    {
        if (job != nullptr)
        {
            CloseHandle(job);
        }
        CloseHandle(read_pipe);
        if (exit_code != nullptr)
        {
            *exit_code = -1;
        }
        return output;
    }
    if (job != nullptr && !AssignProcessToJobObject(job, process.hProcess))
    {
        CloseHandle(job);
        job = nullptr;
    }

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    for (;;)
    {
        drain_pipe(read_pipe, &output);
        const DWORD wait = WaitForSingleObject(process.hProcess, 20);
        if (wait == WAIT_OBJECT_0)
        {
            drain_pipe(read_pipe, &output);
            DWORD code = 0;
            if (GetExitCodeProcess(process.hProcess, &code) && exit_code != nullptr)
            {
                *exit_code = static_cast<int>(code);
            }
            break;
        }
        if (wait == WAIT_FAILED)
        {
            if (exit_code != nullptr)
            {
                *exit_code = -1;
            }
            break;
        }

        if (timeout_ms != 0)
        {
            const uint64_t elapsed_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
            if (elapsed_ms >= timeout_ms)
            {
                if (job != nullptr)
                {
                    TerminateJobObject(job, 1);
                }
                else
                {
                    TerminateProcess(process.hProcess, 1);
                }
                WaitForSingleObject(process.hProcess, INFINITE);
                drain_pipe(read_pipe, &output);
                if (timed_out != nullptr)
                {
                    *timed_out = true;
                }
                if (exit_code != nullptr)
                {
                    *exit_code = -1;
                }
                break;
            }
        }
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (job != nullptr)
    {
        CloseHandle(job);
    }
    CloseHandle(read_pipe);
#else
    std::array<char, 4096> buffer;
    const std::string redirected = command + " 2>&1";
    FILE *pipe = popen(redirected.c_str(), "r");
    if (pipe == nullptr)
    {
        if (exit_code != nullptr)
        {
            *exit_code = -1;
        }
        return output;
    }

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
    {
        output += buffer.data();
    }

    const int rc = pclose(pipe);
    if (exit_code != nullptr)
    {
        *exit_code = rc;
    }
#endif
    return output;
}

tool_result make_tool_result(bool ok, const std::string &output, const std::string &error)
{
    tool_result result;
    result.ok = ok;
    result.output = output;
    result.error = error;
    return result;
}

tool_argument_descriptor tool_argument(const std::string &name, bool required, const std::string &description)
{
    tool_argument_descriptor argument;
    argument.name = name;
    argument.required = required;
    argument.description = description;
    return argument;
}

tool_descriptor tool_schema(const std::string &name,
                            const std::string &side_effect,
                            const std::string &description,
                            const std::vector<tool_argument_descriptor> &arguments)
{
    tool_descriptor descriptor;
    descriptor.name = name;
    descriptor.side_effect = side_effect;
    descriptor.description = description;
    descriptor.arguments = arguments;
    return descriptor;
}

bool known_tool_name(const std::string &name)
{
    return name == "list" || name == "read" || name == "search" || name == "write" || name == "replace" ||
           name == "shell";
}

} // namespace

codepilot_tool_provider::codepilot_tool_provider() : _max_read_bytes(20000), _max_search_matches(200) {}

bool codepilot_shell_command_allowed(const std::string &command,
                                     const std::vector<std::string> &allowed_commands,
                                     std::string *error)
{
    if (allowed_commands.empty())
    {
        return true;
    }
    if (command_contains_shell_metacharacter(command))
    {
        if (error != nullptr)
        {
            *error = "shell command uses metacharacters while shell_allowed_commands is active";
        }
        return false;
    }

    const std::vector<std::string> words = split_words(command);
    if (words.empty())
    {
        if (error != nullptr)
        {
            *error = "shell command is empty";
        }
        return false;
    }

    const std::string executable = canonical_command_name(words[0]);
    for (const std::string &allowed : allowed_commands)
    {
        if (executable == canonical_command_name(allowed))
        {
            return true;
        }
    }

    if (error != nullptr)
    {
        *error = "shell command is not in shell_allowed_commands: " + executable;
    }
    return false;
}

std::string codepilot_shell_command_with_working_directory(const std::string &command,
                                                           const std::string &working_directory)
{
    if (working_directory.empty())
    {
        return command;
    }
#if defined(_WIN32)
    return "cd /d " + quote_shell_path(normalize_platform_path(working_directory)) + " && " + command;
#else
    return "cd " + quote_shell_path(normalize_platform_path(working_directory)) + " && " + command;
#endif
}

std::string codepilot_shell_command_with_container_template(const std::string &command,
                                                            const std::string &working_directory,
                                                            const std::string &template_command,
                                                            std::string *error)
{
    if (template_command.empty())
    {
        if (error != nullptr)
        {
            *error = "rasn.policy.shell_executor=container requires shell_container_template";
        }
        return "";
    }
    if (template_command.find("{command}") == std::string::npos &&
        template_command.find("{raw_command}") == std::string::npos)
    {
        if (error != nullptr)
        {
            *error = "shell_container_template must include {command} or {raw_command}";
        }
        return "";
    }

    std::string wrapped = template_command;
    const std::string workspace = absolute_or_normalized_path(working_directory);
    replace_all(&wrapped, "{command}", quote_shell_path(command));
    replace_all(&wrapped, "{raw_command}", command);
    replace_all(&wrapped, "{workspace}", quote_shell_path(workspace));
    replace_all(&wrapped, "{raw_workspace}", workspace);
    if (error != nullptr)
    {
        error->clear();
    }
    return wrapped;
}

tool_result codepilot_run_shell_command(const std::string &command, uint64_t timeout_ms)
{
    int exit_code = 0;
    bool timed_out = false;
    const std::string output = run_command_capture_stdout_and_stderr(command, timeout_ms, &timed_out, &exit_code);
    if (timed_out)
    {
        return make_tool_result(false,
                                output,
                                "shell command timed out after " + std::to_string(timeout_ms) + " ms");
    }
    if (exit_code != 0)
    {
        return make_tool_result(false, output, "shell command failed with exit code " + std::to_string(exit_code));
    }
    return make_tool_result(true, output, "");
}

bool codepilot_write_file_atomically(const std::string &path, const std::string &content, std::string *error)
{
    if (path.empty())
    {
        if (error != nullptr)
        {
            *error = "cannot write file with empty path";
        }
        return false;
    }

    const std::string parent = ::dsn::utils::filesystem::remove_file_name(path);
    if (!parent.empty() && !::dsn::utils::filesystem::directory_exists(parent) &&
        !::dsn::utils::filesystem::create_directory(parent))
    {
        if (error != nullptr)
        {
            *error = "cannot create parent directory: " + parent;
        }
        return false;
    }

    const std::string file_name = ::dsn::utils::filesystem::get_file_name(path);
    const std::string temp_name = "." + (file_name.empty() ? "codepilot-write" : file_name) + ".tmp." + make_trace_id();
    const std::string temp_path = parent.empty() ? temp_name : ::dsn::utils::filesystem::path_combine(parent, temp_name);

    std::ofstream output(temp_path.c_str(), std::ios::binary | std::ios::trunc);
    if (!output)
    {
        if (error != nullptr)
        {
            *error = "cannot open temporary file: " + temp_path;
        }
        return false;
    }
    output << content;
    output.close();
    if (!output)
    {
        ::dsn::utils::filesystem::remove_path(temp_path);
        if (error != nullptr)
        {
            *error = "cannot flush temporary file: " + temp_path;
        }
        return false;
    }

    if (!::dsn::utils::filesystem::rename_path(temp_path, path))
    {
        ::dsn::utils::filesystem::remove_path(temp_path);
        if (error != nullptr)
        {
            *error = "cannot atomically replace file: " + path;
        }
        return false;
    }
    return true;
}

std::string codepilot_tool_provider::describe_tools() const
{
    std::ostringstream output;
    output << "Available tools:\n";
    for (const tool_descriptor &tool : describe_tool_schemas())
    {
        output << "- " << tool.name;
        for (const tool_argument_descriptor &argument : tool.arguments)
        {
            output << " <" << argument.name << (argument.required ? "" : "?") << ">";
        }
        output << ": " << tool.description << " [" << tool.side_effect << "]\n";
    }
    return output.str();
}

std::vector<tool_descriptor> codepilot_tool_provider::describe_tool_schemas() const
{
    std::vector<tool_descriptor> tools;
    tools.push_back(tool_schema("list",
                                "read_only",
                                "List immediate files and directories.",
                                std::vector<tool_argument_descriptor>{tool_argument("path", false, "Directory to list; defaults to current directory.")}));
    tools.push_back(tool_schema("read",
                                "read_only",
                                "Read a bounded file prefix.",
                                std::vector<tool_argument_descriptor>{tool_argument("path", true, "File to read."),
                                                                       tool_argument("max_bytes", false, "Maximum bytes to read.")}));
    tools.push_back(tool_schema("search",
                                "read_only",
                                "Recursively search text files for a substring.",
                                std::vector<tool_argument_descriptor>{tool_argument("path", true, "File or directory to search."),
                                                                       tool_argument("text", true, "Substring to find.")}));
    tools.push_back(tool_schema("write",
                                "write",
                                "Atomically write a file when policy allows write side effects.",
                                std::vector<tool_argument_descriptor>{tool_argument("path", true, "File to write."),
                                                                       tool_argument("content", true, "Content to write.")}));
    tools.push_back(tool_schema("replace",
                                "write",
                                "Atomically replace the first text occurrence when policy allows write side effects.",
                                std::vector<tool_argument_descriptor>{tool_argument("path", true, "File to edit."),
                                                                       tool_argument("old", true, "Text to replace."),
                                                                       tool_argument("new", true, "Replacement text.")}));
    tools.push_back(tool_schema("shell",
                                "shell",
                                "Run a local command only when policy, approval, and shell sandbox controls allow it.",
                                std::vector<tool_argument_descriptor>{tool_argument("command", true, "Command and arguments to execute.")}));
    return tools;
}

tool_result codepilot_tool_provider::run(const std::string &name,
                                     const std::vector<std::string> &args,
                                     nucleus_runtime &runtime,
                                     const agent_task &task) const
{
    return run_checked(name, args, std::vector<std::string>(), runtime, task);
}

tool_result codepilot_tool_provider::run_with_policy_labels(const std::string &name,
                                                        const std::vector<std::string> &args,
                                                        const std::vector<std::string> &policy_labels,
                                                        nucleus_runtime &runtime,
                                                        const agent_task &task) const
{
    return run_checked(name, args, policy_labels, runtime, task);
}

tool_result codepilot_tool_provider::run_checked(const std::string &name,
                                             const std::vector<std::string> &args,
                                             const std::vector<std::string> &policy_labels,
                                             nucleus_runtime &runtime,
                                             const agent_task &task) const
{
    if (!known_tool_name(name))
    {
        return make_tool_result(false, "", "unknown tool: " + name);
    }

    const std::string arguments = join_tool_args(args, 0);
    const std::string snapshot_key = filesystem_snapshot_key_for_tool(name, args);
    if (!snapshot_key.empty())
    {
        const std::string snapshot = filesystem_snapshot_for_tool(name, args);
        std::string snapshot_error;
        if (!runtime.replay_filesystem_snapshot(task, snapshot_key, snapshot, &snapshot_error))
        {
            runtime.record_failure(task, "replay", "filesystem_snapshot_mismatch", snapshot_error, false, "codepilot.tool");
            return make_tool_result(false, "", snapshot_error);
        }
        runtime.record_filesystem_snapshot(task, snapshot_key, snapshot);
    }

    bool replay_ok = false;
    std::string replay_result;
    if (runtime.replay_tool_call(task, name, arguments, &replay_ok, &replay_result))
    {
        return replay_ok ? make_tool_result(true, replay_result, "") : make_tool_result(false, "", replay_result);
    }
    if (runtime.replay_enabled() && classify_tool_side_effect(name) != tool_side_effect::read_only)
    {
        const std::string error = "replay missing recorded side-effect tool result";
        runtime.record_failure(task, "replay", "missing_tool_result", error, false, "codepilot.tool");
        return make_tool_result(false, "", error);
    }

    const policy_request policy = make_policy_request(name, args, task, policy_labels);
    const policy_decision decision = global_policy_manager().evaluate(policy);
    if (!decision.allowed)
    {
        runtime.record_failure(
            task, "policy", "tool_denied", "policy denied tool '" + name + "': " + decision.reason, false, "codepilot.tool");
        return make_tool_result(false, "", "policy denied tool '" + name + "': " + decision.reason);
    }

    tool_result result;
    if (name == "list")
    {
        result = run_list(args);
    }
    else if (name == "read")
    {
        result = run_read(args);
    }
    else if (name == "search")
    {
        result = run_search(args);
    }
    else if (name == "write")
    {
        result = run_write(args);
    }
    else if (name == "replace")
    {
        result = run_replace(args);
    }
    else if (name == "shell")
    {
        result = run_shell(args);
    }
    return global_policy_manager().apply_tool_output_bounds(name, task, result);
}

tool_result codepilot_tool_provider::run_list(const std::vector<std::string> &args) const
{
    const std::string path = normalize_platform_path(args.empty() ? "." : args[0]);
    std::vector<std::string> entries;
    list_directory_entries(path, &entries);
    if (entries.empty() && !::dsn::utils::filesystem::directory_exists(path))
    {
        return make_tool_result(false, "", "not a directory or cannot list: " + path);
    }

    std::sort(entries.begin(), entries.end());
    std::ostringstream output;
    output << "listing " << path << "\n";
    for (size_t i = 0; i < entries.size() && i < 200; ++i)
    {
        output << entries[i] << "\n";
    }
    if (entries.size() > 200)
    {
        output << "[truncated at 200 entries]\n";
    }
    return make_tool_result(true, output.str(), "");
}

tool_result codepilot_tool_provider::run_read(const std::vector<std::string> &args) const
{
    if (args.empty())
    {
        return make_tool_result(false, "", "usage: tool read <path> [max-bytes]");
    }

    size_t max_bytes = _max_read_bytes;
    if (args.size() > 1 && !parse_size(args[1], &max_bytes))
    {
        return make_tool_result(false, "", "invalid max-bytes: " + args[1]);
    }

    std::string content;
    std::string error;
    const std::string path = normalize_platform_path(args[0]);
    if (!read_text_prefix(path, max_bytes, &content, &error))
    {
        return make_tool_result(false, "", error);
    }

    return make_tool_result(true, "file: " + path + "\n" + content, "");
}

tool_result codepilot_tool_provider::run_search(const std::vector<std::string> &args) const
{
    if (args.size() < 2)
    {
        return make_tool_result(false, "", "usage: tool search <path> <text>");
    }

    std::vector<std::string> files;
    const std::string path = normalize_platform_path(args[0]);
    collect_files(path, &files);

    std::ostringstream output;
    size_t matches = 0;
    for (const std::string &path : files)
    {
        if (matches >= _max_search_matches)
        {
            break;
        }

        std::ifstream input(path.c_str(), std::ios::binary);
        if (!input)
        {
            continue;
        }

        std::string line;
        size_t line_no = 0;
        while (matches < _max_search_matches && std::getline(input, line))
        {
            ++line_no;
            if (line.find(args[1]) != std::string::npos)
            {
                output << path << ":" << line_no << ": " << line << "\n";
                ++matches;
            }
        }
    }

    if (matches == 0)
    {
        output << "no matches for '" << args[1] << "' under " << path << "\n";
    }
    else if (matches >= _max_search_matches)
    {
        output << "[truncated at " << _max_search_matches << " matches]\n";
    }

    return make_tool_result(true, output.str(), "");
}

tool_result codepilot_tool_provider::run_write(const std::vector<std::string> &args) const
{
    if (args.size() < 2)
    {
        return make_tool_result(false, "", "usage: tool write <path> <content>");
    }

    const std::string path = normalize_platform_path(args[0]);
    const std::string content = join_tool_args(args, 1);
    std::string error;
    if (!codepilot_write_file_atomically(path, content, &error))
    {
        return make_tool_result(false, "", error);
    }

    return make_tool_result(true, "wrote " + std::to_string(content.size()) + " bytes to " + path, "");
}

tool_result codepilot_tool_provider::run_replace(const std::vector<std::string> &args) const
{
    if (args.size() < 3)
    {
        return make_tool_result(false, "", "usage: tool replace <path> <old> <new>");
    }

    std::string content;
    std::string error;
    const std::string path = normalize_platform_path(args[0]);
    if (!read_text_prefix(path, 1024 * 1024, &content, &error))
    {
        return make_tool_result(false, "", error);
    }

    const std::string::size_type pos = content.find(args[1]);
    if (pos == std::string::npos)
    {
        return make_tool_result(false, "", "old text not found in " + path);
    }

    content.replace(pos, args[1].size(), join_tool_args(args, 2));

    if (!codepilot_write_file_atomically(path, content, &error))
    {
        return make_tool_result(false, "", error);
    }

    return make_tool_result(true, "replaced first occurrence in " + path, "");
}

tool_result codepilot_tool_provider::run_shell(const std::vector<std::string> &args) const
{
    if (args.empty())
    {
        return make_tool_result(false, "", "usage: tool shell <command>");
    }

    const std::string command = join_tool_args(args, 0);
    std::string error;
    if (!codepilot_shell_command_allowed(command, split_config_list(config_string_compat("shell_allowed_commands", "")), &error))
    {
        return make_tool_result(false, "", error);
    }

    const std::string working_directory =
        normalize_platform_path(config_string_compat("shell_working_directory", config_string_compat("workspace_root", "")));
    if (!working_directory.empty() && !::dsn::utils::filesystem::directory_exists(working_directory))
    {
        return make_tool_result(false, "", "shell working directory does not exist: " + working_directory);
    }

    const std::string executor = lower_ascii(config_string_compat("shell_executor", "local"));
    std::string effective_command;
    if (executor == "local")
    {
        effective_command = codepilot_shell_command_with_working_directory(command, working_directory);
    }
    else if (executor == "container")
    {
        effective_command = codepilot_shell_command_with_container_template(
            command, working_directory, config_string_compat("shell_container_template", ""), &error);
        if (!error.empty())
        {
            return make_tool_result(false, "", error);
        }
    }
    else
    {
        return make_tool_result(false, "", "unsupported shell_executor: " + executor);
    }

    return codepilot_run_shell_command(effective_command,
                                       config_uint64_compat("shell_timeout_ms", 300000));
}

std::unique_ptr<agent_tool_provider> create_codepilot_tool_provider()
{
    return std::unique_ptr<agent_tool_provider>(new codepilot_tool_provider());
}

} // namespace rasn
} // namespace dsn
