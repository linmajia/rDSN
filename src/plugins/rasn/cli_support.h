#pragma once

#include "rasn_core.h"

#include <dsn/cpp/utils.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

namespace dsn {
namespace rasn {

struct cli_startup_context
{
    bool matched = false;
    std::string workspace_root;
    std::string context_path;
    std::string context_text;
    bool context_truncated = false;
    std::string message;
    std::string error;
};

struct cli_workspace_context_options
{
    uint64_t max_files = 256;
    uint64_t max_sampled_files = 24;
    uint64_t max_file_bytes = 4096;
    uint64_t max_total_bytes = 64ull * 1024ull;
};

namespace cli_support_detail {

inline bool change_process_directory(const std::string &path, std::string *error)
{
#if defined(_WIN32)
    const int rc = ::_chdir(path.c_str());
#else
    const int rc = ::chdir(path.c_str());
#endif
    if (rc == 0)
    {
        return true;
    }
    if (error != nullptr)
    {
        *error = "cannot switch workspace to " + path + ": " + std::strerror(errno);
    }
    return false;
}

inline std::string absolute_or_original(const std::string &path)
{
    std::string normalized = normalize_platform_path(path);
    std::string absolute;
    if (::dsn::utils::filesystem::get_absolute_path(normalized, absolute))
    {
        return normalize_platform_path(absolute);
    }
    return normalized;
}

inline bool read_context_prefix(const std::string &path,
                                size_t max_bytes,
                                std::string *content,
                                bool *truncated,
                                std::string *error)
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
            *error = "cannot open context file: " + path;
        }
        return false;
    }

    content->clear();
    if (max_bytes == 0)
    {
        return true;
    }

    char buffer[4096];
    size_t total = 0;
    while (input && total < max_bytes)
    {
        const size_t remaining = max_bytes - total;
        const size_t to_read = (std::min)(remaining, sizeof(buffer));
        input.read(buffer, static_cast<std::streamsize>(to_read));
        const std::streamsize got = input.gcount();
        if (got <= 0)
        {
            break;
        }
        content->append(buffer, static_cast<size_t>(got));
        total += static_cast<size_t>(got);
    }

    if (total >= max_bytes && input)
    {
        const int next = input.peek();
        if (next != EOF && truncated != nullptr)
        {
            *truncated = true;
        }
    }
    return true;
}

inline std::string lower_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

inline size_t clamp_to_size_t(uint64_t value)
{
    const uint64_t max_size = static_cast<uint64_t>((std::numeric_limits<size_t>::max)());
    return value > max_size ? (std::numeric_limits<size_t>::max)() : static_cast<size_t>(value);
}

inline size_t capped_count(uint64_t configured_limit, size_t available)
{
    return clamp_to_size_t((std::min)(configured_limit, static_cast<uint64_t>(available)));
}

inline const std::unordered_set<std::string> &workspace_ignored_components()
{
    static const std::unordered_set<std::string> components = {
        ".git", ".svn", ".hg", "builder", "builder-rasn", "build", "node_modules", ".venv", "venv",
        "__pycache__", "target", "dist", "out", ".aws", ".azure", ".config", ".gnupg", ".kube", ".ssh",
        "certs", "credentials", "secrets",
    };
    return components;
}

inline const std::unordered_set<std::string> &workspace_sensitive_file_names()
{
    static const std::unordered_set<std::string> names = {
        ".env",        ".envrc",       ".npmrc",       ".pypirc",      ".netrc",
        "config.json", "config.yml",   "config.yaml",  "credentials",
        "id_rsa",      "id_dsa",       "id_ecdsa",     "id_ed25519",
        "known_hosts", "secrets.json", "secrets.yml",  "secrets.yaml",
    };
    return names;
}

inline const std::unordered_map<std::string, int> &workspace_file_priorities()
{
    static const std::unordered_map<std::string, int> priorities = {
        {"readme", 0},        {"readme.md", 0},     {"cmakelists.txt", 0}, {"makefile", 0},
        {"go.mod", 0},        {"cargo.toml", 0},    {"package.json", 0},   {"pyproject.toml", 0},
        {"license", 4},       {"license.md", 4},    {"changelog", 4},      {"changelog.md", 4},
        {"configure", 4},     {"configure.ac", 4},  {"meson.build", 4},    {"build", 4},
        {"workspace", 4},
    };
    return priorities;
}

