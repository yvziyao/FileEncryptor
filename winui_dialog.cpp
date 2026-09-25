// winui_dialog.cpp - WinUI 3 风格自绘对话框实现。
//
// 结构：
//   UiDlgState        一次模态会话的全部状态（栈上分配，窗口销毁后返回）
//   ComputeLayout     按 DPI 计算窗口尺寸与各元素矩形
//   UiDlgProc         窗口过程（创建子控件、自绘、响应命令）
//   RunDialog         创建窗口 + 禁用属主 + 嵌套消息循环（模态）
//
// 说明：按钮与复选框同样是真实子控件（保证键盘/无障碍可用），但全部自绘；
//       悬停状态存在控件自身的 GWLP_USERDATA 中（不用 GetWindowSubclass，
//       因为 System32 版 comctl32 并未导出该函数）。

#include "winui_dialog.h"
#include "ui_theme.h"
#include "app_settings.h"
#include "FileEncryptor_helpers.h"

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>

// 由 FileEncryptor.cpp 提供（选择“指定目录”时使用）
std::wstring PickFolderDialog(HWND owner, const std::wstring& initial);

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Gdi32.lib")

#define UIDLG_CLASS L"FileEncryptorWinUI3Dialog"

// 自定义控件 ID（避开 winuser.h 中的标准 ID）
#define IDC_UI_EDIT1 2001
#define IDC_UI_EDIT2 2002
#define IDC_UI_SHOW  2003

namespace {

// ---------------------------------------------------------------- 工具

int MaxI(int a, int b) { return a > b ? a : b; }

std::string WstringToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    if (len <= 0) return {};
    std::string s;
    s.resize(len);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], len, NULL, NULL);
    return s;
}

std::wstring ButtonLabel(int id) {
    switch (id) {
    case IDOK:     return tr(L"确定", L"OK");
    case IDCANCEL: return tr(L"取消", L"Cancel");
    case IDYES:    return tr(L"是", L"Yes");
    case IDNO:     return tr(L"否", L"No");
    default:       return std::wstring();
    }
}

// 单行 EDIT 的文字是**顶部对齐**的：控件比文字高多少，就全堆在下面。
// 所以输入框的可见“白框”与真实 EDIT 控件要分开：
//   白框(well) 保持 32px 的视觉高度；
//   EDIT 控件只取“文字行高 + 2px”，并在白框内垂直居中 —— 文字/光标才居中。
int TextCellHeight(HDC dc, HFONT font) {
    if (!dc || !font) return 16;
    HGDIOBJ old = SelectObject(dc, font);
    TEXTMETRICW tm = {};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    return tm.tmHeight > 0 ? tm.tmHeight : 16;
}

// 单行测量使用 DT_SINGLELINE，多行测量使用 DT_WORDBREAK（二者不可同时指定）
void MeasureText(HDC dc, HFONT font, const std::wstring& text, int maxW, bool singleLine,
                 int* outW, int* outH) {
    int w = 0, h = 0;
    if (!text.empty() && font && dc) {
        HGDIOBJ old = SelectObject(dc, font);
        RECT rc = { 0, 0, maxW, 0 };
        UINT flags = DT_CALCRECT | DT_NOPREFIX | (singleLine ? DT_SINGLELINE : DT_WORDBREAK);
        DrawTextW(dc, text.c_str(), (int)text.size(), &rc, flags);
        SelectObject(dc, old);
        w = rc.right - rc.left;
        h = rc.bottom - rc.top;
    }
    if (outW) *outW = w;
    if (outH) *outH = h;
}

const wchar_t* IconGlyph(UiIcon icon) {
    switch (icon) {
    case UiIcon::Info:     return L"\uE946";
    case UiIcon::Warning:  return L"\uE7BA";
    case UiIcon::Error:    return L"\uEA39";
    case UiIcon::Question: return L"\uE897";
    default:               return nullptr;
    }
}

COLORREF IconColor(UiIcon icon) {
    const UiPalette& c = UiColors();
    switch (icon) {
    case UiIcon::Warning: return c.dark ? RGB(0xFF, 0xB9, 0x00) : RGB(0x9D, 0x5D, 0x00);
    case UiIcon::Error:   return c.danger;
    default:              return c.accent;
    }
}

void PlayDialogSound(UiIcon icon) {
    switch (icon) {
    case UiIcon::Error:    MessageBeep(MB_ICONERROR); break;
    case UiIcon::Warning:  MessageBeep(MB_ICONWARNING); break;
    case UiIcon::Question: MessageBeep(MB_ICONQUESTION); break;
    default:               MessageBeep(MB_ICONINFORMATION); break;
    }
}

// 密码强度评估（与原实现一致：长度 + 字符集）
// 密码强度分级：0=未输入 1=很弱 2=一般 3=良好 4=很强
int EvaluateStrengthLevel(const std::wstring& ws) {
    if (ws.empty()) return 0;
    int score = 0;
    if (ws.length() >= 8) score += 2;
    if (ws.length() >= 12) score += 1;
    bool hasLower = false, hasUpper = false, hasDigit = false, hasSymbol = false;
    for (wchar_t ch : ws) {
        if (iswlower(ch)) hasLower = true;
        else if (iswupper(ch)) hasUpper = true;
        else if (iswdigit(ch)) hasDigit = true;
        else hasSymbol = true;
    }
    if (hasLower && hasUpper) score += 1;
    if (hasDigit) score += 1;
    if (hasSymbol) score += 1;

    if (score <= 1) return 1;
    if (score <= 3) return 2;
    if (score <= 5) return 3;
    return 4;
}

std::wstring StrengthLabel(int level) {
    switch (level) {
    case 1: return tr(L"很弱", L"very weak");
    case 2: return tr(L"一般", L"fair");
    case 3: return tr(L"良好", L"good");
    case 4: return tr(L"很强", L"strong");
    default: return std::wstring();
    }
}

// 强度颜色反馈：很弱=红，一般=黄(琥珀)，良好/很强=绿
COLORREF StrengthColor(int level) {
    const UiPalette& c = UiColors();
    switch (level) {
    case 1: return c.danger;                                                  // 红
    case 2: return c.dark ? RGB(0xFF, 0xB9, 0x00) : RGB(0x9D, 0x5D, 0x00);    // 黄
    case 3: return c.dark ? RGB(0x6C, 0xCB, 0x5E) : RGB(0x0F, 0x7B, 0x0F);    // 绿
    case 4: return c.dark ? RGB(0x9A, 0xE5, 0x8A) : RGB(0x0B, 0x5A, 0x0B);    // 深绿
    default: return c.textMuted;
    }
}

// 用当前 DC 测量一段单行文本的像素宽度（用于“前缀 + 彩色值”分段绘制）
int MeasureLineWidth(HDC dc, HFONT font, const std::wstring& s) {
    if (!dc || !font || s.empty()) return 0;
    HGDIOBJ old = SelectObject(dc, font);
    RECT r = { 0, 0, 0, 0 };
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
    return r.right - r.left;
}

// ---------------------------------------------------------------- 状态

