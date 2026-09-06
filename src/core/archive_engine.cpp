#include "unfurl/archive_engine.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <random>
#include <ranges>
#include <system_error>
#include <utility>

namespace unfurl {
namespace {

std::string native_string(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
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
    const std::string name(value);
    if (name.find("ZIP") != std::string::npos) {
        return ArchiveFormat::zip;
    }
    if (name.find("7-Zip") != std::string::npos || name.find("7zip") != std::string::npos) {
        return ArchiveFormat::seven_zip;
    }
    if (name.find("gzip") != std::string::npos) {
        return ArchiveFormat::tar_gzip;
    }
    if (name.find("bzip") != std::string::npos) {
        return ArchiveFormat::tar_bzip2;
    }
    if (name.find("XZ") != std::string::npos || name.find("xz") != std::string::npos) {
        return ArchiveFormat::tar_xz;
    }
    if (name.find("Zstandard") != std::string::npos || name.find("zstd") != std::string::npos) {
        return ArchiveFormat::tar_zstd;
    }
    return ArchiveFormat::unknown;
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
    const auto stem = requested.stem().string();
    const auto extension = requested.extension().string();
    for (std::uint32_t index = 2; index < 10000; ++index) {
        const auto candidate = parent / (stem + " (" + std::to_string(index) + ")" + extension);
        if (!std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    throw ArchiveFailure(ArchiveFailure::Code::io, "Unable to choose a non-conflicting output name.");
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

void remove_tree(const std::filesystem::path& path) noexcept {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
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

std::filesystem::path prepare_archive_source(const std::filesystem::path& source, TemporaryTree& temporary,
                                             const std::function<bool()>& cancelled) {
    check_cancelled(cancelled);
    auto extension = source.extension().string();
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

    std::vector<char> buffer(1024 * 1024);
    for (std::uint32_t index = 1;; ++index) {
        check_cancelled(cancelled);
        auto suffix = std::to_string(index);
        if (index < 10)
            suffix.insert(0, 2, '0');
        else if (index < 100)
            suffix.insert(0, 1, '0');
        const auto volume = source.parent_path() / (source.stem().string() + "." + suffix);
        if (index != 1 && !std::filesystem::exists(volume))
            break;
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
                                               const std::filesystem::path& destination, std::string name) {
    std::filesystem::path root = staging;
    std::error_code error;
    std::vector<std::filesystem::path> children;
    for (const auto& child : std::filesystem::directory_iterator(staging, error)) {
        if (!error) {
            children.push_back(child.path());
        }
    }
    if (error) {
        throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot inspect staged output.");
    }
    if (children.size() == 1 && std::filesystem::is_directory(children.front(), error) && !error) {
        root = children.front();
        name = root.filename().string();
    }
    const auto output = unique_path(destination / name);
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
    const auto validate = [path](const char* target, std::string_view kind) {
        if (target == nullptr) {
            return;
        }
        try {
            (void)ArchiveEngine::safe_relative_path(target);
        } catch (const ArchiveFailure&) {
            throw ArchiveFailure(ArchiveFailure::Code::unsafe_path, "The archive contains an unsafe " +
                                                                        std::string(kind) + " at " + std::string(path) +
                                                                        ".");
        }
    };
    validate(archive_entry_symlink(const_cast<archive_entry*>(entry)), "symbolic link");
    validate(archive_entry_hardlink(const_cast<archive_entry*>(entry)), "hard link");
}

void write_entry_data(archive* writer, archive* reader, archive_entry* entry, std::uint64_t& bytes,
                      const std::function<bool()>& cancelled, const ArchiveEngine::ProgressCallback& progress,
                      std::string_view path) {
    check_status(archive_write_header(writer, entry), writer, "Cannot write an archive header.");
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
                progress(ArchiveUpdate{std::string(path), bytes, std::nullopt});
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

ArchiveFailure::ArchiveFailure(Code code, std::string message) : std::runtime_error(std::move(message)), code_(code) {
}

ArchiveFailure::Code ArchiveFailure::code() const noexcept {
    return code_;
}

bool ArchiveEngine::is_archive(const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path)) {
        return false;
    }
    const auto extension = path.extension().string();
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
    auto value = path.filename().string();
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
    const auto result = path.stem().string();
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
    std::size_t start = 0;
    std::size_t components = 0;
    while (start <= normalized.size()) {
        const auto end = normalized.find('/', start);
        const auto part = normalized.substr(start, end == std::string::npos ? std::string::npos : end - start);
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
    const auto extension = path.extension().string();
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
    check_cancelled(cancelled);
    TemporaryTree temporary;
    const auto prepared_source = prepare_archive_source(source, temporary, cancelled);
    archive* reader = archive_read_new();
    if (reader == nullptr) {
        throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot create an archive reader.");
    }
    const auto cleanup = [&] { archive_read_free(reader); };
    archive_read_support_filter_all(reader);
    archive_read_support_format_all(reader);
    if (password) {
        archive_read_add_passphrase(reader, std::string(*password).c_str());
    }
    const auto source_name = native_string(prepared_source);
    auto status = archive_read_open_filename(reader, source_name.c_str(), 128 * 1024);
    if (status < ARCHIVE_OK) {
        const auto* message = archive_error_string(reader);
        const std::string detail = message == nullptr ? "Cannot open the archive." : message;
        cleanup();
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
    while (true) {
        check_cancelled(cancelled);
        archive_entry* entry = nullptr;
        status = archive_read_next_header(reader, &entry);
        if (status == ARCHIVE_EOF)
            break;
        check_reader_status(status, reader, "Cannot read the archive header.");
        if (result.format == ArchiveFormat::unknown) {
            result.format = format_from_name(archive_format_name(reader));
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
    archive_read_close(reader);
    cleanup();
    return result;
}

ArchiveResult ArchiveEngine::extract(const std::filesystem::path& source, const std::filesystem::path& destination,
                                     std::optional<std::string_view> password, const std::function<bool()>& cancelled,
                                     const ProgressCallback& progress) {
    if (!std::filesystem::is_regular_file(source)) {
        throw ArchiveFailure(ArchiveFailure::Code::invalid_input, "Choose an archive file to extract.");
    }
    const auto staging = make_staging(destination);
    try {
        TemporaryTree temporary;
        const auto prepared_source = prepare_archive_source(source, temporary, cancelled);
        archive* reader = archive_read_new();
        archive* writer = archive_write_disk_new();
        if (reader == nullptr || writer == nullptr) {
            if (reader)
                archive_read_free(reader);
            if (writer)
                archive_write_free(writer);
            throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot create archive readers.");
        }
        archive_read_support_filter_all(reader);
        archive_read_support_format_all(reader);
        if (password)
            archive_read_add_passphrase(reader, std::string(*password).c_str());
        const auto source_name = native_string(prepared_source);
        check_reader_status(archive_read_open_filename(reader, source_name.c_str(), 128 * 1024), reader,
                            "Cannot open the archive.");
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
            if (type != AE_IFREG && type != AE_IFDIR && type != AE_IFLNK) {
                throw ArchiveFailure(ArchiveFailure::Code::unsafe_path, "The archive contains a special file.");
            }
            validate_link(entry, safe);
            const auto output = staging / std::filesystem::path(safe);
            const auto output_name = native_string(output);
            archive_entry_set_pathname(entry, output_name.c_str());
            archive_entry_set_perm(entry, archive_entry_perm(entry) & 0777);
            write_entry_data(writer, reader, entry, bytes, cancelled, progress, safe);
            ++entries;
        }
        check_status(archive_write_close(writer), writer, "Cannot finish extraction.");
        archive_write_free(writer);
        archive_read_close(reader);
        archive_read_free(reader);
        check_cancelled(cancelled);
        const auto output = publish_staged_directory(staging, destination, stem(source));
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
    if (sources.empty()) {
        throw ArchiveFailure(ArchiveFailure::Code::invalid_input, "Choose at least one file or folder.");
    }
    if (options.level < 0 || options.level > 9 || (options.split_size && *options.split_size == 0)) {
        throw ArchiveFailure(ArchiveFailure::Code::invalid_input, "Choose a compression level from 0 to 9.");
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
        if (error || (std::filesystem::is_directory(source) &&
                      canonical.string().starts_with(destination_canonical.string() +
                                                     std::string(1, std::filesystem::path::preferred_separator)))) {
            throw ArchiveFailure(ArchiveFailure::Code::invalid_input,
                                 "Save the archive outside the folder being compressed.");
        }
        auto item_name = source.filename().string();
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
    archive* writer = archive_write_new();
    if (writer == nullptr) {
        remove_tree(staging);
        throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot create an archive writer.");
    }
    try {
        if (options.format == ArchiveFormat::zip) {
            check_status(archive_write_set_format_zip(writer), writer, "Cannot configure ZIP output.");
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
        check_status(archive_write_open_filename(writer, native_string(temporary).c_str()), writer,
                     "Cannot open temporary archive output.");
        std::uint64_t entries = 0;
        std::uint64_t bytes = 0;
        const auto append = [&](const std::filesystem::path& file, const std::string& relative,
                                auto&& append_ref) -> void {
            check_cancelled(cancelled);
            std::error_code item_error;
            const auto status = std::filesystem::symlink_status(file, item_error);
            if (item_error)
                throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot inspect a selected item.");
            archive_entry* entry = archive_entry_new();
            if (entry == nullptr)
                throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot create an archive entry.");
            const auto entry_name = native_string(std::filesystem::path(relative));
            archive_entry_set_pathname(entry, entry_name.c_str());
            if (std::filesystem::is_directory(status)) {
                archive_entry_set_filetype(entry, AE_IFDIR);
                archive_entry_set_perm(entry, 0755);
                archive_entry_set_size(entry, 0);
                check_status(archive_write_header(writer, entry), writer, "Cannot write a directory entry.");
                check_status(archive_write_finish_entry(writer), writer, "Cannot finish a directory entry.");
                ++entries;
                for (const auto& child : std::filesystem::directory_iterator(
                         file, std::filesystem::directory_options::skip_permission_denied)) {
                    append_ref(child.path(), relative + "/" + child.path().filename().string(), append_ref);
                }
            } else if (std::filesystem::is_symlink(status)) {
                const auto target = std::filesystem::read_symlink(file, item_error);
                if (item_error || target.is_absolute() || target.string().find(':') != std::string::npos) {
                    archive_entry_free(entry);
                    throw ArchiveFailure(ArchiveFailure::Code::unsafe_path,
                                         "Absolute symbolic links are not archived.");
                }
                archive_entry_set_filetype(entry, AE_IFLNK);
                archive_entry_set_symlink(entry, native_string(target).c_str());
                archive_entry_set_size(entry, 0);
                check_status(archive_write_header(writer, entry), writer, "Cannot write a symbolic link.");
                check_status(archive_write_finish_entry(writer), writer, "Cannot finish a symbolic link.");
                ++entries;
            } else if (std::filesystem::is_regular_file(status)) {
                const auto size = std::filesystem::file_size(file, item_error);
                if (item_error) {
                    archive_entry_free(entry);
                    throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot stat a selected file.");
                }
                archive_entry_set_filetype(entry, AE_IFREG);
                archive_entry_set_perm(entry, 0644);
                archive_entry_set_size(entry, static_cast<la_int64_t>(size));
                check_status(archive_write_header(writer, entry), writer, "Cannot write a file entry.");
                std::ifstream input(file, std::ios::binary);
                if (!input) {
                    archive_entry_free(entry);
                    throw ArchiveFailure(ArchiveFailure::Code::io, "Cannot read a selected file.");
                }
                std::array<char, 128 * 1024> buffer{};
                while (input) {
                    check_cancelled(cancelled);
                    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                    const auto count = input.gcount();
                    if (count <= 0)
                        break;
                    const auto written = archive_write_data(writer, buffer.data(), static_cast<size_t>(count));
                    if (written != count) {
                        archive_entry_free(entry);
                        const auto* detail = archive_error_string(writer);
                        throw ArchiveFailure(ArchiveFailure::Code::io,
                                             detail == nullptr ? "Cannot write file data." : std::string(detail));
                    }
                    bytes += static_cast<std::uint64_t>(written);
                    if (progress)
                        progress(ArchiveUpdate{relative, bytes, std::nullopt});
                }
                check_status(archive_write_finish_entry(writer), writer, "Cannot finish a file entry.");
                ++entries;
            } else {
                archive_entry_free(entry);
                throw ArchiveFailure(ArchiveFailure::Code::unsafe_path, "Special files cannot be added to an archive.");
            }
            archive_entry_free(entry);
        };
        for (const auto& source : sources) {
            append(source, source.filename().string(), append);
        }
        check_status(archive_write_close(writer), writer, "Cannot finish archive output.");
        archive_write_free(writer);
        writer = nullptr;
        check_cancelled(cancelled);
        const auto suffix = extension_for(options.format);
        if (!options.split_size) {
            const auto output = unique_path(destination / (std::string(name) + suffix));
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
        std::vector<char> buffer(1024 * 1024);
        while (input) {
            check_cancelled(cancelled);
            const auto volume = staging / ("volume." + std::to_string(index));
            std::ofstream output(volume, std::ios::binary | std::ios::trunc);
            std::uint64_t remaining = *options.split_size;
            while (remaining > 0 && input) {
                const auto amount = static_cast<std::streamsize>(std::min<std::uint64_t>(remaining, buffer.size()));
                input.read(buffer.data(), amount);
                const auto count = input.gcount();
                if (count <= 0)
                    break;
                output.write(buffer.data(), count);
                remaining -= static_cast<std::uint64_t>(count);
            }
            if (output.tellp() > 0)
                volumes.push_back(volume);
            ++index;
        }
        std::vector<std::filesystem::path> outputs;
        const auto base = unique_path(destination / (std::string(name) + suffix));
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
        if (writer != nullptr)
            archive_write_free(writer);
        remove_tree(staging);
        for (const auto& output : committed_outputs) {
            std::error_code ignored;
            std::filesystem::remove(output, ignored);
        }
        throw;
    }
}

} // namespace unfurl
