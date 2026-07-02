#include <rasn/workflow.h>

#include <rasn/redaction.h>

#include <cctype>
#include <cerrno>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>

namespace dsn {
namespace rasn {

namespace {

bool parse_uint64(const std::string &value, uint64_t *result);

struct workflow_json_value
{
    enum kind
    {
        null_value,
        bool_value,
        number_value,
        string_value,
        array_value,
        object_value
    };

    kind type = null_value;
    bool boolean = false;
    uint64_t number = 0;
    std::string string;
    std::vector<workflow_json_value> array;
    std::map<std::string, workflow_json_value> object;
};

class workflow_json_parser
{
public:
    explicit workflow_json_parser(const std::string &text) : _text(text) {}

    bool parse(workflow_json_value *value, std::string *error)
    {
        skip_ws();
        if (!parse_value(value, error))
        {
            return false;
        }
        skip_ws();
        if (_pos != _text.size())
        {
            return fail(error, "unexpected trailing JSON content");
        }
        return true;
    }

private:
    bool parse_value(workflow_json_value *value, std::string *error)
    {
        if (value == nullptr)
        {
            return fail(error, "internal parser error");
        }
        // Cap recursion depth so a deeply nested payload (e.g. "[[[[...]]]]")
        // arriving over RPC cannot overflow the stack and crash the service.
        struct depth_guard
        {
            explicit depth_guard(size_t &depth) : _depth(depth) { ++_depth; }
            ~depth_guard() { --_depth; }
            size_t &_depth;
        } guard(_depth);
        if (_depth > _max_depth)
        {
            return fail(error, "JSON nesting exceeds maximum depth");
        }
        skip_ws();
        if (_pos >= _text.size())
        {
            return fail(error, "unexpected end of JSON");
        }

        const char c = _text[_pos];
        if (c == '{')
        {
            return parse_object(value, error);
        }
        if (c == '[')
        {
            return parse_array(value, error);
        }
        if (c == '"')
        {
            value->type = workflow_json_value::string_value;
            return parse_string(&value->string, error);
        }
        if (std::isdigit(static_cast<unsigned char>(c)))
        {
            value->type = workflow_json_value::number_value;
            return parse_number(&value->number, error);
        }
        if (match_literal("true"))
        {
            value->type = workflow_json_value::bool_value;
            value->boolean = true;
            return true;
        }
        if (match_literal("false"))
        {
            value->type = workflow_json_value::bool_value;
            value->boolean = false;
            return true;
        }
        if (match_literal("null"))
        {
            value->type = workflow_json_value::null_value;
            return true;
        }
        return fail(error, "unexpected JSON token");
    }

    bool parse_object(workflow_json_value *value, std::string *error)
    {
        value->type = workflow_json_value::object_value;
        value->object.clear();
        ++_pos;
        skip_ws();
        if (consume('}'))
        {
            return true;
        }
        while (true)
        {
            std::string key;
            if (!parse_string(&key, error))
            {
                return false;
            }
            skip_ws();
            if (!consume(':'))
            {
                return fail(error, "expected ':' after JSON object key");
            }
            workflow_json_value child;
            if (!parse_value(&child, error))
            {
                return false;
            }
            value->object[key] = child;
            skip_ws();
            if (consume('}'))
            {
                return true;
            }
            if (!consume(','))
            {
                return fail(error, "expected ',' or '}' in JSON object");
            }
            skip_ws();
        }
    }

    bool parse_array(workflow_json_value *value, std::string *error)
    {
        value->type = workflow_json_value::array_value;
        value->array.clear();
        ++_pos;
        skip_ws();
        if (consume(']'))
        {
            return true;
        }
        while (true)
        {
            workflow_json_value child;
            if (!parse_value(&child, error))
            {
                return false;
            }
            value->array.push_back(child);
            skip_ws();
            if (consume(']'))
            {
                return true;
            }
            if (!consume(','))
            {
                return fail(error, "expected ',' or ']' in JSON array");
            }
            skip_ws();
        }
    }

    bool parse_string(std::string *value, std::string *error)
    {
        if (!consume('"'))
        {
            return fail(error, "expected JSON string");
        }
        std::string result;
        while (_pos < _text.size())
        {
            const char c = _text[_pos++];
            if (c == '"')
            {
                if (value != nullptr)
                {
                    *value = result;
                }
                return true;
            }
            if (c != '\\')
            {
                result.push_back(c);
                continue;
            }
            if (_pos >= _text.size())
            {
                return fail(error, "unterminated JSON escape");
            }
            const char escaped = _text[_pos++];
            switch (escaped)
            {
            case '"':
            case '\\':
            case '/':
                result.push_back(escaped);
                break;
            case 'b':
                result.push_back('\b');
                break;
            case 'f':
                result.push_back('\f');
                break;
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
                return fail(error, "unsupported JSON string escape");
            }
        }
        return fail(error, "unterminated JSON string");
    }

