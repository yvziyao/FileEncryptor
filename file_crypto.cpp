// file_crypto.cpp - 加解密核心的唯一实现（CLI 与 GUI 共用）
#include "file_crypto.h"
#include "file_format.h"

#include <windows.h>
#include <bcrypt.h>     // Windows CNG：系统自带的密码学实现

#include <ctime>
#include <memory>
#include <stdexcept>
#include <vector>

// 密码学运算全部使用 Windows 自带的 CNG（bcrypt）：
//   * 不使用任何第三方库 —— 只需要 Windows SDK 就能构建
//   * CNG 是系统组件，始终存在、经过 FIPS 验证
//   * 算法全部是标准实现（AES-GCM / PBKDF2-HMAC-SHA256 / HMAC-SHA256），
//     所以**文件格式与密文均不受影响**，旧版本加密的文件仍可解开
#pragma comment(lib, "bcrypt.lib")

namespace fe {
namespace {

struct HandleGuard {
    HANDLE h = INVALID_HANDLE_VALUE;
    HandleGuard() = default;
    explicit HandleGuard(HANDLE x) : h(x) {}
    ~HandleGuard() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
};

struct AlgGuard {
    AlgGuard() = default;
    BCRYPT_ALG_HANDLE h = NULL;
    ~AlgGuard() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
    AlgGuard(const AlgGuard&) = delete;
    AlgGuard& operator=(const AlgGuard&) = delete;
};
struct HashGuard {
    HashGuard() = default;
    BCRYPT_HASH_HANDLE h = NULL;
    ~HashGuard() { if (h) BCryptDestroyHash(h); }
    HashGuard(const HashGuard&) = delete;
    HashGuard& operator=(const HashGuard&) = delete;
};
struct KeyGuard {
    KeyGuard() = default;
    BCRYPT_KEY_HANDLE h = NULL;
    ~KeyGuard() { if (h) BCryptDestroyKey(h); }
    KeyGuard(const KeyGuard&) = delete;
    KeyGuard& operator=(const KeyGuard&) = delete;
};

void NtCheck(NTSTATUS st, const char* what) {
    if (st != 0) throw std::runtime_error(what);
}

// AES-256-GCM 的流式实现。
//
// 为什么要自己实现：**CNG 的 GCM 不支持分块**。实测 BCRYPT_AUTH_MODE_CHAIN_CALLS_FLAG
// 在任何 cbMacContext 取值（1..600 全试过）下都返回 STATUS_INVALID_BUFFER_SIZE，
// 而我们要处理 10GB 级别的文件，不可能一次性放进内存。
//
// 所以这里按 NIST SP 800-38D 自行拼装 GCM 的 CTR 计数与 GHASH，
// 但 **AES 分组运算仍由 CNG 提供**（FIPS 验证的核心）。
// 正确性由测试保证：分块结果与 CNG 单次调用的 GCM **逐字节一致**。
class GcmStream {
public:
    GcmStream(bool encrypt, const std::vector<uint8_t>& key,
              const uint8_t* iv, const uint8_t* aad, size_t aadLen)
        : enc_(encrypt) {
        NtCheck(BCryptOpenAlgorithmProvider(&alg_.h, BCRYPT_AES_ALGORITHM, NULL, 0),
                "打开AES算法失败");
        NtCheck(BCryptSetProperty(alg_.h, BCRYPT_CHAINING_MODE,
                                  (PUCHAR)BCRYPT_CHAIN_MODE_ECB,
                                  sizeof(BCRYPT_CHAIN_MODE_ECB), 0),
                "设置ECB模式失败");
        DWORD objLen = 0, got = 0;
        NtCheck(BCryptGetProperty(alg_.h, BCRYPT_OBJECT_LENGTH,
                                  (PUCHAR)&objLen, sizeof(objLen), &got, 0),
                "获取AES对象长度失败");
        obj_.resize(objLen);
        NtCheck(BCryptGenerateSymmetricKey(alg_.h, &key_.h, obj_.data(), objLen,
                                           (PUCHAR)key.data(), (ULONG)key.size(), 0),
                "生成AES密钥失败");

        uint8_t zero[16] = {};
        AesBlock(zero, H_);                 // H = E(K, 0^128)

        memset(J0_, 0, sizeof(J0_));        // J0 = IV || 0^31 || 1
        memcpy(J0_, iv, kIvLen);
        J0_[15] = 1;

        memcpy(Ctr_, J0_, 16);              // 数据从 J0+1 开始
        Inc32(Ctr_);

        aadBits_ = (uint64_t)aadLen * 8;    // AAD 是 GHASH 的第一段
        Feed(aad, aadLen);
        EndSection();
    }

