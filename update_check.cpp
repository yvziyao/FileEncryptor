// update_check.cpp - 下载 Gitee 上最新的 FileEncryptor.exe，比较 sha256。
#include "update_check.h"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <vector>
#include <string>


#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

// 语言开关（公开 API）：定义在匿名命名空间之外，才能被 GUI/CLI 链接到
static bool g_updEnglish = false;

void UpdateCheckSetEnglish(bool en) { g_updEnglish = en; }

namespace {

// BCrypt 返回 NTSTATUS；不引 ntstatus.h，这里自己判非负
#define FE_NT_OK(st) (((NTSTATUS)(st)) >= 0)

// 这个文件同时被 GUI 和 CLI 使用，所以**不能**依赖 GUI 的 tr()。
// 语言由调用方通过 UpdateCheckSetEnglish() 指定。



static std::wstring T(const wchar_t* zh, const wchar_t* en) {
    return g_updEnglish ? en : zh;
}

const wchar_t* kGiteeApi =
    L"https://gitee.com/api/v5/repos/yvziyao/FileEncryptor/releases/latest";

struct HttpSession {
    HINTERNET ses = nullptr;
    HINTERNET con = nullptr;
    HttpSession() = default;
    ~HttpSession() {
        if (con) WinHttpCloseHandle(con);
        if (ses) WinHttpCloseHandle(ses);
    }
};

bool OpenSession(HttpSession& s, const wchar_t* url, std::wstring& host,
                 std::wstring& path, INTERNET_PORT& port, bool& https,
                 std::wstring& err) {
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t hbuf[256] = {};
    wchar_t pbuf[2048] = {};
    uc.lpszHostName = hbuf; uc.dwHostNameLength = _countof(hbuf);
    uc.lpszUrlPath = pbuf;  uc.dwUrlPathLength = _countof(pbuf);
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) {
        err = T(L"地址解析失败", L"Could not parse the URL");
        return false;
    }
    host = hbuf;
    path = pbuf;
    port = uc.nPort;
    https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    s.ses = WinHttpOpen(L"FileEncryptor/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s.ses) {
        err = T(L"无法初始化网络组件", L"Could not initialise networking");
        return false;
    }
    WinHttpSetTimeouts(s.ses, 5000, 5000, 5000, 15000);

    s.con = WinHttpConnect(s.ses, host.c_str(), port, 0);
    if (!s.con) {
        err = T(L"无法连接服务器", L"Could not connect to the server");
        return false;
    }
    return true;
}

HINTERNET OpenGet(HttpSession& s, const std::wstring& path, bool https) {
    HINTERNET req = WinHttpOpenRequest(s.con, L"GET", path.c_str(), NULL,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       https ? WINHTTP_FLAG_SECURE : 0);
    if (!req) return nullptr;
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        WinHttpCloseHandle(req);
        return nullptr;
    }
    DWORD code = 0, sz = sizeof(code);
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz,
                             WINHTTP_NO_HEADER_INDEX) || code != 200) {
        WinHttpCloseHandle(req);
        return nullptr;
    }
    return req;
}

bool HttpGetText(const wchar_t* url, std::string& body, std::wstring& err) {
    body.clear();
    HttpSession s;
    std::wstring host, path; INTERNET_PORT port = 0; bool https = false;
    if (!OpenSession(s, url, host, path, port, https, err)) return false;

    HINTERNET req = OpenGet(s, path, https);
    if (!req) {
        err = T(L"网络请求失败，请检查网络或代理设置",
                 L"Network request failed. Check your connection or proxy.");
        return false;
    }
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        if (body.size() > 4 * 1024 * 1024) break;   // 防御
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(req, buf.data(), avail, &read) || read == 0) break;
        body.append(buf.data(), read);
    }
    WinHttpCloseHandle(req);
    if (body.empty()) {
        err = T(L"服务器没有返回内容", L"The server returned no data");
        return false;
    }
    return true;
}

