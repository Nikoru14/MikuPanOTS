#include "typedefs.h"
#include "mikupan_file.h"
#include "mikupan/mikupan_ps5.h"
#include "mikupan/mikupan_config.h"
#include "mikupan/debug/mikupan_logging.h"
#include "spdlog/spdlog.h"
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_platform.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <cctype>
#include <limits>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <system_error>
#include <vector>

#include "mikupan/mikupan_memory.h"
#ifdef __PROSPERO__
#include "mikupan/debug/mikupan_logging_c.h"
#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <unordered_map>
#include <strings.h>
#include <sys/stat.h>
#endif

static std::mutex missing_data_dialog_mutex;
static std::condition_variable missing_data_dialog_cv;
static bool missing_data_dialog_requested = false;
static bool missing_data_dialog_open = false;
static bool missing_data_dialog_selected = false;
static std::string missing_data_path;

extern "C" const char *MikuPan_GetBaseDirectory(void)
{
    static std::string cached;
    if (!cached.empty())
    {
        return cached.c_str();
    }

#ifdef __PROSPERO__
    /* PS5 title: the title folder (eboot.bin and resources/) is mounted
     * read-only at /app0. */
    cached = "/app0/";
    return cached.c_str();
#endif

    const char *base = SDL_GetBasePath();
    cached = (base != nullptr && base[0] != '\0') ? base : "./";

    /* On macOS SDL_GetBasePath() returns the main bundle's resource path.
     * That is right for a .app bundle, but for a bare executable (what the
     * CMake build produces) it comes back as "<exe dir>/Resources/", a
     * folder that does not exist. On the default case-insensitive APFS
     * volume that name also aliases our own "resources/" folder, so every
     * asset lookup turns into "<exe dir>/resources/resources/..." and fails.
     * Detect that by probing for a directory that must exist under a valid
     * base and, if it is missing, fall back to the parent of "Resources/". */
    if (SDL_strcmp(SDL_GetPlatform(), "macOS") == 0)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path probe_dir = fs::path("resources") / "shaders";
        if (!fs::is_directory(fs::path(cached) / probe_dir, ec))
        {
            /* "<dir>/Resources/" -> parent_path() twice strips the trailing
             * separator component and then "Resources". */
            const fs::path parent =
                fs::path(cached).parent_path().parent_path();
            if (!parent.empty() && fs::is_directory(parent / probe_dir, ec))
            {
                cached = parent.string();
                if (cached.back() != '/')
                {
                    cached += '/';
                }
            }
        }
    }

    return cached.c_str();
}

static std::filesystem::path MikuPan_GetApplicationBasePath()
{
    return std::filesystem::path(MikuPan_GetBaseDirectory());
}

#ifdef __PROSPERO__
/* PS5 homebrew keeps its files under /data, so that is the first choice for
 * both the game's IMG_HD.BIN / IMG_BD.BIN and everything MikuPan writes. A
 * title's sandbox may not reach /data, so it is only used once a real write
 * there succeeds; otherwise MikuPan falls back to the title's own storage. */
static constexpr const char *kPs5DataFolder = "/data/MikuPan/";
static constexpr const char *kPs5TitleStorageFolder = "/download0/MikuPan/";

/* A file's size by opening it, or -1. Quiet: the music streamer asks for
 * loose files that only exist inside the archive many times a second. */
static long long MikuPan_Ps5FileSize(const std::string &path)
{
    FILE *file = std::fopen(path.c_str(), "rb");
    if (file != nullptr)
    {
        long long size = -1;
        if (std::fseek(file, 0, SEEK_END) == 0)
        {
            size = std::ftell(file);
        }
        std::fclose(file);
        return size;
    }
    return -1;
}

static void MikuPan_Ps5ListFolder(const char *folder)
{
    DIR *dir = opendir(folder);
    if (dir == nullptr)
    {
        info_log("PS5: cannot list %s (errno %d %s)", folder, errno, std::strerror(errno));
        return;
    }
    std::string names;
    int count = 0;
    while (struct dirent *entry = readdir(dir))
    {
        if (count++ < 40)
        {
            names += entry->d_name;
            names += "  ";
        }
    }
    closedir(dir);
    info_log("PS5: %s holds %d entries: %s", folder, count, names.c_str());
}

extern "C" bool MikuPan_Ps5PathExists(const char *path)
{
    if (MikuPan_Ps5FileSize(path) >= 0)
    {
        return true;
    }
    if (DIR *dir = opendir(path))
    {
        closedir(dir);
        return true;
    }
    return false;
}

/* The path with each part spelt as the folder spells it, ignoring case: the
 * console's /data is case-sensitive, and disc files are often copied as
 * img_hd.bin. A part that matches nothing keeps its own spelling. */
static std::string MikuPan_Ps5ResolveCaseUncached(const std::string &path);

/* Each path is matched once: data files are looked up again and again (the
 * music streamer on every read), and the folders do not change spelling
 * while the game runs. */
static std::string MikuPan_Ps5ResolveCase(const std::string &path)
{
    static std::mutex mutex;
    static std::unordered_map<std::string, std::string> resolved;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = resolved.find(path);
        if (found != resolved.end())
        {
            return found->second;
        }
    }
    std::string result = MikuPan_Ps5ResolveCaseUncached(path);
    std::lock_guard<std::mutex> lock(mutex);
    if (resolved.size() > 4096)
    {
        resolved.clear();
    }
    resolved.emplace(path, result);
    return result;
}

