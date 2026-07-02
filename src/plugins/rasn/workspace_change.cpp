#include "workspace_change.h"

#include "rasn_core.h"

#include <dsn/cpp/utils.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace dsn {
namespace rasn {

namespace {

std::string absolute_or_normalized_path(const std::string &path)
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

bool path_is_absolute(const std::string &path)
{
    if (path.empty())
    {
        return false;
    }
#if defined(_WIN32)
    if (path.size() >= 2 && path[1] == ':')
    {
        return true;
    }
    return path[0] == '\\' || path[0] == '/';
#else
    return path[0] == '/';
#endif
}

std::string content_fingerprint(const std::string &content)
{
    uint64_t hash = 14695981039346656037ull;
    for (const char c : content)
    {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }

    std::ostringstream output;
    output << "bytes=" << content.size() << " fnv1a=0x"
           << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}

bool read_file_bounded(const std::string &path,
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
            *error = "cannot open file: " + path;
        }
        return false;
    }

    input.seekg(0, std::ios::end);
    const std::streamoff end_pos = input.tellg();
    size_t to_read = max_bytes;
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
    if (truncated != nullptr && input.good() && input.peek() != EOF)
    {
        *truncated = true;
    }
    return true;
}

std::string preview_change_text(const std::string &path, const std::string &content)
{
    std::ostringstream output;
    output << "file: " << path << "\n";
    const size_t limit = (std::min)(content.size(), static_cast<size_t>(1200));
    output << content.substr(0, limit);
    if (content.size() > limit)
    {
        output << "\n[preview truncated at " << limit << " bytes]";
    }
    return output.str();
}

workspace_change_plan error_plan(const workspace_change_request &request, const std::string &error)
{
    workspace_change_plan plan;
    plan.kind = request.kind;
    plan.path = request.path;
    plan.workspace_root = request.workspace_root;
    plan.error = error;
    plan.ok = false;
    return plan;
}

workspace_change_plan plan_write(const workspace_change_request &request)
{
    std::string error;
    const std::string path = normalize_workspace_change_path(request.path, request.workspace_root, &error);
    if (path.empty())
    {
        return error_plan(request, error);
    }

    workspace_change_plan plan;
    plan.ok = true;
    plan.kind = request.kind;
    plan.path = path;
    plan.workspace_root = request.workspace_root;
    plan.creates_file = !::dsn::utils::filesystem::file_exists(path);
    int64_t existing_size = 0;
    if (!plan.creates_file && ::dsn::utils::filesystem::file_size(path, existing_size) && existing_size > 0)
    {
        plan.old_size = static_cast<size_t>(existing_size);
    }
    plan.new_size = request.content.size();
    plan.old_fingerprint = plan.creates_file ? "missing" : "existing bytes=" + std::to_string(plan.old_size);
    plan.new_fingerprint = content_fingerprint(request.content);
    plan.new_content = request.content;
    plan.temp_prefix = request.temp_prefix;
    plan.summary = std::string(plan.creates_file ? "create " : "write ") + path + " bytes=" + std::to_string(plan.new_size);
    plan.preview = preview_change_text(path, request.content);
    return plan;
}