    void Update(const uint8_t* in, size_t len, uint8_t* out) {
        for (size_t i = 0; i < len; ++i) {
            if (keystreamLeft_ == 0) {
                AesBlock(Ctr_, ks_);
                Inc32(Ctr_);
                ksPos_ = 0;
                keystreamLeft_ = 16;
            }
            out[i] = (uint8_t)(in[i] ^ ks_[ksPos_]);
            ++ksPos_;
            --keystreamLeft_;
        }
        if (len) {
            dataBits_ += (uint64_t)len * 8;
            Feed(enc_ ? out : in, len);     // GHASH 覆盖的是密文
        }
    }

    void FinishEncrypt(uint8_t* tagOut) {
        memcpy(tagOut, ComputeTag(), kTagLen);
    }

    bool FinishDecrypt(const uint8_t* tagIn) {
        const uint8_t* t = ComputeTag();
        uint8_t diff = 0;
        for (size_t i = 0; i < kTagLen; ++i) diff |= (uint8_t)(t[i] ^ tagIn[i]);
        return diff == 0;
    }

private:
    void AesBlock(const uint8_t* in, uint8_t* out) {
        ULONG done = 0;
        NtCheck(BCryptEncrypt(key_.h, (PUCHAR)in, 16, NULL, NULL, 0,
                              out, 16, &done, 0), "AES运算失败");
    }

    static void Inc32(uint8_t c[16]) {
        for (int i = 15; i >= 12; --i) {
            if (++c[i] != 0) break;
        }
    }

    // Y = (Y ^ block) * H，NIST SP 800-38D Algorithm 1 的逐位实现
    void GfMulAdd(const uint8_t block[16]) {
        for (int i = 0; i < 16; ++i) Y_[i] ^= block[i];
        uint8_t Z[16] = {};
        uint8_t V[16];
        memcpy(V, H_, 16);
        for (int i = 0; i < 128; ++i) {
            if ((Y_[i >> 3] >> (7 - (i & 7))) & 1) {
                for (int j = 0; j < 16; ++j) Z[j] ^= V[j];
            }
            const uint8_t lsb = (uint8_t)(V[15] & 1);
            for (int j = 15; j > 0; --j) {
                V[j] = (uint8_t)((V[j] >> 1) | ((V[j - 1] & 1) << 7));
            }
            V[0] >>= 1;
            if (lsb) V[0] ^= 0xE1;
        }
        memcpy(Y_, Z, 16);
    }

    void Feed(const uint8_t* p, size_t n) {
        size_t i = 0;
        while (i < n) {
            const size_t take = (16 - bufLen_ < n - i) ? (16 - bufLen_) : (n - i);
            memcpy(buf_ + bufLen_, p + i, take);
            bufLen_ += take;
            i += take;
            if (bufLen_ == 16) {
                GfMulAdd(buf_);
                bufLen_ = 0;
            }
        }
    }

    void EndSection() {
        if (bufLen_ > 0) {
            memset(buf_ + bufLen_, 0, 16 - bufLen_);
            GfMulAdd(buf_);
            bufLen_ = 0;
        }
    }

    const uint8_t* ComputeTag() {
        EndSection();
        uint8_t lenBlock[16];
        for (int i = 0; i < 8; ++i) lenBlock[i]     = (uint8_t)((aadBits_  >> (8 * (7 - i))) & 0xFF);
        for (int i = 0; i < 8; ++i) lenBlock[8 + i] = (uint8_t)((dataBits_ >> (8 * (7 - i))) & 0xFF);
        GfMulAdd(lenBlock);

        uint8_t ek[16];
        AesBlock(J0_, ek);
        for (int i = 0; i < 16; ++i) tag_[i] = (uint8_t)(Y_[i] ^ ek[i]);
        return tag_;
    }