inline const std::unordered_map<std::string, int> &workspace_extension_priorities()
{
    static const std::unordered_map<std::string, int> priorities = {
        {".h", 1},    {".hh", 1},   {".hpp", 1},  {".hxx", 1},
        {".c", 2},    {".cc", 2},   {".cpp", 2},  {".cxx", 2},
        {".py", 2},   {".rs", 2},   {".go", 2},   {".java", 2},
        {".js", 2},   {".jsx", 2},  {".ts", 2},   {".tsx", 2},
        {".cs", 2},   {".sh", 3},   {".cmake", 3}, {".yml", 3},
        {".yaml", 3}, {".json", 3}, {".md", 4},   {".txt", 4},
    };
    return priorities;
}

inline bool is_sensitive_workspace_file(const std::string &path)
{
    const std::string file_name = lower_ascii(::dsn::utils::filesystem::get_file_name(path));
    if (workspace_sensitive_file_names().find(file_name) != workspace_sensitive_file_names().end())
    {
        return true;
    }
    if (file_name.find(".env.") == 0)
    {
        return true;
    }
    return file_name.find("secret") != std::string::npos ||
           file_name.find("credential") != std::string::npos ||
           file_name.find("password") != std::string::npos ||
           file_name.find("private") != std::string::npos ||
           file_name.find("token") != std::string::npos ||
           file_name.find("apikey") != std::string::npos ||
           file_name.find("api_key") != std::string::npos;
}

inline bool path_has_ignored_component(const std::string &path)
{
    std::string component;
    std::string previous_component;
    for (char ch : path)
    {
        if (ch == '\\' || ch == '/')
        {
            const std::string lower = lower_ascii(component);
            if (previous_component == "rasn" && (lower == "state" || lower == "artifacts" || lower == "traces"))
            {
                return true;
            }
            if (workspace_ignored_components().find(lower) != workspace_ignored_components().end())
            {
                return true;
            }
            previous_component = lower;
            component.clear();
            continue;
        }
        component.push_back(ch);
    }
    return false;
}

inline std::string file_extension_lower(const std::string &path)
{
    const std::string file_name = lower_ascii(::dsn::utils::filesystem::get_file_name(path));
    const size_t dot = file_name.find_last_of('.');
    if (dot == std::string::npos)
    {
        return "";
    }
    return file_name.substr(dot);
}

inline bool is_workspace_source_candidate(const std::string &path)
{
    const std::string file_name = lower_ascii(::dsn::utils::filesystem::get_file_name(path));
    const std::string ext = file_extension_lower(path);
    return workspace_file_priorities().find(file_name) != workspace_file_priorities().end() ||
           workspace_extension_priorities().find(ext) != workspace_extension_priorities().end();
}

inline int workspace_source_priority(const std::string &path)
{
    const std::string file_name = lower_ascii(::dsn::utils::filesystem::get_file_name(path));
    const std::unordered_map<std::string, int>::const_iterator file_priority =
        workspace_file_priorities().find(file_name);
    if (file_priority != workspace_file_priorities().end())
    {
        return file_priority->second;
    }

    const std::string ext = file_extension_lower(path);
    const std::unordered_map<std::string, int>::const_iterator extension_priority =
        workspace_extension_priorities().find(ext);
    if (extension_priority != workspace_extension_priorities().end())
    {
        return extension_priority->second;
    }
    return 4;
}

inline size_t path_depth(const std::string &path)
{
    return static_cast<size_t>(std::count(path.begin(), path.end(), '\\') + std::count(path.begin(), path.end(), '/'));
}

inline std::string relative_workspace_path(const std::string &workspace_root, const std::string &path)
{
    const std::string root = normalize_platform_path(workspace_root);
    const std::string normalized = normalize_platform_path(path);
    if (normalized == root)
    {
        return ".";
    }
    if (normalized.size() > root.size() &&
        normalized.compare(0, root.size(), root) == 0 &&
        (normalized[root.size()] == '\\' || normalized[root.size()] == '/'))
    {
        return normalized.substr(root.size() + 1);
    }
    return normalized;
}

