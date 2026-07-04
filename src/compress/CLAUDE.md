[根目录](../../CLAUDE.md) > [src](../) > **compress**

# compress -- 压缩模块

## 模块职责

提供完整的文件压缩功能：格式选择 UI（Libadwaita 对话框）、后端命令构建与异步执行（7z/tar/zstd）、进度解析。支持 9 种压缩格式、密码加密、分卷压缩、批量压缩。

## 入口与启动

- `compress_dialog_show()` -- 独立窗口模式（未使用）
- `compress_dialog_show_in_container()` -- 嵌入模式，由 `toolkit_window.c` 调用，嵌入到统一窗口的「压缩」标签页

## 对外接口

### compress-dialog.h -- 压缩对话框
- `CompressDialog` -- GObject 类型（继承 GtkBox），包含完整的设置/进度 UI
- `compress_dialog_show()` -- 独立窗口展示
- `compress_dialog_show_in_container()` -- 嵌入到外部容器，隐藏自身 header bar

### compress-backend.h -- 压缩后端
- `CompressFormat` 枚举 -- 7z, 7z(分卷), ZIP, ZIP(分卷), CBZ, TAR, TAR.XZ, TAR.ZST, WIM
- `CompressTask` -- 压缩任务描述（格式、源文件、输出路径、压缩等级、密码、分卷大小）
- `compress_backend_run_async()` -- 异步执行压缩，通过 GTask 在线程池运行
- `compress_tools_detect()` -- 检测系统可用工具（7z/tar/zstd）
- `compress_format_available()` -- 判断格式是否可用
- `compress_resolve_output_path()` -- 输出路径冲突解决（追加 (1), (2) 等）

### compress-progress.h -- 进度解析
- `progress_parse_7z_line()` -- 解析 7z `-bsp2` 输出的百分比和文件名
- `progress_parse_tar_line()` -- 解析 tar `-v` 输出，按行计数
- `progress_count_files()` -- 递归统计文件数（tar 进度分母）

## 关键依赖与配置

- 7z（或 7zz）-- 用于 7z/ZIP/CBZ/WIM 格式
- tar -- 用于 TAR/TAR.XZ/TAR.ZST 格式
- zstd -- 用于 TAR.ZST 格式
- json-glib -- 压缩对话框中密码库读取（与 core/pwdlib.c 的手写解析器独立）

## 数据模型

- `CompressFormat` -- 9 种格式枚举
- `CompressTask` -- 压缩任务参数结构体
- `ToolAvailability` -- 工具检测结果（has_7z, has_tar, has_zstd 及路径）
- `LevelDef` -- 每种格式的压缩等级定义（7z 7档, ZIP 6档, XZ 5档, ZST 5档）
- `CompressDialog` -- GObject 实例，持有全部 UI widget 引用和运行状态

## 测试与质量

无自动化测试。

## 常见问题 (FAQ)

**Q: 压缩和解压的进程管理方式为何不同？**
A: 压缩使用 GSubprocess（GLib 高层 API），因为不需要密码 pipe 传递的精细控制。7z 密码通过 stdin pipe 传递（`send_7z_password()`），tar 不支持加密。

**Q: 批量压缩如何工作？**
A: 多选文件时可开启「分别压缩」模式，为每个文件/文件夹创建独立 `CompressTask`，串行执行，聚合进度显示。

**Q: 分卷压缩的格式选择逻辑？**
A: UI 中不直接暴露分卷格式。用户选择 7z/ZIP 后填写分卷大小，`on_create_clicked()` 自动将 `FORMAT_7Z` 切换为 `FORMAT_7Z_SPLIT`。

## 相关文件清单

| 文件 | 行数(约) | 职责 |
|------|----------|------|
| `compress-dialog.c` | ~976 | 压缩对话框 UI（Libadwaita）、密码库加载、格式切换、批量压缩 |
| `compress-backend.c` | ~743 | 命令构建（7z/tar）、GSubprocess 异步执行、进度读取、失败清理 |
| `compress-progress.c` | ~125 | 7z/tar 输出进度解析、文件计数 |

## 变更记录 (Changelog)

- 2026-02-25: 初始化模块文档
