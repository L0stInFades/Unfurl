#ifdef NDEBUG
#undef NDEBUG
#endif

#include "unfurl/archive_engine.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
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

void write_octal_field(char* target, std::size_t width, std::uint64_t value) {
    std::fill(target, target + width, '\0');
    for (std::size_t index = width - 1; index-- > 0;) {
        target[index] = static_cast<char>('0' + (value & 7));
        value >>= 3;
    }
    target[width - 1] = ' ';
}

void write_tar_entry(std::ofstream& output, std::string_view entry_name, std::string_view link = {}) {
    std::array<char, 512> header{};
    std::memcpy(header.data(), entry_name.data(), entry_name.size());
    write_octal_field(header.data() + 100, 8, 0644);
    write_octal_field(header.data() + 108, 8, 0);
    write_octal_field(header.data() + 116, 8, 0);
    write_octal_field(header.data() + 124, 12, link.empty() ? 1 : 0);
    write_octal_field(header.data() + 136, 12, 0);
    std::fill(header.begin() + 148, header.begin() + 156, ' ');
    header[156] = link.empty() ? '0' : '1';
    if (!link.empty())
        std::memcpy(header.data() + 157, link.data(), link.size());
    std::memcpy(header.data() + 257, "ustar\0", 6);
    std::memcpy(header.data() + 263, "00", 2);
    std::uint32_t checksum = 0;
    for (const auto byte : header)
        checksum += static_cast<unsigned char>(byte);
    write_octal_field(header.data() + 148, 8, checksum);
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    if (link.empty()) {
        output.put('x');
        std::array<char, 511> padding{};
        output.write(padding.data(), static_cast<std::streamsize>(padding.size()));
    }
}

void write_malicious_tar(const fs::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    write_tar_entry(output, "../escape.txt");
    std::array<char, 1024> end{};
    output.write(end.data(), static_cast<std::streamsize>(end.size()));
}

