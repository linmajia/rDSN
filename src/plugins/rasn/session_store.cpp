#include <rasn/session_store.h>

#include <rasn/rasn_core.h>

#include <dsn/cpp/zlocks.h>
#include <dsn/cpp/utils.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>

namespace dsn {
namespace rasn {

namespace {

uint64_t extract_json_uint64_field(const std::string &json, const std::string &field)
{
    const std::string needle = "\"" + field + "\"";
    const std::string::size_type pos = json.find(needle);
    if (pos == std::string::npos)
    {
        return 0;
    }
    const std::string::size_type colon = json.find(':', pos + needle.size());
    if (colon == std::string::npos)
    {
        return 0;
    }
    uint64_t value = 0;
    for (std::string::size_type i = colon + 1; i < json.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(json[i]);
        if (std::isspace(c))
        {
            continue;
        }
        if (!std::isdigit(c))
        {
            break;
        }
        value = value * 10 + static_cast<uint64_t>(json[i] - '0');
    }
    return value;
}

std::string value_field(const std::string &value, const std::string &key)
{
    std::istringstream input(value);
    std::string line;
    const std::string prefix = key + "=";
    while (std::getline(input, line))
    {
        if (line.find(prefix) == 0)
        {
            return line.substr(prefix.size());
        }
    }
    return "";
}

std::string sanitize_session_id(const std::string &id)
{
    std::string safe;
    for (const char c : id)
    {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '-' || c == '_' || c == '.')
        {
            safe.push_back(c);
        }
        else
        {
            safe.push_back('_');
        }
    }
    return safe.empty() ? make_trace_id() : safe;
}

bool has_jsonl_extension(const std::string &path)
{
    return path.size() >= 6 && path.substr(path.size() - 6) == ".jsonl";
}

std::string session_id_from_path(const std::string &path)
{
    std::string name = ::dsn::utils::filesystem::get_file_name(path);
    if (has_jsonl_extension(name))
    {
        name.resize(name.size() - 6);
    }
    return name;
}

bool ensure_directory_recursive(const std::string &directory, std::string *error)
{
    if (directory.empty() || ::dsn::utils::filesystem::directory_exists(directory))
    {
        return true;
    }

    const std::string parent = ::dsn::utils::filesystem::remove_file_name(directory);
    if (!parent.empty() && parent != directory && !::dsn::utils::filesystem::directory_exists(parent) &&
        !ensure_directory_recursive(parent, error))
    {
        return false;
    }

    if (::dsn::utils::filesystem::create_directory(directory) ||
        ::dsn::utils::filesystem::directory_exists(directory))
    {
        return true;
    }
    if (error != nullptr)
    {
        *error = "cannot create session directory: " + directory;
    }
    return false;
}

std::string session_event_json(const rasn_session_event &event)
{
    std::ostringstream output;
    output << "{\"schema_version\":1"
           << ",\"session_id\":\"" << json_escape(event.session_id)
           << "\",\"sequence\":" << event.sequence
           << ",\"kind\":\"" << json_escape(event.kind)
           << "\",\"name\":\"" << json_escape(event.name)
           << "\",\"value\":\"" << json_escape(event.value)
           << "\",\"timestamp\":\"" << json_escape(event.timestamp) << "\"}";
    return output.str();
}

rasn_session_event session_event_from_json(const std::string &line)
{
    rasn_session_event event;
    event.session_id = extract_json_string_field(line, "session_id");
    event.sequence = extract_json_uint64_field(line, "sequence");
    event.kind = extract_json_string_field(line, "kind");
    event.name = extract_json_string_field(line, "name");
    event.value = extract_json_string_field(line, "value");
    event.timestamp = extract_json_string_field(line, "timestamp");
    return event;
}

void apply_event_to_summary(const rasn_session_event &event, rasn_session_summary *summary)
{
    if (summary->session_id.empty())
    {
        summary->session_id = event.session_id;
    }

    if (summary->created_at.empty())
    {
        summary->created_at = event.timestamp;
    }
    summary->updated_at = event.timestamp;
    summary->event_count = (std::max)(summary->event_count, event.sequence);

    if (event.kind == "session" && event.name == "begin")
    {
        summary->app_name = value_field(event.value, "app");
        summary->workspace_root = value_field(event.value, "workspace");
        summary->trace_file = value_field(event.value, "trace");
    }
    else if (event.kind == "prompt")
    {
        summary->last_prompt = event.value;
    }
}

::dsn::service::zlock &session_sequence_cache_lock()
{
    static ::dsn::service::zlock lock;
    return lock;
}

std::map<std::string, uint64_t> &session_sequence_cache()
{
    static std::map<std::string, uint64_t> cache;
    return cache;
}

uint64_t cached_session_sequence(const std::string &cache_key)
{
    ::dsn::service::zauto_lock guard(session_sequence_cache_lock());
    const std::map<std::string, uint64_t>::const_iterator it = session_sequence_cache().find(cache_key);
    return it == session_sequence_cache().end() ? 0 : it->second;
}

void update_cached_session_sequence(const std::string &cache_key, uint64_t sequence)
{
    ::dsn::service::zauto_lock guard(session_sequence_cache_lock());
    uint64_t &cached = session_sequence_cache()[cache_key];
    cached = (std::max)(cached, sequence);
}

std::string event_display_value(const rasn_session_event &event)
{
    std::string value = event.value;
    value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
    std::replace(value.begin(), value.end(), '\n', ' ');
    if (value.size() > 160)
    {
        value.resize(160);
        value += "...";
    }
    return value;
}

} // namespace

rasn_session_store::rasn_session_store(const rasn_session_store_options &options) : _options(options) {}

bool rasn_session_store::begin_session(const std::string &app_name,
                                       const std::string &workspace_root,
                                       const std::string &trace_file,
                                       const std::string &requested_session_id,
                                       rasn_session_summary *summary,
                                       std::string *error) const
{
    const std::string session_id = sanitize_session_id(requested_session_id);
    std::vector<rasn_session_event> events;
    if (::dsn::utils::filesystem::file_exists(session_file_path(session_id)))
    {
        return load_session(session_id, summary, summary == nullptr ? nullptr : &events, error);
    }
    if (!ensure_directory(error))
    {
        return false;
    }

    rasn_session_event event;
    event.session_id = session_id;
    event.sequence = 1;
    event.kind = "session";
    event.name = "begin";
    event.value = "app=" + app_name + "\nworkspace=" + workspace_root + "\ntrace=" + trace_file;
    event.timestamp = now_utc_string();

    std::ofstream output(session_file_path(session_id).c_str(), std::ios::binary | std::ios::app);
    if (!output)
    {
        if (error != nullptr)
        {
            *error = "cannot open session log: " + session_file_path(session_id);
        }
        return false;
    }
    output << session_event_json(event) << "\n";
    output.flush();
    if (!output)
    {
        // A failed write (disk full, quota, I/O error) must not be reported as
        // success: doing so would drop the event yet still advance the cached
        // sequence, opening a silent gap in the session log.
        if (error != nullptr)
        {
            *error = "failed to write session log: " + session_file_path(session_id);
        }
        return false;
    }

    if (summary != nullptr)
    {
        *summary = rasn_session_summary();
        summary->session_id = session_id;
        summary->file_path = session_file_path(session_id);
        apply_event_to_summary(event, summary);
    }
    update_cached_session_sequence(session_file_path(session_id), event.sequence);
    return true;
}

bool rasn_session_store::append_event(const std::string &session_id,
                                      const std::string &kind,
                                      const std::string &name,
                                      const std::string &value,
                                      std::string *error) const
{
    const std::string safe_id = sanitize_session_id(session_id);
    const std::string path = session_file_path(safe_id);
    uint64_t next_sequence = cached_session_sequence(path) + 1;
    if (next_sequence == 1)
    {
        rasn_session_summary summary;
        if (!load_session(safe_id, &summary, nullptr, error))
        {
            return false;
        }
        next_sequence = summary.event_count + 1;
    }

    rasn_session_event event;
    event.session_id = safe_id;
    event.sequence = next_sequence;
    event.kind = kind;
    event.name = name;
    event.value = value;
    event.timestamp = now_utc_string();

    std::ofstream output(path.c_str(), std::ios::binary | std::ios::app);
    if (!output)
    {
        if (error != nullptr)
        {
            *error = "cannot open session log: " + path;
        }
        return false;
    }
    output << session_event_json(event) << "\n";
    output.flush();
    if (!output)
    {
        // Surface write failures instead of advancing the cached sequence past a
        // dropped event (which would leave a silent hole in the session log).
        if (error != nullptr)
        {
            *error = "failed to write session log: " + path;
        }
        return false;
    }
    update_cached_session_sequence(path, event.sequence);
    return true;
}

bool rasn_session_store::load_session(const std::string &session_id,
                                      rasn_session_summary *summary,
                                      std::vector<rasn_session_event> *events,
                                      std::string *error) const
{
    const std::string safe_id = sanitize_session_id(session_id);
    std::ifstream input(session_file_path(safe_id).c_str(), std::ios::binary);
    if (!input)
    {
        if (error != nullptr)
        {
            *error = "session not found: " + safe_id;
        }
        return false;
    }

    rasn_session_summary loaded;
    loaded.session_id = safe_id;
    loaded.file_path = session_file_path(safe_id);
    if (events != nullptr)
    {
        events->clear();
    }

    std::string line;
    while (std::getline(input, line))
    {
        if (trim(line).empty())
        {
            continue;
        }
        rasn_session_event event = session_event_from_json(line);
        if (event.session_id.empty())
        {
            event.session_id = safe_id;
        }
        apply_event_to_summary(event, &loaded);
        update_cached_session_sequence(session_file_path(safe_id), event.sequence);
        if (events != nullptr)
        {
            events->push_back(event);
        }
    }

    if (loaded.event_count == 0)
    {
        if (error != nullptr)
        {
            *error = "session log is empty: " + safe_id;
        }
        return false;
    }
    if (summary != nullptr)
    {
        *summary = loaded;
    }
    return true;
}

bool rasn_session_store::latest_session(rasn_session_summary *summary, std::string *error) const
{
    std::vector<rasn_session_summary> sessions;
    if (!list_sessions(&sessions, error))
    {
        return false;
    }
    if (sessions.empty())
    {
        if (error != nullptr)
        {
            *error = "no sessions found";
        }
        return false;
    }
    if (summary != nullptr)
    {
        *summary = sessions.front();
    }
    return true;
}

bool rasn_session_store::list_sessions(std::vector<rasn_session_summary> *sessions, std::string *error) const
{
    if (sessions == nullptr)
    {
        return false;
    }
    sessions->clear();
    if (!ensure_directory(error))
    {
        return false;
    }

    std::vector<std::string> files;
    if (!::dsn::utils::filesystem::get_subfiles(_options.directory, files, false))
    {
        if (error != nullptr)
        {
            *error = "cannot enumerate session directory: " + _options.directory;
        }
        return false;
    }

    for (const std::string &file : files)
    {
        if (!has_jsonl_extension(file))
        {
            continue;
        }
        rasn_session_summary summary;
        std::string load_error;
        if (load_session(session_id_from_path(file), &summary, nullptr, &load_error))
        {
            sessions->push_back(summary);
        }
    }
    std::sort(sessions->begin(), sessions->end(), [](const rasn_session_summary &left, const rasn_session_summary &right) {
        if (left.updated_at != right.updated_at)
        {
            return left.updated_at > right.updated_at;
        }
        return left.session_id > right.session_id;
    });
    return true;
}

std::string rasn_session_store::session_file_path(const std::string &session_id) const
{
    return ::dsn::utils::filesystem::path_combine(_options.directory, sanitize_session_id(session_id) + ".jsonl");
}

bool rasn_session_store::ensure_directory(std::string *error) const
{
    return ensure_directory_recursive(_options.directory, error);
}

std::string format_session_resume_context(const rasn_session_summary &summary,
                                          const std::vector<rasn_session_event> &events,
                                          uint64_t max_events)
{
    std::ostringstream output;
    output << "resumed rASN session: " << summary.session_id << "\n"
           << "workspace: " << (summary.workspace_root.empty() ? "<unknown>" : summary.workspace_root) << "\n"
           << "events: " << summary.event_count << "\n";
    if (!summary.last_prompt.empty())
    {
        output << "last prompt: " << summary.last_prompt << "\n";
    }

    output << "recent session events:\n";
    const size_t available = events.size();
    const size_t count = static_cast<size_t>((std::min)(max_events, static_cast<uint64_t>(available)));
    const size_t begin = available > count ? available - count : 0;
    for (size_t i = begin; i < available; ++i)
    {
        output << "- " << events[i].kind << "." << events[i].name << ": " << event_display_value(events[i]) << "\n";
    }
    if (available == 0)
    {
        output << "- <no events loaded>\n";
    }
    return output.str();
}

} // namespace rasn
} // namespace dsn