    bool parse_number(uint64_t *value, std::string *error)
    {
        const size_t begin = _pos;
        while (_pos < _text.size() && std::isdigit(static_cast<unsigned char>(_text[_pos])))
        {
            ++_pos;
        }
        uint64_t parsed = 0;
        if (!parse_uint64(_text.substr(begin, _pos - begin), &parsed))
        {
            return fail(error, "invalid unsigned JSON number");
        }
        if (value != nullptr)
        {
            *value = parsed;
        }
        return true;
    }

    bool consume(char c)
    {
        skip_ws();
        if (_pos < _text.size() && _text[_pos] == c)
        {
            ++_pos;
            return true;
        }
        return false;
    }

    bool match_literal(const char *literal)
    {
        const std::string word(literal);
        if (_text.compare(_pos, word.size(), word) == 0)
        {
            _pos += word.size();
            return true;
        }
        return false;
    }

    void skip_ws()
    {
        while (_pos < _text.size() && std::isspace(static_cast<unsigned char>(_text[_pos])))
        {
            ++_pos;
        }
    }

    bool fail(std::string *error, const std::string &message) const
    {
        if (error != nullptr)
        {
            *error = message + " at byte " + std::to_string(_pos);
        }
        return false;
    }

    const std::string &_text;
    size_t _pos = 0;
    size_t _depth = 0;
    static const size_t _max_depth = 256;
};

std::vector<std::string> split_csv(const std::string &value)
{
    std::vector<std::string> result;
    std::string current;
    for (const char c : value)
    {
        if (c == ',')
        {
            const std::string item = trim(current);
            if (!item.empty())
            {
                result.push_back(item);
            }
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    const std::string item = trim(current);
    if (!item.empty())
    {
        result.push_back(item);
    }
    return result;
}

bool is_workflow_option(const std::string &word)
{
    return word == "after" || word == "capability" || word == "policy" || word == "budget_ms" ||
           word == "retry_budget" || word == "retry" ||
           word == "cost_hint" || word == "cost" || word == "latency_ms" || word == "latency" ||
           word == "reliability" || word == "reliability_hint" || word == "state" || word == "artifact";
}

bool parse_uint64(const std::string &value, uint64_t *result)
{
    if (value.empty())
    {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0')
    {
        return false;
    }
    if (result != nullptr)
    {
        *result = static_cast<uint64_t>(parsed);
    }
    return true;
}

const workflow_json_value *json_object_field(const workflow_json_value &object, const std::string &name)
{
    if (object.type != workflow_json_value::object_value)
    {
        return nullptr;
    }
    const std::map<std::string, workflow_json_value>::const_iterator it = object.object.find(name);
    return it == object.object.end() ? nullptr : &it->second;
}

bool json_string_field(const workflow_json_value &object,
                       const std::string &name,
                       bool required,
                       std::string *value,
                       std::string *error)
{
    const workflow_json_value *field = json_object_field(object, name);
    if (field == nullptr)
    {
        if (required && error != nullptr)
        {
            *error = "missing required string field '" + name + "'";
        }
        return !required;
    }
    if (field->type != workflow_json_value::string_value)
    {
        if (error != nullptr)
        {
            *error = "field '" + name + "' must be a string";
        }
        return false;
    }
    if (value != nullptr)
    {
        *value = field->string;
    }
    return true;
}

bool json_uint64_field(const workflow_json_value &object,
                       const std::string &name,
                       bool required,
                       uint64_t *value,
                       std::string *error)
{
    const workflow_json_value *field = json_object_field(object, name);
    if (field == nullptr)
    {
        if (required && error != nullptr)
        {
            *error = "missing required numeric field '" + name + "'";
        }
        return !required;
    }
    if (field->type != workflow_json_value::number_value)
    {
        if (error != nullptr)
        {
            *error = "field '" + name + "' must be an unsigned integer";
        }
        return false;
    }
    if (value != nullptr)
    {
        *value = field->number;
    }
    return true;
}

bool json_uint32_field(const workflow_json_value &object,
                       const std::string &name,
                       bool required,
                       uint32_t *value,
                       uint64_t max_value,
                       std::string *error)
{
    uint64_t parsed = 0;
    if (!json_uint64_field(object, name, required, &parsed, error))
    {
        return false;
    }
    if (json_object_field(object, name) == nullptr)
    {
        return true;
    }
    if (parsed > max_value)
    {
        if (error != nullptr)
        {
            *error = "field '" + name + "' is outside the allowed range";
        }
        return false;
    }
    if (value != nullptr)
    {
        *value = static_cast<uint32_t>(parsed);
    }
    return true;
}

bool json_string_array_field(const workflow_json_value &object,
                             const std::string &name,
                             std::vector<std::string> *values,
                             std::string *error)
{
    const workflow_json_value *field = json_object_field(object, name);
    if (field == nullptr)
    {
        return true;
    }
    if (field->type != workflow_json_value::array_value)
    {
        if (error != nullptr)
        {
            *error = "field '" + name + "' must be an array of strings";
        }
        return false;
    }
    std::vector<std::string> parsed;
    for (size_t i = 0; i < field->array.size(); ++i)
    {
        if (field->array[i].type != workflow_json_value::string_value)
        {
            if (error != nullptr)
            {
                *error = "field '" + name + "' contains a non-string element";
            }
            return false;
        }
        parsed.push_back(field->array[i].string);
    }
    if (values != nullptr)
    {
        *values = parsed;
    }
    return true;
}

bool workflow_node_from_json(const workflow_json_value &value,
                             size_t index,
                             workflow_node *node,
                             std::string *error)
{
    if (value.type != workflow_json_value::object_value)
    {
        if (error != nullptr)
        {
            *error = "workflow node " + std::to_string(index) + " must be an object";
        }
        return false;
    }

    workflow_node parsed;
    if (!json_string_field(value, "id", true, &parsed.id, error) ||
        !json_string_field(value, "action", true, &parsed.action, error) ||
        !json_string_field(value, "prompt", true, &parsed.prompt, error) ||
        !json_string_field(value, "capability", false, &parsed.capability, error) ||
        !json_string_field(value, "state_key", false, &parsed.state_key, error) ||
        !json_string_field(value, "artifact", false, &parsed.artifact, error) ||
        !json_string_array_field(value, "depends_on", &parsed.depends_on, error) ||
        !json_string_array_field(value, "policy_labels", &parsed.policy_labels, error) ||
        !json_uint64_field(value, "budget_ms", false, &parsed.budget_ms, error) ||
        !json_uint32_field(value, "retry_budget", false, &parsed.retry_budget, std::numeric_limits<uint32_t>::max(), error) ||
        !json_uint32_field(value, "cost_hint", false, &parsed.cost_hint, std::numeric_limits<uint32_t>::max(), error) ||
        !json_uint32_field(value, "latency_hint_ms", false, &parsed.latency_hint_ms, std::numeric_limits<uint32_t>::max(), error) ||
        !json_uint32_field(value, "reliability_hint", false, &parsed.reliability_hint, 100, error))
    {
        if (error != nullptr)
        {
            *error = "invalid workflow node " + std::to_string(index) + ": " + *error;
        }
        return false;
    }

    if (parsed.capability.empty())
    {
        parsed.capability = parsed.action == "tool" ? "tool.run" : "model.complete";
    }
    if (node != nullptr)
    {
        *node = parsed;
    }
    return true;
}

std::string join_csv(const std::vector<std::string> &values)
{
    std::ostringstream output;
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (i != 0)
        {
            output << ",";
        }
        output << values[i];
    }
    return output.str();
}

uint32_t effective_cost(const workflow_node &node)
{
    if (node.cost_hint != 0)
    {
        return node.cost_hint;
    }
    return node.capability == "tool.run" || node.action == "tool" ? 1 : 5;
}

uint32_t effective_latency(const workflow_node &node)
{
    if (node.latency_hint_ms != 0)
    {
        return node.latency_hint_ms;
    }
    return node.capability == "tool.run" || node.action == "tool" ? 50 : 500;
}

uint32_t effective_reliability(const workflow_node &node)
{
    if (node.reliability_hint != 0)
    {
        return node.reliability_hint > 100 ? 100 : node.reliability_hint;
    }
    return node.capability == "tool.run" || node.action == "tool" ? 98 : 90;
}

uint64_t optimizer_score(const workflow_node &node)
{
    return static_cast<uint64_t>(effective_latency(node)) +
           static_cast<uint64_t>(effective_cost(node)) * 50 +
           static_cast<uint64_t>(100 - effective_reliability(node)) * 100;
}

bool optimized_node_less(const workflow_node &left, const workflow_node &right)
{
    const uint64_t left_score = optimizer_score(left);
    const uint64_t right_score = optimizer_score(right);
    if (left_score != right_score)
    {
        return left_score < right_score;
    }
    return left.id < right.id;
}

uint32_t node_timeout_ms(const workflow_node &node)
{
    if (node.budget_ms > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(node.budget_ms);
}

workflow_node_status make_node_status(const workflow_node &node,
                                      const std::string &status,
                                      const std::string &output,
                                      const std::string &error)
{
    workflow_node_status record;
    record.node_id = node.id;
    record.action = node.action;
    record.status = status;
    record.output = output;
    record.error = error;
    return record;
}

void record_node_status(std::vector<workflow_node_status> *statuses,
                        const workflow_graph::workflow_progress_observer &progress,
                        const workflow_node &node,
                        const std::string &status,
                        const std::string &output,
                        const std::string &error)
{
    workflow_node_status record = make_node_status(node, status, output, error);
    if (statuses != nullptr)
    {
        statuses->push_back(record);
    }
    if (progress)
    {
        progress(record);
    }
}

void record_blocked_nodes(std::vector<workflow_node_status> *statuses,
                          const workflow_graph::workflow_progress_observer &progress,
                          const std::vector<workflow_node> &ordered,
                          size_t failed_index,
                          const std::string &error)
{
    std::set<std::string> blocked;
    blocked.insert(ordered[failed_index].id);
    for (size_t index = failed_index + 1; index < ordered.size(); ++index)
    {
        const workflow_node &node = ordered[index];
        bool blocked_by_dependency = false;
        for (const std::string &dependency : node.depends_on)
        {
            if (blocked.find(dependency) != blocked.end())
            {
                blocked_by_dependency = true;
                break;
            }
        }
        if (blocked_by_dependency)
        {
            blocked.insert(node.id);
            record_node_status(statuses, progress, node, "blocked", "", error);
        }
    }
}

void record_cancelled_nodes(std::vector<workflow_node_status> *statuses,
                            const workflow_graph::workflow_progress_observer &progress,
                            const std::vector<workflow_node> &ordered,
                            size_t begin,
                            const std::string &error)
{
    for (size_t index = begin; index < ordered.size(); ++index)
    {
        record_node_status(statuses, progress, ordered[index], "cancelled", "", error);
    }
}

} // namespace

bool workflow_graph::load_from_file(const std::string &path, std::string *error)
{
    std::ifstream input(path.c_str());
    if (!input)
    {
        if (error != nullptr)
        {
            *error = "cannot open workflow file: " + path;
        }
        return false;
    }

    return load_from_stream(input, path, error);
}

bool workflow_graph::load_from_text(const std::string &text, const std::string &source_name, std::string *error)
{
    std::istringstream input(text);
    return load_from_stream(input, source_name.empty() ? "<inline>" : source_name, error);
}

bool workflow_graph::load_from_stream(std::istream &input, const std::string &source_name, std::string *error)
{
    _nodes.clear();

    std::ostringstream content;
    content << input.rdbuf();
    const std::string text = content.str();
    const std::string trimmed = trim(text);
    if (!trimmed.empty() && (trimmed[0] == '{' || trimmed[0] == '['))
    {
        return load_from_json_text(text, source_name, error);
    }

    std::istringstream lines(text);
    std::string line;
    size_t line_number = 0;
    while (std::getline(lines, line))
    {
        ++line_number;
        line = trim(line);
        if (line.empty() || line[0] == '#')
        {
            continue;
        }

        const std::vector<std::string> words = split_words(line);
        if (words.size() < 4 || words[0] != "task")
        {
            if (error != nullptr)
            {
                *error = "invalid workflow line " + std::to_string(line_number) + " in " + source_name +
                         ": expected task <id> <ask|plan> <prompt> [after a,b]";
            }
            return false;
        }

        workflow_node node;
        node.id = words[1];
        node.action = words[2];

        std::ostringstream prompt;
        size_t i = 3;
        // An explicit "--" token unambiguously separates the free-text prompt
        // from options, so a prompt may contain words that collide with option
        // keywords (policy, cost, state, ...). Without a delimiter we fall back
        // to scanning for the first option keyword (backward compatible).
        size_t delimiter = words.size();
        for (size_t d = 3; d < words.size(); ++d)
        {
            if (words[d] == "--")
            {
                delimiter = d;
                break;
            }
        }

        bool prompt_started = false;
        for (; i < words.size(); ++i)
        {
            const bool at_boundary =
                delimiter != words.size() ? (i == delimiter) : is_workflow_option(words[i]);
            if (at_boundary)
            {
                break;
            }
            if (prompt_started)
            {
                prompt << " ";
            }
            prompt << words[i];
            prompt_started = true;
        }
        node.prompt = prompt.str();
        if (delimiter != words.size() && i == delimiter)
        {
            ++i; // consume the "--" delimiter before parsing options
        }

        while (i < words.size())
        {
            if (i + 1 >= words.size())
            {
                if (error != nullptr)
                {
                    *error = "invalid workflow line " + std::to_string(line_number) + " in " + source_name +
                             ": option '" + words[i] + "' is missing a value";
                }
                return false;
            }

            const std::string option = words[i];
            const std::string value = words[i + 1];
            if (option == "after")
            {
                node.depends_on = split_csv(value);
            }
            else if (option == "capability")
            {
                node.capability = value;
            }
            else if (option == "policy")
            {
                node.policy_labels = split_csv(value);
            }
            else if (option == "budget_ms")
            {
                if (!parse_uint64(value, &node.budget_ms))
                {
                    if (error != nullptr)
                    {
                        *error = "invalid workflow line " + std::to_string(line_number) + " in " + source_name +
                                 ": budget_ms must be an unsigned integer";
                    }
                    return false;
                }
            }
            else if (option == "retry_budget" || option == "retry")
            {
                uint64_t retry_budget = 0;
                if (!parse_uint64(value, &retry_budget) ||
                    retry_budget > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
                {
                    if (error != nullptr)
                    {
                        *error = "invalid workflow line " + std::to_string(line_number) + " in " + source_name +
                                 ": retry_budget must be an unsigned 32-bit integer";
                    }
                    return false;
                }
                node.retry_budget = static_cast<uint32_t>(retry_budget);
            }
            else if (option == "cost_hint" || option == "cost")
            {
                uint64_t parsed = 0;
                if (!parse_uint64(value, &parsed) ||
                    parsed > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
                {
                    if (error != nullptr)
                    {
                        *error = "invalid workflow line " + std::to_string(line_number) + " in " + source_name +
                                 ": cost_hint must be an unsigned 32-bit integer";
                    }
                    return false;
                }
                node.cost_hint = static_cast<uint32_t>(parsed);
            }
            else if (option == "latency_ms" || option == "latency")
            {
                uint64_t parsed = 0;
                if (!parse_uint64(value, &parsed) ||
                    parsed > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
                {
                    if (error != nullptr)
                    {
                        *error = "invalid workflow line " + std::to_string(line_number) + " in " + source_name +
                                 ": latency_ms must be an unsigned 32-bit integer";
                    }
                    return false;
                }
                node.latency_hint_ms = static_cast<uint32_t>(parsed);
            }
            else if (option == "reliability" || option == "reliability_hint")
            {
                uint64_t parsed = 0;
                if (!parse_uint64(value, &parsed) || parsed > 100)
                {
                    if (error != nullptr)
                    {
                        *error = "invalid workflow line " + std::to_string(line_number) + " in " + source_name +
                                 ": reliability must be an integer from 0 to 100";
                    }
                    return false;
                }
                node.reliability_hint = static_cast<uint32_t>(parsed);
            }
            else if (option == "state")
            {
                node.state_key = value;
            }
            else if (option == "artifact")
            {
                node.artifact = value;
            }
            else
            {
                if (error != nullptr)
                {
                    *error = "invalid workflow line " + std::to_string(line_number) + " in " + source_name +
                             ": unknown option '" + option + "'";
                }
                return false;
            }
            i += 2;
        }

        if (node.capability.empty())
        {
            node.capability = node.action == "tool" ? "tool.run" : "model.complete";
        }

        if (!add_node(node, error))
        {
            if (error != nullptr)
            {
                *error = "invalid workflow line " + std::to_string(line_number) + " in " + source_name + ": " + *error;
            }
            return false;
        }
    }

    std::vector<workflow_node> ordered;
    return topological_order(&ordered, error);
}

bool workflow_graph::load_from_json_text(const std::string &text, const std::string &source_name, std::string *error)
{
    workflow_json_value root;
    workflow_json_parser parser(text);
    std::string parse_error;
    if (!parser.parse(&root, &parse_error))
    {
        if (error != nullptr)
        {
            *error = "invalid workflow JSON in " + source_name + ": " + parse_error;
        }
        return false;
    }

    const workflow_json_value *nodes_value = &root;
    if (root.type == workflow_json_value::object_value)
    {
        nodes_value = json_object_field(root, "nodes");
        if (nodes_value == nullptr)
        {
            if (error != nullptr)
            {
                *error = "invalid workflow JSON in " + source_name + ": missing 'nodes' array";
            }
            return false;
        }
    }
    if (nodes_value->type != workflow_json_value::array_value)
    {
        if (error != nullptr)
        {
            *error = "invalid workflow JSON in " + source_name + ": workflow root must be an array or object with 'nodes'";
        }
        return false;
    }

    for (size_t i = 0; i < nodes_value->array.size(); ++i)
    {
        workflow_node node;
        std::string node_error;
        if (!workflow_node_from_json(nodes_value->array[i], i, &node, &node_error))
        {
            if (error != nullptr)
            {
                *error = "invalid workflow JSON in " + source_name + ": " + node_error;
            }
            return false;
        }
        if (!add_node(node, &node_error))
        {
            if (error != nullptr)
            {
                *error = "invalid workflow JSON in " + source_name + ": node " + std::to_string(i) + ": " + node_error;
            }
            return false;
        }
    }

    std::vector<workflow_node> ordered;
    return topological_order(&ordered, error);
}

bool workflow_graph::add_node(const workflow_node &node, std::string *error)
{
    if (node.id.empty())
    {
        if (error != nullptr)
        {
            *error = "task id is empty";
        }
        return false;
    }
    if (node.action != "ask" && node.action != "plan" && node.action != "tool")
    {
        if (error != nullptr)
        {
            *error = "unsupported action '" + node.action + "'";
        }
        return false;
    }
    if (node.prompt.empty())
    {
        if (error != nullptr)
        {
            *error = "task prompt is empty";
        }
        return false;
    }
    if (!node.state_key.empty() && node.state_key.find('/') == std::string::npos)
    {
        if (error != nullptr)
        {
            *error = "state key must be namespaced as <scope>/<id>";
        }
        return false;
    }
    for (const workflow_node &existing : _nodes)
    {
        if (existing.id == node.id)
        {
            if (error != nullptr)
            {
                *error = "duplicate task id '" + node.id + "'";
            }
            return false;
        }
    }
    _nodes.push_back(node);
    return true;
}

workflow_result workflow_graph::execute(llm_provider &provider,
                                        nucleus_runtime &runtime,
                                        const workflow_tool_runner &tool_runner,
                                        const workflow_progress_observer &progress,
                                        const workflow_cancel_checker &should_cancel,
                                        const workflow_resume_state &resume_state) const
{
    std::vector<workflow_node> ordered;
    std::string error;
    if (!topological_order(&ordered, &error))
    {
        workflow_result result;
        result.ok = false;
        result.error = error;
        return result;
    }

    std::map<std::string, std::string> outputs;
    std::ostringstream transcript;
    std::vector<workflow_node_status> statuses;
    for (size_t index = 0; index < ordered.size(); ++index)
    {
        const workflow_node &node = ordered[index];
        const workflow_resume_state::const_iterator resume_it = resume_state.find(node.id);
        if (resume_it != resume_state.end() &&
            (resume_it->second.status == "completed" || resume_it->second.status == "resumed") &&
            (resume_it->second.action.empty() || resume_it->second.action == node.action))
        {
            const std::string output = resume_it->second.output.empty() ? "<empty>" : resume_it->second.output;
            outputs[node.id] = output;
            transcript << "## " << node.id << " (" << node.action << ")\n"
                       << output << "\n\n";
            record_node_status(&statuses, progress, node, "resumed", output, "");
            continue;
        }

        if (should_cancel && should_cancel())
        {
            workflow_result result;
            result.ok = false;
            result.cancelled = true;
            result.error = "cancelled by request";
            record_cancelled_nodes(&statuses, progress, ordered, index, result.error);
            result.nodes = statuses;
            return result;
        }

        agent_task task;
        task.id = node.id;
        task.name = node.action;
        task.input = node.prompt;
        std::string replay_error;
        if (!runtime.replay_workflow_node_start(task, node.id, node.action, &replay_error))
        {
            workflow_result result;
            result.ok = false;
            result.error = replay_error;
            record_node_status(&statuses, progress, node, "failed", "", result.error);
            record_blocked_nodes(&statuses, progress, ordered, index, result.error);
            result.nodes = statuses;
            return result;
        }
        runtime.record_workflow_node_start(task, node.id, node.action);
        runtime.begin_task(task);
        record_node_status(&statuses, progress, node, "running", "", "");

        std::string output;
        if (node.action == "tool")
        {
            if (!tool_runner)
            {
                runtime.record_workflow_node_finish(task, node.id, "failed");
                runtime.finish_task(task, "failed");
                workflow_result result;
                result.ok = false;
                result.error = "workflow tool node requires a tool runner: " + node.id;
                record_node_status(&statuses, progress, node, "failed", "", result.error);
                record_blocked_nodes(&statuses, progress, ordered, index, result.error);
                result.nodes = statuses;
                return result;
            }

            const std::vector<std::string> words = split_words(node.prompt);
            if (words.empty())
            {
                runtime.record_workflow_node_finish(task, node.id, "failed");
                runtime.finish_task(task, "failed");
                workflow_result result;
                result.ok = false;
                result.error = "workflow tool node missing tool name: " + node.id;
                record_node_status(&statuses, progress, node, "failed", "", result.error);
                record_blocked_nodes(&statuses, progress, ordered, index, result.error);
                result.nodes = statuses;
                return result;
            }
            const tool_result tool =
                tool_runner(words[0], std::vector<std::string>(words.begin() + 1, words.end()), runtime, task, node_timeout_ms(node));
            if (!tool.ok)
            {
                runtime.record_workflow_node_finish(task, node.id, "failed");
                runtime.finish_task(task, "failed");
                workflow_result result;
                result.ok = false;
                result.error = tool.error;
                record_node_status(&statuses, progress, node, "failed", "", result.error);
                record_blocked_nodes(&statuses, progress, ordered, index, result.error);
                result.nodes = statuses;
                return result;
            }
            output = tool.output;
        }
        else
        {
            llm_request request;
            request.task_id = node.id;
            request.system_prompt = "You are an rASN coding agent node. Be concise, explicit, and preserve traceability.";
            request.user_prompt = node.action == "plan" ? "Plan this coding task: " + node.prompt : node.prompt;
            request.timeout_ms = node_timeout_ms(node);
            request.retry_budget = node.retry_budget;
            request.policy_labels = node.policy_labels;
            if (!node.capability.empty())
            {
                request.context.push_back("capability=" + node.capability);
            }
            if (!node.policy_labels.empty())
            {
                request.context.push_back("policy=" + join_csv(node.policy_labels));
            }
            if (node.budget_ms != 0)
            {
                request.context.push_back("budget_ms=" + std::to_string(node.budget_ms));
            }
            if (node.retry_budget != 0)
            {
                request.context.push_back("retry_budget=" + std::to_string(node.retry_budget));
            }
            request.context.push_back("optimizer_cost=" + std::to_string(effective_cost(node)));
            request.context.push_back("optimizer_latency_ms=" + std::to_string(effective_latency(node)));
            request.context.push_back("optimizer_reliability=" + std::to_string(effective_reliability(node)));
            if (!node.state_key.empty())
            {
                request.context.push_back("state_key=" + node.state_key);
            }
            if (!node.artifact.empty())
            {
                request.context.push_back("artifact=" + node.artifact);
            }
            for (const std::string &dep : node.depends_on)
            {
                request.context.push_back(outputs[dep]);
            }

            request.system_prompt = redact_sensitive_text(request.system_prompt);
            request.user_prompt = redact_sensitive_text(request.user_prompt);
            for (std::string &context : request.context)
            {
                context = redact_sensitive_text(context);
            }

            llm_response response = provider.complete(request, runtime);
            response.text = redact_sensitive_text(response.text);
            response.error = redact_sensitive_text(response.error);
            if (!response.ok)
            {
                runtime.record_workflow_node_finish(task, node.id, "failed");
                runtime.finish_task(task, "failed");
                workflow_result result;
                result.ok = false;
                result.error = response.error;
                record_node_status(&statuses, progress, node, "failed", "", result.error);
                record_blocked_nodes(&statuses, progress, ordered, index, result.error);
                result.nodes = statuses;
                return result;
            }
            output = response.text;
        }

        if (output.empty())
        {
            output = "<empty>";
        }

        outputs[node.id] = output;
        transcript << "## " << node.id << " (" << node.action << ")\n"
                   << output << "\n\n";
        runtime.record_workflow_node_finish(task, node.id, "completed");
        runtime.finish_task(task, "ok");
        record_node_status(&statuses, progress, node, "completed", output, "");
    }

    workflow_result result;
    result.ok = true;
    result.text = transcript.str();
    result.nodes = statuses;
    return result;
}

std::string workflow_graph::describe_plan() const
{
    std::vector<workflow_node> ordered;
    std::string error;
    if (!topological_order(&ordered, &error))
    {
        return "invalid workflow: " + error;
    }

    std::ostringstream oss;
    oss << "Executable agent graph (" << ordered.size() << " nodes):\n";
    for (const workflow_node &node : ordered)
    {
        oss << "- " << node.id << " action=" << node.action;
        if (!node.capability.empty())
        {
            oss << " capability=" << node.capability;
        }
        if (!node.depends_on.empty())
        {
            oss << " after=";
            for (size_t i = 0; i < node.depends_on.size(); ++i)
            {
                if (i != 0)
                {
                    oss << ",";
                }
                oss << node.depends_on[i];
            }
        }
        if (!node.policy_labels.empty())
        {
            oss << " policy=";
            for (size_t i = 0; i < node.policy_labels.size(); ++i)
            {
                if (i != 0)
                {
                    oss << ",";
                }
                oss << node.policy_labels[i];
            }
        }
        if (node.budget_ms != 0)
        {
            oss << " budget_ms=" << node.budget_ms;
        }
        if (node.retry_budget != 0)
        {
            oss << " retry_budget=" << node.retry_budget;
        }
        oss << " cost=" << effective_cost(node)
            << " latency_ms=" << effective_latency(node)
            << " reliability=" << effective_reliability(node)
            << " score=" << optimizer_score(node);
        if (!node.state_key.empty())
        {
            oss << " state=" << node.state_key;
        }
        if (!node.artifact.empty())
        {
            oss << " artifact=" << node.artifact;
        }
        oss << "\n";
    }

    std::map<std::string, uint32_t> stage_by_id;
    std::map<uint32_t, std::vector<std::string> > stage_nodes;
    std::map<std::string, uint64_t> critical_latency_by_id;
    uint64_t total_cost = 0;
    uint32_t min_reliability = 100;
    uint64_t critical_latency = 0;
    for (const workflow_node &node : ordered)
    {
        uint32_t stage = 0;
        uint64_t parent_latency = 0;
        for (const std::string &dep : node.depends_on)
        {
            stage = (std::max)(stage, stage_by_id[dep] + 1);
            parent_latency = (std::max)(parent_latency, critical_latency_by_id[dep]);
        }
        stage_by_id[node.id] = stage;
        stage_nodes[stage].push_back(node.id);
        critical_latency_by_id[node.id] = parent_latency + effective_latency(node);
        critical_latency = (std::max)(critical_latency, critical_latency_by_id[node.id]);
        total_cost += effective_cost(node);
        min_reliability = (std::min)(min_reliability, effective_reliability(node));
    }

    size_t max_parallelism = 0;
    for (std::map<uint32_t, std::vector<std::string> >::value_type &stage : stage_nodes)
    {
        std::sort(stage.second.begin(), stage.second.end());
        max_parallelism = (std::max)(max_parallelism, stage.second.size());
    }

    oss << "Optimization plan: stages=" << stage_nodes.size()
        << " max_parallelism=" << max_parallelism
        << " critical_path_ms=" << critical_latency
        << " estimated_cost_units=" << total_cost
        << " min_reliability=" << min_reliability << "\n";
    for (const std::map<uint32_t, std::vector<std::string> >::value_type &stage : stage_nodes)
    {
        oss << "  stage " << stage.first << ": " << join_csv(stage.second) << "\n";
    }
    oss << "Optimizer policy: ready nodes are ordered by latency + cost + reliability penalty while preserving dependencies.";
    return oss.str();
}

bool workflow_graph::topological_order(std::vector<workflow_node> *ordered, std::string *error) const
{
    std::map<std::string, workflow_node> by_id;
    std::map<std::string, int> indegree;
    std::map<std::string, std::vector<std::string> > edges;

    for (const workflow_node &node : _nodes)
    {
        by_id[node.id] = node;
        indegree[node.id] = 0;
    }

    for (const workflow_node &node : _nodes)
    {
        for (const std::string &dep : node.depends_on)
        {
            if (by_id.find(dep) == by_id.end())
            {
                if (error != nullptr)
                {
                    *error = "task '" + node.id + "' depends on missing task '" + dep + "'";
                }
                return false;
            }
            edges[dep].push_back(node.id);
            ++indegree[node.id];
        }
    }

    std::vector<std::string> ready;
    for (const auto &entry : indegree)
    {
        if (entry.second == 0)
        {
            ready.push_back(entry.first);
        }
    }

    std::vector<workflow_node> result;
    while (!ready.empty())
    {
        std::sort(ready.begin(), ready.end(), [&by_id](const std::string &left, const std::string &right) {
            return optimized_node_less(by_id[left], by_id[right]);
        });
        const std::string id = ready.front();
        ready.erase(ready.begin());
        result.push_back(by_id[id]);

        for (const std::string &next : edges[id])
        {
            --indegree[next];
            if (indegree[next] == 0)
            {
                ready.push_back(next);
            }
        }
    }

    if (result.size() != _nodes.size())
    {
        if (error != nullptr)
        {
            *error = "workflow contains a dependency cycle";
        }
        return false;
    }

    if (ordered != nullptr)
    {
        *ordered = result;
    }
    return true;
}

} // namespace rasn
} // namespace dsn
