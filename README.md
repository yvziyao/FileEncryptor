# FileEncryptor

> 🔐 轻量级文件加密工具 | Windows GUI + 跨平台 CLI | 基于 AES-256-GCM
> 跨平台文件加密工具，提供图形界面 (Win32) 和命令行 (CLI) 两种版本。

基于 AES-256-GCM 与 PBKDF2-HMAC-SHA256 的流式文件加解密工具。支持大文件分块处理，并提供简洁安全的操作方式。

## ✨ 功能特性 (通用)

* **认证加密**：AES-256-GCM + PBKDF2-HMAC-SHA256 密钥派生，密文自带认证标签，密码错误或文件被篡改都能被可靠检出
* **大文件支持**：流式分块加/解密，内存占用低
* **可选删除策略**：加密后按设置保留或覆写删除源文件；解密后可直接删除加密文件
* **跨平台**：命令行版本支持 Windows / Linux / macOS
* **格式互通**：GUI 与 CLI 使用完全相同的文件格式，可互相解密

---

## 🔐 加密算法与文件格式

| 项目 | 说明 |
|---|---|
| 加密算法 | AES-256-GCM（认证加密 / AEAD） |
| 密钥派生 | PBKDF2-HMAC-SHA256，迭代次数可选 |
| 加密强度 | 快速 50,000 / 标准 100,000（默认）/ 安全 300,000 / 极强 600,000 |
| 随机数 | OpenSSL `RAND_bytes`（salt 16 字节，IV 12 字节） |

文件格式（v1）：

```
偏移 0    魔数[8]       "FENC\r\n\x1a\n"   用于与旧格式区分
偏移 8    版本[1]       = 1
偏移 9    保留[1]       = 0
偏移 10   迭代次数[4]   PBKDF2 迭代次数（小端）
偏移 14   salt[16]
偏移 30   IV[12]        GCM 推荐 96-bit nonce
偏移 42   密文[N]       与明文等长（GCM 为流模式，无填充）
末尾 16   认证标签[16]
```

> 迭代次数写在文件头里，**解密时自动读取**，因此调高加密强度不会影响旧文件的解密。

**向后兼容**：旧的 AES-256-CBC 格式（`salt[16] + IV[16] + 密文`，迭代 100,000）仍可正常解密，此前加密的文件不会失效。程序通过文件开头的魔数自动识别格式。

---

## 🖥️ 图形界面版本 (GUI)

适用于 Windows 10/11，提供简洁的图形操作界面。

### 特点

* **设置面板**（右上角齿轮按钮）
  * 语言：中文 / English
  * 主题：跟随系统 / 浅色 / 深色
  * 输出文件位置：源文件同目录 / 指定目录 / 每次都询问
  * 安全删除源文件：禁用（保留原文件）/ 覆写 1 次 / 覆写 3 次
  * 完成后：关闭窗口 / 弹窗提示
  * 加密强度：快速 / 标准 / 安全 / 极强
* **现代 UI**：WinUI 3 风格自绘界面，圆角控件、悬停与进度动画、深色模式
* **密码强度提示**：实时彩色分级（很弱 / 一般 / 良好 / 很强）
* **完全便携**：单 EXE 文件，无需额外 DLL

> 设置保存在注册表 `HKCU\Software\FileEncryptor`。

### 下载与使用

1. 从 [Releases](https://github.com/yvziyao/FileEncryptor/releases) 页面下载 `FileEncryptor.exe`
2. 双击运行，点击「加密」或「解密」按钮
3. 选择目标文件，输入密码即可（也可以直接把文件拖进窗口）

---

## ⌨️ 命令行版本 (CLI)

适用于 Windows / Linux / macOS，适合服务器、远程环境或自动化脚本。

### 编译

**Windows (MSVC，静态链接 OpenSSL)**

```bat
cl /EHsc /std:c++17 /MT /O2 /GL /DNOMINMAX ^
   /I"C:\Program Files\OpenSSL-Win64\include" ^
   enc.cpp /Fe"enc.exe" ^
   /link /LTCG ^
   /LIBPATH:"C:\Program Files\OpenSSL-Win64\lib\VC\x64\MT" ^
   libssl_static.lib libcrypto_static.lib ws2_32.lib gdi32.lib crypt32.lib advapi32.lib user32.lib
```

**Linux / macOS (g++)**

```bash
g++ -std=c++17 enc.cpp -o enc -static -lssl -lcrypto -lws2_32 -lgdi32 -lcrypt32
```

> **依赖**：编译需要 OpenSSL 开发库。  
> - **Ubuntu/Debian**：`sudo apt install libssl-dev`  
> - **Windows (MSYS2)**：`pacman -S mingw-w64-x86_64-openssl`  
> - **macOS**：`brew install openssl`（可能需要指定 include 和 lib 路径）

### 使用方法

```bash
# 交互式加密（会提示输入密码并确认）
enc -e -i 文件路径

# 命令行直接提供密码（加密，会二次确认）
enc -e -i 文件路径 -p 你的密码

# 指定加密强度（fast | standard | secure | extreme，默认 standard）
enc -e -i 文件路径 -p 你的密码 -s extreme

# 交互式解密
enc -d -i 文件路径.enc

# 命令行直接提供密码（解密，适合批量）
enc -d -i 文件路径.enc -p 你的密码
```

### 批量处理示例

```bat
:: Windows 批处理 (CMD)
for %f in (*.enc) do enc -d -i "%f" -p mypass
```

```bash
# Linux / macOS Shell
for f in *.enc; do ./enc -d -i "$f" -p mypass; done
```

---

## 🔒 安全说明

* 命令行版本与图形界面版本使用**完全相同的加密算法与文件格式**（AES-256-GCM + PBKDF2），加密后的文件可以在两个版本之间**互相解密**
* **删除策略**（GUI 可在设置中调整；CLI 固定为覆写 3 次）
  * 加密：源文件含明文，按设置覆写 1 次或 3 次后删除；选择「禁用」则**保留源文件**
  * 解密：加密文件本身是密文、不含明文残留，因此**不做覆写**，直接删除；选择「禁用」则**保留**
* 对于 SSD、云同步文件夹等特殊存储介质，覆写无法保证 100% 不可恢复
* 建议在本地、非同步目录中使用，并自行评估安全风险
* **请妥善保管密码，丢失后将无法解密**（AES-256-GCM 无后门、无恢复机制）

---

## 🔏 代码签名

本项目的 Release 版本使用 [SignPath Foundation](https://signpath.org/) 提供的免费代码签名服务，由 [SignPath.io](https://about.signpath.io/) 颁发证书。

[![Signed by SignPath.io](https://about.signpath.io/images/badges/signed-by-signpath-badge.svg)](https://about.signpath.io/)

## 📄 许可证

本项目采用 MIT License 开源。

## 🤝 贡献

欢迎提交 Issue 与 Pull Request。
