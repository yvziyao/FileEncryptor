// ui_theme.h - WinUI 3 风格主题层：深/浅色检测、调色板、字体、DWM 圆角与深色标题栏。
#pragma once

#include <windows.h>
#include <string>

// WinUI 3 风格调色板
struct UiPalette {
    bool     dark;

    COLORREF bg;              // 窗体/卡片背景
    COLORREF bgAlt;           // 输入框、次级表面
    COLORREF text;            // 主文本
    COLORREF textMuted;       // 次级文本
    COLORREF border;          // 边框/分隔线
    COLORREF control;         // 标准按钮底色
    COLORREF controlHover;
    COLORREF controlPressed;
    COLORREF accent;          // 强调色（主按钮）
    COLORREF accentHover;
    COLORREF accentPressed;
    COLORREF onAccent;        // 强调色上的文本
    COLORREF danger;          // 错误文本
    COLORREF track;           // 进度条轨道
};

// 主题生命周期
void            UiThemeInit();
void            UiThemeShutdown();
// 重新读取系统深浅色设置；返回值表示深浅色是否发生变化
bool            UiThemeRefresh();

// 主题覆盖：0=跟随系统 1=强制浅色 2=强制深色
void            UiSetThemeOverride(int mode);
int             UiThemeOverride();

bool            UiIsDark();
const UiPalette& UiColors();

// DPI（按系统 DPI 缩放 96dpi 设计尺寸）
UINT            UiDpi();
int             UiPx(int designPx);

// 已缓存的字体（px 为设备像素）
// grayScaleAA=true 时使用灰度抗锯齿而非 ClearType：在强调色等饱和底色上
// ClearType 的次像素渲染会产生明显彩边，WinUI 3 在彩色表面上同样使用灰度抗锯齿。
HFONT           UiFont(int devicePx, bool semibold, bool grayScaleAA = false);
HFONT           UiIconFont(int devicePx);
// 系统字体设置变化后调用：丢弃字体缓存，下次按新的系统字体重建
void            UiFontsReload();
// 当前采用的界面字体名（用于诊断）
const wchar_t*  UiFaceName();

// 动画辅助
float           UiEaseOutCubic(float t);
COLORREF        UiBlend(COLORREF from, COLORREF to, float t);

// DWM：圆角 + 深色标题栏
void            UiApplyFrame(HWND hwnd, bool roundCorners, bool darkTitleBar);
// 关闭标准控件的视觉样式，让自定义颜色生效
void            UiDetheme(HWND child);

// ---------- WinUI 风格绘制助手（Direct2D 抗锯齿，主窗口与对话框共用）----------
void            UiFillRect(HDC dc, const RECT& rc, COLORREF color);
void            UiDrawRoundRect(HDC dc, const RECT& rc, int radiusPx,
                                bool fill, COLORREF fillColor,
                                bool border, COLORREF borderColor);
void            UiDrawTextLine(HDC dc, const std::wstring& text, const RECT& rc,
                               HFONT font, COLORREF color, UINT flags);

// 双缓冲：把整帧先画到内存 DC，析构时一次性 BitBlt 到目标 DC。
// 不这样做的话，背景填充、边框、标题、各行会逐个直接落到屏幕 DC 上，
// 元素越多越容易看到撕裂/频闪（设置界面最明显）。
class UiDoubleBuffer {
public:
    UiDoubleBuffer(HDC target, const RECT& rc);
    ~UiDoubleBuffer();
    UiDoubleBuffer(const UiDoubleBuffer&) = delete;
    UiDoubleBuffer& operator=(const UiDoubleBuffer&) = delete;

    // 绘制时使用这个 DC
    HDC Dc() const { return mem_ ? mem_ : target_; }
    operator HDC() const { return Dc(); }

private:
    HDC      target_ = nullptr;
    HDC      mem_ = nullptr;
    HBITMAP  bmp_ = nullptr;
    HBITMAP  old_ = nullptr;
    RECT     rc_{};
};
