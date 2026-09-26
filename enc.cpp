// enc.cpp
// 极简命令行文件加密工具（加密/解密、指定输出目录、可控安全删除）
//
// 源码为 UTF-8（带 BOM），保证在任何 ANSI 代码页的机器上都能正确编译。
// MSVC 的“执行字符集”默认跟随系统 ANSI 代码页，这里强制成 UTF-8，
// 使字面量在可执行文件中固定为 UTF-8，与本程序统一使用 UTF-8 的内部编码一致。

#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define OPENSSL_API_COMPAT 0x10100000L

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/hmac.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <memory>
#include <filesystem>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <thread>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

// ---------- 跨平台路径与编码 ----------
// 内部一律用 UTF-8 的 std::string 表示路径。
// Windows 上必须显式做 UTF-8 -> UTF-16 转换再交给文件 API：否则标准库会按系统
// ANSI 代码页解释窄字符串，中文路径在非中文区域设置下会直接打不开。
// 非 Windows 平台本身就是 UTF-8，直接构造即可。
#ifdef _WIN32
static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// main() 拿到的 argv 是按系统 ANSI 代码页编码的，统一转成 UTF-8 再使用。
// 当系统代码页本身就是 65001 时，这个转换是恒等的。
static std::string ArgToUtf8(const char* s) {
    if (!s || !*s) return std::string();
    int wn = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (wn <= 0) return std::string(s);
    std::wstring w((size_t)wn, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], wn);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    int un = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (un <= 0) return std::string(s);
    std::string out((size_t)un, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], un, nullptr, nullptr);
    return out;
}

// 把控制台输出代码页切到 UTF-8（源码字面量是 UTF-8），退出时还原，
// 免得在默认中文 Windows（代码页 936）下中文显示成乱码。
struct ConsoleUtf8Scope {
    UINT old_cp = 0;
    ConsoleUtf8Scope() {
        old_cp = GetConsoleOutputCP();
        if (old_cp != 0 && old_cp != CP_UTF8) SetConsoleOutputCP(CP_UTF8);
    }
    ~ConsoleUtf8Scope() {
        if (old_cp != 0 && old_cp != CP_UTF8) SetConsoleOutputCP(old_cp);
    }
};
#else
static std::string ArgToUtf8(const char* s) { return std::string(s ? s : ""); }
#endif

static fs::path PathOf(const std::string& utf8_path) {
#ifdef _WIN32
    return fs::path(Utf8ToWide(utf8_path));
#else
    return fs::path(utf8_path);
#endif
}

// ---------- 密码输入（隐藏回显） ----------
// 如果 password 参数非空，直接使用；否则交互式输入
std::string get_password(const std::string& prompt, bool confirm, const std::string& preloaded = "") {
    // 如果已经通过 -p 提供了密码，直接返回，不再确认
    if (!preloaded.empty()) {
        return preloaded;
    }

    // 否则交互式输入（原有逻辑）
    std::string password;
    
#ifdef _WIN32
    std::cout << prompt;
    char ch;
    while ((ch = _getch()) != '\r') {
        if (ch == '\b') {
            if (!password.empty()) {
                password.pop_back();
                std::cout << "\b \b";
            }
        } else if (ch != 0 && ch != -32) {
            password.push_back(ch);
            std::cout << '*';
        }
    }
    std::cout << std::endl;
#else
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    std::cout << prompt;
    std::getline(std::cin, password);
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    std::cout << std::endl;
#endif

    if (confirm) {
        std::string confirm_pwd;
#ifdef _WIN32
        std::cout << "再次输入密码: ";
        char ch2;
        while ((ch2 = _getch()) != '\r') {
            if (ch2 == '\b') {
                if (!confirm_pwd.empty()) {
                    confirm_pwd.pop_back();
                    std::cout << "\b \b";
                }
            } else if (ch2 != 0 && ch2 != -32) {
                confirm_pwd.push_back(ch2);
                std::cout << '*';
            }
        }
        std::cout << std::endl;
#else
        struct termios oldt2, newt2;
        tcgetattr(STDIN_FILENO, &oldt2);
        newt2 = oldt2;
        newt2.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &newt2);
        std::cout << "再次输入密码: ";
        std::getline(std::cin, confirm_pwd);
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt2);
        std::cout << std::endl;
#endif
        if (password != confirm_pwd) {
            throw std::runtime_error("两次输入的密码不一致");
        }
    }
    
    return password;
}
// ---------- 工具函数 ----------