static std::string MikuPan_Ps5ResolveCaseUncached(const std::string &path)
{
    if (MikuPan_Ps5PathExists(path.c_str()))
    {
        return path;
    }
    std::string resolved = path.empty() || path[0] != '/' ? "" : "/";
    size_t start = resolved.size();
    while (start < path.size())
    {
        size_t end = path.find('/', start);
        if (end == std::string::npos)
        {
            end = path.size();
        }
        std::string part = path.substr(start, end - start);
        if (!part.empty())
        {
            std::string candidate = resolved + part;
            if (!MikuPan_Ps5PathExists(candidate.c_str()))
            {
                if (DIR *dir = opendir(resolved.empty() ? "." : resolved.c_str()))
                {
                    while (struct dirent *entry = readdir(dir))
                    {
                        if (strcasecmp(entry->d_name, part.c_str()) == 0)
                        {
                            candidate = resolved + entry->d_name;
                            break;
                        }
                    }
                    closedir(dir);
                }
            }
            resolved = candidate;
            if (end < path.size())
            {
                resolved += '/';
            }
        }
        start = end + 1;
    }
    return resolved;
}

#endif // __PROSPERO__

/* std::filesystem's existence and size checks, except on the console, where
 * the files are opened instead, as loading them does: a title has no working
 * directory (a relative path gets EINVAL), and whether its stat answers
 * std::filesystem as a desktop does is not yet known (see MikuPan_GetDataRoot). */
static bool MikuPan_FsExists(const std::filesystem::path &path, std::error_code &error)
{
#ifdef __PROSPERO__
    error.clear();
    return MikuPan_Ps5PathExists(path.string().c_str());
#else
    return std::filesystem::exists(path, error);
#endif
}

static bool MikuPan_FsExists(const std::filesystem::path &path)
{
    std::error_code error;
    return MikuPan_FsExists(path, error) && !error;
}

static bool MikuPan_FsIsRegularFile(const std::filesystem::path &path, std::error_code &error)
{
#ifdef __PROSPERO__
    error.clear();
    return MikuPan_Ps5FileSize(path.string()) >= 0;
#else
    return std::filesystem::is_regular_file(path, error);
#endif
}

static uintmax_t MikuPan_FsFileSize(const std::filesystem::path &path, std::error_code &error)
{
#ifdef __PROSPERO__
    const long long size = MikuPan_Ps5FileSize(path.string());
    if (size < 0)
    {
        error = std::make_error_code(std::errc::no_such_file_or_directory);
        return static_cast<uintmax_t>(-1);
    }
    error.clear();
    return static_cast<uintmax_t>(size);
#else
    return std::filesystem::file_size(path, error);
#endif
}

static uintmax_t MikuPan_FsFileSize(const std::filesystem::path &path)
{
    std::error_code error;
    const uintmax_t size = MikuPan_FsFileSize(path, error);
    return error ? 0 : size;
}

#ifdef __PROSPERO__
static bool MikuPan_Ps5FolderIsWritable(const char *folder)
{
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (!std::filesystem::is_directory(folder, ec))
    {
        return false;
    }

    const std::string probe = std::string(folder) + ".mikupan_write_test";
    FILE *file = std::fopen(probe.c_str(), "wb");
    if (file == nullptr)
    {
        return false;
    }

    const bool written = std::fputc('1', file) != EOF;
    const bool closed = std::fclose(file) == 0;
    std::remove(probe.c_str());
    return written && closed;
}

extern "C" const char *MikuPan_GetPs5UserDirectory(void)
{
    static std::string cached;
    if (!cached.empty())
    {
        return cached.c_str();
    }

    if (MikuPan_Ps5FolderIsWritable(kPs5DataFolder))
    {
        cached = kPs5DataFolder;
    }
    else
    {
        std::error_code ec;
        std::filesystem::create_directories(kPs5TitleStorageFolder, ec);
        cached = kPs5TitleStorageFolder;
    }

    /* No logging here: the logger itself asks for this folder while it is
     * being created. */
    return cached.c_str();
}
#endif

static std::filesystem::path MikuPan_GetDataRoot()
{
    const char *folder = mikupan_configuration.data_folder;
    if (folder != nullptr && folder[0] != '\0')
    {
        return std::filesystem::path(folder);
    }

#ifdef __PROSPERO__
    /* Prefer /data/MikuPan when the game files are there; otherwise they are
     * expected inside the title folder at /app0. */
    {
        std::error_code ec;
        const std::filesystem::path data_folder(kPs5DataFolder);
        const std::string img_hd =
            MikuPan_Ps5ResolveCase(std::string(kPs5DataFolder) + "IMG_HD.BIN");
        if (MikuPan_FsExists(img_hd, ec))
        {
            return data_folder;
        }
        static bool listed = false;
        if (!listed)
        {
            listed = true;
            MikuPan_Ps5ListFolder(kPs5DataFolder);
            MikuPan_Ps5ListFolder("/app0/");
        }
    }
#endif

    return MikuPan_GetApplicationBasePath();
}

#ifdef __ANDROID__
static bool MikuPan_IsAndroidContentUriString(const std::string& path)
{
    return path.rfind("content://", 0) == 0;
}

static bool MikuPan_IsAndroidTreeUriString(const std::string& path)
{
    return MikuPan_IsAndroidContentUriString(path)
        && path.find("/tree/") != std::string::npos;
}

static bool MikuPan_IsUriUnreserved(unsigned char ch)
{
    return (ch >= 'A' && ch <= 'Z')
        || (ch >= 'a' && ch <= 'z')
        || (ch >= '0' && ch <= '9')
        || ch == '-' || ch == '.' || ch == '_' || ch == '~';
}

static std::string MikuPan_EncodeAndroidDocumentPath(
    const std::string& relative_path)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(relative_path.size() * 3);

    for (unsigned char ch : relative_path)
    {
        if (ch == '/' || ch == '\\')
        {
            encoded += "%2F";
        }
        else if (MikuPan_IsUriUnreserved(ch))
        {
            encoded += static_cast<char>(ch);
        }
        else
        {
            encoded += '%';
            encoded += hex[ch >> 4];
            encoded += hex[ch & 0x0f];
        }
    }

    return encoded;
}

