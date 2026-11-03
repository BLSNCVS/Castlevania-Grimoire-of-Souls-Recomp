// Mouse buttons in the game's keyboard controls. The game's key list (its
// own KEYBOARD_TRIGGER enum, read with Input.GetKey(KeyCode)) has no mouse
// buttons, but it has keys a PC keyboard lacks: F13-F15 and the Mac Command
// keys. The engine's key queries answer for those keys with mouse buttons as
// well, so in the game's keyboard options a mouse button press is taken and
// shown as that key:
//
//   left button   -> Left Command      right button   -> Right Command
//   middle button -> F15
//   side (back)   -> F13               side (forward) -> F14
//
// gosr.ini [mouse] buttons=0 turns this off.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace
{
    // UnityEngine.KeyCode
    enum : int32_t
    {
        kF13 = 294, kF14 = 295, kF15 = 296, kRightCommand = 309, kLeftCommand = 310,
        kMouse0 = 323, kMouse1 = 324, kMouse2 = 325, kMouse3 = 326, kMouse4 = 327
    };

    int32_t MouseFor(int32_t key)
    {
        switch (key)
        {
        case kLeftCommand: return kMouse0;
        case kRightCommand: return kMouse1;
        case kF15: return kMouse2;
        case kF13: return kMouse3;
        case kF14: return kMouse4;
        default: return 0;
        }
    }

    using KeyQuery = bool (*)(int32_t);
    KeyQuery g_getKey, g_getKeyDown, g_getKeyUp;

    bool GetKey(int32_t key)
    {
        int32_t mouse = MouseFor(key);
        return g_getKey(key) || (mouse && g_getKey(mouse));
    }

    bool GetKeyDown(int32_t key)
    {
        int32_t mouse = MouseFor(key);
        return g_getKeyDown(key) || (mouse && g_getKeyDown(mouse));
    }

    bool GetKeyUp(int32_t key)
    {
        int32_t mouse = MouseFor(key);
        return g_getKeyUp(key) || (mouse && g_getKeyUp(mouse));
    }

    bool Enabled()
    {
        static const bool on = [] {
            wchar_t path[MAX_PATH * 4];
            DWORD len = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
            std::wstring ini(path, len);
            ini = ini.substr(0, ini.find_last_of(L"\\/") + 1) + L"gosr.ini";
            return GetPrivateProfileIntW(L"mouse", L"buttons", 1, ini.c_str()) != 0;
        }();
        return on;
    }
}

namespace recomp
{
    bool MouseButtonsEnabled() { return Enabled(); }

    // Called when the game resolves an engine internal call: the key queries
    // get the versions that include mouse buttons.
    void* WrapInputIcall(const char* name, void* original)
    {
        if (!original || strncmp(name, "UnityEngine.Input::GetKey", 25) || !Enabled())
            return original;
        const char* rest = name + 25;
        fprintf(stderr, "GoSRecomp: %s also answers for mouse buttons\n", name);
        if (!strcmp(rest, "Int") || !strncmp(rest, "Int(", 4))
            return g_getKey = reinterpret_cast<KeyQuery>(original), reinterpret_cast<void*>(&GetKey);
        if (!strcmp(rest, "DownInt") || !strncmp(rest, "DownInt(", 8))
            return g_getKeyDown = reinterpret_cast<KeyQuery>(original), reinterpret_cast<void*>(&GetKeyDown);
        if (!strcmp(rest, "UpInt") || !strncmp(rest, "UpInt(", 6))
            return g_getKeyUp = reinterpret_cast<KeyQuery>(original), reinterpret_cast<void*>(&GetKeyUp);
        return original;
    }
}
