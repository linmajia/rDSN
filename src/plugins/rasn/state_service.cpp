#include <rasn/state_service.h>

#include <dsn/cpp/clientlet.h>
#include <dsn/cpp/utils.h>
#include <dsn/service_api_cpp.h>
#include <dsn/tool-api/task_spec.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
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

class nfs_attempt_directory_cleanup
{
public:
    explicit nfs_attempt_directory_cleanup(std::string path)
        : _path(std::move(path))
    {
    }

    ~nfs_attempt_directory_cleanup()
    {
        if (!_preserve &&
            ::dsn::utils::filesystem::path_exists(_path) &&
            !::dsn::utils::filesystem::remove_path(_path))
        {
            dwarn("could not remove completed rASN NFS import staging directory: %s",
                  _path.c_str());
        }
    }

    void preserve() { _preserve = true; }

private:
    std::string _path;
    bool _preserve = false;
};

struct state_replica_config
{
    bool enabled = false;
    std::string directory;
    bool recover = true;
};

struct state_journal_delete_prefix
{
    std::string key_prefix;
    uint64_t max_sequence = 0;
    uint64_t operation_sequence = 0;
};

struct state_journal_append_token
{
    int64_t begin_offset = 0;
    int64_t end_offset = 0;
    bool appended = false;
};

bool ensure_parent_directory(const std::string &path, std::string *error);
std::string path_parent_or_current(const std::string &path);
bool write_durable_state_text_file(const std::string &path,
                                   const std::string &content,
                                   const std::string &description,
                                   std::string *error);
void write_state_record_line(std::ostream &output, const state_record &record);
std::string hex_encode(const std::string &value);
bool valid_state_key(const std::string &key);
bool state_key_matches_prefix(const std::string &key, const std::string &prefix);

const char kStateJournalHeader[] = "rasn-state-journal-v1\n";

std::string state_journal_quarantine_marker(const std::string &path)
{
    return path + ".quarantine";
}

std::string state_journal_quarantined_file(const std::string &path)
{
    return path + ".quarantined";
}

std::string state_nfs_import_marker(const std::string &checkpoint_path)
{
    return checkpoint_path + ".nfs-import.pending";
}

bool state_journal_is_quarantined(const std::string &path)
{
    const std::string marker = state_journal_quarantine_marker(path);
    return ::dsn::utils::filesystem::file_exists(marker) ||
           ::dsn::utils::filesystem::file_exists(marker + ".tmp") ||
           ::dsn::utils::filesystem::file_exists(
               state_journal_quarantined_file(path));
}

