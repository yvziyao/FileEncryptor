```markdown
# FileEncryptor

> 跨平台（Windows）文件加密工具，基于 Win32 原生实现。

基于 AES-256-CBC 与 PBKDF2-HMAC-SHA256 的流式文件加解密工具。支持大文件分块处理，并提供简洁安全的图形界面。

## 功能特性

- **强加密算法**：AES-256-CBC + PBKDF2-HMAC-SHA256 密钥派生
- **大文件支持**：流式分块加/解密，内存占用低
- **双语界面**：加密/解密对话框支持中文/英文切换
- **现代 UI**：基于 Direct2D 绘制的圆角输入框与按钮
- **安全删除**：加密/解密后对原始文件进行一次随机覆盖并删除（尽力减少残留）
- **完全便携**：单 EXE 文件，密码对话框通过子进程自调用，无需额外 DLL
- **自动构建**：提供 GitHub Actions CI 配置

## 系统要求

- Windows 10 / 11（推荐 1809+）
- Windows 7 / 8.1 未经充分测试，不保证完全兼容

## 快速开始

### 下载与使用（普通用户）

1.  **下载程序**：前往本仓库的 **[Releases](https://github.com/yvziyao/FileEncryptor/releases)** 页面，下载最新版本的 `FileEncryptor.exe` 文件。
2.  **运行程序**：双击下载的 `FileEncryptor.exe` 即可启动图形界面。软件为单文件，无需安装，无需额外依赖。
3.  **加/解密文件**：
    - **加密**：点击“加密”按钮，选择要加密的文件并设置密码。
    - **解密**：点击“解密”按钮，选择加密后的文件（通常为 `.enc` 后缀）并输入正确的密码。

### 从源代码构建（开发者）

#### 使用 Visual Studio 构建

1.  使用 Visual Studio 2022（或更高版本）打开项目文件 `FileEncryptor.vcxproj`
2.  确保系统已安装 OpenSSL 开发包（包含头文件与库文件）
3.  项目默认使用静态 OpenSSL（`libcrypto_static.lib` / `libssl_static.lib`），若使用系统 OpenSSL，请调整项目属性中的包含目录与库目录
4.  选择 `Release | x64` 配置，执行重新生成
5.  运行生成的可执行文件

#### 命令行构建

```shell
msbuild FileEncryptor.vcxproj /p:Configuration=Release /p:Platform=x64
```

#### GitHub Actions 自动构建

仓库已包含 `.github/workflows/ci.yml` 工作流，可在 GitHub Actions 上自动完成 MSBuild 构建。

> 注意：CI 环境需要安装 OpenSSL 开发库，可根据实际情况调整工作流使用 `vcpkg` 安装依赖。

## 安全说明

- 加密/解密完成后，程序会对原始文件进行一次随机数据覆盖并删除，以尽可能减少数据残留
- 对于 SSD、云同步文件夹等特殊存储介质，无法保证 100% 不可恢复
- 建议在本地、非同步目录中使用，并自行评估安全风险

## 许可证

本项目采用 MIT License 开源。

## 贡献

欢迎提交 Issue 与 Pull Request。