static std::string MikuPan_NormalizeRelativePathString(
    const std::filesystem::path& path)
{
    std::filesystem::path relative_path;
    for (const auto& part : path.relative_path())
    {
        if (part.empty() || part == "." || part == "..")
        {
            continue;
        }

        relative_path /= part;
    }

    return relative_path.generic_string();
}

static std::string MikuPan_GetAndroidTreeChildUri(
    const std::string& tree_uri,
    const std::filesystem::path& relative_path)
{
    const std::string marker = "/tree/";
    const size_t tree_pos = tree_uri.find(marker);
    if (tree_pos == std::string::npos)
    {
        return tree_uri;
    }

    const size_t tree_doc_id_start = tree_pos + marker.size();
    const size_t tree_doc_id_end = tree_uri.find('/', tree_doc_id_start);
    const std::string tree_doc_id =
        tree_uri.substr(tree_doc_id_start,
                        tree_doc_id_end == std::string::npos
                            ? std::string::npos
                            : tree_doc_id_end - tree_doc_id_start);
    const std::string child =
        MikuPan_NormalizeRelativePathString(relative_path);
    if (tree_doc_id.empty() || child.empty())
    {
        return tree_uri;
    }

    return tree_uri.substr(0, tree_doc_id_start)
        + tree_doc_id
        + "/document/"
        + tree_doc_id
        + "%2F"
        + MikuPan_EncodeAndroidDocumentPath(child);
}
#endif

static std::string MikuPan_GetDataPathString(
    const std::filesystem::path& relative_path)
{
    const char *folder = mikupan_configuration.data_folder;
#ifdef __ANDROID__
    if (folder != nullptr && MikuPan_IsAndroidTreeUriString(folder))
    {
        return MikuPan_GetAndroidTreeChildUri(folder, relative_path);
    }
#endif

#ifdef __PROSPERO__
    return MikuPan_Ps5ResolveCase((MikuPan_GetDataRoot() / relative_path).generic_string());
#else
    return (MikuPan_GetDataRoot() / relative_path).generic_string();
#endif
}

static std::string MikuPan_NormalizeSdlAssetPathString(const std::string& path)
{
    if (path.rfind("assets://", 0) == 0)
    {
        return path;
    }

    if (path.rfind("assets:/", 0) == 0)
    {
        return "assets://" + path.substr(8);
    }

    return path;
}

static bool MikuPan_IsSdlAssetPathString(const std::string& path)
{
    return path.rfind("assets://", 0) == 0
#ifdef __ANDROID__
        || MikuPan_IsAndroidContentUriString(path)
#endif
        ;
}

static bool MikuPan_IsSdlAssetPath(const char *filename,
                                   std::string *normalized = nullptr)
{
    if (filename == nullptr)
    {
        return false;
    }

    std::string path = MikuPan_NormalizeSdlAssetPathString(filename);
    const bool result = MikuPan_IsSdlAssetPathString(path);
    if (result && normalized != nullptr)
    {
        *normalized = path;
    }
    return result;
}

static u_int MikuPan_GetSdlFileSize(const char *filename)
{
    std::string path;
    if (!MikuPan_IsSdlAssetPath(filename, &path))
    {
        return 0;
    }

    SDL_IOStream *stream = SDL_IOFromFile(path.c_str(), "rb");
    if (stream == nullptr)
    {
        return 0;
    }

    const Sint64 size = SDL_GetIOSize(stream);
    SDL_CloseIO(stream);

    if (size <= 0
        || static_cast<uint64_t>(size) > std::numeric_limits<u_int>::max())
    {
        return 0;
    }

    return static_cast<u_int>(size);
}

static bool MikuPan_ReadSdlFileRange(const char *filename,
                                     Sint64 offset,
                                     void *buffer,
                                     size_t size)
{
    if (buffer == nullptr)
    {
        return false;
    }

    std::string path;
    if (!MikuPan_IsSdlAssetPath(filename, &path))
    {
        return false;
    }

    SDL_IOStream *stream = SDL_IOFromFile(path.c_str(), "rb");
    if (stream == nullptr)
    {
        return false;
    }

    if (offset != 0 && SDL_SeekIO(stream, offset, SDL_IO_SEEK_SET) < 0)
    {
        SDL_CloseIO(stream);
        return false;
    }

    size_t total = 0;
    char *dst = static_cast<char *>(buffer);
    while (total < size)
    {
        const size_t read =
            SDL_ReadIO(stream, dst + total, size - total);
        if (read == 0)
        {
            break;
        }
        total += read;
    }

    SDL_CloseIO(stream);
    return total == size;
}

static std::string MikuPan_ToLowerPathString(const std::filesystem::path& path)
{
    std::string value = path.lexically_normal().generic_string();
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return value;
}

static bool MikuPan_IsExcludedRuntimeAsset(const std::filesystem::path& path)
{
    std::string value = MikuPan_ToLowerPathString(path);
    while (value.rfind("./", 0) == 0)
    {
        value.erase(0, 2);
    }

    return value.rfind("resources/", 0) == 0
        || value.find("/resources/") != std::string::npos
        || value == "mikupan.ini"
        || (value.size() >= 12
            && value.compare(value.size() - 12, 12, "/mikupan.ini") == 0);
}

static bool MikuPan_IsPathInsideDataRoot(const std::filesystem::path& path)
{
    std::string root =
        MikuPan_ToLowerPathString(std::filesystem::absolute(MikuPan_GetDataRoot()));
    std::string target =
        MikuPan_ToLowerPathString(std::filesystem::absolute(path));

    while (root.size() > 1 && root.back() == '/')
    {
        root.pop_back();
    }

    if (target == root)
    {
        return true;
    }

    root += '/';
    return target.rfind(root, 0) == 0;
}