inline bool content_looks_binary(const std::string &content)
{
    return content.find('\0') != std::string::npos;
}

} // namespace cli_support_detail

inline bool build_workspace_source_context(const std::string &workspace_root,
                                           const cli_workspace_context_options &options,
                                           std::string *context_text,
                                           bool *truncated,
                                           std::string *error)
{
    if (context_text == nullptr)
    {
        return false;
    }
    if (truncated != nullptr)
    {
        *truncated = false;
    }

    const std::string absolute = cli_support_detail::absolute_or_original(workspace_root);
    std::vector<std::string> files;
    if (!::dsn::utils::filesystem::get_subfiles(absolute, files, true))
    {
        if (error != nullptr)
        {
            *error = "cannot enumerate workspace files: " + absolute;
        }
        return false;
    }

    std::vector<std::string> candidates;
    for (const std::string &file : files)
    {
        const std::string normalized = normalize_platform_path(file);
        const std::string relative = cli_support_detail::relative_workspace_path(absolute, normalized);
        if (!cli_support_detail::path_has_ignored_component(relative) &&
            !cli_support_detail::is_sensitive_workspace_file(relative) &&
            cli_support_detail::is_workspace_source_candidate(relative))
        {
            candidates.push_back(normalized);
        }
    }

    std::sort(candidates.begin(), candidates.end(), [&absolute](const std::string &left, const std::string &right) {
        const std::string left_relative = cli_support_detail::relative_workspace_path(absolute, left);
        const std::string right_relative = cli_support_detail::relative_workspace_path(absolute, right);
        const int left_priority = cli_support_detail::workspace_source_priority(left_relative);
        const int right_priority = cli_support_detail::workspace_source_priority(right_relative);
        if (left_priority != right_priority)
        {
            return left_priority < right_priority;
        }
        const size_t left_depth = cli_support_detail::path_depth(left_relative);
        const size_t right_depth = cli_support_detail::path_depth(right_relative);
        if (left_depth != right_depth)
        {
            return left_depth < right_depth;
        }
        return left_relative < right_relative;
    });

    std::ostringstream output;
    output << "workspace source snapshot: " << absolute
           << "\nThis is a bounded automatic snapshot for repository questions; it may be partial.\n"
           << "source files matched: " << candidates.size() << "\n";

    const size_t files_to_list = cli_support_detail::capped_count(options.max_files, candidates.size());
    output << "source file index";
    if (files_to_list < candidates.size())
    {
        output << " (first " << files_to_list << ")";
        if (truncated != nullptr)
        {
            *truncated = true;
        }
    }
    output << ":\n";
    for (size_t i = 0; i < files_to_list; ++i)
    {
        output << "- " << cli_support_detail::relative_workspace_path(absolute, candidates[i]) << "\n";
    }
    if (candidates.empty())
    {
        output << "- <no source-like files matched>\n";
    }

    output << "\nSelected file excerpts:\n";
    uint64_t total_excerpt_bytes = 0;
    uint64_t sampled_files = 0;
    for (const std::string &file : candidates)
    {
        if (sampled_files >= options.max_sampled_files || total_excerpt_bytes >= options.max_total_bytes)
        {
            if (truncated != nullptr && sampled_files < static_cast<uint64_t>(candidates.size()))
            {
                *truncated = true;
            }
            break;
        }

        const uint64_t remaining = options.max_total_bytes - total_excerpt_bytes;
        const size_t file_budget = cli_support_detail::clamp_to_size_t((std::min)(options.max_file_bytes, remaining));
        std::string content;
        bool file_truncated = false;
        if (!cli_support_detail::read_context_prefix(file, file_budget, &content, &file_truncated, nullptr))
        {
            continue;
        }
        if (cli_support_detail::content_looks_binary(content))
        {
            continue;
        }

        ++sampled_files;
        total_excerpt_bytes += static_cast<uint64_t>(content.size());
        output << "\n--- file: " << cli_support_detail::relative_workspace_path(absolute, file) << " ---\n"
               << content;
        if (file_truncated)
        {
            output << "\n[excerpt truncated at " << file_budget << " bytes]\n";
            if (truncated != nullptr)
            {
                *truncated = true;
            }
        }
        else if (!content.empty() && content[content.size() - 1] != '\n')
        {
            output << "\n";
        }
    }
    if (sampled_files == 0)
    {
        output << "<no text excerpts loaded>\n";
    }

    context_text->assign(output.str());
    return true;
}