int open_state_journal(const std::string &path)
{
#if defined(_WIN32)
    const HANDLE handle = ::CreateFileA(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return -1;
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (::GetFileInformationByHandle(handle, &info) == 0 ||
        (info.dwFileAttributes &
         (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
        info.nNumberOfLinks != 1)
    {
        ::CloseHandle(handle);
        return -1;
    }
    const int fd = ::_open_osfhandle(
        reinterpret_cast<intptr_t>(handle),
        _O_BINARY | _O_RDWR | _O_APPEND);
    if (fd < 0)
    {
        ::CloseHandle(handle);
    }
    return fd;
#else
    int flags = O_CREAT | O_RDWR | O_APPEND;
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    const int fd = ::open(
        path.c_str(),
        flags,
        S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
    if (fd < 0)
    {
        return -1;
    }
    struct stat info;
    struct stat path_info;
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_nlink != 1 ||
        ::lstat(path.c_str(), &path_info) != 0 ||
        S_ISLNK(path_info.st_mode) || path_info.st_dev != info.st_dev ||
        path_info.st_ino != info.st_ino)
    {
        (void)::close(fd);
        return -1;
    }
    return fd;
#endif
}

int open_exclusive_state_file(const std::string &path)
{
#if defined(_WIN32)
    int fd = -1;
    const errno_t result = ::_sopen_s(&fd,
                                      path.c_str(),
                                      _O_BINARY | _O_CREAT | _O_EXCL | _O_WRONLY,
                                      _SH_DENYRW,
                                      _S_IREAD | _S_IWRITE);
    return result == 0 ? fd : -1;
#else
    int flags = O_CREAT | O_EXCL | O_WRONLY;
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    return ::open(path.c_str(),
                  flags,
                  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
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

bool write_all_state_file(int fd, const char *data, size_t size)
{
    size_t written_total = 0;
    while (written_total < size)
    {
#if defined(_WIN32)
        const size_t chunk =
            (std::min)(size - written_total,
                       static_cast<size_t>((std::numeric_limits<int>::max)()));
#else
        const size_t chunk = size - written_total;
#endif
        int64_t written = -1;
        do
        {
            written = write_state_journal(
                fd, data + written_total, chunk);
        } while (written < 0 && errno == EINTR);
        if (written <= 0)
        {
            return false;
        }
        written_total += static_cast<size_t>(written);
    }
    return true;
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

int open_state_file_read_only(const std::string &path, int64_t *size)
{
#if defined(_WIN32)
    const HANDLE handle = ::CreateFileA(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return -1;
    }
    BY_HANDLE_FILE_INFORMATION info;
    LARGE_INTEGER file_size;
    if (::GetFileInformationByHandle(handle, &info) == 0 ||
        (info.dwFileAttributes &
         (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
        info.nNumberOfLinks != 1 ||
        ::GetFileSizeEx(handle, &file_size) == 0)
    {
        ::CloseHandle(handle);
        return -1;
    }
    const int fd = ::_open_osfhandle(
        reinterpret_cast<intptr_t>(handle), _O_BINARY | _O_RDONLY);
    if (fd < 0)
    {
        ::CloseHandle(handle);
        return -1;
    }
    if (size != nullptr)
    {
        *size = static_cast<int64_t>(file_size.QuadPart);
    }
    return fd;
#else
    int flags = O_RDONLY;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0)
    {
        return -1;
    }
    struct stat info;
    struct stat path_info;
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_nlink != 1 ||
        ::lstat(path.c_str(), &path_info) != 0 ||
        S_ISLNK(path_info.st_mode) || path_info.st_dev != info.st_dev ||
        path_info.st_ino != info.st_ino)
    {
        close_state_journal(fd);
        return -1;
    }
    if (size != nullptr)
    {
        *size = static_cast<int64_t>(info.st_size);
    }
    return fd;
#endif
}

int open_state_file_for_sync(const std::string &path)
{
#if defined(_WIN32)
    const HANDLE handle = ::CreateFileA(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return -1;
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (::GetFileInformationByHandle(handle, &info) == 0 ||
        (info.dwFileAttributes &
         (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
        info.nNumberOfLinks != 1)
    {
        ::CloseHandle(handle);
        return -1;
    }
    const int fd = ::_open_osfhandle(
        reinterpret_cast<intptr_t>(handle), _O_BINARY | _O_RDWR);
    if (fd < 0)
    {
        ::CloseHandle(handle);
    }
    return fd;
#else
    int flags = O_RDWR;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0)
    {
        return -1;
    }
    struct stat info;
    struct stat path_info;
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_nlink != 1 ||
        ::lstat(path.c_str(), &path_info) != 0 ||
        S_ISLNK(path_info.st_mode) || path_info.st_dev != info.st_dev ||
        path_info.st_ino != info.st_ino)
    {
        close_state_journal(fd);
        return -1;
    }
    return fd;
#endif
}

FILE *open_state_file_stream(const std::string &path)
{
    const int fd = open_state_file_read_only(path, nullptr);
    if (fd < 0)
    {
        return nullptr;
    }
#if defined(_WIN32)
    FILE *stream = ::_fdopen(fd, "rb");
#else
    FILE *stream = ::fdopen(fd, "rb");
#endif
    if (stream == nullptr)
    {
        close_state_journal(fd);
    }
    return stream;
}

enum class state_file_line_result
{
    complete,
    unterminated,
    end,
    error
};

state_file_line_result read_state_file_line(FILE *stream, std::string *line)
{
    line->clear();
    while (true)
    {
        const int character = std::fgetc(stream);
        if (character == '\n')
        {
            return state_file_line_result::complete;
        }
        if (character == EOF)
        {
            if (std::ferror(stream) != 0)
            {
                return state_file_line_result::error;
            }
            return line->empty() ? state_file_line_result::end
                                 : state_file_line_result::unterminated;
        }
        line->push_back(static_cast<char>(character));
    }
}

bool sync_state_file_descriptor(int fd, bool regular_file = true)
{
    (void)regular_file;
    int result = -1;
    do
    {
#if defined(_WIN32)
        // _commit delegates to the Windows durable-file flush primitive.
        result = ::_commit(fd);
#elif defined(__APPLE__)
        result = regular_file ? ::fcntl(fd, F_FULLFSYNC) : ::fsync(fd);
#elif defined(__linux__)
        // fdatasync persists file data plus metadata required to retrieve it
        // (including an appended file size) without forcing unrelated metadata.
        result = regular_file ? ::fdatasync(fd) : ::fsync(fd);
#else
        result = ::fsync(fd);
#endif
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

bool sync_state_file(const std::string &path,
                     const std::string &description,
                     std::string *error)
{
    const int fd = open_state_file_for_sync(path);
    if (fd < 0)
    {
        if (error != nullptr)
        {
            *error = "failed to open " + description + " for durable flush: " +
                     path;
        }
        return false;
    }
    const bool synced = sync_state_file_descriptor(fd);
    close_state_journal(fd);
    if (!synced && error != nullptr)
    {
        *error = "failed to durably flush " + description + ": " + path;
    }
    return synced;
}

#if defined(_WIN32)
bool validate_windows_state_directory_filesystem(
    const std::string &directory,
    const std::string &description,
    std::string *error)
{
    const DWORD required =
        ::GetFullPathNameA(directory.c_str(), 0, nullptr, nullptr);
    std::vector<char> absolute_path(
        required == 0 ? 1 : static_cast<size_t>(required) + 1, '\0');
    if (required == 0 ||
        ::GetFullPathNameA(directory.c_str(),
                           static_cast<DWORD>(absolute_path.size()),
                           absolute_path.data(),
                           nullptr) == 0)
    {
        if (error != nullptr)
        {
            *error = "failed to resolve " + description +
                     " directory for Windows durability validation: " +
                     directory;
        }
        return false;
    }

    std::vector<char> volume_path(32768, '\0');
    if (::GetVolumePathNameA(absolute_path.data(),
                             volume_path.data(),
                             static_cast<DWORD>(volume_path.size())) == 0)
    {
        if (error != nullptr)
        {
            *error = "failed to resolve the Windows volume for " +
                     description + " directory: " + directory;
        }
        return false;
    }

    std::vector<char> filesystem_name(256, '\0');
    if (::GetVolumeInformationA(volume_path.data(),
                                nullptr,
                                0,
                                nullptr,
                                nullptr,
                                nullptr,
                                filesystem_name.data(),
                                static_cast<DWORD>(filesystem_name.size())) == 0)
    {
        if (error != nullptr)
        {
            *error = "failed to identify the Windows filesystem for " +
                     description + " directory: " + directory;
        }
        return false;
    }

    const char *name = filesystem_name.data();
    const bool durable = ::_stricmp(name, "NTFS") == 0 ||
                         ::_stricmp(name, "ReFS") == 0 ||
                         ::_stricmp(name, "CSVFS") == 0;
    if (!durable && error != nullptr)
    {
        *error = "unsupported Windows filesystem for durable " + description +
                 " metadata: " + name +
                 "; use NTFS, ReFS, or CSVFS instead of FAT/exFAT";
    }
    return durable;
}
#endif

bool sync_state_directory(const std::string &directory,
                          const std::string &description,
                          std::string *error)
{
#if defined(_WIN32)
    // Windows has no portable directory-fsync primitive. Supported filesystem
    // capabilities are validated before lifecycle I/O; durable file flushes and
    // MOVEFILE_WRITE_THROUGH provide the per-operation ordering.
    (void)directory;
    (void)description;
    (void)error;
    return true;
#else
    int flags = O_RDONLY;
#if defined(O_DIRECTORY)
    flags |= O_DIRECTORY;
#endif
    const int fd = ::open(directory.c_str(), flags);
    if (fd < 0)
    {
        if (error != nullptr)
        {
            *error = "failed to open " + description +
                     " directory for durable flush: " + directory;
        }
        return false;
    }
    const bool synced = sync_state_file_descriptor(fd, false);
    close_state_journal(fd);
    if (!synced && error != nullptr)
    {
        *error = "failed to durably flush " + description +
                 " directory: " + directory;
    }
    return synced;
#endif
}

bool durable_rename_state_path(const std::string &source,
                               const std::string &destination,
                               const std::string &description,
                               std::string *error)
{
    if (source == destination)
    {
        return true;
    }
#if defined(_WIN32)
    if (::MoveFileExA(source.c_str(),
                      destination.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0)
    {
        if (error != nullptr)
        {
            *error = "failed to durably move " + description + " into place: " +
                     destination + " (win32=" +
                     std::to_string(static_cast<unsigned long>(::GetLastError())) +
                     ")";
        }
        return false;
    }
    return true;
#else
    if (!::dsn::utils::filesystem::rename_path(source, destination))
    {
        if (error != nullptr)
        {
            *error = "failed to move " + description + " into place: " +
                     destination;
        }
        return false;
    }
    const std::string source_directory = path_parent_or_current(source);
    const std::string destination_directory =
        path_parent_or_current(destination);
    if (!sync_state_directory(destination_directory, description, error))
    {
        return false;
    }
    return source_directory == destination_directory ||
           sync_state_directory(source_directory, description, error);
#endif
}

bool durable_remove_state_path(const std::string &path,
                               const std::string &description,
                               std::string *error)
{
    if (!::dsn::utils::filesystem::path_exists(path))
    {
#if !defined(_WIN32)
        const std::string parent = path_parent_or_current(path);
        if (::dsn::utils::filesystem::directory_exists(parent))
        {
            return sync_state_directory(parent, description, error);
        }
#endif
        return true;
    }
#if defined(_WIN32)
    // Persist removal of the live name first. A crash may leave the ignored
    // delete staging file, but can never resurrect the original journal name.
    const std::string delete_path = path + ".delete.tmp";
    if (::MoveFileExA(path.c_str(),
                      delete_path.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0)
    {
        if (error != nullptr)
        {
            *error = "failed to durably remove " + description + ": " + path +
                     " (win32=" +
                     std::to_string(static_cast<unsigned long>(::GetLastError())) +
                     ")";
        }
        return false;
    }
    (void)::DeleteFileA(delete_path.c_str());
    return true;
#else
    if (!::dsn::utils::filesystem::remove_path(path))
    {
        if (error != nullptr)
        {
            *error = "failed to remove " + description + ": " + path;
        }
        return false;
    }
    return sync_state_directory(
        path_parent_or_current(path), description, error);
#endif
}

bool prepare_exclusive_state_staging_file(const std::string &path,
                                          const std::string &description,
                                          int *fd,
                                          std::string *error)
{
#if defined(_WIN32)
    const DWORD attributes = ::GetFileAttributesA(path.c_str());
    bool unsafe_entry = false;
    if (attributes != INVALID_FILE_ATTRIBUTES)
    {
        const HANDLE handle = ::CreateFileA(
            path.c_str(),
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        BY_HANDLE_FILE_INFORMATION info;
        unsafe_entry =
            handle == INVALID_HANDLE_VALUE ||
            ::GetFileInformationByHandle(handle, &info) == 0 ||
            (info.dwFileAttributes &
             (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
            info.nNumberOfLinks != 1;
        if (handle != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(handle);
        }
    }
    else
    {
        const DWORD open_error = ::GetLastError();
        unsafe_entry = open_error != ERROR_FILE_NOT_FOUND &&
                       open_error != ERROR_PATH_NOT_FOUND;
    }
#else
    struct stat entry;
    const int status = ::lstat(path.c_str(), &entry);
    const bool unsafe_entry =
        status == 0 ? (!S_ISREG(entry.st_mode) || entry.st_nlink != 1)
                    : errno != ENOENT;
#endif
    if (unsafe_entry)
    {
        if (error != nullptr)
        {
            *error = "refusing to replace unsafe or uninspectable " +
                     description + " staging path: " + path;
        }
        return false;
    }
#if defined(_WIN32)
    const bool removed = ::DeleteFileA(path.c_str()) != 0;
    const DWORD remove_error = removed ? ERROR_SUCCESS : ::GetLastError();
    const bool absent = remove_error == ERROR_FILE_NOT_FOUND ||
                        remove_error == ERROR_PATH_NOT_FOUND;
#else
    int remove_result = -1;
    do
    {
        remove_result = ::unlink(path.c_str());
    } while (remove_result != 0 && errno == EINTR);
    const bool removed = remove_result == 0;
    const bool absent = !removed && errno == ENOENT;
#endif
    if ((!removed && !absent) ||
        !sync_state_directory(path_parent_or_current(path),
                              description + " stale staging removal",
                              error))
    {
        if (!removed && !absent && error != nullptr)
        {
            *error = "failed to safely remove " + description +
                     " stale staging file: " + path;
        }
        return false;
    }
    const int opened = open_exclusive_state_file(path);
    if (opened < 0)
    {
        if (error != nullptr)
        {
            *error = "failed to exclusively create " + description +
                     " staging file: " + path;
        }
        return false;
    }
    if (fd != nullptr)
    {
        *fd = opened;
    }
    else
    {
        close_state_journal(opened);
    }
    return true;
}

bool cleanup_pending_nfs_state_import(const std::string &checkpoint_path,
                                      const std::string &journal_path,
                                      std::string *error)
{
    const std::string marker = state_nfs_import_marker(checkpoint_path);
    const std::string marker_temp = marker + ".tmp";
    if (!::dsn::utils::filesystem::file_exists(marker) &&
        !::dsn::utils::filesystem::file_exists(marker_temp))
    {
        const std::string checkpoint_parent =
            path_parent_or_current(checkpoint_path);
        const std::string journal_parent =
            path_parent_or_current(journal_path);
        return sync_state_directory(
                   checkpoint_parent, "NFS recovery cleanup", error) &&
               (checkpoint_parent == journal_parent ||
                sync_state_directory(
                    journal_parent, "NFS recovery cleanup", error));
    }

    // Refresh the canonical marker before deleting any partial destination. It
    // remains authoritative across replica/NFS retry and is removed only after
    // the recovered image has parsed successfully.
    if (!write_durable_state_text_file(marker,
                                       "rasn-state-nfs-import-v1\n",
                                       "pending NFS state import marker",
                                       error) ||
        !durable_remove_state_path(
            checkpoint_path, "partial NFS checkpoint import", error) ||
        !durable_remove_state_path(
            journal_path, "partial NFS journal import", error) ||
        !sync_state_directory(path_parent_or_current(marker),
                              "pending NFS state import marker",
                              error))
    {
        return false;
    }
    dwarn("discarded incomplete rASN NFS recovery import checkpoint=%s",
          checkpoint_path.c_str());
    return true;
}

bool resolve_state_path_entry_identity(const std::string &path,
                                       std::string *identity)
{
    const std::string file_name =
        ::dsn::utils::filesystem::get_file_name(path);
    const std::string parent = path_parent_or_current(path);
#if defined(_WIN32)
    const HANDLE parent_handle = ::CreateFileA(
        parent.c_str(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (!file_name.empty() && parent_handle != INVALID_HANDLE_VALUE)
    {
        std::vector<char> resolved(32768, '\0');
        const DWORD length = ::GetFinalPathNameByHandleA(
            parent_handle,
            resolved.data(),
            static_cast<DWORD>(resolved.size()),
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        ::CloseHandle(parent_handle);
        if (length > 0 && length < resolved.size())
        {
            if (identity != nullptr)
            {
                *identity = ::dsn::utils::filesystem::path_combine(
                    std::string(resolved.data(), length), file_name);
            }
            return true;
        }
    }
    else if (parent_handle != INVALID_HANDLE_VALUE)
    {
        ::CloseHandle(parent_handle);
    }
#endif
    std::string absolute_parent;
#if !defined(_WIN32)
    char *resolved_parent = ::realpath(parent.c_str(), nullptr);
    if (resolved_parent != nullptr)
    {
        absolute_parent.assign(resolved_parent);
        std::free(resolved_parent);
    }
#endif
    if (file_name.empty() ||
        (absolute_parent.empty() &&
         !::dsn::utils::filesystem::get_absolute_path(
             parent, absolute_parent)))
    {
        return false;
    }
    if (identity != nullptr)
    {
        *identity =
            ::dsn::utils::filesystem::path_combine(absolute_parent, file_name);
    }
    return true;
}

#if defined(__APPLE__)
std::string fold_ascii_state_path_component(std::string value)
{
    for (char &character : value)
    {
        if (character >= 'A' && character <= 'Z')
        {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

bool state_parent_directory_is_case_sensitive(const std::string &parent)
{
#if defined(_PC_CASE_SENSITIVE)
    errno = 0;
    const long result = ::pathconf(parent.c_str(), _PC_CASE_SENSITIVE);
    return result != 0;
#else
    (void)parent;
    return true;
#endif
}
#endif

struct existing_state_path_identities
{
    std::string preferred;
    std::string fallback;
};

enum class state_path_identity_status
{
    absent,
    resolved,
    uninspectable
};

state_path_identity_status resolve_existing_state_path_identities(
    const std::string &path, existing_state_path_identities &identities)
{
    identities = existing_state_path_identities();
#if defined(_WIN32)
    const HANDLE handle = ::CreateFileA(
        path.c_str(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        const DWORD error = ::GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                   ? state_path_identity_status::absent
                   : state_path_identity_status::uninspectable;
    }

    // FileIdInfo is Windows 8+ and carries the full 128-bit identifier used by
    // ReFS. Keep the Vista-targeted build compatible with older SDK headers.
    struct state_file_id_info
    {
        ULONGLONG volume_serial_number;
        BYTE file_id[16];
    };
    const FILE_INFO_BY_HANDLE_CLASS file_id_info_class =
        static_cast<FILE_INFO_BY_HANDLE_CLASS>(18);
    state_file_id_info extended_info = {};
    bool resolved = false;
    if (::GetFileInformationByHandleEx(handle,
                                       file_id_info_class,
                                       &extended_info,
                                       sizeof(extended_info)) != 0)
    {
        resolved = true;
        identities.preferred = "windows-file-id-128:";
        identities.preferred.append(
            reinterpret_cast<const char *>(
                &extended_info.volume_serial_number),
            sizeof(extended_info.volume_serial_number));
        identities.preferred.append(
            reinterpret_cast<const char *>(extended_info.file_id),
            sizeof(extended_info.file_id));
    }

    // Older systems and some filesystem redirectors reject FileIdInfo with
    // provider-specific errors. Always collect the universally supported legacy
    // identity too, so mixed extended/fallback results share a comparable key.
    BY_HANDLE_FILE_INFORMATION info;
    if (::GetFileInformationByHandle(handle, &info) != 0)
    {
        resolved = true;
        identities.fallback = "windows-file-index-64:";
        identities.fallback.append(
            reinterpret_cast<const char *>(&info.dwVolumeSerialNumber),
            sizeof(info.dwVolumeSerialNumber));
        identities.fallback.append(
            reinterpret_cast<const char *>(&info.nFileIndexHigh),
            sizeof(info.nFileIndexHigh));
        identities.fallback.append(
            reinterpret_cast<const char *>(&info.nFileIndexLow),
            sizeof(info.nFileIndexLow));
    }
    ::CloseHandle(handle);
    // A preferred-only result cannot be compared with a fallback-only alias.
    // The legacy query is supported on every accepted Windows filesystem, so
    // fail closed rather than silently omit this existing path from an index.
    if (!identities.preferred.empty() && identities.fallback.empty())
    {
        return state_path_identity_status::uninspectable;
    }
    return resolved ? state_path_identity_status::resolved
                    : state_path_identity_status::uninspectable;
#else
    struct stat info;
    if (::stat(path.c_str(), &info) != 0)
    {
        return errno == ENOENT
                   ? state_path_identity_status::absent
                   : state_path_identity_status::uninspectable;
    }
    identities.preferred = "posix-file-id:";
    identities.preferred.append(
        reinterpret_cast<const char *>(&info.st_dev), sizeof(info.st_dev));
    identities.preferred.append(
        reinterpret_cast<const char *>(&info.st_ino), sizeof(info.st_ino));
    return state_path_identity_status::resolved;
#endif
}

bool state_path_identities_overlap(
    const existing_state_path_identities &left,
    const existing_state_path_identities &right)
{
    if (!left.preferred.empty() && !right.preferred.empty())
    {
        return left.preferred == right.preferred;
    }
    return !left.fallback.empty() && !right.fallback.empty() &&
           left.fallback == right.fallback;
}

enum class state_path_relation
{
    distinct,
    same,
    uninspectable
};

std::string normalize_state_path_entry_identity(std::string identity)
{
#if defined(__APPLE__)
    const std::string parent = path_parent_or_current(identity);
    if (!state_parent_directory_is_case_sensitive(parent))
    {
        identity = ::dsn::utils::filesystem::path_combine(
            parent,
            fold_ascii_state_path_component(
                ::dsn::utils::filesystem::get_file_name(identity)));
    }
#endif
    return identity;
}

struct state_path_entry_identity_less
{
    bool operator()(const std::string &left, const std::string &right) const
    {
#if defined(_WIN32)
        return ::_stricmp(left.c_str(), right.c_str()) < 0;
#else
        return left < right;
#endif
    }
};

bool state_path_entries_refer_to_same_file(const std::string &left,
                                           const std::string &right)
{
    if (left == right)
    {
        return true;
    }
    std::string absolute_left;
    std::string absolute_right;
    if (!resolve_state_path_entry_identity(left, &absolute_left) ||
        !resolve_state_path_entry_identity(right, &absolute_right))
    {
        return false;
    }
#if defined(_WIN32)
    return ::_stricmp(absolute_left.c_str(), absolute_right.c_str()) == 0;
#else
    return normalize_state_path_entry_identity(std::move(absolute_left)) ==
           normalize_state_path_entry_identity(std::move(absolute_right));
#endif
}

state_path_relation compare_state_paths(const std::string &left,
                                        const std::string &right)
{
    if (left == right)
    {
        return state_path_relation::same;
    }

    existing_state_path_identities left_identities;
    existing_state_path_identities right_identities;
    const state_path_identity_status left_status =
        resolve_existing_state_path_identities(left, left_identities);
    const state_path_identity_status right_status =
        resolve_existing_state_path_identities(right, right_identities);
    if (left_status == state_path_identity_status::uninspectable ||
        right_status == state_path_identity_status::uninspectable)
    {
        return state_path_relation::uninspectable;
    }
    if (left_status == state_path_identity_status::resolved &&
        right_status == state_path_identity_status::resolved &&
        state_path_identities_overlap(left_identities, right_identities))
    {
        return state_path_relation::same;
    }

    return state_path_entries_refer_to_same_file(left, right)
               ? state_path_relation::same
               : state_path_relation::distinct;
}

bool write_durable_state_text_file(const std::string &path,
                                   const std::string &content,
                                   const std::string &description,
                                   std::string *error)
{
    if (!ensure_parent_directory(path, error))
    {
        return false;
    }
    const std::string temp_path = path + ".tmp";
    int fd = -1;
    if (!prepare_exclusive_state_staging_file(
            temp_path, description, &fd, error))
    {
        return false;
    }
    const bool written =
        write_all_state_file(fd, content.data(), content.size());
    const bool synced = written && sync_state_file_descriptor(fd);
    close_state_journal(fd);
    if (!synced)
    {
        ::dsn::utils::filesystem::remove_path(temp_path);
        if (error != nullptr && error->empty())
        {
            *error = written ? "failed to flush " + description + ": " + temp_path
                             : "failed to write " + description + ": " + temp_path;
        }
        return false;
    }
    if (!durable_rename_state_path(
            temp_path, path, description, error))
    {
        ::dsn::utils::filesystem::remove_path(temp_path);
        return false;
    }
    return true;
}

bool persist_external_state_journal_quarantine(
    const std::string &journal_path,
    const std::string &reason,
    std::string *error)
{
    const std::string marker =
        state_journal_quarantine_marker(journal_path);
    if (write_durable_state_text_file(
            marker, reason + "\n", "state journal quarantine marker", error))
    {
        return true;
    }
    if (::dsn::utils::filesystem::file_exists(journal_path))
    {
        return durable_rename_state_path(
            journal_path,
            state_journal_quarantined_file(journal_path),
            "quarantined state journal",
            error);
    }
    return false;
}

void fail_stop_quarantined_state_journal(const std::string &journal_path,
                                         const std::string &reason)
{
    std::string quarantine_error;
    if (!persist_external_state_journal_quarantine(
            journal_path, reason, &quarantine_error))
    {
        derror("failed to persist state journal quarantine path=%s error=%s",
               journal_path.c_str(),
               quarantine_error.c_str());
    }
    derror("state journal entered fail-stop quarantine path=%s reason=%s",
           journal_path.c_str(),
           reason.c_str());
    ::dsn_exit(1);
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

bool append_state_journal_line(const std::string &path,
                               const std::string &record_line,
                               const std::string &description,
                               std::string *error,
                               state_journal_append_token *token = nullptr,
                               std::atomic<bool> *quarantine_latch = nullptr)
{
    if (state_journal_is_quarantined(path))
    {
        if (quarantine_latch != nullptr)
        {
            quarantine_latch->store(true, std::memory_order_release);
        }
        if (error != nullptr)
        {
            *error = "state journal is quarantined and requires operator repair: " +
                     path;
        }
        return false;
    }

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
    if (state_journal_is_quarantined(path))
    {
        if (quarantine_latch != nullptr)
        {
            quarantine_latch->store(true, std::memory_order_release);
        }
        close_state_journal(fd);
        if (error != nullptr)
        {
            *error = "state journal is quarantined and requires operator repair: " +
                     path;
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
    if (append_offset >
        (std::numeric_limits<int64_t>::max)() -
            static_cast<int64_t>(payload.size()))
    {
        close_state_journal(fd);
        if (error != nullptr)
        {
            *error = description + " exceeds the supported journal size: " + path;
        }
        return false;
    }

    size_t written_total = 0;
    while (written_total < payload.size())
    {
        int64_t written = -1;
        do
        {
            written = write_state_journal(
                fd,
                payload.data() + written_total,
                payload.size() - written_total);
        } while (written < 0 && errno == EINTR);
        if (written <= 0)
        {
            break;
        }
        written_total += static_cast<size_t>(written);
    }

    if (written_total != payload.size())
    {
        const bool rolled_back =
            truncate_state_journal(fd, append_offset) &&
            sync_state_file_descriptor(fd);
        close_state_journal(fd);
        if (!rolled_back)
        {
            fail_stop_quarantined_state_journal(
                path,
                "partial " + description +
                    " append could not be durably rolled back");
            return false;
        }
        if (error != nullptr)
        {
            *error = "failed to append " + description + ": " + path;
        }
        return false;
    }

    bool durable = sync_state_file_descriptor(fd);
    if (durable && append_offset == 0)
    {
        durable = sync_state_directory(
            path_parent_or_current(path), description, error);
    }
    if (!durable)
    {
        const bool rolled_back =
            truncate_state_journal(fd, append_offset) &&
            sync_state_file_descriptor(fd);
        close_state_journal(fd);
        if (!rolled_back)
        {
            fail_stop_quarantined_state_journal(
                path,
                description + " durable flush failed and rollback was unprovable");
            return false;
        }
        if (error != nullptr && error->empty())
        {
            *error = "failed to durably append " + description + ": " + path;
        }
        return false;
    }

    close_state_journal(fd);
    if (token != nullptr)
    {
        token->begin_offset = append_offset;
        token->end_offset = append_offset + static_cast<int64_t>(payload.size());
        token->appended = true;
    }
    return true;
}

bool rollback_state_journal_append(const std::string &path,
                                   const state_journal_append_token &token,
                                   const std::string &description,
                                   std::string *error)
{
    if (!token.appended)
    {
        return true;
    }
    const int fd = open_state_journal(path);
    if (fd < 0)
    {
        if (error != nullptr)
        {
            *error = "failed to reopen " + description + " for rollback: " + path;
        }
        return false;
    }
    if (!lock_state_journal(fd))
    {
        close_state_journal(fd);
        if (error != nullptr)
        {
            *error = "failed to lock " + description + " for rollback: " + path;
        }
        return false;
    }

    const int64_t current_size = seek_state_journal(fd, 0, SEEK_END);
    if (current_size != token.end_offset ||
        !truncate_state_journal(fd, token.begin_offset) ||
        !sync_state_file_descriptor(fd))
    {
        close_state_journal(fd);
        if (error != nullptr)
        {
            *error = "cannot prove " + description +
                     " rollback at expected end offset: " + path;
        }
        return false;
    }
    close_state_journal(fd);
    return true;
}

bool append_state_journal_file(const std::string &path,
                               const state_record &record,
                               const std::string &description,
                               std::string *error,
                               state_journal_append_token *token = nullptr,
                               std::atomic<bool> *quarantine_latch = nullptr)
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
    return append_state_journal_line(
        path,
        encoded.str(),
        description,
        error,
        token,
        quarantine_latch);
}

bool append_state_delete_prefix_file(const std::string &path,
                                     const state_delete_prefix_request &request,
                                     uint64_t operation_sequence,
                                     const std::string &description,
                                     std::string *error,
                                     state_journal_append_token *token = nullptr,
                                     std::atomic<bool> *quarantine_latch = nullptr)
{
    std::ostringstream encoded;
    encoded << "delete-prefix\t" << operation_sequence << "\t" << request.max_sequence
            << "\t" << hex_encode(request.key_prefix) << "\n";
    if (!encoded)
    {
        if (error != nullptr)
        {
            *error = "failed to encode " + description + " delete-prefix record";
        }
        return false;
    }
    return append_state_journal_line(
        path,
        encoded.str(),
        description,
        error,
        token,
        quarantine_latch);
}

bool append_state_sequence_barrier_file(
    const std::string &path,
    const state_sequence_barrier_request &request,
    const std::string &description,
    std::string *error,
    state_journal_append_token *token = nullptr,
    std::atomic<bool> *quarantine_latch = nullptr)
{
    std::ostringstream encoded;
    encoded << "sequence-barrier\t" << request.minimum_sequence << "\n";
    if (!encoded)
    {
        if (error != nullptr)
        {
            *error = "failed to encode " + description + " sequence-barrier record";
        }
        return false;
    }
    return append_state_journal_line(
        path,
        encoded.str(),
        description,
        error,
        token,
        quarantine_latch);
}

bool rollback_primary_journal_after_mirror_failure(
    const std::string &journal_path,
    const state_journal_append_token &token,
    const std::string &mirror_error,
    std::string *error)
{
    std::string rollback_error;
    if (!rollback_state_journal_append(
            journal_path, token, "state journal", &rollback_error))
    {
        const std::string quarantine_reason =
            "mirror=" + mirror_error + "; rollback=" + rollback_error;
        std::string quarantine_error;
        const bool journal_marked = append_state_journal_line(
            journal_path,
            "quarantine\t" + hex_encode(quarantine_reason) + "\n",
            "state journal quarantine",
            &quarantine_error);
        std::string external_error;
        const bool externally_quarantined =
            persist_external_state_journal_quarantine(
                journal_path, quarantine_reason, &external_error);
        derror("cannot roll back primary state journal after mirror failure: mirror=%s rollback=%s",
               mirror_error.c_str(),
               rollback_error.c_str());
        if (!journal_marked)
        {
            derror("failed to append in-journal quarantine marker path=%s error=%s",
                   journal_path.c_str(),
                   quarantine_error.c_str());
        }
        if (!externally_quarantined)
        {
            derror("failed to persist external state journal quarantine path=%s error=%s",
                   journal_path.c_str(),
                   external_error.c_str());
        }
        ::dsn_exit(1);
        return false;
    }
    if (error != nullptr)
    {
        *error = mirror_error;
    }
    return false;
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
    if (!parent.empty())
    {
#if defined(_WIN32)
        if (parent.size() == 2 && parent[1] == ':')
        {
            return parent + "\\";
        }
#endif
        return parent;
    }
#if defined(_WIN32)
    if (!path.empty() && (path[0] == '/' || path[0] == '\\'))
#else
    if (!path.empty() && path[0] == '/')
#endif
    {
        return path.substr(0, 1);
    }
    return ".";
}

bool copy_local_file(const std::string &source, const std::string &destination, std::string *error)
{
    const state_path_relation relation =
        compare_state_paths(source, destination);
    if (relation == state_path_relation::uninspectable)
    {
        if (error != nullptr)
        {
            *error = "failed to compare state file identities: " + source +
                     " and " + destination;
        }
        return false;
    }
    if (relation == state_path_relation::same)
    {
        return sync_state_file(source, "state file copy", error);
    }

    int64_t source_size = 0;
    const int source_fd =
        open_state_file_read_only(source, &source_size);
    if (source_fd < 0)
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
        close_state_journal(source_fd);
        if (error != nullptr)
        {
            *error = directory_error;
        }
        return false;
    }

    const std::string temp_path = destination + ".nfs.tmp";
    int output_fd = -1;
    if (!prepare_exclusive_state_staging_file(
            temp_path, "state file copy", &output_fd, error))
    {
        close_state_journal(source_fd);
        return false;
    }
    char buffer[65536];
    int64_t remaining = source_size;
    bool copied = true;
    while (remaining > 0)
    {
        const size_t chunk = static_cast<size_t>(
            (std::min)(remaining,
                       static_cast<int64_t>(sizeof(buffer))));
        int64_t read = -1;
        do
        {
            read = read_state_journal(source_fd, buffer, chunk);
        } while (read < 0 && errno == EINTR);
        if (read != static_cast<int64_t>(chunk))
        {
            copied = false;
            break;
        }
        if (!write_all_state_file(
                output_fd, buffer, chunk))
        {
            copied = false;
            break;
        }
        remaining -= static_cast<int64_t>(chunk);
    }
    char extra = '\0';
    if (copied)
    {
        int64_t read = -1;
        do
        {
            read = read_state_journal(source_fd, &extra, 1);
        } while (read < 0 && errno == EINTR);
        copied = read == 0;
    }
    close_state_journal(source_fd);
    const bool synced = copied && sync_state_file_descriptor(output_fd);
    close_state_journal(output_fd);
    if (!synced)
    {
        ::dsn::utils::filesystem::remove_path(temp_path);
        if (error != nullptr)
        {
            *error = copied
                         ? "failed to flush copied state file: " + temp_path
                         : "failed to copy complete source state file: " + source;
        }
        return false;
    }
    if (!durable_rename_state_path(
            temp_path, destination, "state file copy", error))
    {
        ::dsn::utils::filesystem::remove_path(temp_path);
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

bool state_replica_journal_is_quarantined(const std::string &journal_path)
{
    const state_replica_config config = load_state_replica_config();
    return config.enabled && !config.directory.empty() &&
           state_journal_is_quarantined(replica_path_for(config, journal_path));
}

struct named_state_path
{
    std::string path;
    std::string description;
};

bool state_path_is_link(const std::string &path)
{
#if defined(_WIN32)
    const DWORD attributes = ::GetFileAttributesA(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
    {
        return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    }
    const HANDLE handle = ::CreateFileA(
        path.c_str(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info;
    const bool reparse =
        ::GetFileInformationByHandle(handle, &info) != 0 &&
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    ::CloseHandle(handle);
    return reparse;
#else
    struct stat link_info;
    return ::lstat(path.c_str(), &link_info) == 0 &&
           S_ISLNK(link_info.st_mode);
#endif
}

void add_checkpoint_lifecycle_paths(const std::string &checkpoint_path,
                                    const std::string &description,
                                    bool copied,
                                    std::vector<named_state_path> *paths)
{
    paths->push_back({checkpoint_path, description});
    if (copied)
    {
        paths->push_back(
            {checkpoint_path + ".nfs.tmp", description + " copy staging"});
        return;
    }
    paths->push_back({checkpoint_path + ".tmp", description + " staging"});
    paths->push_back(
        {checkpoint_path + ".delete.tmp", description + " delete staging"});
    paths->push_back(
        {checkpoint_path + ".nfs.tmp", description + " import staging"});
    paths->push_back(
        {checkpoint_path + ".bak", description + " legacy backup"});
    paths->push_back({checkpoint_path + ".bak.nfs.tmp",
                      description + " legacy backup staging"});
    paths->push_back({checkpoint_path + ".bak.delete.tmp",
                      description + " legacy backup delete staging"});
    const std::string nfs_marker = state_nfs_import_marker(checkpoint_path);
    paths->push_back({nfs_marker, description + " NFS import marker"});
    paths->push_back(
        {nfs_marker + ".tmp", description + " NFS import marker staging"});
    paths->push_back({nfs_marker + ".delete.tmp",
                      description + " NFS import marker delete staging"});
}

void add_journal_lifecycle_paths(const std::string &journal_path,
                                 const std::string &description,
                                 std::vector<named_state_path> *paths)
{
    paths->push_back({journal_path, description});
    paths->push_back(
        {journal_path + ".quarantine", description + " quarantine marker"});
    paths->push_back({journal_path + ".quarantine.tmp",
                      description + " quarantine staging"});
    paths->push_back(
        {journal_path + ".quarantined", description + " quarantined journal"});
    paths->push_back(
        {journal_path + ".delete.tmp", description + " delete staging"});
    paths->push_back(
        {journal_path + ".nfs.tmp", description + " import staging"});
}

bool validate_distinct_state_paths(const std::vector<named_state_path> &paths,
                                   std::string *error)
{
    for (const named_state_path &candidate : paths)
    {
        if (state_path_is_link(candidate.path))
        {
            if (error != nullptr)
            {
                *error = "state storage path is a link or reparse point: " +
                         candidate.description + "=" + candidate.path;
            }
            return false;
        }
    }

    std::map<std::string, size_t> exact_paths;
    std::map<std::string, size_t> preferred_path_identities;
    // Full IDs remain authoritative when both handles provide them. These two
    // fallback indexes only bridge pairs where at least one handle lacks one.
    std::map<std::string, size_t> all_fallback_path_identities;
    std::map<std::string, size_t> fallback_only_path_identities;
    std::map<std::string, size_t, state_path_entry_identity_less>
        entry_identities;
    size_t first_overlap = paths.size();
    size_t second_overlap = paths.size();
    for (size_t index = 0; index < paths.size(); ++index)
    {
        const named_state_path &candidate = paths[index];
        size_t overlap = paths.size();
        const std::map<std::string, size_t>::const_iterator exact =
            exact_paths.find(candidate.path);
        if (exact != exact_paths.end())
        {
            overlap = exact->second;
        }
        exact_paths.emplace(candidate.path, index);

        existing_state_path_identities identities;
        const state_path_identity_status identity_status =
            resolve_existing_state_path_identities(candidate.path, identities);
        if (identity_status == state_path_identity_status::uninspectable)
        {
            if (error != nullptr)
            {
                *error = "failed to inspect state storage path identity: " +
                         candidate.description + "=" + candidate.path;
            }
            return false;
        }
        if (identity_status == state_path_identity_status::resolved)
        {
            if (!identities.preferred.empty())
            {
                const std::map<std::string, size_t>::const_iterator existing =
                    preferred_path_identities.find(identities.preferred);
                if (existing != preferred_path_identities.end() &&
                    existing->second < overlap)
                {
                    overlap = existing->second;
                }
                if (!identities.fallback.empty())
                {
                    const std::map<std::string, size_t>::const_iterator fallback =
                        fallback_only_path_identities.find(identities.fallback);
                    if (fallback != fallback_only_path_identities.end() &&
                        fallback->second < overlap)
                    {
                        overlap = fallback->second;
                    }
                }
            }
            else if (!identities.fallback.empty())
            {
                const std::map<std::string, size_t>::const_iterator fallback =
                    all_fallback_path_identities.find(identities.fallback);
                if (fallback != all_fallback_path_identities.end() &&
                    fallback->second < overlap)
                {
                    overlap = fallback->second;
                }
            }

            if (!identities.preferred.empty())
            {
                preferred_path_identities.emplace(identities.preferred, index);
            }
            if (!identities.fallback.empty())
            {
                all_fallback_path_identities.emplace(identities.fallback,
                                                     index);
                if (identities.preferred.empty())
                {
                    fallback_only_path_identities.emplace(identities.fallback,
                                                          index);
                }
            }
        }

        std::string identity;
        if (resolve_state_path_entry_identity(candidate.path, &identity))
        {
            identity =
                normalize_state_path_entry_identity(std::move(identity));
            const std::map<std::string,
                           size_t,
                           state_path_entry_identity_less>::const_iterator
                entry = entry_identities.find(identity);
            if (entry != entry_identities.end() &&
                entry->second < overlap)
            {
                overlap = entry->second;
            }
            entry_identities.emplace(std::move(identity), index);
        }

        if (overlap != paths.size() &&
            (overlap < first_overlap ||
             (overlap == first_overlap && index < second_overlap)))
        {
            first_overlap = overlap;
            second_overlap = index;
        }
    }
    if (first_overlap == paths.size())
    {
        return true;
    }
    if (error != nullptr)
    {
        *error = "state storage paths overlap: " +
                 paths[first_overlap].description + "=" +
                 paths[first_overlap].path + " and " +
                 paths[second_overlap].description + "=" +
                 paths[second_overlap].path;
    }
    return false;
}

bool validate_state_storage_filesystems(
    const std::vector<named_state_path> &paths, std::string *error)
{
#if defined(_WIN32)
    std::vector<std::string> checked_directories;
    for (const named_state_path &path : paths)
    {
        const std::string directory = path_parent_or_current(path.path);
        if (std::find(checked_directories.begin(),
                      checked_directories.end(),
                      directory) != checked_directories.end())
        {
            continue;
        }
        if (!validate_windows_state_directory_filesystem(
                directory, path.description, error))
        {
            return false;
        }
        checked_directories.push_back(directory);
    }
#else
    (void)paths;
    (void)error;
#endif
    return true;
}

bool validate_checkpoint_lifecycle_target(const std::string &checkpoint_path,
                                          std::string *error)
{
    if (!ensure_parent_directory(checkpoint_path, error))
    {
        return false;
    }
    std::vector<named_state_path> paths;
    add_checkpoint_lifecycle_paths(
        checkpoint_path, "checkpoint", false, &paths);
    return validate_distinct_state_paths(paths, error) &&
           validate_state_storage_filesystems(paths, error);
}

bool collect_state_storage_paths(const std::string &checkpoint_path,
                                 bool include_replica_checkpoint,
                                 std::vector<named_state_path> *paths,
                                 std::string *error)
{
    const std::string journal_path = configured_state_journal_path();
    if (!ensure_parent_directory(checkpoint_path, error) ||
        !ensure_parent_directory(journal_path, error))
    {
        return false;
    }

    add_checkpoint_lifecycle_paths(
        checkpoint_path, "primary checkpoint", false, paths);
    add_journal_lifecycle_paths(
        journal_path, "primary journal", paths);
    const state_replica_config config = load_state_replica_config();
    if (config.enabled)
    {
        if (config.directory.empty())
        {
            if (error != nullptr)
            {
                *error =
                    "rasn.state.replica is enabled but directory is empty";
            }
            return false;
        }
        const std::string replica_checkpoint =
            replica_path_for(config, checkpoint_path);
        const std::string replica_journal =
            replica_path_for(config, journal_path);
        if (!ensure_parent_directory(replica_checkpoint, error) ||
            !ensure_parent_directory(replica_journal, error))
        {
            return false;
        }
        if (include_replica_checkpoint)
        {
            add_checkpoint_lifecycle_paths(
                replica_checkpoint, "replica checkpoint", true, paths);
        }
        add_journal_lifecycle_paths(
            replica_journal, "replica journal", paths);
    }
    return true;
}

bool validate_state_checkpoint_target(const std::string &checkpoint_path,
                                      bool include_replica_checkpoint,
                                      std::string *error)
{
    std::vector<named_state_path> paths;
    return collect_state_storage_paths(
               checkpoint_path, include_replica_checkpoint, &paths, error) &&
           validate_distinct_state_paths(paths, error) &&
           validate_state_storage_filesystems(paths, error);
}

bool validate_custom_checkpoint_target(const std::string &checkpoint_path,
                                       std::string *error)
{
    if (!validate_state_checkpoint_target(
            checkpoint_path, false, error))
    {
        return false;
    }

    const std::string configured_checkpoint =
        configured_state_checkpoint_path();
    if (!ensure_parent_directory(configured_checkpoint, error))
    {
        return false;
    }
    std::vector<named_state_path> custom_paths;
    std::vector<named_state_path> protected_paths;
    add_checkpoint_lifecycle_paths(
        checkpoint_path, "custom checkpoint", false, &custom_paths);
    add_checkpoint_lifecycle_paths(configured_checkpoint,
                                   "configured recovery checkpoint",
                                   false,
                                   &protected_paths);
    std::vector<std::string> protected_entries{configured_checkpoint};

    const state_replica_config config = load_state_replica_config();
    if (config.enabled)
    {
        const std::string replica_checkpoint =
            replica_path_for(config, configured_checkpoint);
        if (!ensure_parent_directory(replica_checkpoint, error))
        {
            return false;
        }
        add_checkpoint_lifecycle_paths(replica_checkpoint,
                                       "configured replica checkpoint",
                                       true,
                                       &protected_paths);
        protected_entries.push_back(replica_checkpoint);
    }
    if (!validate_distinct_state_paths(protected_paths, error))
    {
        return false;
    }

    for (const named_state_path &custom : custom_paths)
    {
        for (const named_state_path &protected_path : protected_paths)
        {
            const bool distinct_live_entry_alias =
                custom.path == checkpoint_path &&
                std::find(protected_entries.begin(),
                          protected_entries.end(),
                          protected_path.path) != protected_entries.end() &&
                !state_path_entries_refer_to_same_file(
                    custom.path, protected_path.path);
            if (distinct_live_entry_alias)
            {
                continue;
            }
            const state_path_relation relation =
                compare_state_paths(custom.path, protected_path.path);
            if (relation == state_path_relation::uninspectable)
            {
                if (error != nullptr)
                {
                    *error =
                        "failed to compare custom and recovery checkpoint "
                        "storage identities: " +
                        custom.path + " and " + protected_path.path;
                }
                return false;
            }
            if (relation == state_path_relation::same)
            {
                if (error != nullptr)
                {
                    *error = "custom checkpoint lifecycle overlaps recovery "
                             "checkpoint storage: " +
                             custom.path + " and " + protected_path.path;
                }
                return false;
            }
        }
    }
    return true;
}

bool copy_checkpoint_to_replica(
    const std::string &checkpoint_path,
    std::string *error,
    std::atomic<bool> *quarantine_latch)
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

    const std::string replica_journal =
        replica_path_for(config, configured_state_journal_path());
    if (state_journal_is_quarantined(replica_journal))
    {
        if (quarantine_latch != nullptr)
        {
            quarantine_latch->store(true, std::memory_order_release);
        }
        if (error != nullptr)
        {
            *error =
                "replicated state journal is quarantined and requires operator "
                "repair: " +
                replica_journal;
        }
        return false;
    }

    const std::string replica_checkpoint = replica_path_for(config, checkpoint_path);
    return copy_local_file(checkpoint_path, replica_checkpoint, error);
}

bool remove_replica_journal(
    const std::string &journal_path,
    std::string *error,
    std::atomic<bool> *quarantine_latch)
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
    if (state_journal_is_quarantined(replica_journal))
    {
        if (quarantine_latch != nullptr)
        {
            quarantine_latch->store(true, std::memory_order_release);
        }
        if (error != nullptr)
        {
            *error =
                "replicated state journal is quarantined and requires operator "
                "repair: " +
                replica_journal;
        }
        return false;
    }
    if (!durable_remove_state_path(
            replica_journal, "replicated state journal", error))
    {
        if (error != nullptr && error->empty())
        {
            *error = "failed to compact replicated state journal: " + replica_journal;
        }
        return false;
    }
    return true;
}

bool mirror_journal_record_to_replica(
    const std::string &journal_path,
    const state_record &record,
    std::string *error,
    std::atomic<bool> *quarantine_latch)
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
    return append_state_journal_file(replica_journal,
                                     record,
                                     "replicated state journal",
                                     error,
                                     nullptr,
                                     quarantine_latch);
}

bool mirror_journal_delete_prefix_to_replica(const std::string &journal_path,
                                             const state_delete_prefix_request &request,
                                             uint64_t operation_sequence,
                                             std::string *error,
                                             std::atomic<bool> *quarantine_latch)
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
    return append_state_delete_prefix_file(replica_journal,
                                           request,
                                           operation_sequence,
                                           "replicated state journal",
                                           error,
                                           nullptr,
                                           quarantine_latch);
}

bool mirror_journal_sequence_barrier_to_replica(
    const std::string &journal_path,
    const state_sequence_barrier_request &request,
    std::string *error,
    std::atomic<bool> *quarantine_latch)
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
    return append_state_sequence_barrier_file(
        replica_journal,
        request,
        "replicated state journal",
        error,
        nullptr,
        quarantine_latch);
}

bool import_state_recovery_files_from_replica(const std::string &checkpoint_path,
                                              const std::string &journal_path,
                                              std::string *error,
                                              std::atomic<bool> *quarantine_latch)
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

    const std::string replica_journal = replica_path_for(config, journal_path);
    if (state_journal_is_quarantined(replica_journal))
    {
        if (quarantine_latch != nullptr)
        {
            quarantine_latch->store(true, std::memory_order_release);
        }
        if (error != nullptr)
        {
            *error =
                "replicated state journal is quarantined and requires operator "
                "repair: " +
                replica_journal;
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
        config.local_import_dir = checkpoint_path + ".nfs-import";
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

    const bool checkpoint_missing =
        !::dsn::utils::filesystem::file_exists(checkpoint_path);
    const bool journal_missing =
        !::dsn::utils::filesystem::file_exists(journal_path);
    if (!checkpoint_missing || !journal_missing)
    {
        // A missing journal is also the normal compacted state. Without a shared
        // generation identifier, never combine one local artifact with a
        // potentially stale remote counterpart.
        return true;
    }

    const bool import_checkpoint = !config.remote_checkpoint_file.empty();
    const bool import_journal = !config.remote_journal_file.empty();

    std::vector<std::string> files;
    if (import_checkpoint)
    {
        files.push_back(config.remote_checkpoint_file);
    }
    if (import_journal)
    {
        files.push_back(config.remote_journal_file);
    }
    if (files.empty())
    {
        if (!import_checkpoint && !import_journal)
        {
            if (error != nullptr)
            {
                *error =
                    "rasn.state.nfs is enabled but no remote checkpoint or "
                    "journal file is configured";
            }
            return false;
        }
        return true;
    }

    static std::atomic<uint64_t> nfs_import_attempt{0};
    std::ostringstream attempt_name;
#if defined(_WIN32)
    const uint64_t process_id = static_cast<uint64_t>(::GetCurrentProcessId());
#else
    const uint64_t process_id = static_cast<uint64_t>(::getpid());
#endif
    attempt_name << "attempt-" << std::hex << process_id << "-"
                 << ::dsn_now_ns() << "-"
                 << nfs_import_attempt.fetch_add(1, std::memory_order_relaxed);
    const std::string attempt_directory =
        ::dsn::utils::filesystem::path_combine(
            config.local_import_dir, attempt_name.str());

    std::string directory_error;
    if (!ensure_parent_directory(
            ::dsn::utils::filesystem::path_combine(
                attempt_directory, "placeholder"),
            &directory_error))
    {
        if (error != nullptr)
        {
            *error = directory_error;
        }
        return false;
    }
    nfs_attempt_directory_cleanup attempt_cleanup(attempt_directory);

    std::vector<named_state_path> storage_paths;
    if (!collect_state_storage_paths(
            checkpoint_path, true, &storage_paths, error))
    {
        return false;
    }
    if (import_checkpoint)
    {
        const std::string imported_checkpoint =
            ::dsn::utils::filesystem::path_combine(
                attempt_directory, config.remote_checkpoint_file);
        const state_path_relation relation =
            compare_state_paths(imported_checkpoint, checkpoint_path);
        if (relation == state_path_relation::uninspectable)
        {
            if (error != nullptr)
            {
                *error = "failed to compare NFS checkpoint staging and primary "
                         "checkpoint identities: " +
                         imported_checkpoint + " and " + checkpoint_path;
            }
            return false;
        }
        if (relation == state_path_relation::same)
        {
            if (error != nullptr)
            {
                *error =
                    "NFS checkpoint staging must be distinct from primary "
                    "checkpoint storage: " +
                    imported_checkpoint;
            }
            return false;
        }
        storage_paths.push_back(
            {imported_checkpoint, "NFS checkpoint import"});
    }
    if (import_journal)
    {
        const std::string imported_journal =
            ::dsn::utils::filesystem::path_combine(
                attempt_directory, config.remote_journal_file);
        const state_path_relation relation =
            compare_state_paths(imported_journal, journal_path);
        if (relation == state_path_relation::uninspectable)
        {
            if (error != nullptr)
            {
                *error =
                    "failed to compare NFS journal staging and primary journal "
                    "identities: " +
                    imported_journal + " and " + journal_path;
            }
            return false;
        }
        if (relation == state_path_relation::same)
        {
            if (error != nullptr)
            {
                *error =
                    "NFS journal staging must be distinct from primary journal "
                    "storage: " +
                    imported_journal;
            }
            return false;
        }
        storage_paths.push_back(
            {imported_journal, "NFS journal import"});
    }
    if (!validate_distinct_state_paths(storage_paths, error))
    {
        return false;
    }

    ::dsn::rpc_address remote;
    remote.assign_ipv4(config.remote_host.c_str(), config.remote_port);

    auto copy_result = std::make_shared<nfs_copy_result>();
    ::dsn::task_ptr task = ::dsn::file::copy_remote_files(
        remote,
        config.remote_checkpoint_dir,
        files,
        attempt_directory,
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
        attempt_cleanup.preserve();
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

    const std::string import_marker =
        state_nfs_import_marker(checkpoint_path);
    if (!write_durable_state_text_file(import_marker,
                                       "rasn-state-nfs-import-v1\n",
                                       "pending NFS state import marker",
                                       error))
    {
        return false;
    }

    if (import_journal)
    {
        const std::string imported_journal =
            ::dsn::utils::filesystem::path_combine(
                attempt_directory, config.remote_journal_file);
        if (!copy_local_file(imported_journal, journal_path, error))
        {
            return false;
        }
    }
    if (import_checkpoint)
    {
        const std::string imported_checkpoint =
            ::dsn::utils::filesystem::path_combine(
                attempt_directory, config.remote_checkpoint_file);
        if (!copy_local_file(imported_checkpoint, checkpoint_path, error))
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
    if (decoded.sequence == 0)
    {
        if (error != nullptr)
        {
            *error = "checkpoint record sequence must be non-zero: " + source;
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
    FILE *raw_input = open_state_file_stream(path);
    if (raw_input == nullptr)
    {
        if (error != nullptr)
        {
            *error = "failed to open checkpoint for recovery: " + path;
        }
        return false;
    }
    std::unique_ptr<FILE, int (*)(FILE *)> input(raw_input, &std::fclose);

    std::string header;
    const state_file_line_result header_result =
        read_state_file_line(input.get(), &header);
    if (header_result == state_file_line_result::end ||
        header_result == state_file_line_result::error)
    {
        if (error != nullptr)
        {
            *error = header_result == state_file_line_result::error
                         ? "failed to read checkpoint: " + path
                         : "empty checkpoint: " + path;
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
    while (true)
    {
        const state_file_line_result line_result =
            read_state_file_line(input.get(), &line);
        if (line_result == state_file_line_result::error)
        {
            if (error != nullptr)
            {
                *error = "failed to read checkpoint: " + path;
            }
            return false;
        }
        if (line_result == state_file_line_result::end)
        {
            break;
        }
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
    int fd = -1;
    if (!prepare_exclusive_state_staging_file(
            temp_path, "state checkpoint", &fd, error))
    {
        return false;
    }

    std::ostringstream header;
    header << "rasn-state-v1\t" << last_sequence << "\n";
    const std::string encoded_header = header.str();
    bool written =
        write_all_state_file(fd, encoded_header.data(), encoded_header.size());
    for (const std::map<std::string, state_record>::value_type &entry : records)
    {
        if (!written)
        {
            break;
        }
        std::ostringstream line;
        write_state_record_line(line, entry.second);
        const std::string encoded = line.str();
        written = write_all_state_file(fd, encoded.data(), encoded.size());
    }
    const bool synced = written && sync_state_file_descriptor(fd);
    close_state_journal(fd);
    if (!synced)
    {
        ::dsn::utils::filesystem::remove_path(temp_path);
        if (error != nullptr && error->empty())
        {
            *error = written ? "failed to flush checkpoint: " + temp_path
                             : "failed to write checkpoint: " + temp_path;
        }
        return false;
    }

    // An older interrupted replacement may have left a stale backup beside the
    // live checkpoint. Refresh it from the flushed new image before replacing the
    // live name, so even a failed cleanup cannot later resurrect an old baseline
    // after journal compaction.
    const std::string backup_path = path + ".bak";
    if (::dsn::utils::filesystem::file_exists(backup_path) &&
        !copy_local_file(temp_path, backup_path, error))
    {
        ::dsn::utils::filesystem::remove_path(temp_path);
        if (error != nullptr && error->empty())
        {
            *error =
                "failed to synchronize legacy state checkpoint backup: " +
                backup_path;
        }
        return false;
    }

    // Replacing the live name directly is atomic on both supported platforms.
    // Moving the old checkpoint aside first leaves a crash window with no recovery
    // baseline, so do not use a rename-to-backup sequence here.
    if (!durable_rename_state_path(
            temp_path, path, "state checkpoint", error))
    {
        ::dsn::utils::filesystem::remove_path(temp_path);
        if (error != nullptr && error->empty())
        {
            *error = "failed to move checkpoint into place: " + path;
        }
        return false;
    }

    // Refuse to report a compactable checkpoint while a recovery alias remains.
    // The backup now contains the same snapshot, but treating cleanup failure as
    // an error also keeps the journal until the operator resolves the filesystem
    // problem.
    // The refreshed backup is safe if cleanup encounters a transient error.
    if (!durable_remove_state_path(
            backup_path, "legacy state checkpoint backup", error))
    {
        return false;
    }
    return true;
}

bool ensure_parent_directory(const std::string &path, std::string *error)
{
    const std::string directory = ::dsn::utils::filesystem::remove_file_name(path);
    if (directory.empty())
    {
        return true;
    }
    if (::dsn::utils::filesystem::directory_exists(directory))
    {
        if (::dsn::utils::filesystem::path_exists(path))
        {
            return true;
        }

        // A previous attempt may have created the directory tree but failed while
        // flushing one of its links. Before creating the first file in that tree,
        // re-flush every ancestor so a retry cannot acknowledge through an
        // unverified directory entry.
        std::string candidate = directory;
        while (true)
        {
            if (!sync_state_directory(
                    candidate, "state directory creation", error))
            {
                return false;
            }
            const std::string parent = path_parent_or_current(candidate);
            if (parent == candidate)
            {
                break;
            }
            candidate = parent;
        }
        return true;
    }

    std::vector<std::string> directories_to_sync;
    std::string candidate = directory;
    while (!candidate.empty() &&
           !::dsn::utils::filesystem::directory_exists(candidate))
    {
        directories_to_sync.push_back(candidate);
        const std::string parent = path_parent_or_current(candidate);
        if (parent == candidate)
        {
            break;
        }
        candidate = parent;
    }
    if (::dsn::utils::filesystem::create_directory(directory))
    {
        directories_to_sync.push_back(candidate);
        for (const std::string &created : directories_to_sync)
        {
            if (!sync_state_directory(
                    created, "state directory creation", error))
            {
                return false;
            }
        }
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

uint64_t monotonic_state_time_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

uint64_t configured_state_quarantine_probe_interval_ms()
{
    return config_uint64(
        "rasn.state",
        "quarantine_probe_interval_ms",
        k_default_quarantine_probe_interval_ms,
        "Maximum healthy-cache interval for external state quarantine probes");
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

bool same_state_record(const state_record &left, const state_record &right)
{
    return left.schema_version == right.schema_version && left.key == right.key &&
           left.kind == right.kind && left.scope == right.scope &&
           left.value == right.value && left.sequence == right.sequence;
}

bool validate_replicated_state_write_classification(std::string *error)
{
    const ::dsn::task_code *write_codes[] = {
        &RPC_RASN_STATE_PUT,
        &RPC_RASN_STATE_PUT_CONDITIONAL,
        &RPC_RASN_STATE_DELETE_PREFIX,
        &RPC_RASN_STATE_DELETE_PREFIX_DETAILED,
        &RPC_RASN_STATE_ADVANCE_SEQUENCE,
        &RPC_RASN_STATE_RECOVER};
    for (const ::dsn::task_code *code : write_codes)
    {
        const ::dsn::task_spec *spec =
            ::dsn::task_spec::get(static_cast<dsn_task_code_t>(*code));
        if (spec == nullptr || !spec->rpc_request_is_write_operation)
        {
            if (error != nullptr)
            {
                *error = std::string(code->to_string()) +
                         " must set rpc_request_is_write_operation=true for "
                         "replicated rasn.state";
            }
            return false;
        }
    }
    return true;
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
    const std::string nfs_import_marker =
        state_nfs_import_marker(checkpoint);
    if (::dsn::utils::filesystem::file_exists(checkpoint) ||
        ::dsn::utils::filesystem::file_exists(checkpoint + ".bak") ||
        ::dsn::utils::filesystem::file_exists(journal) ||
        ::dsn::utils::filesystem::file_exists(nfs_import_marker) ||
        ::dsn::utils::filesystem::file_exists(nfs_import_marker + ".tmp") ||
        state_journal_is_quarantined(journal) ||
        state_replica_journal_is_quarantined(journal))
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

state_store::state_store(bool journal_enabled,
                         uint64_t quarantine_probe_interval_ms)
    : _journal_enabled(journal_enabled),
      _quarantine_probe_interval_ms(quarantine_probe_interval_ms)
{
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

    ::dsn::service::zauto_lock mutation_guard(_mutation_lock);
    if (journal_is_quarantined(true))
    {
        return error_response(quarantine_error());
    }
    state_record stored = request.record;
    uint64_t last_sequence = 0;
    {
        ::dsn::service::zauto_lock guard(_lock);
        const std::map<std::string, state_record>::const_iterator existing = _records.find(stored.key);
        if (stored.sequence != 0 && existing != _records.end() &&
            same_state_record(existing->second, stored))
        {
            state_response response;
            response.record = existing->second;
            response.last_sequence = _last_sequence;
            return response;
        }
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

    }

    std::string journal_error;
    if (_journal_enabled && !append_journal_record(stored, &journal_error))
    {
        return error_response(journal_error);
    }

    {
        ::dsn::service::zauto_lock guard(_lock);
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
    if (journal_is_quarantined(false))
    {
        return error_response(quarantine_error());
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
    if (journal_is_quarantined(false))
    {
        return error_response(quarantine_error());
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

state_response state_store::delete_prefix(const state_delete_prefix_request &request)
{
    return delete_prefix_impl(request, true).response;
}

state_delete_prefix_result
state_store::delete_prefix_detailed(const state_delete_prefix_request &request)
{
    return delete_prefix_impl(request, false);
}

state_delete_prefix_result
state_store::delete_prefix_impl(const state_delete_prefix_request &request,
                                bool include_deleted_records)
{
    state_delete_prefix_result result;
    if (!valid_schema(request.schema_version))
    {
        result.response =
            error_response("state delete-prefix request has unsupported schema version");
        return result;
    }
    if (request.key_prefix.empty())
    {
        result.response =
            error_response("state delete-prefix request missing key prefix");
        return result;
    }
    if (!valid_state_key(request.key_prefix))
    {
        result.response = error_response(
            "state delete-prefix key must be namespaced as <scope>/<id>");
        return result;
    }
    if (request.max_sequence == 0)
    {
        result.response = error_response(
            "state delete-prefix requires a non-zero maximum sequence");
        return result;
    }

    ::dsn::service::zauto_lock mutation_guard(_mutation_lock);
    if (journal_is_quarantined(true))
    {
        result.response = error_response(quarantine_error());
        return result;
    }
    uint64_t operation_sequence = 0;
    {
        ::dsn::service::zauto_lock guard(_lock);
        if (request.max_sequence > _last_sequence)
        {
            result.response =
                error_response("state delete-prefix maximum sequence " +
                               std::to_string(request.max_sequence) +
                               " exceeds current sequence " +
                               std::to_string(_last_sequence));
            return result;
        }

        for (const std::map<std::string, state_record>::value_type &entry : _records)
        {
            if (entry.second.sequence <= request.max_sequence &&
                state_key_matches_prefix(entry.first, request.key_prefix))
            {
                ++result.deleted_records;
                if (include_deleted_records)
                {
                    result.response.records.push_back(entry.second);
                }
            }
        }
        if (result.deleted_records == 0)
        {
            result.response.last_sequence = _last_sequence;
            return result;
        }
        if (_last_sequence == (std::numeric_limits<uint64_t>::max)())
        {
            result.response = error_response(
                "state sequence space exhausted while deleting prefix " +
                request.key_prefix);
            return result;
        }
        operation_sequence = _last_sequence + 1;
    }

    std::string journal_error;
    if (_journal_enabled &&
        !append_journal_delete_prefix(request, operation_sequence, &journal_error))
    {
        result.response = error_response(journal_error);
        return result;
    }

    {
        ::dsn::service::zauto_lock guard(_lock);
        for (std::map<std::string, state_record>::iterator it = _records.begin();
             it != _records.end();)
        {
            if (it->second.sequence <= request.max_sequence &&
                state_key_matches_prefix(it->first, request.key_prefix))
            {
                it = _records.erase(it);
            }
            else
            {
                ++it;
            }
        }
        _last_sequence = operation_sequence;
        ++_write_epoch;
        result.response.last_sequence = _last_sequence;
    }

    dinfo("deleted rASN state prefix=%s records=%llu through_sequence=%llu operation_sequence=%llu",
          request.key_prefix.c_str(),
          static_cast<unsigned long long>(result.deleted_records),
          static_cast<unsigned long long>(request.max_sequence),
          static_cast<unsigned long long>(result.response.last_sequence));
    return result;
}

state_response
state_store::advance_sequence(const state_sequence_barrier_request &request)
{
    if (!valid_schema(request.schema_version))
    {
        return error_response(
            "state sequence-barrier request has unsupported schema version");
    }
    if (request.minimum_sequence == 0)
    {
        return error_response(
            "state sequence-barrier request requires a non-zero sequence");
    }

    ::dsn::service::zauto_lock mutation_guard(_mutation_lock);
    if (journal_is_quarantined(true))
    {
        return error_response(quarantine_error());
    }
    state_response response;
    {
        ::dsn::service::zauto_lock guard(_lock);
        if (request.minimum_sequence <= _last_sequence)
        {
            response.last_sequence = _last_sequence;
            return response;
        }
    }

    std::string journal_error;
    if (_journal_enabled &&
        !append_journal_sequence_barrier(request, &journal_error))
    {
        return error_response(journal_error);
    }

    {
        ::dsn::service::zauto_lock guard(_lock);
        _last_sequence = request.minimum_sequence;
        ++_write_epoch;
        response.last_sequence = _last_sequence;
    }
    return response;
}

state_response state_store::checkpoint(const state_checkpoint_request &request)
{
    return checkpoint_detailed(request).response;
}

state_checkpoint_result
state_store::checkpoint_detailed(const state_checkpoint_request &request)
{
    state_checkpoint_result result;
    if (!valid_schema(request.schema_version))
    {
        result.response =
            error_response("state checkpoint request has unsupported schema version");
        return result;
    }

    ::dsn::service::zauto_lock checkpoint_guard(_checkpoint_lock);
    const std::string path = request.path.empty() ? default_checkpoint_path() : request.path;
    const bool recovery_checkpoint =
        request.path.empty() ||
        state_path_entries_refer_to_same_file(
            path, default_checkpoint_path());
    result.checkpoint_path = path;
    std::string checkpoint_error;
    const bool checkpoint_path_valid =
        _journal_enabled
            ? (recovery_checkpoint
                   ? validate_storage_paths(&checkpoint_error)
                   : validate_custom_checkpoint_target(
                         path, &checkpoint_error))
            : validate_checkpoint_lifecycle_target(path, &checkpoint_error);
    if (!checkpoint_path_valid)
    {
        result.response = error_response(checkpoint_error);
        return result;
    }
    std::map<std::string, state_record> snapshot;
    uint64_t last_sequence = 0;
    uint64_t snapshot_epoch = 0;
    {
        ::dsn::service::zauto_lock mutation_guard(_mutation_lock);
        if (journal_is_quarantined(true))
        {
            result.response = error_response(quarantine_error());
            return result;
        }
        {
            ::dsn::service::zauto_lock guard(_lock);
            snapshot = _records;
            last_sequence = _last_sequence;
            snapshot_epoch = _write_epoch;
        }
    }

    if (!write_state_checkpoint_file(path, snapshot, last_sequence, &checkpoint_error))
    {
        result.response = error_response(checkpoint_error);
        return result;
    }

    if (_journal_enabled && recovery_checkpoint)
    {
        const std::string journal_path = default_journal_path();

        // Mirror the checkpoint file to the replica first. This is the slow I/O and
        // must stay outside _lock; copying a checkpoint that is current-or-superseded
        // is always safe because the journal (kept or compacted below) determines
        // which post-snapshot writes recovery must replay.
        std::string replica_error;
        if (!copy_checkpoint_to_replica(
                path, &replica_error, &_quarantine_seen))
        {
            result.response = error_response(replica_error);
            return result;
        }

        // Mutations may proceed while the snapshot and replica copy are written.
        // Re-enter the mutation lifecycle before checking the epoch and compacting;
        // this prevents a write from landing between the check and durable journal
        // removal without holding the read lock across filesystem I/O.
        {
            ::dsn::service::zauto_lock mutation_guard(_mutation_lock);
            if (journal_is_quarantined(true))
            {
                result.response = error_response(quarantine_error());
                return result;
            }
            bool snapshot_current = false;
            {
                ::dsn::service::zauto_lock guard(_lock);
                snapshot_current = _write_epoch == snapshot_epoch;
            }
            if (snapshot_current)
            {
                if (!remove_replica_journal(
                        journal_path, &replica_error, &_quarantine_seen))
                {
                    result.response = error_response(replica_error);
                    return result;
                }
                if (!durable_remove_state_path(
                        journal_path, "state journal", &checkpoint_error))
                {
                    result.response = error_response(checkpoint_error);
                    return result;
                }
                result.journal_compacted = true;
            }
        }
    }

    state_response &response = result.response;
    response.last_sequence = last_sequence;
    response.records.reserve(snapshot.size());
    for (const std::map<std::string, state_record>::value_type &entry : snapshot)
    {
        response.records.push_back(entry.second);
    }
    dinfo("checkpointed rASN state records=%llu path=%s",
          static_cast<unsigned long long>(response.records.size()),
          path.c_str());
    return result;
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

    ::dsn::service::zauto_lock checkpoint_guard(_checkpoint_lock);
    ::dsn::service::zauto_lock mutation_guard(_mutation_lock);
    if (journal_is_quarantined(true))
    {
        return error_response(quarantine_error());
    }
    std::map<std::string, state_record> imported;
    uint64_t last_sequence = 0;
    std::string import_error;
    if (!load_state_checkpoint_file(request.path, &imported, &last_sequence, &import_error))
    {
        return error_response(import_error);
    }
    state_path_relation durable_relation = state_path_relation::same;
    if (!durable_path.empty())
    {
        durable_relation = compare_state_paths(durable_path, request.path);
        if (durable_relation == state_path_relation::uninspectable)
        {
            return error_response(
                "failed to compare imported and durable checkpoint "
                "identities: " +
                request.path + " and " + durable_path);
        }
    }
    if (!durable_path.empty() &&
        durable_relation == state_path_relation::distinct)
    {
        const bool durable_path_valid =
            _journal_enabled
                ? validate_custom_checkpoint_target(
                      durable_path, &import_error)
                : validate_checkpoint_lifecycle_target(
                      durable_path, &import_error);
        if (!durable_path_valid)
        {
            return error_response(import_error);
        }
        if (!write_state_checkpoint_file(
                durable_path, imported, last_sequence, &import_error))
        {
            return error_response(import_error);
        }
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

    ::dsn::service::zauto_lock checkpoint_guard(_checkpoint_lock);
    ::dsn::service::zauto_lock mutation_guard(_mutation_lock);
    const std::string path = request.path.empty() ? default_checkpoint_path() : request.path;
    const std::string journal_path =
        _journal_enabled ? default_journal_path() : "";
    const bool configured_recovery =
        request.path.empty() ||
        state_path_entries_refer_to_same_file(
            path, default_checkpoint_path());
    if (journal_is_quarantined(true))
    {
        return error_response(quarantine_error());
    }
    std::string storage_error;
    const bool storage_paths_valid =
        _journal_enabled
            ? (configured_recovery
                   ? validate_storage_paths(&storage_error)
                   : validate_state_checkpoint_target(
                         path, true, &storage_error))
            : validate_checkpoint_lifecycle_target(path, &storage_error);
    if (!storage_paths_valid)
    {
        return error_response(storage_error);
    }
    if (_journal_enabled &&
        !cleanup_pending_nfs_state_import(
            path, journal_path, &storage_error))
    {
        return error_response(storage_error);
    }
    std::map<std::string, state_record> recovered;
    std::vector<state_journal_delete_prefix> recovered_deletions;
    uint64_t last_sequence = 0;

    if (_journal_enabled &&
        (!::dsn::utils::filesystem::file_exists(path) ||
         !::dsn::utils::filesystem::file_exists(journal_path)))
    {
        std::string replica_error;
        if (!import_state_recovery_files_from_replica(
                path,
                journal_path,
                &replica_error,
                &_quarantine_seen))
        {
            return error_response(replica_error);
        }
    }

    const std::string legacy_backup_path = path + ".bak";
    if (!::dsn::utils::filesystem::file_exists(path) &&
        ::dsn::utils::filesystem::file_exists(legacy_backup_path))
    {
        std::string restore_error;
        if (!durable_rename_state_path(legacy_backup_path,
                                       path,
                                       "legacy state checkpoint backup",
                                       &restore_error))
        {
            return error_response(restore_error);
        }
        dwarn("restored legacy state checkpoint backup path=%s", path.c_str());
    }

    if (_journal_enabled &&
        (!::dsn::utils::filesystem::file_exists(path) ||
         !::dsn::utils::filesystem::file_exists(journal_path)))
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
        if (!durable_remove_state_path(legacy_backup_path,
                                       "superseded legacy state checkpoint backup",
                                       &checkpoint_error))
        {
            return error_response(checkpoint_error);
        }
    }
    else if (!_journal_enabled ||
             !::dsn::utils::filesystem::file_exists(journal_path))
    {
        return error_response("failed to open checkpoint for recovery: " + path);
    }

    if (_journal_enabled &&
        ::dsn::utils::filesystem::file_exists(journal_path))
    {
        FILE *raw_journal = open_state_file_stream(journal_path);
        if (raw_journal == nullptr)
        {
            return error_response("failed to open state journal for recovery: " + journal_path);
        }
        std::unique_ptr<FILE, int (*)(FILE *)> journal(
            raw_journal, &std::fclose);

        const std::string expected_header = "rasn-state-journal-v1";
        std::string header;
        bool replay_records = true;
        const state_file_line_result header_result =
            read_state_file_line(journal.get(), &header);
        if (header_result == state_file_line_result::error)
        {
            return error_response("failed to read state journal header: " + journal_path);
        }
        if (header_result == state_file_line_result::end)
        {
            dwarn("ignoring empty state journal left by an interrupted first append: %s",
                  journal_path.c_str());
            replay_records = false;
        }
        else if (header != expected_header)
        {
            const bool torn_header =
                header_result == state_file_line_result::unterminated &&
                header.size() < expected_header.size() &&
                expected_header.compare(0, header.size(), header) == 0;
            if (!torn_header)
            {
                return error_response("invalid state journal header: " + journal_path);
            }
            dwarn("ignoring torn state journal header left by an interrupted first append: %s",
                  journal_path.c_str());
            replay_records = false;
        }
        else if (header_result == state_file_line_result::unterminated)
        {
            dwarn("ignoring unterminated state journal header: %s", journal_path.c_str());
            replay_records = false;
        }

        std::string line;
        while (replay_records)
        {
            const state_file_line_result line_result =
                read_state_file_line(journal.get(), &line);
            if (line_result == state_file_line_result::error)
            {
                return error_response("failed to read state journal: " +
                                      journal_path);
            }
            if (line_result == state_file_line_result::end)
            {
                break;
            }
            if (line.empty())
            {
                continue;
            }

            if (line_result == state_file_line_result::unterminated)
            {
                // Every complete journal entry is newline-terminated. A non-empty
                // line read with eofbit already set
                // is therefore an unterminated append tail, even if the bytes form
                // a decodable prefix. Drop only that tail; newline-terminated
                // corrupt records are still decoded below and fail recovery.
                dwarn("dropping unterminated trailing record in state journal %s",
                      journal_path.c_str());
                break;
            }

            const std::vector<std::string> fields = split_tab_fields(line);
            if (!fields.empty() && fields[0] == "quarantine")
            {
                _quarantine_seen.store(true, std::memory_order_release);
                return error_response(
                    "state journal contains a quarantine marker after an "
                    "unprovable mirror rollback: " +
                    journal_path);
            }
            if (!fields.empty() && fields[0] == "sequence-barrier")
            {
                uint64_t minimum_sequence = 0;
                if (fields.size() != 2 ||
                    !parse_uint64(fields[1], &minimum_sequence) ||
                    minimum_sequence == 0)
                {
                    return error_response(
                        "invalid state journal sequence-barrier record: " +
                        journal_path);
                }
                last_sequence = (std::max)(last_sequence, minimum_sequence);
                continue;
            }
            if (!fields.empty() && fields[0] == "delete-prefix")
            {
                state_journal_delete_prefix deletion;
                if (fields.size() != 4 ||
                    !parse_uint64(fields[1], &deletion.operation_sequence) ||
                    !parse_uint64(fields[2], &deletion.max_sequence) ||
                    !hex_decode(fields[3], &deletion.key_prefix) ||
                    deletion.operation_sequence == 0 || deletion.max_sequence == 0 ||
                    deletion.max_sequence >= deletion.operation_sequence ||
                    !valid_state_key(deletion.key_prefix))
                {
                    return error_response("invalid state journal delete-prefix record: " +
                                          journal_path);
                }

                for (std::map<std::string, state_record>::iterator it = recovered.begin();
                     it != recovered.end();)
                {
                    if (it->second.sequence <= deletion.max_sequence &&
                        state_key_matches_prefix(it->first, deletion.key_prefix))
                    {
                        it = recovered.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
                recovered_deletions.push_back(deletion);
                last_sequence =
                    (std::max)(last_sequence, deletion.operation_sequence);
                continue;
            }

            state_record record;
            std::string decode_error;
            if (!decode_state_record_fields(fields, journal_path, &record, &decode_error))
            {
                // A newline-terminated line that fails to decode is a complete
                // corrupt record, so stay strict even when it is the final record.
                return error_response(decode_error);
            }
            recovered[record.key] = record;
            last_sequence = (std::max)(last_sequence, record.sequence);
        }
    }

    const std::string nfs_import_marker =
        state_nfs_import_marker(path);
    if (_journal_enabled &&
        !durable_remove_state_path(nfs_import_marker,
                                   "completed NFS state import marker",
                                   &storage_error))
    {
        return error_response(storage_error);
    }

    {
        ::dsn::service::zauto_lock guard(_lock);
        bool changed = false;
        // Merge by max sequence instead of swapping wholesale. A mutation that
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
                changed = true;
            }
        }
        for (const state_journal_delete_prefix &deletion : recovered_deletions)
        {
            for (std::map<std::string, state_record>::iterator it = _records.begin();
                 it != _records.end();)
            {
                // A key present in the final replay image was recreated after this
                // tombstone and must survive even if an explicit imported sequence
                // is below the cutoff.
                if (recovered.find(it->first) == recovered.end() &&
                    it->second.sequence <= deletion.max_sequence &&
                    state_key_matches_prefix(it->first, deletion.key_prefix))
                {
                    it = _records.erase(it);
                    changed = true;
                }
                else
                {
                    ++it;
                }
            }
        }
        changed = changed || last_sequence > _last_sequence;
        _last_sequence = (std::max)(_last_sequence, last_sequence);
        if (changed)
        {
            ++_write_epoch;
        }
    }

    // Report the merged store's records and last_sequence together. query()
    // reads both under _lock, so a concurrent mutation preserved by the merge above
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

    if (_journal_enabled)
    {
        return configured_state_recovery_available(request);
    }
    const std::string path =
        request.path.empty() ? default_checkpoint_path() : request.path;
    return ::dsn::utils::filesystem::file_exists(path) ||
           ::dsn::utils::filesystem::file_exists(path + ".bak");
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

std::string state_store::quarantine_error() const
{
    return "state journal or configured replica is quarantined and requires "
           "offline operator repair; primary journal: " +
           default_journal_path();
}

bool state_store::journal_is_quarantined(bool force_refresh) const
{
    if (!_journal_enabled)
    {
        return false;
    }
    if (_quarantine_seen.load(std::memory_order_acquire))
    {
        return true;
    }
    uint64_t now_ms = monotonic_state_time_ms();
    if (!force_refresh && _quarantine_probe_interval_ms != 0 &&
        now_ms < _next_quarantine_probe_ms.load(std::memory_order_acquire))
    {
        return false;
    }

    ::dsn::service::zauto_lock guard(_quarantine_probe_lock);
    if (_quarantine_seen.load(std::memory_order_acquire))
    {
        return true;
    }
    now_ms = monotonic_state_time_ms();
    if (!force_refresh && _quarantine_probe_interval_ms != 0 &&
        now_ms < _next_quarantine_probe_ms.load(std::memory_order_relaxed))
    {
        return false;
    }

    const bool quarantined =
        state_journal_is_quarantined(default_journal_path()) ||
        state_replica_journal_is_quarantined(default_journal_path());
    if (quarantined)
    {
        _quarantine_seen.store(true, std::memory_order_release);
    }
    else
    {
        const uint64_t next_probe =
            _quarantine_probe_interval_ms >
                    (std::numeric_limits<uint64_t>::max)() - now_ms
                ? (std::numeric_limits<uint64_t>::max)()
                : now_ms + _quarantine_probe_interval_ms;
        _next_quarantine_probe_ms.store(next_probe, std::memory_order_release);
    }
    return quarantined;
}

bool state_store::validate_storage_paths(std::string *error) const
{
    if (!_journal_enabled)
    {
        return true;
    }
    std::vector<named_state_path> paths;
    if (!collect_state_storage_paths(
            default_checkpoint_path(), true, &paths, error) ||
        !validate_distinct_state_paths(paths, error))
    {
        return false;
    }

    ::dsn::service::zauto_lock guard(_storage_filesystem_validation_lock);
    if (!_storage_filesystem_validation_checked)
    {
        _storage_filesystem_validation_ok =
            validate_state_storage_filesystems(
                paths, &_storage_filesystem_validation_error);
        _storage_filesystem_validation_checked = true;
    }
    if (!_storage_filesystem_validation_ok && error != nullptr)
    {
        *error = _storage_filesystem_validation_error;
    }
    return _storage_filesystem_validation_ok;
}

bool state_store::validate_cached_storage_filesystem(std::string *error) const
{
    if (!_journal_enabled)
    {
        return true;
    }
    {
        ::dsn::service::zauto_lock guard(
            _storage_filesystem_validation_lock);
        if (_storage_filesystem_validation_checked)
        {
            if (!_storage_filesystem_validation_ok && error != nullptr)
            {
                *error = _storage_filesystem_validation_error;
            }
            return _storage_filesystem_validation_ok;
        }
    }
    return validate_storage_paths(error);
}

bool state_store::append_journal_record(const state_record &record, std::string *error) const
{
    if (!validate_cached_storage_filesystem(error))
    {
        return false;
    }
    const std::string journal_path = default_journal_path();
    if (!ensure_parent_directory(journal_path, error))
    {
        return false;
    }

    state_journal_append_token token;
    if (!append_state_journal_file(
            journal_path,
            record,
            "state journal",
            error,
            &token,
            &_quarantine_seen))
    {
        return false;
    }
    std::string mirror_error;
    if (!mirror_journal_record_to_replica(
            journal_path, record, &mirror_error, &_quarantine_seen))
    {
        return rollback_primary_journal_after_mirror_failure(
            journal_path, token, mirror_error, error);
    }
    return true;
}

bool state_store::append_journal_delete_prefix(const state_delete_prefix_request &request,
                                               uint64_t operation_sequence,
                                               std::string *error) const
{
    if (!validate_cached_storage_filesystem(error))
    {
        return false;
    }
    const std::string journal_path = default_journal_path();
    if (!ensure_parent_directory(journal_path, error))
    {
        return false;
    }
    state_journal_append_token token;
    if (!append_state_delete_prefix_file(journal_path,
                                         request,
                                         operation_sequence,
                                         "state journal",
                                         error,
                                         &token,
                                         &_quarantine_seen))
    {
        return false;
    }
    std::string mirror_error;
    if (!mirror_journal_delete_prefix_to_replica(
            journal_path,
            request,
            operation_sequence,
            &mirror_error,
            &_quarantine_seen))
    {
        return rollback_primary_journal_after_mirror_failure(
            journal_path, token, mirror_error, error);
    }
    return true;
}

bool state_store::append_journal_sequence_barrier(
    const state_sequence_barrier_request &request,
    std::string *error) const
{
    if (!validate_cached_storage_filesystem(error))
    {
        return false;
    }
    const std::string journal_path = default_journal_path();
    if (!ensure_parent_directory(journal_path, error))
    {
        return false;
    }
    state_journal_append_token token;
    if (!append_state_sequence_barrier_file(
            journal_path,
            request,
            "state journal",
            error,
            &token,
            &_quarantine_seen))
    {
        return false;
    }
    std::string mirror_error;
    if (!mirror_journal_sequence_barrier_to_replica(
            journal_path,
            request,
            &mirror_error,
            &_quarantine_seen))
    {
        return rollback_primary_journal_after_mirror_failure(
            journal_path, token, mirror_error, error);
    }
    return true;
}

state_store &global_state_store()
{
    static state_store store(
        true, configured_state_quarantine_probe_interval_ms());
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
    this->register_async_rpc_handler(RPC_RASN_STATE_DELETE_PREFIX,
                                     "delete_prefix",
                                     &rasn_state_rpc_service::on_delete_prefix,
                                     gpid);
    this->register_async_rpc_handler(
        RPC_RASN_STATE_DELETE_PREFIX_DETAILED,
        "delete_prefix_detailed",
        &rasn_state_rpc_service::on_delete_prefix_detailed,
        gpid);
    this->register_async_rpc_handler(RPC_RASN_STATE_ADVANCE_SEQUENCE,
                                     "advance_sequence",
                                     &rasn_state_rpc_service::on_advance_sequence,
                                     gpid);
    this->register_async_rpc_handler(
        RPC_RASN_STATE_CHECKPOINT,
        "checkpoint",
        &rasn_state_rpc_service::on_checkpoint,
        gpid);
    this->register_async_rpc_handler(
        RPC_RASN_STATE_CHECKPOINT_DETAILED,
        "checkpoint_detailed",
        &rasn_state_rpc_service::on_checkpoint_detailed,
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
    this->unregister_rpc_handler(RPC_RASN_STATE_DELETE_PREFIX, gpid);
    this->unregister_rpc_handler(RPC_RASN_STATE_DELETE_PREFIX_DETAILED, gpid);
    this->unregister_rpc_handler(RPC_RASN_STATE_ADVANCE_SEQUENCE, gpid);
    this->unregister_rpc_handler(RPC_RASN_STATE_CHECKPOINT, gpid);
    this->unregister_rpc_handler(RPC_RASN_STATE_CHECKPOINT_DETAILED, gpid);
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

void rasn_state_rpc_service::on_delete_prefix(
    const state_delete_prefix_request &request,
    ::dsn::rpc_replier<state_response> &reply)
{
    reply(_store->delete_prefix(request));
}

void rasn_state_rpc_service::on_delete_prefix_detailed(
    const state_delete_prefix_request &request,
    ::dsn::rpc_replier<state_delete_prefix_result> &reply)
{
    reply(_store->delete_prefix_detailed(request));
}

void rasn_state_rpc_service::on_advance_sequence(
    const state_sequence_barrier_request &request,
    ::dsn::rpc_replier<state_response> &reply)
{
    reply(_store->advance_sequence(request));
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

void rasn_state_rpc_service::on_checkpoint_detailed(
    const state_checkpoint_request &request,
    ::dsn::rpc_replier<state_checkpoint_result> &reply)
{
    if (_replicated)
    {
        state_checkpoint_result result;
        result.response.ok = false;
        result.response.error =
            "state checkpoints are managed by rDSN replication in replicated mode";
        reply(result);
        return;
    }
    reply(_store->checkpoint_detailed(request));
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
rasn_state_client::delete_prefix_sync(const state_delete_prefix_request &request,
                                      std::chrono::milliseconds timeout,
                                      int thread_hash,
                                      uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<state_response>(
        ::dsn::rpc::call(_server,
                         RPC_RASN_STATE_DELETE_PREFIX,
                         request,
                         nullptr,
                         empty_callback,
                         timeout,
                         thread_hash,
                         partition_hash));
}

std::pair< ::dsn::error_code, state_delete_prefix_result>
rasn_state_client::delete_prefix_detailed_sync(
    const state_delete_prefix_request &request,
    std::chrono::milliseconds timeout,
    int thread_hash,
    uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<state_delete_prefix_result>(
        ::dsn::rpc::call(_server,
                         RPC_RASN_STATE_DELETE_PREFIX_DETAILED,
                         request,
                         nullptr,
                         empty_callback,
                         timeout,
                         thread_hash,
                         partition_hash));
}

std::pair< ::dsn::error_code, state_response>
rasn_state_client::advance_sequence_sync(
    const state_sequence_barrier_request &request,
    std::chrono::milliseconds timeout,
    int thread_hash,
    uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<state_response>(
        ::dsn::rpc::call(_server,
                         RPC_RASN_STATE_ADVANCE_SEQUENCE,
                         request,
                         nullptr,
                         empty_callback,
                         timeout,
                         thread_hash,
                         partition_hash));
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

std::pair< ::dsn::error_code, state_checkpoint_result>
rasn_state_client::checkpoint_detailed_sync(
    const state_checkpoint_request &request,
    std::chrono::milliseconds timeout,
    int thread_hash,
    uint64_t partition_hash)
{
    return ::dsn::rpc::wait_and_unwrap<state_checkpoint_result>(
        ::dsn::rpc::call(_server,
                         RPC_RASN_STATE_CHECKPOINT_DETAILED,
                         request,
                         nullptr,
                         empty_callback,
                         timeout,
                         thread_hash,
                         partition_hash));
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
    std::string storage_error;
    if (!global_state_store().validate_storage_paths(&storage_error))
    {
        derror("refusing to start rasn.state with unsafe storage paths: %s",
               storage_error.c_str());
        return ::dsn::ERR_INVALID_PARAMETERS;
    }

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
      _store(false, 0),
      _rpc(&_store, true),
      _last_durable_decree(0)
{
}

::dsn::error_code rasn_replicated_state_app::start(int argc, char **argv)
{
    std::string classification_error;
    if (!validate_replicated_state_write_classification(
            &classification_error))
    {
        derror("refusing to start replicated rasn.state: %s",
               classification_error.c_str());
        return ::dsn::ERR_INVALID_PARAMETERS;
    }

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
