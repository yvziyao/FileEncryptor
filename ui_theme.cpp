// ui_theme.cpp - WinUI 3 风格主题层实现。
//
// 设计要点：
//  * 深浅色跟随系统（HKCU\...\Themes\Personalize\AppsUseLightTheme）。
//  * 只使用**公开** API：DwmSetWindowAttribute 动态加载，SetWindowTheme 关闭视觉样式。
//    不使用 uxtheme 的未公开序号导出，避免不同 Windows 版本下调用错函数而崩溃。
//  * 所有颜色/字体集中在此，主窗口与对话框共用，保证“全局一致”。

#include "ui_theme.h"

#include <uxtheme.h>
#include <d2d1.h>
#include <string>
#include <utility>
#include <cstring>
#include <cwchar>
#include <cwctype>

#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "UxTheme.lib")
#pragma comment(lib, "D2d1.lib")

// ---- DWM 属性（自行定义常量，避免依赖特定 SDK 版本的头文件）----
#define UI_DWMWA_USE_IMMERSIVE_DARK_MODE  20
#define UI_DWMWA_WINDOW_CORNER_PREFERENCE 33
#define UI_DWMWA_BORDER_COLOR             34
#define UI_DWMWA_CAPTION_COLOR            35
#define UI_DWMWA_TEXT_COLOR               36
#define UI_DWMWCP_DONOTROUND              1
#define UI_DWMWCP_ROUND                   2

namespace {

typedef HRESULT(WINAPI* PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);

PFN_DwmSetWindowAttribute g_dwmSet = nullptr;
bool  g_dwmTried = false;

UiPalette g_palette = {};
bool      g_paletteReady = false;
UINT      g_dpi = 0;

const int kMaxFonts = 24;
HFONT g_fonts[kMaxFonts] = {};
int   g_fontPx[kMaxFonts] = {};
bool  g_fontBold[kMaxFonts] = {};
bool  g_fontGray[kMaxFonts] = {};
int   g_fontCount = 0;

HFONT g_iconFont = NULL;
int   g_iconFontPx = 0;

// ---------- 系统深浅色 ----------
bool QueryAppsUseLightTheme() {
    DWORD value = 1; // 默认浅色
    DWORD size = sizeof(value);
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &key) == ERROR_SUCCESS) {
        RegQueryValueExW(key, L"AppsUseLightTheme", NULL, NULL,
            reinterpret_cast<LPBYTE>(&value), &size);
        RegCloseKey(key);
    }
    return value != 0;
}

int g_themeOverride = 0;   // 0=跟随系统 1=浅色 2=深色

bool QueryDarkNow() {
    if (g_themeOverride == 1) return false;
    if (g_themeOverride == 2) return true;
    return !QueryAppsUseLightTheme();
}

void EnsureDwm() {
    if (g_dwmTried) return;
    g_dwmTried = true;
    HMODULE mod = LoadLibraryW(L"dwmapi.dll");
    if (mod) {
        g_dwmSet = reinterpret_cast<PFN_DwmSetWindowAttribute>(
            GetProcAddress(mod, "DwmSetWindowAttribute"));
    }
}

// ---------- 字体族选择 ----------
// 通过 EnumFontFamiliesEx 真实枚举系统已安装字体，而不是猜字体文件名
// （文件名随版本/语言包变化，猜错会被 GDI 静默替换成别的字体，可能出现方框乱码）。

std::wstring ToLowerW(const std::wstring& s) {
    std::wstring r = s;
    for (wchar_t& c : r) c = (wchar_t)towlower(c);
    return r;
}

int CALLBACK EnumFaceProc(const LOGFONTW* lf, const TEXTMETRICW*, DWORD, LPARAM lp) {
    std::pair<std::wstring, bool>* ctx = reinterpret_cast<std::pair<std::wstring, bool>*>(lp);
    if (ToLowerW(lf->lfFaceName) == ctx->first) {
        ctx->second = true;
        return 0; // 找到即停止
    }
    return 1;
}

std::wstring g_faceCache;

