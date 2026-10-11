// file_format.h - 加密文件格式的**唯一定义**
//
// 背景：这套格式原本在 CLI (enc.cpp) 和 GUI (FileEncryptor.cpp) 里各写了一份。
// 引入双槽位（诱饵 / 可否认加密）后两份必然漂移 —— 典型后果是
// 「GUI 加密的文件 CLI 解不开」。所以格式只在这里定义一次。
//
// 这里只放**格式本身**（常量、字节序、头部序列化/解析）。
// 加解密流程在 file_crypto.h/.cpp，同样是两边共用一份。
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace fe {

// ---------------------------------------------------------------- 格式常量

// 文件标识（8 字节）
inline const uint8_t kMagic[8] = { 'F', 'E', 'N', 'C', '\r', '\n', 0x1A, '\n' };

inline const size_t kSaltLen = 16;
inline const size_t kIvLen   = 12;   // GCM 推荐 96-bit nonce
inline const size_t kTagLen  = 16;   // GCM 认证标签
inline const size_t kKeyLen  = 32;   // AES-256

// v3 头部布局（两个槽位完全对称，头部**没有**任何字段标明哪个是真）：
//
//    0  magic[8]
//    8  version[1]      = 3
//    9  flags[1]        bit0 启用保护, bit1 模式(0=永久删除, 1=限时锁定)
//   10  iterations[4]   LE
//   14  maxTries[2]     LE
//   16  lockDays[2]     LE
//   18  saltA[16]
//   34  ivA[12]
//   46  verifierA[8]
//   54  sizeA[8]        LE  槽 A 密文长度（含 GCM tag）
//   62  saltB[16]
//   78  ivB[12]
//   90  verifierB[8]
//   98  remaining[2]    LE   ← 可变（每次解密失败 -1）
//  100  lockUntil[8]    LE   ← 可变（Unix 秒，0 = 未锁定）
//  108  ciphertextA[sizeA]
//  108+sizeA  ciphertextB（直到文件末尾）
//
// 槽 A / 槽 B 只是标号，不代表真假：加密时**随机**决定真实载荷进哪个槽。
// 即使没有诱饵也照样写满两个槽（另一槽填随机数据），因此
// 「是否存在诱饵」「哪个槽是真」都无法从文件本身判断。
inline const size_t kHeaderLenV3 = 108;

// 参与 AAD 认证的范围：偏移 8..98（两槽元数据 + 保护策略）。
// 改动其中任何一字节，解密都会认证失败 —— 攻击者无法「关掉保护」，
// 也无法把某个槽的元数据换掉。
inline const size_t kAadOff = 8;
inline const size_t kAadLen = 90;   // 8..98

// 头部内各字段偏移
inline const size_t kOffVersion   = 8;
inline const size_t kOffFlags     = 9;
inline const size_t kOffIter      = 10;
inline const size_t kOffMaxTries  = 14;
inline const size_t kOffLockDays  = 16;
inline const size_t kOffSaltA     = 18;
inline const size_t kOffIvA       = 34;
inline const size_t kOffVerifierA = 46;
inline const size_t kOffSizeA     = 54;
inline const size_t kOffSaltB     = 62;
inline const size_t kOffIvB       = 78;
inline const size_t kOffVerifierB = 90;
inline const size_t kRemOff       = 98;    // remaining[2]  ← 可变，不参与认证
inline const size_t kLockOff      = 100;   // lockUntil[8]  ← 可变，不参与认证

inline const uint8_t kFlagProtected = 0x01;
inline const uint8_t kFlagModeLock  = 0x02;

// verifier 用的 HMAC 标签：换了它等于换格式，两边必须一致
inline const char kVerifyLabel[] = "FileEncryptor/v3/verify";

// ---------------------------------------------------------------- 字节序
// 文件里一律小端，不依赖主机字节序

inline void PutU16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x & 0xFF));
    v.push_back((uint8_t)((x >> 8) & 0xFF));
}
inline void PutU32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back((uint8_t)((x >> (8 * i)) & 0xFF));
}
inline void PutU64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back((uint8_t)((x >> (8 * i)) & 0xFF));
}
inline uint16_t GetU16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
inline uint32_t GetU32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint64_t GetU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= ((uint64_t)p[i]) << (8 * i);
    return v;
}
inline void StoreU16(uint8_t* p, uint16_t x) {
    p[0] = (uint8_t)(x & 0xFF);
    p[1] = (uint8_t)((x >> 8) & 0xFF);
}
inline void StoreU64(uint8_t* p, uint64_t x) {
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)((x >> (8 * i)) & 0xFF);
}

