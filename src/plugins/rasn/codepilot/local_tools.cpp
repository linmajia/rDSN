#include "local_tools.h"

#include "../policy_manager.h"

#include <dsn/cpp/utils.h>
#include <dsn/service_api_cpp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
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
    // strtoul silently accepts a leading '-' (wrapping to ULONG_MAX) and does not
    // by itself signal overflow, so a model-supplied "read <file> -1" or a huge
    // number would otherwise drive an enormous allocation downstream. Require a
    // pure non-negative decimal and honor ERANGE.
    if (value.empty() || !std::isdigit(static_cast<unsigned char>(value.front())))
    {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long long result = std::strtoull(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || errno == ERANGE)
    {
        return false;
    }
    if (result > static_cast<unsigned long long>((std::numeric_limits<size_t>::max)()))
    {
        return false;
    }
    *parsed = static_cast<size_t>(result);
    return true;
}

// Hard ceiling on how many bytes any single read/edit will buffer, independent of
// the model-supplied max-bytes. Bounds worst-case memory for one tool call.
const size_t k_max_file_read_bytes = 64u * 1024u * 1024u;

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
    // Include $ ( ) { } so an allow-listed word[0] cannot smuggle a command
    // substitution / subshell (e.g. "echo $(rm -rf ~)" or "echo `id`") past the
    // allow-list: on POSIX the string still runs through /bin/sh -c.
    return command.find_first_of("&|;<>`$(){}") != std::string::npos ||
           command.find('\n') != std::string::npos || command.find('\r') != std::string::npos;
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

bool read_text_prefix(const std::string &path,
                      size_t max_bytes,
                      std::string *content,
                      std::string *error,
                      bool *truncated = nullptr)
{
    if (truncated != nullptr)
    {
        *truncated = false;
    }

    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input)
    {
        if (error != nullptr)
        {
            *error = "cannot open file: " + path;
        }
        return false;
    }

    // Bound the allocation by the actual file size so a large model-supplied
    // max_bytes cannot force a multi-GB allocation for a small (or empty) file.
    // When the size is unknown (non-seekable input) fall back to max_bytes, which
    // callers already clamp to k_max_file_read_bytes.
    size_t to_read = max_bytes;
    input.seekg(0, std::ios::end);
    const std::streamoff end_pos = input.tellg();
    if (end_pos >= 0)
    {
        to_read = (std::min)(max_bytes, static_cast<size_t>(end_pos));
    }
    input.clear();
    input.seekg(0, std::ios::beg);

    content->clear();
    if (to_read > 0)
    {
        content->resize(to_read);
        input.read(&(*content)[0], static_cast<std::streamsize>(to_read));
        content->resize(static_cast<size_t>(input.gcount()));
    }

    // Report truncation to the caller WITHOUT mutating content. Editing tools must
    // never write back a marker-polluted buffer, and read can surface the notice
    // in its own output.
    const bool more = input.good() && input.peek() != EOF;
    if (more && truncated != nullptr)
    {
        *truncated = true;
    }
    return true;
}

// Read one line, retaining at most max_len bytes and discarding the rest of an
// over-long line. A file with no newlines (minified JS, binary blob, giant log)
// would otherwise load entirely into memory as a single std::getline "line".
// Returns false only at end of input.
bool read_bounded_line(std::istream &input, std::string *line, size_t max_len,
                       bool *line_truncated = nullptr, size_t *bytes_consumed = nullptr)
{
    line->clear();
    if (line_truncated != nullptr)
    {
        *line_truncated = false;
    }
    if (bytes_consumed != nullptr)
    {
        *bytes_consumed = 0;
    }
    int ch = input.get();
    if (ch == EOF)
    {
        return false;
    }
    size_t consumed = 0;
    while (ch != EOF && ch != '\n')
    {
        ++consumed;
        if (line->size() < max_len)
        {
            line->push_back(static_cast<char>(ch));
        }
        else if (line_truncated != nullptr)
        {
            // The line is longer than max_len; the tail is consumed but dropped,
            // so a caller scanning `line` must treat its result as partial.
            *line_truncated = true;
        }
        ch = input.get();
    }
    if (ch == '\n')
    {
        ++consumed; // count the delimiter so byte-bounded callers stay accurate
    }
    if (bytes_consumed != nullptr)
    {
        *bytes_consumed = consumed;
    }
    return true;
}