bool FontFamilyInstalled(const wchar_t* face) {
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) return false;
    std::pair<std::wstring, bool> ctx(ToLowerW(face), false);
    LOGFONTW lf = {};
    lf.lfCharSet = DEFAULT_CHARSET;   // 枚举全部字符集的字体族
    lf.lfFaceName[0] = L'\0';
    EnumFontFamiliesExW(dc, &lf, EnumFaceProc, reinterpret_cast<LPARAM>(&ctx), 0);
    DeleteDC(dc);
    return ctx.second;
}

void PickUiFace() {
    if (!g_faceCache.empty()) return;

    // 优先级：中文系统默认 UI 字体 -> 西文系统默认 UI 字体 -> 兜底
    static const wchar_t* kCandidates[] = {
        L"Microsoft YaHei UI",        // Win10/11 中文默认界面字体，中英文齐备
        L"Microsoft YaHei",
        L"Segoe UI Variable Text",    // Win11 西文默认
        L"Segoe UI Variable",
        L"Segoe UI",                  // Win10 西文默认
        L"Tahoma",
    };
    for (const wchar_t* c : kCandidates) {
        if (FontFamilyInstalled(c)) { g_faceCache = c; return; }
    }

    // 全部候选都缺失时，直接采用系统“消息字体”
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0) &&
        ncm.lfMessageFont.lfFaceName[0] != L'\0') {
        g_faceCache = ncm.lfMessageFont.lfFaceName;
        return;
    }
    g_faceCache = L"Tahoma";
}

const wchar_t* UiIconFaceName() {
    static const wchar_t* face = nullptr;
    if (face == reinterpret_cast<const wchar_t*>(-1)) return nullptr;
    if (face) return face;

    wchar_t dir[MAX_PATH] = {};
    GetWindowsDirectoryW(dir, MAX_PATH);
    std::wstring fonts = std::wstring(dir) + L"\\Fonts\\";
    auto exists = [&](const wchar_t* file) {
        return GetFileAttributesW((fonts + file).c_str()) != INVALID_FILE_ATTRIBUTES;
    };

    if (exists(L"SegoeIcons.ttf")) {
        face = L"Segoe Fluent Icons";
    } else if (exists(L"segmdl2.ttf")) {
        face = L"Segoe MDL2 Assets";
    } else {
        face = reinterpret_cast<const wchar_t*>(-1); // 无图标字体
    }
    return face == reinterpret_cast<const wchar_t*>(-1) ? nullptr : face;
}

void BuildPalette(bool dark) {
    UiPalette& p = g_palette;
    p.dark = dark;
    if (dark) {
        // WinUI 3 Dark
        p.bg             = RGB(0x20, 0x20, 0x20);
        p.bgAlt          = RGB(0x2C, 0x2C, 0x2C);
        p.text           = RGB(0xFF, 0xFF, 0xFF);
        p.textMuted      = RGB(0xC5, 0xC5, 0xC5);
        p.border         = RGB(0x3A, 0x3A, 0x3A);
        p.control        = RGB(0x33, 0x33, 0x33);
        p.controlHover   = RGB(0x3D, 0x3D, 0x3D);
        p.controlPressed = RGB(0x2A, 0x2A, 0x2A);
        p.accent         = RGB(0x60, 0xCD, 0xFF);
        p.accentHover    = RGB(0x7A, 0xD7, 0xFF);
        p.accentPressed  = RGB(0x4A, 0xBE, 0xF0);
        p.onAccent       = RGB(0x00, 0x00, 0x00);
        p.danger         = RGB(0xFF, 0x99, 0xA4);
        p.track          = RGB(0x3A, 0x3A, 0x3A);
    } else {
        // WinUI 3 Light
        p.bg             = RGB(0xF3, 0xF3, 0xF3);
        p.bgAlt          = RGB(0xFF, 0xFF, 0xFF);
        p.text           = RGB(0x1A, 0x1A, 0x1A);
        p.textMuted      = RGB(0x5D, 0x5D, 0x5D);
        p.border         = RGB(0xD6, 0xD6, 0xD6);
        p.control        = RGB(0xFD, 0xFD, 0xFD);
        p.controlHover   = RGB(0xF6, 0xF6, 0xF6);
        p.controlPressed = RGB(0xEF, 0xEF, 0xEF);
        p.accent         = RGB(0x00, 0x5F, 0xB8);
        p.accentHover    = RGB(0x19, 0x71, 0xC9);
        p.accentPressed  = RGB(0x00, 0x50, 0x9E);
        p.onAccent       = RGB(0xFF, 0xFF, 0xFF);
        p.danger         = RGB(0xC4, 0x2B, 0x1C);
        p.track          = RGB(0xE0, 0xE0, 0xE0);
    }
    g_paletteReady = true;
}

} // namespace