// 对话框动画：入场淡入 + 按钮悬停渐变，共用一个 15ms 定时器
#define DLG_ANIM_TIMER 1
#define DLG_ANIM_MS    15

struct BtnExtra { bool hover = false; float anim = 0.0f; };

struct UiDlgState {
    // 内容
    bool         isPassword = false;
    std::wstring caption;
    std::wstring text;
    UiIcon       icon = UiIcon::None;
    UINT         buttons = MB_OK;
    bool         confirm = false;
    bool         topmost = false;
    bool         sound = true;

    // 控件
    HWND              hwnd = nullptr;
    HWND              edit1 = nullptr;
    HWND              edit2 = nullptr;
    HWND              chkShow = nullptr;
    std::vector<HWND> btns;        // 与 btnIds / btnWidths 同序（左 -> 右）
    std::vector<int>  btnIds;
    std::vector<int>  btnWidths;
    std::vector<HWND> tabOrder;
    int               primaryId = IDOK;
    int               cancelId = IDCANCEL;

    // 会话状态
    int          result = 0;
    bool         done = false;
    bool         showPwd = false;
    std::wstring password;
    std::wstring error;
    int          strengthLevel = 0;   // 0=未输入 1=很弱 2=一般 3=良好 4=很强
    std::wstring subtitle;            // 标题下方的补充说明（批量时显示当前文件路径）

    // 布局
    int winW = 0, winH = 0;
    int pad = 0, btnH = 0, btnGap = 0, editH = 0;
    int titleY = 0, titleH = 0;
    int iconX = 0, iconY = 0, iconSize = 0;
    int contentX = 0, contentW = 0;
    RECT rBody{}, rLabel1{}, rEdit1{}, rLabel2{}, rEdit2{}, rStrength{}, rError{}, rChk{}, btnArea{};
    RECT rSubtitle{};
    RECT rEdit1Inner{}, rEdit2Inner{};   // 真实 EDIT 控件（在白框内垂直居中）
    int  editInnerH = 0;
    int  editInnerOffset = 0;

    HBRUSH altBrush = nullptr;

    // 动画
    bool        animRunning = false;
    bool        layered = false;      // 淡入期间临时使用分层窗口
    float       fadeAlpha = 0.0f;
    ULONGLONG   lastAnimTick = 0;

    // ---- 设置对话框 ----
    struct SegRow {
        std::wstring              label;
        std::vector<std::wstring> options;
        int                       selected = 0;
        float                     animSel = 0.0f;   // 动画中的选中位置（0..n-1）
        int                       key = 0;      // 0=语言 1=主题 2=输出位置 3=安全删除 4=完成后 5=加密强度
        RECT                      labelRc{};
        RECT                      segRc{};
        std::vector<RECT>         itemRc;
    };
    bool                     isSettings = false;
    std::vector<SegRow>      rows;
    int                      focusedRow = 0;
    UiSettingsData           data;
    RECT                     hintRc{};
    bool                     hintVisible = false;
};

void EnsureDlgAnimation(HWND hDlg);
void PositionButtons(UiDlgState* st);
void RelayoutDialog(HWND hDlg, UiDlgState* st);
void ApplySettingsRowChange(HWND hDlg, UiDlgState* st, int key);
void TickSettingsSlide(HWND hDlg, UiDlgState* st, float dt, bool& active);


// ---------------------------------------------------------------- 布局

// 设置对话框布局：标题 + 若干“标签 + 分段控件”行 + 按钮
void ComputeSettingsLayout(UiDlgState* st, HDC dc) {
    HFONT fTitle = UiFont(UiPx(18), true);

    st->pad = UiPx(24);
    st->btnH = UiPx(32);
    st->btnGap = UiPx(8);

    int titleW = 0, titleH = 0;
    MeasureText(dc, fTitle, st->caption, UiPx(700), true, &titleW, &titleH);
    st->titleH = titleH > 0 ? titleH : UiPx(24);

    const int rowH = UiPx(34);
    const int rowGap = UiPx(14);
    const int labelW = UiPx(132);
    const int hintH = UiPx(18);

    int winW = UiPx(560);
    // 保证标题与所有行都放得下
    {
        HFONT fBody = UiFont(UiPx(14), false);
        int needW = titleW + st->pad * 2;
        for (const auto& r : st->rows) {
            int lw = 0, lh = 0;
            MeasureText(dc, fBody, r.label, UiPx(400), true, &lw, &lh);
            int optsW = 0;
            for (const auto& o : r.options) {
                int ow = 0, oh = 0;
                MeasureText(dc, fBody, o, UiPx(400), true, &ow, &oh);
                optsW += ow + UiPx(28);
            }
            const int rowNeed = st->pad * 2 + labelW + UiPx(16) + optsW;
            if (rowNeed > needW) needW = rowNeed;
            (void)lw;
        }
        if (needW > winW) winW = needW;
    }

    st->contentX = st->pad;
    st->contentW = winW - st->pad * 2;

    int y = st->pad;
    st->titleY = y;
    y += st->titleH + UiPx(18);

    st->hintVisible = false;
    for (auto& r : st->rows) {
        r.labelRc = { st->contentX, y, st->contentX + labelW, y + rowH };
        const int segLeft = st->contentX + labelW + UiPx(16);
        r.segRc = { segLeft, y, st->contentX + st->contentW, y + rowH };

        r.itemRc.clear();
        const int n = (int)r.options.size();
        const int totalW = r.segRc.right - r.segRc.left;
        for (int k = 0; k < n; ++k) {
            const int x0 = r.segRc.left + totalW * k / (n ? n : 1);
            const int x1 = r.segRc.left + totalW * (k + 1) / (n ? n : 1);
            r.itemRc.push_back({ x0, r.segRc.top, x1, r.segRc.bottom });
        }
        y += rowH + rowGap;

        // “指定目录”时在下一行显示当前目录
        if (r.key == 2 && r.selected == 1) {
            st->hintRc = { segLeft, y - rowGap + UiPx(2), st->contentX + st->contentW, y - rowGap + UiPx(2) + hintH };
            st->hintVisible = true;
            y += hintH + UiPx(6);
        }
    }

    st->btnArea = { st->pad, y, winW - st->pad, y + st->btnH };
    y += st->btnH + st->pad;

    // 按钮宽度（文本 + 左右内边距，最小宽度 96）—— 少了这一步 PositionButtons 会越界
    {
        HFONT fBody = UiFont(UiPx(14), false);
        st->btnWidths.assign(st->btnIds.size(), 0);
        int totalBtnW = 0;
        for (size_t i = 0; i < st->btnIds.size(); ++i) {
            int tw = 0, th = 0;
            MeasureText(dc, fBody, ButtonLabel(st->btnIds[i]), UiPx(240), true, &tw, &th);
            const int bw = MaxI(tw + UiPx(36), UiPx(96));
            st->btnWidths[i] = bw;
            totalBtnW += bw;
        }
        if (!st->btnIds.empty())
            totalBtnW += st->btnGap * (int)(st->btnIds.size() - 1);
        const int needBtnW = st->pad * 2 + totalBtnW;
        if (winW < needBtnW) {
            winW = needBtnW;
            st->btnArea.right = winW - st->pad;
            st->contentW = winW - st->pad * 2;
        }
    }

    st->winW = winW;
    st->winH = y;
}