// 只支持当前格式（v2）。历史格式（v1 / 最旧的 CBC）已不再读取。
//
//   0  magic[8]      "FENC\r\n\x1a\n"
//   8  version[1]    = 2
//   9  flags[1]      bit0 启用保护, bit1 模式(0=永久删除, 1=限时锁定)
//  10  iterations[4] LE   PBKDF2-HMAC-SHA256 迭代次数（即“加密强度”）
//  14  maxTries[2]   LE   允许的错误尝试次数
//  16  lockDays[2]   LE   限时锁定模式下的锁定时长（天）
//  18  salt[16]
//  34  iv[12]         GCM 推荐 96-bit nonce
//  46  verifier[8]    HMAC-SHA256(key, label) 前 8 字节，用于快速校验密码
//  54  remaining[2]   LE   ← 可变（每次解密失败 -1）
//  56  lockUntil[8]   LE   ← 可变（Unix 秒，0 = 未锁定）
//  64  ciphertext[N]  与明文等长（GCM 为流模式，无填充）
//  末尾 tag[16]       GCM 认证标签
//
// 偏移 8..54 作为 GCM 的 AAD 参与认证：salt/IV/迭代次数/保护策略一旦被改动，
// 解密会直接认证失败——攻击者无法「关掉保护」。
// 偏移 54..64 必须在没有正确密码时也能改写，所以不参与认证，
// 这也意味着理论上可以被重置（详见 README 的安全说明）。
static const unsigned char kMagic[8] = { 'F', 'E', 'N', 'C', '\r', '\n', 0x1A, '\n' };
static const size_t kSaltLen = 16;
static const size_t kIvLen = 12;
static const size_t kTagLen = 16;
static const size_t kHeaderLenV2 = 64;

static const size_t kAadOff = 8;
static const size_t kAadLen = 46;      // 8..54
static const size_t kRemOff = 54;      // 剩余次数 [2]
static const size_t kLockOff = 56;     // 锁定截止 [8]

static const uint8_t kFlagProtected = 0x01;
static const uint8_t kFlagModeLock = 0x02;

static const char kVerifyLabel[] = "FileEncryptor/v2/verify";

enum ExhaustAction { EXHAUST_DELETE = 0, EXHAUST_LOCK = 1 };

// 加密时写入文件的保护策略
struct Protection {
    bool enabled = false;
    int  maxTries = 3;                       // 1..9999
    int  action = EXHAUST_DELETE;
    int  lockDays = 7;                       // 0..3650
};

// 从文件头读出的保护状态
struct FileState {
    bool     isProtected = false;
    int      action = EXHAUST_DELETE;
    int      maxTries = 0;
    int      remaining = 0;
    int      lockDays = 0;
    uint64_t lockUntil = 0;                  // Unix 秒，0 = 未锁定
};

enum Strength { STRENGTH_FAST = 0, STRENGTH_STANDARD = 1, STRENGTH_SECURE = 2, STRENGTH_EXTREME = 3 };

int strength_iterations(int strength) {
    switch (strength) {
    case STRENGTH_FAST:    return 50000;
    case STRENGTH_STANDARD: return 100000;
    case STRENGTH_SECURE:  return 300000;
    case STRENGTH_EXTREME: return 600000;
    }
    return 100000;
}

void put_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x & 0xFF));
    v.push_back((uint8_t)((x >> 8) & 0xFF));
}
void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x & 0xFF));
    v.push_back((uint8_t)((x >> 8) & 0xFF));
    v.push_back((uint8_t)((x >> 16) & 0xFF));
    v.push_back((uint8_t)((x >> 24) & 0xFF));
}
void put_u64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back((uint8_t)((x >> (8 * i)) & 0xFF));
}
uint16_t get_u16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
uint32_t get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint64_t get_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= ((uint64_t)p[i]) << (8 * i);
    return v;
}

std::vector<uint8_t> derive_key(const std::string& password, const std::vector<uint8_t>& salt, int iterations) {
    std::vector<uint8_t> key(32);
    if (PKCS5_PBKDF2_HMAC(password.c_str(), (int)password.length(),
        salt.data(), (int)salt.size(),
        iterations,
        EVP_sha256(),
        (int)key.size(),
        key.data()) != 1) {
        throw std::runtime_error("密钥派生失败");
    }
    return key;
}