// ---------- 公开实现 ----------

// 当前采用的界面字体名（须定义在匿名命名空间之外，与头文件声明保持同一实体）
const wchar_t* UiFaceName() {
    PickUiFace();
    return g_faceCache.c_str();
}

void UiThemeInit() {
    UiDpi(); // 预热 DPI
    BuildPalette(QueryDarkNow());
}

void UiSetThemeOverride(int mode) {
    if (mode < 0 || mode > 2) mode = 0;
    g_themeOverride = mode;
    UiThemeRefresh();
}

int UiThemeOverride() {
    return g_themeOverride;
}

void UiFontsReload() {
    for (int i = 0; i < g_fontCount; ++i) {
        if (g_fonts[i]) DeleteObject(g_fonts[i]);
        g_fonts[i] = NULL;
    }
    g_fontCount = 0;
    if (g_iconFont) { DeleteObject(g_iconFont); g_iconFont = NULL; }
    g_iconFontPx = 0;
    g_faceCache.clear();   // 下次取字体时重新检测系统字体
}

void UiThemeShutdown() {
    UiFontsReload();
}

bool UiThemeRefresh() {
    const bool dark = QueryDarkNow();
    if (!g_paletteReady || g_palette.dark != dark) {
        BuildPalette(dark);
        return true;
    }
    return false;
}

bool UiIsDark() {
    if (!g_paletteReady) UiThemeInit();
    return g_palette.dark;
}

const UiPalette& UiColors() {
    if (!g_paletteReady) UiThemeInit();
    return g_palette;
}

UINT UiDpi() {
    if (g_dpi) return g_dpi;
    typedef UINT(WINAPI* PFN_GetDpiForSystem)();
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        auto fn = reinterpret_cast<PFN_GetDpiForSystem>(
            GetProcAddress(user32, "GetDpiForSystem"));
        if (fn) g_dpi = fn();
    }
    if (!g_dpi) {
        HDC dc = GetDC(NULL);
        if (dc) {
            g_dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSY);
            ReleaseDC(NULL, dc);
        }
    }
    if (!g_dpi) g_dpi = 96;
    return g_dpi;
}

int UiPx(int designPx) {
    return MulDiv(designPx, (int)UiDpi(), 96);
}

HFONT UiFont(int devicePx, bool semibold, bool grayScaleAA) {
    for (int i = 0; i < g_fontCount; ++i) {
        if (g_fontPx[i] == devicePx && g_fontBold[i] == semibold && g_fontGray[i] == grayScaleAA)
            return g_fonts[i];
    }
    if (g_fontCount >= kMaxFonts) return g_fonts[0];

    LOGFONTW lf = {};
    lf.lfHeight = -devicePx;
    lf.lfWeight = semibold ? FW_SEMIBOLD : FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    // 彩色/强调色底面上用灰度抗锯齿，避免 ClearType 次像素彩边
    lf.lfQuality = grayScaleAA ? ANTIALIASED_QUALITY : CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
    wcsncpy_s(lf.lfFaceName, UiFaceName(), _TRUNCATE);

    HFONT font = CreateFontIndirectW(&lf);
    g_fonts[g_fontCount] = font;
    g_fontPx[g_fontCount] = devicePx;
    g_fontBold[g_fontCount] = semibold;
    g_fontGray[g_fontCount] = grayScaleAA;
    ++g_fontCount;
    return font;
}