static bool MikuPan_IsRequiredArchiveDataFile(
    const std::filesystem::path& path)
{
    const std::string filename =
        MikuPan_ToLowerPathString(path.filename());

    return filename == "img_hd.bin" || filename == "img_bd.bin";
}

static bool MikuPan_ShouldPromptForMissingDataFile(
    const std::filesystem::path& path)
{
    if (MikuPan_IsRequiredArchiveDataFile(path))
    {
        return true;
    }

    if (MikuPan_IsExcludedRuntimeAsset(path))
    {
        return false;
    }

    return MikuPan_IsPathInsideDataRoot(path);
}

static void MikuPan_ApplySelectedDataFolder(const std::string& folder)
{
    if (folder.empty())
    {
        return;
    }

    std::strncpy(mikupan_configuration.data_folder,
                 folder.c_str(),
                 sizeof(mikupan_configuration.data_folder) - 1);
    mikupan_configuration
        .data_folder[sizeof(mikupan_configuration.data_folder) - 1] = '\0';

    if (!MikuPan_SaveConfiguration(nullptr))
    {
        spdlog::error("Failed to save selected data folder: {}", folder);
    }
    else
    {
        spdlog::info("Selected data folder: {}", folder);
    }
}

static bool MikuPan_MakePathRelativeToRoot(
    const std::filesystem::path& path,
    const std::filesystem::path& root,
    std::filesystem::path& relative)
{
    std::error_code ec;
    std::filesystem::path abs_path = std::filesystem::absolute(path, ec);
    if (ec)
    {
        return false;
    }

    std::filesystem::path abs_root = std::filesystem::absolute(root, ec);
    if (ec)
    {
        return false;
    }

    relative = abs_path.lexically_normal().lexically_relative(
        abs_root.lexically_normal());
    if (relative.empty())
    {
        return false;
    }

    for (const auto& part : relative)
    {
        if (part == "..")
        {
            return false;
        }
    }

    return true;
}

static std::filesystem::path MikuPan_RebasePathAfterDataFolderSelection(
    const std::filesystem::path& path,
    const std::filesystem::path& old_root)
{
    if (MikuPan_IsRequiredArchiveDataFile(path))
    {
        return MikuPan_GetDataRoot() / path.filename();
    }

    std::filesystem::path relative;
    if (MikuPan_MakePathRelativeToRoot(path, old_root, relative))
    {
        return MikuPan_GetDataRoot() / relative;
    }

    return path;
}

static void SDLCALL MikuPan_ServiceMissingDataFolderDialogOnMainThread(
    void *userdata)
{
    (void)userdata;
    MikuPan_ServiceMissingDataFolderDialog();
}

static bool MikuPan_WaitForMissingDataFolderDialogOnMainThread()
{
    for (;;)
    {
        {
            std::lock_guard<std::mutex> lock(missing_data_dialog_mutex);
            if (!missing_data_dialog_open && !missing_data_dialog_requested)
            {
                return missing_data_dialog_selected;
            }
        }

        MikuPan_ServiceMissingDataFolderDialog();
        SDL_PumpEvents();
        SDL_Delay(10);
    }
}

static bool MikuPan_RequestMissingDataFolderDialog(
    const std::filesystem::path& path)
{
    if (!MikuPan_ShouldPromptForMissingDataFile(path))
    {
        return false;
    }

    if (SDL_WasInit(SDL_INIT_VIDEO) == 0)
    {
        return false;
    }

    const bool is_main_thread = SDL_IsMainThread();

    {
        std::unique_lock<std::mutex> lock(missing_data_dialog_mutex);
        if (missing_data_dialog_open || missing_data_dialog_requested)
        {
            if (!is_main_thread)
            {
                missing_data_dialog_cv.wait(lock, [] {
                    return !missing_data_dialog_open
                        && !missing_data_dialog_requested;
                });
                return missing_data_dialog_selected;
            }

            lock.unlock();
            return MikuPan_WaitForMissingDataFolderDialogOnMainThread();
        }

        missing_data_path = path.lexically_normal().generic_string();
        missing_data_dialog_requested = true;
        missing_data_dialog_selected = false;
    }

    if (is_main_thread)
    {
        MikuPan_ServiceMissingDataFolderDialog();
    }
    else if (!SDL_RunOnMainThread(
                 MikuPan_ServiceMissingDataFolderDialogOnMainThread,
                 nullptr,
                 false))
    {
        {
            std::lock_guard<std::mutex> lock(missing_data_dialog_mutex);
            missing_data_dialog_requested = false;
            missing_data_dialog_selected = false;
        }
        missing_data_dialog_cv.notify_all();
        return false;
    }

    if (is_main_thread)
    {
        return MikuPan_WaitForMissingDataFolderDialogOnMainThread();
    }
    else
    {
        std::unique_lock<std::mutex> lock(missing_data_dialog_mutex);
        missing_data_dialog_cv.wait(lock, [] {
            return !missing_data_dialog_open && !missing_data_dialog_requested;
        });
        return missing_data_dialog_selected;
    }
}

static bool MikuPan_RequestMissingDataFolderDialogAsync(
    const std::filesystem::path& path)
{
    if (!MikuPan_ShouldPromptForMissingDataFile(path))
    {
        return false;
    }

    if (SDL_WasInit(SDL_INIT_VIDEO) == 0)
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(missing_data_dialog_mutex);
        if (missing_data_dialog_open || missing_data_dialog_requested)
        {
            return true;
        }

        missing_data_path = path.lexically_normal().generic_string();
        missing_data_dialog_requested = true;
        missing_data_dialog_selected = false;
    }

    return true;
}

