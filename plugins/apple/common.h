// Shared helpers for the Windows stand-ins of the game's macOS plugins.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

#define GOSR_EXPORT extern "C" __declspec(dllexport)

namespace gosr
{
    // Strings returned to a P/Invoke "string" result are freed by the
    // marshaler with CoTaskMemFree.
    inline char* MarshalString(const char* s)
    {
        if (!s)
            return nullptr;
        size_t n = lstrlenA(s) + 1;
        char* out = static_cast<char*>(CoTaskMemAlloc(n));
        if (out)
            CopyMemory(out, s, n);
        return out;
    }

    // <game folder>\<name>
    inline void GamePath(wchar_t* out, DWORD size, const wchar_t* name)
    {
        DWORD len = GetModuleFileNameW(nullptr, out, size);
        while (len > 0 && out[len - 1] != L'\\' && out[len - 1] != L'/')
            len--;
        out[len] = 0;
        if (len + lstrlenW(name) + 1 < size)
            lstrcatW(out, name);
    }
}