HFONT UiIconFont(int devicePx) {
    const wchar_t* face = UiIconFaceName();
    if (!face) return NULL;
    if (g_iconFont && g_iconFontPx == devicePx) return g_iconFont;
    if (g_iconFont) { DeleteObject(g_iconFont); g_iconFont = NULL; }

    LOGFONTW lf = {};
    lf.lfHeight = -devicePx;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
    g_iconFont = CreateFontIndirectW(&lf);
    g_iconFontPx = devicePx;
    return g_iconFont;
}

void UiApplyFrame(HWND hwnd, bool roundCorners, bool darkTitleBar) {
    if (!hwnd) return;
    EnsureDwm();
    if (!g_dwmSet) return;

    const BOOL dark = darkTitleBar ? TRUE : FALSE;
    g_dwmSet(hwnd, UI_DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    // 旧版 Windows 10 使用 19 作为该属性编号
    g_dwmSet(hwnd, 19, &dark, sizeof(dark));

    DWORD pref = roundCorners ? UI_DWMWCP_ROUND : UI_DWMWCP_DONOTROUND;
    g_dwmSet(hwnd, UI_DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));

    if (darkTitleBar) {
        COLORREF cap = UiColors().bg;
        COLORREF txt = UiColors().text;
        g_dwmSet(hwnd, UI_DWMWA_CAPTION_COLOR, &cap, sizeof(cap));
        g_dwmSet(hwnd, UI_DWMWA_TEXT_COLOR, &txt, sizeof(txt));
    }
}

void UiDetheme(HWND child) {
    if (!child) return;
    // 空字符串表示关闭该控件的视觉样式，之后自定义颜色/自绘才生效
    SetWindowTheme(child, L"", L"");
}

// ---------- Direct2D 绘制助手 ----------

namespace {

ID2D1Factory* g_d2dFactory = nullptr;
bool g_d2dTried = false;

ID2D1Factory* D2DFactory() {
    if (!g_d2dTried) {
        g_d2dTried = true;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2dFactory);
    }
    return g_d2dFactory;
}

// 注意：**不要复用** DC 渲染目标。实测在同一个 HDC 上第二次及以后调用 BindDC，
// BeginDraw/EndDraw 不再产生任何像素（只有第一次的绘制会落到 DC 上），
// 会导致同一窗口里后续绘制的元素（状态栏底色、输入框白框、卡片描边）静默丢失。
// 因此每次绘制都新建一个渲染目标，用完立即释放。
ID2D1DCRenderTarget* D2DTargetFor(HDC dc, const RECT& rc) {
    ID2D1Factory* factory = D2DFactory();
    if (!factory) return nullptr;

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        0.0f, 0.0f,
        D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);

    ID2D1DCRenderTarget* rt = nullptr;
    if (FAILED(factory->CreateDCRenderTarget(&props, &rt)) || !rt) return nullptr;
    if (FAILED(rt->BindDC(dc, &rc))) {
        rt->Release();
        return nullptr;
    }
    return rt;
}

D2D1_COLOR_F ToD2D(COLORREF c) {
    return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, 1.0f);
}

} // namespace

void UiFillRect(HDC dc, const RECT& rc, COLORREF color) {
    if (!dc) return;
    ID2D1DCRenderTarget* rt = D2DTargetFor(dc, rc);
    if (!rt) {
        HBRUSH b = CreateSolidBrush(color);
        FillRect(dc, &rc, b);
        DeleteObject(b);
        return;
    }
    // BindDC 会把渲染目标原点平移到绑定矩形的左上角，
    // 所以绘制坐标必须用“相对绑定矩形”的坐标，而不能直接用 DC 坐标。
    rt->BeginDraw();
    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(ToD2D(color), &brush)) && brush) {
        rt->FillRectangle(
            D2D1::RectF(0.0f, 0.0f, (FLOAT)(rc.right - rc.left), (FLOAT)(rc.bottom - rc.top)),
            brush);
        brush->Release();
    }
    rt->EndDraw();
    rt->Release();
}

