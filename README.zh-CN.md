# Unfurl 简体中文版

Unfurl 是一款适用于 Windows 11 的原生压缩工具，支持预览压缩包内容、解压，以及将文件和文件夹创建为压缩包。

## 使用

- 在“压缩”页添加文件或文件夹，设置压缩包名称、格式及保存位置，然后选择“压缩”。
- 在“解压”页打开压缩包，确认保存位置，然后选择“解压”。加密压缩包需要填写密码。
- “更多选项”包含密码和分卷大小。创建压缩包时，密码加密与分卷仅支持 ZIP 格式。
- 分卷大小使用 MiB：1 MiB = 1,048,576 字节。留空表示不分卷。
- “外观”可以选择跟随系统、浅色或深色。Mica 材质会遵循 Windows 的透明效果设置。
- 输出完成后，可通过底部文件夹按钮在文件资源管理器中显示结果。已有文件不会被自动覆盖，重名结果会使用新的编号名称。

支持创建 ZIP、7Z、TAR.GZ、TAR.BZ2、TAR.XZ 和 TAR.ZST。ZIP 分卷名称为 `.zip.001`、`.zip.002` 等；解压时选择第一个分卷，并将所有分卷放在同一文件夹中。

## 运行与集成

自动更新版从 [GitHub Releases](https://github.com/L0stInFades/Unfurl/releases/latest) 下载 `Unfurl.cer` 和 `Unfurl.appinstaller`。首版使用自签名证书，首次安装前需按[安装说明](docs/updates.md#install)核对证书指纹并将公开证书导入“本地计算机 / 受信任人”。然后双击 `Unfurl.appinstaller`，由 Windows 安装应用和微软运行时依赖。

通过 `.appinstaller` 安装后，Windows 会在启动时检查更新，并每 8 小时执行后台检查；更新仅升版，不强制中断压缩或解压任务。需要手动更新时，可再次下载并打开最新的 `.appinstaller` 文件。单独安装 `.msix` 或使用便携 ZIP 不会加入自动更新通道。自动更新依赖 Windows App Installer 和对 GitHub 下载地址的网络访问。

解压整个发行包后运行 `Unfurl.exe`。需要 Windows App SDK 2.4 运行时；若发行包包含 `windows-app-runtime` 文件夹，可使用 PowerShell 7 运行 `installer/install-runtime.ps1` 安装。

资源管理器右键菜单为可选功能。使用 PowerShell 7 运行：

```powershell
./installer/install-shell.ps1 -Executable "C:\实际安装路径\Unfurl.exe"
```

菜单项位于 Windows 11 的“显示更多选项”中。运行 `installer/uninstall-shell.ps1` 可移除这些菜单项。仅打开程序不会写入这些注册表项。

## 翻译约定

| 原文 | 中文 | 含义 |
| --- | --- | --- |
| Archive | 压缩包 | 在本工具界面中泛指支持的归档文件，包括 TAR。 |
| Extract | 解压 | 将压缩包中的内容还原到目标文件夹。 |
| Entry / Item | 项目、项 | 计数同时包含文件和文件夹，不译为“文件数”。 |
| Volume / Split | 分卷 | 指压缩包分卷，不是磁盘卷。 |
| Destination | 保存位置 | 输出文件或解压结果所在的位置。 |
| Password | 密码 | 创建时用于加密，解压时用于解密。 |
| MiB | MiB | 二进制容量单位，不与十进制 MB 混用。 |

界面、状态提示、文件选择器标题和应用自有错误信息均使用简体中文。错误状态的悬浮提示保留底层诊断原文；无法逐字可靠翻译的第三方错误按错误类别显示中文说明，原文仍可用于排查。文件名、路径、格式名称及品牌名称保持原样。系统文件对话框中的系统按钮遵循 Windows 的显示语言。

## 交互依据

界面使用 WinUI 的 TitleBar、NavigationView 和 Expander 控件。标题栏与侧栏共享 Mica 背景，窗口按钮由 Windows 绘制。

“更多选项”保留自适应高度。展开后使用 `StartBringIntoView` 请求平滑滚动；收起时临时保留原滚动范围，用 `ChangeView` 平滑回到有效位置后释放，避免内容变短时滚动位置突然跳变。动画遵循 Windows 的动画效果设置。

- [微软 Expander 指南](https://learn.microsoft.com/en-us/windows/apps/develop/ui/controls/expander)
- [StartBringIntoView](https://learn.microsoft.com/en-us/windows/windows-app-sdk/api/winrt/microsoft.ui.xaml.uielement.startbringintoview)
- [ScrollViewer.ChangeView](https://learn.microsoft.com/en-us/windows/windows-app-sdk/api/winrt/microsoft.ui.xaml.controls.scrollviewer.changeview)
- [Mica 材质指南](https://learn.microsoft.com/en-us/windows/apps/design/style/mica)

测试环境与覆盖范围见 [验证记录](docs/verification.md)。

也可以只下载 [UnfurlSetup.exe](https://github.com/L0stInFades/Unfurl/releases/latest/download/UnfurlSetup.exe)，正常双击运行。这个独立的 WinUI 3 安装器内嵌公开证书和更新配置，会准备缺少的微软运行组件，仅在信任证书时请求管理员确认，最后打开 Windows 安装窗口，由你确认安装并加入自动更新通道。无需下载或摆放其他文件。
