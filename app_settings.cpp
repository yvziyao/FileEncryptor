#include "app_settings.h"
#include "FileEncryptor_helpers.h"

#include <string>

namespace {

const wchar_t* kRegKey = L"Software\\FileEncryptor";

DWORD RegGetDword(const wchar_t* name, DWORD def) {
    DWORD value = def;
    DWORD size = sizeof(value);
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        RegQueryValueExW(key, name, NULL, NULL, reinterpret_cast<LPBYTE>(&value), &size);
        RegCloseKey(key);
    }
    return value;
}

std::wstring RegGetString(const wchar_t* name) {
    wchar_t buf[1024] = {};
    DWORD size = sizeof(buf);
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        RegQueryValueExW(key, name, NULL, NULL, reinterpret_cast<LPBYTE>(buf), &size);
        RegCloseKey(key);
    }
    return std::wstring(buf);
}

void RegSetDword(const wchar_t* name, DWORD value) {
    HKEY key = NULL;
    DWORD disp = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, NULL, 0, KEY_WRITE, NULL, &key, &disp) == ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(key);
    }
}

void RegSetString(const wchar_t* name, const std::wstring& value) {
    HKEY key = NULL;
    DWORD disp = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, NULL, 0, KEY_WRITE, NULL, &key, &disp) == ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(value.c_str()),
                       (DWORD)((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
}

// 读到的值可能被人为改坏，这里统一夹取到合法范围
template <typename E>
E ClampEnum(DWORD raw, DWORD maxValue, E def) {
    if (raw > maxValue) return def;
    return static_cast<E>(raw);
}

} // namespace

AppSettings& Settings() {
    static AppSettings s;
    return s;
}

void SettingsLoad() {
    AppSettings& s = Settings();
    s.language     = ClampEnum<LangMode>(RegGetDword(L"Language", 0), 1, LangMode::Chinese);
    s.theme        = ClampEnum<ThemeMode>(RegGetDword(L"Theme", 0), 2, ThemeMode::System);
    s.outDir       = ClampEnum<OutDirMode>(RegGetDword(L"OutDirMode", 0), 2, OutDirMode::SourceDir);
    s.fixedOutDir  = RegGetString(L"OutDir");
    s.completion   = ClampEnum<CompletionMode>(RegGetDword(L"Completion", 1), 1, CompletionMode::ShowDialog);

    const DWORD sd = RegGetDword(L"SecureDelete", 3);
    if (sd == 0)      s.secureDelete = SecureDeleteMode::Off;
    else if (sd == 1) s.secureDelete = SecureDeleteMode::Overwrite1;
    else              s.secureDelete = SecureDeleteMode::Overwrite3;

    s.strength = ClampEnum<StrengthMode>(RegGetDword(L"Strength", 1), 3, StrengthMode::Standard);
}

void SettingsSave() {
    const AppSettings& s = Settings();
    RegSetDword(L"Language", (DWORD)s.language);
    RegSetDword(L"Theme", (DWORD)s.theme);
    RegSetDword(L"OutDirMode", (DWORD)s.outDir);
    RegSetString(L"OutDir", s.fixedOutDir);
    RegSetDword(L"SecureDelete", (DWORD)s.secureDelete);
    RegSetDword(L"Completion", (DWORD)s.completion);
    RegSetDword(L"Strength", (DWORD)s.strength);
}

int StrengthIterations(StrengthMode s) {
    switch (s) {
    case StrengthMode::Fast:    return 50000;
    case StrengthMode::Standard: return 100000;
    case StrengthMode::Secure:  return 300000;
    case StrengthMode::Extreme: return 600000;
    }
    return 100000;
}

StrengthMode StrengthFromIterations(unsigned int iterations) {
    if (iterations <= 50000)  return StrengthMode::Fast;
    if (iterations <= 100000) return StrengthMode::Standard;
    if (iterations <= 300000) return StrengthMode::Secure;
    return StrengthMode::Extreme;
}

std::wstring StrengthName(StrengthMode s) {
    switch (s) {
    case StrengthMode::Fast:     return tr(L"快速", L"Fast");
    case StrengthMode::Standard: return tr(L"标准", L"Standard");
    case StrengthMode::Secure:   return tr(L"安全", L"Secure");
    case StrengthMode::Extreme:  return tr(L"极强", L"Extreme");
    }
    return std::wstring();
}

std::wstring ThemeName(ThemeMode t) {
    switch (t) {
    case ThemeMode::System: return tr(L"跟随系统", L"System");
    case ThemeMode::Light:  return tr(L"浅色", L"Light");
    case ThemeMode::Dark:   return tr(L"深色", L"Dark");
    }
    return std::wstring();
}

std::wstring OutDirName(OutDirMode d) {
    switch (d) {
    case OutDirMode::SourceDir:    return tr(L"源文件同目录", L"Same folder");
    case OutDirMode::FixedDir:     return tr(L"指定目录", L"Fixed folder");
    case OutDirMode::AskEveryTime: return tr(L"每次都询问", L"Ask each time");
    }
    return std::wstring();
}

std::wstring SecureDeleteName(SecureDeleteMode d) {
    switch (d) {
    case SecureDeleteMode::Off:        return tr(L"禁用", L"Off");
    case SecureDeleteMode::Overwrite1: return tr(L"覆写 1 次", L"1 pass");
    case SecureDeleteMode::Overwrite3: return tr(L"覆写 3 次", L"3 passes");
    }
    return std::wstring();
}

std::wstring CompletionName(CompletionMode c) {
    switch (c) {
    case CompletionMode::CloseWindow: return tr(L"关闭窗口", L"Close window");
    case CompletionMode::ShowDialog:  return tr(L"弹窗提示", L"Show dialog");
    }
    return std::wstring();
}
