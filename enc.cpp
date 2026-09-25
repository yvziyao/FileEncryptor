// enc.cpp
// 极简命令行文件加密工具 (支持 -p 参数)
// 编译: 见下方说明

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define OPENSSL_API_COMPAT 0x10100000L

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>
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

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

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

// 文件格式 v1（AES-256-GCM），与 GUI 版完全一致：
//   magic[8] = "FENC\r\n\x1a\n"
//   version[1] = 1
//   reserved[1] = 0
//   iterations[4] LE   PBKDF2-HMAC-SHA256 迭代次数（即“加密强度”）
//   salt[16]
//   iv[12]             GCM 推荐 96-bit nonce
//   ciphertext[N]      与明文等长（流模式，无填充）
//   tag[16]            GCM 认证标签
static const unsigned char kMagic[8] = { 'F', 'E', 'N', 'C', '\r', '\n', 0x1A, '\n' };
static const size_t kSaltLen = 16;
static const size_t kIvLen = 12;
static const size_t kTagLen = 16;
static const size_t kHeaderLen = 8 + 1 + 1 + 4 + kSaltLen + kIvLen;   // 42

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

void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x & 0xFF));
    v.push_back((uint8_t)((x >> 8) & 0xFF));
    v.push_back((uint8_t)((x >> 16) & 0xFF));
    v.push_back((uint8_t)((x >> 24) & 0xFF));
}
uint32_t get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
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

void secure_delete(const std::string& path) {
    try {
        fs::path file_path(path);
        if (!fs::exists(file_path)) return;
        
        size_t file_size = fs::file_size(file_path);
        if (file_size == 0) {
            fs::remove(file_path);
            return;
        }

        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        if (!file) {
            fs::remove(file_path);
            return;
        }

        const size_t CHUNK = 4096;
        std::vector<uint8_t> buf(CHUNK);
        
        for (int pass = 0; pass < 3; ++pass) {
            file.seekp(0, std::ios::beg);
            size_t remaining = file_size;
            while (remaining > 0) {
                size_t to_write = std::min(CHUNK, remaining);
                if (RAND_bytes(buf.data(), (int)to_write) != 1) {
                    std::fill(buf.begin(), buf.begin() + to_write, (uint8_t)(pass + 1));
                }
                if (pass == 1) {
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
        fs::remove(path);
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
                  const std::string& password, int iterations) {
    const size_t CHUNK = 4 * 1024 * 1024;

    std::vector<uint8_t> salt(kSaltLen), iv(kIvLen);
    if (RAND_bytes(salt.data(), (int)salt.size()) != 1)
        throw std::runtime_error("生成salt失败");
    if (RAND_bytes(iv.data(), (int)iv.size()) != 1)
        throw std::runtime_error("生成IV失败");

    auto key = derive_key(password, salt, iterations);

    std::ifstream in_file(input_path, std::ios::binary);
    if (!in_file) throw std::runtime_error("无法打开输入文件");

    std::ofstream out_file(output_path, std::ios::binary);
    if (!out_file) throw std::runtime_error("无法创建输出文件");

    // 文件头
    std::vector<uint8_t> header;
    header.reserve(kHeaderLen);
    header.insert(header.end(), kMagic, kMagic + 8);
    header.push_back(1);                       // version
    header.push_back(0);                       // reserved
    put_u32(header, (uint32_t)iterations);
    header.insert(header.end(), salt.begin(), salt.end());
    header.insert(header.end(), iv.begin(), iv.end());
    out_file.write((char*)header.data(), header.size());

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("创建加密上下文失败");
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)kIvLen, nullptr) != 1 ||
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("AES初始化失败");
    }

    uint64_t total_size = fs::file_size(input_path);
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

    show_progress(total_size, total_size);
    std::cout << std::endl;

    EVP_CIPHER_CTX_free(ctx);
    in_file.close();
    out_file.close();

    secure_delete(input_path);
    std::cout << "加密成功，源文件已删除。输出文件: " << output_path << std::endl;
}

// ---------- 解密核心（自动识别新旧格式）----------
void decrypt_file(const std::string& input_path, const std::string& output_path, const std::string& password) {
    const size_t CHUNK = 4 * 1024 * 1024;

    std::ifstream in_file(input_path, std::ios::binary);
    if (!in_file) throw std::runtime_error("无法打开加密文件");

    uint64_t file_size = fs::file_size(input_path);

    // 嗅探文件头：新版以魔数开头，旧版直接是随机 salt
    uint8_t head[8] = {};
    in_file.read((char*)head, 8);
    if (in_file.gcount() != 8) throw std::runtime_error("加密文件过小或已被截断");
    const bool new_format = (memcmp(head, kMagic, 8) == 0);

    std::vector<uint8_t> salt(kSaltLen), iv;
    int iterations = 100000;   // 旧格式固定 100000
    size_t prefix_len = 0;

    if (new_format) {
        uint8_t meta[1 + 1 + 4] = {};
        in_file.read((char*)meta, sizeof(meta));
        if (in_file.gcount() != (std::streamsize)sizeof(meta)) throw std::runtime_error("读取文件头失败");
        if (meta[0] != 1) throw std::runtime_error("不支持的加密文件版本");
        iterations = (int)get_u32(&meta[2]);
        if (iterations < 1000 || iterations > 10000000) throw std::runtime_error("文件头中的迭代次数非法");

        in_file.read((char*)salt.data(), salt.size());
        if (in_file.gcount() != (std::streamsize)salt.size()) throw std::runtime_error("读取salt失败");
        iv.resize(kIvLen);
        in_file.read((char*)iv.data(), iv.size());
        if (in_file.gcount() != (std::streamsize)iv.size()) throw std::runtime_error("读取IV失败");
        prefix_len = kHeaderLen;
    }
    else {
        // 旧格式：salt[16] + iv[16] + CBC 密文
        in_file.clear();
        in_file.seekg(0, std::ios::beg);
        in_file.read((char*)salt.data(), salt.size());
        if (in_file.gcount() != (std::streamsize)salt.size()) throw std::runtime_error("读取salt失败");
        iv.resize(16);
        in_file.read((char*)iv.data(), iv.size());
        if (in_file.gcount() != (std::streamsize)iv.size()) throw std::runtime_error("读取IV失败");
        prefix_len = salt.size() + iv.size();
    }

    const uint64_t overhead = (uint64_t)prefix_len + (new_format ? kTagLen : 0);
    if (file_size <= overhead) throw std::runtime_error("加密文件不完整");
    const uint64_t cipher_len = file_size - overhead;

    auto key = derive_key(password, salt, iterations);

    std::ofstream out_file(output_path, std::ios::binary);
    if (!out_file) throw std::runtime_error("无法创建输出文件");

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("创建解密上下文失败");
    if (new_format) {
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)kIvLen, nullptr) != 1 ||
            EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("AES初始化失败");
        }
    }
    else {
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, key.data(), iv.data()) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("AES初始化失败");
        }
    }

    // 失败时清理半成品输出
    auto fail = [&](const char* msg) -> void {
        EVP_CIPHER_CTX_free(ctx);
        out_file.close();
        std::error_code ec;
        fs::remove(output_path, ec);
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
    if (new_format) {
        uint8_t tag[kTagLen] = {};
        in_file.read((char*)tag, kTagLen);
        if (in_file.gcount() != (std::streamsize)kTagLen) fail("读取认证标签失败");
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, (int)kTagLen, tag) != 1)
            fail("设置认证标签失败");
        if (EVP_DecryptFinal_ex(ctx, outbuf.data(), &outlen) != 1)
            fail("认证失败：密码错误或文件已被篡改");
    }
    else {
        if (EVP_DecryptFinal_ex(ctx, outbuf.data(), &outlen) != 1)
            fail("解密失败：密码错误或文件已被篡改");
    }
    if (outlen > 0) out_file.write((char*)outbuf.data(), outlen);

    show_progress(cipher_len, cipher_len);
    std::cout << std::endl;

    EVP_CIPHER_CTX_free(ctx);
    in_file.close();
    out_file.close();

    // 加密文件是密文，无明文残留，直接删除即可（与 GUI 版一致）
    std::error_code ec;
    fs::remove(input_path, ec);
    std::cout << "解密成功，加密文件已删除。输出文件: " << output_path << std::endl;
}

