#include <rasn/state_service.h>

#include <dsn/cpp/clientlet.h>
#include <dsn/cpp/utils.h>
#include <dsn/service_api_cpp.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/locking.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace dsn {
namespace rasn {

namespace {

struct nfs_state_import_config
{
    bool enabled = false;
    std::string remote_host;
    uint16_t remote_port = 0;
    std::string remote_checkpoint_dir;
    std::string remote_checkpoint_file;
    std::string remote_journal_file;
    std::string local_import_dir;
    bool overwrite = true;
    int timeout_ms = 20000;
};

struct nfs_copy_result
{
    ::dsn::error_code error = ::dsn::ERR_OK;
    size_t size = 0;
};

struct state_replica_config
{
    bool enabled = false;
    std::string directory;
    bool recover = true;
};

bool ensure_parent_directory(const std::string &path, std::string *error);
void write_state_record_line(std::ostream &output, const state_record &record);

const char kStateJournalHeader[] = "rasn-state-journal-v1\n";

int open_state_journal(const std::string &path)
{
#if defined(_WIN32)
    int fd = -1;
    const errno_t result = _sopen_s(&fd,
                                    path.c_str(),
                                    _O_BINARY | _O_CREAT | _O_RDWR | _O_APPEND,
                                    _SH_DENYNO,
                                    _S_IREAD | _S_IWRITE);
    return result == 0 ? fd : -1;
#else
    return ::open(path.c_str(),
                  O_CREAT | O_RDWR | O_APPEND,
                  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
#endif
}

int64_t seek_state_journal(int fd, int64_t offset, int origin)
{
#if defined(_WIN32)
    return static_cast<int64_t>(::_lseeki64(fd, offset, origin));
#else
    return static_cast<int64_t>(::lseek(fd, static_cast<off_t>(offset), origin));
#endif
}

bool lock_state_journal(int fd)
{
#if defined(_WIN32)
    return seek_state_journal(fd, 0, SEEK_SET) == 0 && ::_locking(fd, _LK_LOCK, 1) == 0;
#else
    int result = -1;
    do
    {
        result = ::flock(fd, LOCK_EX);
    } while (result != 0 && errno == EINTR);
    return result == 0;
#endif
}

int64_t read_state_journal(int fd, char *data, size_t size)
{
#if defined(_WIN32)
    return static_cast<int64_t>(::_read(fd, data, static_cast<unsigned int>(size)));
#else
    return static_cast<int64_t>(::read(fd, data, size));
#endif
}

int64_t write_state_journal(int fd, const char *data, size_t size)
{
#if defined(_WIN32)
    return static_cast<int64_t>(::_write(fd, data, static_cast<unsigned int>(size)));
#else
    return static_cast<int64_t>(::write(fd, data, size));
#endif
}

bool truncate_state_journal(int fd, int64_t size)
{
#if defined(_WIN32)
    return ::_chsize_s(fd, size) == 0;
#else
    return ::ftruncate(fd, static_cast<off_t>(size)) == 0;
#endif
}

void close_state_journal(int fd)
{
#if defined(_WIN32)
    (void)::_close(fd);
#else
    (void)::close(fd);
#endif
}

bool read_state_journal_at(int fd, int64_t offset, char *data, size_t size)
{
    if (seek_state_journal(fd, offset, SEEK_SET) != offset)
    {
        return false;
    }

    size_t consumed = 0;
    while (consumed < size)
    {
        const int64_t result = read_state_journal(fd, data + consumed, size - consumed);
        if (result < 0 && errno == EINTR)
        {
            continue;
        }
        if (result <= 0)
        {
            return false;
        }
        consumed += static_cast<size_t>(result);
    }
    return true;
}

bool find_complete_state_journal_size(int fd,
                                      int64_t file_size,
                                      const std::string &path,
                                      const std::string &description,
                                      int64_t *complete_size,
                                      std::string *error)
{
    if (file_size == 0)
    {
        *complete_size = 0;
        return true;
    }

    const size_t header_size = sizeof(kStateJournalHeader) - 1;
    const size_t prefix_size =
        static_cast<size_t>((std::min)(file_size, static_cast<int64_t>(header_size)));
    char header[sizeof(kStateJournalHeader) - 1];
    if (!read_state_journal_at(fd, 0, header, prefix_size))
    {
        if (error != nullptr)
        {
            *error = "failed to inspect " + description + " before append: " + path;
        }
        return false;
    }
    if (std::memcmp(header, kStateJournalHeader, prefix_size) != 0)
    {
        if (error != nullptr)
        {
            *error = "invalid " + description + " header before append: " + path;
        }
        return false;
    }
    if (file_size < static_cast<int64_t>(header_size))
    {
        *complete_size = 0;
        return true;
    }

    char last = '\0';
    if (!read_state_journal_at(fd, file_size - 1, &last, 1))
    {
        if (error != nullptr)
        {
            *error = "failed to inspect " + description + " tail before append: " + path;
        }
        return false;
    }
    if (last == '\n')
    {
        *complete_size = file_size;
        return true;
    }

    char buffer[4096];
    int64_t cursor = file_size;
    while (cursor > 0)
    {
        const int64_t begin = (std::max)(int64_t{0}, cursor - static_cast<int64_t>(sizeof(buffer)));
        const size_t length = static_cast<size_t>(cursor - begin);
        if (!read_state_journal_at(fd, begin, buffer, length))
        {
            if (error != nullptr)
            {
                *error = "failed to inspect " + description + " tail before append: " + path;
            }
            return false;
        }
        for (size_t i = length; i > 0; --i)
        {
            if (buffer[i - 1] == '\n')
            {
                *complete_size = begin + static_cast<int64_t>(i);
                return true;
            }
        }
        cursor = begin;
    }

    if (error != nullptr)
    {
        *error = "invalid " + description + " before append: " + path;
    }
    return false;
}

bool append_state_journal_file(const std::string &path,
                               const state_record &record,
                               const std::string &description,
                               std::string *error)
{
    std::ostringstream encoded;
    write_state_record_line(encoded, record);
    if (!encoded)
    {
        if (error != nullptr)
        {
            *error = "failed to encode " + description + " record";
        }
        return false;
    }
    const std::string record_line = encoded.str();

#if defined(_WIN32)
    const size_t max_write_size = static_cast<size_t>((std::numeric_limits<int>::max)());
#else
    const size_t max_write_size = static_cast<size_t>((std::numeric_limits<ssize_t>::max)());
#endif
    const size_t header_size = sizeof(kStateJournalHeader) - 1;
    if (record_line.size() > max_write_size - header_size)
    {
        if (error != nullptr)
        {
            *error = description + " record is too large: " + path;
        }
        return false;
    }

    const int fd = open_state_journal(path);
    if (fd < 0)
    {
        if (error != nullptr)
        {
            *error = "failed to open " + description + " for append: " + path;
        }
        return false;
    }
    if (!lock_state_journal(fd))
    {
        close_state_journal(fd);
        if (error != nullptr)
        {
            *error = "failed to lock " + description + " for append: " + path;
        }
        return false;
    }

    const int64_t file_size = seek_state_journal(fd, 0, SEEK_END);
    int64_t append_offset = 0;
    if (file_size < 0 ||
        !find_complete_state_journal_size(fd, file_size, path, description, &append_offset, error))
    {
        close_state_journal(fd);
        return false;
    }

    if (append_offset != file_size)
    {
        if (!truncate_state_journal(fd, append_offset))
        {
            close_state_journal(fd);
            if (error != nullptr)
            {
                *error = "failed to discard torn " + description + " tail: " + path;
            }
            return false;
        }
        dwarn("discarded torn %s tail path=%s offset=%lld",
              description.c_str(),
              path.c_str(),
              static_cast<long long>(append_offset));
    }

    std::string payload;
    payload.reserve((append_offset == 0 ? header_size : 0) + record_line.size());
    if (append_offset == 0)
    {
        payload.append(kStateJournalHeader, header_size);
    }
    payload.append(record_line);

    int64_t written = -1;
    do
    {
        written = write_state_journal(fd, payload.data(), payload.size());
    } while (written < 0 && errno == EINTR);

    if (written != static_cast<int64_t>(payload.size()))
    {
        const bool rolled_back = truncate_state_journal(fd, append_offset);
        close_state_journal(fd);
        if (error != nullptr)
        {
            *error = "failed to append " + description + ": " + path;
            if (!rolled_back)
            {
                *error += " (partial record could not be rolled back)";
            }
        }
        return false;
    }

    close_state_journal(fd);
    return true;
}

std::string hex_encode(const std::string &value)
{
    std::ostringstream oss;
    for (const unsigned char c : value)
    {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(c);
    }
    return oss.str();
}

bool hex_decode(const std::string &encoded, std::string *value)
{
    if (encoded.size() % 2 != 0)
    {
        return false;
    }

    std::string decoded;
    decoded.reserve(encoded.size() / 2);
    for (size_t i = 0; i < encoded.size(); i += 2)
    {
        unsigned int byte = 0;
        std::istringstream input(encoded.substr(i, 2));
        input >> std::hex >> byte;
        if (!input)
        {
            return false;
        }
        decoded.push_back(static_cast<char>(byte));
    }

    if (value != nullptr)
    {
        *value = decoded;
    }
    return true;
}

std::vector<std::string> split_tab_fields(const std::string &line)
{
    std::vector<std::string> fields;
    std::string current;
    for (const char c : line)
    {
        if (c == '\t')
        {
            fields.push_back(current);
            current.clear();
        }
        else
        {
            current.push_back(c);
        }
    }
    fields.push_back(current);
    return fields;
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

std::string config_string(const std::string &section,
                          const std::string &key,
                          const std::string &fallback,
                          const std::string &description)
{
    const char *value = ::dsn_config_get_value_string(section.c_str(), key.c_str(), fallback.c_str(), description.c_str());
    return value == nullptr ? fallback : value;
}

uint64_t config_uint64(const std::string &section,
                       const std::string &key,
                       uint64_t fallback,
                       const std::string &description)
{
    return ::dsn_config_get_value_uint64(section.c_str(), key.c_str(), fallback, description.c_str());
}

bool config_bool(const std::string &section, const std::string &key, bool fallback, const std::string &description)
{
    return ::dsn_config_get_value_bool(section.c_str(), key.c_str(), fallback, description.c_str());
}

std::string path_parent_or_current(const std::string &path)
{
    const std::string parent = ::dsn::utils::filesystem::remove_file_name(path);
    return parent.empty() ? "." : parent;
}

bool copy_local_file(const std::string &source, const std::string &destination, std::string *error)
{
    if (source == destination)
    {
        return true;
    }

    std::ifstream input(source.c_str(), std::ios::binary);
    if (!input)
    {
        if (error != nullptr)
        {
            *error = "failed to open source state file: " + source;
        }
        return false;
    }

    std::string directory_error;
    if (!ensure_parent_directory(destination, &directory_error))
    {
        if (error != nullptr)
        {
            *error = directory_error;
        }
        return false;
    }

    const std::string temp_path = destination + ".nfs.tmp";
    std::ofstream output(temp_path.c_str(), std::ios::binary | std::ios::trunc);
    if (!output)
    {
        if (error != nullptr)
        {
            *error = "failed to open state file target: " + temp_path;
        }
        return false;
    }
    output << input.rdbuf();
    output.close();
    if (!output)
    {
        ::dsn::utils::filesystem::remove_path(temp_path);
        if (error != nullptr)
        {
            *error = "failed to flush state file target: " + temp_path;
        }
        return false;
    }
    if (!::dsn::utils::filesystem::rename_path(temp_path, destination))
    {
        ::dsn::utils::filesystem::remove_path(temp_path);
        if (error != nullptr)
        {
            *error = "failed to move state file into place: " + destination;
        }
        return false;
    }
    return true;
}

std::string checkpoint_file_name_from_path(const std::string &path)
{
    const std::string file_name = ::dsn::utils::filesystem::get_file_name(path);
    return file_name.empty() ? "rasn-state.chkpt" : file_name;
}

std::string journal_file_name_from_path(const std::string &journal_path)
{
    return ::dsn::utils::filesystem::get_file_name(journal_path);
}

state_replica_config load_state_replica_config()
{
    state_replica_config config;
    config.enabled = config_bool("rasn.state.replica", "enabled", false, "Mirror state checkpoints and journal to a local replica");
    config.directory =
        config_string("rasn.state.replica", "directory", "rasn/state/replica", "Local rASN state replica directory");
    config.recover =
        config_bool("rasn.state.replica", "recover", true, "Recover state from local replica when primary files are absent");
    return config;
}

std::string replica_path_for(const state_replica_config &config, const std::string &source_path)
{
    const std::string file_name = ::dsn::utils::filesystem::get_file_name(source_path);
    return ::dsn::utils::filesystem::path_combine(config.directory, file_name.empty() ? "rasn-state" : file_name);
}

bool copy_checkpoint_to_replica(const std::string &checkpoint_path, std::string *error)
{
    const state_replica_config config = load_state_replica_config();
    if (!config.enabled)
    {
        return true;
    }
    if (config.directory.empty())
    {
        if (error != nullptr)
        {
            *error = "rasn.state.replica is enabled but directory is empty";
        }
        return false;
    }

    const std::string replica_checkpoint = replica_path_for(config, checkpoint_path);
    return copy_local_file(checkpoint_path, replica_checkpoint, error);
}

bool remove_replica_journal(const std::string &journal_path, std::string *error)
{
    const state_replica_config config = load_state_replica_config();
    if (!config.enabled)
    {
        return true;
    }
    if (config.directory.empty())
    {
        if (error != nullptr)
        {
            *error = "rasn.state.replica is enabled but directory is empty";
        }
        return false;
    }

    const std::string replica_journal = replica_path_for(config, journal_path);
    if (::dsn::utils::filesystem::file_exists(replica_journal) &&
        !::dsn::utils::filesystem::remove_path(replica_journal))
    {
        if (error != nullptr)
        {
            *error = "failed to compact replicated state journal: " + replica_journal;
        }
        return false;
    }
    return true;
}

bool mirror_journal_record_to_replica(const std::string &journal_path, const state_record &record, std::string *error)
{
    const state_replica_config config = load_state_replica_config();
    if (!config.enabled)
    {
        return true;
    }
    if (config.directory.empty())
    {
        if (error != nullptr)
        {
            *error = "rasn.state.replica is enabled but directory is empty";
        }
        return false;
    }

    const std::string replica_journal = replica_path_for(config, journal_path);
    if (!ensure_parent_directory(replica_journal, error))
    {
        return false;
    }
    return append_state_journal_file(replica_journal, record, "replicated state journal", error);
}

bool import_state_recovery_files_from_replica(const std::string &checkpoint_path,
                                              const std::string &journal_path,
                                              std::string *error)
{
    const state_replica_config config = load_state_replica_config();
    if (!config.enabled || !config.recover)
    {
        return true;
    }
    if (config.directory.empty())
    {
        if (error != nullptr)
        {
            *error = "rasn.state.replica is enabled but directory is empty";
        }
        return false;
    }

    const std::string replica_checkpoint = replica_path_for(config, checkpoint_path);
    if (!::dsn::utils::filesystem::file_exists(checkpoint_path) &&
        ::dsn::utils::filesystem::file_exists(replica_checkpoint) &&
        !copy_local_file(replica_checkpoint, checkpoint_path, error))
    {
        return false;
    }

    const std::string replica_journal = replica_path_for(config, journal_path);
    if (!::dsn::utils::filesystem::file_exists(journal_path) &&
        ::dsn::utils::filesystem::file_exists(replica_journal) &&
        !copy_local_file(replica_journal, journal_path, error))
    {
        return false;
    }

    return true;
}

nfs_state_import_config load_nfs_state_import_config(const std::string &checkpoint_path,
                                                     const std::string &journal_path)
{
    nfs_state_import_config config;
    config.enabled = config_bool("rasn.state.nfs", "enabled", false, "Enable rDSN NFS import before state recovery");
    config.remote_host = config_string("rasn.state.nfs", "remote_host", "", "Remote rDSN NFS host");
    const uint64_t remote_port = config_uint64("rasn.state.nfs", "remote_port", 0, "Remote rDSN NFS port");
    if (remote_port > (std::numeric_limits<uint16_t>::max)())
    {
        dwarn("rasn.state.nfs.remote_port=%llu exceeds uint16_t port range; disabling NFS import",
              static_cast<unsigned long long>(remote_port));
        config.enabled = false;
    }
    else
    {
        config.remote_port = static_cast<uint16_t>(remote_port);
    }
    config.remote_checkpoint_dir =
        config_string("rasn.state.nfs", "remote_checkpoint_dir", "", "Remote rASN checkpoint directory");
    config.remote_checkpoint_file = config_string("rasn.state.nfs",
                                                  "remote_checkpoint_file",
                                                  checkpoint_file_name_from_path(checkpoint_path),
                                                  "Remote rASN checkpoint file name");
    config.remote_journal_file = config_string(
        "rasn.state.nfs", "remote_journal_file", "", "Optional remote rASN state journal file name");
    config.local_import_dir =
        config_string("rasn.state.nfs", "local_import_dir", "", "Local NFS import directory");
    if (config.local_import_dir.empty())
    {
        config.local_import_dir = path_parent_or_current(checkpoint_path);
    }
    config.overwrite = config_bool("rasn.state.nfs", "overwrite", true, "Overwrite existing local NFS imports");
    const uint64_t timeout_ms =
        config_uint64("rasn.state.nfs", "timeout_ms", 20000, "NFS state import timeout in milliseconds");
    if (timeout_ms > static_cast<uint64_t>((std::numeric_limits<int>::max)()))
    {
        dwarn("rasn.state.nfs.timeout_ms=%llu exceeds int range; using INT_MAX",
              static_cast<unsigned long long>(timeout_ms));
        config.timeout_ms = (std::numeric_limits<int>::max)();
    }
    else
    {
        config.timeout_ms = static_cast<int>(timeout_ms);
    }

    if (config.remote_journal_file.empty() && config_bool("rasn.state.nfs", "import_journal", false, "Import journal from NFS"))
    {
        config.remote_journal_file = journal_file_name_from_path(journal_path);
    }
    return config;
}

bool import_state_recovery_files_from_nfs(const std::string &checkpoint_path,
                                          const std::string &journal_path,
                                          std::string *error)
{
    const nfs_state_import_config config = load_nfs_state_import_config(checkpoint_path, journal_path);
    if (!config.enabled)
    {
        return true;
    }

    if (config.remote_host.empty() || config.remote_port == 0 || config.remote_checkpoint_dir.empty())
    {
        if (error != nullptr)
        {
            *error = "rasn.state.nfs is enabled but remote_host, remote_port, or remote_checkpoint_dir is empty";
        }
        return false;
    }

    std::vector<std::string> files;
    if (!config.remote_checkpoint_file.empty())
    {
        files.push_back(config.remote_checkpoint_file);
    }
    if (!config.remote_journal_file.empty())
    {
        files.push_back(config.remote_journal_file);
    }
    if (files.empty())
    {
        if (error != nullptr)
        {
            *error = "rasn.state.nfs is enabled but no remote checkpoint or journal file is configured";
        }
        return false;
    }

    std::string directory_error;
    if (!ensure_parent_directory(::dsn::utils::filesystem::path_combine(config.local_import_dir, "placeholder"), &directory_error))
    {
        if (error != nullptr)
        {
            *error = directory_error;
        }
        return false;
    }

    ::dsn::rpc_address remote;
    remote.assign_ipv4(config.remote_host.c_str(), config.remote_port);

    auto copy_result = std::make_shared<nfs_copy_result>();
    ::dsn::task_ptr task = ::dsn::file::copy_remote_files(
        remote,
        config.remote_checkpoint_dir,
        files,
        config.local_import_dir,
        config.overwrite,
        LPC_RASN_STATE_NFS_COPY,
        nullptr,
        [copy_result](::dsn::error_code err, size_t size) {
            copy_result->error = err;
            copy_result->size = size;
        });
    if (task == nullptr)
    {
        if (error != nullptr)
        {
            *error = "failed to create rDSN NFS state import task";
        }
        return false;
    }
    if (!task->wait(config.timeout_ms))
    {
        // Best-effort cancel; the completion callback holds a shared_ptr to the
        // result, so it stays alive even if the copy finishes after we return.
        bool finished = false;
        task->cancel(false, &finished);
        if (error != nullptr)
        {
            *error = "timed out importing rASN state through rDSN NFS";
        }
        return false;
    }
    if (task->error() != ::dsn::ERR_OK)
    {
        if (error != nullptr)
        {
            *error = std::string("rDSN NFS state import failed: ") + task->error().to_string();
        }
        return false;
    }
    if (copy_result->error != ::dsn::ERR_OK)
    {
        if (error != nullptr)
        {
            *error = std::string("rDSN NFS state import callback failed: ") + copy_result->error.to_string();
        }
        return false;
    }

    if (!config.remote_checkpoint_file.empty())
    {
        const std::string imported_checkpoint =
            ::dsn::utils::filesystem::path_combine(config.local_import_dir, config.remote_checkpoint_file);
        if (!copy_local_file(imported_checkpoint, checkpoint_path, error))
        {
            return false;
        }
    }
    if (!config.remote_journal_file.empty())
    {
        const std::string imported_journal =
            ::dsn::utils::filesystem::path_combine(config.local_import_dir, config.remote_journal_file);
        if (!copy_local_file(imported_journal, journal_path, error))
        {
            return false;
        }
    }

    dinfo("imported rASN state recovery files via rDSN NFS source=%s:%u dir=%s bytes=%llu",
          config.remote_host.c_str(),
          static_cast<unsigned int>(config.remote_port),
          config.remote_checkpoint_dir.c_str(),
          static_cast<unsigned long long>(copy_result->size));
    return true;
}

void write_state_record_line(std::ostream &output, const state_record &record)
{
    output << record.schema_version << "\t"
           << record.sequence << "\t"
           << hex_encode(record.key) << "\t"
           << hex_encode(record.kind) << "\t"
           << hex_encode(record.scope) << "\t"
           << hex_encode(record.value) << "\n";
}

bool decode_state_record_fields(const std::vector<std::string> &fields,
                                const std::string &source,
                                state_record *record,
                                std::string *error)
{
    if (fields.size() != 6)
    {
        if (error != nullptr)
        {
            *error = "invalid checkpoint record field count: " + source;
        }
        return false;
    }

    state_record decoded;
    uint64_t schema = 0;
    if (!parse_uint64(fields[0], &schema))
    {
        if (error != nullptr)
        {
            *error = "invalid checkpoint record schema: " + source;
        }
        return false;
    }
    decoded.schema_version = static_cast<uint32_t>(schema);
    if (decoded.schema_version != RASN_AGENT_SCHEMA_VERSION)
    {
        if (error != nullptr)
        {
            *error = "unsupported checkpoint record schema: " + source;
        }
        return false;
    }
    if (!parse_uint64(fields[1], &decoded.sequence))
    {
        if (error != nullptr)
        {
            *error = "invalid checkpoint record sequence: " + source;
        }
        return false;
    }
    if (!hex_decode(fields[2], &decoded.key) || !hex_decode(fields[3], &decoded.kind) ||
        !hex_decode(fields[4], &decoded.scope) || !hex_decode(fields[5], &decoded.value))
    {
        if (error != nullptr)
        {
            *error = "invalid checkpoint record encoding: " + source;
        }
        return false;
    }
    if (decoded.key.empty())
    {
        if (error != nullptr)
        {
            *error = "checkpoint contains empty key: " + source;
        }
        return false;
    }
    const std::string::size_type separator = decoded.key.find('/');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= decoded.key.size())
    {
        if (error != nullptr)
        {
            *error = "checkpoint contains non-namespaced key: " + source;
        }
        return false;
    }

    if (record != nullptr)
    {
        *record = decoded;
    }
    return true;
}

bool load_state_checkpoint_file(const std::string &path,
                                std::map<std::string, state_record> *records,
                                uint64_t *last_sequence,
                                std::string *error)
{
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input)
    {
        if (error != nullptr)
        {
            *error = "failed to open checkpoint for recovery: " + path;
        }
        return false;
    }

    std::string header;
    if (!std::getline(input, header))
    {
        if (error != nullptr)
        {
            *error = "empty checkpoint: " + path;
        }
        return false;
    }
    const std::vector<std::string> header_fields = split_tab_fields(header);
    uint64_t loaded_last_sequence = 0;
    if (header_fields.size() != 2 || header_fields[0] != "rasn-state-v1")
    {
        if (error != nullptr)
        {
            *error = "invalid checkpoint header: " + path;
        }
        return false;
    }
    if (!parse_uint64(header_fields[1], &loaded_last_sequence))
    {
        if (error != nullptr)
        {
            *error = "invalid checkpoint sequence: " + path;
        }
        return false;
    }

    std::map<std::string, state_record> loaded;
    std::string line;
    while (std::getline(input, line))
    {
        if (line.empty())
        {
            continue;
        }

        state_record record;
        if (!decode_state_record_fields(split_tab_fields(line), path, &record, error))
        {
            return false;
        }
        loaded[record.key] = record;
        loaded_last_sequence = (std::max)(loaded_last_sequence, record.sequence);
    }
    if (input.bad())
    {
        if (error != nullptr)
        {
            *error = "failed to read checkpoint: " + path;
        }
        return false;
    }

    if (records != nullptr)
    {
        records->swap(loaded);
    }
    if (last_sequence != nullptr)
    {
        *last_sequence = loaded_last_sequence;
    }
    return true;
}

bool write_state_checkpoint_file(const std::string &path,
                                 const std::map<std::string, state_record> &records,
                                 uint64_t last_sequence,
                                 std::string *error)
{
    if (!ensure_parent_directory(path, error))
    {
        return false;
    }

    const std::string temp_path = path + ".tmp";
    std::ofstream output(temp_path.c_str(), std::ios::binary | std::ios::trunc);
    if (!output)
    {
        if (error != nullptr)
        {
            *error = "failed to open checkpoint for write: " + temp_path;
        }
        return false;
    }

    output << "rasn-state-v1\t" << last_sequence << "\n";
    for (const std::map<std::string, state_record>::value_type &entry : records)
    {
        write_state_record_line(output, entry.second);
    }
    output.close();
    if (!output)
    {
        ::dsn::utils::filesystem::remove_path(temp_path);
        if (error != nullptr)
        {
            *error = "failed to flush checkpoint: " + temp_path;
        }
        return false;
    }

    const std::string backup_path = path + ".bak";
    bool has_backup = false;
    if (::dsn::utils::filesystem::file_exists(path))
    {
        if (::dsn::utils::filesystem::file_exists(backup_path) &&
            !::dsn::utils::filesystem::remove_path(backup_path))
        {
            ::dsn::utils::filesystem::remove_path(temp_path);
            if (error != nullptr)
            {
                *error = "failed to remove stale checkpoint backup: " + backup_path;
            }
            return false;
        }
        if (!::dsn::utils::filesystem::rename_path(path, backup_path))
        {
            ::dsn::utils::filesystem::remove_path(temp_path);
            if (error != nullptr)
            {
                *error = "failed to preserve existing checkpoint: " + path;
            }
            return false;
        }
        has_backup = true;
    }

    if (!::dsn::utils::filesystem::rename_path(temp_path, path))
    {
        if (has_backup)
        {
            ::dsn::utils::filesystem::rename_path(backup_path, path);
        }
        ::dsn::utils::filesystem::remove_path(temp_path);
        if (error != nullptr)
        {
            *error = "failed to move checkpoint into place: " + path;
        }
        return false;
    }
    if (has_backup)
    {
        ::dsn::utils::filesystem::remove_path(backup_path);
    }
    return true;
}

bool ensure_parent_directory(const std::string &path, std::string *error)
{
    const std::string directory = ::dsn::utils::filesystem::remove_file_name(path);
    if (directory.empty() || ::dsn::utils::filesystem::directory_exists(directory))
    {
        return true;
    }
    if (::dsn::utils::filesystem::create_directory(directory))
    {
        return true;
    }
    if (error != nullptr)
    {
        *error = "failed to create state directory: " + directory;
    }
    return false;
}

bool valid_schema(uint32_t schema_version)
{
    return schema_version == RASN_AGENT_SCHEMA_VERSION;
}

bool valid_state_key(const std::string &key)
{
    const std::string::size_type separator = key.find('/');
    return separator != std::string::npos && separator > 0 && separator + 1 < key.size();
}

// Namespace-aware prefix match. Keys are namespaced as <scope>/<id>, so a raw
// substring prefix ("team1") must not match a sibling namespace ("team10/..").
// A key matches when the prefix is empty, an exact key, already ends with the
// separator, or the match stops on a path separator boundary.
bool state_key_matches_prefix(const std::string &key, const std::string &prefix)
{
    if (prefix.empty())
    {
        return true;
    }
    if (key.size() < prefix.size() || key.compare(0, prefix.size(), prefix) != 0)
    {
        return false;
    }
    if (key.size() == prefix.size() || prefix.back() == '/')
    {
        return true;
    }
    return key[prefix.size()] == '/';
}

const char kReplicatedStateCheckpointPrefix[] = "rasn-state-checkpoint.";

bool parse_replicated_checkpoint_decree(const std::string &file_name, int64_t *decree)
{
    const size_t prefix_length = sizeof(kReplicatedStateCheckpointPrefix) - 1;
    if (file_name.size() <= prefix_length ||
        file_name.compare(0, prefix_length, kReplicatedStateCheckpointPrefix) != 0)
    {
        return false;
    }

    uint64_t parsed = 0;
    if (!parse_uint64(file_name.substr(prefix_length), &parsed) ||
        parsed > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()))
    {
        return false;
    }
    if (decree != nullptr)
    {
        *decree = static_cast<int64_t>(parsed);
    }
    return true;
}

} // namespace