void ComputeLayout(UiDlgState* st, HDC dc) {
    if (st->isSettings) {
        ComputeSettingsLayout(st, dc);
        return;
    }

    HFONT fTitle = UiFont(UiPx(18), true);
    HFONT fBody = UiFont(UiPx(14), false);

    st->pad = UiPx(24);
    st->btnH = UiPx(32);
    st->btnGap = UiPx(8);
    st->editH = UiPx(32);
    st->iconSize = UiPx(20);

    const int gap = UiPx(12);
    const int maxBodyW = UiPx(460);
    const int minW = UiPx(360);

    int titleW = 0, titleH = 0;
    MeasureText(dc, fTitle, st->caption, UiPx(700), true, &titleW, &titleH);
    st->titleH = titleH > 0 ? titleH : UiPx(24);

    const bool hasIcon = (st->icon != UiIcon::None) && (UiIconFont(st->iconSize) != NULL);
    const int iconCol = hasIcon ? (st->iconSize + gap) : 0;

    int winW = 0;
    int bodyW = 0, bodyH = 0;

    if (st->isPassword) {
        int chkW = 0, chkH = 0;
        MeasureText(dc, fBody, tr(L"显示密码", L"Show password"), maxBodyW, true, &chkW, &chkH);
        int minContent = UiPx(300);
        if (chkW + UiPx(30) > minContent) minContent = chkW + UiPx(30);
        winW = st->pad * 2 + MaxI(titleW, minContent);
        if (winW < minW) winW = minW;
        st->contentX = st->pad;
        st->contentW = winW - st->pad * 2;
    }
    else {
        MeasureText(dc, fBody, st->text, maxBodyW, false, &bodyW, &bodyH);
        winW = st->pad * 2 + iconCol + MaxI(titleW, bodyW);
        if (winW < minW) winW = minW;
        const int hardMax = st->pad * 2 + iconCol + maxBodyW;
        if (winW > hardMax) winW = hardMax;
        st->contentX = st->pad + iconCol;
        st->contentW = winW - st->pad * 2 - iconCol;
        if (!st->text.empty())
            MeasureText(dc, fBody, st->text, st->contentW, false, &bodyW, &bodyH);
    }

    // 按钮宽度（文本 + 左右内边距，最小宽度 96）
    st->btnWidths.assign(st->btnIds.size(), 0);
    int totalBtnW = 0;
    for (size_t i = 0; i < st->btnIds.size(); ++i) {
        int tw = 0, th = 0;
        MeasureText(dc, fBody, ButtonLabel(st->btnIds[i]), UiPx(240), true, &tw, &th);
        int bw = MaxI(tw + UiPx(36), UiPx(96));
        st->btnWidths[i] = bw;
        totalBtnW += bw;
    }
    if (!st->btnIds.empty())
        totalBtnW += st->btnGap * (int)(st->btnIds.size() - 1);
    if (winW < st->pad * 2 + totalBtnW) winW = st->pad * 2 + totalBtnW;

    // ---- 高度 ----
    int y = st->pad;
    st->titleY = y;

    if (st->isPassword) {
        y += st->titleH + UiPx(16);

        // 副标题：批量处理时指明当前正在为哪个文件设置密码
        if (!st->subtitle.empty()) {
            const int subH = UiPx(18);
            st->rSubtitle = { st->contentX, y, st->contentX + st->contentW, y + subH };
            y += subH + UiPx(12);
        }

        const int labelH = UiPx(20);
        st->rLabel1 = { st->contentX, y, st->contentX + st->contentW, y + labelH };
        y += labelH + UiPx(4);
        st->rEdit1 = { st->contentX, y, st->contentX + st->contentW, y + st->editH };
        y += st->editH + gap;

        if (st->confirm) {
            st->rLabel2 = { st->contentX, y, st->contentX + st->contentW, y + labelH };
            y += labelH + UiPx(4);
            st->rEdit2 = { st->contentX, y, st->contentX + st->contentW, y + st->editH };
            y += st->editH + UiPx(6);
            st->rStrength = { st->contentX, y, st->contentX + st->contentW, y + UiPx(18) };
            y += UiPx(18) + UiPx(8);
        }
        else {
            y += UiPx(8);
        }

        st->rChk = { st->contentX, y, st->contentX + st->contentW, y + UiPx(22) };
        y += UiPx(22) + UiPx(6);

        // 错误行始终预留，避免出现错误时窗口尺寸变化
        st->rError = { st->contentX, y, st->contentX + st->contentW, y + UiPx(18) };
        y += UiPx(18) + UiPx(12);
    }
    else {
        if (!st->caption.empty() || hasIcon) y += st->titleH + UiPx(10);
        st->rBody = { st->contentX, y, st->contentX + st->contentW, y + bodyH };
        y += bodyH + UiPx(24);
    }

    st->btnArea = { st->pad, y, winW - st->pad, y + st->btnH };
    y += st->btnH + st->pad;

    st->winW = winW;
    st->winH = y;

    // 真实 EDIT 控件：只用“文字行高 + 2px”，在 32px 白框内垂直居中
    st->editInnerH = TextCellHeight(dc, fBody) + 2;
    if (st->editInnerH > st->editH) st->editInnerH = st->editH;
    st->editInnerOffset = (st->editH - st->editInnerH) / 2;

    auto innerOf = [&](const RECT& well) {
        RECT r = { well.left, well.top + st->editInnerOffset,
                   well.right, well.top + st->editInnerOffset + st->editInnerH };
        return r;
    };
    st->rEdit1Inner = innerOf(st->rEdit1);
    st->rEdit2Inner = innerOf(st->rEdit2);

    if (hasIcon) {
        st->iconX = st->pad;
        st->iconY = st->titleY + (st->titleH - st->iconSize) / 2;
    }
    else {
        st->iconSize = 0;
    }
}

// ---------------------------------------------------------------- 自绘

float ButtonAnimValue(HWND h) {
    BtnExtra* ex = reinterpret_cast<BtnExtra*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return ex ? ex->anim : 0.0f;
}