// ---------- 主函数 ----------
void print_usage(const char* prog_name) {
    std::cout << "用法: " << prog_name << " -e|-d -i <输入文件> [-p <密码>] [-s <强度>]" << std::endl;
    std::cout << "  -e    加密模式" << std::endl;
    std::cout << "  -d    解密模式" << std::endl;
    std::cout << "  -i    输入文件路径" << std::endl;
    std::cout << "  -p    密码（可选）。如不提供，会交互式输入" << std::endl;
    std::cout << "  -s    加密强度：fast|standard|secure|extreme（默认 standard）" << std::endl;
    std::cout << "        对应 PBKDF2 迭代次数 5万 / 10万 / 30万 / 60万" << std::endl;
    std::cout << "示例:" << std::endl;
    std::cout << "  交互式加密: " << prog_name << " -e -i secret.txt" << std::endl;
    std::cout << "  命令行加密: " << prog_name << " -e -i secret.txt -p mypass" << std::endl;
    std::cout << "  高强度加密: " << prog_name << " -e -i secret.txt -p mypass -s extreme" << std::endl;
    std::cout << "  批量解密:   for %f in (*.enc) do " << prog_name << " -d -i \"%f\" -p mypass" << std::endl;
}

int main(int argc, char* argv[]) {
    OpenSSL_add_all_algorithms();
    ERR_load_crypto_strings();
    
    std::string mode, input_file, password;
    bool pwd_provided = false;
    int strength = STRENGTH_STANDARD;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-e" || arg == "-d") {
            mode = arg;
        } else if (arg == "-i" && i + 1 < argc) {
            input_file = argv[++i];
        } else if (arg == "-p" && i + 1 < argc) {
            password = argv[++i];
            pwd_provided = true;
        } else if (arg == "-s" && i + 1 < argc) {
            std::string s = argv[++i];
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
        }
    }
    
    if (mode.empty() || input_file.empty()) {
        print_usage(argv[0]);
        return 1;
    }
    
    try {
        if (mode == "-e") {
            std::cout << "加密文件: " << input_file << std::endl;
            // 如果提供了 -p，直接使用；否则交互式输入（加密需要确认）
            std::string final_pwd = get_password("输入密码: ", true, pwd_provided ? password : "");
            std::string output = input_file + ".enc";
            encrypt_file(input_file, output, final_pwd, strength_iterations(strength));
        } else if (mode == "-d") {
            if (input_file.size() < 4 || input_file.substr(input_file.size() - 4) != ".enc") {
                std::cerr << "错误：解密文件应以 .enc 结尾" << std::endl;
                return 1;
            }
            std::cout << "解密文件: " << input_file << std::endl;
            // 解密时，如果提供了 -p，直接使用；否则交互式输入（不需要确认）
            std::string final_pwd = get_password("输入密码: ", false, pwd_provided ? password : "");
            std::string output = input_file.substr(0, input_file.size() - 4);
            decrypt_file(input_file, output, final_pwd);
        } else {
            std::cerr << "未知模式: " << mode << std::endl;
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}