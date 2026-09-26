// winui_dialog.h - WinUI 3 风格的自绘模态对话框层（密码框 / 消息框统一实现）。
//
// 全部在**本进程内**创建，因此可以：
//   * 与主窗口共用同一份语言状态（g_langEnglish / tr()），实现全局中英切换；
//   * 与主窗口共用同一份深/浅色主题（ui_theme）；
//   * 支持提示音与强制置顶。
#pragma once

#include <windows.h>
#include <string>

enum class UiIcon { None, Info, Warning, Error, Question };

struct UiMessage {
    std::wstring caption;
    std::wstring text;
    UiIcon       icon = UiIcon::Info;
    UINT         buttons = MB_OK;   // MB_OK / MB_OKCANCEL / MB_YESNO
    bool         topmost = false;   // 强制置顶，避免被其他窗口覆盖
    bool         sound = true;      // 显示时播放系统提示音
};

// 返回值：IDOK / IDYES / IDNO / IDCANCEL
int  UiShowMessage(HWND owner, const UiMessage& msg);

// 防暴力破解设置（仅加密时由用户在“设置密码”框中配置）
struct UiProtection {
    bool enabled = false;   // 默认关闭：避免用户无意中触发不可逆的删除
    int  maxTries = 3;      // 允许的错误尝试次数 1..9999
    int  action = 0;        // 0=永久删除文件 1=限时锁定
    int  lockDays = 7;      // 限时锁定的天数 0..3650
};

// 密码输入/设置对话框。成功返回 true 并写出 UTF-8 密码。
// subtitle 非空时会在标题下方显示（用于批量处理时指明当前是哪个文件），
// 过长时按路径省略（保留文件名）。
//
// confirm==true（加密）时：
//   showProtection 为真则显示防暴力破解设置区，ioProt 用于回填与读取。
// confirm==false（解密）时：
//   remainTries >= 0 会在密码输入框下方用红字显示“还能尝试 X 次”（受保护文件专用）；
//   传 -1 表示不显示（未启用保护的文件）。
bool UiShowPassword(HWND owner, bool confirm, std::string& outPassword,
                    const std::wstring& subtitle = std::wstring(),
                    bool showProtection = false,
                    UiProtection* ioProt = nullptr,
                    int remainTries = -1);

// 设置对话框的数据（取值与 app_settings 中的枚举一一对应）
struct UiSettingsData {
    int          language = 0;       // 0=中文 1=English
    int          theme = 0;          // 0=跟随系统 1=浅色 2=深色
    int          outDir = 0;         // 0=源文件同目录 1=指定目录 2=每次都询问
    std::wstring fixedDir;           // outDir==1 时生效
    int          secureDelete = 3;   // 0=禁用 1=覆写1次 3=覆写3次
    int          completion = 1;     // 0=关闭窗口 1=弹窗提示
    int          strength = 1;       // 0=快速 1=标准 2=安全 3=极强
};

// 打开设置对话框；用户点“确定”返回 true 并回填 io
bool UiShowSettings(HWND owner, UiSettingsData& io);
