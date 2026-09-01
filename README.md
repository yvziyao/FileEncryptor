# FileEncryptor

跨平台（Windows）文件加密工具（本仓库为 Win32 应用）

功能
- 使用 AES-256-CBC 与 PBKDF2-HMAC-SHA256 派生密钥
- 流式分块加/解密以支持大文件
- 加密/解密对话支持中文/英文切换
- 密码对话使用现代样式（Direct2D 绘制圆角输入框与按钮）
- 安全删除：对原始文件进行一次随机覆盖并删除（便携、无备份）
- 自动构建：提供 GitHub Actions CI 配置（Windows/MSBuild）

快速开始
1. 在 Windows 上打开 Visual Studio（建议 2022/2025/2026）并加载此项目（FileEncryptor.vcxproj）。
2. 确认你的系统已安装 OpenSSL 开发包（包含头文件与 lib）。项目目前在 Release/x64 配置下使用静态 OpenSSL 库（libcrypto_static.lib / libssl_static.lib）。
   - 如果使用系统 OpenSSL，请在项目属性中设置：附加包含目录与附加库目录，或修改 .vcxproj 中对应路径。
3. 选择 Release | x64，构建（Rebuild）。
4. 运行生成的可执行文件（单个 EXE，程序会在需要时以自身作为子进程弹出密码对话以保证便携性）。

命令行（开发）
- 使用 msbuild: `msbuild FileEncryptor.vcxproj /p:Configuration=Release /p:Platform=x64`

CI（GitHub Actions）
- 本仓库包含一个示例 GitHub Actions 工作流（.github/workflows/ci.yml），在 Windows Runner 上执行 MSBuild 来构建项目。
- 注意：CI 需要可用的 OpenSSL 开发库或使用 vcpkg 在 runner 上安装依赖。根据你的环境，可能需要调整工作流来自动安装 OpenSSL（或使用 vcpkg 的 `openssl:x64-windows`）。

便携性与兼容性
- 便携：Release 配置使用静态 CRT (MT) 以减少运行时依赖，程序设计为单 EXE（密码对话通过子进程自调用实现），无需附带额外 DLL。仍然需注意目标主机必须具备 Windows API 支持（Windows 10/11 推荐）。
- 兼容性：建议在 Windows 10 (1809+) / Windows 11 上运行。Windows 7/8.1 未经充分测试，不保证可用性。

安全说明
- 程序会在加密/解密后对原始文件执行一次随机数据覆盖并删除（尽最大努力减少残留），但对于特殊存储介质或文件系统，不能保证 100% 无痕（例如 SSD、云同步等）。

自动推送到 GitHub
- 我可以将此仓库的改动提交并使用 `gh` CLI 将其推送到你的 GitHub。请在确认要推送时提供目标仓库：`OWNER/REPO`（或说明是否创建新仓库）。

许可证
- 本项目基于用户提供代码恢复并修改，未指定单一开源许可证。请在发布前补充许可信息。

