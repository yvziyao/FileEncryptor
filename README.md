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

## 💻 运行环境

### 图形界面版（GUI）

| 项目 | 说明 |
|---|---|
| 系统 | **Windows 10 / 11（x64）** 为推荐与测试环境 |
| 架构 | 仅 x64（32 位版需要另行编译 OpenSSL x86 库） |
| 运行库 | **无需安装**——静态链接 CRT 与 OpenSSL，单 EXE 无外部 DLL 依赖 |
| DPI | 已声明系统级 DPI 感知，125% / 150% 等缩放下界面清晰不模糊 |
| 权限 | `asInvoker`，不需要管理员权限 |

**更早的系统**：程序 PE 头的最低版本为 6.00（Vista），导入表中没有 Win8/Win10 专有 API，
因此在 Windows 7 SP1 上**理论上可以启动**；但以下效果只在较新系统上生效：

| 效果 | 最低要求 |
|---|---|
| 窗口圆角 | Windows 11 |
| 深色标题栏 | Windows 10 1809（build 17763） |
| 自定义标题栏配色 | Windows 11 |
| 深浅色跟随系统 | Windows 10 |

在不支持的系统上这些调用会被忽略（不会报错），界面自动退化为方角 + 系统默认标题栏。
**实际只在 Windows 10 / 11 上做过完整验证**，更早的系统未测试。

> 说明：程序未嵌入 `Microsoft.Windows.Common-Controls 6.0` 清单，标准控件按经典样式渲染。
> 由于界面控件全部为自绘（并主动调用了 `SetWindowTheme(h, L"", L"")` 关闭主题），
> 视觉上不受影响；`SetWindowSubclass` 等 API 通过 comctl32 v5 的序号导出解析。

### 命令行版（CLI）

| 项目 | 说明 |
|---|---|
| 系统 | Windows / Linux / macOS |
| 架构 | x64（源码与架构无关，32 位同样可编译） |
| 运行库 | Windows 下静态链接，单文件无外部依赖；Linux/macOS 需系统 OpenSSL（`libssl` / `libcrypto`）或静态链接 |
| 中文路径 | 已适配：Windows 下内部统一 UTF-8 并显式转为 UTF-16 调用文件 API，任何区域设置下均可处理中文路径 |

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
cl /EHsc /std:c++17 /MT /O2 /GL /DNOMINMAX /utf-8 ^
   /I"C:\Program Files\OpenSSL-Win64\include" ^
   enc.cpp /Fe"enc.exe" ^
   /link /LTCG ^
   /LIBPATH:"C:\Program Files\OpenSSL-Win64\lib\VC\x64\MT" ^
   libssl_static.lib libcrypto_static.lib ws2_32.lib gdi32.lib crypt32.lib advapi32.lib user32.lib
```

> `/utf-8` 建议保留：源码是 UTF-8（带 BOM），源文件本身已能保证被正确识别，
> 但 MSVC 的“执行字符集”默认跟随系统 ANSI 代码页，加上 `/utf-8` 才能让中文
> 提示信息在任何区域设置下都固定为 UTF-8。
> 源码内也用 `#pragma execution_character_set("utf-8")` 做了兜底，漏加该参数也不会乱码。

**Linux (g++)**

```bash
g++ -std=c++17 enc.cpp -o enc -lssl -lcrypto
# GCC 8 及更早版本需要额外链接 filesystem：
# g++ -std=c++17 enc.cpp -o enc -lssl -lcrypto -lstdc++fs
```

**macOS (clang++)**

```bash
brew install openssl
clang++ -std=c++17 enc.cpp -o enc \
  -I"$(brew --prefix openssl)/include" -L"$(brew --prefix openssl)/lib" -lssl -lcrypto
```

> **注意**：Windows 专有库（`ws2_32` / `gdi32` / `crypt32` / `advapi32` / `user32`）**只用于 Windows 构建**，
> 在 Linux / macOS 上不存在，不要把上面的 MSVC 参数照搬到 g++。
> 也不要轻易加 `-static`：多数发行版默认不提供 OpenSSL 静态库，会直接链接失败。