    bool enc_;
    AlgGuard alg_;
    KeyGuard key_;
    std::vector<uint8_t> obj_;
    uint8_t H_[16]  = {};
    uint8_t J0_[16] = {};
    uint8_t Ctr_[16] = {};
    uint8_t ks_[16] = {};
    size_t  ksPos_ = 0;
    size_t  keystreamLeft_ = 0;
    uint8_t Y_[16] = {};
    uint8_t buf_[16] = {};
    size_t  bufLen_ = 0;
    uint64_t aadBits_ = 0;
    uint64_t dataBits_ = 0;
    uint8_t tag_[16] = {};
};
const size_t kChunk = 4 * 1024 * 1024;   // 4MB

// 测试用槽位固定值，-1 = 随机
int g_fixedSlot = -1;

void RandBytes(void* buf, size_t len) {
    if (len == 0) return;
    NtCheck(BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len,
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG), "随机数生成失败");
}

uint8_t RandByte() {
    uint8_t b = 0;
    RandBytes(&b, 1);
    return b;
}


std::vector<uint8_t> DeriveKey(const std::string& password,
                               const uint8_t* salt, size_t saltLen, int iterations) {
    AlgGuard alg;
    // PBKDF2 需要一个带 HMAC 标志的 SHA-256 提供程序
    NtCheck(BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_SHA256_ALGORITHM, NULL,
                                        BCRYPT_ALG_HANDLE_HMAC_FLAG),
            "打开SHA256失败");
    std::vector<uint8_t> key(kKeyLen);
    NtCheck(BCryptDeriveKeyPBKDF2(alg.h,
                                  (PUCHAR)password.data(), (ULONG)password.size(),
                                  (PUCHAR)salt, (ULONG)saltLen,
                                  (ULONGLONG)iterations,
                                  key.data(), (ULONG)key.size(), 0),
            "密钥派生失败");
    return key;
}

std::vector<uint8_t> DeriveKey(const std::string& password,
                               const uint8_t (&salt)[kSaltLen], int iterations) {
    return DeriveKey(password, salt, kSaltLen, iterations);
}

std::vector<uint8_t> VerifierOf(const std::vector<uint8_t>& key) {
    AlgGuard alg;
    NtCheck(BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_SHA256_ALGORITHM, NULL,
                                        BCRYPT_ALG_HANDLE_HMAC_FLAG),
            "打开HMAC失败");
    HashGuard h;
    NtCheck(BCryptCreateHash(alg.h, &h.h, NULL, 0,
                             (PUCHAR)key.data(), (ULONG)key.size(), 0),
            "创建HMAC失败");
    NtCheck(BCryptHashData(h.h, (PUCHAR)kVerifyLabel,
                           (ULONG)(sizeof(kVerifyLabel) - 1), 0), "计算HMAC失败");
    uint8_t mac[32] = {};
    NtCheck(BCryptFinishHash(h.h, mac, sizeof(mac), 0), "完成HMAC失败");
    return std::vector<uint8_t>(mac, mac + 8);
}

// 常数时间比较，避免通过比较耗时泄露「前几字节对不对」
bool ConstantTimeEq(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

uint64_t FileSizeOf(HANDLE h) {
    LARGE_INTEGER li = {};
    if (!GetFileSizeEx(h, &li)) throw std::runtime_error("获取文件大小失败");
    return (uint64_t)li.QuadPart;
}

void WriteAll(HANDLE h, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    while (len > 0) {
        DWORD bw = 0;
        DWORD chunk = (DWORD)(len > 0x40000000u ? 0x40000000u : len);
        if (!WriteFile(h, p, chunk, &bw, NULL)) throw std::runtime_error("写入文件失败");
        if (bw == 0) throw std::runtime_error("写入文件失败");
        p += bw;
        len -= bw;
    }
}

void ReadAll(HANDLE h, void* data, size_t len) {
    uint8_t* p = (uint8_t*)data;
    while (len > 0) {
        DWORD br = 0;
        DWORD chunk = (DWORD)(len > 0x40000000u ? 0x40000000u : len);
        if (!ReadFile(h, p, chunk, &br, NULL) || br == 0)
            throw std::runtime_error("加密文件不完整");
        p += br;
        len -= br;
    }
}

void SeekTo(HANDLE h, uint64_t off) {
    LARGE_INTEGER li = {};
    li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) throw std::runtime_error("定位文件失败");
}

