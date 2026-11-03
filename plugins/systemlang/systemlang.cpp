// SystemLangBundle: the macOS/iOS plugin behind DeviceLang.GetSystemLanguage.
// Returns a language tag such as "fr-FR" or "zh-TW"; the game maps it onto its
// languages (gdConst.Lang) and already knows the Windows culture names.
// gosr.ini next to the game can force one: [game] language=ja-JP
// The P/Invoke marshaler frees the returned string with CoTaskMemFree.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

namespace
{
    void ConfiguredLanguage(wchar_t* name, DWORD size)
    {
        wchar_t path[MAX_PATH];
        DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
        while (len > 0 && path[len - 1] != L'\\' && path[len - 1] != L'/')
            len--;
        if (len == 0 || len + 9 >= MAX_PATH)
            return;
        lstrcpyW(path + len, L"gosr.ini");
        GetPrivateProfileStringW(L"game", L"language", L"", name, size, path);
    }

    void SystemLanguage(wchar_t* name, DWORD size)
    {
        // the display language first, then the regional locale
        ULONG count = 0, chars = 0;
        wchar_t list[LOCALE_NAME_MAX_LENGTH * 8];
        chars = ARRAYSIZE(list);
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, list, &chars) && list[0])
            lstrcpynW(name, list, size);
        else
            GetUserDefaultLocaleName(name, size);

        // Latin American Spanish is "es-419" on Windows; the game knows es-MX
        if (lstrcmpiW(name, L"es-419") == 0)
            lstrcpynW(name, L"es-MX", size);
    }
}

extern "C" __declspec(dllexport) char* GetSystemLanguage()
{
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = L"";
    ConfiguredLanguage(name, LOCALE_NAME_MAX_LENGTH);
    if (!name[0])
        SystemLanguage(name, LOCALE_NAME_MAX_LENGTH);
    if (!name[0])
        lstrcpyW(name, L"en-US");

    int bytes = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
    char* out = static_cast<char*>(CoTaskMemAlloc(bytes > 0 ? bytes : 1));
    if (!out)
        return nullptr;
    if (bytes <= 0 || !WideCharToMultiByte(CP_UTF8, 0, name, -1, out, bytes, nullptr, nullptr))
        out[0] = 0;
    return out;
}