> **依赖**：编译需要 OpenSSL 开发库（1.1.0 及以上）。  
> - **Ubuntu/Debian**：`sudo apt install libssl-dev`  
> - **Windows (MSYS2)**：`pacman -S mingw-w64-x86_64-openssl`  
> - **macOS**：`brew install openssl`

### 中文与路径编码

* 源码为 **UTF-8（带 BOM）**，在 ANSI 代码页不是 UTF-8 的机器（例如默认的简体中文 Windows，代码页 936）上同样能正确编译
* Windows 下程序内部统一使用 **UTF-8** 表示路径，调用文件 API 前显式转换为 UTF-16，
  因此**中文文件名、中文输出目录在任何区域设置下都能正常工作**
* 启动时会把控制台输出代码页临时切到 UTF-8（退出时还原），保证中文提示不会显示为乱码

### 使用方法

```bash
# 交互式加密（会提示输入密码并确认）
enc -e -i 文件路径

# 命令行直接提供密码（加密，会二次确认）
enc -e -i 文件路径 -p 你的密码

# 指定加密强度（fast | standard | secure | extreme，默认 standard）
enc -e -i 文件路径 -p 你的密码 -s extreme

# 输出到指定目录（加密后在 D:\Encrypted 下生成 文件路径.enc）
enc -e -i 文件路径 -p 你的密码 -o D:\Encrypted

# 保留源文件（加密后不删除原文件）
enc -e -i 文件路径 -p 你的密码 -k

# 交互式解密
enc -d -i 文件路径.enc

# 命令行直接提供密码（解密，适合批量）
enc -d -i 文件路径.enc -p 你的密码

# 解密到指定目录，并保留加密文件
enc -d -i 文件路径.enc -p 你的密码 -o D:\Decrypted -k
```

### 参数说明

| 参数 | 说明 |
|---|---|
| `-e` / `-d` | 加密 / 解密模式（必填其一） |
| `-i <文件>` | 输入文件路径（必填） |
| `-o <目录>` | 输出目录，默认为输入文件所在目录。目录必须已存在 |
| `-p <密码>` | 密码；不提供则交互式输入（输入时不回显） |
| `-s <强度>` | `fast` / `standard` / `secure` / `extreme`，默认 `standard` |
| `-w <次数>` | 删除前覆写次数：`0` = 直接删除，`1` / `3` = 覆写次数，默认 `3` |
| `-k` / `--keep` | 保留源文件（加密时）/ 加密文件（解密时），不做任何删除 |
| `-h` / `--help` | 显示帮助 |

> 输出文件名自动推导：加密在原名后追加 `.enc`，解密去掉 `.enc`。
>
> 与 GUI 版一致，**解密时加密文件始终直接删除、不做覆写**（密文不含明文残留）；`-w` 只影响加密时源文件的删除方式。

### 批量处理示例

```bat
:: Windows 批处理 (CMD)：批量解密到指定目录
for %f in (*.enc) do enc -d -i "%f" -p mypass -o D:\Decrypted
```

```bash
# Linux / macOS Shell：批量加密并保留源文件
for f in *.txt; do ./enc -e -i "$f" -p mypass -o ./encrypted -k; done
```

---

## 🔒 安全说明

* 命令行版本与图形界面版本使用**完全相同的加密算法与文件格式**（AES-256-GCM + PBKDF2），加密后的文件可以在两个版本之间**互相解密**
* **删除策略**（GUI 在设置中调整，CLI 用 `-k` / `-w` 控制）
  * 加密：源文件含明文，覆写 1 次或 3 次后删除；选择「禁用」或加 `-k` 则**保留源文件**
  * 解密：加密文件本身是密文、不含明文残留，因此**不做覆写**，直接删除；选择「禁用」或加 `-k` 则**保留**
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