// 进度上报：调用方可能传空
struct Progress {
    const ProgressFn* fn = nullptr;
    uint64_t done = 0;
    uint64_t total = 0;
    void Report(uint64_t add) {
        done += add;
        if (fn && *fn) (*fn)(done, total);
    }
    void ReportAbsolute() {
        if (fn && *fn) (*fn)(done, total);
    }
};

// 用 AES-256-GCM 加密 src 的全部内容写入 hOut（末尾附 tag）
void EncryptStream(HANDLE hSrc, HANDLE hOut, const std::vector<uint8_t>& key,
                   const uint8_t* iv, const uint8_t* aad, size_t aadLen,
                   Progress& prog, bool reportProgress) {
    GcmStream gcm(true, key, iv, aad, aadLen);

    std::vector<uint8_t> inbuf(kChunk), outbuf(kChunk + 32);
    DWORD br = 0;
    while (ReadFile(hSrc, inbuf.data(), (DWORD)inbuf.size(), &br, NULL) && br > 0) {
        gcm.Update(inbuf.data(), (size_t)br, outbuf.data());
        WriteAll(hOut, outbuf.data(), (size_t)br);
        if (reportProgress) prog.Report(br);
    }

    uint8_t tag[kTagLen] = {};
    gcm.FinishEncrypt(tag);
    WriteAll(hOut, tag, kTagLen);
}

// 写入 len 字节随机数据，充当「没有诱饵时」的假槽位密文段。
// 用 CSPRNG 而不是重复填充：重复模式会被看出这是填充。
void WriteRandomSegment(HANDLE hOut, uint64_t len, Progress& prog) {
    std::vector<uint8_t> buf(64 * 1024);
    while (len > 0) {
        size_t n = (size_t)(len < buf.size() ? len : buf.size());
        RandBytes(buf.data(), n);
        WriteAll(hOut, buf.data(), n);
        len -= n;
        prog.Report(n);
    }
}

// 解密一个槽的密文段到 hOut
void DecryptSegment(HANDLE hIn, uint64_t offset, uint64_t cipherLen,
                    const std::vector<uint8_t>& key, const uint8_t* iv,
                    const uint8_t* aad, size_t aadLen, HANDLE hOut, Progress& prog) {
    if (cipherLen < kTagLen) throw std::runtime_error("加密文件不完整");   // 空明文的密文体只剩 tag，允许等于 kTagLen
    const uint64_t bodyLen = cipherLen - kTagLen;

    GcmStream gcm(false, key, iv, aad, aadLen);

    // 密文末尾才是 tag：先读出来交给 CNG，收尾调用时会校验它
    SeekTo(hIn, offset + bodyLen);
    uint8_t tag[kTagLen] = {};
    ReadAll(hIn, tag, kTagLen);
    SeekTo(hIn, offset);
    std::vector<uint8_t> inbuf(kChunk), outbuf(kChunk + 32);
    uint64_t remaining = bodyLen;
    while (remaining > 0) {
        size_t want = (size_t)(remaining < (uint64_t)inbuf.size() ? remaining : (uint64_t)inbuf.size());
        ReadAll(hIn, inbuf.data(), want);
        gcm.Update(inbuf.data(), want, outbuf.data());
        WriteAll(hOut, outbuf.data(), want);
        remaining -= want;
        prog.Report(want);
    }

    // 校验 tag：不通过说明密码错误或文件被篡改
    if (!gcm.FinishDecrypt(tag))
        throw std::runtime_error("认证失败：密码错误或文件已被篡改");
}

} // namespace

