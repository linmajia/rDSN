#pragma once

#include "rasn_core.h"

#include <dsn/cpp/utils.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
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

} // namespace cli_support_detail

inline bool cli_argument_is_command(const std::string &arg, const std::vector<std::string> &commands)
{
    return std::find(commands.begin(), commands.end(), arg) != commands.end();
}

inline bool bootstrap_single_path_argument(const std::vector<std::string> &args,
                                           const std::vector<std::string> &commands,
                                           cli_startup_context *context,
                                           size_t max_context_bytes = 1024u * 1024u)
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

} // namespace rasn
} // namespace dsn
