#pragma once

#include <rasn/agent_tools.h>

#include <cstddef>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

struct tool_catalog_entry
{
    tool_descriptor descriptor;
    std::vector<std::string> aliases;
};

struct tool_invocation
{
    bool ok = false;
    std::string name;
    std::vector<std::string> args;
    std::string error;
};

class tool_catalog
{
public:
    void add(const tool_descriptor &descriptor, const std::vector<std::string> &aliases = std::vector<std::string>());

    const tool_descriptor *find(const std::string &name) const;
    bool contains(const std::string &name) const;
    std::string canonical_name(const std::string &name) const;
    std::vector<tool_descriptor> descriptors() const;
    std::string describe(const std::string &title = "Available tools:") const;

private:
    std::vector<tool_catalog_entry> _entries;
};

tool_argument_descriptor make_tool_argument(const std::string &name,
                                            bool required,
                                            const std::string &description);
tool_descriptor make_tool_descriptor(const std::string &name,
                                     const std::string &side_effect,
                                     const std::string &description,
                                     const std::vector<tool_argument_descriptor> &arguments);
tool_invocation normalize_tool_invocation(const tool_catalog &catalog,
                                          const std::string &name,
                                          const std::vector<std::string> &args);
std::string join_tool_arguments(const std::vector<std::string> &args, size_t begin);

} // namespace rasn
} // namespace dsn