std::string configured_state_checkpoint_path()
{
    const std::string directory =
        config_string("rasn.state", "checkpoint_dir", "rasn/state", "default rASN state checkpoint directory");
    const std::string file =
        config_string("rasn.state", "checkpoint_file", "rasn-state.chkpt", "default rASN state checkpoint file");
    const std::string file_name = file.empty() ? "rasn-state.chkpt" : file;
    return directory.empty() ? file_name : ::dsn::utils::filesystem::path_combine(directory, file_name);
}

std::string configured_state_journal_path()
{
    const std::string journal =
        config_string("rasn.state", "journal_file", "", "append-only rASN state journal file");
    if (!journal.empty())
    {
        return journal;
    }
    return configured_state_checkpoint_path() + ".journal";
}

state_checkpoint_request configured_state_recovery_request()
{
    state_checkpoint_request request;
    const std::string recover_on_start =
        config_string("rasn.state", "recover_on_start", "", "checkpoint file to recover at startup");
    if (!recover_on_start.empty())
    {
        request.path = recover_on_start;
    }
    return request;
}

bool configured_state_recovery_available(const state_checkpoint_request &request)
{
    if (!request.path.empty())
    {
        return true;
    }

    const std::string checkpoint = configured_state_checkpoint_path();
    const std::string journal = configured_state_journal_path();
    if (::dsn::utils::filesystem::file_exists(checkpoint) || ::dsn::utils::filesystem::file_exists(journal))
    {
        return true;
    }

    const state_replica_config replica = load_state_replica_config();
    if (replica.enabled && replica.recover)
    {
        if (replica.directory.empty())
        {
            return true;
        }
        if (::dsn::utils::filesystem::file_exists(replica_path_for(replica, checkpoint)) ||
            ::dsn::utils::filesystem::file_exists(replica_path_for(replica, journal)))
        {
            return true;
        }
    }

    const nfs_state_import_config nfs = load_nfs_state_import_config(checkpoint, journal);
    return nfs.enabled;
}

