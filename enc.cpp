// enc.cpp
// 极简命令行文件加密工具 (支持 -p 参数)
// 编译: 见下方说明

#define NOMINMAX
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
    // 如果已经提供了密码，直接返回（用于 -p 参数）
    if (!preloaded.empty()) {
        if (confirm) {
            // 加密模式下，即使提供了 -p，也要确认一次（避免手误）
            std::cout << "使用命令行密码，请再次输入以确认: ";
            std::string confirm_pwd;
#ifdef _WIN32
            char ch;
            while ((ch = _getch()) != '\r') {
                if (ch == '\b') {
                    if (!confirm_pwd.empty()) {
                        confirm_pwd.pop_back();
                        std::cout << "\b \b";
                    }
                } else if (ch != 0 && ch != -32) {
                    confirm_pwd.push_back(ch);
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
            std::getline(std::cin, confirm_pwd);
            tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
            std::cout << std::endl;
#endif
            if (preloaded != confirm_pwd) {
                throw std::runtime_error("两次输入的密码不一致");
            }
        }
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
std::vector<uint8_t> derive_key(const std::string& password, const std::vector<uint8_t>& salt) {
    std::vector<uint8_t> key(32);
    PKCS5_PBKDF2_HMAC(password.c_str(), (int)password.length(),
        salt.data(), (int)salt.size(),
        100000,
        EVP_sha256(),
        (int)key.size(),
        key.data());
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

// ---------- 加密核心 ----------
void encrypt_file(const std::string& input_path, const std::string& output_path, const std::string& password) {
    const size_t CHUNK = 4 * 1024 * 1024;
    
    std::vector<uint8_t> salt(16), iv(16);
    if (RAND_bytes(salt.data(), (int)salt.size()) != 1) 
        throw std::runtime_error("生成salt失败");
    if (RAND_bytes(iv.data(), (int)iv.size()) != 1) 
        throw std::runtime_error("生成IV失败");
    
    auto key = derive_key(password, salt);
    
    std::ifstream in_file(input_path, std::ios::binary);
    if (!in_file) throw std::runtime_error("无法打开输入文件");
    
    std::ofstream out_file(output_path, std::ios::binary);
    if (!out_file) throw std::runtime_error("无法创建输出文件");
    
    out_file.write((char*)salt.data(), salt.size());
    out_file.write((char*)iv.data(), iv.size());
    
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("创建加密上下文失败");
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, key.data(), iv.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("AES初始化失败");
    }
    
    uint64_t total_size = fs::file_size(input_path);
    uint64_t processed = 0;
    
    std::vector<uint8_t> inbuf(CHUNK), outbuf(CHUNK + 16);
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
    out_file.write((char*)outbuf.data(), outlen);
    show_progress(total_size, total_size);
    std::cout << std::endl;
    
    EVP_CIPHER_CTX_free(ctx);
    in_file.close();
    out_file.close();
    
    secure_delete(input_path);
    std::cout << "加密成功！输出文件: " << output_path << std::endl;
}

// ---------- 解密核心 ----------
void decrypt_file(const std::string& input_path, const std::string& output_path, const std::string& password) {
    const size_t CHUNK = 4 * 1024 * 1024;
    
    std::ifstream in_file(input_path, std::ios::binary);
    if (!in_file) throw std::runtime_error("无法打开加密文件");
    
    std::vector<uint8_t> salt(16), iv(16);
    in_file.read((char*)salt.data(), salt.size());
    in_file.read((char*)iv.data(), iv.size());
    if (!in_file) throw std::runtime_error("读取salt/IV失败");
    
    auto key = derive_key(password, salt);
    
    std::ofstream out_file(output_path, std::ios::binary);
    if (!out_file) throw std::runtime_error("无法创建输出文件");
    
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("创建解密上下文失败");
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, key.data(), iv.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("AES初始化失败");
    }
    
    uint64_t total_size = fs::file_size(input_path) - 32;
    uint64_t processed = 0;
    
    std::vector<uint8_t> inbuf(CHUNK), outbuf(CHUNK + 16);
    while (in_file.read((char*)inbuf.data(), CHUNK) || in_file.gcount() > 0) {
        int bytes_read = (int)in_file.gcount();
        int outlen = 0;
        if (EVP_DecryptUpdate(ctx, outbuf.data(), &outlen, inbuf.data(), bytes_read) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("解密失败，密码可能错误");
        }
        out_file.write((char*)outbuf.data(), outlen);
        processed += bytes_read;
        show_progress(processed, total_size);
    }
    
    int outlen = 0;
    if (EVP_DecryptFinal_ex(ctx, outbuf.data(), &outlen) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("完成解密失败，密码错误或文件损坏");
    }
    out_file.write((char*)outbuf.data(), outlen);
    show_progress(total_size, total_size);
    std::cout << std::endl;
    
    EVP_CIPHER_CTX_free(ctx);
    in_file.close();
    out_file.close();
    
    secure_delete(input_path);
    std::cout << "解密成功！输出文件: " << output_path << std::endl;
}

// ---------- 主函数 ----------
void print_usage(const char* prog_name) {
    std::cout << "用法: " << prog_name << " -e|-d -i <输入文件> [-p <密码>]" << std::endl;
    std::cout << "  -e    加密模式" << std::endl;
    std::cout << "  -d    解密模式" << std::endl;
    std::cout << "  -i    输入文件路径" << std::endl;
    std::cout << "  -p    密码（可选）。如不提供，会交互式输入" << std::endl;
    std::cout << "示例:" << std::endl;
    std::cout << "  交互式加密: " << prog_name << " -e -i secret.txt" << std::endl;
    std::cout << "  命令行加密: " << prog_name << " -e -i secret.txt -p mypass" << std::endl;
    std::cout << "  批量解密:   for %f in (*.enc) do " << prog_name << " -d -i \"%f\" -p mypass" << std::endl;
}

int main(int argc, char* argv[]) {
    OpenSSL_add_all_algorithms();
    ERR_load_crypto_strings();
    
    std::string mode, input_file, password;
    bool pwd_provided = false;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-e" || arg == "-d") {
            mode = arg;
        } else if (arg == "-i" && i + 1 < argc) {
            input_file = argv[++i];
        } else if (arg == "-p" && i + 1 < argc) {
            password = argv[++i];
            pwd_provided = true;
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
            encrypt_file(input_file, output, final_pwd);
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