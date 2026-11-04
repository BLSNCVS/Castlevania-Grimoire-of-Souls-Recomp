// Controller fixes, as native hooks in gsJoystick (arm64_recomp.py --hook):
//
//   gsJoystick.IsTrigger(TRIGGER)      one table entry pressed or not
//   gsJoystick.NewInputDevices()       the per-frame input pass
//   outMenuGameSettingControlJoyStickInputCheck.GetInputSystemKeyDown(TRIGGER)
//                                      new binding detection, Input System part
//
// gosr.ini:
//   [controller]
//   dpad_move=1   ; the d-pad moves the character too
#include <cstdint>
#include <iterator>
#include <string>

#include <windows.h>

#include "il2cpp-api.h"
#include "il2cpp-object-internals.h"
#include "recomp_ctx.h"

namespace recomp
{
    void f_200fbe0_orig(Ctx& c);
    void f_200f60c_orig(Ctx& c);
    void f_1ecc8f0_orig(Ctx& c);
}

namespace
{
    // gsJoystick fields
    constexpr size_t kAxisL = 0x54;       // Vector2 m_axis_l
    constexpr size_t kConfig = 0x68;      // TRIGGER[] m_config
    constexpr size_t kFlags = 0x84;       // int m_flags, bit per ASSIGN
    constexpr size_t kAxisNames = 0x98;   // string[] m_axis_names, axis01-axis28
    constexpr size_t kDevice = 0xd0;      // Gamepad m_currentDevice (Input System)

    enum : int32_t
    {
        AXS01N = 21, AXS05N = 25, AXS28N = 48, AXS01P = 49, AXS05P = 53, AXS28P = 76,
        NEW_RIGHT_TRIGGER = 77, NEW_LEFT_TRIGGER, NEW_DPAD_UP, NEW_DPAD_DOWN, NEW_DPAD_RIGHT, NEW_DPAD_LEFT,
    };
    enum : int { LSTICK_U = 0, LSTICK_R, LSTICK_D, LSTICK_L, DPAD_U = 20, DPAD_R, DPAD_D, DPAD_L };

    template <typename T>
    T& Field(void* obj, size_t offset)
    {
        return *reinterpret_cast<T*>(static_cast<uint8_t*>(obj) + offset);
    }

    float Axis(void* joystick, size_t index)  // 0-based: axis01 is 0
    {
        using AxisRaw = float (*)(Il2CppString*);
        static auto getAxis = reinterpret_cast<AxisRaw>(il2cpp_resolve_icall("UnityEngine.Input::GetAxisRaw(System.String)"));
        auto* names = Field<Il2CppArray*>(joystick, kAxisNames);
        if (!getAxis || !names || index >= il2cpp_array_length(names))
            return 0;
        auto* name = reinterpret_cast<Il2CppString**>(reinterpret_cast<uint8_t*>(names) + kIl2CppSizeOfArray)[index];
        return name ? getAxis(name) : 0;
    }

    // The Windows Xbox table reads LT/RT (and the Mac tables the d-pad) through
    // the Input System gamepad. The macOS build has no XInput support in its
    // Input System, so an Xbox pad never shows up there. Without a gamepad,
    // these entries read the classic axes the pad has on Windows: LT 9, RT 10,
    // d-pad 6 (x) and 7 (y).
    bool InputSystemFallback(void* joystick, int32_t trigger, bool& pressed)
    {
        if (trigger < NEW_RIGHT_TRIGGER || trigger > NEW_DPAD_LEFT || Field<void*>(joystick, kDevice))
            return false;
        switch (trigger)
        {
        case NEW_RIGHT_TRIGGER: pressed = Axis(joystick, 9) > 0.5f; break;
        case NEW_LEFT_TRIGGER: pressed = Axis(joystick, 8) > 0.5f; break;
        case NEW_DPAD_UP: pressed = Axis(joystick, 6) > 0.5f; break;
        case NEW_DPAD_DOWN: pressed = Axis(joystick, 6) < -0.5f; break;
        case NEW_DPAD_RIGHT: pressed = Axis(joystick, 5) > 0.5f; break;
        case NEW_DPAD_LEFT: pressed = Axis(joystick, 5) < -0.5f; break;
        }
        return true;
    }