state_response state_store::put(const state_record &record)
{
    state_put_request request;
    request.record = record;
    return put(request);
}

state_response state_store::put(const state_put_request &request)
{
    if (!valid_schema(request.schema_version))
    {
        return error_response("state put request has unsupported schema version");
    }
    if (!valid_schema(request.record.schema_version))
    {
        return error_response("state record has unsupported schema version");
    }
    if (request.record.key.empty())
    {
        return error_response("state record missing key");
    }
    if (!valid_state_key(request.record.key))
    {
        return error_response("state key must be namespaced as <scope>/<id>");
    }
    if (request.record.kind == "secret" || request.record.kind == "credential")
    {
        return error_response("state service stores secret references, not raw secret values");
    }

    state_record stored = request.record;
    uint64_t last_sequence = 0;
    {
        ::dsn::service::zauto_lock guard(_lock);
        const std::map<std::string, state_record>::const_iterator existing = _records.find(stored.key);
        if (request.create_only && existing != _records.end())
        {
            state_response response = error_response("state conditional put conflict: key already exists: " + stored.key);
            response.record = existing->second;
            response.last_sequence = _last_sequence;
            return response;
        }
        if (request.check_sequence)
        {
            const uint64_t actual_sequence = existing == _records.end() ? 0 : existing->second.sequence;
            if (actual_sequence != request.expected_sequence)
            {
                state_response response =
                    error_response("state conditional put conflict for key " + stored.key + ": expected sequence " +
                                   std::to_string(request.expected_sequence) + " got " +
                                   std::to_string(actual_sequence));
                if (existing != _records.end())
                {
                    response.record = existing->second;
                }
                response.last_sequence = _last_sequence;
                return response;
            }
        }

        if (stored.sequence == 0)
        {
            if (_last_sequence == (std::numeric_limits<uint64_t>::max)())
            {
                return error_response("state sequence space exhausted for key " + stored.key);
            }
            stored.sequence = _last_sequence + 1;
        }
        else if (request.check_sequence && existing != _records.end() &&
                 stored.sequence <= existing->second.sequence)
        {
            // A caller that manages its own versions (e.g. workflow runs) must
            // advance the version on every conditional write. Refuse a stored
            // sequence that does not move strictly forward so a stale or replayed
            // record cannot silently overwrite a newer committed one.
            return error_response("state conditional put must advance sequence for key " + stored.key +
                                  ": existing " + std::to_string(existing->second.sequence) + " proposed " +
                                  std::to_string(stored.sequence));
        }

        std::string journal_error;
        if (_journal_enabled && !append_journal_record(stored, &journal_error))
        {
            return error_response(journal_error);
        }

        _last_sequence = (std::max)(_last_sequence, stored.sequence);
        _records[stored.key] = stored;
        ++_write_epoch;
        last_sequence = _last_sequence;
    }

    state_response response;
    response.record = stored;
    response.last_sequence = last_sequence;
    dinfo("stored rASN state key=%s kind=%s sequence=%llu",
          stored.key.c_str(),
          stored.kind.c_str(),
          static_cast<unsigned long long>(stored.sequence));
    return response;
}

