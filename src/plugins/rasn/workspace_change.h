#pragma once

#include <cstddef>
#include <string>

namespace dsn {
namespace rasn {

enum class workspace_change_kind
{
    write_file,
    replace_text,
    shell_command
};

struct workspace_change_request
{
    workspace_change_kind kind = workspace_change_kind::write_file;
    std::string path;
    std::string workspace_root;
    std::string content;
    std::string old_text;
    std::string new_text;
    std::string command;
    size_t max_edit_bytes = 64u * 1024u * 1024u;
    std::string temp_prefix = "rasn-write";
};

struct workspace_change_plan
{
    bool ok = false;
    workspace_change_kind kind = workspace_change_kind::write_file;
    std::string path;
    std::string workspace_root;
    std::string summary;
    std::string preview;
    std::string error;
    bool creates_file = false;
    size_t old_size = 0;
    size_t new_size = 0;
    std::string old_fingerprint;
    std::string new_fingerprint;
    std::string new_content;
    std::string temp_prefix = "rasn-write";
};

std::string workspace_change_kind_name(workspace_change_kind kind);
std::string normalize_workspace_change_path(const std::string &path,
                                            const std::string &workspace_root,
                                            std::string *error);
workspace_change_plan plan_workspace_change(const workspace_change_request &request);
bool apply_workspace_change_plan(const workspace_change_plan &plan, std::string *error);
bool write_workspace_file_atomically(const std::string &path,
                                     const std::string &content,
                                     const std::string &temp_prefix,
                                     std::string *error);
std::string describe_workspace_change_plan(const workspace_change_plan &plan);

} // namespace rasn
} // namespace dsn
