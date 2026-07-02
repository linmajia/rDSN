#include "tool_catalog.h"

#include <sstream>

namespace dsn {
namespace rasn {

namespace {

bool names_match(const std::string &lhs, const std::string &rhs)
{
    return lhs == rhs;
}

size_t required_argument_count(const tool_descriptor &descriptor)
{
    size_t count = 0;
    for (const tool_argument_descriptor &argument : descriptor.arguments)
    {
        if (argument.required)
        {
            ++count;
        }
    }
    return count;
}

std::string usage_for_tool(const tool_descriptor &descriptor)
{
    std::ostringstream output;
    output << "usage: tool " << descriptor.name;
    for (const tool_argument_descriptor &argument : descriptor.arguments)
    {
        output << " ";
        if (!argument.required)
        {
            output << "[";
        }
        output << "<" << argument.name << ">";
        if (!argument.required)
        {
            output << "]";
        }
    }
    return output.str();
}

} // namespace

void tool_catalog::add(const tool_descriptor &descriptor, const std::vector<std::string> &aliases)
{
    tool_catalog_entry entry;
    entry.descriptor = descriptor;
    entry.aliases = aliases;
    _entries.push_back(entry);
}

const tool_descriptor *tool_catalog::find(const std::string &name) const
{
    for (const tool_catalog_entry &entry : _entries)
    {
        if (names_match(entry.descriptor.name, name))
        {
            return &entry.descriptor;
        }
        for (const std::string &alias : entry.aliases)
        {
            if (names_match(alias, name))
            {
                return &entry.descriptor;
            }
        }
    }
    return nullptr;
}

bool tool_catalog::contains(const std::string &name) const
{
    return find(name) != nullptr;
}

std::string tool_catalog::canonical_name(const std::string &name) const
{
    const tool_descriptor *descriptor = find(name);
    return descriptor == nullptr ? name : descriptor->name;
}

std::vector<tool_descriptor> tool_catalog::descriptors() const
{
    std::vector<tool_descriptor> values;
    values.reserve(_entries.size());
    for (const tool_catalog_entry &entry : _entries)
    {
        values.push_back(entry.descriptor);
    }
    return values;
}

std::string tool_catalog::describe(const std::string &title) const
{
    std::ostringstream output;
    output << title << "\n";
    for (const tool_catalog_entry &entry : _entries)
    {
        const tool_descriptor &tool = entry.descriptor;
        output << "- " << tool.name;
        for (const tool_argument_descriptor &argument : tool.arguments)
        {
            output << " <" << argument.name << (argument.required ? "" : "?") << ">";
        }
        output << ": " << tool.description << " [" << tool.side_effect << "]";
        if (!entry.aliases.empty())
        {
            output << " aliases=";
            for (size_t i = 0; i < entry.aliases.size(); ++i)
            {
                if (i != 0)
                {
                    output << ",";
                }
                output << entry.aliases[i];
            }
        }
        output << "\n";
    }
    return output.str();
}

tool_argument_descriptor make_tool_argument(const std::string &name,
                                            bool required,
                                            const std::string &description)
{
    tool_argument_descriptor argument;
    argument.name = name;
    argument.required = required;
    argument.description = description;
    return argument;
}

tool_descriptor make_tool_descriptor(const std::string &name,
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

tool_invocation normalize_tool_invocation(const tool_catalog &catalog,
                                          const std::string &name,
                                          const std::vector<std::string> &args)
{
    tool_invocation invocation;
    const tool_descriptor *descriptor = catalog.find(name);
    if (descriptor == nullptr)
    {
        invocation.error = "unknown tool: " + name;
        return invocation;
    }

    const size_t required = required_argument_count(*descriptor);
    if (args.size() < required)
    {
        invocation.error = usage_for_tool(*descriptor);
        return invocation;
    }

    invocation.ok = true;
    invocation.name = descriptor->name;
    invocation.args = args;
    return invocation;
}

std::string join_tool_arguments(const std::vector<std::string> &args, size_t begin)
{
    std::ostringstream output;
    for (size_t i = begin; i < args.size(); ++i)
    {
        if (i != begin)
        {
            output << " ";
        }
        output << args[i];
    }
    return output.str();
}

} // namespace rasn
} // namespace dsn