// 密码验证标签：让「密码对不对」只需 PBKDF2 + 一次 HMAC，
// 不必为了判断密码而把整个密文过一遍（GUI 的重试循环依赖这一点）。
std::vector<uint8_t> VerifierOf(const std::vector<uint8_t>& key) {
    unsigned char mac[EVP_MAX_MD_SIZE] = {};
    unsigned int macLen = 0;
    if (HMAC(EVP_sha256(), key.data(), (int)key.size(),
             (const unsigned char*)kVerifyLabel, sizeof(kVerifyLabel) - 1,
             mac, &macLen) == nullptr) {
        throw std::runtime_error("计算验证标签失败");
    }
    return std::vector<uint8_t>(mac, mac + 8);
}

uint64_t NowUnix() {
    return (uint64_t)std::time(nullptr);
}

// 读取文件头的保护状态。返回 false 表示不是有效的 v2 加密文件。
bool ReadFileState(const std::string& path, FileState& st) {
    std::ifstream f(PathOf(path), std::ios::binary);
    if (!f) return false;
    uint8_t head[kHeaderLenV2] = {};
    f.read((char*)head, kHeaderLenV2);
    if (f.gcount() != (std::streamsize)kHeaderLenV2) return false;
    if (std::memcmp(head, kMagic, 8) != 0) return false;
    if (head[8] != 2) return false;

    const uint8_t flags = head[9];
    st.isProtected = (flags & kFlagProtected) != 0;
    st.action = (flags & kFlagModeLock) ? EXHAUST_LOCK : EXHAUST_DELETE;
    st.maxTries = (int)get_u16(&head[14]);
    st.lockDays = (int)get_u16(&head[16]);
    st.remaining = (int)get_u16(&head[kRemOff]);
    st.lockUntil = get_u64(&head[kLockOff]);
    return true;
}

// 只改写文件头中的可变部分（剩余次数 / 锁定截止），不动密文。
// 这两个字段不在 AAD 内，所以改写不会破坏认证。
bool WriteMutableState(const std::string& path, int remaining, uint64_t lockUntil) {
    std::fstream f(PathOf(path), std::ios::in | std::ios::out | std::ios::binary);
    if (!f) return false;
    uint8_t buf[10];
    buf[0] = (uint8_t)(remaining & 0xFF);
    buf[1] = (uint8_t)((remaining >> 8) & 0xFF);
    for (int i = 0; i < 8; ++i) buf[2 + i] = (uint8_t)((lockUntil >> (8 * i)) & 0xFF);
    f.seekp((std::streamoff)kRemOff, std::ios::beg);
    f.write((char*)buf, sizeof(buf));
    f.flush();
    return f.good();
}

