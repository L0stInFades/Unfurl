#include "unfurl/archive_engine.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <clocale>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <ranges>
#include <system_error>
#include <utility>

namespace unfurl {
namespace {

constexpr std::size_t io_buffer_size = std::size_t{128} * 1024;

// libarchive converts entry names through the CRT locale, even with wide file handles.
// Scope UTF-8 to the worker thread and restore the caller's locale on return.
struct ArchiveLocale {
#ifdef _WIN32
    std::string previous{std::setlocale(LC_CTYPE, nullptr)};
    int mode{_configthreadlocale(_ENABLE_PER_THREAD_LOCALE)};

    ArchiveLocale() {
        if (!std::setlocale(LC_CTYPE, ".UTF8")) {
            _configthreadlocale(mode);
            throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot initialize Unicode archive support.");
        }
    }
    ~ArchiveLocale() {
        std::setlocale(LC_CTYPE, previous.c_str());
        _configthreadlocale(mode);
    }
#endif
};

std::string native_string(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::filesystem::path utf8_path(std::string_view value) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

int open_archive_reader(archive* reader, const std::filesystem::path& path) {
#ifdef _WIN32
    return archive_read_open_filename_w(reader, path.c_str(), io_buffer_size);
#else
    return archive_read_open_filename(reader, path.c_str(), io_buffer_size);
#endif
}

[[noreturn]] void throw_archive(ArchiveFailure::Code code, archive* handle, std::string_view fallback) {
    const auto* detail = handle == nullptr ? nullptr : archive_error_string(handle);
    throw ArchiveFailure(code, detail == nullptr || *detail == '\0' ? std::string(fallback) : std::string(detail));
}

void check_status(int status, archive* handle, std::string_view operation) {
    if (status < ARCHIVE_OK) {
        throw_archive(ArchiveFailure::Code::io, handle, operation);
    }
}

void check_reader_status(int status, archive* handle, std::string_view operation) {
    if (status >= ARCHIVE_OK) {
        return;
    }
    const auto* detail = handle == nullptr ? nullptr : archive_error_string(handle);
    std::string message = detail == nullptr ? std::string(operation) : std::string(detail);
    std::ranges::transform(message, message.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (message.find("passphrase") != std::string::npos || message.find("password") != std::string::npos ||
        message.find("encrypted") != std::string::npos) {
        throw ArchiveFailure(ArchiveFailure::Code::password_required,
                             detail == nullptr ? std::string(operation) : std::string(detail));
    }
    throw_archive(ArchiveFailure::Code::io, handle, operation);
}

void check_cancelled(const std::function<bool()>& cancelled) {
    if (cancelled && cancelled()) {
        throw ArchiveFailure(ArchiveFailure::Code::cancelled, "The archive operation was cancelled.");
    }
}

ArchiveFormat format_from_name(const char* value) {
    if (value == nullptr) {
        return ArchiveFormat::unknown;
    }
    std::string name(value);
    std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name.find("gzip") != std::string::npos) {
        return ArchiveFormat::tar_gzip;
    }
    if (name.find("bzip") != std::string::npos) {
        return ArchiveFormat::tar_bzip2;
    }
    if (name.find("7-zip") != std::string::npos || name.find("7zip") != std::string::npos) {
        return ArchiveFormat::seven_zip;
    }
    if (name.find("zip") != std::string::npos) {
        return ArchiveFormat::zip;
    }
    if (name.find("xz") != std::string::npos) {
        return ArchiveFormat::tar_xz;
    }
    if (name.find("zstandard") != std::string::npos || name.find("zstd") != std::string::npos) {
        return ArchiveFormat::tar_zstd;
    }
    return ArchiveFormat::unknown;
}

ArchiveFormat format_from_reader(archive* reader) {
    auto format = format_from_name(archive_format_name(reader));
    if (format == ArchiveFormat::unknown) {
        switch (archive_format(reader) & ARCHIVE_FORMAT_BASE_MASK) {
        case ARCHIVE_FORMAT_ZIP:
            format = ArchiveFormat::zip;
            break;
        case ARCHIVE_FORMAT_7ZIP:
            format = ArchiveFormat::seven_zip;
            break;
        default:
            break;
        }
    }
    if (format == ArchiveFormat::unknown)
        format = format_from_name(archive_filter_name(reader, 0));
    return format;
}

std::string extension_for(ArchiveFormat format) {
    switch (format) {
    case ArchiveFormat::zip:
        return ".zip";
    case ArchiveFormat::tar_gzip:
        return ".tar.gz";
    case ArchiveFormat::tar_bzip2:
        return ".tar.bz2";
    case ArchiveFormat::tar_xz:
        return ".tar.xz";
    case ArchiveFormat::tar_zstd:
        return ".tar.zst";
    case ArchiveFormat::seven_zip:
        return ".7z";
    default:
        return ".zip";
    }
}

std::filesystem::path unique_path(const std::filesystem::path& requested) {
    if (!std::filesystem::exists(requested)) {
        return requested;
    }
    const auto parent = requested.parent_path();
    const auto stem = native_string(requested.stem());
    const auto extension = native_string(requested.extension());
    for (std::uint32_t index = 2; index < 10000; ++index) {
        auto name = stem;
        name.append(" (").append(std::to_string(index)).append(")").append(extension);
        const auto candidate = parent / utf8_path(name);
        if (!std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    throw ArchiveFailure(ArchiveFailure::Code::io, "Unable to choose a non-conflicting output name.");
}

std::filesystem::path unique_split_path(const std::filesystem::path& requested, std::size_t volume_count) {
    const auto parent = requested.parent_path();
    const auto stem = native_string(requested.stem());
    const auto extension = native_string(requested.extension());
    for (std::uint32_t index = 1; index < 10000; ++index) {
        auto candidate = requested;
        if (index != 1) {
            auto name = stem;
            name.append(" (").append(std::to_string(index)).append(")").append(extension);
            candidate = parent / utf8_path(name);
        }
        if (std::filesystem::exists(candidate))
            continue;
        bool volume_exists = false;
        for (std::size_t volume = 1; volume <= volume_count; ++volume) {
            auto volume_path = candidate;
            volume_path += ".";
            volume_path += volume < 10 ? "00" : volume < 100 ? "0" : "";
            volume_path += std::to_string(volume);
            if (std::filesystem::exists(volume_path)) {
                volume_exists = true;
                break;
            }
        }
        if (!volume_exists)
            return candidate;
    }
    throw ArchiveFailure(ArchiveFailure::Code::io, "Unable to choose a non-conflicting split archive name.");
}

std::filesystem::path make_staging(const std::filesystem::path& destination) {
    std::error_code error;
    std::filesystem::create_directories(destination, error);
    if (error) {
        throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot create the destination folder.");
    }
    std::random_device random;
    for (int attempt = 0; attempt != 32; ++attempt) {
        const auto token = std::to_string(static_cast<unsigned long long>(
                               std::chrono::steady_clock::now().time_since_epoch().count())) +
                           "-" + std::to_string(random());
        const auto candidate = destination / (".unfurl-stage-" + token);
        if (std::filesystem::create_directory(candidate, error)) {
            return candidate;
        }
        error.clear();
    }
    throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot create a private staging directory.");
}

std::string path_key(const std::filesystem::path& path) {
    auto value = native_string(path.lexically_normal());
    std::ranges::replace(value, '\\', '/');
#ifdef _WIN32
    std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    while (value.size() > 1 && value.back() == '/')
        value.pop_back();
    return value;
}

bool is_same_or_descendant(const std::filesystem::path& child, const std::filesystem::path& parent) {
    const auto child_key = path_key(child);
    const auto parent_key = path_key(parent);
    return child_key == parent_key || (child_key.size() > parent_key.size() && child_key.starts_with(parent_key) &&
                                       child_key[parent_key.size()] == '/');
}

bool is_windows_metadata(const std::filesystem::path& path) {
    auto filename = native_string(path.filename());
    std::ranges::transform(filename, filename.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return filename == "desktop.ini" || filename == "thumbs.db" || filename == "ehthumbs.db" ||
           filename == "iconcache.db";
}

void remove_tree(const std::filesystem::path& path) noexcept {
    try {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    } catch (...) {
        // Even the error_code overload may allocate. Cleanup during unwinding
        // must not terminate the process or replace the original archive error.
        return;
    }
}

struct TemporaryTree {
    std::filesystem::path value;

    ~TemporaryTree() {
        if (!value.empty())
            remove_tree(value);
    }
    TemporaryTree() = default;
    TemporaryTree(const TemporaryTree&) = delete;
    TemporaryTree& operator=(const TemporaryTree&) = delete;
};

struct ArchiveReadHandle {
    archive* value{};
    bool closed{};

    ~ArchiveReadHandle() {
        if (value != nullptr) {
            if (!closed)
                archive_read_close(value);
            archive_read_free(value);
        }
    }

    ArchiveReadHandle() = default;
    explicit ArchiveReadHandle(archive* archive_handle) : value(archive_handle) {
    }
    ArchiveReadHandle(const ArchiveReadHandle&) = delete;
    ArchiveReadHandle& operator=(const ArchiveReadHandle&) = delete;

    [[nodiscard]] archive* get() const noexcept {
        return value;
    }
    [[nodiscard]] archive* operator->() const noexcept {
        return value;
    }
    operator archive*() const noexcept {
        return value;
    }

    int close() {
        if (value == nullptr || closed)
            return ARCHIVE_OK;
        closed = true;
        return archive_read_close(value);
    }
};

struct ArchiveWriteHandle {
    archive* value{};
    bool closed{};

    ~ArchiveWriteHandle() {
        if (value != nullptr) {
            if (!closed)
                archive_write_close(value);
            archive_write_free(value);
        }
    }

    ArchiveWriteHandle() = default;
    explicit ArchiveWriteHandle(archive* archive_handle) : value(archive_handle) {
    }
    ArchiveWriteHandle(const ArchiveWriteHandle&) = delete;
    ArchiveWriteHandle& operator=(const ArchiveWriteHandle&) = delete;

    [[nodiscard]] archive* get() const noexcept {
        return value;
    }
    [[nodiscard]] archive* operator->() const noexcept {
        return value;
    }
    operator archive*() const noexcept {
        return value;
    }

    int close() {
        if (value == nullptr || closed)
            return ARCHIVE_OK;
        closed = true;
        return archive_write_close(value);
    }
};

struct ArchiveEntryHandle {
    archive_entry* value{};

    ~ArchiveEntryHandle() {
        if (value != nullptr)
            archive_entry_free(value);
    }

    ArchiveEntryHandle() = default;
    explicit ArchiveEntryHandle(archive_entry* archive_entry_value) : value(archive_entry_value) {
    }
    ArchiveEntryHandle(const ArchiveEntryHandle&) = delete;
    ArchiveEntryHandle& operator=(const ArchiveEntryHandle&) = delete;

    [[nodiscard]] archive_entry* get() const noexcept {
        return value;
    }
    operator archive_entry*() const noexcept {
        return value;
    }
};

std::filesystem::path prepare_archive_source(const std::filesystem::path& source, TemporaryTree& temporary,
                                             const std::function<bool()>& cancelled) {
    check_cancelled(cancelled);
    auto extension = native_string(source.extension());
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".001")
        return source;
    if (!std::filesystem::is_regular_file(source))
        throw ArchiveFailure(ArchiveFailure::Code::invalid_input, "The first archive volume is not a file.");

    temporary.value = make_staging(std::filesystem::temp_directory_path());
    const auto joined = temporary.value / source.stem();
    std::ofstream output(joined, std::ios::binary | std::ios::trunc);
    if (!output)
        throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot create a temporary archive stream.");

    const auto parent = source.parent_path().empty() ? std::filesystem::path(".") : source.parent_path();
    const auto prefix = native_string(source.stem()) + ".";
    std::uint32_t highest_volume = 1;
    std::error_code directory_error;
    for (std::filesystem::directory_iterator iterator(parent, directory_error), end; iterator != end;
         iterator.increment(directory_error)) {
        if (directory_error)
            break;
        const auto filename = native_string(iterator->path().filename());
        if (!filename.starts_with(prefix) || filename.size() < prefix.size() + 3)
            continue;
        const auto suffix = std::string_view(filename).substr(prefix.size());
        std::uint32_t index{};
        const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), index);
        if (parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size() ||
            !std::filesystem::is_regular_file(iterator->path())) {
            continue;
        }
        if (index != 0)
            highest_volume = highest_volume > index ? highest_volume : index;
    }
    if (directory_error)
        throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot inspect archive volumes.");

    std::vector<char> buffer(io_buffer_size);
    for (std::uint32_t index = 1; index <= highest_volume; ++index) {
        check_cancelled(cancelled);
        auto suffix = std::to_string(index);
        if (index < 10)
            suffix.insert(0, 2, '0');
        else if (index < 100)
            suffix.insert(0, 1, '0');
        const auto volume = parent / utf8_path(prefix + suffix);
        if (!std::filesystem::is_regular_file(volume))
            throw ArchiveFailure(ArchiveFailure::Code::io, "An archive volume is missing.");
        std::ifstream input(volume, std::ios::binary);
        if (!input)
            throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot read an archive volume.");
        while (input) {
            check_cancelled(cancelled);
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count > 0)
                output.write(buffer.data(), count);
        }
        if (input.bad() || !output)
            throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot join the archive volumes.");
    }
    output.close();
    if (!output)
        throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot finish the temporary archive stream.");
    return joined;
}

std::filesystem::path publish_staged_directory(const std::filesystem::path& staging,
                                               const std::filesystem::path& destination, std::string name,
                                               bool unwrap_single_directory = true) {
    std::filesystem::path root = staging;
    std::error_code error;
    std::filesystem::directory_iterator child(staging, error), end;
    std::filesystem::path single_child;
    if (!error && child != end) {
        single_child = child->path();
        child.increment(error);
    }
    if (error) {
        throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot inspect staged output.");
    }
    if (unwrap_single_directory && !single_child.empty() && child == end &&
        std::filesystem::is_directory(single_child, error) && !error) {
        root = std::move(single_child);
        name = native_string(root.filename());
    }
    const auto output = unique_path(destination / utf8_path(name));
    std::filesystem::rename(root, output, error);
    if (error) {
        throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot commit the completed archive operation.");
    }
    return output;
}

std::string entry_path(const archive_entry* entry) {
    const auto* pointer = archive_entry_pathname_utf8(const_cast<archive_entry*>(entry));
    if (pointer == nullptr) {
        pointer = archive_entry_pathname(const_cast<archive_entry*>(entry));
    }
    if (pointer == nullptr) {
        throw ArchiveFailure(ArchiveFailure::Code::unsafe_path, "The archive contains an undecodable filename.");
    }
    return pointer;
}

void validate_link(const archive_entry* entry, std::string_view path) {
    // Do not let a format resolve a hard-link target against the process directory.
    if (archive_entry_hardlink(const_cast<archive_entry*>(entry)) != nullptr)
        throw ArchiveFailure(ArchiveFailure::Code::unsafe_path, "Hard links are not extracted.");
    const auto validate = [path](const char* target, std::string_view kind) {
        if (target == nullptr) {
            return;
        }
        try {
            if (ArchiveEngine::safe_relative_path(target).empty())
                throw ArchiveFailure(ArchiveFailure::Code::unsafe_path, "The link target is empty.");
        } catch (const ArchiveFailure&) {
            throw ArchiveFailure(ArchiveFailure::Code::unsafe_path, "The archive contains an unsafe " +
                                                                        std::string(kind) + " at " + std::string(path) +
                                                                        ".");
        }
    };
    validate(archive_entry_symlink(const_cast<archive_entry*>(entry)), "symbolic link");
}

void write_entry_data(archive* writer, archive* reader, archive_entry* entry, std::uint64_t& bytes,
                      const std::function<bool()>& cancelled, const ArchiveEngine::ProgressCallback& progress,
                      std::string_view path) {
    check_status(archive_write_header(writer, entry), writer, "Cannot write an archive header.");
    ArchiveUpdate update;
    if (progress)
        update.path = path;
    if (archive_entry_filetype(entry) == AE_IFREG) {
        while (true) {
            check_cancelled(cancelled);
            const void* buffer = nullptr;
            size_t size = 0;
            la_int64_t offset = 0;
            const auto status = archive_read_data_block(reader, &buffer, &size, &offset);
            if (status == ARCHIVE_EOF) {
                break;
            }
            check_reader_status(status, reader, "Cannot read archive data.");
            const auto written = archive_write_data_block(writer, buffer, size, offset);
            // The disk writer returns ARCHIVE_OK (0) after accepting a block;
            // streaming writers may instead return the byte count.
            if (written < 0) {
                throw_archive(ArchiveFailure::Code::io, writer, "Cannot write archive data.");
            }
            bytes += static_cast<std::uint64_t>(size);
            if (progress) {
                update.bytes = bytes;
                progress(update);
            }
        }
    } else {
        check_reader_status(archive_read_data_skip(reader), reader, "Cannot skip archive metadata.");
    }
    check_status(archive_write_finish_entry(writer), writer, "Cannot finish an archive entry.");
}

} // namespace

ArchiveResult::ArchiveResult(std::vector<std::filesystem::path> outputs, std::uint64_t entries, std::uint64_t bytes)
    : outputs(std::move(outputs)), entries(entries), bytes(bytes) {
}

const std::filesystem::path& ArchiveResult::output() const {
    if (outputs.empty()) {
        throw std::logic_error("ArchiveResult has no outputs");
    }
    return outputs.front();
}

ArchiveFailure::ArchiveFailure(Code code, const std::string& message) : std::runtime_error(message), code_(code) {
}

ArchiveFailure::Code ArchiveFailure::code() const noexcept {
    return code_;
}

bool ArchiveEngine::is_archive(const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path)) {
        return false;
    }
    const auto extension = native_string(path.extension());
    std::string lower;
    lower.reserve(extension.size());
    for (const auto character : extension) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    static constexpr std::array<std::string_view, 11> extensions{".zip", ".7z",  ".rar", ".tar",  ".gz", ".bz2",
                                                                 ".xz",  ".zst", ".tgz", ".tbz2", ".txz"};
    return std::ranges::find(extensions, lower) != extensions.end() || lower == ".001";
}