state_response state_store::get(const state_key_request &request) const
{
    if (!valid_schema(request.schema_version))
    {
        return error_response("state get request has unsupported schema version");
    }
    if (request.key.empty())
    {
        return error_response("state get request missing key");
    }
    if (!valid_state_key(request.key))
    {
        return error_response("state key must be namespaced as <scope>/<id>");
    }

    ::dsn::service::zauto_lock guard(_lock);
    const std::map<std::string, state_record>::const_iterator it = _records.find(request.key);
    if (it == _records.end())
    {
        return error_response("state key not found: " + request.key);
    }

    state_response response;
    response.record = it->second;
    response.records.push_back(it->second);
    response.last_sequence = _last_sequence;
    return response;
}

state_response state_store::query(const state_query_request &request) const
{
    if (!valid_schema(request.schema_version))
    {
        return error_response("state query request has unsupported schema version");
    }

    ::dsn::service::zauto_lock guard(_lock);
    state_response response;
    for (const std::map<std::string, state_record>::value_type &entry : _records)
    {
        if (state_key_matches_prefix(entry.first, request.key_prefix))
        {
            response.records.push_back(entry.second);
        }
    }
    response.last_sequence = _last_sequence;
    return response;
}

state_response state_store::checkpoint(const state_checkpoint_request &request) const
{
    if (!valid_schema(request.schema_version))
    {
        return error_response("state checkpoint request has unsupported schema version");
    }

    const std::string path = request.path.empty() ? default_checkpoint_path() : request.path;
    std::map<std::string, state_record> snapshot;
    uint64_t last_sequence = 0;
    uint64_t snapshot_epoch = 0;
    {
        ::dsn::service::zauto_lock guard(_lock);
        snapshot = _records;
        last_sequence = _last_sequence;
        snapshot_epoch = _write_epoch;
    }

    std::string checkpoint_error;
    if (!write_state_checkpoint_file(path, snapshot, last_sequence, &checkpoint_error))
    {
        return error_response(checkpoint_error);
    }

    if (_journal_enabled)
    {
        const std::string journal_path = default_journal_path();

        // Mirror the checkpoint file to the replica first. This is the slow I/O and
        // must stay outside _lock; copying a checkpoint that is current-or-superseded
        // is always safe because the journal (kept or compacted below) determines
        // which post-snapshot writes recovery must replay.
        std::string replica_error;
        if (!copy_checkpoint_to_replica(path, &replica_error))
        {
            return error_response(replica_error);
        }

        // Only compact (delete) the journals if no write landed since we snapshotted
        // _records. A put that appended to the journal after the snapshot is not in
        // this checkpoint file, so removing the journal would lose it from durable
        // storage. In that case leave both journals for the next checkpoint to fold
        // in (recovery replays checkpoint + journal, so keeping them is safe).
        //
        // The epoch re-check and the removals must happen under a single lock
        // acquisition. A concurrent put() serializes on _lock to append its durable
        // journal record (to both the primary and, via append_journal_record, the
        // mirrored replica journal) and bump _write_epoch atomically; checking the
        // epoch under the lock but unlinking after releasing it would let such a put
        // slip in between and have its just-acknowledged record deleted from either
        // journal. The primary and replica journals are compacted together so the
        // replica can never lose an acked write the primary keeps (and vice versa).
        // remove_path is a fast unlink, so holding _lock across both matches the
        // pre-refactor design where the whole checkpoint ran locked.
        {
            ::dsn::service::zauto_lock guard(_lock);
            if (_write_epoch == snapshot_epoch)
            {
                if (::dsn::utils::filesystem::file_exists(journal_path) &&
                    !::dsn::utils::filesystem::remove_path(journal_path))
                {
                    return error_response("failed to compact state journal: " + journal_path);
                }
                if (!remove_replica_journal(journal_path, &replica_error))
                {
                    return error_response(replica_error);
                }
            }
        }
    }

    state_response response;
    response.last_sequence = last_sequence;
    response.records.reserve(snapshot.size());
    for (const std::map<std::string, state_record>::value_type &entry : snapshot)
    {
        response.records.push_back(entry.second);
    }
    dinfo("checkpointed rASN state records=%llu path=%s",
          static_cast<unsigned long long>(response.records.size()),
          path.c_str());
    return response;
}

