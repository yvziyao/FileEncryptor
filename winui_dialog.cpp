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
#include "file_crypto.h"   // 随机密码生成（与 CLI 共用）
#include "FileEncryptor_helpers.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>
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
#define IDC_UI_CHK_PROT 2004
#define IDC_UI_TRIES    2005
#define IDC_UI_DAYS     2006
#define IDC_UI_CHK_DECOY 2007   // 可否认加密：使用诱饵文件
#define IDC_UI_PASSES    2008   // 设置：安全删除覆写次数（数字输入框）
#define IDC_GEN_PWD      2009   // 密码框：随机生成按钮（自绘，不是真控件）
#define IDC_UI_GENLEN    2010   // 设置：随机密码位数
#define IDC_SELECTABLE   2011   // 消息框：可选中复制的只读文本框

// 设置里「双击赞助」打开的页面（站点地址如需变更，改这一行即可）。
// #donate 对应 index.html 里的捐助区块。
static const wchar_t* kSponsorUrl = L"https://yvziyao.github.io/FileEncryptor/#donate";

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
    // 只有警告 / 错误 / 询问才发声。
    // 普通提示（Information，比如“加密成功”）一律静音 ——
    // 正常工作消息不该打扰用户，这也是现代应用的普遍做法。
    switch (icon) {
    case UiIcon::Error:    MessageBeep(MB_ICONERROR); break;
    case UiIcon::Warning:  MessageBeep(MB_ICONWARNING); break;
    case UiIcon::Question: MessageBeep(MB_ICONQUESTION); break;
    default:               break;   // Information：静音
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
static int g_modalDepth = 0;   // 当前打开的模态对话框层数
// 用户无视弹窗（切回主窗口）时的注意力提示定时器 ID
#define DLG_ATTENTION_TIMER 2
#define DLG_SHAKE_TIMER     3   // 弹窗「晃几下」用的高速定时器（30ms 一步）
#define DLG_ANIM_TIMER 1
#define DLG_ANIM_MS    15
// 选中底色滑动：固定时长，与跨几个选项无关（缓动曲线用 UiEaseOutCubic）
#define UI_SLIDE_DURATION 0.18f
// 关闭淡出时长（秒）
#define UI_CLOSE_FADE     0.12f

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
    HWND              chkDecoy = nullptr;          // 诱饵文件复选框
    bool              useDecoy = false;            // 是否启用诱饵
    bool              showDecoyOption = false;     // 是否提供诱饵选项（仅加密单文件）
    RECT              rChkDecoy{};
    RECT              rChkDecoyHelp{};      // 诱饵说明的问号图标
    HWND              hTip = nullptr;       // 悬停提示（TOOLTIPS_CLASS）
    bool              tipAdded = false;     // tooltip 工具是否已注册
    bool              helpHover = false;    // 鼠标是否停在「?」图标上
    std::wstring      tipText;              // 提示文本（tooltip 只存指针，必须留住）
    // 设置面板：「禁用诱饵文件」复选框
    int               decoyDisabled = 0;
    RECT              rDecoyBox{}, rDecoyLabel{}, rDecoyHelp{};
    // 设置面板：「加密后保留源文件」复选框
    int               keepSource = 0;
    RECT              rKeepBox{}, rKeepLabel{};

    // 密码框右侧的「随机生成」按钮（仅加密时显示）
    bool              showGenButton = false;
    RECT              rGenBtn{};
    bool              passwordGenerated = false;   // 本次密码由生成器填入
    // 设置面板：随机密码生成器（4 个类别复选框 + 位数输入框 + 弱提示）
    bool              genUpper = true, genLower = true, genDigits = true, genSymbols = true;
    RECT              rGenBox[4]{}, rGenLabel[4]{};
    int               genLen = 20;
    RECT              rGenLenLabel{}, rGenLenEdit{};
    HWND              editGenLen = nullptr;
    RECT              rGenHint{};
    std::wstring      genHintText;
    bool              genLenError = false;   // 位数超出 4-128（红色提示优先于黄色弱提示）
    // 弹窗强调：被用户无视时左右晃几下（比闪任务栏直观，且不依赖标题栏/任务栏）
    POINT             shakeBase{};
    int               shakeLeft = -1;   // -1 = 没有在晃
    // 消息框里「可选中复制」的一行（随机密码用）
    std::wstring      selectableText;
    bool              hasSelectable = false;
    RECT              rSelectable{};
    HWND              editSelectable = nullptr;
    // 设置面板左下角的「双击赞助」小按钮
    RECT              rSponsor{};
    ULONGLONG         sponsorLastClick = 0;
    // 设置面板：「在资源管理器右键菜单中显示」
    int               shellMenu = 1;
    RECT              rMenuBox{}, rMenuLabel{};
    // 设置面板：「安全删除覆写次数」数字输入框（第 3 行，替换原来的分段控件）
    HWND              editPasses = nullptr;
    RECT              rPassesEdit{}, rPassesHint{};
    std::wstring      passesHint;          // 空 = 不显示提示
    bool              passesError = false; // true = 红色错误，false = 橙色警告
    bool              passesValid = true;
    std::vector<HWND> btns;        // 与 btnIds / btnWidths 同序（左 -> 右）
    std::vector<int>  btnIds;
    std::vector<int>  btnWidths;
    // 自定义按钮文字（可选）：按 btnIds 的顺序一一对应。
    // 用于“强制关闭 / 后台运行 / 取消”这类不能靠 ID 推断措辞的对话框。
    std::vector<std::wstring> btnLabels;
    std::vector<HWND> tabOrder;
    int               primaryId = IDOK;
    int               cancelId = IDCANCEL;

    // 会话状态
    int          result = 0;
    bool         done = false;
    bool         showPwd = false;
    HWND         focusedEdit = nullptr;   // 当前获得焦点的输入框，用于画蓝色聚焦边框
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

    // ---- 防暴力破解设置区（仅“设置密码”时显示）----
    bool         showProtection = false;   // 是否显示该区域
    UiProtection prot;                     // 当前值（打开时回填，确定时读回）
    HWND         chkProt = nullptr;        // “输入错误密码 N 次后”复选框
    HWND         editTries = nullptr;      // 次数输入框（ES_NUMBER）
    HWND         editDays = nullptr;       // 锁定天数输入框（ES_NUMBER）
    RECT         rChkProt{};
    RECT         rTriesLabel{}, rTriesSuffix{}, rTriesEdit{};
    RECT         rSeg{}, rDaysLabel{}, rDaysEdit{};
    std::vector<RECT> protItemRc;          // 分段控件两项
    RECT         rRemain{};                // 解密时“还能尝试 X 次”红字
    int          remainTries = -1;         // <0 表示不显示

    HBRUSH altBrush = nullptr;

    // 动画
    bool        animRunning = false;
    bool        layered = false;      // 淡入期间临时使用分层窗口
    float       fadeAlpha = 0.0f;
    ULONGLONG   lastAnimTick = 0;
    // 关闭淡出：出现有动画，关闭也要有
    bool        closing = false;
    int         closeResult = IDCANCEL;
    float       closeAlpha = 0.0f;

    // ---- 设置对话框 ----
    struct SegRow {
        std::wstring              label;
        std::vector<std::wstring> options;
        int                       selected = 0;
        float                     animSel = 0.0f;   // 动画中的选中位置（0..n-1）
        // 固定时长的缓动：起点 + 目标 + 起始时刻。
        // 不能用“每帧固定步长”，否则距离越远耗时越长（快速->极强会是快速->标准的 3 倍）。
        float                     animFrom = 0.0f;
        float                     animTo = 0.0f;
        ULONGLONG                 animStart = 0;
        bool                      animActive = false;
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

// 取某个按钮的显示文字：优先用对话框自带的自定义文字（按 btnIds 顺序一一对应），
// 否则回退到按 ID 推断的默认措辞
std::wstring LabelFor(const UiDlgState* st, size_t index, int id) {
    if (st && index < st->btnLabels.size() && !st->btnLabels[index].empty())
        return st->btnLabels[index];
    return ButtonLabel(id);
}
std::wstring LabelForId(const UiDlgState* st, int id) {
    if (st) {
        for (size_t i = 0; i < st->btnIds.size(); ++i)
            if (st->btnIds[i] == id) return LabelFor(st, i, id);
    }
    return ButtonLabel(id);
}

void EnsureDlgAnimation(HWND hDlg);
UiDlgState* StateOf(HWND hDlg);
void BeginCloseDialog(HWND hDlg, UiDlgState* st, int result);
void StartRowSlide(UiDlgState::SegRow& row, int newSel);
void PositionButtons(UiDlgState* st);
void RelayoutDialog(HWND hDlg, UiDlgState* st);
void ApplySettingsRowChange(HWND hDlg, UiDlgState* st, int key);
// 按复选框状态启用/禁用保护区的输入框（关闭时置灰）
void SyncProtectionEnabled(UiDlgState* st);
void TickSettingsSlide(HWND hDlg, UiDlgState* st, float dt, bool& active);


// ---------------------------------------------------------------- 布局

// 设置对话框布局：标题 + 若干“标签 + 分段控件”行 + 按钮
// 密码框里「使用诱饵文件」的悬停说明
std::wstring DecoyTip() {
    return tr(
        L"勾选后可以再选一个「诱饵文件」并给它设一个假密码：\n"
        L"真密码解开的是真实文件，假密码解开的是诱饵文件。\n"
        L"两个槽位完全对称——文件本身看不出哪个是真、也没有「是否使用诱饵」的标志，\n"
        L"所以受胁迫时可以交出错的那个密码。\n"
        L"注意：文件体积约为明文的 2 倍；两个密码都知道的人两个文件都能解开。",
        L"When checked, you can pick a decoy file and give it a fake password:\n"
        L"the real password opens the real file, the fake one opens the decoy.\n"
        L"The two slots are completely symmetric -- nothing in the file reveals which\n"
        L"is which, nor even whether a decoy is used, so you can hand over the other\n"
        L"password under coercion.\n"
        L"Note: the file is about 2x the plaintext; anyone knowing both passwords can\n"
        L"open both files.");
}

// 定义在后面（绘制区），这里先声明，供布局函数调用
void SyncHelpTooltip(UiDlgState* st);

// 诱饵复选框的悬停说明文案
std::wstring DecoyTip();

// 设置面板里「禁用诱饵文件」复选框的文案（布局与绘制共用，必须一致）
std::wstring DecoyChkLabel() {
    return tr(L"禁用诱饵文件（加密后体积不再翻倍）", L"Disable decoy (no size doubling)");
}
// 「加密后保留源文件」复选框文案
// 随机密码生成器：4 个类别复选框的文字
std::wstring GenClassLabel(int i) {
    switch (i) {
    case 0:  return tr(L"大写字母", L"Uppercase");
    case 1:  return tr(L"小写字母", L"Lowercase");
    case 2:  return tr(L"数字", L"Digits");
    default: return tr(L"特殊符号", L"Symbols");
    }
}
std::wstring GenLenLabel() { return tr(L"随机密码位数", L"Generated password length"); }

// 按当前位数与类别数决定是否显示黄色弱提示。
// 规则：位数 < 12，或只勾了 1 类（甚至 0 类），都算偏弱。
void UpdateGenHint(UiDlgState* st) {
    // 超出范围是硬错误，优先显示（红色）；否则再看是否偏弱（黄色）
    if (st->genLenError) {
        st->genHintText = tr(L"位数需在 4-128 之间", L"Length must be between 4 and 128");
        return;
    }
    const int classes = (st->genUpper ? 1 : 0) + (st->genLower ? 1 : 0) +
                        (st->genDigits ? 1 : 0) + (st->genSymbols ? 1 : 0);
    if (st->genLen < 12 || classes <= 1) {
        st->genHintText = tr(
            L"位数偏短或字符类别过少，可能降低安全性",
            L"Short length or too few character classes may reduce security");
    }
    else {
        st->genHintText.clear();
    }
}

std::wstring KeepChkLabel() {
    return tr(L"加/解密后保留源文件（不删除）", L"Keep source files after encrypting/decrypting");
}

// 「在资源管理器右键菜单中显示」复选框文案
std::wstring MenuChkLabel() {
    return tr(L"在资源管理器右键菜单中显示", L"Show in Explorer context menu");
}

// 悬停说明
std::wstring DecoyChkTip() {
    return tr(
        L"勾选后，加密时只写入一个槽，文件体积约为原来的 1 倍（默认是 2 倍）。\n"
        L"代价：文件大小会暴露「这个文件没有诱饵」——单槽恰好是 1 倍、双槽是 2 倍，\n"
        L"因此「不可否认性」会减弱。只有确实不需要诱饵功能时才建议勾选。",
        L"When checked, only one slot is written, so the file is about 1x the size\n"
        L"(default is 2x).\n"
        L"Trade-off: the file size then reveals that there is no decoy -- a single-slot\n"
        L"file is exactly 1x while a dual-slot file is 2x, so deniability is weakened.\n"
        L"Only enable this if you do not need the decoy feature.");
}

void ComputeSettingsLayout(UiDlgState* st, HDC dc) {    HFONT fTitle = UiFont(UiPx(18), true);

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

    // 「禁用诱饵文件」复选框行（设置面板里唯一的复选框）
    {
        HFONT fBody = UiFont(UiPx(14), false);
        int lw = 0, lh = 0;
        MeasureText(dc, fBody, DecoyChkLabel(), UiPx(520), true, &lw, &lh);
        const int boxW = UiPx(18);
        const int chkH = UiPx(26);
        const int boxTop = y + (chkH - boxW) / 2;
        st->rDecoyBox   = { st->contentX, boxTop, st->contentX + boxW, boxTop + boxW };
        st->rDecoyLabel = { st->rDecoyBox.right + UiPx(8), y,
                            st->rDecoyBox.right + UiPx(8) + lw, y + chkH };
        const int hs = UiPx(16);
        const int hx = st->rDecoyLabel.right + UiPx(6);
        st->rDecoyHelp  = { hx, y + (chkH - hs) / 2, hx + hs, y + (chkH - hs) / 2 + hs };
        y += chkH + rowGap;
    }

    // 「加密后保留源文件」复选框（在诱饵复选框下面一行）
    {
        HFONT fBody = UiFont(UiPx(14), false);
        int lw = 0, lh = 0;
        MeasureText(dc, fBody, KeepChkLabel(), UiPx(520), true, &lw, &lh);
        const int boxW = UiPx(18);
        const int chkH = UiPx(26);
        const int boxTop = y + (chkH - boxW) / 2;
        st->rKeepBox   = { st->contentX, boxTop, st->contentX + boxW, boxTop + boxW };
        st->rKeepLabel = { st->rKeepBox.right + UiPx(8), y,
                           st->rKeepBox.right + UiPx(8) + lw, y + chkH };
        y += chkH + rowGap;
    }

    // 「在资源管理器右键菜单中显示」复选框
    {
        HFONT fBody = UiFont(UiPx(14), false);
        int lw = 0, lh = 0;
        MeasureText(dc, fBody, MenuChkLabel(), UiPx(520), true, &lw, &lh);
        const int boxW = UiPx(18);
        const int chkH = UiPx(26);
        const int boxTop = y + (chkH - boxW) / 2;
        st->rMenuBox   = { st->contentX, boxTop, st->contentX + boxW, boxTop + boxW };
        st->rMenuLabel = { st->rMenuBox.right + UiPx(8), y,
                           st->rMenuBox.right + UiPx(8) + lw, y + chkH };
        y += chkH + rowGap;
    }

    // ---- 随机密码生成器：4 个类别复选框（一行）+ 位数输入框 + 弱提示 ----
    {
        HFONT fBody = UiFont(UiPx(14), false);
        const int boxW = UiPx(18);
        const int chkH = UiPx(26);
        int x = st->contentX;
        for (int i = 0; i < 4; ++i) {
            const int boxTop = y + (chkH - boxW) / 2;
            st->rGenBox[i] = { x, boxTop, x + boxW, boxTop + boxW };
            int lw = 0, lh = 0;
            MeasureText(dc, fBody, GenClassLabel(i), UiPx(220), true, &lw, &lh);
            st->rGenLabel[i] = { x + boxW + UiPx(6), y, x + boxW + UiPx(6) + lw, y + chkH };
            x = st->rGenLabel[i].right + UiPx(18);
        }
        y += chkH + UiPx(6);

        st->rGenLenLabel = { st->contentX, y, st->contentX + labelW, y + rowH };
        const int editW = UiPx(72);
        const int editH = UiPx(28);
        const int editTop = y + (rowH - editH) / 2;
        st->rGenLenEdit = { st->contentX + labelW + UiPx(16), editTop,
                            st->contentX + labelW + UiPx(16) + editW, editTop + editH };
        y += rowH + UiPx(4);
        // 提示行始终占位：有提示时窗口尺寸不跳动
        st->rGenHint = { st->contentX + labelW + UiPx(16), y,
                         st->contentX + st->contentW, y + hintH };
        y += hintH + UiPx(6);
    }

    // 左下角留一个「双击赞助」的小按钮（右侧的确定/取消由 PositionButtons 右对齐）。
    // 宽度按**当前语言**的文字实测 —— 固定 112px 装不下英文 "Double-click to sponsor"。
    {
        int sw = 0, sh = 0;
        MeasureText(dc, UiFont(UiPx(12), false),
                    tr(L"☕ 双击赞助", L"☕ Double-click to sponsor"),
                    UiPx(420), true, &sw, &sh);
        const int pillW = sw + UiPx(26);   // 左右各留 13px 内边距
        const int pillTop = y + (st->btnH - UiPx(28)) / 2;
        st->rSponsor = { st->pad, pillTop, st->pad + pillW, pillTop + UiPx(28) };
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
            MeasureText(dc, fBody, LabelFor(st, i, st->btnIds[i]), UiPx(240), true, &tw, &th);
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
        SyncHelpTooltip(st);     // 布局确定后同步「?」图标的悬停区域
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

        // 防暴力破解设置区比较宽（复选框 + 次数输入框 + 分段控件），要单独算最小宽度
        if (st->showProtection) {
            int w = 0, h = 0;
            auto add = [&](const std::wstring& s) {
                int tw = 0, th = 0;
                MeasureText(dc, fBody, s, maxBodyW, true, &tw, &th);
                w += tw;
            };
            w += UiPx(18) + UiPx(8);                       // 复选框
            add(tr(L"输入错误密码", L"After"));
            w += UiPx(6) + UiPx(56) + UiPx(6);             // 次数输入框
            add(tr(L"次后", L"wrong attempts"));
            w += UiPx(12);
            add(tr(L"永久删除文件", L"Delete file"));
            w += UiPx(14) * 2 + UiPx(10);
            add(tr(L"禁止解密", L"Lock"));
            w += UiPx(14) * 2;
            if (w > minContent) minContent = w;
            (void)h;
        }

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
        MeasureText(dc, fBody, LabelFor(st, i, st->btnIds[i]), UiPx(240), true, &tw, &th);
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
        if (st->showGenButton) {
            // 按钮嵌在输入框右端内部，所以下面还要把 EDIT 的可视区往左收
            const int genW = UiPx(92);
            st->rGenBtn = { st->rEdit1.right - genW - UiPx(6),
                            st->rEdit1.top + UiPx(5),
                            st->rEdit1.right - UiPx(6),
                            st->rEdit1.bottom - UiPx(5) };
        }
        y += st->editH + gap;

        if (st->confirm) {
            st->rLabel2 = { st->contentX, y, st->contentX + st->contentW, y + labelH };
            y += labelH + UiPx(4);
            st->rEdit2 = { st->contentX, y, st->contentX + st->contentW, y + st->editH };
            y += st->editH + UiPx(6);
            st->rStrength = { st->contentX, y, st->contentX + st->contentW, y + UiPx(18) };
            y += UiPx(18) + UiPx(8);

            // ---- 防暴力破解设置区（密码强度下方、显示密码上方）----
            if (st->showProtection) {
                const int rowH = UiPx(28);
                const int boxW = UiPx(18);
                const int editW = UiPx(56);

                int x = st->contentX;
                st->rChkProt = { x, y, x + boxW, y + rowH };
                x += boxW + UiPx(8);

                int tw = 0, th = 0;
                MeasureText(dc, fBody, tr(L"输入错误密码", L"After"), maxBodyW, true, &tw, &th);
                st->rTriesLabel = { x, y, x + tw, y + rowH };
                x += tw + UiPx(6);

                st->rTriesEdit = { x, y + (rowH - UiPx(26)) / 2, x + editW, y + (rowH - UiPx(26)) / 2 + UiPx(26) };
                x += editW + UiPx(6);

                MeasureText(dc, fBody, tr(L"次后", L"wrong attempts"), maxBodyW, true, &tw, &th);
                st->rTriesSuffix = { x, y, x + tw, y + rowH };
                x += tw + UiPx(12);

                // 分段控件：两项等宽
                int w1 = 0, w2 = 0;
                MeasureText(dc, fBody, tr(L"永久删除文件", L"Delete file"), maxBodyW, true, &w1, &th);
                MeasureText(dc, fBody, tr(L"禁止解密", L"Lock"), maxBodyW, true, &w2, &th);
                const int item1 = w1 + UiPx(28);
                const int item2 = w2 + UiPx(28);
                st->rSeg = { x, y, x + item1 + item2, y + rowH };
                st->protItemRc.clear();
                st->protItemRc.push_back({ x, y, x + item1, y + rowH });
                st->protItemRc.push_back({ x + item1, y, x + item1 + item2, y + rowH });
                y += rowH + UiPx(6);

                // 第二行：锁定时长（仅“禁止解密”模式下显示）
                if (st->prot.action == 1) {
                    int lw = 0;
                    MeasureText(dc, fBody, tr(L"锁定时长", L"Lock for"), maxBodyW, true, &lw, &th);
                    int dx = st->contentX + boxW + UiPx(8);
                    st->rDaysLabel = { dx, y, dx + lw, y + rowH };
                    dx += lw + UiPx(6);
                    st->rDaysEdit = { dx, y + (rowH - UiPx(26)) / 2, dx + editW, y + (rowH - UiPx(26)) / 2 + UiPx(26) };
                    y += rowH + UiPx(6);
                }
                else {
                    st->rDaysLabel = {};
                    st->rDaysEdit = {};
                }
            }
            else {
                st->rChkProt = {};
                st->rTriesEdit = {};
                st->rDaysEdit = {};
                st->protItemRc.clear();
            }
        }
        else {
            // 解密：受保护文件在密码框下方用红字提示还能尝试几次
            if (st->remainTries >= 0) {
                st->rRemain = { st->contentX, y, st->contentX + st->contentW, y + UiPx(20) };
                y += UiPx(20) + UiPx(6);
            }
            else {
                y += UiPx(8);
            }
        }

        st->rChk = { st->contentX, y, st->contentX + st->contentW, y + UiPx(22) };
        y += UiPx(22) + UiPx(6);

        // 诱饵文件（可否认加密）：只在加密单个文件时提供
        if (st->showDecoyOption) {
            // 复选框宽度只包住文字，右边留出「?」说明图标的位置
            int lw = 0, lh = 0;
            MeasureText(dc, UiFont(UiPx(14), false),
                        tr(L"使用诱饵文件（可否认加密）", L"Use a decoy file (deniable)"),
                        UiPx(420), true, &lw, &lh);
            const int chkH = UiPx(22);
            const int chkW = UiPx(18) + UiPx(10) + lw;   // 与 DrawOwnerCheckbox 的 box+gap 对齐
            st->rChkDecoy = { st->contentX, y, st->contentX + chkW, y + chkH };
            const int hs = UiPx(16);
            const int hx = st->rChkDecoy.right + UiPx(4);
            st->rChkDecoyHelp = { hx, y + (chkH - hs) / 2, hx + hs, y + (chkH - hs) / 2 + hs };
            y += chkH + UiPx(6);
        }
        else {
            st->rChkDecoy = {};
            st->rChkDecoyHelp = {};
        }

        // 错误行始终预留，避免出现错误时窗口尺寸变化
        st->rError = { st->contentX, y, st->contentX + st->contentW, y + UiPx(18) };
        y += UiPx(18) + UiPx(12);
    }
    else {
        if (!st->caption.empty() || hasIcon) y += st->titleH + UiPx(10);
        st->rBody = { st->contentX, y, st->contentX + st->contentW, y + bodyH };
        y += bodyH + UiPx(10);

        // 「可选中复制」的行（随机密码）：正文下面放一个只读编辑框
        if (st->hasSelectable) {
            st->rSelectable = { st->contentX, y, st->contentX + st->contentW, y + UiPx(30) };
            y += UiPx(30) + UiPx(14);
        }
        else {
            st->rSelectable = {};
            y += UiPx(14);
        }
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
    if (st->showGenButton) {
        // 给右边的「随机生成」按钮让出位置，否则文字会压在按钮下面
        st->rEdit1Inner.right = st->rGenBtn.left - UiPx(6);
    }

    if (hasIcon) {
        st->iconX = st->pad;
        st->iconY = st->titleY + (st->titleH - st->iconSize) / 2;
    }
    else {
        st->iconSize = 0;
    }

    SyncHelpTooltip(st);     // 布局确定后同步「?」图标的悬停区域
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
    const UiDlgState* dlgSt = StateOf(GetParent(pdis->hwndItem));
    // 蓝色主按钮：优先用对话框指定的，默认仍按 ID 推断
    const bool primary = (dlgSt && dlgSt->primaryId)
        ? (pdis->CtlID == dlgSt->primaryId)
        : (pdis->CtlID == IDOK || pdis->CtlID == IDYES);
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

    const std::wstring label = LabelForId(StateOf(GetParent(pdis->hwndItem)), pdis->CtlID);
    RECT textRc = rc;
    // 强调色按钮用灰度抗锯齿，避免 ClearType 在饱和底色上产生彩边
    UiDrawTextLine(dc, label, textRc, UiFont(UiPx(14), false, primary), textColor,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// 自绘复选框。密码框里有两个：
//   IDC_UI_SHOW       “显示密码”，自带文字
//   IDC_UI_CHK_PROT   防暴力破解开关，只画方框（后面的说明文字由父窗口绘制，
//                     这样它才能与次数输入框、分段控件排在同一行）
// 画一个圆圈问号：鼠标悬停时由 tooltip 显示具体说明
void DrawHelpIcon(HDC dc, const RECT& rc) {
    if (IsRectEmpty(&rc)) return;
    const UiPalette& c = UiColors();
    const int d = (rc.bottom - rc.top);
    UiDrawRoundRect(dc, rc, d / 2, true, c.bgAlt, true, c.border);
    HFONT f = UiFont(UiPx(11), true);
    if (f) {
        RECT r = rc;
        UiDrawTextLine(dc, L"?", r, f, c.textMuted,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

// 读取「安全删除覆写次数」输入框并刷新提示；返回是否合法。
// 规则：0-10 之外 -> 红色错误；4-10 -> 橙色警告（可能伤磁盘、且变慢）；0-3 -> 无提示。
static bool ValidatePasses(UiDlgState* st) {
    if (!st || !st->editPasses) return true;

    wchar_t buf[16] = {};
    GetWindowTextW(st->editPasses, buf, _countof(buf));

    bool digitsOnly = (buf[0] != L'\0');
    for (const wchar_t* p = buf; *p; ++p) {
        if (*p < L'0' || *p > L'9') { digitsOnly = false; break; }
    }

    const int v = digitsOnly ? _wtoi(buf) : -1;
    // 范围 1-10：0 对「安全删除」没有意义（不覆写就不是安全删除）
    if (!digitsOnly || v < 1 || v > 10) {
        st->passesHint = tr(L"错误：请输入1-10之间的数字",
                            L"Error: please enter a number between 1 and 10");
        st->passesError = true;
        st->passesValid = false;
        return false;
    }
    if (v > 3) {
        st->passesHint = tr(L"注意：覆写次数过多可能减少磁盘使用寿命，并降低删除速度",
                            L"Note: too many passes may reduce disk lifespan and slow deletion");
        st->passesError = false;
        st->passesValid = true;
        return true;
    }
    st->passesHint.clear();
    st->passesError = false;
    st->passesValid = true;
    return true;
}

// 读取「?」图标所在的矩形（设置面板 / 密码框）；没有则返回 nullptr
const RECT* HelpIconRect(const UiDlgState* st) {
    if (!st) return nullptr;
    if (st->isSettings) return &st->rDecoyHelp;
    if (st->isPassword && st->showDecoyOption) return &st->rChkDecoyHelp;
    return nullptr;
}

// 自绘的悬停提示窗。
//
// 为什么不用通用控件的 TOOLTIPS_CLASS：实测 `CreateWindowExW(TOOLTIPS_CLASSW, ...)`
// 在这个进程里始终返回 NULL（窗口枚举里根本没有 tooltip 窗口），提示框一次都不弹。
// 这套 UI 本来就是全自绘的，提示框也自己画，样式更统一、也不依赖 comctl32 的行为。
LRESULT CALLBACK HelpTipProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        const UiPalette& c = UiColors();
        UiFillRect(dc, rc, c.bgAlt);
        UiDrawRoundRect(dc, rc, UiPx(6), false, c.bgAlt, true, c.border);
        const UiDlgState* st =
            reinterpret_cast<const UiDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (st && !st->tipText.empty()) {
            RECT r = rc;
            InflateRect(&r, -UiPx(10), -UiPx(8));
            UiDrawTextLine(dc, st->tipText, r, UiFont(UiPx(12), false), c.text,
                           DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// 每次布局后调用：把当前该显示的说明文本存起来（悬停时才真正弹窗）
void SyncHelpTooltip(UiDlgState* st) {
    if (!st) return;
    if (st->isSettings)                        st->tipText = DecoyChkTip();
    else if (st->isPassword && st->showDecoyOption) st->tipText = DecoyTip();
    else                                       st->tipText.clear();
}

// 显示 / 隐藏「?」的悬停提示（自己驱动，不依赖 comctl32 的 tooltip）
void SetHelpTipVisible(UiDlgState* st, bool visible) {
    if (!st || !st->hwnd) return;
    if (visible == st->helpHover) return;
    st->helpHover = visible;

    if (!visible) {
        if (st->hTip) ShowWindow(st->hTip, SW_HIDE);
        return;
    }

    const RECT* rc = HelpIconRect(st);
    if (!rc || IsRectEmpty(rc) || st->tipText.empty()) return;

    if (!st->hTip) {
        static bool registered = false;
        HINSTANCE inst = GetModuleHandleW(nullptr);
        if (!registered) {
            WNDCLASSEXW wc = {};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = HelpTipProc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = L"FileEncryptorHelpTip";
            RegisterClassExW(&wc);
            registered = true;
        }
        st->hTip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                   L"FileEncryptorHelpTip", L"", WS_POPUP,
                                   0, 0, 10, 10, st->hwnd, nullptr, inst, nullptr);
        if (!st->hTip) return;
        SetWindowLongPtrW(st->hTip, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
    }

    // 按文本量算尺寸：固定最大宽度，高度自适应
    const int maxTextW = UiPx(340);
    HDC dc = GetDC(st->hTip);
    HFONT f = UiFont(UiPx(12), false);
    HGDIOBJ old = f ? SelectObject(dc, f) : nullptr;
    RECT calc = { 0, 0, maxTextW, 0 };
    DrawTextW(dc, st->tipText.c_str(), -1, &calc,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    if (old) SelectObject(dc, old);
    ReleaseDC(st->hTip, dc);
    int w = calc.right + UiPx(20);
    int h = calc.bottom + UiPx(16);

    POINT p = { rc->left, rc->bottom + UiPx(6) };
    ClientToScreen(st->hwnd, &p);

    // 贴到屏幕外就翻到上方 / 往里收
    HMONITOR mon = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfoW(mon, &mi)) {
        if (p.x + w > mi.rcWork.right)  p.x = mi.rcWork.right - w - UiPx(4);
        if (p.x < mi.rcWork.left)       p.x = mi.rcWork.left + UiPx(4);
        if (p.y + h > mi.rcWork.bottom) {
            POINT up = { rc->left, rc->top - UiPx(6) };
            ClientToScreen(st->hwnd, &up);
            p.y = up.y - h;
            if (p.y < mi.rcWork.top) p.y = mi.rcWork.top + UiPx(4);
        }
    }

    SetWindowPos(st->hTip, HWND_TOPMOST, p.x, p.y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(st->hTip, NULL, TRUE);
}

void DrawOwnerCheckbox(LPDRAWITEMSTRUCT pdis, const UiDlgState* st) {    const UiPalette& c = UiColors();
    RECT rc = pdis->rcItem;
    UiDoubleBuffer buffer(pdis->hDC, rc);
    HDC dc = buffer.Dc();
    UiFillRect(dc, rc, c.bg);   // 铺满，避免残留
    const bool isProt  = (pdis->CtlID == IDC_UI_CHK_PROT);
    const bool isDecoy = (pdis->CtlID == IDC_UI_CHK_DECOY);
    const bool checked = isProt ? st->prot.enabled
                                : (isDecoy ? st->useDecoy : st->showPwd);

    const int box = UiPx(18);
    RECT boxRc = { rc.left, rc.top + (rc.bottom - rc.top - box) / 2,
                   rc.left + box, rc.top + (rc.bottom - rc.top - box) / 2 + box };

    if (checked) {
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

    if (isProt) return;   // 说明文字由父窗口画

    RECT labelRc = { boxRc.right + UiPx(10), rc.top, rc.right, rc.bottom };
    UiDrawTextLine(dc,
                   isDecoy ? tr(L"使用诱饵文件（可否认加密）", L"Use a decoy file (deniable)")
                           : tr(L"显示密码", L"Show password"),
                   labelRc, UiFont(UiPx(14), false), c.text,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

// 设置对话框：分段控件（SegmentedControl）风格的自绘行
// 警告色（橙色）：调色板里没有，按深浅色各给一套
static COLORREF WarningColor() {
    return UiIsDark() ? RGB(255, 190, 92) : RGB(196, 108, 0);
}

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

    // 「禁用诱饵文件」复选框（设置面板里唯一的复选框）
    if (!IsRectEmpty(&st->rDecoyBox)) {
        if (st->decoyDisabled) {
            UiDrawRoundRect(dc, st->rDecoyBox, UiPx(4), true, c.accent, false, c.accent);
            HFONT iconFont = UiIconFont(UiPx(11));
            if (iconFont) {
                RECT gr = st->rDecoyBox;
                UiDrawTextLine(dc, L"\uE73E", gr, iconFont, c.onAccent,
                               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        } else {
            UiDrawRoundRect(dc, st->rDecoyBox, UiPx(4), true, c.bgAlt, true, c.border);
        }
        RECT lr = st->rDecoyLabel;
        UiDrawTextLine(dc, DecoyChkLabel(), lr, fBody, c.text,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawHelpIcon(dc, st->rDecoyHelp);
    }

    // 「加密后保留源文件」复选框
    if (!IsRectEmpty(&st->rKeepBox)) {
        if (st->keepSource) {
            UiDrawRoundRect(dc, st->rKeepBox, UiPx(4), true, c.accent, false, c.accent);
            HFONT iconFont = UiIconFont(UiPx(11));
            if (iconFont) {
                RECT gr = st->rKeepBox;
                UiDrawTextLine(dc, L"\uE73E", gr, iconFont, c.onAccent,
                               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        } else {
            UiDrawRoundRect(dc, st->rKeepBox, UiPx(4), true, c.bgAlt, true, c.border);
        }
        RECT lr = st->rKeepLabel;
        UiDrawTextLine(dc, KeepChkLabel(), lr, fBody, c.text,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // 「在资源管理器右键菜单中显示」复选框
    if (!IsRectEmpty(&st->rMenuBox)) {
        if (st->shellMenu) {
            UiDrawRoundRect(dc, st->rMenuBox, UiPx(4), true, c.accent, false, c.accent);
            HFONT iconFont = UiIconFont(UiPx(11));
            if (iconFont) {
                RECT gr = st->rMenuBox;
                UiDrawTextLine(dc, L"\uE73E", gr, iconFont, c.onAccent,
                               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        } else {
            UiDrawRoundRect(dc, st->rMenuBox, UiPx(4), true, c.bgAlt, true, c.border);
        }
        RECT mr = st->rMenuLabel;
        UiDrawTextLine(dc, MenuChkLabel(), mr, fBody, c.text,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // 随机密码生成器：4 个类别复选框
    for (int i = 0; i < 4; ++i) {
        if (IsRectEmpty(&st->rGenBox[i])) continue;
        const bool on = (i == 0) ? st->genUpper : (i == 1) ? st->genLower
                       : (i == 2) ? st->genDigits : st->genSymbols;
        if (on) {
            UiDrawRoundRect(dc, st->rGenBox[i], UiPx(4), true, c.accent, false, c.accent);
            HFONT iconFont = UiIconFont(UiPx(11));
            if (iconFont) {
                RECT gr = st->rGenBox[i];
                UiDrawTextLine(dc, L"\uE73E", gr, iconFont, c.onAccent,
                               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
        else {
            UiDrawRoundRect(dc, st->rGenBox[i], UiPx(4), true, c.bgAlt, true, c.border);
        }
        RECT lr = st->rGenLabel[i];
        UiDrawTextLine(dc, GenClassLabel(i), lr, fBody, c.text,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // 位数：标签 + 输入框 + 黄色弱提示
    if (!IsRectEmpty(&st->rGenLenLabel)) {
        RECT ll = st->rGenLenLabel;
        UiDrawTextLine(dc, GenLenLabel(), ll, fBody, c.text,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT well = st->rGenLenEdit;
        InflateRect(&well, UiPx(3), UiPx(3));
        UiDrawRoundRect(dc, well, UiPx(5), true, c.bgAlt, true, c.border);
        if (!st->genHintText.empty()) {
            const COLORREF hintColor = st->genLenError ? c.danger : WarningColor();
            UiDrawTextLine(dc, st->genHintText, st->rGenHint, fSmall, hintColor,
                           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    // 左下角「双击赞助」：不做成独立控件，直接自绘一个低调的小胶囊
    if (!IsRectEmpty(&st->rSponsor)) {
        UiDrawRoundRect(dc, st->rSponsor, UiPx(6), true, c.bgAlt, true, c.border);
        RECT sr = st->rSponsor;
        UiDrawTextLine(dc, tr(L"☕ 双击赞助", L"☕ Double-click to sponsor"), sr,
                       UiFont(UiPx(12), false), c.textMuted,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
        // 获得焦点的输入框用强调色描边，让用户一眼看出光标在哪个框里
        auto drawWell = [&](const RECT& r, bool visible, HWND edit) {
            if (!visible) return;
            RECT w = r;
            InflateRect(&w, UiPx(1), UiPx(1));
            const bool focused = (edit != nullptr && st->focusedEdit == edit);
            UiDrawRoundRect(dc, w, UiPx(5), true, c.bgAlt, true,
                            focused ? c.accent : wellBorder);
        };
        // 消息框里的只读文本框（画个底，真正的文字由 EDIT 自己绘制）
        if (st->hasSelectable && !IsRectEmpty(&st->rSelectable)) {
            RECT w = st->rSelectable;
            InflateRect(&w, UiPx(1), UiPx(1));
            UiDrawRoundRect(dc, w, UiPx(5), true, c.bgAlt, true, c.border);
        }

        drawWell(st->rEdit1, st->edit1 != nullptr, st->edit1);
        drawWell(st->rEdit2, st->edit2 != nullptr && IsWindowVisible(st->edit2), st->edit2);

        // 密码框右侧的「随机生成」小按钮
        if (st->showGenButton && !IsRectEmpty(&st->rGenBtn)) {
            UiDrawRoundRect(dc, st->rGenBtn, UiPx(4), true, c.bgAlt, true, c.border);
            UiDrawTextLine(dc, tr(L"随机生成", L"Generate"), st->rGenBtn,
                           UiFont(UiPx(12), false), c.accent,
                           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

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

        // ---- 防暴力破解设置区（仅加密）----
        if (st->confirm && st->showProtection) {
            const bool on = st->prot.enabled;
            const COLORREF labelColor = on ? c.text : c.textMuted;

            UiDrawTextLine(dc, tr(L"输入错误密码", L"After"), st->rTriesLabel, fBody, labelColor,
                           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            UiDrawTextLine(dc, tr(L"次后", L"wrong attempts"), st->rTriesSuffix, fBody, labelColor,
                           DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // 分段控件容器 + 选中底色（与设置面板同款观感）
            UiDrawRoundRect(dc, st->rSeg, UiPx(5), true, c.bgAlt, true, c.border);
            for (size_t k = 0; k < st->protItemRc.size(); ++k) {
                RECT ir = st->protItemRc[k];
                InflateRect(&ir, -UiPx(2), -UiPx(2));
                const bool sel = on && ((int)k == st->prot.action);
                if (sel) UiDrawRoundRect(dc, ir, UiPx(4), true, c.accent, false, c.accent);
                const std::wstring label = (k == 0) ? tr(L"永久删除文件", L"Delete file")
                                                    : tr(L"禁止解密", L"Lock");
                UiDrawTextLine(dc, label, ir, fBody,
                               sel ? c.onAccent : (on ? c.text : c.textMuted),
                               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }

            // 第二行：锁定时长（仅“禁止解密”模式）
            if (st->prot.action == 1) {
                UiDrawTextLine(dc, tr(L"锁定时长", L"Lock for"), st->rDaysLabel, fBody, labelColor,
                               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                int dw = 0, dh = 0;
                MeasureText(dc, fBody, tr(L"天", L"days"), UiPx(120), true, &dw, &dh);
                RECT dr = st->rDaysEdit;
                dr.left = dr.right + UiPx(6);
                dr.right = dr.left + dw;
                UiDrawTextLine(dc, tr(L"天", L"days"), dr, fBody, labelColor,
                               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }

            // 数字输入框的聚焦边框：EDIT 是子窗口、父窗口画不到它内部，
            // 所以向外扩 2px 画一圈强调色描边
            auto focusRing = [&](const RECT& r, HWND edit) {
                if (edit == nullptr || st->focusedEdit != edit) return;
                RECT fr = r;
                InflateRect(&fr, UiPx(2), UiPx(2));
                UiDrawRoundRect(dc, fr, UiPx(5), false, c.bg, true, c.accent);
            };
            focusRing(st->rTriesEdit, st->editTries);
            focusRing(st->rDaysEdit, st->editDays);
        }

        // ---- 解密：红字提示还能尝试几次 ----
        if (!st->confirm && st->remainTries >= 0) {
            const std::wstring s = tr(L"还能尝试 ", L"Attempts left: ")
                                 + std::to_wstring(st->remainTries)
                                 + tr(L" 次", L"");
            UiDrawTextLine(dc, s, st->rRemain, fBody, c.danger,
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

    // 诱饵复选框右侧的「?」说明图标（复选框本身是子控件，图标由父窗口画）
    if (st->isPassword && st->showDecoyOption)
        DrawHelpIcon(dc, st->rChkDecoyHelp);

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

// 定义在文件后面（主题相关的编辑框样式）
void ApplyEditTheme(HWND edit, const UiDlgState* st, bool mask = true);

// 随机生成一个密码，同时填入「密码」和「确认」两个框，并切换成明文，
// 方便用户核对并复制保存（密文点号状态没法复制）。
static void DoGeneratePassword(HWND hwnd, UiDlgState* st) {
    if (!st) return;
    fe::PasswordRules rules;
    rules.length  = Settings().pwGenLength;
    rules.upper   = Settings().pwGenUpper;
    rules.lower   = Settings().pwGenLower;
    rules.digits  = Settings().pwGenDigits;
    rules.symbols = Settings().pwGenSymbols;

    const std::string pw = fe::GeneratePassword(rules);
    const std::wstring wpw(pw.begin(), pw.end());

    st->showPwd = true;                      // 明文：否则用户看不到、也复制不了
    if (st->edit1) { SetWindowTextW(st->edit1, wpw.c_str()); ApplyEditTheme(st->edit1, st); }
    if (st->edit2) { SetWindowTextW(st->edit2, wpw.c_str()); ApplyEditTheme(st->edit2, st); }
    if (st->chkShow) {
        SendMessageW(st->chkShow, BM_SETCHECK, BST_CHECKED, 0);
        InvalidateRect(st->chkShow, NULL, FALSE);
    }
    // 全选：按 Ctrl+C 就能直接复制
    if (st->edit1) { SetFocus(st->edit1); SendMessageW(st->edit1, EM_SETSEL, 0, -1); }

    st->passwordGenerated = true;
    if (hwnd) InvalidateRect(hwnd, NULL, FALSE);
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

// 按**当前**客户区尺寸重算圆角窗口区域。
// 必须这样做的原因：控件若在尺寸为 0 时创建（例如“锁定时长”输入框在
// “永久删除文件”模式下布局矩形是空的），CreateRoundRectRgn 会因为圆角
// 直径大于盒子而得到一个**空区域**；之后即使把它挪到正确位置，窗口区域
// 也不会自动重算，结果就是布局看着对、却既画不出来也点不到。
void ApplyEditRoundRegion(HWND edit) {
    if (!edit) return;
    RECT rc;
    GetClientRect(edit, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;   // 尺寸还没定下来，等下次
    const int r = UiPx(5) * 2;
    HRGN rgn = CreateRoundRectRgn(0, 0, rc.right + 1, rc.bottom + 1, r, r);
    if (rgn) SetWindowRgn(edit, rgn, TRUE);
}

// mask=true 时套用密码掩码。保护区的数字输入框必须传 false，
// 否则会被当成密码框显示成一串星号。
void ApplyEditTheme(HWND edit, const UiDlgState* st, bool mask) {
    if (!edit) return;
    UiDetheme(edit);
    ApplyEditRoundRegion(edit);
    if (mask) SetEditPasswordMask(edit, st->showPwd);
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

// 选中项变化时启动固定时长的滑动动画
void StartRowSlide(UiDlgState::SegRow& row, int newSel) {
    row.animFrom = row.animSel;
    row.animTo = (float)newSel;
    row.animStart = GetTickCount64();
    row.animActive = (row.animFrom != row.animTo);
}

// 关闭请求统一走这里：先淡出，动画结束后由 TickDialogAnimation 真正销毁窗口。
// 直接 DestroyWindow 的话关闭就没有动画了。
void BeginCloseDialog(HWND hDlg, UiDlgState* st, int result) {
    if (!st || st->closing) return;
    st->closing = true;
    st->closeResult = result;
    st->closeAlpha = 255.0f;
    st->layered = false;              // 放弃淡入，直接转为淡出
    LONG_PTR ex = GetWindowLongPtrW(hDlg, GWL_EXSTYLE);
    SetWindowLongPtrW(hDlg, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    SetLayeredWindowAttributes(hDlg, 0, 255, LWA_ALPHA);
    EnsureDlgAnimation(hDlg);
}

void TickDialogAnimation(HWND hDlg, UiDlgState* st) {
    const ULONGLONG now = GetTickCount64();
    float dt = st->lastAnimTick ? (float)(now - st->lastAnimTick) / 1000.0f
                                : (float)DLG_ANIM_MS / 1000.0f;
    st->lastAnimTick = now;
    if (dt <= 0.0f) dt = 0.001f;
    if (dt > 0.12f) dt = 0.12f;

    bool active = false;

    // 0) 关闭淡出：出现有动画，关闭也要有；淡完再真正销毁窗口
    if (st->closing) {
        st->closeAlpha -= 255.0f * dt / UI_CLOSE_FADE;
        if (st->closeAlpha <= 0.0f) {
            st->closeAlpha = 0.0f;
            st->result = st->closeResult;
            DestroyWindow(hDlg);
            return;
        }
        SetLayeredWindowAttributes(hDlg, 0, (BYTE)st->closeAlpha, LWA_ALPHA);
        active = true;
    }

    // 入场淡入（约 140ms）；结束后移除分层样式，恢复正常渲染（不影响 ClearType）
    if (!st->closing && st->layered) {
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

// 设置对话框：选中底色的滑动动画。
// 用「起点 + 目标 + 起始时刻」做固定时长的 ease-out，
// 这样无论跨几个选项耗时都一样；之前每帧走固定步长，距离越远拖得越久。
void TickSettingsSlide(HWND hDlg, UiDlgState* st, float /*dt*/, bool& active) {
    const ULONGLONG now = GetTickCount64();
    bool moving = false;
    for (auto& row : st->rows) {
        if (!row.animActive) continue;
        const float t = (float)(now - row.animStart) / 1000.0f / UI_SLIDE_DURATION;
        if (t >= 1.0f) {
            row.animSel = row.animTo;
            row.animActive = false;
        }
        else {
            row.animSel = row.animFrom + (row.animTo - row.animFrom) * UiEaseOutCubic(t);
            active = true;
        }
        // 只重画这一行的分段控件，不整窗重绘——整窗重绘每帧要重建几十个
        // D2D 渲染目标，是滑动卡顿的主要原因
        RECT r = row.segRc;
        InflateRect(&r, UiPx(2), UiPx(2));
        InvalidateRect(hDlg, &r, FALSE);
        moving = true;
    }
    (void)moving;
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
// 把密码框的子控件摆到 ComputeLayout 算出的位置。
// 保护区分段控件切换模式时窗口高度会变，必须重新摆放（含显示/隐藏锁定时长输入框）。
void PositionPasswordControls(UiDlgState* st) {
    if (!st->isPassword) return;
    auto move = [](HWND h, const RECT& r, bool visible) {
        if (!h) return;
        SetWindowPos(h, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        // 尺寸变了必须重算圆角区域，否则从 0 尺寸创建出来的控件会一直是空区域
        wchar_t cls[32] = {};
        GetClassNameW(h, cls, 32);
        if (wcscmp(cls, L"Edit") == 0) ApplyEditRoundRegion(h);
        ShowWindow(h, visible ? SW_SHOW : SW_HIDE);
    };
    RECT none = {};
    move(st->edit1, st->rEdit1Inner, true);
    move(st->edit2, st->rEdit2Inner, st->confirm);
    move(st->chkShow, st->rChk, true);
        move(st->chkDecoy, st->rChkDecoy, true);
    if (st->showProtection) {
        const bool showDays = (st->prot.action == 1);
        move(st->chkProt, st->rChkProt, true);
        move(st->editTries, st->rTriesEdit, true);
        move(st->editDays, showDays ? st->rDaysEdit : none, showDays);
    }
}

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
    PositionPasswordControls(st);
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
            if (st->data.fixedDir.empty()) { StartRowSlide(row, 0); row.selected = 0; }  // 没选到就退回“源文件同目录”
        }
        break;
    }
    RelayoutDialog(hDlg, st);
}

// 按复选框状态启用/禁用保护区的输入框（关闭时置灰）
void SyncProtectionEnabled(UiDlgState* st) {
    const bool on = st->prot.enabled;
    if (st->editTries) EnableWindow(st->editTries, on);
    if (st->editDays)  EnableWindow(st->editDays, on);
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
            // 回填（从确认框点「返回」回来时带着刚才那个密码）
            if (!st->password.empty()) {
                if (st->edit1) SetWindowTextW(st->edit1, st->password.c_str());
                if (st->edit2) SetWindowTextW(st->edit2, st->password.c_str());
            }

            st->chkShow = CreateWindowExW(0, L"BUTTON", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                st->rChk.left, st->rChk.top, st->rChk.right - st->rChk.left,
                st->rChk.bottom - st->rChk.top,
                hwnd, (HMENU)(INT_PTR)IDC_UI_SHOW, inst, NULL);
            if (st->chkShow) {
                UiDetheme(st->chkShow);
                AttachHover(st->chkShow);
            }

            if (st->showDecoyOption) {
                st->chkDecoy = CreateWindowExW(0, L"BUTTON", L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                    st->rChkDecoy.left, st->rChkDecoy.top,
                    st->rChkDecoy.right - st->rChkDecoy.left,
                    st->rChkDecoy.bottom - st->rChkDecoy.top,
                    hwnd, (HMENU)(INT_PTR)IDC_UI_CHK_DECOY, inst, NULL);
                if (st->chkDecoy) {
                    UiDetheme(st->chkDecoy);
                    AttachHover(st->chkDecoy);
                }
            }

            // ---- 防暴力破解设置区的子控件 ----
            if (st->showProtection) {
                auto makeNumEdit = [&](int id, const RECT& r) {
                    HWND e = CreateWindowExW(0, L"EDIT", L"",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_CENTER | ES_AUTOHSCROLL,
                        r.left, r.top, r.right - r.left, r.bottom - r.top,
                        hwnd, (HMENU)(INT_PTR)id, inst, NULL);
                    if (e) {
                        SendMessageW(e, WM_SETFONT, (WPARAM)UiFont(UiPx(14), false), TRUE);
                        SendMessageW(e, EM_SETLIMITTEXT, 4, 0);   // 9999 / 3650
                        ApplyEditTheme(e, st, false);            // 数字框不能套密码掩码
                    }
                    return e;
                };

                st->chkProt = CreateWindowExW(0, L"BUTTON", L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                    st->rChkProt.left, st->rChkProt.top,
                    st->rChkProt.right - st->rChkProt.left,
                    st->rChkProt.bottom - st->rChkProt.top,
                    hwnd, (HMENU)(INT_PTR)IDC_UI_CHK_PROT, inst, NULL);
                if (st->chkProt) {
                    UiDetheme(st->chkProt);
                    AttachHover(st->chkProt);
                    SendMessageW(st->chkProt, BM_SETCHECK, st->prot.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
                }

                st->editTries = makeNumEdit(IDC_UI_TRIES, st->rTriesEdit);
                if (st->editTries)
                    SetWindowTextW(st->editTries, std::to_wstring(st->prot.maxTries).c_str());

                st->editDays = makeNumEdit(IDC_UI_DAYS, st->rDaysEdit);
                if (st->editDays) {
                    SetWindowTextW(st->editDays, std::to_wstring(st->prot.lockDays).c_str());
                    // 只有“禁止解密”模式下才显示锁定时长
                    ShowWindow(st->editDays, st->prot.action == 1 ? SW_SHOW : SW_HIDE);
                }
                SyncProtectionEnabled(st);
            }
        }

        // 消息框：可选中复制的只读文本框
        if (st->hasSelectable && !IsRectEmpty(&st->rSelectable)) {
            const RECT& sr = st->rSelectable;
            st->editSelectable = CreateWindowExW(0, L"EDIT", st->selectableText.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_READONLY | ES_AUTOHSCROLL,
                sr.left + UiPx(8), sr.top + UiPx(6),
                sr.right - sr.left - UiPx(16), sr.bottom - sr.top - UiPx(12),
                hwnd, (HMENU)(INT_PTR)IDC_SELECTABLE, inst, NULL);
            if (st->editSelectable) {
                SendMessageW(st->editSelectable, WM_SETFONT, (WPARAM)UiFont(UiPx(14), false), TRUE);
                ApplyEditTheme(st->editSelectable, st, false);
                // 打开就全选：用户直接 Ctrl+C 就能复制
                SendMessageW(st->editSelectable, EM_SETSEL, 0, -1);
            }
        }

        // 设置面板：随机密码位数输入框（4-128）
        if (st->isSettings && !IsRectEmpty(&st->rGenLenEdit)) {
            const RECT& er = st->rGenLenEdit;
            st->editGenLen = CreateWindowExW(0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_CENTER | ES_AUTOHSCROLL,
                er.left, er.top, er.right - er.left, er.bottom - er.top,
                hwnd, (HMENU)(INT_PTR)IDC_UI_GENLEN, inst, NULL);
            if (st->editGenLen) {
                SendMessageW(st->editGenLen, WM_SETFONT, (WPARAM)UiFont(UiPx(14), false), TRUE);
                SendMessageW(st->editGenLen, EM_SETLIMITTEXT, 3, 0);
                ApplyEditTheme(st->editGenLen, st, false);   // 数字框不用密码掩码
                SetWindowTextW(st->editGenLen, std::to_wstring(st->genLen).c_str());
            }
        }

        // 按钮：先按 0 尺寸创建，随后统一从右向左排布
        for (size_t i = 0; i < st->btnIds.size(); ++i) {
            const int id = st->btnIds[i];
            HWND b = CreateWindowExW(0, L"BUTTON", LabelFor(st, i, id).c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, UiPx(10), st->btnH, hwnd, (HMENU)(INT_PTR)id, inst, NULL);
            if (b) {
                UiDetheme(b);
                SendMessageW(b, WM_SETFONT, (WPARAM)UiFont(UiPx(14), false), TRUE);
                AttachHover(b);
            }
            st->btns.push_back(b);
        }

        // 子控件是按布局矩形创建的，但要再摆一次：
        // 一来保证位置/可见性与最新布局一致，二来让圆角区域按真实尺寸重算
        PositionPasswordControls(st);
        PositionButtons(st);

        // Tab 顺序：输入框 -> 复选框 -> 按钮
        if (st->edit1) st->tabOrder.push_back(st->edit1);
        if (st->confirm && st->edit2) st->tabOrder.push_back(st->edit2);
        if (st->chkShow) st->tabOrder.push_back(st->chkShow);
        if (st->chkDecoy) st->tabOrder.push_back(st->chkDecoy);
        for (HWND b : st->btns) if (b) st->tabOrder.push_back(b);

        UiApplyFrame(hwnd, true, true);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; // 背景由 WM_PAINT 完整绘制

    case WM_MOUSEMOVE: {
        // 注意：这里**不能**判断 st->hTip 是否为空 —— 提示窗正是由
        // SetHelpTipVisible 按需创建的，先判空就永远建不起来（鸡生蛋）。
        if (!st) break;
        const POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        const RECT* rc = HelpIconRect(st);
        if (rc && !IsRectEmpty(rc) && PtInRect(rc, pt)) {
            SetHelpTipVisible(st, true);
            TRACKMOUSEEVENT tme = {};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
        } else {
            SetHelpTipVisible(st, false);
        }
        break;
    }

    case WM_MOUSELEAVE:
        if (st) SetHelpTipVisible(st, false);
        break;

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

    // 控件一旦被禁用，Windows 改发 WM_CTLCOLORSTATIC 而不是 WM_CTLCOLOREDIT。
    // 只处理 EDIT 的话，禁用状态的输入框会掉回系统默认的浅色背景 —— 深色模式下很突兀。
    case WM_CTLCOLORSTATIC: {
        if (!st) break;
        const UiPalette& c = UiColors();
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, c.textMuted);   // 禁用态用弱化色，视觉上也能看出不可编辑
        SetBkColor(dc, c.bgAlt);
        return reinterpret_cast<LRESULT>(st->altBrush ? st->altBrush : GetStockObject(WHITE_BRUSH));
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT pdis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
        if (!pdis || !st) break;
        if (pdis->CtlType == ODT_BUTTON) {
            if (pdis->CtlID == IDC_UI_SHOW || pdis->CtlID == IDC_UI_CHK_PROT ||
            pdis->CtlID == IDC_UI_CHK_DECOY)
                DrawOwnerCheckbox(pdis, st);
            else
                DrawOwnerButton(pdis);
            return TRUE;
        }
        break;
    }

    case WM_NCHITTEST: {
        if (st && st->showGenButton && !IsRectEmpty(&st->rGenBtn)) {
            POINT hp = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            POINT cp = hp;
            ScreenToClient(hwnd, &cp);
            if (PtInRect(&st->rGenBtn, cp)) return HTCLIENT;
        }
        const LRESULT hit = DefWindowProcW(hwnd, msg, wParam, lParam);
        if (hit != HTCLIENT || !st) return hit;
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        ScreenToClient(hwnd, &pt);
        if (st->isPassword) {
            // 输入框白框（比真实 EDIT 大一圈）也要接收点击，否则点到留白会变成拖动窗口
            if (PtInRect(&st->rEdit1, pt)) return HTCLIENT;
            if (st->confirm && PtInRect(&st->rEdit2, pt)) return HTCLIENT;
            // 保护区的分段控件由父窗口自绘并处理点击
            if (st->showProtection && PtInRect(&st->rSeg, pt)) return HTCLIENT;
            // 「?」图标必须返回 HTCLIENT，才能收到 WM_MOUSEMOVE 弹提示
            if (st->showDecoyOption && !IsRectEmpty(&st->rChkDecoyHelp) &&
                PtInRect(&st->rChkDecoyHelp, pt))
                return HTCLIENT;
        }
        if (st->isSettings) {
            for (const auto& r : st->rows) {
                if (PtInRect(&r.segRc, pt)) return HTCLIENT;
            }
            // 复选框区域必须返回 HTCLIENT，否则会被当成标题栏拖动窗口
            if (!IsRectEmpty(&st->rDecoyBox) &&
                (PtInRect(&st->rDecoyBox, pt) || PtInRect(&st->rDecoyLabel, pt)))
                return HTCLIENT;
            // 「?」图标同样必须返回 HTCLIENT：
            // 否则该区域算标题栏，鼠标移上去只来 WM_NCMOUSEMOVE，
            // 收不到 WM_MOUSEMOVE，悬停提示永远不会弹。
            if (!IsRectEmpty(&st->rDecoyHelp) && PtInRect(&st->rDecoyHelp, pt))
                return HTCLIENT;
            if (!IsRectEmpty(&st->rKeepBox) &&
                (PtInRect(&st->rKeepBox, pt) || PtInRect(&st->rKeepLabel, pt)))
                return HTCLIENT;
            if (!IsRectEmpty(&st->rMenuBox) &&
                (PtInRect(&st->rMenuBox, pt) || PtInRect(&st->rMenuLabel, pt)))
                return HTCLIENT;
            if (!IsRectEmpty(&st->rSponsor) && PtInRect(&st->rSponsor, pt))
                return HTCLIENT;
            for (int i = 0; i < 4; ++i) {
                if (!IsRectEmpty(&st->rGenBox[i]) &&
                    (PtInRect(&st->rGenBox[i], pt) || PtInRect(&st->rGenLabel[i], pt)))
                    return HTCLIENT;
            }
        }
        return HTCAPTION; // 其余区域允许拖动整个卡片
    }

    case WM_LBUTTONDOWN: {
        if (!st || st->closing) break;   // 关闭淡出期间不再响应输入
        const POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };

        // 密码框右侧的「随机生成」（自绘按钮，走这里而不是 WM_COMMAND）
        if (st->showGenButton && !IsRectEmpty(&st->rGenBtn) && PtInRect(&st->rGenBtn, pt)) {
            DoGeneratePassword(hwnd, st);
            return 0;
        }

        if (st->isSettings) {
            for (size_t i = 0; i < st->rows.size(); ++i) {
                auto& row = st->rows[i];
                for (size_t k = 0; k < row.itemRc.size(); ++k) {
                    if (!PtInRect(&row.itemRc[k], pt)) continue;
                    st->focusedRow = (int)i;
                    if (row.selected != (int)k) {
                        StartRowSlide(row, (int)k);     // 固定时长，与跨几个选项无关
                        row.selected = (int)k;          // 一次点击立刻切换
                        EnsureDlgAnimation(hwnd);
                        ApplySettingsRowChange(hwnd, st, row.key);
                        return 0;
                    }
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
            }
            // 随机密码生成器：4 个类别复选框
            for (int i = 0; i < 4; ++i) {
                if (IsRectEmpty(&st->rGenBox[i])) continue;
                if (!PtInRect(&st->rGenBox[i], pt) && !PtInRect(&st->rGenLabel[i], pt)) continue;
                if (i == 0)      st->genUpper   = !st->genUpper;
                else if (i == 1) st->genLower   = !st->genLower;
                else if (i == 2) st->genDigits  = !st->genDigits;
                else             st->genSymbols = !st->genSymbols;
                UpdateGenHint(st);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }

            // 「禁用诱饵文件」复选框：点方块或文字都能切换（点问号图标不切换）
            if (!IsRectEmpty(&st->rDecoyBox) &&
                (PtInRect(&st->rDecoyBox, pt) || PtInRect(&st->rDecoyLabel, pt))) {
                st->decoyDisabled = st->decoyDisabled ? 0 : 1;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            // 「加密后保留源文件」
            if (!IsRectEmpty(&st->rKeepBox) &&
                (PtInRect(&st->rKeepBox, pt) || PtInRect(&st->rKeepLabel, pt))) {
                st->keepSource = st->keepSource ? 0 : 1;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            // 「在资源管理器右键菜单中显示」
            if (!IsRectEmpty(&st->rMenuBox) &&
                (PtInRect(&st->rMenuBox, pt) || PtInRect(&st->rMenuLabel, pt))) {
                st->shellMenu = st->shellMenu ? 0 : 1;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            // 左下角「双击赞助」：两次点击间隔 <500ms 才算双击，
            // 免得用户想点确定却误触把浏览器打开。
            if (!IsRectEmpty(&st->rSponsor) && PtInRect(&st->rSponsor, pt)) {
                const ULONGLONG now = GetTickCount64();
                if (st->sponsorLastClick != 0 && now - st->sponsorLastClick < 500) {
                    st->sponsorLastClick = 0;
                    ShellExecuteW(NULL, L"open", kSponsorUrl, NULL, NULL, SW_SHOWNORMAL);
                }
                else {
                    st->sponsorLastClick = now;
                }
                return 0;
            }
            return 0;
        }

        if (st->isPassword) {
            if (st->edit1 && PtInRect(&st->rEdit1, pt)) { SetFocus(st->edit1); return 0; }
            if (st->confirm && st->edit2 && PtInRect(&st->rEdit2, pt)) { SetFocus(st->edit2); return 0; }

            // 保护区的“永久删除文件 / 禁止解密”分段控件：点一下即切换
            if (st->showProtection && PtInRect(&st->rSeg, pt)) {
                for (size_t k = 0; k < st->protItemRc.size(); ++k) {
                    if (!PtInRect(&st->protItemRc[k], pt)) continue;
                    if (st->prot.action != (int)k) {
                        st->prot.action = (int)k;
                        // 两行高度不同，“禁止解密”多一行锁定时长 -> 需要重新布局
                        RelayoutDialog(hwnd, st);
                    }
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                return 0;
            }
        }
        break;
    }

    case WM_COMMAND: {
        if (!st) break;
        if (st->closing) return 0;       // 关闭淡出期间不再响应输入
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);

        // 输入框获得/失去焦点：记录并在对应位置画（或擦掉）蓝色聚焦边框
        if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
            st->focusedEdit = (code == EN_SETFOCUS) ? (HWND)lParam : nullptr;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

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

        // 「安全删除覆写次数」：边输边校验，提示实时更新
        if (id == IDC_UI_PASSES && code == EN_CHANGE) {
            ValidatePasses(st);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        if (id == IDC_UI_CHK_DECOY) {
            st->useDecoy = !st->useDecoy;
            if (st->chkDecoy) {
                SendMessageW(st->chkDecoy, BM_SETCHECK,
                             st->useDecoy ? BST_CHECKED : BST_UNCHECKED, 0);
                InvalidateRect(st->chkDecoy, NULL, FALSE);
            }
            return 0;
        }

        if (id == IDC_GEN_PWD) {
            DoGeneratePassword(hwnd, st);
            return 0;
        }

        // 只读文本框获得焦点时全选，方便直接 Ctrl+C
        if (id == IDC_SELECTABLE && HIWORD(wParam) == EN_SETFOCUS && st->editSelectable) {
            SendMessageW(st->editSelectable, EM_SETSEL, 0, -1);
            return 0;
        }

        // 位数输入：边输边校验，只接受 4-128；超范围立刻变红字提示
        if (id == IDC_UI_GENLEN && HIWORD(wParam) == EN_CHANGE && st->editGenLen) {
            wchar_t buf[8] = {};
            GetWindowTextW(st->editGenLen, buf, _countof(buf));
            const int v = _wtoi(buf);
            if (buf[0] == L'\0') {
                st->genLenError = false;      // 空着不报错，只是不更新
            }
            else {
                st->genLenError = (v < 4 || v > 128);
                if (!st->genLenError) st->genLen = v;
            }
            UpdateGenHint(st);
            InvalidateRect(hwnd, NULL, FALSE);
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

        if (id == IDC_UI_CHK_PROT) {
            st->prot.enabled = !st->prot.enabled;
            if (st->chkProt) {
                SendMessageW(st->chkProt, BM_SETCHECK, st->prot.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
                InvalidateRect(st->chkProt, NULL, FALSE);
            }
            SyncProtectionEnabled(st);
            InvalidateRect(hwnd, NULL, FALSE);
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

                // 读取并校验防暴力破解设置。非法值给出行内红字，不静默纠正为合法值。
                if (st->showProtection && st->prot.enabled) {
                    wchar_t tb[16] = {};
                    wchar_t db[16] = {};
                    if (st->editTries) GetWindowTextW(st->editTries, tb, _countof(tb));
                    if (st->editDays)  GetWindowTextW(st->editDays, db, _countof(db));

                    auto parseNum = [](const wchar_t* s, int& out) {
                        if (!s || !*s) return false;          // 空视为非法，提示用户填写
                        long long v = 0;
                        for (const wchar_t* p = s; *p; ++p) {
                            if (*p < L'0' || *p > L'9') return false;
                            v = v * 10 + (*p - L'0');
                            if (v > 100000) return false;
                        }
                        out = (int)v;
                        return true;
                    };

                    int tries = 0;
                    if (!parseNum(tb, tries) || tries < 1 || tries > 9999) {
                        st->error = tr(L"错误尝试次数需为 1-9999 的整数", L"Attempts must be an integer from 1 to 9999");
                        MessageBeep(MB_ICONWARNING);
                        InvalidateRect(hwnd, NULL, FALSE);
                        if (st->editTries) SetFocus(st->editTries);
                        return 0;
                    }
                    st->prot.maxTries = tries;

                    if (st->prot.action == 1) {
                        int days = 0;
                        if (!parseNum(db, days) || days < 0 || days > 3650) {
                            st->error = tr(L"锁定时长需为 0-3650 的整数", L"Lock days must be an integer from 0 to 3650");
                            MessageBeep(MB_ICONWARNING);
                            InvalidateRect(hwnd, NULL, FALSE);
                            if (st->editDays) SetFocus(st->editDays);
                            return 0;
                        }
                        st->prot.lockDays = days;
                    }
                }

                st->password = buf1;
            }
            BeginCloseDialog(hwnd, st, id);   // 先淡出，动画结束后再销毁
            return 0;
        }
        break;
    }

    case WM_TIMER:
        if (wParam == DLG_ANIM_TIMER && st) {
            TickDialogAnimation(hwnd, st);
            return 0;
        }
        if (wParam == DLG_SHAKE_TIMER && st) {
            // 左右小幅晃动：8 步，幅度递减，最后回到原位
            static const int kOffset[] = { 12, -12, 10, -10, 7, -7, 4, -4 };
            if (st->shakeLeft <= 0) {
                KillTimer(hwnd, DLG_SHAKE_TIMER);
                SetWindowPos(hwnd, NULL, st->shakeBase.x, st->shakeBase.y, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                st->shakeLeft = -1;
                return 0;
            }
            const int idx = 8 - st->shakeLeft;
            if (idx >= 0 && idx < 8) {
                SetWindowPos(hwnd, NULL, st->shakeBase.x + kOffset[idx], st->shakeBase.y, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            --st->shakeLeft;
            return 0;
        }

        if (wParam == DLG_ATTENTION_TIMER && st) {
            const ULONGLONG now = GetTickCount64();
            if (st->isPassword || st->isSettings) return 0;

            // 强调方式：把弹窗自己置顶并左右晃几下。
            // 比 FlashWindowEx 靠谱得多 —— 本弹窗无标题栏、无任务栏按钮，
            // 闪它本身是空操作；闪主窗口又容易让人莫名其妙。
            auto StartShake = [&]() {
                RECT rc = {};
                if (!GetWindowRect(hwnd, &rc)) return;
                st->shakeBase.x = rc.left;
                st->shakeBase.y = rc.top;
                st->shakeLeft = 8;                 // 8 步 × 30ms ≈ 240ms
                // 置顶，保证一定看得见
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                SetTimer(hwnd, DLG_SHAKE_TIMER, 30, NULL);
            };

            // 为什么不用 GetForegroundWindow 判断"用户是否想回到主窗口"：
            // 弹窗打开时主窗口被 EnableWindow(owner, FALSE) 禁用了，点它不会
            // 改变前台窗口，所以"前台 != 弹窗"永远不成立 —— 这就是最早完全不闪
            // 也不响的原因。改成直接嗅探鼠标左键：按下且落点不在弹窗上，就认为
            // 用户想绕过弹窗继续操作。
            static bool      s_lastDown = false;
            static ULONGLONG s_lastNag  = 0;
            const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            if (down && !s_lastDown && now - s_lastNag > 1200) {
                POINT pt = {};
                GetCursorPos(&pt);
                HWND under = WindowFromPoint(pt);
                const bool onDialog = (under == hwnd) || (under && IsChild(hwnd, under));
                if (!onDialog && under) {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(under, &pid);
                    if (pid == GetCurrentProcessId()) {
                        s_lastNag = now;
                        MessageBeep(MB_ICONWARNING);
                        StartShake();
                    }
                }
            }
            s_lastDown = down;


            return 0;
        }
        break;

    case WM_CLOSE:
        if (st) BeginCloseDialog(hwnd, st, st->cancelId);
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
        KillTimer(hwnd, DLG_ATTENTION_TIMER);
        KillTimer(hwnd, DLG_SHAKE_TIMER);
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
    ++g_modalDepth;   // 计数：主窗口据此判断能否安全销毁自己

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
    // 消息框：每 600ms 检查一次是否被无视（切换到这个定时器在 WM_DESTROY 里清掉）
    if (!st->isPassword && !st->isSettings) {
        // 80ms 扫一次鼠标左键：太快没必要，太慢会漏掉快速点击
        SetTimer(hwnd, DLG_ATTENTION_TIMER, 80, NULL);
    }
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
                        StartRowSlide(row, sel);
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

    if (g_modalDepth > 0) --g_modalDepth;
    return st->result;
}

} // namespace

// ---------------------------------------------------------------- 对外接口

int UiModalDepth() { return g_modalDepth; }


int UiShowMessage(HWND owner, const UiMessage& m) {
    UiDlgState st;
    st.caption = m.caption;
    st.text = m.text;
    st.icon = m.icon;
    st.buttons = m.buttons;
    st.topmost = m.topmost;
    st.sound = m.sound;
    st.btnLabels = m.labels;
    st.selectableText = m.selectable;
    st.hasSelectable  = !m.selectable.empty();

    switch (m.buttons & 0x0F) {
    case MB_YESNO:
        st.btnIds = { IDYES, IDNO };
        st.primaryId = IDYES;
        st.cancelId = IDNO;
        break;
    case MB_YESNOCANCEL:
        // 三按钮：左 -> 右依次是 IDYES / IDNO / IDCANCEL
        st.btnIds = { IDYES, IDNO, IDCANCEL };
        st.primaryId = IDYES;
        st.cancelId = IDCANCEL;
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
    // 调用方可以用 primaryId 覆盖蓝色主按钮（例如把“取消”而不是“是”作为主按钮）
    if (m.primaryId) st.primaryId = m.primaryId;

    const int r = RunDialog(owner, &st);
    if ((m.buttons & 0x0F) == MB_OK && r == 0) return IDOK;
    return r;
}

bool UiShowPassword(HWND owner, bool confirm, std::string& outPassword,
                    const std::wstring& subtitle, bool showProtection,
                    UiProtection* ioProt, int remainTries, bool showDecoy,
                    bool* ioUseDecoy, bool showGenButton, bool* ioGenerated,
                    const std::string& initialPassword, bool initialGenerated) {
    UiDlgState st;
    st.isPassword = true;
    st.showGenButton = showGenButton;
    st.passwordGenerated = initialGenerated;
    // 从「请保存密码」点返回时会带上刚才那个密码，免得用户再也没法复制
    if (!initialPassword.empty()) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, initialPassword.c_str(),
                                          (int)initialPassword.size(), NULL, 0);
        if (n > 0) {
            st.password.resize((size_t)n);
            MultiByteToWideChar(CP_UTF8, 0, initialPassword.c_str(),
                                (int)initialPassword.size(), &st.password[0], n);
            // 回填的是刚生成的随机密码：直接明文显示，方便用户再复制一次
            st.showPwd = true;
        }
    }
    st.confirm = confirm;
    st.subtitle = subtitle;
    st.sound = false;   // 密码框不需要提示音
    st.caption = confirm ? tr(L"设置密码", L"Set password") : tr(L"输入密码", L"Enter password");
    st.btnIds = { IDOK, IDCANCEL };
    st.primaryId = IDOK;
    st.cancelId = IDCANCEL;

    // 只有加密时才显示保护设置区
    st.showProtection = confirm && showProtection && ioProt != nullptr;
    if (st.showProtection) st.prot = *ioProt;
    // 可否认加密：同样只在加密时提供，且由调用方决定（目前仅单文件）
    st.showDecoyOption = confirm && showDecoy;
    if (st.showDecoyOption && ioUseDecoy) st.useDecoy = *ioUseDecoy;
    // 解密时由调用方给出剩余次数用于红字提示，-1 表示不显示
    st.remainTries = confirm ? -1 : remainTries;

    const int r = RunDialog(owner, &st);
    if (r == IDOK) {
        outPassword = WstringToUtf8(st.password);
        if (st.showProtection && ioProt) *ioProt = st.prot;
        if (st.showDecoyOption && ioUseDecoy) *ioUseDecoy = st.useDecoy;
        if (ioGenerated) *ioGenerated = st.passwordGenerated;
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
    st.decoyDisabled = io.decoyDisabled;
    st.keepSource = io.keepSource;
    st.shellMenu = io.shellMenu;
    // 随机密码生成器
    st.genLen     = io.pwGenLength;
    st.genUpper   = io.pwGenUpper   != 0;
    st.genLower   = io.pwGenLower   != 0;
    st.genDigits  = io.pwGenDigits  != 0;
    st.genSymbols = io.pwGenSymbols != 0;
    UpdateGenHint(&st);
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
    // 覆写次数：1 / 2 / 3 次（对应 secureDelete = 1..3）
    addRow(3, tr(L"安全删除覆写次数", L"Overwrite passes"),
           { tr(L"1 次", L"1 pass"), tr(L"2 次", L"2 passes"), tr(L"3 次", L"3 passes") },
           st.data.secureDelete >= 3 ? 2 : (st.data.secureDelete == 2 ? 1 : 0));
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
    io.secureDelete = (st.rows.size() > 3) ? (st.rows[3].selected + 1) : 3;
    io.completion   = st.rows[4].selected;
    io.strength     = st.rows[5].selected;
    io.decoyDisabled = st.decoyDisabled;
    io.keepSource = st.keepSource;
    io.shellMenu = st.shellMenu;
    io.pwGenLength  = st.genLen;
    io.pwGenUpper   = st.genUpper   ? 1 : 0;
    io.pwGenLower   = st.genLower   ? 1 : 0;
    io.pwGenDigits  = st.genDigits  ? 1 : 0;
    io.pwGenSymbols = st.genSymbols ? 1 : 0;
    return true;
}