extern "C" void MikuPan_RequestDataFolderSelection(const char *missing_path)
{
    const char *path = (missing_path != nullptr && missing_path[0] != '\0')
                           ? missing_path
                           : "IMG_HD.BIN";
    MikuPan_RequestMissingDataFolderDialogAsync(std::filesystem::path(path));
}

static bool MikuPan_DataPathExistsNoPrompt(
    const std::filesystem::path& relative_path)
{
    const std::string data_path = MikuPan_GetDataPathString(relative_path);
    if (MikuPan_IsSdlAssetPathString(data_path))
    {
        return MikuPan_GetSdlFileSize(data_path.c_str()) > 0;
    }

    std::error_code error;
    if (!MikuPan_FsIsRegularFile(data_path, error) || error)
    {
        return false;
    }

    const uintmax_t file_size = MikuPan_FsFileSize(data_path, error);
    return !error && file_size > 0;
}

extern "C" bool MikuPan_HasRequiredDataFiles(char *missing_file,
                                             size_t missing_file_size)
{
    const char *required_files[] = {
        "IMG_HD.BIN",
        "IMG_BD.BIN",
    };

    for (const char *required_file : required_files)
    {
        if (!MikuPan_DataPathExistsNoPrompt(required_file))
        {
            if (missing_file != nullptr && missing_file_size > 0)
            {
                std::strncpy(missing_file, required_file,
                             missing_file_size - 1);
                missing_file[missing_file_size - 1] = '\0';
            }
            return false;
        }
    }

    if (missing_file != nullptr && missing_file_size > 0)
    {
        missing_file[0] = '\0';
    }
    return true;
}

static void SDLCALL MikuPan_MissingDataFolderSelected(
    void *userdata, const char *const *filelist, int filter)
{
    (void)userdata;
    (void)filter;

    std::string selected_folder;
    if (filelist != nullptr && filelist[0] != nullptr
        && filelist[0][0] != '\0')
    {
        selected_folder = filelist[0];
        MikuPan_ApplySelectedDataFolder(selected_folder);
    }

    {
        std::lock_guard<std::mutex> lock(missing_data_dialog_mutex);
        missing_data_dialog_selected = !selected_folder.empty();
        missing_data_dialog_open = false;
    }
    missing_data_dialog_cv.notify_all();
}

extern "C" void MikuPan_ServiceMissingDataFolderDialog()
{
    std::string missing_path_to_show;

    {
        std::lock_guard<std::mutex> lock(missing_data_dialog_mutex);

        if (missing_data_dialog_requested && !missing_data_dialog_open)
        {
            missing_path_to_show = missing_data_path;
            missing_data_dialog_requested = false;
            missing_data_dialog_open = true;
        }
    }

    if (missing_path_to_show.empty())
    {
        return;
    }

    const std::string message =
        "The game data file was not found:\n\n" + missing_path_to_show
        + "\n\nSelect the folder that contains the Fatal Frame data files. "
          "The selected folder will be saved to the configuration file.";

#ifndef __ANDROID__
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                             "File not found",
                             message.c_str(),
                             nullptr);
#endif

    const char *start = mikupan_configuration.data_folder[0] != '\0'
                            ? mikupan_configuration.data_folder
                            : nullptr;
#ifdef __ANDROID__
    SDL_SetHint("SDL_ANDROID_ALLOW_PERSISTENT_FOLDER_ACCESS", "1");
#endif
    SDL_ShowOpenFolderDialog(MikuPan_MissingDataFolderSelected,
                             nullptr,
                             nullptr,
                             start,
                             false);
}

void MikuPan_LoadImgHdFile()
{
    std::string data_path_string = MikuPan_GetDataPathString("IMG_HD.BIN");
    if (MikuPan_IsSdlAssetPathString(data_path_string))
    {
        if (MikuPan_GetSdlFileSize(data_path_string.c_str()) == 0)
        {
            if (MikuPan_RequestMissingDataFolderDialog("IMG_HD.BIN"))
            {
                data_path_string = MikuPan_GetDataPathString("IMG_HD.BIN");
            }
        }

        if (MikuPan_GetSdlFileSize(data_path_string.c_str()) == 0)
        {
            return;
        }

        MikuPan_ReadFullFile(data_path_string.c_str(),
            static_cast<char *>(MikuPan_GetHostPointer(ImgHdAddress)));
        return;
    }

    auto data_path = std::filesystem::path(data_path_string);
    if (!MikuPan_FsExists(data_path))
    {
        const auto old_root = MikuPan_GetDataRoot();
        if (MikuPan_RequestMissingDataFolderDialog(data_path))
        {
            data_path_string = MikuPan_GetDataPathString("IMG_HD.BIN");
            if (MikuPan_IsSdlAssetPathString(data_path_string))
            {
                MikuPan_LoadImgHdFile();
                return;
            }
            data_path = MikuPan_RebasePathAfterDataFolderSelection(data_path,
                                                                   old_root);
        }

        if (!MikuPan_FsExists(data_path))
        {
            return;
        }
    }

    const auto path = data_path.generic_string();
    MikuPan_ReadFullFile(path.c_str(),
        static_cast<char *>(MikuPan_GetHostPointer(ImgHdAddress)));
}