    void* Joystick()
    {
        static FieldInfo* instance = [] {
            const Il2CppAssembly* const* assemblies;
            size_t count = 0;
            assemblies = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &count);
            for (size_t i = 0; i < count; i++)
                if (Il2CppClass* k = il2cpp_class_from_name(il2cpp_assembly_get_image(assemblies[i]), "", "gsJoystick"))
                    for (; k; k = il2cpp_class_get_parent(k))
                        if (FieldInfo* f = il2cpp_class_get_field_from_name(k, "<Instance>k__BackingField"))
                            return f;
            return static_cast<FieldInfo*>(nullptr);
        }();
        void* obj = nullptr;
        if (instance)
            il2cpp_field_static_get_value(instance, &obj);
        return obj;
    }

    bool DpadMoves()
    {
        static const bool on = [] {
            wchar_t path[MAX_PATH * 4];
            DWORD len = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
            std::wstring ini(path, len);
            ini = ini.substr(0, ini.find_last_of(L"\\/") + 1) + L"gosr.ini";
            return GetPrivateProfileIntW(L"controller", L"dpad_move", 0, ini.c_str()) != 0;
        }();
        return on;
    }
}

namespace recomp
{
    // The macOS build of IsTrigger only handles axes 1-4. Windows needs more
    // (PS right stick Y is axis 6, Xbox d-pad is 6-7), in game and when
    // rebinding in the controller options.
    void HookIsTrigger(Ctx& c)
    {
        void* self = reinterpret_cast<void*>(c.x[0]);
        int32_t trigger = int32_t(uint32_t(c.x[1]));
        bool pressed = false;
        if (self && trigger >= AXS05N && trigger <= AXS28N)
            pressed = Axis(self, size_t(trigger - AXS01N)) < -0.5f;
        else if (self && trigger >= AXS05P && trigger <= AXS28P)
            pressed = Axis(self, size_t(trigger - AXS01P)) > 0.5f;
        else if (!self || !InputSystemFallback(self, trigger, pressed))
            return f_200fbe0_orig(c);
        c.x[0] = pressed;
    }

    // [controller] dpad_move=1: after the game's input pass, the d-pad also
    // counts as the left stick (its bits, and the vector when the stick rests).
    void HookJoystickInput(Ctx& c)
    {
        void* self = reinterpret_cast<void*>(c.x[0]);
        f_200f60c_orig(c);
        if (!self || !DpadMoves())
            return;
        int32_t& flags = Field<int32_t>(self, kFlags);
        bool up = flags & (1 << DPAD_U), right = flags & (1 << DPAD_R);
        bool down = flags & (1 << DPAD_D), left = flags & (1 << DPAD_L);
        if (!(up || right || down || left))
            return;
        flags |= (up << LSTICK_U) | (right << LSTICK_R) | (down << LSTICK_D) | (left << LSTICK_L);
        float* axis = &Field<float>(self, kAxisL);
        if (axis[0] * axis[0] + axis[1] * axis[1] < 0.04f)
        {
            axis[0] = float(right) - float(left);
            axis[1] = float(up) - float(down);  // m_axis_l.y is up-positive
        }
    }

    // Detects a press for the rebinding screen; the fallback reports the
    // frame an axis goes past half range.
    void HookRebindInputSystemKeyDown(Ctx& c)
    {
        int32_t trigger = int32_t(uint32_t(c.x[1]));
        void* joystick = Joystick();
        bool pressed = false;
        if (!joystick || !InputSystemFallback(joystick, trigger, pressed))
            return f_1ecc8f0_orig(c);
        static bool before[NEW_DPAD_LEFT - NEW_RIGHT_TRIGGER + 1] = {};
        bool& was = before[trigger - NEW_RIGHT_TRIGGER];
        c.x[0] = pressed && !was;
        was = pressed;
    }
}