// 格式化剩余时长
std::string FormatDuration(uint64_t seconds) {
    const uint64_t days = seconds / 86400;
    const uint64_t hours = (seconds % 86400) / 3600;
    const uint64_t mins = (seconds % 3600) / 60;
    std::string s;
    if (days > 0)  s += std::to_string(days) + " 天 ";
    if (hours > 0) s += std::to_string(hours) + " 小时 ";
    if (days == 0 && mins > 0) s += std::to_string(mins) + " 分钟";
    if (s.empty()) s = "不到 1 分钟";
    // 去掉结尾空格
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// 一次解密失败之后调用：扣减次数，必要时执行后果。
// 返回扣减后的剩余次数；remaining <= 0 表示已经用尽。
// outMsg 会写入给用户看的说明。
int ConsumeAttempt(const std::string& path, const FileState& st, std::string& outMsg) {
    if (!st.isProtected) return -1;

    // 之前已锁定且尚未到期，不重复扣减
    if (st.lockUntil != 0 && NowUnix() < st.lockUntil) {
        outMsg = "已锁定，剩余 " + FormatDuration(st.lockUntil - NowUnix());
        return 0;
    }

    int remaining = st.remaining - 1;
    if (remaining < 0) remaining = 0;

    if (remaining > 0) {
        WriteMutableState(path, remaining, 0);
        outMsg = "还可尝试 " + std::to_string(remaining) + " 次";
        return remaining;
    }

    // 用尽
    if (st.action == EXHAUST_LOCK) {
        const uint64_t until = NowUnix() + (uint64_t)st.lockDays * 86400ULL;
        WriteMutableState(path, 0, until);
        outMsg = "错误次数已达上限，已禁止解密 " + std::to_string(st.lockDays) + " 天";
        return 0;
    }

    // 永久删除
    std::error_code ec;
    fs::remove(PathOf(path), ec);
    outMsg = "错误次数已达上限，加密文件已被永久删除";
    return 0;
}

enum VerifyResult { VERIFY_OK, VERIFY_WRONG, VERIFY_UNAVAILABLE };

// 快速校验密码：只读文件头，做一次 PBKDF2 + 一次 HMAC，不解密正文。
// 只有能读到合法 v2 文件头时才可用，否则返回 VERIFY_UNAVAILABLE。
VerifyResult VerifyPassword(const std::string& path, const std::string& password, FileState& st) {
    if (!ReadFileState(path, st)) return VERIFY_UNAVAILABLE;

    std::ifstream f(PathOf(path), std::ios::binary);
    if (!f) return VERIFY_UNAVAILABLE;
    uint8_t hdr[kHeaderLenV2] = {};
    f.read((char*)hdr, kHeaderLenV2);
    if (f.gcount() != (std::streamsize)kHeaderLenV2) return VERIFY_UNAVAILABLE;

    const int iterations = (int)get_u32(&hdr[10]);
    if (iterations < 1000 || iterations > 10000000) return VERIFY_UNAVAILABLE;

    std::vector<uint8_t> salt(kSaltLen);
    std::memcpy(salt.data(), &hdr[18], kSaltLen);

    auto key = derive_key(password, salt, iterations);
    auto expect = VerifierOf(key);

    uint8_t diff = 0;
    for (size_t i = 0; i < expect.size(); ++i) diff |= (uint8_t)(expect[i] ^ hdr[46 + i]);
    return diff == 0 ? VERIFY_OK : VERIFY_WRONG;
}

// 删除文件。passes = 0 时直接删除（密文无明文残留，无需覆写）；
// passes >= 1 时先用随机数据覆写 N 遍再删除（明文源文件用）。
void secure_delete(const std::string& path, int passes) {
    if (passes <= 0) {
        std::error_code ec;
        fs::remove(PathOf(path), ec);
        return;
    }
    try {
        fs::path file_path = PathOf(path);
        if (!fs::exists(file_path)) return;
        
        size_t file_size = fs::file_size(file_path);
        if (file_size == 0) {
            fs::remove(file_path);
            return;
        }

        std::fstream file(PathOf(path), std::ios::in | std::ios::out | std::ios::binary);
        if (!file) {
            fs::remove(file_path);
            return;
        }

        const size_t CHUNK = 4096;
        std::vector<uint8_t> buf(CHUNK);
        
        for (int pass = 0; pass < passes; ++pass) {
            file.seekp(0, std::ios::beg);
            size_t remaining = file_size;
            while (remaining > 0) {
                size_t to_write = std::min(CHUNK, remaining);
                if (RAND_bytes(buf.data(), (int)to_write) != 1) {
                    std::fill(buf.begin(), buf.begin() + to_write, (uint8_t)(pass + 1));
                }
                if (pass % 2 == 1) {
                    for (size_t i = 0; i < to_write; ++i) buf[i] = ~buf[i];
                }
                file.write((char*)buf.data(), to_write);
                remaining -= to_write;
            }
            file.flush();
        }
        file.close();
        fs::remove(file_path);
    } catch (...) {
        std::error_code ec;
        fs::remove(path, ec);
    }
}

void show_progress(uint64_t current, uint64_t total) {
    const int bar_width = 50;
    double progress = (double)current / total;
    int pos = (int)(bar_width * progress);
    
    std::cout << "\r[";
    for (int i = 0; i < bar_width; ++i) {
        if (i < pos) std::cout << "=";
        else if (i == pos) std::cout << ">";
        else std::cout << " ";
    }
    std::cout << "] " << int(progress * 100.0) << "%  ";
    std::cout.flush();
}

// ---------- 加密核心（AES-256-GCM）----------
void encrypt_file(const std::string& input_path, const std::string& output_path,
                  const std::string& password, int iterations,
                  const Protection& prot = Protection()) {
    const size_t CHUNK = 4 * 1024 * 1024;

    std::vector<uint8_t> salt(kSaltLen), iv(kIvLen);
    if (RAND_bytes(salt.data(), (int)salt.size()) != 1)
        throw std::runtime_error("生成salt失败");
    if (RAND_bytes(iv.data(), (int)iv.size()) != 1)
        throw std::runtime_error("生成IV失败");

    auto key = derive_key(password, salt, iterations);
    auto verifier = VerifierOf(key);

    std::ifstream in_file(PathOf(input_path), std::ios::binary);
    if (!in_file) throw std::runtime_error("无法打开输入文件");

    std::ofstream out_file(PathOf(output_path), std::ios::binary);
    if (!out_file) throw std::runtime_error("无法创建输出文件");

    // ---- v2 文件头 ----
    std::vector<uint8_t> header;
    header.reserve(kHeaderLenV2);
    header.insert(header.end(), kMagic, kMagic + 8);
    header.push_back(2);                                   // version
    header.push_back(prot.enabled
        ? (uint8_t)(kFlagProtected | (prot.action == EXHAUST_LOCK ? kFlagModeLock : 0))
        : (uint8_t)0);                                     // flags
    put_u32(header, (uint32_t)iterations);
    put_u16(header, (uint16_t)(prot.enabled ? prot.maxTries : 0));
    put_u16(header, (uint16_t)(prot.enabled && prot.action == EXHAUST_LOCK ? prot.lockDays : 0));
    header.insert(header.end(), salt.begin(), salt.end());
    header.insert(header.end(), iv.begin(), iv.end());
    header.insert(header.end(), verifier.begin(), verifier.end());
    put_u16(header, (uint16_t)(prot.enabled ? prot.maxTries : 0));   // remaining = maxTries
    put_u64(header, 0);                                              // lockUntil
    out_file.write((char*)header.data(), header.size());

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("创建加密上下文失败");
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)kIvLen, nullptr) != 1 ||
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("AES初始化失败");
    }

    // AAD：把版本/标志/迭代次数/策略参数/salt/IV 一起纳入认证。
    // 攻击者若改动这些字段（例如把“启用保护”改成 0），认证会失败，文件也就解不开了。
    {
        int aadOut = 0;
        if (EVP_EncryptUpdate(ctx, nullptr, &aadOut,
                              header.data() + kAadOff, (int)kAadLen) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("写入AAD失败");
        }
    }

    uint64_t total_size = fs::file_size(PathOf(input_path));
    uint64_t processed = 0;

    std::vector<uint8_t> inbuf(CHUNK), outbuf(CHUNK + 32);
    while (in_file.read((char*)inbuf.data(), CHUNK) || in_file.gcount() > 0) {
        int bytes_read = (int)in_file.gcount();
        int outlen = 0;
        if (EVP_EncryptUpdate(ctx, outbuf.data(), &outlen, inbuf.data(), bytes_read) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("加密失败");
        }
        out_file.write((char*)outbuf.data(), outlen);
        processed += bytes_read;
        show_progress(processed, total_size);
    }

    int outlen = 0;
    if (EVP_EncryptFinal_ex(ctx, outbuf.data(), &outlen) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("完成加密失败");
    }
    if (outlen > 0) out_file.write((char*)outbuf.data(), outlen);

    uint8_t tag[kTagLen] = {};
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, (int)kTagLen, tag) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("获取认证标签失败");
    }
    out_file.write((char*)tag, kTagLen);

    // 循环里最后一次 show_progress 已经到 100%，这里不再重复打印进度条
    std::cout << std::endl;

    EVP_CIPHER_CTX_free(ctx);
    in_file.close();
    out_file.close();
    // 源文件的删除策略由 main() 决定（-k 保留 / -w 控制覆写次数）
}