void MikuPan_ReadFullFile(const char *filename, char *buffer)
{
    std::string asset_path;
    if (MikuPan_IsSdlAssetPath(filename, &asset_path))
    {
        const u_int file_size = MikuPan_GetSdlFileSize(asset_path.c_str());
        if (file_size == 0 || buffer == nullptr)
        {
            return;
        }

        MikuPan_ReadSdlFileRange(asset_path.c_str(), 0, buffer, file_size);
        return;
    }

    std::filesystem::path path_filename(filename);

    if (!MikuPan_FsExists(path_filename))
    {
        const auto old_root = MikuPan_GetDataRoot();
        if (MikuPan_RequestMissingDataFolderDialog(path_filename))
        {
            path_filename =
                MikuPan_RebasePathAfterDataFolderSelection(path_filename,
                                                           old_root);
        }

        if (!MikuPan_FsExists(path_filename))
        {
            return;
        }
    }

    if (buffer == nullptr)
    {
        return;
    }

    auto file_size = MikuPan_FsFileSize(path_filename);
    std::ifstream infile(path_filename, std::ios::binary);
    infile.read(buffer, file_size);
    infile.close();
}


void MikuPan_ReadFileInArchive(int sector, int size, u_int *address)
{
    std::string archive_string = MikuPan_GetDataPathString("IMG_BD.BIN");

    if (MikuPan_IsSdlAssetPathString(archive_string))
    {
        if (MikuPan_GetSdlFileSize(archive_string.c_str()) == 0)
        {
            if (MikuPan_RequestMissingDataFolderDialog("IMG_BD.BIN"))
            {
                archive_string = MikuPan_GetDataPathString("IMG_BD.BIN");
            }
        }

        if (MikuPan_GetSdlFileSize(archive_string.c_str()) == 0)
        {
            return;
        }

        if (address == nullptr)
        {
            spdlog::critical("File loading address is NULL, abort load request!");
            return;
        }

        if (!MikuPan_IsPs2MemoryPointer((int64_t) address))
        {
            spdlog::critical("File loading address is not in PS2 memory range!");
            return;
        }

        info_log("PS2 Address 0x%x",
                 MikuPan_GetPs2OffsetFromHostPointer(address));

        MikuPan_ReadSdlFileRange(archive_string.c_str(),
                                 static_cast<Sint64>(sector) * 0x800,
                                 address,
                                 static_cast<size_t>(size));
        return;
    }

    auto archive = std::filesystem::path(archive_string);
    if (!MikuPan_FsExists(archive))
    {
        spdlog::critical("IMG_BD.BIN not found!");
        const auto old_root = MikuPan_GetDataRoot();
        if (MikuPan_RequestMissingDataFolderDialog(archive))
        {
            archive_string = MikuPan_GetDataPathString("IMG_BD.BIN");
            if (MikuPan_IsSdlAssetPathString(archive_string))
            {
                MikuPan_ReadFileInArchive(sector, size, address);
                return;
            }
            archive = MikuPan_RebasePathAfterDataFolderSelection(archive,
                                                                 old_root);
        }

        if (!MikuPan_FsExists(archive))
        {
            return;
        }
    }

    if (address == nullptr)
    {
        spdlog::critical("File loading address is NULL, abort load request!");
        return;
    }

    if (!MikuPan_IsPs2MemoryPointer((int64_t) address))
    {
        spdlog::critical("File loading address is not in PS2 memory range!");
        return;
    }

    info_log("PS2 Address 0x%x", MikuPan_GetPs2OffsetFromHostPointer(address));

    std::ifstream infile(archive, std::ios::binary);
    infile.seekg(sector * 0x800, std::ios::beg);
    infile.read(reinterpret_cast<char *>(address), size);

    infile.close();
}

u_int MikuPan_GetDataFileSize(const char *relative_name)
{
    const std::string path_string = MikuPan_GetDataPathString(relative_name);

    if (MikuPan_IsSdlAssetPathString(path_string))
    {
        return MikuPan_GetSdlFileSize(path_string.c_str());
    }

    std::error_code ec;
    const auto size = MikuPan_FsFileSize(path_string, ec);
    if (ec)
    {
        return 0;
    }

    return static_cast<u_int>(size);
}

bool MikuPan_ReadDataFileRange(const char *relative_name, int64_t offset,
                               void *buffer, size_t size)
{
    if (buffer == nullptr)
    {
        return false;
    }

    const std::string path_string = MikuPan_GetDataPathString(relative_name);

    if (MikuPan_IsSdlAssetPathString(path_string))
    {
        return MikuPan_ReadSdlFileRange(path_string.c_str(), offset, buffer,
                                        size);
    }

    std::ifstream infile(path_string, std::ios::binary);
    if (!infile.is_open())
    {
        return false;
    }

    infile.seekg(offset, std::ios::beg);
    infile.read(static_cast<char *>(buffer), static_cast<std::streamsize>(size));
    return !infile.bad() && static_cast<size_t>(infile.gcount()) == size;
}

void *MikuPan_OpenDataFile(const char *relative_name)
{
    const std::string path_string = MikuPan_GetDataPathString(relative_name);
    return SDL_IOFromFile(path_string.c_str(), "rb");
}

bool MikuPan_ReadOpenDataFile(void *file, int64_t offset, void *buffer,
                              size_t size)
{
    SDL_IOStream *io = static_cast<SDL_IOStream *>(file);

    if (io == nullptr || buffer == nullptr
        || SDL_SeekIO(io, offset, SDL_IO_SEEK_SET) < 0)
    {
        return false;
    }

    auto *dst = static_cast<uint8_t *>(buffer);
    size_t remaining = size;

    while (remaining > 0)
    {
        const size_t got = SDL_ReadIO(io, dst, remaining);

        if (got == 0)
        {
            return false;
        }

        dst += got;
        remaining -= got;
    }

    return true;
}

void MikuPan_CloseDataFile(void *file)
{
    if (file != nullptr)
    {
        SDL_CloseIO(static_cast<SDL_IOStream *>(file));
    }
}

