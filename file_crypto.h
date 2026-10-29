// file_crypto.h - 加解密核心（CLI 与 GUI 共用的唯一实现）
//
// 之前这套逻辑在 enc.cpp 和 FileEncryptor.cpp 里各写了一份。v3 引入双槽位后
// 两份必然漂移，所以实现也集中到这里。
//
// 职责边界：
//   * 这里负责：文件格式的读写、AES-256-GCM 加解密、保护状态、计数扣减
//   * 前端负责：提示文案、锁定前的交互流程、进度条外观、UI
//
// 密码学运算使用 Windows 自带的 CNG（bcrypt），不依赖任何第三方库。
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace fe {

enum ExhaustAction { EXHAUST_DELETE = 0, EXHAUST_LOCK = 1 };

// 加密时写入文件的保护策略
struct Protection {
    bool enabled = false;
    int  maxTries = 3;                        // 1..9999
    int  action = EXHAUST_DELETE;
    int  lockDays = 7;                        // 0..3650
};

// 从文件头读出的保护状态
struct FileState {
    bool     isProtected = false;
    int      action = EXHAUST_DELETE;
    int      maxTries = 0;
    int      remaining = 0;
    int      lockDays = 0;
    uint64_t lockUntil = 0;                   // Unix 秒，0 = 未锁定
};

enum class VerifyResult { Ok, WrongPassword, Unavailable };

// 进度回调：(已完成字节, 总字节)。可能被高频调用，前端自行节流。
using ProgressFn = std::function<void(uint64_t, uint64_t)>;

// 加密。
//   decoyPath 为空  -> 没有诱饵：仍写满两个槽，另一槽填随机数据，
//                      因此无法从文件判断「是否存在诱饵」或「哪个槽是真」。
//   decoyPath 非空  -> 诱饵文件用 decoyPassword 加密进另一个槽。
// 真实载荷进哪个槽是**随机**的，与是否为诱饵无关。
//
// singleSlot = true 时**只写一个槽**，文件体积回到约 1 倍（不再翻倍）。
// 代价：文件大小本身会暴露「没有诱饵」——单槽恰好是 1x、双槽是 2x，
// 也就是用「不可区分性」换「体积」。仅在没有诱饵需求时才应开启。
void EncryptFileTo(const std::wstring& in, const std::wstring& out,
                   const std::string& password, int iterations,
                   const Protection& prot,
                   const std::wstring& decoyPath, const std::string& decoyPassword,
                   const ProgressFn& progress, bool singleSlot = false);

// 解密。依次尝试两个槽的 verifier，命中哪个就解哪个 --
// 两个槽完全对称，此函数不区分也不对外暴露「这是第几个槽」。
// 密码不匹配任何槽时抛 std::runtime_error。
void DecryptFileTo(const std::wstring& in, const std::wstring& out,
                   const std::string& password, const ProgressFn& progress);

// 读取文件头的保护状态。返回 false 表示不是有效的 v3 加密文件。
bool ReadProtState(const std::wstring& path, FileState& out);

// 只改写头部可变字段（剩余次数 / 锁定截止），不动密文。
// 这两个字段不在 AAD 内，改写不会破坏认证。
bool WriteMutableState(const std::wstring& path, int remaining, uint64_t lockUntil);

// 快速校验密码：只读头部，做一次 PBKDF2 + 两次 HMAC，不解密正文。
VerifyResult VerifyPassword(const std::wstring& path, const std::string& password, FileState& out);

// 一次解密失败后调用：扣减次数，必要时执行后果（锁定 / 永久删除）。
// 返回扣减后的剩余次数；-1 表示未启用保护。
int ConsumeAttempt(const std::wstring& path, const FileState& st, std::string& outMsg);

// 把秒数格式化成中文时长（"3 天 2 小时"）
std::string FormatDuration(uint64_t seconds);

// 删除文件。passes = 0 直接删除；passes >= 1 先用随机数据覆写 N 遍再删除。
void SecureDeleteFile(const std::wstring& path, int passes,
                      const ProgressFn& progress = ProgressFn());

uint64_t NowUnix();

// 测试用：暴露槽位分配，便于验证随机性（不影响安全性）
void SetSlotChoiceForTest(int fixedSlot);   // -1 = 恢复随机

// ---- 随机密码生成（GUI 与 CLI 共用一份实现）----
// 用系统的 CSPRNG（BCryptGenRandom）+ 拒绝采样避免取模偏差，
// 并保证"每一类最少出现一次"，最后打乱位置。
struct PasswordRules {
    int  length  = 20;
    bool upper   = true;
    bool lower   = true;
    bool digits  = true;
    bool symbols = true;
};
std::string GeneratePassword(const PasswordRules& r);

} // namespace fe
