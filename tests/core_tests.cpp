#include "unfurl/archive_engine.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

void expect_failure(std::string_view value) {
    bool failed = false;
    try {
        (void)unfurl::ArchiveEngine::safe_relative_path(value);
    } catch (const unfurl::ArchiveFailure& error) {
        failed = error.code() == unfurl::ArchiveFailure::Code::unsafe_path;
    }
    assert(failed);
}

int main() {
    const auto root = fs::temp_directory_path() /
                      ("unfurl-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const auto cleanup = [&] {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    };

    assert(unfurl::ArchiveEngine::safe_relative_path("folder\\item.txt") == "folder/item.txt");
    assert(unfurl::ArchiveEngine::safe_relative_path("./folder/item.txt") == "folder/item.txt");
    expect_failure("../escape.txt");
    expect_failure("C:/escape.txt");
    expect_failure("/escape.txt");
    expect_failure("folder/../../escape.txt");
    assert(unfurl::ArchiveEngine::stem(root / "photos.tar.gz") == "photos");
    assert(unfurl::ArchiveEngine::stem(root / "photos.zip.001") == "photos");

    const auto input = root / "notes.txt";
    {
        std::ofstream file(input, std::ios::binary);
        file << "unfurl archive core\n";
    }
    const auto archives = root / "archives";
    const auto compressed = unfurl::ArchiveEngine::compress({input}, archives, "notes");
    assert(compressed.outputs.size() == 1);
    assert(fs::is_regular_file(compressed.output()));
    const auto preview = unfurl::ArchiveEngine::preview(compressed.output());
    assert(!preview.items.empty());
    assert(preview.items.front().path == "notes.txt");

    const auto seven_zip =
        unfurl::ArchiveEngine::compress({input}, archives, "notes-7z",
                                        unfurl::CompressionOptions{.format = unfurl::ArchiveFormat::seven_zip,
                                                                   .level = 6,
                                                                   .password = {},
                                                                   .zip_aes = true,
                                                                   .exclude_windows_metadata = true,
                                                                   .split_size = std::nullopt});
    assert(seven_zip.outputs.size() == 1);
    const auto seven_zip_preview = unfurl::ArchiveEngine::preview(seven_zip.output());
    assert(seven_zip_preview.format == unfurl::ArchiveFormat::seven_zip);
    assert(!seven_zip_preview.items.empty());
    const auto seven_zip_extracted = unfurl::ArchiveEngine::extract(seven_zip.output(), root / "extracted-7z");
    assert(fs::exists(seven_zip_extracted.output() / "notes.txt"));

    const auto protected_archive =
        unfurl::ArchiveEngine::compress({input}, archives, "notes-protected",
                                        unfurl::CompressionOptions{.format = unfurl::ArchiveFormat::zip,
                                                                   .level = 6,
                                                                   .password = "correct horse",
                                                                   .zip_aes = true,
                                                                   .exclude_windows_metadata = true,
                                                                   .split_size = std::nullopt});
    bool password_required = false;
    bool encrypted_without_password = false;
    try {
        encrypted_without_password = unfurl::ArchiveEngine::preview(protected_archive.output()).encrypted;
    } catch (const unfurl::ArchiveFailure& error) {
        password_required = error.code() == unfurl::ArchiveFailure::Code::password_required;
    }
    assert(password_required || encrypted_without_password);
    const auto protected_preview = unfurl::ArchiveEngine::preview(protected_archive.output(), "correct horse");
    assert(!protected_preview.items.empty());
    const auto protected_extracted =
        unfurl::ArchiveEngine::extract(protected_archive.output(), root / "extracted-protected", "correct horse");
    assert(fs::exists(protected_extracted.output() / "notes.txt"));

    const auto extracted = unfurl::ArchiveEngine::extract(compressed.output(), root / "extracted");
    assert(extracted.outputs.size() == 1);
    assert(fs::exists(extracted.output() / "notes.txt"));
    {
        std::ifstream file(extracted.output() / "notes.txt", std::ios::binary);
        std::string value((std::istreambuf_iterator<char>(file)), {});
        assert(value == "unfurl archive core\n");
    }

    const auto split = unfurl::ArchiveEngine::compress({input}, archives, "notes-split",
                                                       unfurl::CompressionOptions{.format = unfurl::ArchiveFormat::zip,
                                                                                  .level = 6,
                                                                                  .password = {},
                                                                                  .zip_aes = true,
                                                                                  .exclude_windows_metadata = true,
                                                                                  .split_size = 32});
    assert(split.outputs.size() >= 2);
    for (const auto& volume : split.outputs)
        assert(fs::is_regular_file(volume));
    const auto split_preview = unfurl::ArchiveEngine::preview(split.outputs.front());
    assert(!split_preview.items.empty());
    const auto split_extracted = unfurl::ArchiveEngine::extract(split.outputs.front(), root / "extracted-split");
    assert(fs::exists(split_extracted.output() / "notes.txt"));

    bool cancelled = false;
    try {
        (void)unfurl::ArchiveEngine::compress({input}, archives, "cancelled", {}, [] { return true; });
    } catch (const unfurl::ArchiveFailure& error) {
        cancelled = error.code() == unfurl::ArchiveFailure::Code::cancelled;
    }
    assert(cancelled);
    cleanup();
    std::cout << "unfurl core tests passed\n";
}