// ---------- 解密核心（自动识别 v1 / v2 / 旧 CBC 格式）----------
void decrypt_file(const std::string& input_path, const std::string& output_path, const std::string& password) {
    const size_t CHUNK = 4 * 1024 * 1024;

    std::ifstream in_file(PathOf(input_path), std::ios::binary);
    if (!in_file) throw std::runtime_error("无法打开加密文件");

    uint64_t file_size = fs::file_size(PathOf(input_path));

    // 只认 v2 格式：固定 64 字节文件头，AAD 一律读取
    uint8_t hdr[kHeaderLenV2] = {};
    in_file.read((char*)hdr, kHeaderLenV2);
    if (in_file.gcount() != (std::streamsize)kHeaderLenV2)
        throw std::runtime_error("不是有效的加密文件（文件过小或已被截断）");
    if (std::memcmp(hdr, kMagic, 8) != 0)
        throw std::runtime_error("不是有效的加密文件（文件标识不匹配）");
    if (hdr[8] != 2)
        throw std::runtime_error("不支持的加密文件版本");

    const int iterations = (int)get_u32(&hdr[10]);
    if (iterations < 1000 || iterations > 10000000)
        throw std::runtime_error("文件头中的迭代次数非法");

    std::vector<uint8_t> salt(kSaltLen), iv(kIvLen);
    std::memcpy(salt.data(), &hdr[18], kSaltLen);
    std::memcpy(iv.data(), &hdr[34], kIvLen);

    uint8_t aad[kAadLen] = {};
    std::memcpy(aad, &hdr[kAadOff], kAadLen);

    const uint64_t overhead = (uint64_t)kHeaderLenV2 + kTagLen;
    if (file_size <= overhead) throw std::runtime_error("加密文件不完整");
    const uint64_t cipher_len = file_size - overhead;

    auto key = derive_key(password, salt, iterations);

    std::ofstream out_file(PathOf(output_path), std::ios::binary);
    if (!out_file) throw std::runtime_error("无法创建输出文件");

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("创建解密上下文失败");
    {
        int aadOut = 0;
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)kIvLen, nullptr) != 1 ||
            EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) != 1 ||
            EVP_DecryptUpdate(ctx, nullptr, &aadOut, aad, (int)kAadLen) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("AES初始化失败");
        }
    }

    // 失败时清理半成品输出
    auto fail = [&](const char* msg) -> void {
        EVP_CIPHER_CTX_free(ctx);
        out_file.close();
        std::error_code ec;
        fs::remove(PathOf(output_path), ec);
        throw std::runtime_error(msg);
    };

    uint64_t remaining = cipher_len;
    uint64_t processed = 0;
    std::vector<uint8_t> inbuf(CHUNK), outbuf(CHUNK + 32);
    while (remaining > 0) {
        size_t want = (size_t)std::min<uint64_t>(CHUNK, remaining);
        in_file.read((char*)inbuf.data(), want);
        std::streamsize got = in_file.gcount();
        if (got <= 0) fail("读取加密文件失败");
        int outlen = 0;
        if (EVP_DecryptUpdate(ctx, outbuf.data(), &outlen, inbuf.data(), (int)got) != 1)
            fail("解密失败");
        out_file.write((char*)outbuf.data(), outlen);
        remaining -= (uint64_t)got;
        processed += (uint64_t)got;
        show_progress(processed, cipher_len);
    }

    int outlen = 0;
    {
        uint8_t tag[kTagLen] = {};
        in_file.read((char*)tag, kTagLen);
        if (in_file.gcount() != (std::streamsize)kTagLen) fail("读取认证标签失败");
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, (int)kTagLen, tag) != 1)
            fail("设置认证标签失败");
        if (EVP_DecryptFinal_ex(ctx, outbuf.data(), &outlen) != 1)
            fail("认证失败：密码错误或文件已被篡改");
    }
    if (outlen > 0) out_file.write((char*)outbuf.data(), outlen);

    // 循环里最后一次 show_progress 已经到 100%，这里不再重复打印进度条
    std::cout << std::endl;

    EVP_CIPHER_CTX_free(ctx);
    in_file.close();
    out_file.close();
    // 加密文件的删除策略由 main() 决定（-k 保留）
}