state_response state_store::copy_checkpoint(const state_checkpoint_request &request,
                                            const std::string &durable_path)
{
    return import_checkpoint(request, durable_path, false);
}

state_response state_store::replace_from_checkpoint(const state_checkpoint_request &request,
                                                    const std::string &durable_path)
{
    return import_checkpoint(request, durable_path, true);
}

state_response state_store::import_checkpoint(const state_checkpoint_request &request,
                                              const std::string &durable_path,
                                              bool replace)
{
    if (!valid_schema(request.schema_version))
    {
        return error_response("state checkpoint import request has unsupported schema version");
    }
    if (request.path.empty())
    {
        return error_response("state checkpoint import requires a source path");
    }

    std::map<std::string, state_record> imported;
    uint64_t last_sequence = 0;
    std::string import_error;
    if (!load_state_checkpoint_file(request.path, &imported, &last_sequence, &import_error))
    {
        return error_response(import_error);
    }
    if (!durable_path.empty() && durable_path != request.path &&
        !write_state_checkpoint_file(durable_path, imported, last_sequence, &import_error))
    {
        return error_response(import_error);
    }

    if (replace)
    {
        {
            ::dsn::service::zauto_lock guard(_lock);
            _records.swap(imported);
            _last_sequence = last_sequence;
            ++_write_epoch;
        }
        state_response response = query(state_query_request());
        dinfo("replaced rASN state records=%llu checkpoint=%s",
              static_cast<unsigned long long>(response.records.size()),
              request.path.c_str());
        return response;
    }

    state_response response;
    response.last_sequence = last_sequence;
    response.records.reserve(imported.size());
    for (const std::map<std::string, state_record>::value_type &entry : imported)
    {
        response.records.push_back(entry.second);
    }
    return response;
}