void MikuPan_BufferFile(int sector, int size, int64_t address)
{
    std::string archive_string = MikuPan_GetDataPathString("IMG_BD.BIN");

    if (MikuPan_IsSdlAssetPathString(archive_string))
    {
        if (MikuPan_GetSdlFileSize(archive_string.c_str()) == 0)
        {
            if (MikuPan_RequestMissingDataFolderDialog("IMG_BD.BIN"))
            {
                archive_string = MikuPan_GetDataPathString("IMG_BD.BIN");
            }
        }

        if (MikuPan_GetSdlFileSize(archive_string.c_str()) == 0)
        {
            return;
        }

        if (address == 0)
        {
            return;
        }

        if ((int64_t *) *(int64_t *) address != nullptr)
        {
            free((int64_t *) *(int64_t *) address);
        }

        void *file_ptr = malloc(size);
        *((int64_t *) address) = (int64_t) file_ptr;

        MikuPan_ReadSdlFileRange(archive_string.c_str(),
                                 static_cast<Sint64>(sector) * 0x800,
                                 file_ptr,
                                 static_cast<size_t>(size));
        return;
    }

    auto archive = std::filesystem::path(archive_string);
    if (!MikuPan_FsExists(archive))
    {
        const auto old_root = MikuPan_GetDataRoot();
        if (MikuPan_RequestMissingDataFolderDialog(archive))
        {
            archive_string = MikuPan_GetDataPathString("IMG_BD.BIN");
            if (MikuPan_IsSdlAssetPathString(archive_string))
            {
                MikuPan_BufferFile(sector, size, address);
                return;
            }
            archive = MikuPan_RebasePathAfterDataFolderSelection(archive,
                                                                 old_root);
        }

        if (!MikuPan_FsExists(archive))
        {
            return;
        }
    }

    if (address == 0)
    {
        return;
    }

    if ((int64_t *) *(int64_t *) address != nullptr)
    {
        free((int64_t *) *(int64_t *) address);
    }

    void *file_ptr = malloc(size);

    *((int64_t *) address) = (int64_t) file_ptr;

    std::ifstream infile(archive, std::ios::binary);
    infile.seekg(sector * 0x800, std::ios::beg);
    infile.read(static_cast<char *>(file_ptr), size);

    infile.close();
}

u_char MikuPan_ReadFile(const char *filename, void *buffer, int size)
{
    auto relative_path = std::filesystem::path(MikuPan_GetRelativePath(filename));

    if (!MikuPan_FsExists(relative_path))
    {
        return 0;
    }

    std::ifstream inFile;
    inFile.open(relative_path, std::ios::binary);
    inFile.read(static_cast<char *>(buffer), size);
    inFile.close();
    return inFile.bad() == 0;
}

u_char MikuPan_WriteFile(const char *filename, const void *buffer, int size)
{
    if (size < 0 || (size && !buffer)) return 0;
    const auto path = std::filesystem::path(MikuPan_GetRelativePath(filename));
    std::error_code error;
    if (!path.parent_path().empty())
    {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
        {
            spdlog::error("Failed to create user data directory {}: {}",
                          path.parent_path().generic_string(), error.message());
            return 0;
        }
    }
    std::ofstream file(path, std::ios::binary);
    if (!file) return 0;
    if (size) file.write(static_cast<const char *>(buffer), size);
    file.close();
    return !file.fail();
}

u_int MikuPan_GetFileSize(const char *filename)
{
    std::string asset_path;
    if (MikuPan_IsSdlAssetPath(filename, &asset_path))
    {
        return MikuPan_GetSdlFileSize(asset_path.c_str());
    }

    std::filesystem::path path(filename);
    if (!MikuPan_FsExists(path))
    {
        const auto old_root = MikuPan_GetDataRoot();
        if (MikuPan_RequestMissingDataFolderDialog(path))
        {
            path = MikuPan_RebasePathAfterDataFolderSelection(path, old_root);
        }

        if (!MikuPan_FsExists(path))
        {
            return 0;
        }
    }

    return MikuPan_FsFileSize(path);
}

bool MikuPan_ResolveCdPath(const char* path, char* buffer, size_t buffer_size)
{
    if (!path) {
        spdlog::error("MikuPan_ResolveCdPath: path is null");
        return false;
    }
    
    if (!buffer) {
        spdlog::error("MikuPan_ResolveCdPath: buffer is null");
        return false;
    }
    
    if (buffer_size == 0) {
        spdlog::error("MikuPan_ResolveCdPath: buffer_size is 0");
        return false;
    }
    
    std::string s(path);
    
    auto ltrim = [](std::string& x) {
        x.erase(x.begin(), std::find_if(x.begin(), x.end(), [](unsigned char ch) {
            return !std::isspace(ch);
        }));
    };
    
    auto rtrim = [](std::string& x) {
        x.erase(std::find_if(x.rbegin(), x.rend(), [](unsigned char ch) {
            return !std::isspace(ch);
        }).base(), x.end());
    };
    
    ltrim(s);
    rtrim(s);
    
    if (auto semi = s.find(';'); semi != std::string::npos) {
        s.erase(semi);
    }
    
    std::replace(s.begin(), s.end(), '\\', '/');
    
    if (s.rfind("./", 0) == 0) {
        s.erase(0, 2);
    } else if (auto colon = s.find(':'); colon != std::string::npos) {
        s.erase(0, colon + 1);
        if (!s.empty() && s.front() == '/') {
            s.erase(0, 1);
        }
    }

    while (!s.empty() && s.front() == '/') {
        s.erase(0, 1);
    }

    if (!s.empty()) {
#ifdef __ANDROID__
        const std::filesystem::path relative_data_path(s);
        const bool required_archive =
            MikuPan_IsRequiredArchiveDataFile(relative_data_path);
        const char *folder = mikupan_configuration.data_folder;
        if (folder != nullptr && MikuPan_IsAndroidTreeUriString(folder))
        {
            std::string resolved_path = MikuPan_GetDataPathString(s);
            if (MikuPan_GetSdlFileSize(resolved_path.c_str()) == 0)
            {
                if (!required_archive)
                {
                    return false;
                }

                if (MikuPan_RequestMissingDataFolderDialog(relative_data_path))
                {
                    resolved_path = MikuPan_GetDataPathString(s);
                }

                if (MikuPan_GetSdlFileSize(resolved_path.c_str()) == 0)
                {
                    return false;
                }
            }
            s = resolved_path;
        }
        else
#endif
        {
        const std::filesystem::path relative_data_path(s);
        std::filesystem::path resolved_path =
            MikuPan_GetDataRoot() / relative_data_path;
#ifdef __PROSPERO__
        /* the disc's folders may have been copied as movie/scn0010.pss */
        resolved_path = MikuPan_Ps5ResolveCase(resolved_path.generic_string());
#endif
        if (!MikuPan_FsExists(resolved_path)) {
            if (!MikuPan_IsRequiredArchiveDataFile(relative_data_path))
            {
                return false;
            }

            if (MikuPan_RequestMissingDataFolderDialog(resolved_path)) {
                resolved_path = MikuPan_GetDataRoot() / relative_data_path;
            }

            if (!MikuPan_FsExists(resolved_path))
            {
                return false;
            }
        }
        s = resolved_path.generic_string();
        }
    }

    if (s.length() + 1 > buffer_size) {
        spdlog::error("MikuPan_ResolveCdPath: Buffer too small. Need {} bytes, got {}", 
                      s.length() + 1, buffer_size);
        return false;
    }
    
    std::strcpy(buffer, s.c_str());
    
    return true;
}