inline bool cli_argument_is_command(const std::string &arg, const std::vector<std::string> &commands)
{
    return std::find(commands.begin(), commands.end(), arg) != commands.end();
}

inline bool bootstrap_single_path_argument(const std::vector<std::string> &args,
                                           const std::vector<std::string> &commands,
                                           cli_startup_context *context,
                                           size_t max_context_bytes = 1024u * 1024u,
                                           const cli_workspace_context_options *workspace_options = nullptr)
{
    if (context == nullptr)
    {
        return false;
    }
    *context = cli_startup_context();

    if (args.size() != 1 || cli_argument_is_command(args[0], commands) ||
        !::dsn::utils::filesystem::path_exists(args[0]))
    {
        return true;
    }

    context->matched = true;
    const std::string absolute = cli_support_detail::absolute_or_original(args[0]);
    if (::dsn::utils::filesystem::directory_exists(args[0]))
    {
        if (!cli_support_detail::change_process_directory(absolute, &context->error))
        {
            return false;
        }
        context->workspace_root = absolute;
        context->message = "workspace: " + absolute;
        context->context_path = absolute;
        if (workspace_options != nullptr)
        {
            std::string snapshot_error;
            if (!build_workspace_source_context(
                    absolute, *workspace_options, &context->context_text, &context->context_truncated, &snapshot_error))
            {
                context->message += "\nworkspace source context unavailable: " + snapshot_error;
                context->context_text.clear();
                context->context_truncated = false;
                return true;
            }
            context->message += "\nloaded workspace source context";
            if (context->context_truncated)
            {
                context->message += " [bounded]";
            }
        }
        return true;
    }

    if (::dsn::utils::filesystem::file_exists(args[0]))
    {
        const std::string parent = ::dsn::utils::filesystem::remove_file_name(absolute);
        if (!parent.empty() && !cli_support_detail::change_process_directory(parent, &context->error))
        {
            return false;
        }

        std::string file_content;
        if (!cli_support_detail::read_context_prefix(
                absolute, max_context_bytes, &file_content, &context->context_truncated, &context->error))
        {
            return false;
        }
        if (context->context_truncated)
        {
            std::ostringstream suffix;
            suffix << "\n\n[context truncated at " << max_context_bytes << " bytes]";
            file_content += suffix.str();
        }

        context->workspace_root = parent.empty() ? "." : parent;
        context->context_path = absolute;
        context->context_text = "file: " + absolute + "\n" + file_content;
        context->message = "workspace: " + context->workspace_root + "\nloaded context file: " + absolute;
        return true;
    }

    context->error = "path is not a file or directory: " + args[0];
    return false;
}

inline std::string interactive_help_intro(const std::string &plain_text_behavior)
{
    return "  In interactive mode, prefix commands with '/': /help and /exit always work.\n"
           "  Without '/', plain text is " +
           plain_text_behavior + ".\n\n";
}

inline std::string cli_help_intro(bool interactive_mode, const std::string &plain_text_behavior)
{
    if (interactive_mode)
    {
        return "  Commands below are shown with '/'. /help and /exit always work.\n"
               "  Without '/', plain text is " +
               plain_text_behavior + ".\n\n";
    }
    return "  Direct CLI commands are shown without '/'. In interactive mode, prefix commands with '/'.\n"
           "  Plain text is " +
           plain_text_behavior + ".\n\n";
}

inline std::string cli_help_item(bool interactive_mode,
                                 const std::string &usage,
                                 const std::string &description,
                                 size_t width = 32u)
{
    const std::string rendered = interactive_mode ? "/" + usage : usage;
    std::ostringstream output;
    output << "  " << rendered;
    if (rendered.size() < width)
    {
        output << std::string(width - rendered.size(), ' ');
    }
    else
    {
        output << " ";
    }
    output << description << "\n";
    return output.str();
}

} // namespace rasn
} // namespace dsn
