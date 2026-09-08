# FileEncryptor

> 🔐 轻量级文件加密工具 | Windows GUI + 跨平台 CLI | 基于 AES-256-CBC
> 跨平台文件加密工具，提供图形界面 (Win32) 和命令行 (CLI) 两种版本。

基于 AES-256-CBC 与 PBKDF2-HMAC-SHA256 的流式文件加解密工具。支持大文件分块处理，并提供简洁安全的操作方式。

## ✨ 功能特性 (通用)

* **强加密算法**：AES-256-CBC + PBKDF2-HMAC-SHA256 密钥派生
* **大文件支持**：流式分块加/解密，内存占用低
* **安全删除**：加密/解密后对原始文件进行一次随机覆盖并删除（尽力减少残留）
* **跨平台**：命令行版本支持 Windows / Linux / macOS

---

## 🖥️ 图形界面版本 (GUI)

适用于 Windows 10/11，提供简洁的图形操作界面。

### 特点

* **双语界面**：加密/解密对话框支持中文/英文切换
* **现代 UI**：基于 Direct2D 绘制的圆角输入框与按钮
* **完全便携**：单 EXE 文件，无需额外 DLL

### 下载与使用

1. 从 [Releases](https://github.com/yvziyao/FileEncryptor/releases) 页面下载 `FileEncryptor.exe`
2. 双击运行，点击「加密」或「解密」按钮
3. 选择目标文件，输入密码即可

---

## ⌨️ 命令行版本 (CLI)

适用于 Windows / Linux / macOS，适合服务器、远程环境或自动化脚本。

### 特点

* **极简命令行**：轻量无依赖，适合批量处理
* **隐藏密码输入**：交互式输入时不回显字符，防止窥屏
* **支持 -p 参数**：可直接在命令行传递密码，方便脚本集成

### 编译

```bash
# Linux / macOS / Windows (MSYS2 / Cygwin / WSL)
g++ -std=c++17 enc.cpp -o enc -static -lssl -lcrypto -lws2\_32 -lgdi32 -lcrypt32
```

> **依赖**：编译需要 OpenSSL 开发库。  
> - **Ubuntu/Debian**：`sudo apt install libssl-dev`  
> - **Windows (MSYS2)**：`pacman -S mingw-w64-x86\_64-openssl`  
> - **macOS**：`brew install openssl`（可能需要指定 include 和 lib 路径）

### 使用方法

```bash
# 交互式加密（会提示输入密码并确认）
enc -e -i 文件路径

# 命令行直接提供密码（加密，会二次确认）
enc -e -i 文件路径 -p 你的密码

# 交互式解密
enc -d -i 文件路径.enc

# 命令行直接提供密码（解密，适合批量）
enc -d -i 文件路径.enc -p 你的密码
```

### 批量处理示例

```bash
# Windows 批处理 (CMD)
for %f in (\*.enc) do enc -d -i "%f" -p mypass

# Linux / macOS Shell
for f in \*.enc; do ./enc -d -i "$f" -p mypass; done
```

### 🔒安全说明

命令行版本与图形界面版本使用**完全相同的加密算法**（AES-256-CBC + PBKDF2），加密后的文件可以在两个版本之间**互相解密**，无需担心兼容性问题。

* 加密/解密完成后，程序会对原始文件进行随机数据覆盖并删除
* 对于 SSD、云同步文件夹等特殊存储介质，无法保证 100% 不可恢复
* 建议在本地、非同步目录中使用，并自行评估安全风险
* **请妥善保管密码，丢失后将无法解密**

---

## 🔏 代码签名

本项目的 Release 版本使用 [SignPath Foundation](https://signpath.org/) 提供的免费代码签名服务，由 [SignPath.io](https://about.signpath.io/) 颁发证书。

[![Signed by SignPath.io](https://about.signpath.io/images/badges/signed-by-signpath-badge.svg)](https://about.signpath.io/)

## 📄 许可证

本项目采用 MIT License 开源。

## 🤝 贡献

欢迎提交 Issue 与 Pull Request。

