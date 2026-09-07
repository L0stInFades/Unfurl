#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace unfurl {

enum class ArchiveFormat {
    zip,
    seven_zip,
    tar_gzip,
    tar_bzip2,
    tar_xz,
    tar_zstd,
    unknown,
};

struct ArchiveItem {
    std::string path;
    std::uint64_t size{};
    bool directory{};
};

struct ArchivePreview {
    std::vector<ArchiveItem> items;
    ArchiveFormat format{ArchiveFormat::unknown};
    bool truncated{};
    bool encrypted{};
};

struct ArchiveUpdate {
    std::string path;
    std::uint64_t bytes{};
    std::optional<double> fraction;
};

struct ArchiveResult {
    ArchiveResult() = default;
    ArchiveResult(std::vector<std::filesystem::path> outputs, std::uint64_t entries, std::uint64_t bytes);

    std::vector<std::filesystem::path> outputs;
    std::uint64_t entries{};
    std::uint64_t bytes{};

    [[nodiscard]] const std::filesystem::path& output() const;
};

struct CompressionOptions {
    ArchiveFormat format{ArchiveFormat::zip};
    int level{6};
    std::string password;
    bool zip_aes{true};
    bool exclude_windows_metadata{true};
    std::optional<std::uint64_t> split_size;
};

class ArchiveFailure final : public std::runtime_error {
  public:
    enum class Code {
        invalid_input,
        unsupported_format,
        password_required,
        cancelled,
        io,
        unsafe_path,
    };

    ArchiveFailure(Code code, std::string message);
    [[nodiscard]] Code code() const noexcept;

  private:
    Code code_;
};

class ArchiveEngine final {
  public:
    using ProgressCallback = std::function<void(const ArchiveUpdate&)>;

    static constexpr std::size_t preview_limit = 250;

    [[nodiscard]] static bool is_archive(const std::filesystem::path& path);
    [[nodiscard]] static std::string stem(const std::filesystem::path& path);
    [[nodiscard]] static std::string safe_relative_path(std::string_view value);
    [[nodiscard]] static ArchiveFormat format_for_path(const std::filesystem::path& path);

    [[nodiscard]] static ArchivePreview preview(const std::filesystem::path& source,
                                                std::optional<std::string_view> password = std::nullopt,
                                                std::size_t limit = preview_limit,
                                                const std::function<bool()>& cancelled = {});

    [[nodiscard]] static ArchiveResult extract(
        const std::filesystem::path& source, const std::filesystem::path& destination,
        std::optional<std::string_view> password = std::nullopt, const std::function<bool()>& cancelled = {},
        const ProgressCallback& progress = {},
        // nullopt extracts everything; selected directories include their descendants.
        const std::optional<std::vector<std::string>>& selected_paths = std::nullopt);

    [[nodiscard]] static ArchiveResult compress(const std::vector<std::filesystem::path>& sources,
                                                const std::filesystem::path& destination, std::string_view name,
                                                const CompressionOptions& options = {},
                                                const std::function<bool()>& cancelled = {},
                                                const ProgressCallback& progress = {});
};

} // namespace unfurl