void DrawOwnerButton(LPDRAWITEMSTRUCT pdis) {
    const UiPalette& c = UiColors();
    RECT rc = pdis->rcItem;
    UiDoubleBuffer buffer(pdis->hDC, rc);
    HDC dc = buffer.Dc();
    const bool pressed = (pdis->itemState & ODS_SELECTED) != 0;
    const bool disabled = (pdis->itemState & ODS_DISABLED) != 0;
    const bool primary = (pdis->CtlID == IDOK || pdis->CtlID == IDYES);
    const float hover = ButtonAnimValue(pdis->hwndItem);

    COLORREF fill, textColor, borderColor;
    if (primary) {
        fill = pressed ? c.accentPressed : UiBlend(c.accent, c.accentHover, hover);
        textColor = c.onAccent;
        borderColor = fill;
    }
    else {
        fill = pressed ? c.controlPressed : UiBlend(c.control, c.controlHover, hover);
        textColor = c.text;
        borderColor = UiBlend(c.border, c.textMuted, hover);
    }
    if (disabled) {
        fill = c.control;
        textColor = c.textMuted;
        borderColor = c.border;
    }

    // 先铺满整个控件，圆角之外才不会残留未绘制像素
    UiFillRect(dc, rc, c.bg);
    UiDrawRoundRect(dc, rc, UiPx(5), true, fill, true, borderColor);

    const std::wstring label = ButtonLabel(pdis->CtlID);
    RECT textRc = rc;
    // 强调色按钮用灰度抗锯齿，避免 ClearType 在饱和底色上产生彩边
    UiDrawTextLine(dc, label, textRc, UiFont(UiPx(14), false, primary), textColor,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawOwnerCheckbox(LPDRAWITEMSTRUCT pdis, const UiDlgState* st) {
    const UiPalette& c = UiColors();
    RECT rc = pdis->rcItem;
    UiDoubleBuffer buffer(pdis->hDC, rc);
    HDC dc = buffer.Dc();
    UiFillRect(dc, rc, c.bg);   // 铺满，避免残留
    const int box = UiPx(18);
    RECT boxRc = { rc.left, rc.top + (rc.bottom - rc.top - box) / 2,
                   rc.left + box, rc.top + (rc.bottom - rc.top - box) / 2 + box };

    if (st->showPwd) {
        UiDrawRoundRect(dc, boxRc, UiPx(4), true, c.accent, false, c.accent);
        HFONT iconFont = UiIconFont(UiPx(11));
        if (iconFont) {
            RECT glyphRc = boxRc;
            UiDrawTextLine(dc, L"\uE73E", glyphRc, iconFont, c.onAccent,
                           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    else {
        UiDrawRoundRect(dc, boxRc, UiPx(4), true, c.bgAlt, true, c.border);
    }

    RECT labelRc = { boxRc.right + UiPx(10), rc.top, rc.right, rc.bottom };
    UiDrawTextLine(dc, tr(L"显示密码", L"Show password"), labelRc,
                   UiFont(UiPx(14), false), c.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

// 设置对话框：分段控件（SegmentedControl）风格的自绘行
void PaintSettings(HDC dc, UiDlgState* st, const RECT& rcClient) {
    const UiPalette& c = UiColors();
    HFONT fTitle = UiFont(UiPx(18), true);
    HFONT fBody = UiFont(UiPx(14), false);
    HFONT fSmall = UiFont(UiPx(12), false);

    UiFillRect(dc, rcClient, c.bg);
    UiDrawRoundRect(dc, rcClient, UiPx(10), false, c.bg, true, c.border);

    if (!st->caption.empty()) {
        RECT r = { st->contentX, st->titleY, st->contentX + st->contentW, st->titleY + st->titleH };
        UiDrawTextLine(dc, st->caption, r, fTitle, c.text,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    for (size_t i = 0; i < st->rows.size(); ++i) {
        const UiDlgState::SegRow& row = st->rows[i];

        RECT lr = row.labelRc;
        UiDrawTextLine(dc, row.label, lr, fBody, c.text,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // 分段控件容器
        UiDrawRoundRect(dc, row.segRc, UiPx(5), true, c.bgAlt, true, c.border);

        // 选中底色：按动画位置绘制，切换时从旧选项滑到新选项
        const int n = (int)row.itemRc.size();
        int activeIdx = row.selected;
        if (n > 0) {
            const int totalW = row.segRc.right - row.segRc.left;
            float pos = row.animSel;
            if (pos < 0.0f) pos = 0.0f;
            if (pos > (float)(n - 1)) pos = (float)(n - 1);
            RECT pill = {
                row.segRc.left + (int)(totalW * pos / n),
                row.segRc.top,
                row.segRc.left + (int)(totalW * (pos + 1.0f) / n),
                row.segRc.bottom
            };
            InflateRect(&pill, -UiPx(2), -UiPx(2));
            UiDrawRoundRect(dc, pill, UiPx(4), true, c.accent, false, c.accent);
            // 文字高亮必须跟随色块的**实际**位置：否则动画途中目标项会变成
            // 白字压在白色容器上（完全看不见），而旧项是黑字压在蓝底上。
            activeIdx = (int)(pos + 0.5f);
            if (activeIdx < 0) activeIdx = 0;
            if (activeIdx > n - 1) activeIdx = n - 1;
        }

        for (size_t k = 0; k < row.itemRc.size(); ++k) {
            RECT ir = row.itemRc[k];
            InflateRect(&ir, -UiPx(2), -UiPx(2));
            const bool selected = ((int)k == activeIdx);
            // 选中项为强调色底 -> 灰度抗锯齿
            UiDrawTextLine(dc, row.options[k], ir, fBody,
                           selected ? c.onAccent : c.text,
                           DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    if (st->hintVisible) {
        std::wstring hint = st->data.fixedDir.empty()
            ? tr(L"（未选择目录，将使用源文件所在目录）", L"(no folder chosen; source folder will be used)")
            : st->data.fixedDir;
        RECT hr = st->hintRc;
        UiDrawTextLine(dc, hint, hr, fSmall, c.textMuted,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_PATH_ELLIPSIS);
    }
}

// 普通对话框（消息框 / 密码框）的绘制主体
void PaintNormalDialog(HDC dc, UiDlgState* st, const RECT& rcClient) {
    const UiPalette& c = UiColors();
    HFONT fTitle = UiFont(UiPx(18), true);
    HFONT fBody = UiFont(UiPx(14), false);
    HFONT fSmall = UiFont(UiPx(12), false);

    UiFillRect(dc, rcClient, c.bg);
    UiDrawRoundRect(dc, rcClient, UiPx(10), false, c.bg, true, c.border);

    const wchar_t* glyph = IconGlyph(st->icon);
    HFONT iconFont = st->iconSize ? UiIconFont(st->iconSize) : NULL;
    if (glyph && iconFont) {
        RECT r = { st->iconX, st->iconY, st->iconX + st->iconSize, st->iconY + st->iconSize };
        UiDrawTextLine(dc, glyph, r, iconFont, IconColor(st->icon),
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    if (!st->caption.empty()) {
        RECT r = { st->contentX, st->titleY, st->contentX + st->contentW, st->titleY + st->titleH };
        UiDrawTextLine(dc, st->caption, r, fTitle, c.text,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // 副标题：路径过长时中间省略，保证文件名始终可见
    if (st->isPassword && !st->subtitle.empty()) {
        RECT r = st->rSubtitle;
        UiDrawTextLine(dc, st->subtitle, r, UiFont(UiPx(13), false), c.textMuted,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    }

    if (st->isPassword) {
        const COLORREF wellBorder = st->error.empty() ? c.border : c.danger;
        auto drawWell = [&](const RECT& r, bool visible) {
            if (!visible) return;
            RECT w = r;
            InflateRect(&w, UiPx(1), UiPx(1));
            UiDrawRoundRect(dc, w, UiPx(5), true, c.bgAlt, true, wellBorder);
        };
        drawWell(st->rEdit1, st->edit1 != nullptr);
        drawWell(st->rEdit2, st->edit2 != nullptr && IsWindowVisible(st->edit2));

        RECT l1 = st->rLabel1;
        UiDrawTextLine(dc, tr(L"密码:", L"Password:"), l1, fBody, c.textMuted,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (st->confirm) {
            RECT l2 = st->rLabel2;
            UiDrawTextLine(dc, tr(L"确认:", L"Confirm:"), l2, fBody, c.textMuted,
                           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        if (st->confirm && st->strengthLevel > 0) {
            // 前缀用次要色，强度值用分级颜色（很弱红 / 一般黄 / 良好·很强绿）
            const std::wstring prefix = tr(L"密码强度：", L"Strength: ");
            const std::wstring value = StrengthLabel(st->strengthLevel);
            RECT r = st->rStrength;
            UiDrawTextLine(dc, prefix, r, fSmall, c.textMuted,
                           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            RECT v = r;
            v.left += MeasureLineWidth(dc, fSmall, prefix);
            UiDrawTextLine(dc, value, v, fSmall, StrengthColor(st->strengthLevel),
                           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        if (!st->error.empty()) {
            RECT r = st->rError;
            UiDrawTextLine(dc, st->error, r, fSmall, c.danger,
                           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
    else if (!st->text.empty()) {
        // 必须走 UiDrawTextLine：它才会选中我们缓存的字体、设成 TRANSPARENT 背景模式
        // 并使用主题文本色。直接 DrawTextW 会退回 DC 默认（位图）字体 + 不透明白底。
        RECT r = st->rBody;
        UiDrawTextLine(dc, st->text, r, fBody, c.text, DT_LEFT | DT_TOP | DT_WORDBREAK);
    }

}

// 对话框统一入口：整帧先画到内存 DC，最后一次性贴上去，
// 否则背景、边框、标题、各行会逐个直接落到屏幕 DC 上而产生频闪。
void PaintDialog(HWND hwnd, UiDlgState* st) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rcClient;
    GetClientRect(hwnd, &rcClient);

    {
        UiDoubleBuffer buffer(hdc, rcClient);
        HDC dc = buffer.Dc();
        if (st->isSettings) PaintSettings(dc, st, rcClient);
        else                PaintNormalDialog(dc, st, rcClient);
    }

    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------- 子类化

LRESULT CALLBACK BtnSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                 UINT_PTR /*id*/, DWORD_PTR /*refData*/) {
    BtnExtra* ex = reinterpret_cast<BtnExtra*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_MOUSEMOVE:
        if (ex && !ex->hover) {
            ex->hover = true;
            EnsureDlgAnimation(GetParent(hwnd));   // 交给动画定时器渐变，不直接跳变
            TRACKMOUSEEVENT tme = {};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
        }
        break;
    case WM_MOUSELEAVE:
        if (ex && ex->hover) {
            ex->hover = false;
            EnsureDlgAnimation(GetParent(hwnd));
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, BtnSubclassProc, GetDlgCtrlID(hwnd));
        delete ex;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void AttachHover(HWND h) {
    BtnExtra* ex = new BtnExtra();
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ex));
    SetWindowSubclass(h, BtnSubclassProc, (UINT_PTR)GetDlgCtrlID(h), 0);
}

// 编辑控件在 EM_SETPASSWORDCHAR 之后**不会自动重绘**：
// 必须重新写入一次相同文本才能强制全量重画，否则要等到获得焦点/再次输入才刷新。
void SetEditPasswordMask(HWND edit, bool show) {
    if (!edit) return;
    const DWORD sel = (DWORD)SendMessageW(edit, EM_GETSEL, 0, 0);
    SendMessageW(edit, EM_SETPASSWORDCHAR, show ? 0 : (WPARAM)L'*', 0);

    const int len = GetWindowTextLengthW(edit);
    std::wstring text((size_t)len + 1, L'\0');
    GetWindowTextW(edit, &text[0], len + 1);
    text.resize((size_t)len);
    SetWindowTextW(edit, text.c_str());

    SendMessageW(edit, EM_SETSEL, LOWORD(sel), HIWORD(sel));
    InvalidateRect(edit, NULL, FALSE);
    UpdateWindow(edit);
}

void ApplyEditTheme(HWND edit, const UiDlgState* st) {
    if (!edit) return;
    UiDetheme(edit);
    RECT rc;
    GetClientRect(edit, &rc);
    const int r = UiPx(5) * 2;
    HRGN rgn = CreateRoundRectRgn(0, 0, rc.right + 1, rc.bottom + 1, r, r);
    if (rgn) SetWindowRgn(edit, rgn, TRUE);
    SetEditPasswordMask(edit, st->showPwd);
}

// ---------- 对话框动画 ----------

UiDlgState* StateOf(HWND hDlg) {
    return reinterpret_cast<UiDlgState*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));
}

void EnsureDlgAnimation(HWND hDlg) {
    UiDlgState* st = StateOf(hDlg);
    if (!st || st->animRunning) return;
    st->animRunning = true;
    st->lastAnimTick = 0;
    SetTimer(hDlg, DLG_ANIM_TIMER, DLG_ANIM_MS, NULL);
}

void TickDialogAnimation(HWND hDlg, UiDlgState* st) {
    const ULONGLONG now = GetTickCount64();
    float dt = st->lastAnimTick ? (float)(now - st->lastAnimTick) / 1000.0f
                                : (float)DLG_ANIM_MS / 1000.0f;
    st->lastAnimTick = now;
    if (dt <= 0.0f) dt = 0.001f;
    if (dt > 0.12f) dt = 0.12f;

    bool active = false;

    // 入场淡入（约 140ms）；结束后移除分层样式，恢复正常渲染（不影响 ClearType）
    if (st->layered) {
        st->fadeAlpha += 255.0f * dt / 0.14f;
        if (st->fadeAlpha >= 255.0f) {
            st->fadeAlpha = 255.0f;
            SetLayeredWindowAttributes(hDlg, 0, 255, LWA_ALPHA);
            LONG_PTR ex = GetWindowLongPtrW(hDlg, GWL_EXSTYLE);
            SetWindowLongPtrW(hDlg, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
            st->layered = false;
            InvalidateRect(hDlg, NULL, FALSE);
        }
        else {
            SetLayeredWindowAttributes(hDlg, 0, (BYTE)st->fadeAlpha, LWA_ALPHA);
            active = true;
        }
    }

    // 2) 按钮悬停渐变（约 120ms）
    const float step = dt / 0.12f;
    for (HWND b : st->btns) {
        if (!b) continue;
        BtnExtra* ex = reinterpret_cast<BtnExtra*>(GetWindowLongPtrW(b, GWLP_USERDATA));
        if (!ex) continue;
        const float target = ex->hover ? 1.0f : 0.0f;
        if (ex->anim == target) continue;
        if (target > ex->anim) { ex->anim += step; if (ex->anim > target) ex->anim = target; }
        else                   { ex->anim -= step; if (ex->anim < target) ex->anim = target; }
        InvalidateRect(b, NULL, FALSE);
        if (ex->anim != target) active = true;
    }

    // 3) 设置对话框：选中底色的滑动动画
    if (st->isSettings) TickSettingsSlide(hDlg, st, dt, active);

    if (!active) {
        KillTimer(hDlg, DLG_ANIM_TIMER);
        st->animRunning = false;
    }
}

// 设置对话框：选中底色滑动动画（约 160ms）
void TickSettingsSlide(HWND hDlg, UiDlgState* st, float dt, bool& active) {
    const float step = dt / 0.16f;
    bool moving = false;
    for (auto& row : st->rows) {
        const float target = (float)row.selected;
        if (row.animSel == target) continue;
        const float diff = target - row.animSel;
        if (diff > 0.0f) row.animSel = (row.animSel + step > target) ? target : row.animSel + step;
        else             row.animSel = (row.animSel - step < target) ? target : row.animSel - step;
        moving = true;
        if (row.animSel != target) active = true;
    }
    if (moving) InvalidateRect(hDlg, NULL, FALSE);
}

// ---- 设置对话框辅助 ----

void PositionButtons(UiDlgState* st) {
    int x = st->btnArea.right;
    for (int i = (int)st->btns.size() - 1; i >= 0; --i) {
        if (!st->btns[i]) continue;
        const int w = st->btnWidths[i];
        x -= w;
        SetWindowPos(st->btns[i], NULL, x, st->btnArea.top, w, st->btnH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        x -= st->btnGap;
    }
}

// 分段选项变化后重新布局：“指定目录”的提示行会让窗口变高/变矮
void RelayoutDialog(HWND hDlg, UiDlgState* st) {
    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    ComputeLayout(st, mem);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);

    RECT wr;
    GetWindowRect(hDlg, &wr);
    const int cx = wr.left + (wr.right - wr.left) / 2;
    const int cy = wr.top + (wr.bottom - wr.top) / 2;
    SetWindowPos(hDlg, NULL, cx - st->winW / 2, cy - st->winH / 2, st->winW, st->winH,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    PositionButtons(st);
    InvalidateRect(hDlg, NULL, FALSE);
}

void ApplySettingsRowChange(HWND hDlg, UiDlgState* st, int key) {
    if (key != 2) return;   // 只有“输出文件位置”需要额外处理
    for (auto& row : st->rows) {
        if (row.key != 2) continue;
        if (row.selected == 1) {
            const std::wstring picked = PickFolderDialog(hDlg, st->data.fixedDir);
            if (!picked.empty()) st->data.fixedDir = picked;
            if (st->data.fixedDir.empty()) row.selected = 0;  // 没选到就退回“源文件同目录”
        }
        break;
    }
    RelayoutDialog(hDlg, st);
}

// 自绘窗口没有对话框管理器，Tab 焦点切换需要自己实现
void CycleFocus(UiDlgState* st, bool backwards) {
    std::vector<HWND> order;
    for (HWND h : st->tabOrder) {
        if (h && IsWindow(h) && IsWindowVisible(h) && IsWindowEnabled(h)) order.push_back(h);
    }
    if (order.empty()) return;

    HWND focus = GetFocus();
    int idx = -1;
    for (size_t i = 0; i < order.size(); ++i) {
        if (order[i] == focus) { idx = (int)i; break; }
    }
    const int n = (int)order.size();
    int next;
    if (backwards) next = (idx <= 0) ? n - 1 : idx - 1;
    else           next = (idx < 0 || idx + 1 >= n) ? 0 : idx + 1;
    SetFocus(order[next]);
}

// ---------------------------------------------------------------- 窗口过程

LRESULT CALLBACK UiDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    UiDlgState* st = reinterpret_cast<UiDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        st = reinterpret_cast<UiDlgState*>(cs->lpCreateParams);
        st->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        return TRUE;
    }

    case WM_CREATE: {
        if (!st) return -1;
        HINSTANCE inst = GetModuleHandleW(NULL);
        st->altBrush = CreateSolidBrush(UiColors().bgAlt);

        if (st->isPassword) {
            auto makeEdit = [&](int id, const RECT& r, bool visible) {
                HWND e = CreateWindowExW(0, L"EDIT", L"",
                    WS_CHILD | (visible ? WS_VISIBLE : 0) | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
                    r.left, r.top, r.right - r.left, r.bottom - r.top,
                    hwnd, (HMENU)(INT_PTR)id, inst, NULL);
                if (e) {
                    SendMessageW(e, WM_SETFONT, (WPARAM)UiFont(UiPx(14), false), TRUE);
                    SendMessageW(e, EM_SETLIMITTEXT, 255, 0);
                    SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                                 MAKELPARAM(UiPx(10), UiPx(10)));
                    ApplyEditTheme(e, st);
                }
                return e;
            };
            st->edit1 = makeEdit(IDC_UI_EDIT1, st->rEdit1Inner, true);
            st->edit2 = makeEdit(IDC_UI_EDIT2, st->rEdit2Inner, st->confirm);

            st->chkShow = CreateWindowExW(0, L"BUTTON", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                st->rChk.left, st->rChk.top, st->rChk.right - st->rChk.left,
                st->rChk.bottom - st->rChk.top,
                hwnd, (HMENU)(INT_PTR)IDC_UI_SHOW, inst, NULL);
            if (st->chkShow) {
                UiDetheme(st->chkShow);
                AttachHover(st->chkShow);
            }
        }

        // 按钮：先按 0 尺寸创建，随后统一从右向左排布
        for (size_t i = 0; i < st->btnIds.size(); ++i) {
            const int id = st->btnIds[i];
            HWND b = CreateWindowExW(0, L"BUTTON", ButtonLabel(id).c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, UiPx(10), st->btnH, hwnd, (HMENU)(INT_PTR)id, inst, NULL);
            if (b) {
                UiDetheme(b);
                SendMessageW(b, WM_SETFONT, (WPARAM)UiFont(UiPx(14), false), TRUE);
                AttachHover(b);
            }
            st->btns.push_back(b);
        }
        PositionButtons(st);

        // Tab 顺序：输入框 -> 复选框 -> 按钮
        if (st->edit1) st->tabOrder.push_back(st->edit1);
        if (st->confirm && st->edit2) st->tabOrder.push_back(st->edit2);
        if (st->chkShow) st->tabOrder.push_back(st->chkShow);
        for (HWND b : st->btns) if (b) st->tabOrder.push_back(b);

        UiApplyFrame(hwnd, true, true);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; // 背景由 WM_PAINT 完整绘制

    case WM_PAINT:
        if (st) PaintDialog(hwnd, st);
        return 0;

    case WM_CTLCOLOREDIT: {
        if (!st) break;
        const UiPalette& c = UiColors();
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, c.text);
        SetBkColor(dc, c.bgAlt);
        return reinterpret_cast<LRESULT>(st->altBrush ? st->altBrush : GetStockObject(WHITE_BRUSH));
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT pdis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
        if (!pdis || !st) break;
        if (pdis->CtlType == ODT_BUTTON) {
            if (pdis->CtlID == IDC_UI_SHOW) DrawOwnerCheckbox(pdis, st);
            else                            DrawOwnerButton(pdis);
            return TRUE;
        }
        break;
    }

    case WM_NCHITTEST: {
        const LRESULT hit = DefWindowProcW(hwnd, msg, wParam, lParam);
        if (hit != HTCLIENT || !st) return hit;
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        ScreenToClient(hwnd, &pt);
        if (st->isPassword) {
            // 输入框白框（比真实 EDIT 大一圈）也要接收点击，否则点到留白会变成拖动窗口
            if (PtInRect(&st->rEdit1, pt)) return HTCLIENT;
            if (st->confirm && PtInRect(&st->rEdit2, pt)) return HTCLIENT;
        }
        if (st->isSettings) {
            for (const auto& r : st->rows) {
                if (PtInRect(&r.segRc, pt)) return HTCLIENT;
            }
        }
        return HTCAPTION; // 其余区域允许拖动整个卡片
    }

    case WM_LBUTTONDOWN: {
        if (!st) break;
        const POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };

        if (st->isSettings) {
            for (size_t i = 0; i < st->rows.size(); ++i) {
                auto& row = st->rows[i];
                for (size_t k = 0; k < row.itemRc.size(); ++k) {
                    if (!PtInRect(&row.itemRc[k], pt)) continue;
                    st->focusedRow = (int)i;
                    if (row.selected != (int)k) {
                        row.selected = (int)k;          // 一次点击立刻切换
                        EnsureDlgAnimation(hwnd);       // 启动底色滑动动画
                        ApplySettingsRowChange(hwnd, st, row.key);
                        return 0;
                    }
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
            }
            return 0;
        }

        if (st->isPassword) {
            if (st->edit1 && PtInRect(&st->rEdit1, pt)) { SetFocus(st->edit1); return 0; }
            if (st->confirm && st->edit2 && PtInRect(&st->rEdit2, pt)) { SetFocus(st->edit2); return 0; }
        }
        break;
    }

    case WM_COMMAND: {
        if (!st) break;
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);

        if (code == EN_CHANGE && (id == IDC_UI_EDIT1 || id == IDC_UI_EDIT2)) {
            if (st->confirm) {
                wchar_t buf[256] = {};
                if (st->edit1) GetWindowTextW(st->edit1, buf, _countof(buf));
                st->strengthLevel = EvaluateStrengthLevel(buf);
                InvalidateRect(hwnd, &st->rStrength, FALSE);
            }
            if (!st->error.empty()) {
                st->error.clear();
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        if (id == IDC_UI_SHOW) {
            st->showPwd = !st->showPwd;
            // ApplyEditTheme 内部会重新写入文本以强制编辑控件立刻重绘
            ApplyEditTheme(st->edit1, st);
            ApplyEditTheme(st->edit2, st);
            if (st->chkShow) InvalidateRect(st->chkShow, NULL, FALSE);
            return 0;
        }

        if (id == IDOK || id == IDCANCEL || id == IDYES || id == IDNO) {
            if (st->isPassword && id == IDOK) {
                wchar_t buf1[256] = {};
                wchar_t buf2[256] = {};
                if (st->edit1) GetWindowTextW(st->edit1, buf1, _countof(buf1));
                if (st->confirm) {
                    if (st->edit2) GetWindowTextW(st->edit2, buf2, _countof(buf2));
                }
                else {
                    wcscpy_s(buf2, buf1);
                }

                if (buf1[0] == L'\0') {
                    st->error = tr(L"密码不能为空！", L"Password cannot be empty!");
                    MessageBeep(MB_ICONWARNING);
                    InvalidateRect(hwnd, NULL, FALSE);
                    if (st->edit1) SetFocus(st->edit1);
                    return 0;
                }
                if (wcscmp(buf1, buf2) != 0) {
                    st->error = tr(L"两次输入的密码不一致！", L"Passwords do not match!");
                    MessageBeep(MB_ICONWARNING);
                    InvalidateRect(hwnd, NULL, FALSE);
                    if (st->edit2) SetFocus(st->edit2);
                    return 0;
                }
                st->password = buf1;
            }
            st->result = id;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }

    case WM_TIMER:
        if (wParam == DLG_ANIM_TIMER && st) {
            TickDialogAnimation(hwnd, st);
            return 0;
        }
        break;

    case WM_CLOSE:
        if (st) {
            st->result = st->cancelId;
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        if (UiThemeRefresh() && st) {
            if (st->altBrush) {
                DeleteObject(st->altBrush);
                st->altBrush = CreateSolidBrush(UiColors().bgAlt);
            }
            UiApplyFrame(hwnd, true, true);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;

    case WM_DESTROY:
        if (st) {
            st->done = true;
            KillTimer(hwnd, DLG_ANIM_TIMER);
            st->animRunning = false;
            if (st->altBrush) { DeleteObject(st->altBrush); st->altBrush = nullptr; }
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool EnsureDialogClass() {
    static bool registered = false;
    static bool tried = false;
    if (registered) return true;
    if (tried) return false;
    tried = true;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = UiDlgProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = UIDLG_CLASS;

    registered = (RegisterClassExW(&wc) != 0);
    return registered;
}

// ---------------------------------------------------------------- 模态运行

int RunDialog(HWND owner, UiDlgState* st) {
    if (!EnsureDialogClass()) {
        return st->isPassword
            ? IDCANCEL
            : MessageBoxW(owner, st->text.c_str(), st->caption.c_str(),
                          st->buttons | MB_ICONINFORMATION);
    }

    // 先用内存 DC 计算尺寸
    {
        HDC screen = GetDC(NULL);
        HDC mem = CreateCompatibleDC(screen);
        ComputeLayout(st, mem);
        DeleteDC(mem);
        ReleaseDC(NULL, screen);
    }

    // 居中于属主窗口，并限制在工作区内
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    RECT anchor = work;
    if (owner && IsWindow(owner)) {
        RECT r;
        if (GetWindowRect(owner, &r) && !IsIconic(owner)) anchor = r;
    }
    int x = anchor.left + ((anchor.right - anchor.left) - st->winW) / 2;
    int y = anchor.top + ((anchor.bottom - anchor.top) - st->winH) / 2;
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;
    if (x + st->winW > work.right) x = work.right - st->winW;
    if (y + st->winH > work.bottom) y = work.bottom - st->winH;

    DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT;
    if (st->topmost) exStyle |= WS_EX_TOPMOST;

    HWND hwnd = CreateWindowExW(exStyle, UIDLG_CLASS, st->caption.c_str(),
        WS_POPUP | WS_CLIPCHILDREN, x, y, st->winW, st->winH,
        (owner && IsWindow(owner)) ? owner : NULL, NULL, GetModuleHandleW(NULL), st);
    if (!hwnd) {
        return st->isPassword
            ? IDCANCEL
            : MessageBoxW(owner, st->text.c_str(), st->caption.c_str(),
                          st->buttons | MB_ICONINFORMATION);
    }

    if (st->sound) PlayDialogSound(st->icon);

    const bool ownOwner = (owner && IsWindow(owner));
    if (ownOwner) EnableWindow(owner, FALSE);

    // 入场淡入：动画期间临时加 WS_EX_LAYERED（会让 ClearType 失效），
    // 淡入结束后立刻移除该样式，恢复正常的文字渲染。
    {
        const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
        st->layered = true;
        st->fadeAlpha = 0.0f;
    }

    ShowWindow(hwnd, SW_SHOW);
    if (st->topmost) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);

    if (st->edit1) SetFocus(st->edit1);
    else if (!st->btns.empty() && st->btns.front()) SetFocus(st->btns.front());

    EnsureDlgAnimation(hwnd);   // 启动淡入

    bool quit = false;
    MSG msg = {};
    while (!st->done) {
        const BOOL got = GetMessageW(&msg, NULL, 0, 0);
        if (got == 0) { quit = true; break; }
        if (got == -1) break;

        // 自绘窗口没有对话框管理器，回车/Esc/Tab 需显式处理
        if (msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_RETURN) {
                SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(st->primaryId, BN_CLICKED), 0);
                continue;
            }
            if (msg.wParam == VK_ESCAPE) {
                SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(st->cancelId, BN_CLICKED), 0);
                continue;
            }
            if (msg.wParam == VK_TAB) {
                if (st->isSettings) {
                    // 设置对话框：Tab/上下键在选项行之间移动
                    const int n = (int)st->rows.size();
                    if (n > 0) {
                        const int d = (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1;
                        st->focusedRow = ((st->focusedRow + d) % n + n) % n;
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                }
                else {
                    CycleFocus(st, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
                }
                continue;
            }
            if (st->isSettings && (msg.wParam == VK_DOWN || msg.wParam == VK_UP)) {
                const int n = (int)st->rows.size();
                if (n > 0) {
                    const int d = (msg.wParam == VK_UP) ? -1 : 1;
                    st->focusedRow = ((st->focusedRow + d) % n + n) % n;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                continue;
            }
            if (st->isSettings && (msg.wParam == VK_LEFT || msg.wParam == VK_RIGHT)) {
                const int n = (int)st->rows.size();
                if (n > 0 && st->focusedRow >= 0 && st->focusedRow < n) {
                    auto& row = st->rows[st->focusedRow];
                    const int cnt = (int)row.options.size();
                    int sel = row.selected + ((msg.wParam == VK_LEFT) ? -1 : 1);
                    if (sel < 0) sel = 0;
                    if (sel >= cnt) sel = cnt - 1;
                    if (sel != row.selected) {
                        row.selected = sel;
                        EnsureDlgAnimation(hwnd);
                        ApplySettingsRowChange(hwnd, st, row.key);
                    }
                    else {
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                }
                continue;
            }
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (quit) PostQuitMessage((int)msg.wParam);

    if (ownOwner && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }

    return st->result;
}

} // namespace

// ---------------------------------------------------------------- 对外接口

int UiShowMessage(HWND owner, const UiMessage& m) {
    UiDlgState st;
    st.caption = m.caption;
    st.text = m.text;
    st.icon = m.icon;
    st.buttons = m.buttons;
    st.topmost = m.topmost;
    st.sound = m.sound;

    switch (m.buttons & 0x0F) {
    case MB_YESNO:
    case MB_YESNOCANCEL:
        st.btnIds = { IDYES, IDNO };
        st.primaryId = IDYES;
        st.cancelId = IDNO;
        break;
    case MB_OKCANCEL:
        st.btnIds = { IDOK, IDCANCEL };
        st.primaryId = IDOK;
        st.cancelId = IDCANCEL;
        break;
    default:
        st.btnIds = { IDOK };
        st.primaryId = IDOK;
        st.cancelId = IDOK;
        break;
    }

    const int r = RunDialog(owner, &st);
    if ((m.buttons & 0x0F) == MB_OK && r == 0) return IDOK;
    return r;
}

bool UiShowPassword(HWND owner, bool confirm, std::string& outPassword,
                    const std::wstring& subtitle) {
    UiDlgState st;
    st.isPassword = true;
    st.confirm = confirm;
    st.subtitle = subtitle;
    st.sound = false;   // 密码框不需要提示音
    st.caption = confirm ? tr(L"设置密码", L"Set password") : tr(L"输入密码", L"Enter password");
    st.btnIds = { IDOK, IDCANCEL };
    st.primaryId = IDOK;
    st.cancelId = IDCANCEL;

    const int r = RunDialog(owner, &st);
    if (r == IDOK) {
        outPassword = WstringToUtf8(st.password);
        return true;
    }
    return false;
}

bool UiShowSettings(HWND owner, UiSettingsData& io) {
    UiDlgState st;
    st.isSettings = true;
    st.sound = false;   // 设置面板不需要提示音
    st.caption = tr(L"设置", L"Settings");
    st.data = io;
    st.btnIds = { IDOK, IDCANCEL };
    st.primaryId = IDOK;
    st.cancelId = IDCANCEL;

    auto addRow = [&](int key, const std::wstring& label,
                      std::vector<std::wstring> options, int selected) {
        UiDlgState::SegRow row;
        row.key = key;
        row.label = label;
        row.options = std::move(options);
        row.selected = selected;
        row.animSel = (float)selected;
        st.rows.push_back(std::move(row));
    };

    // 语言：当前为中文/English；后续要加别的语言，往这里追加选项即可
    addRow(0, tr(L"语言", L"Language"),
           { tr(L"中文", L"中文"), L"English" }, st.data.language);
    addRow(1, tr(L"主题", L"Theme"),
           { tr(L"跟随系统", L"System"), tr(L"浅色", L"Light"), tr(L"深色", L"Dark") },
           st.data.theme);
    addRow(2, tr(L"输出文件位置", L"Output location"),
           { tr(L"源文件同目录", L"Same folder"), tr(L"指定目录", L"Fixed folder"),
             tr(L"每次都询问", L"Ask each time") },
           st.data.outDir);
    addRow(3, tr(L"安全删除源文件", L"Securely delete source"),
           { tr(L"禁用", L"Off"), tr(L"覆写 1 次", L"1 pass"), tr(L"覆写 3 次", L"3 passes") },
           st.data.secureDelete == 0 ? 0 : (st.data.secureDelete == 1 ? 1 : 2));
    addRow(4, tr(L"完成后", L"When done"),
           { tr(L"关闭窗口", L"Close window"), tr(L"弹窗提示", L"Show dialog") },
           st.data.completion);
    addRow(5, tr(L"加密强度", L"Encryption strength"),
           { tr(L"快速", L"Fast"), tr(L"标准", L"Standard"),
             tr(L"安全", L"Secure"), tr(L"极强", L"Extreme") },
           st.data.strength);

    if (RunDialog(owner, &st) != IDOK) return false;

    io.language     = st.rows[0].selected;
    io.theme        = st.rows[1].selected;
    io.outDir       = st.rows[2].selected;
    io.fixedDir     = st.data.fixedDir;
    io.secureDelete = st.rows[3].selected == 0 ? 0 : (st.rows[3].selected == 1 ? 1 : 3);
    io.completion   = st.rows[4].selected;
    io.strength     = st.rows[5].selected;
    return true;
}