state_response state_store::recover(const state_checkpoint_request &request)
{
    if (!valid_schema(request.schema_version))
    {
        return error_response("state recover request has unsupported schema version");
    }

    const std::string path = request.path.empty() ? default_checkpoint_path() : request.path;
    const std::string journal_path = default_journal_path();
    std::map<std::string, state_record> recovered;
    uint64_t last_sequence = 0;

    if (!::dsn::utils::filesystem::file_exists(path) && !::dsn::utils::filesystem::file_exists(journal_path))
    {
        std::string replica_error;
        if (!import_state_recovery_files_from_replica(path, journal_path, &replica_error))
        {
            return error_response(replica_error);
        }
    }

    if (!::dsn::utils::filesystem::file_exists(path) && !::dsn::utils::filesystem::file_exists(journal_path))
    {
        std::string import_error;
        if (!import_state_recovery_files_from_nfs(path, journal_path, &import_error))
        {
            return error_response(import_error);
        }
    }

    if (::dsn::utils::filesystem::file_exists(path))
    {
        std::string checkpoint_error;
        if (!load_state_checkpoint_file(path, &recovered, &last_sequence, &checkpoint_error))
        {
            return error_response(checkpoint_error);
        }
    }
    else if (!::dsn::utils::filesystem::file_exists(journal_path))
    {
        return error_response("failed to open checkpoint for recovery: " + path);
    }

    if (::dsn::utils::filesystem::file_exists(journal_path))
    {
        std::ifstream journal(journal_path.c_str(), std::ios::binary);
        if (!journal)
        {
            return error_response("failed to open state journal for recovery: " + journal_path);
        }

        const std::string expected_header = "rasn-state-journal-v1";
        std::string header;
        bool replay_records = true;
        if (!std::getline(journal, header))
        {
            if (!journal.eof() || journal.bad())
            {
                return error_response("failed to read state journal header: " + journal_path);
            }
            dwarn("ignoring empty state journal left by an interrupted first append: %s",
                  journal_path.c_str());
            replay_records = false;
        }
        else if (header != expected_header)
        {
            const bool torn_header =
                journal.eof() && !journal.bad() && header.size() < expected_header.size() &&
                expected_header.compare(0, header.size(), header) == 0;
            if (!torn_header)
            {
                return error_response("invalid state journal header: " + journal_path);
            }
            dwarn("ignoring torn state journal header left by an interrupted first append: %s",
                  journal_path.c_str());
            replay_records = false;
        }
        else if (journal.eof())
        {
            dwarn("ignoring unterminated state journal header: %s", journal_path.c_str());
            replay_records = false;
        }

        std::string line;
        while (replay_records && std::getline(journal, line))
        {
            if (line.empty())
            {
                continue;
            }

            if (journal.eof())
            {
                // write_state_record_line always terminates complete journal
                // records with '\n'. A non-empty line read with eofbit already set
                // is therefore an unterminated append tail, even if the bytes form
                // a decodable prefix. Drop only that tail; newline-terminated
                // corrupt records are still decoded below and fail recovery.
                dwarn("dropping unterminated trailing record in state journal %s",
                      journal_path.c_str());
                break;
            }

            state_record record;
            std::string decode_error;
            if (!decode_state_record_fields(split_tab_fields(line), journal_path, &record, &decode_error))
            {
                // A newline-terminated line that fails to decode is a complete
                // corrupt record, so stay strict even when it is the final record.
                return error_response(decode_error);
            }
            recovered[record.key] = record;
            last_sequence = (std::max)(last_sequence, record.sequence);
        }
    }

    {
        ::dsn::service::zauto_lock guard(_lock);
        // Merge by max sequence instead of swapping wholesale. A put that
        // committed (and was acked) after we read the checkpoint/journal files
        // but before acquiring this lock has a higher per-key sequence than the
        // recovered copy, so it must be preserved. Recovered records only win
        // when they are strictly newer than the in-memory record (or the key is
        // absent, e.g. startup recovery into an empty store).
        for (std::map<std::string, state_record>::value_type &entry : recovered)
        {
            const std::map<std::string, state_record>::iterator current = _records.find(entry.first);
            if (current == _records.end() || entry.second.sequence > current->second.sequence)
            {
                _records[entry.first] = entry.second;
            }
        }
        _last_sequence = (std::max)(_last_sequence, last_sequence);
    }

    // Report the merged store's records and last_sequence together. query()
    // reads both under _lock, so a concurrent put() preserved by the merge above
    // cannot leave response.records holding a per-key sequence higher than
    // response.last_sequence (which would happen if we returned the pre-merge
    // on-disk last_sequence while records came from the merged in-memory store).
    state_response response = query(state_query_request());
    dinfo("recovered rASN state records=%llu path=%s",
          static_cast<unsigned long long>(response.records.size()),
          path.c_str());
    return response;
}

bool state_store::has_recovery_state(const state_checkpoint_request &request) const
{
    if (!valid_schema(request.schema_version))
    {
        return false;
    }

    const std::string path = request.path.empty() ? default_checkpoint_path() : request.path;
    const std::string journal_path = default_journal_path();
    return ::dsn::utils::filesystem::file_exists(path) || ::dsn::utils::filesystem::file_exists(journal_path);
}

state_response state_store::error_response(const std::string &error) const
{
    state_response response;
    response.ok = false;
    response.error = error;
    return response;
}

