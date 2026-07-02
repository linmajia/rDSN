#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

struct rasn_session_store_options
{
    std::string directory = "rasn/sessions";
};

struct rasn_session_event
{
    std::string session_id;
    uint64_t sequence = 0;
    std::string kind;
    std::string name;
    std::string value;
    std::string timestamp;
};

struct rasn_session_summary
{
    std::string session_id;
    std::string app_name;
    std::string workspace_root;
    std::string trace_file;
    std::string created_at;
    std::string updated_at;
    std::string last_prompt;
    std::string file_path;
    uint64_t event_count = 0;
};

class rasn_session_store
{
public:
    explicit rasn_session_store(const rasn_session_store_options &options = rasn_session_store_options());

    bool begin_session(const std::string &app_name,
                       const std::string &workspace_root,
                       const std::string &trace_file,
                       const std::string &requested_session_id,
                       rasn_session_summary *summary,
                       std::string *error) const;
    bool append_event(const std::string &session_id,
                      const std::string &kind,
                      const std::string &name,
                      const std::string &value,
                      std::string *error) const;
    bool load_session(const std::string &session_id,
                      rasn_session_summary *summary,
                      std::vector<rasn_session_event> *events,
                      std::string *error) const;
    bool latest_session(rasn_session_summary *summary, std::string *error) const;
    bool list_sessions(std::vector<rasn_session_summary> *sessions, std::string *error) const;

    std::string session_file_path(const std::string &session_id) const;

private:
    bool ensure_directory(std::string *error) const;

    rasn_session_store_options _options;
};

std::string format_session_resume_context(const rasn_session_summary &summary,
                                          const std::vector<rasn_session_event> &events,
                                          uint64_t max_events);

} // namespace rasn
} // namespace dsn