bool MikuPan_ResolveBasePath(const char* path, char* buffer, size_t buffer_size)
{
    if (!path)
    {
        spdlog::error("MikuPan_ResolveBasePath: path is null");
        return false;
    }

    if (!buffer)
    {
        spdlog::error("MikuPan_ResolveBasePath: buffer is null");
        return false;
    }

    if (buffer_size == 0)
    {
        spdlog::error("MikuPan_ResolveBasePath: buffer_size is 0");
        return false;
    }

    std::string asset_path;
    if (MikuPan_IsSdlAssetPath(path, &asset_path))
    {
        if (asset_path.length() + 1 > buffer_size)
        {
            spdlog::error(
                "MikuPan_ResolveBasePath: Buffer too small. Need {} bytes, got {}",
                asset_path.length() + 1, buffer_size);
            return false;
        }

        std::strcpy(buffer, asset_path.c_str());
        return true;
    }

    std::filesystem::path resolved_path(path);
    if (resolved_path.is_relative())
    {
        const char *base = SDL_GetBasePath();
        if (base != nullptr && std::strncmp(base, "assets://", 9) == 0)
        {
            std::string relative =
                resolved_path.lexically_normal().generic_string();
            while (relative.rfind("./", 0) == 0)
            {
                relative.erase(0, 2);
            }
            while (!relative.empty() && relative.front() == '/')
            {
                relative.erase(0, 1);
            }

            const std::string resolved = std::string(base) + relative;
            if (resolved.length() + 1 > buffer_size)
            {
                spdlog::error(
                    "MikuPan_ResolveBasePath: Buffer too small. Need {} bytes, got {}",
                    resolved.length() + 1, buffer_size);
                return false;
            }

            std::strcpy(buffer, resolved.c_str());
            return true;
        }

        resolved_path = MikuPan_GetApplicationBasePath()
            / resolved_path.relative_path();
    }

    const std::string resolved =
        resolved_path.lexically_normal().generic_string();
    if (resolved.length() + 1 > buffer_size)
    {
        spdlog::error(
            "MikuPan_ResolveBasePath: Buffer too small. Need {} bytes, got {}",
            resolved.length() + 1, buffer_size);
        return false;
    }

    std::strcpy(buffer, resolved.c_str());
    return true;
}

u_char MikuPan_FolderExists(const char *folder)
{
    auto relative_path = std::filesystem::path(MikuPan_GetRelativePath(folder));

    if (!std::filesystem::is_directory(relative_path))
    {
        relative_path = relative_path.remove_filename();
    }

    return MikuPan_FsExists(relative_path);
}

u_char MikuPan_CreateFolder(const char *folder)
{
    const auto path = std::filesystem::path(MikuPan_GetRelativePath(folder));
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return !error && std::filesystem::is_directory(path, error);
}

int MikuPan_GetListFiles(const char *folder, MikuPan_McTblGetDir *table, int max_entries)
{
    std::string regex_folder(folder);
    size_t wildcard = regex_folder.find('*');

    if (wildcard != std::string::npos)
    {
        regex_folder = regex_folder.substr(0, wildcard);
    }

    std::filesystem::path regex_folder_path(regex_folder);

    auto relative_path = std::filesystem::path(MikuPan_GetRelativePath(regex_folder.c_str()));

    if (!std::filesystem::is_directory(relative_path))
    {
        relative_path = relative_path.remove_filename();
    }

    int i = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(relative_path))
    {
        if (entry.path().compare(regex_folder_path) == std::string::npos)
        {
            continue;
        }

        if (entry.is_directory())
        {
            continue;
        }

        table[i].FileSizeByte = entry.file_size();
        strncpy(reinterpret_cast<char *>(table[i].EntryName), entry.path().filename().generic_string().c_str(), sizeof(table[i].EntryName));

        if (++i > max_entries)
        {
            break;
        }
    }

    return i;
}

std::string MikuPan_GetRelativePath(const char *path)
{
    char resolved_path[1024];
    if (MikuPan_ResolveUserPath(path, resolved_path, sizeof(resolved_path)))
    {
        return resolved_path;
    }

    return std::filesystem::path(path).relative_path().generic_string();
}
