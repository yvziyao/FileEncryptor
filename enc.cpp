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

#include "file_format.h"   // 文件格式的唯一定义（与 GUI 共用）
#include "file_crypto.h"   // 加解密核心的唯一实现（与 GUI 共用）
using namespace fe;
using fe::PutU16; using fe::PutU32; using fe::PutU64;
using fe::GetU16; using fe::GetU32; using fe::GetU64;
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

// 文件格式（magic / 头部布局 / 偏移 / 标志位 / AAD 范围 / verifier 标签）
// 统一定义在 file_format.h，CLI 与 GUI 共用一份 —— 避免两份实现漂移。

// 加密强度 -> PBKDF2 迭代次数（CLI 专有，不属于共享格式）
enum Strength { STRENGTH_FAST = 0, STRENGTH_STANDARD = 1, STRENGTH_SECURE = 2, STRENGTH_EXTREME = 3 };

int strength_iterations(int strength) {
    switch (strength) {
    case STRENGTH_FAST:     return 50000;
    case STRENGTH_STANDARD: return 100000;
    case STRENGTH_SECURE:   return 300000;
    case STRENGTH_EXTREME:  return 600000;
    }
    return 100000;
}
// ---------------- 与共用实现的适配层 ----------------
// 真正的逻辑在 file_crypto.cpp（CLI 与 GUI 共用一份）。
// 这里只做 UTF-8 路径 -> 宽字符的转换和名字映射，避免调用点到处改。

static bool ReadFileState(const std::string& path, FileState& st) {
    return fe::ReadProtState(Utf8ToWide(path), st);
}

static bool WriteMutableState(const std::string& path, int remaining, uint64_t lockUntil) {
    return fe::WriteMutableState(Utf8ToWide(path), remaining, lockUntil);
}

static int ConsumeAttempt(const std::string& path, const FileState& st, std::string& outMsg) {
    return fe::ConsumeAttempt(Utf8ToWide(path), st, outMsg);
}

enum VerifyOutcome { VERIFY_OK, VERIFY_WRONG, VERIFY_UNAVAILABLE };

static VerifyOutcome VerifyPassword(const std::string& path, const std::string& password,
                                   FileState& st) {
    switch (fe::VerifyPassword(Utf8ToWide(path), password, st)) {
    case fe::VerifyResult::Ok:            return VERIFY_OK;
    case fe::VerifyResult::WrongPassword: return VERIFY_WRONG;
    default:                              return VERIFY_UNAVAILABLE;
    }
}

static void secure_delete(const std::string& path, int passes) {
    fe::SecureDeleteFile(Utf8ToWide(path), passes);
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
                  const Protection& prot = Protection(),
                  const std::string& decoy_path = std::string(),
                  const std::string& decoy_password = std::string(),
                  bool single_slot = false) {
    fe::ProgressFn prog = [](uint64_t cur, uint64_t total) { show_progress(cur, total); };
    const std::wstring decoyW = decoy_path.empty() ? std::wstring() : Utf8ToWide(decoy_path);
    fe::EncryptFileTo(Utf8ToWide(input_path), Utf8ToWide(output_path),
                      password, iterations, prot, decoyW, decoy_password, prog, single_slot);
    std::cout << std::endl;
    // 源文件的删除策略由 main() 决定（-k 保留 / -w 覆写次数）
}