// ---------- 主函数 ----------
void print_usage(const char* prog_name) {
    std::cout << "用法: " << prog_name << " -e|-d -i <输入文件> [选项]" << std::endl;
    std::cout << "  -e            加密模式" << std::endl;
    std::cout << "  -d            解密模式" << std::endl;
    std::cout << "  -i <文件>     输入文件路径" << std::endl;
    std::cout << "  -o <目录>     输出目录（默认与输入文件同目录）" << std::endl;
    std::cout << "  -p <密码>     密码（可选）。如不提供，会交互式输入" << std::endl;
    std::cout << "  -s <强度>     加密强度：fast|standard|secure|extreme（默认 standard）" << std::endl;
    std::cout << "                对应 PBKDF2 迭代次数 5万 / 10万 / 30万 / 60万" << std::endl;
    std::cout << "  -w <次数>     删除前覆写次数：0=直接删除，1/3=覆写次数（默认 3）" << std::endl;
    std::cout << "  -k, --keep    保留源文件（加密时）/ 加密文件（解密时），不做删除" << std::endl;
    std::cout << "                说明：解密后的加密文件不含明文残留，始终直接删除，不覆写" << std::endl;
    std::cout << "防暴力破解（仅加密时写入文件头）:" << std::endl;
    std::cout << "  -t, --max-tries <次数>   启用保护：允许的错误尝试次数（1-9999）" << std::endl;
    std::cout << "  -x, --on-exhaust <动作>  次数用尽后：delete=永久删除文件（默认）," << std::endl;
    std::cout << "                           lock=禁止解密一段时间" << std::endl;
    std::cout << "  -l, --lock-days <天数>   lock 模式下的锁定时长（0-3650，默认 7）" << std::endl;
    std::cout << "示例:" << std::endl;
    std::cout << "  交互式加密: " << prog_name << " -e -i secret.txt" << std::endl;
    std::cout << "  命令行加密: " << prog_name << " -e -i secret.txt -p mypass" << std::endl;
    std::cout << "  高强度加密: " << prog_name << " -e -i secret.txt -p mypass -s extreme" << std::endl;
    std::cout << "  输出到指定目录: " << prog_name << " -e -i secret.txt -p mypass -o D:\\Encrypted" << std::endl;
    std::cout << "  保留源文件: " << prog_name << " -e -i secret.txt -p mypass -k" << std::endl;
    std::cout << "  错 3 次即永久删除: " << prog_name << " -e -i secret.txt -p mypass -t 3" << std::endl;
    std::cout << "  错 5 次锁定 7 天:  " << prog_name << " -e -i secret.txt -p mypass -t 5 -x lock -l 7" << std::endl;
    std::cout << "  批量解密:   for %f in (*.enc) do " << prog_name
              << " -d -i \"%f\" -p mypass -o D:\\Decrypted" << std::endl;
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // 源码字面量是 UTF-8：把控制台输出代码页临时切到 UTF-8，退出时还原
    ConsoleUtf8Scope console_utf8;
    (void)console_utf8;
#endif
    OpenSSL_add_all_algorithms();
    ERR_load_crypto_strings();
    
    std::string mode, input_file, password, out_dir;
    bool pwd_provided = false;
    bool keep_source = false;
    int strength = STRENGTH_STANDARD;
    int overwrite_passes = 3;
    Protection prot;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = ArgToUtf8(argv[i]);
        if (arg == "-e" || arg == "-d") {
            mode = arg;
        } else if (arg == "-i" && i + 1 < argc) {
            input_file = ArgToUtf8(argv[++i]);
        } else if (arg == "-o" && i + 1 < argc) {
            out_dir = ArgToUtf8(argv[++i]);
        } else if (arg == "-p" && i + 1 < argc) {
            password = ArgToUtf8(argv[++i]);
            pwd_provided = true;
        } else if (arg == "-k" || arg == "--keep") {
            keep_source = true;
        } else if ((arg == "-t" || arg == "--max-tries") && i + 1 < argc) {
            try {
                prot.maxTries = std::stoi(ArgToUtf8(argv[++i]));
            } catch (...) {
                std::cerr << "错误：-t 需要一个整数（允许的错误尝试次数）" << std::endl;
                return 1;
            }
            if (prot.maxTries < 1 || prot.maxTries > 9999) {
                std::cerr << "错误：-t 取值范围为 1-9999" << std::endl;
                return 1;
            }
            prot.enabled = true;
        } else if ((arg == "-x" || arg == "--on-exhaust") && i + 1 < argc) {
            const std::string s = ArgToUtf8(argv[++i]);
            if (s == "delete")      prot.action = EXHAUST_DELETE;
            else if (s == "lock")   prot.action = EXHAUST_LOCK;
            else {
                std::cerr << "错误：-x 只能是 delete（永久删除）或 lock（禁止解密）" << std::endl;
                return 1;
            }
        } else if ((arg == "-l" || arg == "--lock-days") && i + 1 < argc) {
            try {
                prot.lockDays = std::stoi(ArgToUtf8(argv[++i]));
            } catch (...) {
                std::cerr << "错误：-l 需要一个整数（锁定的天数）" << std::endl;
                return 1;
            }
            if (prot.lockDays < 0 || prot.lockDays > 3650) {
                std::cerr << "错误：-l 取值范围为 0-3650 天" << std::endl;
                return 1;
            }
        } else if (arg == "-w" && i + 1 < argc) {
            std::string s = ArgToUtf8(argv[++i]);
            try {
                overwrite_passes = std::stoi(s);
            } catch (...) {
                std::cerr << "错误：-w 需要一个整数（0=直接删除，1/3=覆写次数）" << std::endl;
                return 1;
            }
            if (overwrite_passes < 0 || overwrite_passes > 10) {
                std::cerr << "错误：-w 取值范围为 0-10（推荐 0 / 1 / 3）" << std::endl;
                return 1;
            }
        } else if (arg == "-s" && i + 1 < argc) {
            std::string s = ArgToUtf8(argv[++i]);
            if (s == "fast")          strength = STRENGTH_FAST;
            else if (s == "standard") strength = STRENGTH_STANDARD;
            else if (s == "secure")   strength = STRENGTH_SECURE;
            else if (s == "extreme")  strength = STRENGTH_EXTREME;
            else {
                std::cerr << "错误：未知强度 '" << s << "'（可选 fast|standard|secure|extreme）" << std::endl;
                return 1;
            }
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "错误：无法识别的参数 '" << arg << "'" << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }
    
    if (mode.empty() || input_file.empty()) {
        print_usage(argv[0]);
        return 1;
    }
    
    // 输出目录校验（-o）
    if (!out_dir.empty()) {
        std::error_code ec;
        if (!fs::exists(PathOf(out_dir), ec) || !fs::is_directory(PathOf(out_dir), ec)) {
            std::cerr << "错误：输出目录不存在或不是目录: " << out_dir << std::endl;
            return 1;
        }
    }
    
    try {
        const bool encrypting = (mode == "-e");
        
        if (!encrypting && (input_file.size() < 4 || input_file.substr(input_file.size() - 4) != ".enc")) {
            std::cerr << "错误：解密文件应以 .enc 结尾" << std::endl;
            return 1;
        }
        
        // 输出文件名：加密加 .enc，解密去掉 .enc；目录由 -o 决定，默认与输入同目录
        const fs::path in_path = PathOf(input_file);
        std::string name = in_path.filename().string();
        if (encrypting) {
            name += ".enc";
        } else {
            name = name.substr(0, name.size() - 4);
        }
        const fs::path out_path = out_dir.empty()
            ? (in_path.parent_path() / name)
            : (PathOf(out_dir) / name);
        const std::string output = out_path.string();
        
        if (encrypting) {
            std::cout << "加密文件: " << input_file << std::endl;
            // 如果提供了 -p，直接使用；否则交互式输入（加密需要确认）
            std::string final_pwd = get_password("输入密码: ", true, pwd_provided ? password : "");
            encrypt_file(input_file, output, final_pwd, strength_iterations(strength), prot);
            if (prot.enabled) {
                if (prot.action == EXHAUST_LOCK) {
                    std::cout << "防暴力破解：错误 " << prot.maxTries << " 次后禁止解密 "
                              << prot.lockDays << " 天" << std::endl;
                } else {
                    std::cout << "防暴力破解：错误 " << prot.maxTries << " 次后永久删除文件" << std::endl;
                }
            }
            // 源文件含明文：-k 保留，否则按 -w 覆写后删除
            if (keep_source) {
                std::cout << "加密成功，源文件已保留。输出文件: " << output << std::endl;
            } else {
                secure_delete(input_file, overwrite_passes);
                std::cout << "加密成功，源文件已删除。输出文件: " << output << std::endl;
            }
        } else {
            std::cout << "解密文件: " << input_file << std::endl;

            // ---- 防暴力破解：先看保护状态与锁定情况 ----
            FileState st;
            bool haveState = ReadFileState(input_file, st);
            int remaining = st.remaining;

            if (haveState && st.isProtected) {
                if (st.lockUntil != 0) {
                    const uint64_t now = NowUnix();
                    if (now < st.lockUntil) {
                        std::cerr << "错误：该文件已被锁定，剩余 " << FormatDuration(st.lockUntil - now)
                                  << "（解锁时间由加密时的设置决定）" << std::endl;
                        return 1;
                    }
                    // 锁定已到期：恢复次数并清除锁定
                    remaining = st.maxTries;
                    WriteMutableState(input_file, remaining, 0);
                    st.remaining = remaining;
                    st.lockUntil = 0;
                    std::cout << "锁定已到期，剩余尝试次数已重置为 " << remaining << " 次" << std::endl;
                }
            }

            // ---- 收集密码：受保护文件支持循环重试 ----
            std::string final_pwd;
            if (haveState && st.isProtected) {
                for (;;) {
                    const std::string prompt =
                        "输入密码（还能尝试 " + std::to_string(remaining) + " 次）: ";
                    const std::string tryPwd = pwd_provided ? password : get_password(prompt, false, "");

                    FileState cur = st;
                    const VerifyResult vr = VerifyPassword(input_file, tryPwd, cur);
                    if (vr == VERIFY_OK) { final_pwd = tryPwd; break; }

                    std::string msg;
                    const int left = ConsumeAttempt(input_file, cur, msg);
                    std::cerr << "密码错误。" << msg << std::endl;
                    if (left <= 0) return 1;
                    remaining = left;
                    // 用了 -p 就没法再问用户，直接失败退出
                    if (pwd_provided) return 1;
                }
            }
            else {
                final_pwd = get_password("输入密码: ", false, pwd_provided ? password : "");
            }

            decrypt_file(input_file, output, final_pwd);
            // 加密文件本身是密文、不含明文残留，因此不做覆写，直接删除（与 GUI 一致）
            if (keep_source) {
                std::cout << "解密成功，加密文件已保留。输出文件: " << output << std::endl;
            } else {
                secure_delete(input_file, 0);
                std::cout << "解密成功，加密文件已删除。输出文件: " << output << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}