// ---------------------------------------------------------------- 槽位
// 0 = 槽 A，1 = 槽 B。二者完全对称，仅在文件中的位置不同。
inline const int kSlotCount = 2;

inline size_t SaltOff(int slot)     { return slot == 0 ? kOffSaltA : kOffSaltB; }
inline size_t IvOff(int slot)       { return slot == 0 ? kOffIvA   : kOffIvB; }
inline size_t VerifierOff(int slot) { return slot == 0 ? kOffVerifierA : kOffVerifierB; }

// ---------------------------------------------------------------- 头部结构

struct HeaderV3 {
    uint8_t  version = 3;
    uint8_t  flags = 0;
    uint32_t iterations = 0;
    uint16_t maxTries = 0;
    uint16_t lockDays = 0;
    uint8_t  salt[kSlotCount][kSaltLen] = {};
    uint8_t  iv[kSlotCount][kIvLen] = {};
    uint8_t  verifier[kSlotCount][8] = {};
    uint64_t sizeA = 0;         // 槽 A 密文长度
    uint16_t remaining = 0;     // 可变
    uint64_t lockUntil = 0;     // 可变

    bool isProtected() const { return (flags & kFlagProtected) != 0; }
    bool isLockMode()  const { return (flags & kFlagModeLock) != 0; }
};

// 序列化为 108 字节头部
inline std::vector<uint8_t> SerializeHeaderV3(const HeaderV3& h) {
    std::vector<uint8_t> v;
    v.reserve(kHeaderLenV3);
    v.insert(v.end(), kMagic, kMagic + 8);
    v.push_back(h.version);
    v.push_back(h.flags);
    PutU32(v, h.iterations);
    PutU16(v, h.maxTries);
    PutU16(v, h.lockDays);
    v.insert(v.end(), h.salt[0], h.salt[0] + kSaltLen);
    v.insert(v.end(), h.iv[0], h.iv[0] + kIvLen);
    v.insert(v.end(), h.verifier[0], h.verifier[0] + 8);
    PutU64(v, h.sizeA);
    v.insert(v.end(), h.salt[1], h.salt[1] + kSaltLen);
    v.insert(v.end(), h.iv[1], h.iv[1] + kIvLen);
    v.insert(v.end(), h.verifier[1], h.verifier[1] + 8);
    PutU16(v, h.remaining);
    PutU64(v, h.lockUntil);
    return v;
}

// 错误信息用中文：两个前端都直接显示给用户
inline std::string HeaderErrorText(int code) {
    switch (code) {
    case 1: return "不是有效的加密文件（文件过小或已被截断）";
    case 2: return "不是有效的加密文件（文件标识不匹配）";
    case 3: return "不支持的加密文件版本";
    case 4: return "文件头中的迭代次数非法";
    case 5: return "加密文件不完整";
    default: return "加密文件头部损坏";
    }
}

// 解析 108 字节头部。返回 0 成功，否则是 HeaderErrorText 的错误码。
inline int ParseHeaderV3(const uint8_t* buf, size_t len, HeaderV3& out) {
    if (!buf || len < kHeaderLenV3) return 1;
    if (memcmp(buf, kMagic, 8) != 0) return 2;
    out = HeaderV3();
    out.version = buf[kOffVersion];
    if (out.version != 3) return 3;
    out.flags      = buf[kOffFlags];
    out.iterations = GetU32(buf + kOffIter);
    if (out.iterations < 1000 || out.iterations > 10000000) return 4;
    out.maxTries   = GetU16(buf + kOffMaxTries);
    out.lockDays   = GetU16(buf + kOffLockDays);
    memcpy(out.salt[0],     buf + kOffSaltA,     kSaltLen);
    memcpy(out.iv[0],       buf + kOffIvA,       kIvLen);
    memcpy(out.verifier[0], buf + kOffVerifierA, 8);
    out.sizeA      = GetU64(buf + kOffSizeA);
    memcpy(out.salt[1],     buf + kOffSaltB,     kSaltLen);
    memcpy(out.iv[1],       buf + kOffIvB,       kIvLen);
    memcpy(out.verifier[1], buf + kOffVerifierB, 8);
    out.remaining  = GetU16(buf + kRemOff);
    out.lockUntil  = GetU64(buf + kLockOff);
    return 0;
}

// 取 AAD（偏移 8..98，共 90 字节）
inline void CopyAad(const uint8_t* header, uint8_t* out) {
    memcpy(out, header + kAadOff, kAadLen);
}

// 就地改写可变字段（不需要密码也能改：防暴力破解计数）
inline void PatchMutable(uint8_t* header, uint16_t remaining, uint64_t lockUntil) {
    StoreU16(header + kRemOff, remaining);
    StoreU64(header + kLockOff, lockUntil);
}

} // namespace fe