bool HttpDownload(const wchar_t* url, const std::wstring& outPath,
                  unsigned long long& size, std::wstring& err) {
    size = 0;
    HttpSession s;
    std::wstring host, path; INTERNET_PORT port = 0; bool https = false;
    if (!OpenSession(s, url, host, path, port, https, err)) return false;

    HINTERNET req = OpenGet(s, path, https);
    if (!req) {
        err = T(L"下载失败，请检查网络或代理设置",
                 L"Download failed. Check your connection or proxy.");
        return false;
    }

    HANDLE h = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        WinHttpCloseHandle(req);
        err = T(L"无法写入临时文件", L"Could not write a temporary file");
        return false;
    }

    bool ok = true;
    std::vector<char> buf(64 * 1024);
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        const DWORD want = (avail < buf.size()) ? avail : (DWORD)buf.size();
        DWORD read = 0;
        if (!WinHttpReadData(req, buf.data(), want, &read) || read == 0) break;
        DWORD written = 0;
        if (!WriteFile(h, buf.data(), read, &written, NULL) || written != read) {
            ok = false;
            break;
        }
        size += written;
        if (size > 64ull * 1024 * 1024) { ok = false; break; }   // 防御
    }
    CloseHandle(h);
    WinHttpCloseHandle(req);

    if (!ok || size == 0) {
        DeleteFileW(outPath.c_str());
        err = T(L"下载过程中出错", L"An error occurred while downloading");
        return false;
    }
    return true;
}

std::wstring Sha256OfFile(const std::wstring& path, unsigned long long* outSize = nullptr) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return L"";

    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    std::wstring result;
    DWORD objSize = 0, cb = 0, hashLen = 0;
    std::vector<BYTE> obj, dig;
    unsigned long long total = 0;

    do {
        if (!FE_NT_OK(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)))
            break;
        if (!FE_NT_OK(BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
                                        (PUCHAR)&objSize, sizeof(objSize), &cb, 0)))
            break;
        if (!FE_NT_OK(BCryptGetProperty(alg, BCRYPT_HASH_LENGTH,
                                        (PUCHAR)&hashLen, sizeof(hashLen), &cb, 0)))
            break;
        obj.resize(objSize);
        dig.resize(hashLen);
        if (!FE_NT_OK(BCryptCreateHash(alg, &hash, obj.data(), objSize, NULL, 0, 0)))
            break;

        std::vector<BYTE> buf(64 * 1024);
        DWORD read = 0;
        bool fail = false;
        for (;;) {
            if (!ReadFile(h, buf.data(), (DWORD)buf.size(), &read, NULL)) { fail = true; break; }
            if (read == 0) break;
            total += read;
            if (!FE_NT_OK(BCryptHashData(hash, buf.data(), read, 0))) { fail = true; break; }
        }
        if (fail) break;
        if (!FE_NT_OK(BCryptFinishHash(hash, dig.data(), hashLen, 0))) break;

        wchar_t hex[3] = {};
        for (DWORD i = 0; i < hashLen; ++i) {
            wsprintfW(hex, L"%02x", dig[i]);
            result += hex;
        }
    } while (false);

    if (hash) BCryptDestroyHash(hash);
    if (alg)  BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(h);
    if (outSize) *outSize = total;
    return result;
}

// 从 assets 数组里按 name 找 browser_download_url
std::wstring FindAssetUrl(const std::string& json, const std::string& wantName) {
    size_t p = 0;
    while ((p = json.find("\"name\"", p)) != std::string::npos) {
        const size_t colon = json.find(':', p);
        if (colon == std::string::npos) break;
        const size_t q1 = json.find('"', colon);
        if (q1 == std::string::npos) break;
        const size_t q2 = json.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        const std::string name = json.substr(q1 + 1, q2 - q1 - 1);
        p = q2 + 1;
        if (name != wantName) continue;

        // 同一个 asset 对象里 browser_download_url 出现在 name 之前
        const size_t keyPos = json.rfind("\"browser_download_url\"", p);
        if (keyPos == std::string::npos) continue;
        const size_t c2 = json.find(':', keyPos);
        if (c2 == std::string::npos) continue;
        const size_t u1 = json.find('"', c2);
        if (u1 == std::string::npos) continue;
        const size_t u2 = json.find('"', u1 + 1);
        if (u2 == std::string::npos) continue;
        const std::string url = json.substr(u1 + 1, u2 - u1 - 1);

        const int n = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), (int)url.size(), NULL, 0);
        if (n <= 0) continue;
        std::wstring w((size_t)n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, url.c_str(), (int)url.size(), &w[0], n);
        return w;
    }
    return L"";
}

std::wstring ExtractTagName(const std::string& json) {
    const std::string key = "\"tag_name\"";
    size_t p = json.find(key);
    if (p == std::string::npos) return L"";
    p = json.find(':', p + key.size());
    if (p == std::string::npos) return L"";
    p = json.find('"', p);
    if (p == std::string::npos) return L"";
    const size_t e = json.find('"', p + 1);
    if (e == std::string::npos) return L"";
    const std::string tag = json.substr(p + 1, e - p - 1);
    const int n = MultiByteToWideChar(CP_UTF8, 0, tag.c_str(), (int)tag.size(), NULL, 0);
    if (n <= 0) return L"";
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, tag.c_str(), (int)tag.size(), &w[0], n);
    return w;
}

} // namespace