std::string state_store::default_checkpoint_path() const
{
    return configured_state_checkpoint_path();
}

std::string state_store::default_journal_path() const
{
    return configured_state_journal_path();
}

bool state_store::append_journal_record(const state_record &record, std::string *error) const
{
    const std::string journal_path = default_journal_path();
    if (!ensure_parent_directory(journal_path, error))
    {
        return false;
    }

    if (!append_state_journal_file(journal_path, record, "state journal", error))
    {
        return false;
    }
    if (!mirror_journal_record_to_replica(journal_path, record, error))
    {
        return false;
    }
    return true;
}

state_store &global_state_store()
{
    static state_store store;
    return store;
}

void rasn_state_rpc_service::open_service(::dsn_gpid gpid)
{
    dinfo("opening rasn.state serverlet");
    this->register_async_rpc_handler(
        RPC_RASN_STATE_PUT, "put", &rasn_state_rpc_service::on_put, gpid);
    this->register_async_rpc_handler(
        RPC_RASN_STATE_PUT_CONDITIONAL,
        "put_conditional",
        &rasn_state_rpc_service::on_put_conditional,
        gpid);
    this->register_async_rpc_handler(
        RPC_RASN_STATE_GET, "get", &rasn_state_rpc_service::on_get, gpid);
    this->register_async_rpc_handler(
        RPC_RASN_STATE_QUERY, "query", &rasn_state_rpc_service::on_query, gpid);
    this->register_async_rpc_handler(
        RPC_RASN_STATE_CHECKPOINT,
        "checkpoint",
        &rasn_state_rpc_service::on_checkpoint,
        gpid);
    this->register_async_rpc_handler(
        RPC_RASN_STATE_RECOVER, "recover", &rasn_state_rpc_service::on_recover, gpid);
}

void rasn_state_rpc_service::close_service(::dsn_gpid gpid)
{
    dinfo("closing rasn.state serverlet");
    this->unregister_rpc_handler(RPC_RASN_STATE_PUT, gpid);
    this->unregister_rpc_handler(RPC_RASN_STATE_PUT_CONDITIONAL, gpid);
    this->unregister_rpc_handler(RPC_RASN_STATE_GET, gpid);
    this->unregister_rpc_handler(RPC_RASN_STATE_QUERY, gpid);
    this->unregister_rpc_handler(RPC_RASN_STATE_CHECKPOINT, gpid);
    this->unregister_rpc_handler(RPC_RASN_STATE_RECOVER, gpid);
}

void rasn_state_rpc_service::on_put(const state_record &request, ::dsn::rpc_replier<state_response> &reply)
{
    reply(_store->put(request));
}

void rasn_state_rpc_service::on_put_conditional(const state_put_request &request,
                                                ::dsn::rpc_replier<state_response> &reply)
{
    reply(_store->put(request));
}

void rasn_state_rpc_service::on_get(const state_key_request &request, ::dsn::rpc_replier<state_response> &reply)
{
    reply(_store->get(request));
}

void rasn_state_rpc_service::on_query(const state_query_request &request, ::dsn::rpc_replier<state_response> &reply)
{
    reply(_store->query(request));
}

void rasn_state_rpc_service::on_checkpoint(const state_checkpoint_request &request, ::dsn::rpc_replier<state_response> &reply)
{
    if (_replicated)
    {
        state_response response;
        response.ok = false;
        response.error =
            "state checkpoints are managed by rDSN replication in replicated mode";
        reply(response);
        return;
    }
    reply(_store->checkpoint(request));
}

void rasn_state_rpc_service::on_recover(const state_checkpoint_request &request, ::dsn::rpc_replier<state_response> &reply)
{
    if (_replicated)
    {
        state_response response;
        response.ok = false;
        response.error =
            "state recovery is managed by rDSN checkpoint learning in replicated mode";
        reply(response);
        return;
    }
    reply(_store->recover(request));
}