std::string ArchiveEngine::stem(const std::filesystem::path& path) {
    auto value = native_string(path.filename());
    const auto lower = [&value] {
        std::string result = value;
        std::ranges::transform(result, result.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return result;
    }();
    for (const auto suffix : std::array<std::string_view, 4>{".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst"}) {
        if (lower.ends_with(suffix)) {
            value.resize(value.size() - suffix.size());
            return value.empty() ? "Archive" : value;
        }
    }
    if (lower.ends_with(".001")) {
        auto first_volume = path;
        first_volume.replace_extension();
        return stem(first_volume);
    }
    const auto result = native_string(path.stem());
    return result.empty() ? "Archive" : result;
}

std::string ArchiveEngine::safe_relative_path(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    if (value.size() > 4096 || value.front() == '/' || value.front() == '\\' ||
        value.find('\0') != std::string_view::npos) {
        throw ArchiveFailure(ArchiveFailure::Code::unsafe_path, "The archive contains an unsafe path.");
    }
    std::string normalized(value);
    std::ranges::replace(normalized, '\\', '/');
    if (normalized.find(':') != std::string::npos) {
        throw ArchiveFailure(ArchiveFailure::Code::unsafe_path, "The archive contains an absolute path.");
    }
    std::string result;
    result.reserve(normalized.size());
    std::size_t start = 0;
    std::size_t components = 0;
    while (start <= normalized.size()) {
        const auto end = normalized.find('/', start);
        const auto part =
            std::string_view(normalized).substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part == "..") {
            throw ArchiveFailure(ArchiveFailure::Code::unsafe_path, "The archive attempts to leave its destination.");
        }
        if (!part.empty() && part != ".") {
            if (!result.empty()) {
                result.push_back('/');
            }
            result += part;
            ++components;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    if (components > 127) {
        throw ArchiveFailure(ArchiveFailure::Code::unsafe_path, "The archive path is too deeply nested.");
    }
    return result;
}

ArchiveFormat ArchiveEngine::format_for_path(const std::filesystem::path& path) {
    const auto extension = native_string(path.extension());
    std::string lower;
    std::ranges::transform(extension, std::back_inserter(lower),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == ".zip")
        return ArchiveFormat::zip;
    if (lower == ".7z")
        return ArchiveFormat::seven_zip;
    if (lower == ".gz" || lower == ".tgz")
        return ArchiveFormat::tar_gzip;
    if (lower == ".bz2" || lower == ".tbz2")
        return ArchiveFormat::tar_bzip2;
    if (lower == ".xz" || lower == ".txz")
        return ArchiveFormat::tar_xz;
    if (lower == ".zst")
        return ArchiveFormat::tar_zstd;
    return ArchiveFormat::unknown;
}

ArchivePreview ArchiveEngine::preview(const std::filesystem::path& source, std::optional<std::string_view> password,
                                      std::size_t limit, const std::function<bool()>& cancelled) {
    [[maybe_unused]] const ArchiveLocale locale;
    if (!std::filesystem::is_regular_file(source)) {
        throw ArchiveFailure(ArchiveFailure::Code::invalid_input, "Choose an archive file to preview.");
    }
    check_cancelled(cancelled);
    TemporaryTree temporary;
    const auto prepared_source = prepare_archive_source(source, temporary, cancelled);
    ArchiveReadHandle reader{archive_read_new()};
    if (reader.get() == nullptr) {
        throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot create an archive reader.");
    }
    archive_read_support_filter_all(reader);
    archive_read_support_format_all(reader);
    if (password) {
        archive_read_add_passphrase(reader, std::string(*password).c_str());
    }
    auto status = open_archive_reader(reader, prepared_source);
    if (status < ARCHIVE_OK) {
        const auto* message = archive_error_string(reader);
        const std::string detail = message == nullptr ? "Cannot open the archive." : message;
        const auto lowered = [&detail] {
            std::string value = detail;
            std::ranges::transform(value, value.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }();
        if (lowered.find("passphrase") != std::string::npos || lowered.find("password") != std::string::npos ||
            lowered.find("encrypted") != std::string::npos) {
            throw ArchiveFailure(ArchiveFailure::Code::password_required, detail);
        }
        throw ArchiveFailure(ArchiveFailure::Code::io, detail);
    }
    ArchivePreview result;
    result.format = format_from_reader(reader);
    if (result.format == ArchiveFormat::unknown)
        result.format = format_for_path(prepared_source);
    while (true) {
        check_cancelled(cancelled);
        archive_entry* entry = nullptr;
        status = archive_read_next_header(reader, &entry);
        if (status == ARCHIVE_EOF)
            break;
        check_reader_status(status, reader, "Cannot read the archive header.");
        if (result.format == ArchiveFormat::unknown) {
            result.format = format_from_reader(reader);
        }
        result.encrypted = result.encrypted || archive_entry_is_encrypted(entry) > 0;
        if (result.items.size() >= limit) {
            result.truncated = true;
            break;
        }
        const auto path = safe_relative_path(entry_path(entry));
        result.items.push_back(
            ArchiveItem{path, static_cast<std::uint64_t>(std::max<la_int64_t>(0, archive_entry_size(entry))),
                        archive_entry_filetype(entry) == AE_IFDIR});
        check_reader_status(archive_read_data_skip(reader), reader, "Cannot scan the archive entry.");
    }
    check_status(reader.close(), reader, "Cannot finish archive preview.");
    return result;
}

ArchiveResult ArchiveEngine::extract(const std::filesystem::path& source, const std::filesystem::path& destination,
                                     std::optional<std::string_view> password, const std::function<bool()>& cancelled,
                                     const ProgressCallback& progress,
                                     const std::optional<std::vector<std::string>>& selected_paths) {
    [[maybe_unused]] const ArchiveLocale locale;
    if (!std::filesystem::is_regular_file(source)) {
        throw ArchiveFailure(ArchiveFailure::Code::invalid_input, "Choose an archive file to extract.");
    }
    check_cancelled(cancelled);
    std::map<std::string, bool, std::less<>> selection;
    if (selected_paths) {
        if (selected_paths->empty())
            throw ArchiveFailure(ArchiveFailure::Code::invalid_input, "Choose at least one archive item.");
        for (const auto& path : *selected_paths) {
            auto safe = safe_relative_path(path);
            if (safe.empty())
                throw ArchiveFailure(ArchiveFailure::Code::invalid_input, "Choose at least one archive item.");
            selection.emplace(std::move(safe), false);
        }
    }
    const auto staging = make_staging(destination);
    try {
        TemporaryTree temporary;
        const auto prepared_source = prepare_archive_source(source, temporary, cancelled);
        ArchiveReadHandle reader{archive_read_new()};
        ArchiveWriteHandle writer{archive_write_disk_new()};
        if (reader.get() == nullptr || writer.get() == nullptr) {
            throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot create archive readers.");
        }
        archive_read_support_filter_all(reader);
        archive_read_support_format_all(reader);
        if (password)
            archive_read_add_passphrase(reader, std::string(*password).c_str());
        check_reader_status(open_archive_reader(reader, prepared_source), reader, "Cannot open the archive.");
        constexpr int options = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_NO_OVERWRITE |
                                ARCHIVE_EXTRACT_SECURE_SYMLINKS | ARCHIVE_EXTRACT_SECURE_NODOTDOT;
        check_status(archive_write_disk_set_options(writer, options), writer, "Cannot configure archive extraction.");
        std::uint64_t bytes = 0;
        std::uint64_t entries = 0;
        while (true) {
            check_cancelled(cancelled);
            archive_entry* entry = nullptr;
            const auto status = archive_read_next_header(reader, &entry);
            if (status == ARCHIVE_EOF)
                break;
            check_reader_status(status, reader, "Cannot read the archive header.");
            const auto safe = safe_relative_path(entry_path(entry));
            if (safe.empty()) {
                check_status(archive_read_data_skip(reader), reader, "Cannot scan archive metadata.");
                continue;
            }
            const auto type = archive_entry_filetype(entry);
            validate_link(entry, safe);
            if (type != AE_IFREG && type != AE_IFDIR && type != AE_IFLNK) {
                throw ArchiveFailure(ArchiveFailure::Code::unsafe_path, "The archive contains a special file.");
            }
            if (selected_paths) {
                // Match complete path components, including implicit parent directories.
                bool included = false;
                auto prefix = std::string_view(safe);
                while (!prefix.empty()) {
                    if (const auto found = selection.find(prefix); found != selection.end()) {
                        found->second = true;
                        included = true;
                    }
                    const auto slash = prefix.rfind('/');
                    if (slash == std::string_view::npos)
                        break;
                    prefix = prefix.substr(0, slash);
                }
                if (!included) {
                    check_reader_status(archive_read_data_skip(reader), reader, "Cannot scan the archive entry.");
                    continue;
                }
            }
            const auto output = staging / utf8_path(safe);
#ifdef _WIN32
            archive_entry_copy_pathname_w(entry, output.c_str());
#else
            archive_entry_set_pathname(entry, output.c_str());
#endif
            archive_entry_set_perm(entry, archive_entry_perm(entry) & 0777);
            write_entry_data(writer, reader, entry, bytes, cancelled, progress, safe);
            ++entries;
        }
        check_status(writer.close(), writer, "Cannot finish extraction.");
        check_status(reader.close(), reader, "Cannot finish archive reading.");
        check_cancelled(cancelled);
        if (std::ranges::any_of(selection, [](const auto& item) { return !item.second; }))
            throw ArchiveFailure(ArchiveFailure::Code::invalid_input, "A selected archive item was not found.");
        const auto output = publish_staged_directory(staging, destination, stem(source), !selected_paths);
        remove_tree(staging);
        return ArchiveResult{{output}, entries, bytes};
    } catch (...) {
        remove_tree(staging);
        throw;
    }
}

ArchiveResult ArchiveEngine::compress(const std::vector<std::filesystem::path>& sources,
                                      const std::filesystem::path& destination, std::string_view name,
                                      const CompressionOptions& options, const std::function<bool()>& cancelled,
                                      const ProgressCallback& progress) {
    [[maybe_unused]] const ArchiveLocale locale;
    if (sources.empty()) {
        throw ArchiveFailure(ArchiveFailure::Code::invalid_input, "Choose at least one file or folder.");
    }
    check_cancelled(cancelled);
    std::string archive_name(name);
    if (archive_name.empty())
        archive_name = "Archive";
    if (archive_name.size() > 255 || archive_name == "." || archive_name == ".." ||
        archive_name.find_first_of("/\\:") != std::string::npos || archive_name.find('\0') != std::string::npos) {
        throw ArchiveFailure(ArchiveFailure::Code::invalid_input, "The archive name must be a simple file name.");
    }
    if (options.level < 0 || options.level > 9 || (options.split_size && *options.split_size == 0)) {
        throw ArchiveFailure(ArchiveFailure::Code::invalid_input, "Choose a compression level from 0 to 9.");
    }
    if (options.format == ArchiveFormat::unknown) {
        throw ArchiveFailure(ArchiveFailure::Code::unsupported_format, "Choose a supported archive format.");
    }
    if (options.password.size() > 1024 || options.password.find_first_of("\r\n") != std::string::npos ||
        options.password.find('\0') != std::string::npos) {
        throw ArchiveFailure(ArchiveFailure::Code::invalid_input,
                             "The password must be at most 1,024 bytes and contain no line breaks.");
    }
    if ((options.password.size() > 0 || options.split_size) && options.format != ArchiveFormat::zip) {
        throw ArchiveFailure(ArchiveFailure::Code::unsupported_format, "Passwords and split volumes use ZIP format.");
    }
    std::error_code error;
    std::filesystem::create_directories(destination, error);
    if (error)
        throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot create the destination folder.");
    const auto destination_canonical = std::filesystem::weakly_canonical(destination, error);
    if (error)
        throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot resolve the destination folder.");
    std::vector<std::string> names;
    for (const auto& source : sources) {
        if (!std::filesystem::exists(source)) {
            throw ArchiveFailure(ArchiveFailure::Code::invalid_input, "A selected item no longer exists.");
        }
        const auto canonical = std::filesystem::weakly_canonical(source, error);
        if (error ||
            (std::filesystem::is_directory(source) && is_same_or_descendant(destination_canonical, canonical))) {
            throw ArchiveFailure(ArchiveFailure::Code::invalid_input,
                                 "Save the archive outside the folder being compressed.");
        }
        auto item_name = native_string(source.filename());
        std::ranges::transform(item_name, item_name.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        names.push_back(std::move(item_name));
    }
    std::ranges::sort(names);
    if (std::ranges::adjacent_find(names) != names.end()) {
        throw ArchiveFailure(ArchiveFailure::Code::invalid_input, "Selected items must have distinct names.");
    }

    const auto staging = make_staging(destination);
    const auto temporary = staging / "archive.part";
    std::vector<std::filesystem::path> committed_outputs;
    try {
        ArchiveWriteHandle writer{archive_write_new()};
        if (writer.get() == nullptr)
            throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot create an archive writer.");
        if (options.format == ArchiveFormat::zip) {
            check_status(archive_write_set_format_zip(writer), writer, "Cannot configure ZIP output.");
            check_status(archive_write_set_format_option(writer, "zip", "hdrcharset", "UTF-8"), writer,
                         "Cannot configure Unicode ZIP filenames.");
            check_status(archive_write_set_format_option(writer, "zip", "compression-level",
                                                         std::to_string(options.level).c_str()),
                         writer, "Cannot configure ZIP compression.");
            if (!options.password.empty()) {
                check_status(archive_write_set_passphrase(writer, options.password.c_str()), writer,
                             "Cannot configure ZIP encryption.");
                check_status(archive_write_set_format_option(writer, "zip", "encryption",
                                                             options.zip_aes ? "aes256" : "zipcrypt"),
                             writer, "Cannot configure ZIP encryption.");
            }
        } else if (options.format == ArchiveFormat::seven_zip) {
            check_status(archive_write_set_format_7zip(writer), writer, "Cannot configure 7Z output.");
            check_status(archive_write_set_format_option(writer, "7zip", "compression-level",
                                                         std::to_string(options.level).c_str()),
                         writer, "Cannot configure 7Z compression.");
        } else {
            check_status(archive_write_set_format_pax_restricted(writer), writer, "Cannot configure TAR output.");
            switch (options.format) {
            case ArchiveFormat::tar_gzip:
                check_status(archive_write_add_filter_gzip(writer), writer, "Cannot configure gzip.");
                break;
            case ArchiveFormat::tar_bzip2:
                check_status(archive_write_add_filter_bzip2(writer), writer, "Cannot configure bzip2.");
                break;
            case ArchiveFormat::tar_xz:
                check_status(archive_write_add_filter_xz(writer), writer, "Cannot configure xz.");
                break;
            case ArchiveFormat::tar_zstd:
                check_status(archive_write_add_filter_zstd(writer), writer, "Cannot configure zstd.");
                break;
            default:
                break;
            }
        }
#ifdef _WIN32
        check_status(archive_write_open_filename_w(writer, temporary.c_str()), writer,
                     "Cannot open temporary archive output.");
#else
        check_status(archive_write_open_filename(writer, temporary.c_str()), writer,
                     "Cannot open temporary archive output.");
#endif
        std::uint64_t entries = 0;
        std::uint64_t bytes = 0;
        // One heap buffer serves the entire traversal; a buffer in the recursive
        // lambda would reserve 128 KiB of stack for every directory level.
        std::vector<char> buffer(io_buffer_size);
        const auto append = [&](const std::filesystem::path& file, const std::string& relative,
                                auto&& append_ref) -> void {
            check_cancelled(cancelled);
            std::error_code item_error;
            const auto status = std::filesystem::symlink_status(file, item_error);
            if (item_error)
                throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot inspect a selected item.");
            ArchiveEntryHandle entry{archive_entry_new()};
            if (entry.get() == nullptr)
                throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot create an archive entry.");
            archive_entry_set_pathname_utf8(entry, relative.c_str());
            if (!std::filesystem::is_symlink(status)) {
                const auto modified = std::filesystem::last_write_time(file, item_error);
                if (item_error)
                    throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot read the file modification time.");
                const auto timestamp = std::chrono::clock_cast<std::chrono::system_clock>(modified).time_since_epoch();
                const auto seconds = std::chrono::floor<std::chrono::seconds>(timestamp);
                const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(timestamp - seconds);
                archive_entry_set_mtime(entry, seconds.count(), static_cast<long>(nanoseconds.count()));
            }
            if (std::filesystem::is_directory(status)) {
                archive_entry_set_filetype(entry, AE_IFDIR);
                archive_entry_set_perm(entry, 0755);
                archive_entry_set_size(entry, 0);
                check_status(archive_write_header(writer, entry), writer, "Cannot write a directory entry.");
                check_status(archive_write_finish_entry(writer), writer, "Cannot finish a directory entry.");
                ++entries;
                std::error_code directory_error;
                for (std::filesystem::directory_iterator iterator(file, directory_error), end; iterator != end;
                     iterator.increment(directory_error)) {
                    if (directory_error)
                        break;
                    const auto child = iterator->path();
                    if (options.exclude_windows_metadata && is_windows_metadata(child))
                        continue;
                    append_ref(child, relative + "/" + native_string(child.filename()), append_ref);
                }
                if (directory_error)
                    throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot enumerate a selected folder.");
            } else if (std::filesystem::is_symlink(status)) {
                const auto target = std::filesystem::read_symlink(file, item_error);
                const auto target_name = item_error ? std::string{} : native_string(target);
                if (item_error || target_name.empty() || target.is_absolute()) {
                    throw ArchiveFailure(ArchiveFailure::Code::unsafe_path,
                                         "Absolute symbolic links are not archived.");
                }
                try {
                    if (ArchiveEngine::safe_relative_path(target_name).empty())
                        throw ArchiveFailure(ArchiveFailure::Code::unsafe_path, "The link target is empty.");
                } catch (const ArchiveFailure&) {
                    throw ArchiveFailure(ArchiveFailure::Code::unsafe_path, "Unsafe symbolic links are not archived.");
                }
                archive_entry_set_filetype(entry, AE_IFLNK);
                archive_entry_set_symlink_utf8(entry, target_name.c_str());
                archive_entry_set_size(entry, 0);
                check_status(archive_write_header(writer, entry), writer, "Cannot write a symbolic link.");
                check_status(archive_write_finish_entry(writer), writer, "Cannot finish a symbolic link.");
                ++entries;
            } else if (std::filesystem::is_regular_file(status)) {
                const auto size = std::filesystem::file_size(file, item_error);
                if (item_error) {
                    throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot stat a selected file.");
                }
                archive_entry_set_filetype(entry, AE_IFREG);
                archive_entry_set_perm(entry, 0644);
                archive_entry_set_size(entry, static_cast<la_int64_t>(size));
                check_status(archive_write_header(writer, entry), writer, "Cannot write a file entry.");
                std::ifstream input(file, std::ios::binary);
                if (!input) {
                    throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot read a selected file.");
                }
                std::uint64_t file_bytes = 0;
                ArchiveUpdate update;
                if (progress)
                    update.path = relative;
                while (input) {
                    check_cancelled(cancelled);
                    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                    const auto count = input.gcount();
                    if (count <= 0)
                        break;
                    const auto written = archive_write_data(writer, buffer.data(), static_cast<size_t>(count));
                    if (written != count) {
                        const auto* detail = archive_error_string(writer);
                        throw ArchiveFailure(ArchiveFailure::Code::io,
                                             detail == nullptr ? "Cannot write file data." : std::string(detail));
                    }
                    bytes += static_cast<std::uint64_t>(written);
                    file_bytes += static_cast<std::uint64_t>(written);
                    if (progress) {
                        update.bytes = bytes;
                        progress(update);
                    }
                }
                if (input.bad() || file_bytes != size)
                    throw ArchiveFailure(ArchiveFailure::Code::io,
                                         "The selected file changed or could not be read completely.");
                check_status(archive_write_finish_entry(writer), writer, "Cannot finish a file entry.");
                ++entries;
            } else {
                throw ArchiveFailure(ArchiveFailure::Code::unsafe_path, "Special files cannot be added to an archive.");
            }
        };
        for (const auto& source : sources) {
            if (options.exclude_windows_metadata && is_windows_metadata(source))
                continue;
            append(source, native_string(source.filename()), append);
        }
        check_status(writer.close(), writer, "Cannot finish archive output.");
        check_cancelled(cancelled);
        const auto suffix = extension_for(options.format);
        if (!options.split_size) {
            const auto output = unique_path(destination / utf8_path(archive_name + suffix));
            std::filesystem::rename(temporary, output, error);
            if (error)
                throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot commit archive output.");
            committed_outputs.push_back(output);
            remove_tree(staging);
            return ArchiveResult{{output}, entries, bytes};
        }
        std::vector<std::filesystem::path> volumes;
        std::ifstream input(temporary, std::ios::binary);
        if (!input)
            throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot read temporary archive output.");
        std::uint32_t index = 1;
        while (input) {
            check_cancelled(cancelled);
            const auto volume = staging / ("volume." + std::to_string(index));
            std::ofstream output(volume, std::ios::binary | std::ios::trunc);
            if (!output)
                throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot create a split archive volume.");
            std::uint64_t remaining = *options.split_size;
            while (remaining > 0 && input) {
                check_cancelled(cancelled);
                const auto amount = static_cast<std::streamsize>(std::min<std::uint64_t>(remaining, buffer.size()));
                input.read(buffer.data(), amount);
                const auto count = input.gcount();
                if (count <= 0)
                    break;
                output.write(buffer.data(), count);
                remaining -= static_cast<std::uint64_t>(count);
            }
            output.flush();
            if (!output)
                throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot write a split archive volume.");
            if (output.tellp() > 0)
                volumes.push_back(volume);
            output.close();
            if (!output)
                throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot finish a split archive volume.");
            ++index;
        }
        if (input.bad())
            throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot read temporary archive output.");
        input.close();
        std::vector<std::filesystem::path> outputs;
        const auto base = unique_split_path(destination / utf8_path(archive_name + suffix), volumes.size());
        for (std::size_t i = 0; i < volumes.size(); ++i) {
            auto target = base;
            target += ".";
            target += (i + 1 < 10 ? "00" : i + 1 < 100 ? "0" : "");
            target += std::to_string(i + 1);
            std::filesystem::rename(volumes[i], target, error);
            if (error)
                throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot commit split archive output.");
            committed_outputs.push_back(target);
            outputs.push_back(target);
        }
        remove_tree(staging);
        return ArchiveResult{std::move(outputs), entries, bytes};
    } catch (...) {
        remove_tree(staging);
        for (const auto& output : committed_outputs) {
            std::error_code ignored;
            std::filesystem::remove(output, ignored);
        }
        throw;
    }
}

} // namespace unfurl