void UiDrawRoundRect(HDC dc, const RECT& rc, int radiusPx,
                     bool fill, COLORREF fillColor,
                     bool border, COLORREF borderColor) {
    if (!dc) return;
    const FLOAT r = (FLOAT)(radiusPx > 0 ? radiusPx : 1);

    // 描边以路径为中心向两侧各扩 0.5px，绑定区域外扩 1px 才不会把边缘裁掉
    RECT bind = rc;
    InflateRect(&bind, 1, 1);

    ID2D1DCRenderTarget* rt = D2DTargetFor(dc, bind);
    if (!rt) {
        // 退化路径：使用 GDI 绘制
        HGDIOBJ oldBrush = SelectObject(dc, fill ? CreateSolidBrush(fillColor) : GetStockObject(NULL_BRUSH));
        HGDIOBJ oldPen = SelectObject(dc, border ? CreatePen(PS_SOLID, 1, borderColor) : GetStockObject(NULL_PEN));
        RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radiusPx * 2, radiusPx * 2);
        if (border) DeleteObject(SelectObject(dc, oldPen));
        if (fill) DeleteObject(SelectObject(dc, oldBrush));
        return;
    }

    // 相对绑定矩形的坐标（见 UiFillRect 中的说明）
    const FLOAT dx = (FLOAT)(rc.left - bind.left);
    const FLOAT dy = (FLOAT)(rc.top - bind.top);
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
        D2D1::RectF(dx, dy,
                    dx + (FLOAT)(rc.right - rc.left),
                    dy + (FLOAT)(rc.bottom - rc.top)), r, r);

    rt->BeginDraw();
    if (fill) {
        ID2D1SolidColorBrush* brush = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(ToD2D(fillColor), &brush)) && brush) {
            rt->FillRoundedRectangle(rr, brush);
            brush->Release();
        }
    }
    if (border) {
        ID2D1SolidColorBrush* pen = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(ToD2D(borderColor), &pen)) && pen) {
            rt->DrawRoundedRectangle(rr, pen, 1.0f);
            pen->Release();
        }
    }
    rt->EndDraw();
    rt->Release();
}

void UiDrawTextLine(HDC dc, const std::wstring& text, const RECT& rc,
                    HFONT font, COLORREF color, UINT flags) {
    if (!dc || text.empty()) return;
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    const int oldMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldColor = SetTextColor(dc, color);

    RECT r = rc;
    DrawTextW(dc, text.c_str(), (int)text.size(), &r, flags | DT_NOPREFIX);

    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldMode);
    if (oldFont) SelectObject(dc, oldFont);
}

// ---------- 双缓冲 ----------

UiDoubleBuffer::UiDoubleBuffer(HDC target, const RECT& rc) : target_(target), rc_(rc) {
    if (!target_) return;
    const int w = rc_.right - rc_.left;
    const int h = rc_.bottom - rc_.top;
    if (w <= 0 || h <= 0) return;

    mem_ = CreateCompatibleDC(target_);
    if (!mem_) return;
    bmp_ = CreateCompatibleBitmap(target_, w, h);
    if (!bmp_) {
        DeleteDC(mem_);
        mem_ = nullptr;
        return;
    }
    old_ = static_cast<HBITMAP>(SelectObject(mem_, bmp_));
}

UiDoubleBuffer::~UiDoubleBuffer() {
    if (!mem_) return;
    // 整帧一次性贴到目标 DC
    BitBlt(target_, rc_.left, rc_.top, rc_.right - rc_.left, rc_.bottom - rc_.top,
           mem_, 0, 0, SRCCOPY);
    if (old_) SelectObject(mem_, old_);
    if (bmp_) DeleteObject(bmp_);
    DeleteDC(mem_);
}

// ---------- 动画辅助 ----------

float UiEaseOutCubic(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

COLORREF UiBlend(COLORREF from, COLORREF to, float t) {
    if (t <= 0.0f) return from;
    if (t >= 1.0f) return to;
    const int r = (int)(GetRValue(from) + (GetRValue(to) - GetRValue(from)) * t + 0.5f);
    const int g = (int)(GetGValue(from) + (GetGValue(to) - GetGValue(from)) * t + 0.5f);
    const int b = (int)(GetBValue(from) + (GetBValue(to) - GetBValue(from)) * t + 0.5f);
    return RGB(r, g, b);
}
