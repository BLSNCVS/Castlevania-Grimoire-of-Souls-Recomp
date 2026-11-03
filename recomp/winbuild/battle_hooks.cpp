// Native hooks inside recompiled game functions (arm64_recomp.py --hook):
// each runs before the original, which it calls as f_<addr>_orig.
//
//   ope_pl_status.Damage(int)            damage the player's character takes
//   ope_en_status.Damage(int, int, int)  damage an enemy takes
//   uiUtility.GetKeyboardInputText(KEYBOARD_TRIGGER)  key names in the options
//   gsJoystick.IsTrigger(TRIGGER)        controller axes 5-28
#include <atomic>
#include <cstdint>

#include "il2cpp-api.h"
#include "il2cpp-object-internals.h"
#include "recomp_ctx.h"

namespace recomp
{
    void f_23b08bc_orig(Ctx& c);
    void f_1e08d38_orig(Ctx& c);
    void f_1bec6fc_orig(Ctx& c);
    void f_200fbe0_orig(Ctx& c);
    bool MouseButtonsEnabled();  // mouse_keys.cpp

    // Set from the debug menu
    std::atomic<bool> g_invincible{ false };
    std::atomic<int> g_damageDealtFactor{ 1 };

    void HookPlayerDamage(Ctx& c)
    {
        if (g_invincible)
            c.x[1] = 0;
        f_23b08bc_orig(c);
    }

    void HookEnemyDamage(Ctx& c)
    {
        int factor = g_damageDealtFactor;
        if (factor > 1)
        {
            int64_t damage = int64_t(int32_t(uint32_t(c.x[1]))) * factor;
            if (damage > 999999999)
                damage = 999999999;
            c.x[1] = uint32_t(int32_t(damage));
        }
        f_1e08d38_orig(c);
    }

    // The game's key list has no mouse buttons; they stand in for keys a PC
    // keyboard lacks (mouse_keys.cpp), and get their own names here.
    void HookKeyboardInputText(Ctx& c)
    {
        const char* name = nullptr;
        if (MouseButtonsEnabled())
        {
            switch (int32_t(uint32_t(c.x[0])))  // gsJoystick.KEYBOARD_TRIGGER
            {
            case 83: name = "Mouse L"; break;  // LeftCommand
            case 82: name = "Mouse R"; break;  // RightCommand
            case 49: name = "Mouse M"; break;  // F15
            case 47: name = "Mouse 4"; break;  // F13
            case 48: name = "Mouse 5"; break;  // F14
            }
        }
        if (!name)
            return f_1bec6fc_orig(c);
        c.x[0] = reinterpret_cast<uint64_t>(il2cpp_string_new(name));
    }

    // The macOS build of IsTrigger only handles axes 1-4. Windows needs more
    // (PS right stick Y is axis 6, Xbox d-pad is 6-7), in game and when
    // rebinding in the controller options.
    void HookIsTrigger(Ctx& c)
    {
        int32_t trigger = int32_t(uint32_t(c.x[1]));
        bool negative = trigger >= 25 && trigger <= 48;  // AXS05N-AXS28N
        bool positive = trigger >= 53 && trigger <= 76;  // AXS05P-AXS28P
        if (!negative && !positive)
            return f_200fbe0_orig(c);
        using AxisRaw = float (*)(Il2CppString*);
        static auto getAxis = reinterpret_cast<AxisRaw>(il2cpp_resolve_icall("UnityEngine.Input::GetAxisRaw(System.String)"));
        auto* self = reinterpret_cast<uint8_t*>(c.x[0]);
        auto* names = self ? *reinterpret_cast<Il2CppArray**>(self + 0x98) : nullptr;  // m_axis_names
        size_t index = size_t(trigger - (negative ? 21 : 49));
        bool on = false;
        if (getAxis && names && index < il2cpp_array_length(names))
        {
            auto* name = reinterpret_cast<Il2CppString**>(reinterpret_cast<uint8_t*>(names) + kIl2CppSizeOfArray)[index];
            if (name)
            {
                float v = getAxis(name);
                on = negative ? v < -0.5f : v > 0.5f;
            }
        }
        c.x[0] = on;
    }
}