void decrypt_file(const std::string& input_path, const std::string& output_path,
                  const std::string& password) {
    fe::ProgressFn prog = [](uint64_t cur, uint64_t total) { show_progress(cur, total); };
    fe::DecryptFileTo(Utf8ToWide(input_path), Utf8ToWide(output_path), password, prog);
    std::cout << std::endl;
}
void print_usage(const char* prog_name) {
    std::cout << "用法: " << prog_name << " -e|-d -i <输入文件> [选项]" << std::endl;
    std::cout << "  -e            加密模式" << std::endl;
    std::cout << "  -d            解密模式" << std::endl;
    std::cout << "  -i <文件>     输入文件路径" << std::endl;
    std::cout << "  -o <目录>     输出目录（默认与输入文件同目录）" << std::endl;
    std::cout << "  -p <密码>     密码（可选）。如不提供，会交互式输入" << std::endl;
    std::cout << "  -s <强度>     加密强度：fast|standard|secure|extreme（默认 standard）" << std::endl;
    std::cout << "                对应 PBKDF2 迭代次数 5万 / 10万 / 30万 / 60万" << std::endl;
    std::cout << "  -w <次数>     源文件处理：0=保留源文件（等同 -k），1-10=删除前覆写次数（默认 3）" << std::endl;
    std::cout << "  -k, --keep    保留源文件（加密时）/ 加密文件（解密时），不做删除" << std::endl;
    std::cout << "                说明：解密后的加密文件不含明文残留，删除时不覆写" << std::endl;
    std::cout << "  --decoy <文件>      可否认加密：指定诱饵文件" << std::endl;
    std::cout << "  --decoy-pass <密码> 可否认加密：假密码，用它解密得到的是诱饵文件" << std::endl;
    std::cout << "                      两者必须同时给出；受胁迫时交出假密码即可" << std::endl;
    std::cout << "                      真实内容进哪个槽是随机的，文件本身看不出哪个是诱饵" << std::endl;
    std::cout << "  --no-decoy          只写一个槽，文件体积约 1 倍" << std::endl;
    std::cout << "                      代价：体积会暴露「没有诱饵」，不可否认性减弱" << std::endl;
    std::cout << "安全删除（只删文件，不加密也不解密）:" << std::endl;
    std::cout << "  --delete, -D <文件...>  用随机数据覆写后删除；可一次给多个文件" << std::endl;
    std::cout << "                      覆写次数由 -w 决定（0=直接删除不覆写）" << std::endl;
    std::cout << "防暴力破解（仅加密时写入文件头）:" << std::endl;
    std::cout << "  -t, --max-tries <次数>   启用保护：允许的错误尝试次数（1-9999）" << std::endl;
    std::cout << "  -x, --on-exhaust <动作>  次数用尽后：delete=永久删除文件（默认）," << std::endl;
    std::cout << "                           lock=禁止解密一段时间" << std::endl;
    std::cout << "  -l, --lock-days <天数>   lock 模式下的锁定时长（0-3650，默认 7）" << std::endl;
    std::cout << "示例:" << std::endl;
    std::cout << "  交互式加密: " << prog_name << " -e -i secret.txt" << std::endl;
    std::cout << "  命令行加密: " << prog_name << " -e -i secret.txt -p mypass" << std::endl;
    std::cout << "  高强度加密: " << prog_name << " -e -i secret.txt -p mypass -s extreme" << std::endl;
    std::cout << "  安全删除文件: " << prog_name << " --delete secret.txt" << std::endl;
    std::cout << "  安全删除多个: " << prog_name << " --delete a.txt b.txt c.txt -w 3" << std::endl;
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
    // 密码学运算已改用 Windows CNG（bcrypt），无需 OpenSSL 初始化
    
    std::string mode, input_file, password, out_dir;
    bool pwd_provided = false;
    bool keep_source = false;
    int strength = STRENGTH_STANDARD;
    int overwrite_passes = 3;
    Protection prot;
    // 可否认加密：诱饵文件 + 假密码
    std::string decoy_file, decoy_password;
    bool decoy_pwd_provided = false;
    bool single_slot = false;   // --no-decoy：只写一个槽，体积约 1 倍
    // 安全删除模式（--delete）的文件列表：支持一次删多个
    std::vector<std::string> delete_files;
    
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
                std::cerr << "错误：-w 需要一个整数（0=保留源文件，1/3=覆写次数）" << std::endl;
                return 1;
            }
            if (overwrite_passes < 0 || overwrite_passes > 10) {
                std::cerr << "错误：-w 取值范围为 0-10（推荐 0 / 1 / 3）" << std::endl;
                return 1;
            }
        } else if ((arg == "--decoy" || arg == "--decoy-file") && i + 1 < argc) {
            decoy_file = ArgToUtf8(argv[++i]);
        } else if (arg == "--decoy-pass" && i + 1 < argc) {
            decoy_password = ArgToUtf8(argv[++i]);
            decoy_pwd_provided = true;
        } else if (arg == "--no-decoy" || arg == "--single-slot") {
            // 只写一个槽：体积约 1 倍，但文件大小会暴露「没有诱饵」
            single_slot = true;
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
        } else if (arg == "--delete" || arg == "-D") {
            mode = "--delete";
        } else if (!arg.empty() && arg[0] != '-' && mode == "--delete") {
            // --delete 之后的位置参数一律当作要删除的文件（支持一次多个）
            delete_files.push_back(arg);
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "错误：无法识别的参数 '" << arg << "'" << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }
    
    if (mode.empty()) {
        print_usage(argv[0]);
        return 1;
    }
    // --delete 可以用位置参数给文件，所以不强制 -i
    if (mode == "--delete") {
        if (input_file.empty() && delete_files.empty()) {
            print_usage(argv[0]);
            return 1;
        }
    } else if (input_file.empty()) {
        print_usage(argv[0]);
        return 1;
    }
    
    // ---- 安全删除模式：只删文件，不加密也不解密 ----
    // 与 GUI 的「安全删除」是同一个实现（fe::SecureDeleteFile），
    // 所以覆写次数、失败处理的行为完全一致。
    if (mode == "--delete") {
        if (!input_file.empty()) delete_files.insert(delete_files.begin(), input_file);

        int okCount = 0, failCount = 0;
        for (const std::string& f : delete_files) {
            try {
                // fe::SecureDeleteFile 对不存在的文件不会报错，这里先自己判断，
                // 否则会把「没删到」也算成成功
                std::error_code ec;
                if (!fs::exists(PathOf(f), ec) || fs::is_directory(PathOf(f), ec)) {
                    std::cerr << "错误：文件不存在或不是普通文件: " << f << std::endl;
                    ++failCount;
                    continue;
                }
                if (overwrite_passes > 0) {
                    std::cout << "正在安全删除（覆写 " << overwrite_passes << " 次）: " << f << std::endl;
                } else {
                    std::cout << "正在删除: " << f << std::endl;
                }
                fe::SecureDeleteFile(Utf8ToWide(f), overwrite_passes);
                ++okCount;
            }
            catch (const std::exception& e) {
                std::cerr << "错误：删除失败 " << f << " —— " << e.what() << std::endl;
                ++failCount;
            }
        }
        std::cout << "完成：成功 " << okCount << " 个";
        if (failCount > 0) std::cout << "，失败 " << failCount << " 个";
        std::cout << std::endl;
        return failCount > 0 ? 1 : 0;
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

            // 可否认加密：给了诱饵文件就必须给假密码，且两者都要
            std::string final_decoy_pwd;
            if (!decoy_file.empty()) {
                if (!decoy_pwd_provided) {
                    std::cerr << "错误：使用 --decoy 时必须同时提供 --decoy-pass（假密码）" << std::endl;
                    return 1;
                }
                std::error_code dec;
                if (!fs::exists(PathOf(decoy_file), dec)) {
                    std::cerr << "错误：诱饵文件不存在" << std::endl;
                    return 1;
                }
                if (decoy_password == final_pwd) {
                    std::cerr << "错误：假密码不能与真密码相同" << std::endl;
                    return 1;
                }
                final_decoy_pwd = decoy_password;
                std::cout << "已启用可否认加密：被胁迫时可交出假密码，解出的是诱饵文件"
                          << std::endl;
            }

            encrypt_file(input_file, output, final_pwd, strength_iterations(strength), prot,
                         decoy_file, final_decoy_pwd, single_slot);
            if (prot.enabled) {
                if (prot.action == EXHAUST_LOCK) {
                    std::cout << "防暴力破解：错误 " << prot.maxTries << " 次后禁止解密 "
                              << prot.lockDays << " 天" << std::endl;
                } else {
                    std::cout << "防暴力破解：错误 " << prot.maxTries << " 次后永久删除文件" << std::endl;
                }
            }
            // 源文件含明文。-k 与 -w 0 都表示保留：
            // -w 0 对应 GUI 设置里的“禁用安全删除”，GUI 那边是保留源文件，
            // 这里必须一致，否则同一个选项在两边行为不同。
            if (keep_source || overwrite_passes == 0) {
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
                    const VerifyOutcome vr = VerifyPassword(input_file, tryPwd, cur);
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
            // 加密文件本身是密文、不含明文残留，因此不做覆写，直接删除（与 GUI 一致）；
            // 同样地，-k 或 -w 0 表示保留
            if (keep_source || overwrite_passes == 0) {
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