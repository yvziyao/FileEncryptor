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

// 密码输入/设置对话框。成功返回 true 并写出 UTF-8 密码。
// subtitle 非空时会在标题下方显示（用于批量处理时指明当前是哪个文件），
// 过长时按路径省略（保留文件名）。
bool UiShowPassword(HWND owner, bool confirm, std::string& outPassword,
                    const std::wstring& subtitle = std::wstring());

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
