#pragma once

#include "unfurl/archive_engine.hpp"

#include <optional>
#include <string_view>
#include <utility>

namespace unfurl::localization {

inline std::wstring_view error_message(std::string_view message, std::optional<ArchiveFailure::Code> code = {}) {
    // Source messages remain diagnostic identifiers; the archive engine stays language-neutral.
    static constexpr std::pair<std::string_view, std::wstring_view> translations[] = {
        {"Cannot initialize Unicode archive support.", L"无法初始化压缩包的 Unicode 支持。"},
        {"The archive operation was cancelled.", L"已取消。"},
        {"Unable to choose a non-conflicting output name.", L"无法生成不与现有文件重名的输出名称。"},
        {"Unable to choose a non-conflicting split archive name.", L"无法生成不与现有文件重名的分卷名称。"},
        {"Cannot create the destination folder.", L"无法创建目标文件夹。"},
        {"Cannot create a private staging directory.", L"无法创建独立的临时工作文件夹。"},
        {"The first archive volume is not a file.", L"压缩包的第一个分卷不是有效文件。"},
        {"Cannot create a temporary archive stream.", L"无法创建压缩包临时数据流。"},
        {"Cannot inspect archive volumes.", L"无法检查压缩包分卷。"},
        {"An archive volume is missing.", L"压缩包缺少分卷，请确认所有分卷均位于同一文件夹中。"},
        {"Cannot read an archive volume.", L"无法读取压缩包分卷。"},
        {"Cannot join the archive volumes.", L"无法合并压缩包分卷。"},
        {"Cannot finish the temporary archive stream.", L"无法完成压缩包临时数据流的写入。"},
        {"Cannot inspect staged output.", L"无法检查临时输出结果。"},
        {"Cannot commit the completed archive operation.", L"无法将已完成的结果保存到目标位置。"},
        {"The archive contains an undecodable filename.", L"压缩包中包含无法解码的文件名。"},
        {"The link target is empty.", L"链接的目标路径为空。"},
        {"Hard links are not extracted.", L"不支持解压硬链接。"},
        {"Cannot write an archive header.", L"无法写入压缩包条目的头部信息。"},
        {"Cannot read archive data.", L"无法读取压缩包数据。"},
        {"Cannot write archive data.", L"无法写入压缩包数据。"},
        {"Cannot skip archive metadata.", L"无法跳过压缩包元数据。"},
        {"Cannot finish an archive entry.", L"无法完成压缩包条目的写入。"},
        {"The archive contains an unsafe path.", L"压缩包中包含不安全的路径。"},
        {"The archive contains an absolute path.", L"压缩包中包含不允许解压的绝对路径。"},
        {"The archive attempts to leave its destination.", L"压缩包中的路径试图访问目标文件夹以外的位置。"},
        {"The archive path is too deeply nested.", L"压缩包中的路径层级过深。"},
        {"Choose an archive file to preview.", L"请选择要预览的压缩包文件。"},
        {"Cannot create an archive reader.", L"无法初始化压缩包读取器。"},
        {"Cannot create archive readers.", L"无法初始化压缩包读取器。"},
        {"Cannot open the archive.", L"无法打开压缩包。"},
        {"Cannot read the archive header.", L"无法读取压缩包的头部信息。"},
        {"Cannot scan the archive entry.", L"无法读取压缩包中的条目信息。"},
        {"Cannot finish archive preview.", L"无法完成压缩包预览。"},
        {"Choose an archive file to extract.", L"请选择要解压的压缩包文件。"},
        {"Choose at least one archive item.", L"请至少选择一个要解压的项目。"},
        {"A selected archive item was not found.", L"压缩包中找不到所选项目，请重新读取压缩包。"},
        {"Cannot configure archive extraction.", L"无法初始化解压设置。"},
        {"Cannot scan archive metadata.", L"无法读取压缩包元数据。"},
        {"The archive contains a special file.", L"压缩包中包含不支持解压的特殊文件。"},
        {"Cannot finish extraction.", L"无法完成解压。"},
        {"Cannot finish archive reading.", L"无法完成压缩包读取。"},
        {"Choose at least one file or folder.", L"请至少选择一个文件或文件夹。"},
        {"The archive name must be a simple file name.", L"压缩包名称必须是有效的文件名，不能包含路径。"},
        {"Choose a compression level from 0 to 9.", L"压缩级别必须为 0 到 9。"},
        {"Choose a supported archive format.", L"请选择受支持的压缩格式。"},
        {"The password must be at most 1,024 bytes and contain no line breaks.",
         L"密码的 UTF-8 编码长度不能超过 1,024 字节，且不能包含换行符或空字符。"},
        {"Passwords and split volumes use ZIP format.", L"密码加密和分卷压缩仅支持 ZIP 格式。"},
        {"Cannot resolve the destination folder.", L"无法确定目标文件夹的实际路径。"},
        {"A selected item no longer exists.", L"所选的文件或文件夹已不存在。"},
        {"A selected file or folder no longer exists.", L"所选的文件或文件夹已不存在。"},
        {"Save the archive outside the folder being compressed.", L"保存位置不能位于待压缩文件夹内，请选择其他位置。"},
        {"Selected items must have distinct names.", L"所选文件和文件夹不能重名。"},
        {"Cannot create an archive writer.", L"无法初始化压缩包写入器。"},
        {"Cannot configure ZIP output.", L"无法初始化 ZIP 输出。"},
        {"Cannot configure Unicode ZIP filenames.", L"无法设置 ZIP 文件名的 Unicode 编码。"},
        {"Cannot configure ZIP compression.", L"无法设置 ZIP 压缩参数。"},
        {"Cannot configure ZIP encryption.", L"无法设置 ZIP 加密参数。"},
        {"Cannot configure 7Z output.", L"无法初始化 7Z 输出。"},
        {"Cannot configure 7Z compression.", L"无法设置 7Z 压缩参数。"},
        {"Cannot configure TAR output.", L"无法初始化 TAR 输出。"},
        {"Cannot configure gzip.", L"无法初始化 gzip 压缩。"},
        {"Cannot configure bzip2.", L"无法初始化 bzip2 压缩。"},
        {"Cannot configure xz.", L"无法初始化 xz 压缩。"},
        {"Cannot configure zstd.", L"无法初始化 zstd 压缩。"},
        {"Cannot open temporary archive output.", L"无法打开压缩包临时输出文件。"},
        {"Cannot inspect a selected item.", L"无法读取所选项目的信息。"},
        {"Cannot create an archive entry.", L"无法创建压缩包条目。"},
        {"Cannot read the file modification time.", L"无法读取文件的修改时间。"},
        {"Cannot write a directory entry.", L"无法写入文件夹条目。"},
        {"Cannot finish a directory entry.", L"无法完成文件夹条目的写入。"},
        {"Cannot enumerate a selected folder.", L"无法列出所选文件夹中的内容。"},
        {"Unsafe symbolic links are not archived.", L"无法压缩不安全的符号链接。"},
        {"Cannot write a symbolic link.", L"无法写入符号链接。"},
        {"Cannot finish a symbolic link.", L"无法完成符号链接的写入。"},
        {"Cannot stat a selected file.", L"无法读取所选文件的状态信息。"},
        {"Cannot write a file entry.", L"无法写入文件条目。"},
        {"Cannot read a selected file.", L"无法读取所选文件。"},
        {"The selected file changed or could not be read completely.", L"所选文件在读取期间发生变化或未能完整读取。"},
        {"Cannot write file data.", L"无法写入文件数据。"},
        {"Cannot finish a file entry.", L"无法完成文件条目的写入。"},
        {"Special files cannot be added to an archive.", L"无法将特殊文件添加到压缩包。"},
        {"Cannot finish archive output.", L"无法完成压缩包写入。"},
        {"Cannot commit archive output.", L"无法将压缩包保存到目标位置。"},
        {"Cannot read temporary archive output.", L"无法读取压缩包临时输出文件。"},
        {"Cannot create a split archive volume.", L"无法创建压缩包分卷。"},
        {"Cannot write a split archive volume.", L"无法写入压缩包分卷。"},
        {"Cannot finish a split archive volume.", L"无法完成压缩包分卷的写入。"},
        {"Cannot commit split archive output.", L"无法将压缩包分卷保存到目标位置。"},
        {"Unrecognized archive format", L"无法识别压缩包格式。"},
        {"Passphrase required for this entry", L"此项目已加密，请输入密码。"},
        {"Incorrect passphrase", L"密码不正确。"},
    };
    for (const auto& [source, translation] : translations) {
        if (message == source)
            return translation;
    }
    if (code) {
        switch (*code) {
        case ArchiveFailure::Code::password_required:
            return L"无法解密压缩包，请检查密码是否已填写且正确。";
        case ArchiveFailure::Code::unsupported_format:
            return L"当前不支持此压缩格式或压缩选项。";
        case ArchiveFailure::Code::unsafe_path:
            return L"检测到不安全的路径、链接或特殊文件，操作已停止。";
        case ArchiveFailure::Code::invalid_input:
            return L"所选项目或压缩参数无效。";
        case ArchiveFailure::Code::cancelled:
            return L"已取消。";
        case ArchiveFailure::Code::io:
            return L"无法完成文件读写操作。";
        }
    }
    return L"操作未完成。";
}

} // namespace unfurl::localization