workspace_change_plan plan_replace(const workspace_change_request &request)
{
    if (request.old_text.empty())
    {
        return error_plan(request, "old text cannot be empty");
    }

    std::string error;
    const std::string path = normalize_workspace_change_path(request.path, request.workspace_root, &error);
    if (path.empty())
    {
        return error_plan(request, error);
    }

    std::string content;
    bool truncated = false;
    if (!read_file_bounded(path, request.max_edit_bytes, &content, &truncated, &error))
    {
        return error_plan(request, error);
    }
    if (truncated)
    {
        return error_plan(
            request,
            "file too large to edit safely (exceeds " + std::to_string(request.max_edit_bytes) + " bytes): " + path);
    }

    const std::string::size_type pos = content.find(request.old_text);
    if (pos == std::string::npos)
    {
        return error_plan(request, "old text not found in " + path);
    }

    const size_t original_size = content.size();
    const std::string before_fingerprint = content_fingerprint(content);
    content.replace(pos, request.old_text.size(), request.new_text);

    workspace_change_plan plan;
    plan.ok = true;
    plan.kind = request.kind;
    plan.path = path;
    plan.workspace_root = request.workspace_root;
    plan.creates_file = false;
    plan.old_size = original_size;
    plan.new_size = content.size();
    plan.old_fingerprint = before_fingerprint;
    plan.new_fingerprint = content_fingerprint(content);
    plan.new_content = content;
    plan.temp_prefix = request.temp_prefix;
    plan.summary = "replace first occurrence in " + path;
    plan.preview = preview_change_text(path, content);
    return plan;
}

} // namespace

std::string workspace_change_kind_name(workspace_change_kind kind)
{
    switch (kind)
    {
    case workspace_change_kind::write_file:
        return "write_file";
    case workspace_change_kind::replace_text:
        return "replace_text";
    case workspace_change_kind::shell_command:
        return "shell_command";
    }
    return "unknown";
}

std::string normalize_workspace_change_path(const std::string &path,
                                            const std::string &workspace_root,
                                            std::string *error)
{
    if (trim(path).empty())
    {
        if (error != nullptr)
        {
            *error = "cannot plan workspace change with empty path";
        }
        return "";
    }

    std::string target = normalize_platform_path(path);
    if (workspace_root.empty())
    {
        if (error != nullptr)
        {
            error->clear();
        }
        return target;
    }

    if (!path_is_absolute(target))
    {
        target = ::dsn::utils::filesystem::path_combine(workspace_root, target);
    }

    if (error != nullptr)
    {
        error->clear();
    }
    return absolute_or_normalized_path(target);
}

workspace_change_plan plan_workspace_change(const workspace_change_request &request)
{
    if (request.kind == workspace_change_kind::write_file)
    {
        return plan_write(request);
    }
    if (request.kind == workspace_change_kind::replace_text)
    {
        return plan_replace(request);
    }
    return error_plan(request, "workspace change kind is not file-editable: " + workspace_change_kind_name(request.kind));
}

bool write_workspace_file_atomically(const std::string &path,
                                     const std::string &content,
                                     const std::string &temp_prefix,
                                     std::string *error)
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
    const std::string prefix = temp_prefix.empty() ? "rasn-write" : temp_prefix;
    const std::string temp_name = "." + (file_name.empty() ? prefix : file_name) + ".tmp." + make_trace_id();
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

bool apply_workspace_change_plan(const workspace_change_plan &plan, std::string *error)
{
    if (!plan.ok)
    {
        if (error != nullptr)
        {
            *error = plan.error.empty() ? "workspace change plan is not valid" : plan.error;
        }
        return false;
    }
    if (plan.kind != workspace_change_kind::write_file && plan.kind != workspace_change_kind::replace_text)
    {
        if (error != nullptr)
        {
            *error = "workspace change kind is not file-editable: " + workspace_change_kind_name(plan.kind);
        }
        return false;
    }
    return write_workspace_file_atomically(plan.path, plan.new_content, plan.temp_prefix, error);
}

std::string describe_workspace_change_plan(const workspace_change_plan &plan)
{
    if (!plan.ok)
    {
        return plan.error;
    }
    std::ostringstream output;
    output << plan.summary
           << "\nkind=" << workspace_change_kind_name(plan.kind)
           << " old_size=" << plan.old_size
           << " new_size=" << plan.new_size
           << " old=" << plan.old_fingerprint
           << " new=" << plan.new_fingerprint;
    if (!plan.preview.empty())
    {
        output << "\n" << plan.preview;
    }
    return output.str();
}

} // namespace rasn
} // namespace dsn