// ---- 随机密码生成（对外导出，GUI 与 CLI 共用）----
// 从字符集里取一个字符。用拒绝采样而不是 rand()%n，
// 否则字符集长度不是 256 的约数时会出现取模偏差（某些字符更常出现）。
static char PickFrom(const char* set) {
    const size_t n = strlen(set);
    if (n == 0) return '\0';
    // 注意用 size_t：若写成 uint8_t，(256 - 0) 会被截断成 0 而变成死循环
    const size_t limit = 256u - (256u % n);
    uint8_t b = 0;
    do { RandBytes(&b, 1); } while ((size_t)b >= limit);
    return set[b % n];
}

std::string GeneratePassword(const PasswordRules& r) {
    static const char* kUpper   = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char* kLower   = "abcdefghijklmnopqrstuvwxyz";
    static const char* kDigits  = "0123456789";
    // 特殊符号里刻意避开引号、反斜杠、空格、$ 等会折磨命令行和配置文件的字符
    static const char* kSymbols = "!@#%^&*()-_=+[]{}:;,.?/";

    std::string pool;
    if (r.upper)   pool += kUpper;
    if (r.lower)   pool += kLower;
    if (r.digits)  pool += kDigits;
    if (r.symbols) pool += kSymbols;
    if (pool.empty()) pool = kLower;              // 全都不选时兜底：小写

    int len = r.length;
    if (len < 4)   len = 4;
    if (len > 128) len = 128;

    std::string out;
    // 每一类先各出一个，保证不会出现"选了数字但一个都没有"
    if (r.upper)   out += PickFrom(kUpper);
    if (r.lower)   out += PickFrom(kLower);
    if (r.digits)  out += PickFrom(kDigits);
    if (r.symbols) out += PickFrom(kSymbols);

    if ((int)out.size() > len) out.resize(len);
    while ((int)out.size() < len) out += PickFrom(pool.c_str());

    // 打乱：否则前几个字符的类别是固定的
    for (size_t i = out.size(); i > 1; --i) {
        const size_t n = i;
        const size_t limit = 256u - (256u % n);
        uint8_t b = 0;
        do { RandBytes(&b, 1); } while ((size_t)b >= limit);
        std::swap(out[i - 1], out[b % n]);
    }
    return out;
}

uint64_t NowUnix() { return (uint64_t)std::time(nullptr); }

void SetSlotChoiceForTest(int fixedSlot) { g_fixedSlot = fixedSlot; }

// ---------------------------------------------------------------- 加密