// Detect obviously-binary files (a NUL byte in the first block) so search skips
// them instead of streaming megabytes of non-text. Rewinds on a text result.
bool stream_looks_binary(std::istream &input)
{
    std::array<char, 4096> probe;
    input.read(probe.data(), static_cast<std::streamsize>(probe.size()));
    const std::streamsize got = input.gcount();
    for (std::streamsize i = 0; i < got; ++i)
    {
        if (probe[static_cast<size_t>(i)] == '\0')
        {
            return true;
        }
    }
    input.clear();
    input.seekg(0, std::ios::beg);
    return false;
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

    // Hash in blocks (not byte-by-byte) and stop at the same ceiling read/replace
    // enforce, so fingerprinting a huge file cannot become an unbounded CPU sink.
    uint64_t hash = 1469598103934665603ull;
    uint64_t bytes = 0;
    std::array<char, 65536> block;
    while (bytes < k_max_file_read_bytes && input)
    {
        input.read(block.data(), static_cast<std::streamsize>(block.size()));
        const std::streamsize got = input.gcount();
        if (got <= 0)
        {
            break;
        }
        for (std::streamsize i = 0; i < got; ++i)
        {
            hash ^= static_cast<unsigned char>(block[static_cast<size_t>(i)]);
            hash *= 1099511628211ull;
            ++bytes;
        }
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
        const DWORD to_read = (std::min)(available, static_cast<DWORD>(buffer.size()));
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
    // popen/pclose give us no way to enforce a timeout or kill a hung child, so
    // the previous POSIX path ignored timeout_ms entirely (a blocking command hung
    // the agent forever). Run the command under our own fork/exec with a new
    // session so a timeout can SIGKILL the whole process group.
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0)
    {
        if (exit_code != nullptr)
        {
            *exit_code = -1;
        }
        return output;
    }

    const pid_t child = fork();
    if (child < 0)
    {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        if (exit_code != nullptr)
        {
            *exit_code = -1;
        }
        return output;
    }

    if (child == 0)
    {
        // Child: become a session/group leader, redirect stdout+stderr to the
        // pipe, then exec the command through the shell.
        setsid();
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }

    // Parent: read until EOF or timeout, killing the child's process group if it
    // overruns the deadline.
    close(pipe_fds[1]);
    const int read_fd = pipe_fds[0];
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    bool killed = false;
    std::array<char, 4096> buffer;
    for (;;)
    {
        struct pollfd pfd;
        pfd.fd = read_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        const int poll_rc = poll(&pfd, 1, 200);
        if (poll_rc > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0)
        {
            const ssize_t n = read(read_fd, buffer.data(), buffer.size());
            if (n > 0)
            {
                output.append(buffer.data(), static_cast<size_t>(n));
                continue;
            }
            if (n == 0)
            {
                break; // child closed the pipe (exited)
            }
            if (errno != EINTR && errno != EAGAIN)
            {
                break;
            }
        }
        else if (poll_rc < 0 && errno != EINTR)
        {
            break;
        }

        if (timeout_ms != 0)
        {
            const uint64_t elapsed_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
            if (elapsed_ms >= timeout_ms)
            {
                kill(-child, SIGKILL);
                if (timed_out != nullptr)
                {
                    *timed_out = true;
                }
                killed = true;
                // Drain output the child already wrote before we killed it, so a
                // timeout does not silently discard buffered stdout/stderr. The
                // killed process group's write ends close, so read() reaches EOF
                // (or poll reports POLLHUP) quickly. Cap the drain by a deadline
                // so a process that escaped the group (e.g. via setsid) cannot
                // hang the parent here.
                const std::chrono::steady_clock::time_point drain_deadline =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                while (std::chrono::steady_clock::now() < drain_deadline)
                {
                    struct pollfd dpfd;
                    dpfd.fd = read_fd;
                    dpfd.events = POLLIN;
                    dpfd.revents = 0;
                    const int drain_rc = poll(&dpfd, 1, 50);
                    if (drain_rc < 0)
                    {
                        if (errno == EINTR)
                        {
                            continue;
                        }
                        break;
                    }
                    if (drain_rc == 0)
                    {
                        continue; // no data yet; keep waiting until the deadline
                    }
                    const ssize_t drained = read(read_fd, buffer.data(), buffer.size());
                    if (drained > 0)
                    {
                        output.append(buffer.data(), static_cast<size_t>(drained));
                        continue;
                    }
                    break; // EOF (write ends closed) or unrecoverable read error
                }
                break;
            }
        }
    }
    close(read_fd);

    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR)
    {
    }

    if (exit_code != nullptr)
    {
        if (killed)
        {
            *exit_code = -1;
        }
        else if (WIFEXITED(status))
        {
            // Decode the real exit code; a raw wait-status would report exit 1 as
            // 256 and hide signal termination.
            *exit_code = WEXITSTATUS(status);
        }
        else if (WIFSIGNALED(status))
        {
            *exit_code = 128 + WTERMSIG(status);
        }
        else
        {
            *exit_code = -1;
        }
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
                                                                       tool_argument("content", true, "Content to write; pass as a single quoted argument so tabs, repeated spaces, and newlines are preserved (unquoted content is joined with single spaces).")}));
    tools.push_back(tool_schema("replace",
                                "write",
                                "Atomically replace the first text occurrence when policy allows write side effects.",
                                std::vector<tool_argument_descriptor>{tool_argument("path", true, "File to edit."),
                                                                       tool_argument("old", true, "Text to replace; pass as a single quoted argument to match interior whitespace exactly."),
                                                                       tool_argument("new", true, "Replacement text; pass as a single quoted argument so tabs, repeated spaces, and newlines are preserved (unquoted text is joined with single spaces).")}));
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
    // Only fingerprint the filesystem when a deterministic trace is actually being
    // recorded or replayed. Otherwise computing the snapshot (hashing whole files
    // / walking whole trees on every read/list/search) is pure overhead and a
    // model-reachable CPU/time sink even with no trace configured.
    const std::string snapshot_key = filesystem_snapshot_key_for_tool(name, args);
    if (!snapshot_key.empty() && (runtime.replay_enabled() || !runtime.trace_file().empty()))
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
    max_bytes = (std::min)(max_bytes, k_max_file_read_bytes);

    std::string content;
    std::string error;
    bool truncated = false;
    const std::string path = normalize_platform_path(args[0]);
    if (!read_text_prefix(path, max_bytes, &content, &error, &truncated))
    {
        return make_tool_result(false, "", error);
    }
    if (truncated)
    {
        content += "\n\n[truncated at " + std::to_string(max_bytes) + " bytes]";
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
        if (stream_looks_binary(input))
        {
            continue;
        }

        std::string line;
        size_t line_no = 0;
        uint64_t scanned_bytes = 0;
        bool file_partial = false;
        while (matches < _max_search_matches)
        {
            // Stop scanning at the same ceiling the filesystem-replay fingerprint
            // hashes (k_max_file_read_bytes). If search read past it, a change or
            // match beyond the fingerprinted prefix could alter output without
            // changing the replay snapshot, breaking deterministic replay.
            if (scanned_bytes >= k_max_file_read_bytes)
            {
                file_partial = true;
                break;
            }
            bool line_truncated = false;
            size_t consumed = 0;
            if (!read_bounded_line(input, &line, 64u * 1024u, &line_truncated, &consumed))
            {
                break;
            }
            scanned_bytes += consumed;
            ++line_no;
            if (line_truncated)
            {
                // A match could exist past the 64 KiB per-line cap in this line.
                file_partial = true;
            }
            if (line.find(args[1]) != std::string::npos)
            {
                output << path << ":" << line_no << ": " << line << "\n";
                ++matches;
            }
        }
        if (file_partial)
        {
            output << path << ": [partial: scan limited to first " << k_max_file_read_bytes
                   << " bytes/file and " << (64u * 1024u)
                   << " bytes/line; some matches may be omitted]\n";
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
    bool truncated = false;
    const std::string path = normalize_platform_path(args[0]);
    // Read the entire file (up to the safety ceiling). Never edit a truncated
    // buffer: previously replace read only the first 1 MiB and wrote that back,
    // silently discarding everything past the cap and corrupting the source file.
    if (!read_text_prefix(path, k_max_file_read_bytes, &content, &error, &truncated))
    {
        return make_tool_result(false, "", error);
    }
    if (truncated)
    {
        return make_tool_result(
            false,
            "",
            "file too large to edit safely (exceeds " + std::to_string(k_max_file_read_bytes) + " bytes): " + path);
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