int run_tests() {
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

    const auto empty_zip = archives / "empty.zip";
    {
        std::array<unsigned char, 22> end_of_central_directory{};
        end_of_central_directory[0] = 0x50;
        end_of_central_directory[1] = 0x4b;
        end_of_central_directory[2] = 0x05;
        end_of_central_directory[3] = 0x06;
        std::ofstream file(empty_zip, std::ios::binary);
        file.write(reinterpret_cast<const char*>(end_of_central_directory.data()),
                   static_cast<std::streamsize>(end_of_central_directory.size()));
    }
    const auto empty_preview = unfurl::ArchiveEngine::preview(empty_zip);
    assert(empty_preview.format == unfurl::ArchiveFormat::zip);
    assert(empty_preview.items.empty());

    bool rejected_preview_directory = false;
    try {
        (void)unfurl::ArchiveEngine::preview(root);
    } catch (const unfurl::ArchiveFailure& error) {
        rejected_preview_directory = error.code() == unfurl::ArchiveFailure::Code::invalid_input;
    }
    assert(rejected_preview_directory);

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
    bool wrong_password_rejected = false;
    try {
        (void)unfurl::ArchiveEngine::extract(protected_archive.output(), root / "wrong-password", "incorrect password");
    } catch (const unfurl::ArchiveFailure& error) {
        wrong_password_rejected = error.code() == unfurl::ArchiveFailure::Code::password_required;
    }
    assert(wrong_password_rejected);
    const auto protected_extracted =
        unfurl::ArchiveEngine::extract(protected_archive.output(), root / "extracted-protected", "correct horse");
    assert(fs::exists(protected_extracted.output() / "notes.txt"));

    const auto extracted = unfurl::ArchiveEngine::extract(compressed.output(), root / "extracted");
    assert(extracted.outputs.size() == 1);
    assert(fs::exists(extracted.output() / "notes.txt"));
    const auto restored_time = fs::last_write_time(extracted.output() / "notes.txt");
    const auto original_time = fs::last_write_time(input);
    assert(std::chrono::abs(restored_time - original_time) < std::chrono::seconds(2));
    {
        std::ifstream file(extracted.output() / "notes.txt", std::ios::binary);
        std::string value((std::istreambuf_iterator<char>(file)), {});
        assert(value == "unfurl archive core\n");
    }
    const auto extracted_again = unfurl::ArchiveEngine::extract(compressed.output(), root / "extracted");
    assert(extracted_again.output().filename().string() == "notes (2)");

    const auto malicious_archive = root / "malicious.tar";
    write_malicious_tar(malicious_archive);
    bool rejected_malicious_archive = false;
    try {
        (void)unfurl::ArchiveEngine::extract(malicious_archive, root / "malicious-output");
    } catch (const unfurl::ArchiveFailure& error) {
        rejected_malicious_archive = error.code() == unfurl::ArchiveFailure::Code::unsafe_path;
    }
    assert(rejected_malicious_archive);
    assert(!fs::exists(root / "malicious-output" / "escape.txt"));
    assert(!fs::exists(root / "escape.txt"));

    const auto source_folder = root / "source-folder";
    fs::create_directories(source_folder / "nested");
    {
        std::ofstream file(source_folder / "nested" / "item.txt", std::ios::binary);
        file << "nested archive item\n";
    }
    {
        std::ofstream metadata(source_folder / "desktop.ini", std::ios::binary);
        metadata << "[.ShellClassInfo]\n";
        std::ofstream thumbnails(source_folder / "Thumbs.db", std::ios::binary);
        thumbnails << "windows thumbnail cache\n";
    }
    const auto folder_archive = unfurl::ArchiveEngine::compress({source_folder}, archives, "source-folder");
    const auto folder_preview = unfurl::ArchiveEngine::preview(folder_archive.output());
    assert(folder_preview.items.size() >= 2);
    assert(std::ranges::none_of(folder_preview.items, [](const unfurl::ArchiveItem& item) {
        return item.path.ends_with("desktop.ini") || item.path.ends_with("Thumbs.db");
    }));
    const auto folder_extracted = unfurl::ArchiveEngine::extract(folder_archive.output(), root / "extracted-folder");
    assert(fs::exists(folder_extracted.output() / "nested" / "item.txt"));

    const auto unicode_folder = root / fs::path(u8"\u9879\u76ee\u8d44\u6599");
    const auto unicode_filename = fs::path(u8"\u4f1a\u8bae\u7b14\u8bb0.txt");
    fs::create_directories(unicode_folder);
    {
        std::ofstream file(unicode_folder / unicode_filename, std::ios::binary);
        file << "Unicode archive round trip\n";
    }
    const auto unicode_name_u8 = unicode_folder.filename().u8string();
    const std::string unicode_name(reinterpret_cast<const char*>(unicode_name_u8.data()), unicode_name_u8.size());
    const auto unicode_destination = root / fs::path(u8"\u538b\u7f29\u5305");
    const auto unicode_archive = unfurl::ArchiveEngine::compress({unicode_folder}, unicode_destination, unicode_name);
    assert(unicode_archive.output().filename() == fs::path(u8"\u9879\u76ee\u8d44\u6599.zip"));
    const auto unicode_preview = unfurl::ArchiveEngine::preview(unicode_archive.output());
    const auto unicode_entry_u8 = (unicode_folder.filename() / unicode_filename).generic_u8string();
    const std::string unicode_entry(reinterpret_cast<const char*>(unicode_entry_u8.data()), unicode_entry_u8.size());
    assert(std::ranges::any_of(unicode_preview.items, [&](const auto& item) { return item.path == unicode_entry; }));
    const auto unicode_extracted = unfurl::ArchiveEngine::extract(unicode_archive.output(), root / "unicode-output");
    assert(fs::exists(unicode_extracted.output() / unicode_filename));
    const auto unicode_duplicate = unfurl::ArchiveEngine::compress({unicode_folder}, unicode_destination, unicode_name);
    assert(unicode_duplicate.output().filename() == fs::path(u8"\u9879\u76ee\u8d44\u6599 (2).zip"));
    const auto unicode_split = unfurl::ArchiveEngine::compress(
        {unicode_folder}, unicode_destination, unicode_name,
        unfurl::CompressionOptions{.format = unfurl::ArchiveFormat::zip, .split_size = 64});
    const auto unicode_split_result =
        unfurl::ArchiveEngine::extract(unicode_split.output(), root / "unicode-split-output");
    assert(fs::exists(unicode_split_result.output() / unicode_filename));

    auto retain_metadata_options = unfurl::CompressionOptions{};
    retain_metadata_options.exclude_windows_metadata = false;
    const auto metadata_archive = unfurl::ArchiveEngine::compress(
        {source_folder}, archives, "source-folder-with-metadata", retain_metadata_options);
    const auto metadata_preview = unfurl::ArchiveEngine::preview(metadata_archive.output());
    assert(std::ranges::any_of(metadata_preview.items,
                               [](const unfurl::ArchiveItem& item) { return item.path.ends_with("desktop.ini"); }));

    const auto many_files = root / "many-files";
    fs::create_directories(many_files);
    for (int index = 0; index < 260; ++index) {
        std::ofstream file(many_files / ("item-" + std::to_string(index) + ".txt"), std::ios::binary);
        file << index;
    }
    const auto many_archive = unfurl::ArchiveEngine::compress({many_files}, archives, "many-files");
    const auto many_preview = unfurl::ArchiveEngine::preview(many_archive.output());
    assert(many_preview.items.size() == unfurl::ArchiveEngine::preview_limit);
    assert(many_preview.truncated);
    const auto complete_preview = unfurl::ArchiveEngine::preview(many_archive.output(), std::nullopt, 1000);
    assert(!complete_preview.truncated && complete_preview.items.size() == 261);
    const auto complete_extracted = unfurl::ArchiveEngine::extract(many_archive.output(), root / "all-many");
    assert(complete_extracted.entries == 261);
    const auto late_path = complete_preview.items.back().path;
    const auto late_extracted = unfurl::ArchiveEngine::extract(
        many_archive.output(), root / "partial-many", std::nullopt, {}, {}, std::vector<std::string>{late_path});
    assert(late_extracted.entries == 1);
    assert(fs::is_regular_file(late_extracted.output() / late_path));

    const auto selective_folder = root / "selective-input";
    fs::create_directories(selective_folder / "nested");
    fs::create_directories(selective_folder / "nested-other");
    fs::create_directories(selective_folder / "empty");
    for (const auto filename : {"keep.txt", "skip.txt", "nested/one.txt", "nested/two.txt", "nested-other/skip.txt"})
        std::ofstream(selective_folder / filename) << filename;
    const auto selective_archive = unfurl::ArchiveEngine::compress(
        {selective_folder / "keep.txt", selective_folder / "skip.txt", selective_folder / "nested",
         selective_folder / "nested-other", selective_folder / "empty"},
        archives, "selective");
    for (const auto& [selection, expected] : std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>>{
             {{"keep.txt"}, {"keep.txt"}},
             {{"nested/one.txt"}, {"nested/one.txt"}},
             {{"./nested/", "nested/one.txt"}, {"nested/one.txt", "nested/two.txt"}},
             {{"keep.txt", "nested/two.txt"}, {"keep.txt", "nested/two.txt"}},
             {{"empty"}, {}}}) {
        const auto result = unfurl::ArchiveEngine::extract(selective_archive.output(), root / "partial", std::nullopt,
                                                           {}, {}, selection);
        std::vector<std::string> actual;
        for (const auto& entry : fs::recursive_directory_iterator(result.output())) {
            if (entry.is_regular_file())
                actual.push_back(fs::relative(entry.path(), result.output()).generic_string());
        }
        std::ranges::sort(actual);
        assert(actual == expected);
        if (selection.front() == "empty")
            assert(fs::is_directory(result.output() / "empty"));
    }
    const auto partial_unicode =
        unfurl::ArchiveEngine::extract(unicode_archive.output(), root / "partial-unicode", std::nullopt, {}, {},
                                       std::vector<std::string>{unicode_entry});
    assert(partial_unicode.entries == 1);
    assert(fs::is_regular_file(partial_unicode.output() / unicode_folder.filename() / unicode_filename));
    for (const auto& selection :
         std::vector<std::vector<std::string>>{{}, {""}, {"absent.txt"}, {"keep.txt", "absent.txt"}}) {
        bool rejected = false;
        const auto destination = root / "rejected-selection";
        try {
            (void)unfurl::ArchiveEngine::extract(selective_archive.output(), destination, std::nullopt, {}, {},
                                                 selection);
        } catch (const unfurl::ArchiveFailure& error) {
            rejected = error.code() == unfurl::ArchiveFailure::Code::invalid_input;
        }
        assert(rejected);
        assert(!fs::exists(destination) || fs::is_empty(destination));
    }
    bool rejected_partial_unsafe = false;
    try {
        (void)unfurl::ArchiveEngine::extract(malicious_archive, root / "partial-unsafe", std::nullopt, {}, {},
                                             std::vector<std::string>{"keep.txt"});
    } catch (const unfurl::ArchiveFailure& error) {
        rejected_partial_unsafe = error.code() == unfurl::ArchiveFailure::Code::unsafe_path;
    }
    assert(rejected_partial_unsafe);
    assert(fs::is_empty(root / "partial-unsafe"));
    bool partial_cancel_requested = false;
    bool partial_cancelled = false;
    try {
        (void)unfurl::ArchiveEngine::extract(
            selective_archive.output(), root / "partial-cancelled", std::nullopt,
            [&] { return partial_cancel_requested; }, [&](const auto&) { partial_cancel_requested = true; },
            std::vector<std::string>{"nested"});
    } catch (const unfurl::ArchiveFailure& error) {
        partial_cancelled = error.code() == unfurl::ArchiveFailure::Code::cancelled;
    }
    assert(partial_cancel_requested && partial_cancelled);
    assert(fs::is_empty(root / "partial-cancelled"));

    bool rejected_recursive_destination = false;
    try {
        (void)unfurl::ArchiveEngine::compress({source_folder}, source_folder, "recursive");
    } catch (const unfurl::ArchiveFailure& error) {
        rejected_recursive_destination = error.code() == unfurl::ArchiveFailure::Code::invalid_input;
    }
    assert(rejected_recursive_destination);

    const auto duplicate = unfurl::ArchiveEngine::compress({input}, archives, "notes");
    assert(duplicate.output().filename().string() == "notes (2).zip");

    bool rejected_unsafe_name = false;
    try {
        (void)unfurl::ArchiveEngine::compress({input}, archives, "../escape");
    } catch (const unfurl::ArchiveFailure& error) {
        rejected_unsafe_name = error.code() == unfurl::ArchiveFailure::Code::invalid_input;
    }
    assert(rejected_unsafe_name);

    bool rejected_unknown_format = false;
    try {
        (void)unfurl::ArchiveEngine::compress({input}, archives, "unknown",
                                              unfurl::CompressionOptions{.format = unfurl::ArchiveFormat::unknown});
    } catch (const unfurl::ArchiveFailure& error) {
        rejected_unknown_format = error.code() == unfurl::ArchiveFailure::Code::unsupported_format;
    }
    assert(rejected_unknown_format);

    const std::array tar_formats{unfurl::ArchiveFormat::tar_gzip, unfurl::ArchiveFormat::tar_bzip2,
                                 unfurl::ArchiveFormat::tar_xz, unfurl::ArchiveFormat::tar_zstd};
    std::size_t tar_index = 0;
    for (const auto tar_format : tar_formats) {
        const auto tar_archive = unfurl::ArchiveEngine::compress({input}, archives, "notes-tar",
                                                                 unfurl::CompressionOptions{.format = tar_format});
        const auto tar_preview = unfurl::ArchiveEngine::preview(tar_archive.output());
        assert(tar_preview.format == tar_format);
        assert(!tar_preview.items.empty());
        const auto tar_extracted = unfurl::ArchiveEngine::extract(
            tar_archive.output(), root / ("extracted-tar-" + std::to_string(tar_index++)));
        assert(fs::exists(tar_extracted.output() / "notes.txt"));
    }

    const auto split = unfurl::ArchiveEngine::compress({input}, archives, "notes-split",
                                                       unfurl::CompressionOptions{.format = unfurl::ArchiveFormat::zip,
                                                                                  .level = 6,
                                                                                  .password = {},
                                                                                  .zip_aes = true,
                                                                                  .exclude_windows_metadata = true,
                                                                                  .split_size = 16});
    assert(split.outputs.size() >= 3);
    for (const auto& volume : split.outputs)
        assert(fs::is_regular_file(volume));
    const auto split_preview = unfurl::ArchiveEngine::preview(split.outputs.front());
    assert(!split_preview.items.empty());
    const auto split_extracted = unfurl::ArchiveEngine::extract(split.outputs.front(), root / "extracted-split");
    assert(fs::exists(split_extracted.output() / "notes.txt"));
    const auto split_duplicate =
        unfurl::ArchiveEngine::compress({input}, archives, "notes-split",
                                        unfurl::CompressionOptions{.format = unfurl::ArchiveFormat::zip,
                                                                   .level = 6,
                                                                   .password = {},
                                                                   .zip_aes = true,
                                                                   .exclude_windows_metadata = true,
                                                                   .split_size = 16});
    assert(split_duplicate.output().filename().string() == "notes-split (2).zip.001");

    std::error_code missing_volume_error;
    fs::remove(split.outputs[1], missing_volume_error);
    assert(!missing_volume_error);
    bool missing_volume_rejected = false;
    try {
        (void)unfurl::ArchiveEngine::preview(split.outputs.front());
    } catch (const unfurl::ArchiveFailure& error) {
        missing_volume_rejected = error.code() == unfurl::ArchiveFailure::Code::io;
    }
    assert(missing_volume_rejected);

    bool cancelled = false;
    try {
        (void)unfurl::ArchiveEngine::compress({input}, archives, "cancelled", {}, [] { return true; });
    } catch (const unfurl::ArchiveFailure& error) {
        cancelled = error.code() == unfurl::ArchiveFailure::Code::cancelled;
    }
    assert(cancelled);

    bool extract_cancelled = false;
    try {
        (void)unfurl::ArchiveEngine::extract(compressed.output(), root / "cancelled-extract", std::nullopt,
                                             [] { return true; });
    } catch (const unfurl::ArchiveFailure& error) {
        extract_cancelled = error.code() == unfurl::ArchiveFailure::Code::cancelled;
    }
    assert(extract_cancelled);
    assert(!fs::exists(root / "cancelled-extract"));

    const auto stream_input = root / "stream.bin";
    {
        std::array<char, 128 * 1024> block{};
        block.fill('x');
        std::ofstream file(stream_input, std::ios::binary);
        for (int index = 0; index < 4; ++index)
            file.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
    bool cancel_requested = false;
    bool stream_cancelled = false;
    try {
        (void)unfurl::ArchiveEngine::compress(
            {stream_input}, archives, "cancelled-stream", {}, [&] { return cancel_requested; },
            [&](const unfurl::ArchiveUpdate& update) { cancel_requested = update.bytes >= 128 * 1024; });
    } catch (const unfurl::ArchiveFailure& error) {
        stream_cancelled = error.code() == unfurl::ArchiveFailure::Code::cancelled;
    }
    assert(cancel_requested && stream_cancelled);
    assert(!fs::exists(archives / "cancelled-stream.zip"));

    // A deep traversal must not put the streaming buffer on every recursive frame.
    const auto deep_root = root / "deep";
    auto deep_directory = deep_root;
    for (int depth = 0; depth < 24; ++depth)
        deep_directory /= "d";
    fs::create_directories(deep_directory);
    const auto deep_input = deep_directory / "leaf.txt";
    std::ofstream(deep_input) << "deep archive fixture";
    const auto deep_archive = unfurl::ArchiveEngine::compress({deep_root}, archives, "deep");
    const auto deep_output = unfurl::ArchiveEngine::extract(deep_archive.output(), root / "deep-output");
    assert(fs::file_size(deep_output.output() / fs::relative(deep_input, deep_root)) == fs::file_size(deep_input));

    const auto changing_input = root / "changing.bin";
    fs::copy_file(stream_input, changing_input);
    bool shortened = false;
    bool incomplete_rejected = false;
    try {
        (void)unfurl::ArchiveEngine::compress({changing_input}, archives, "incomplete", {}, {},
                                              [&](const unfurl::ArchiveUpdate&) {
                                                  if (!shortened) {
                                                      fs::resize_file(changing_input, 0);
                                                      shortened = true;
                                                  }
                                              });
    } catch (const unfurl::ArchiveFailure& error) {
        incomplete_rejected = error.code() == unfurl::ArchiveFailure::Code::io;
    }
    assert(shortened && incomplete_rejected);
    assert(!fs::exists(archives / "incomplete.zip"));

    const auto hardlink_archive = archives / "hardlink.tar";
    {
        std::ofstream output(hardlink_archive, std::ios::binary);
        write_tar_entry(output, "target.txt");
        write_tar_entry(output, "alias.txt", "target.txt");
        std::array<char, 1024> end{};
        output.write(end.data(), end.size());
    }
    bool hardlink_rejected = false;
    try {
        (void)unfurl::ArchiveEngine::extract(hardlink_archive, root / "hardlink-output");
    } catch (const unfurl::ArchiveFailure& error) {
        hardlink_rejected = error.code() == unfurl::ArchiveFailure::Code::unsafe_path;
    }
    assert(hardlink_rejected);
    assert(fs::is_empty(root / "hardlink-output"));
    const auto missing_link_archive = archives / "missing-link.tar";
    {
        std::ofstream output(missing_link_archive, std::ios::binary);
        write_tar_entry(output, "alias.txt", "outside.txt");
        std::array<char, 1024> end{};
        output.write(end.data(), end.size());
    }
    // The target exists in the caller's current directory, outside staging.
    std::ofstream(root / "outside.txt") << "private data";
    const auto previous_directory = fs::current_path();
    fs::current_path(root);
    bool outside_link_rejected = false;
    try {
        (void)unfurl::ArchiveEngine::extract(missing_link_archive, root / "missing-link-output");
    } catch (const unfurl::ArchiveFailure&) {
        outside_link_rejected = true;
    }
    fs::current_path(previous_directory);
    assert(outside_link_rejected);
    assert(fs::is_empty(root / "missing-link-output"));

    const auto many_volumes = unfurl::ArchiveEngine::compress(
        {stream_input}, archives, "many-volumes",
        unfurl::CompressionOptions{.format = unfurl::ArchiveFormat::zip, .level = 0, .split_size = 512});
    assert(many_volumes.outputs.size() > 999);
    const auto many_volumes_result =
        unfurl::ArchiveEngine::extract(many_volumes.output(), root / "many-volumes-output");
    assert(fs::file_size(many_volumes_result.output() / "stream.bin") == fs::file_size(stream_input));

    cancel_requested = false;
    stream_cancelled = false;
    const auto cancelled_stream_destination = root / "cancelled-stream-extract";
    try {
        (void)unfurl::ArchiveEngine::extract(
            many_volumes.output(), cancelled_stream_destination, std::nullopt, [&] { return cancel_requested; },
            [&](const unfurl::ArchiveUpdate& update) { cancel_requested = update.bytes > 0; });
    } catch (const unfurl::ArchiveFailure& error) {
        stream_cancelled = error.code() == unfurl::ArchiveFailure::Code::cancelled;
    }
    assert(cancel_requested && stream_cancelled);
    assert(fs::is_empty(cancelled_stream_destination));
    for (const auto& entry : fs::recursive_directory_iterator(root))
        assert(!entry.path().filename().u8string().starts_with(u8".unfurl-stage-"));

    cleanup();
    std::cout << "unfurl core tests passed\n";
    return 0;
}

int main() {
    try {
        return run_tests();
    } catch (const std::exception& error) {
        std::cerr << "unfurl core test failed: " << error.what() << '\n';
        return 1;
    }
}