std::pair< ::dsn::error_code, state_response>
rasn_state_client::put_sync(const state_record &request,
                            std::chrono::milliseconds timeout,
                            int thread_hash,
                            uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<state_response>(::dsn::rpc::call(
        _server, RPC_RASN_STATE_PUT, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

std::pair< ::dsn::error_code, state_response>
rasn_state_client::put_conditional_sync(const state_put_request &request,
                                        std::chrono::milliseconds timeout,
                                        int thread_hash,
                                        uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<state_response>(::dsn::rpc::call(_server,
                                                                        RPC_RASN_STATE_PUT_CONDITIONAL,
                                                                        request,
                                                                        nullptr,
                                                                        empty_callback,
                                                                        timeout,
                                                                        thread_hash,
                                                                        partition_hash));
}

std::pair< ::dsn::error_code, state_response>
rasn_state_client::get_sync(const state_key_request &request,
                            std::chrono::milliseconds timeout,
                            int thread_hash,
                            uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<state_response>(::dsn::rpc::call(
        _server, RPC_RASN_STATE_GET, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

std::pair< ::dsn::error_code, state_response>
rasn_state_client::query_sync(const state_query_request &request,
                              std::chrono::milliseconds timeout,
                              int thread_hash,
                              uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<state_response>(::dsn::rpc::call(
        _server, RPC_RASN_STATE_QUERY, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

std::pair< ::dsn::error_code, state_response>
rasn_state_client::checkpoint_sync(const state_checkpoint_request &request,
                                   std::chrono::milliseconds timeout,
                                   int thread_hash,
                                   uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<state_response>(::dsn::rpc::call(
        _server, RPC_RASN_STATE_CHECKPOINT, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

std::pair< ::dsn::error_code, state_response>
rasn_state_client::recover_sync(const state_checkpoint_request &request,
                                std::chrono::milliseconds timeout,
                                int thread_hash,
                                uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<state_response>(::dsn::rpc::call(
        _server, RPC_RASN_STATE_RECOVER, request, nullptr, empty_callback, timeout, thread_hash, partition_hash));
}

::dsn::error_code rasn_state_app::start(int argc, char **argv)
{
    const char *recover_on_start_value =
        ::dsn_config_get_value_string("rasn.state", "recover_on_start", "", "checkpoint file to recover at startup");
    const std::string recover_on_start = recover_on_start_value == nullptr ? "" : recover_on_start_value;
    state_checkpoint_request request;
    if (!recover_on_start.empty())
    {
        request.path = recover_on_start;
        state_response response = global_state_store().recover(request);
        if (!response.ok)
        {
            derror("failed to recover configured rASN state path=%s error=%s",
                   recover_on_start.c_str(),
                   response.error.c_str());
            return ::dsn::ERR_INVALID_PARAMETERS;
        }
    }
    else if (global_state_store().has_recovery_state(request))
    {
        state_response response = global_state_store().recover(request);
        if (!response.ok)
        {
            derror("failed to auto-recover rASN state: %s", response.error.c_str());
            return ::dsn::ERR_INVALID_PARAMETERS;
        }
        dinfo("auto-recovered rASN state records=%llu",
              static_cast<unsigned long long>(response.records.size()));
    }
    else
    {
        dinfo("no persisted rASN state found for startup recovery");
    }

    _rpc.open_service();
    return ::dsn::ERR_OK;
}

::dsn::error_code rasn_state_app::stop(bool cleanup)
{
    _rpc.close_service();
    return ::dsn::ERR_OK;
}

rasn_replicated_state_app::rasn_replicated_state_app(::dsn_gpid gpid)
    : ::dsn::replicated_service_app_type_1(gpid),
      _checkpoint_lock(true),
      _store(false),
      _rpc(&_store, true),
      _last_durable_decree(0)
{
}

::dsn::error_code rasn_replicated_state_app::start(int argc, char **argv)
{
    const char *data_dir = ::dsn_get_app_data_dir(get_gpid());
    if (data_dir == nullptr || data_dir[0] == '\0')
    {
        derror("replicated rasn.state partition has no app data directory");
        return ::dsn::ERR_INVALID_PARAMETERS;
    }
    _data_dir = data_dir;

    const ::dsn::error_code recovered = recover_latest_checkpoint();
    if (recovered != ::dsn::ERR_OK)
    {
        return recovered;
    }

    _rpc.open_service(get_gpid());
    dinfo("opened quorum-replicated rasn.state partition=%d checkpoint_decree=%lld",
          get_gpid().u.partition_index,
          static_cast<long long>(_last_durable_decree.load()));
    return ::dsn::ERR_OK;
}

::dsn::error_code rasn_replicated_state_app::stop(bool cleanup)
{
    _rpc.close_service(get_gpid());
    if (cleanup && !_data_dir.empty())
    {
        ::dsn::service::zauto_lock guard(_checkpoint_lock);
        if (::dsn::utils::filesystem::path_exists(_data_dir) &&
            !::dsn::utils::filesystem::remove_path(_data_dir))
        {
            derror("failed to remove replicated rasn.state data directory: %s",
                   _data_dir.c_str());
            return ::dsn::ERR_FILE_OPERATION_FAILED;
        }
    }
    return ::dsn::ERR_OK;
}

::dsn::error_code rasn_replicated_state_app::sync_checkpoint(int64_t last_commit)
{
    if (last_commit < 0)
    {
        return ::dsn::ERR_INVALID_PARAMETERS;
    }

    ::dsn::service::zauto_lock guard(_checkpoint_lock);
    const int64_t current = _last_durable_decree.load();
    if (last_commit < current)
    {
        return ::dsn::ERR_INVALID_STATE;
    }

    const std::string path = checkpoint_path(last_commit);
    if (last_commit == current && ::dsn::utils::filesystem::file_exists(path))
    {
        return ::dsn::ERR_OK;
    }

    // This must remain a synchronous framework checkpoint. The type-1 layer blocks
    // later committed writes while this callback runs, so the store snapshot is
    // exactly the state at last_commit even though checkpoint() releases its lock
    // during file I/O. An asynchronous callback would need an app-level
    // decree/snapshot barrier before it could safely reuse state_store::checkpoint.
    state_checkpoint_request request;
    request.path = path;
    const state_response checkpointed = _store.checkpoint(request);
    if (!checkpointed.ok)
    {
        derror("failed to checkpoint replicated rasn.state decree=%lld: %s",
               static_cast<long long>(last_commit),
               checkpointed.error.c_str());
        return ::dsn::ERR_CHECKPOINT_FAILED;
    }

    _last_durable_decree.store(last_commit);
    return ::dsn::ERR_OK;
}

::dsn::error_code rasn_replicated_state_app::async_checkpoint(int64_t last_commit)
{
    (void)last_commit;
    // Keep the replication framework on its synchronous path; see sync_checkpoint.
    return ::dsn::ERR_NOT_IMPLEMENTED;
}

int64_t rasn_replicated_state_app::get_last_checkpoint_decree()
{
    return _last_durable_decree.load();
}

::dsn::error_code rasn_replicated_state_app::get_checkpoint(int64_t learn_start,
                                                            int64_t local_commit,
                                                            void *learn_request,
                                                            int learn_request_size,
                                                            app_learn_state &state)
{
    const int64_t decree = _last_durable_decree.load();
    if (decree <= 0)
    {
        return ::dsn::ERR_OBJECT_NOT_FOUND;
    }

    const std::string path = checkpoint_path(decree);
    if (!::dsn::utils::filesystem::file_exists(path))
    {
        derror("replicated rasn.state checkpoint is missing: %s", path.c_str());
        return ::dsn::ERR_OBJECT_NOT_FOUND;
    }

    state.from_decree_excluded = 0;
    state.to_decree_included = decree;
    state.files.push_back(path);
    return ::dsn::ERR_OK;
}

::dsn::error_code rasn_replicated_state_app::apply_checkpoint(
    ::dsn_chkpt_apply_mode mode,
    int64_t local_commit,
    const ::dsn_app_learn_state &state)
{
    if ((mode != ::DSN_CHKPT_LEARN && mode != ::DSN_CHKPT_COPY) ||
        state.to_decree_included < 0 || state.file_state_count != 1 ||
        state.files == nullptr || state.files[0] == nullptr || state.files[0][0] == '\0')
    {
        return ::dsn::ERR_INVALID_PARAMETERS;
    }

    ::dsn::service::zauto_lock guard(_checkpoint_lock);
    const int64_t current = _last_durable_decree.load();
    if (mode == ::DSN_CHKPT_COPY && state.to_decree_included < current)
    {
        derror("refusing stale replicated rasn.state checkpoint copy decree=%lld current=%lld",
               static_cast<long long>(state.to_decree_included),
               static_cast<long long>(current));
        return ::dsn::ERR_INVALID_STATE;
    }

    state_checkpoint_request request;
    request.path = state.files[0];
    const std::string durable_path = checkpoint_path(state.to_decree_included);
    const state_response imported =
        mode == ::DSN_CHKPT_LEARN
            ? _store.replace_from_checkpoint(request, durable_path)
            : _store.copy_checkpoint(request, durable_path);
    if (!imported.ok)
    {
        derror("failed to apply replicated rasn.state checkpoint decree=%lld mode=%d: %s",
               static_cast<long long>(state.to_decree_included),
               static_cast<int>(mode),
               imported.error.c_str());
        return ::dsn::ERR_CHECKPOINT_FAILED;
    }

    _last_durable_decree.store(state.to_decree_included);
    return ::dsn::ERR_OK;
}

::dsn::error_code rasn_replicated_state_app::recover_latest_checkpoint()
{
    std::vector<std::string> files;
    if (!::dsn::utils::filesystem::get_subfiles(_data_dir, files, false))
    {
        derror("failed to enumerate replicated rasn.state data directory: %s",
               _data_dir.c_str());
        return ::dsn::ERR_FILE_OPERATION_FAILED;
    }

    std::vector<std::pair<int64_t, std::string>> checkpoints;
    for (const std::string &path : files)
    {
        int64_t decree = 0;
        if (parse_replicated_checkpoint_decree(
                ::dsn::utils::filesystem::get_file_name(path), &decree))
        {
            checkpoints.emplace_back(decree, path);
        }
    }
    if (checkpoints.empty())
    {
        return ::dsn::ERR_OK;
    }

    std::sort(checkpoints.rbegin(), checkpoints.rend());
    for (const std::pair<int64_t, std::string> &checkpoint : checkpoints)
    {
        state_checkpoint_request request;
        request.path = checkpoint.second;
        const state_response recovered = _store.replace_from_checkpoint(request);
        if (recovered.ok)
        {
            _last_durable_decree.store(checkpoint.first);
            return ::dsn::ERR_OK;
        }

        dwarn("ignoring invalid replicated rasn.state checkpoint path=%s error=%s",
              checkpoint.second.c_str(),
              recovered.error.c_str());
    }

    derror("all replicated rasn.state checkpoints are invalid in %s", _data_dir.c_str());
    return ::dsn::ERR_CHECKPOINT_FAILED;
}

std::string rasn_replicated_state_app::checkpoint_path(int64_t decree) const
{
    return ::dsn::utils::filesystem::path_combine(
        _data_dir,
        std::string(kReplicatedStateCheckpointPrefix) + std::to_string(decree));
}

void register_rasn_state_apps()
{
    dassert(::dsn::register_app<rasn_state_app>("rasn.state"),
            "register rasn.state app failed");
    dassert(
        ::dsn::register_app_with_type_1_replication_support<rasn_replicated_state_app>(
            "rasn.state.replicated"),
        "register rasn.state.replicated type-1 app failed");
}

} // namespace rasn
} // namespace dsn
