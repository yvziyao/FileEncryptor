// update_check.h - 检查更新（下载远端构建、比较内容哈希）。
//
// 为什么不用版本号比较：发布页的 tag 一直是 v1.0.0（滚动更新，每次提交覆盖同一
// 个 Release）。tag 不变 ⇒ 版本号比较永远认为"已是最新"，完全失效。
//
// 为什么必须下载：Gitee 的 Release API 只给出附件的 name / browser_download_url，
// **不提供 size、sha256 或时间戳**。除了把文件拿下来算哈希，没有别的可靠信号。
// 单个 exe 约 700 KB，对一次手动点击来说可以接受。
//
// 于是：取最新 Release 里 FileEncryptor.exe 的下载地址 → 下到临时目录 →
// 算 sha256 → 与本机 exe 的 sha256 比较。相同即最新，不同即有更新。
#pragma once

#include <string>

// 仅用于显示（滚动更新下 tag 是固定的 v1.0.0，不参与判断）
#define FE_APP_VERSION L"1.0.0"

struct UpdateCheckResult {
    bool         ok = false;           // 网络/下载/解析是否成功
    bool         hasUpdate = false;    // 远端内容与本机不同
    std::wstring tag;                  // 远端 Release tag（仅显示）
    std::wstring localSha256;          // 本机 exe 的 sha256
    std::wstring remoteSha256;         // 远端 exe 的 sha256
    unsigned long long remoteSize = 0; // 远端 exe 字节数
    unsigned long long localSize = 0;  // 本机 exe 字节数
    std::wstring error;                // ok==false 时的原因（已本地化）
    // 下载到本地的远端 exe（hasUpdate 时保留，供 ApplyUpdate 直接替换；否则为空）
    std::wstring downloadedPath;
};

// 用下载好的新版本替换当前 exe。
//   做法：把自己改名成 <exe>.old（运行中的 exe 允许改名），再把新文件放到原位置，
//   然后调用方重启进程；.old 会在下次启动时被清理（或重启后删除）。
// 失败时 err 给出原因（例如安装在 Program Files 但没有管理员权限）。
bool ApplyUpdate(const std::wstring& newExePath, std::wstring& err);

// 启动时清理上次更新留下的 <exe>.old
void CleanupOldVersion();

// 错误信息用中文还是英文（GUI 传当前界面语言；CLI 传 !g_zh）
void UpdateCheckSetEnglish(bool en);

// 同步执行（会在后台线程里调用；内部超时约 5 秒/步）
UpdateCheckResult CheckForUpdate();