void EncryptFileTo(const std::wstring& in, const std::wstring& out,
                   const std::string& password, int iterations,
                   const Protection& prot,
                   const std::wstring& decoyPath, const std::string& decoyPassword,
                   const ProgressFn& progress, bool singleSlot) {
    if (iterations <= 0) iterations = 100000;
    // 单槽模式：不写第二段（体积回到 1×），此时诱饵无意义
    const bool hasDecoy = !singleSlot && !decoyPath.empty();

    HandleGuard hIn(CreateFileW(in.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
    if (hIn.h == INVALID_HANDLE_VALUE) throw std::runtime_error("无法打开输入文件");
    const uint64_t inSize = FileSizeOf(hIn.h);

    HandleGuard hDecoy;
    uint64_t decoySize = 0;
    if (hasDecoy) {
        hDecoy.h = CreateFileW(decoyPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hDecoy.h == INVALID_HANDLE_VALUE) throw std::runtime_error("无法打开诱饵文件");
        decoySize = FileSizeOf(hDecoy.h);
    }

    HandleGuard hOut(CreateFileW(out.c_str(), GENERIC_WRITE, 0, NULL,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL));
    if (hOut.h == INVALID_HANDLE_VALUE) throw std::runtime_error("无法创建输出文件");

    // 两个槽的密文长度（含 tag）
    const uint64_t realCipherLen = inSize + kTagLen;
    uint64_t otherCipherLen = 0;
    if (singleSlot) {
        otherCipherLen = 0;              // 只写槽 A
    } else if (hasDecoy) {
        otherCipherLen = decoySize + kTagLen;
    } else {
        // 没有诱饵：另一槽填随机数据，长度取真实密文的 50%..150%，
        // 免得"另一段总是同一个长度"成为可识别特征
        uint32_t r = 0;
        RandBytes(&r, sizeof(r));
        const double f = 0.5 + (double)(r % 1001) / 1000.0;
        otherCipherLen = (uint64_t)((double)realCipherLen * f);
        if (otherCipherLen < kTagLen + 16) otherCipherLen = kTagLen + 16;
    }

    // 真实载荷进哪个槽：随机。这是"无法从文件判断哪个槽是真"的关键 ——
    // 如果真文件永远在槽 A，统计规律就能把它认出来。
    // 单槽模式固定用槽 A；否则随机决定真实载荷进哪个槽
    const int realSlot = singleSlot
        ? 0
        : ((g_fixedSlot >= 0) ? (g_fixedSlot & 1) : (RandByte() & 1));
    const int otherSlot = 1 - realSlot;

    HeaderV3 h;
    h.version    = 3;
    h.flags      = prot.enabled
        ? (uint8_t)(kFlagProtected | (prot.action == EXHAUST_LOCK ? kFlagModeLock : 0))
        : (uint8_t)0;
    h.iterations = (uint32_t)iterations;
    h.maxTries   = (uint16_t)(prot.enabled ? prot.maxTries : 0);
    h.lockDays   = (uint16_t)(prot.enabled && prot.action == EXHAUST_LOCK ? prot.lockDays : 0);
    h.remaining  = (uint16_t)(prot.enabled ? prot.maxTries : 0);
    h.lockUntil  = 0;
    h.sizeA      = (realSlot == 0) ? realCipherLen : otherCipherLen;

    for (int s = 0; s < kSlotCount; ++s) {
        RandBytes(h.salt[s], kSaltLen);
        RandBytes(h.iv[s], kIvLen);
    }

    // 每槽一把密钥。没有诱饵时，另一槽用随机密钥 —— 它的 verifier 也随机，
    // 与"真 verifier"在统计上不可区分。
    std::vector<uint8_t> key[kSlotCount];
    key[realSlot] = DeriveKey(password, h.salt[realSlot], iterations);
    if (hasDecoy) {
        key[otherSlot] = DeriveKey(decoyPassword, h.salt[otherSlot], iterations);
    } else {
        key[otherSlot].resize(kKeyLen);
        RandBytes(key[otherSlot].data(), kKeyLen);
    }
    for (int s = 0; s < kSlotCount; ++s) {
        std::vector<uint8_t> v = VerifierOf(key[s]);
        memcpy(h.verifier[s], v.data(), 8);
    }

    const std::vector<uint8_t> header = SerializeHeaderV3(h);
    WriteAll(hOut.h, header.data(), header.size());

    uint8_t aad[kAadLen] = {};
    CopyAad(header.data(), aad);

    Progress prog;
    prog.fn = &progress;
    prog.total = realCipherLen + otherCipherLen;
    prog.ReportAbsolute();

    for (int slot = 0; slot < kSlotCount; ++slot) {
        if (singleSlot && slot != realSlot) break;   // 单槽模式：不写第二段
        if (slot == realSlot) {
            EncryptStream(hIn.h, hOut.h, key[slot], h.iv[slot], aad, kAadLen, prog, true);
        } else if (hasDecoy) {
            // 诱饵也是真加密的，只是用假密码的密钥
            EncryptStream(hDecoy.h, hOut.h, key[slot], h.iv[slot], aad, kAadLen, prog, true);
        } else {
            WriteRandomSegment(hOut.h, otherCipherLen, prog);
        }
    }
    prog.ReportAbsolute();
}

// ---------------------------------------------------------------- 解密

void DecryptFileTo(const std::wstring& in, const std::wstring& out,
                   const std::string& password, const ProgressFn& progress) {
    HandleGuard hIn(CreateFileW(in.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
    if (hIn.h == INVALID_HANDLE_VALUE) throw std::runtime_error("无法打开加密文件");
    const uint64_t fileSize = FileSizeOf(hIn.h);

    uint8_t hdr[kHeaderLenV3] = {};
    ReadAll(hIn.h, hdr, kHeaderLenV3);

    HeaderV3 h;
    const int perr = ParseHeaderV3(hdr, sizeof(hdr), h);
    if (perr) throw std::runtime_error(HeaderErrorText(perr));

    if (fileSize <= (uint64_t)kHeaderLenV3) throw std::runtime_error(HeaderErrorText(5));
    const uint64_t payloadTotal = fileSize - (uint64_t)kHeaderLenV3;
    if (h.sizeA < kTagLen || h.sizeA > payloadTotal) throw std::runtime_error(HeaderErrorText(5));

    uint8_t aad[kAadLen] = {};
    CopyAad(hdr, aad);

    // 依次尝试两个槽。**顺序固定，但槽位内容是随机分配的**，
    // 所以"先命中 A"不代表 A 是真的 —— 这个函数也不对外暴露是第几个槽。
    for (int slot = 0; slot < kSlotCount; ++slot) {
        std::vector<uint8_t> key = DeriveKey(password, h.salt[slot], (int)h.iterations);
        std::vector<uint8_t> ver = VerifierOf(key);
        if (!ConstantTimeEq(ver.data(), h.verifier[slot], 8)) continue;

        const uint64_t offset = (uint64_t)kHeaderLenV3 + (slot == 0 ? 0 : h.sizeA);
        const uint64_t len    = (slot == 0) ? h.sizeA : (payloadTotal - h.sizeA);
        if (len < kTagLen) throw std::runtime_error(HeaderErrorText(5));

        HandleGuard hOut(CreateFileW(out.c_str(), GENERIC_WRITE, 0, NULL,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL));
        if (hOut.h == INVALID_HANDLE_VALUE) throw std::runtime_error("无法创建输出文件");

        Progress prog;
        prog.fn = &progress;
        prog.total = len;
        prog.ReportAbsolute();

        try {
            DecryptSegment(hIn.h, offset, len, key, h.iv[slot], aad, kAadLen, hOut.h, prog);
        }
        catch (...) {
            // 不要留下残破的输出文件：先关掉句柄（并阻止析构重复关闭），再删
            HANDLE raw = hOut.h;
            hOut.h = INVALID_HANDLE_VALUE;
            if (raw != INVALID_HANDLE_VALUE) CloseHandle(raw);
            DeleteFileW(out.c_str());
            throw;
        }
        prog.ReportAbsolute();
        return;
    }

    throw std::runtime_error("认证失败：密码错误或文件已被篡改");
}

// ---------------------------------------------------------------- 保护状态

bool ReadProtState(const std::wstring& path, FileState& out) {
    HandleGuard h(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
    if (h.h == INVALID_HANDLE_VALUE) return false;
    uint8_t hdr[kHeaderLenV3] = {};
    DWORD got = 0;
    if (!ReadFile(h.h, hdr, (DWORD)kHeaderLenV3, &got, NULL) || got != kHeaderLenV3)
        return false;
    HeaderV3 parsed;
    if (ParseHeaderV3(hdr, sizeof(hdr), parsed) != 0) return false;

    out.isProtected = parsed.isProtected();
    out.action      = parsed.isLockMode() ? EXHAUST_LOCK : EXHAUST_DELETE;
    out.maxTries    = (int)parsed.maxTries;
    out.lockDays    = (int)parsed.lockDays;
    out.remaining   = (int)parsed.remaining;
    out.lockUntil   = parsed.lockUntil;
    return true;
}

bool WriteMutableState(const std::wstring& path, int remaining, uint64_t lockUntil) {
    HandleGuard h(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
    if (h.h == INVALID_HANDLE_VALUE) return false;
    uint8_t buf[10];
    StoreU16(buf, (uint16_t)remaining);
    StoreU64(buf + 2, lockUntil);
    SeekTo(h.h, kRemOff);
    DWORD bw = 0;
    if (!WriteFile(h.h, buf, (DWORD)sizeof(buf), &bw, NULL) || bw != sizeof(buf)) return false;
    FlushFileBuffers(h.h);
    return true;
}

VerifyResult VerifyPassword(const std::wstring& path, const std::string& password, FileState& out) {
    HandleGuard h(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
    if (h.h == INVALID_HANDLE_VALUE) return VerifyResult::Unavailable;
    uint8_t hdr[kHeaderLenV3] = {};
    DWORD got = 0;
    if (!ReadFile(h.h, hdr, (DWORD)kHeaderLenV3, &got, NULL) || got != kHeaderLenV3)
        return VerifyResult::Unavailable;

    HeaderV3 parsed;
    if (ParseHeaderV3(hdr, sizeof(hdr), parsed) != 0) return VerifyResult::Unavailable;

    out.isProtected = parsed.isProtected();
    out.action      = parsed.isLockMode() ? EXHAUST_LOCK : EXHAUST_DELETE;
    out.maxTries    = (int)parsed.maxTries;
    out.lockDays    = (int)parsed.lockDays;
    out.remaining   = (int)parsed.remaining;
    out.lockUntil   = parsed.lockUntil;

    // 两个槽都试：用哪个密码解都算密码正确
    for (int slot = 0; slot < kSlotCount; ++slot) {
        std::vector<uint8_t> key = DeriveKey(password, parsed.salt[slot], (int)parsed.iterations);
        std::vector<uint8_t> ver = VerifierOf(key);
        if (ConstantTimeEq(ver.data(), parsed.verifier[slot], 8)) return VerifyResult::Ok;
    }
    return VerifyResult::WrongPassword;
}

std::string FormatDuration(uint64_t seconds) {
    const uint64_t days = seconds / 86400;
    const uint64_t hours = (seconds % 86400) / 3600;
    const uint64_t mins = (seconds % 3600) / 60;
    std::string s;
    if (days > 0)  s += std::to_string(days) + " 天 ";
    if (hours > 0) s += std::to_string(hours) + " 小时 ";
    if (days == 0 && mins > 0) s += std::to_string(mins) + " 分钟";
    if (s.empty()) s = "不到 1 分钟";
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

int ConsumeAttempt(const std::wstring& path, const FileState& st, std::string& outMsg) {
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

    if (st.action == EXHAUST_LOCK) {
        const uint64_t until = NowUnix() + (uint64_t)st.lockDays * 86400ULL;
        WriteMutableState(path, 0, until);
        outMsg = "错误次数已达上限，已禁止解密 " + std::to_string(st.lockDays) + " 天";
        return 0;
    }

    DeleteFileW(path.c_str());
    outMsg = "错误次数已达上限，加密文件已被永久删除";
    return 0;
}

// ---------------------------------------------------------------- 安全删除

void SecureDeleteFile(const std::wstring& path, int passes, const ProgressFn& progress) {
    if (passes <= 0) {
        DeleteFileW(path.c_str());
        return;
    }
    try {
        HandleGuard h(CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
        if (h.h != INVALID_HANDLE_VALUE) {
            const uint64_t fileSize = FileSizeOf(h.h);
            if (fileSize > 0) {
                std::vector<uint8_t> buf(64 * 1024);
                uint64_t done = 0;
                const uint64_t total = fileSize * (uint64_t)passes;
                for (int pass = 0; pass < passes; ++pass) {
                    SeekTo(h.h, 0);
                    uint64_t remaining = fileSize;
                    while (remaining > 0) {
                        size_t n = (size_t)(remaining < (uint64_t)buf.size() ? remaining : (uint64_t)buf.size());
                        RandBytes(buf.data(), n);
                        // 交替写入随机数据与随机数据的反码，覆盖更彻底
                        if (pass % 2 == 1) {
                            for (size_t i = 0; i < n; ++i) buf[i] = (uint8_t)~buf[i];
                        }
                        DWORD bw = 0;
                        if (!WriteFile(h.h, buf.data(), (DWORD)n, &bw, NULL) || bw == 0) break;
                        remaining -= bw;
                        done += bw;
                        if (progress) progress(done, total);
                    }
                    FlushFileBuffers(h.h);
                }
            }
        }
    }
    catch (...) {
        // best-effort：无论如何都要尝试删除
    }
    DeleteFileW(path.c_str());
}

} // namespace fe