UpdateCheckResult CheckForUpdate() {
    UpdateCheckResult r;

    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(NULL, self, _countof(self));
    r.localSha256 = Sha256OfFile(self, &r.localSize);
    if (r.localSha256.empty()) {
        r.error = T(L"无法读取本机程序文件", L"Could not read the local exe");
        return r;
    }

    std::string json;
    std::wstring err;
    if (!HttpGetText(kGiteeApi, json, err)) {
        r.error = err;
        return r;
    }
    r.tag = ExtractTagName(json);

    const std::wstring url = FindAssetUrl(json, "FileEncryptor.exe");
    if (url.empty()) {
        r.error = T(L"这个版本里没有找到 FileEncryptor.exe 附件",
                     L"FileEncryptor.exe was not found among the release assets");
        return r;
    }

    // Gitee 不提供远端 sha256，只能下下来自己算
    wchar_t tmpDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmpDir);
    const std::wstring tmpPath =
        std::wstring(tmpDir) + L"fe_update_" + std::to_wstring(GetCurrentProcessId()) + L".exe";

    if (!HttpDownload(url.c_str(), tmpPath, r.remoteSize, err)) {
        r.error = err;
        return r;
    }
    r.remoteSha256 = Sha256OfFile(tmpPath);

    if (r.remoteSha256.empty()) {
        DeleteFileW(tmpPath.c_str());
        r.error = T(L"无法校验下载的文件", L"Could not verify the downloaded file");
        return r;
    }

    r.ok = true;
    r.hasUpdate = (_wcsicmp(r.localSha256.c_str(), r.remoteSha256.c_str()) != 0);
    if (r.hasUpdate) {
        // 有新版本：文件留着，用户点"立即更新"时直接用它替换
        r.downloadedPath = tmpPath;
    }
    else {
        DeleteFileW(tmpPath.c_str());
    }
    return r;
}

bool ApplyUpdate(const std::wstring& newExePath, std::wstring& err) {
    if (GetFileAttributesW(newExePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = T(L"新版本文件不存在，请重新检查更新",
                 L"The new version file is missing; please check again");
        return false;
    }

    wchar_t self[MAX_PATH] = {};
    if (!GetModuleFileNameW(NULL, self, _countof(self))) {
        err = T(L"无法定位当前程序文件", L"Could not locate the current executable");
        return false;
    }

    // 运行中的 exe 不能覆盖，但**可以改名**。先把自己挪开，
    // 再把新文件放到原路径；新进程启动后就用到新版本了。
    const std::wstring backup = std::wstring(self) + L".old";
    DeleteFileW(backup.c_str());   // 可能存在的旧残留

    if (!MoveFileExW(self, backup.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        err = T(L"无法重命名当前程序文件（可能没有写入权限）",
                 L"Could not rename the current executable (no write permission?)");
        return false;
    }

    if (!MoveFileExW(newExePath.c_str(), self, MOVEFILE_REPLACE_EXISTING)) {
        // 放回原样，别把程序弄丢
        MoveFileExW(backup.c_str(), self, MOVEFILE_REPLACE_EXISTING);
        err = T(L"无法写入新的程序文件（可能没有写入权限）",
                 L"Could not write the new executable (no write permission?)");
        return false;
    }

    // .old 文件被老进程占用着，删不掉 → 排到重启后删除；
    // 正常情况下下次启动时 CleanupOldVersion() 就会把它清掉。
    MoveFileExW(backup.c_str(), NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    return true;
}

void CleanupOldVersion() {
    wchar_t self[MAX_PATH] = {};
    if (!GetModuleFileNameW(NULL, self, _countof(self))) return;

    // 上次更新留下的老版本：此时它没被任何进程占用，直接删
    const std::wstring backup = std::wstring(self) + L".old";
    DeleteFileW(backup.c_str());

    // 顺便清掉上次「检查更新」下载后没来得及删的临时文件
    // （比如用户在更新提示框上直接关掉了程序）
    wchar_t tmpDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmpDir);
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((std::wstring(tmpDir) + L"fe_update_*.exe").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            DeleteFileW((std::wstring(tmpDir) + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}
