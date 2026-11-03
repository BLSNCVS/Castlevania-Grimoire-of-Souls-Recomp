// GCControllerBundleWrapper: names of the connected controller's buttons, as
// the GameController framework reports them (SF Symbol names for the button
// icons, short aliases for text). The family follows the connected pad:
// PlayStation (Sony vendor id), Nintendo, otherwise Xbox layout.
#include "common.h"

namespace
{
    enum class Family { Xbox, PlayStation, Nintendo };

    Family Detect()
    {
        UINT count = 0;
        if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0 || count > 256)
            return Family::Xbox;
        RAWINPUTDEVICELIST list[256];
        count = GetRawInputDeviceList(list, &count, sizeof(RAWINPUTDEVICELIST));
        if (count == UINT(-1))
            return Family::Xbox;
        for (UINT i = 0; i < count; i++)
        {
            if (list[i].dwType != RIM_TYPEHID)
                continue;
            RID_DEVICE_INFO info{};
            info.cbSize = sizeof(info);
            UINT size = sizeof(info);
            if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &size) == UINT(-1))
                continue;
            // gamepads and joysticks only (generic desktop page, usages 4/5)
            if (info.hid.usUsagePage != 1 || (info.hid.usUsage != 4 && info.hid.usUsage != 5))
                continue;
            if (info.hid.dwVendorId == 0x054C)
                return Family::PlayStation;
            if (info.hid.dwVendorId == 0x057E)
                return Family::Nintendo;
        }
        return Family::Xbox;
    }

    enum Button { LShoulder, RShoulder, LTrigger, RTrigger, Menu, Options, Home, A, B, X, Y,
                  DPad, DUp, DDown, DLeft, DRight, LStick, RStick, LStickButton, RStickButton, kCount };

    struct Names { const char* symbol[kCount]; const char* alias[kCount]; };

    const Names kXbox = {
        { "lb.rectangle.roundedbottom", "rb.rectangle.roundedbottom", "lt.rectangle.roundedtop", "rt.rectangle.roundedtop",
          "line.3.horizontal.circle", "rectangle.on.rectangle.circle", "xbox.logo", "a.circle", "b.circle", "x.circle", "y.circle",
          "dpad", "dpad.up.filled", "dpad.down.filled", "dpad.left.filled", "dpad.right.filled",
          "l.joystick", "r.joystick", "l.joystick.press.down", "r.joystick.press.down" },
        { "LB", "RB", "LT", "RT", "Menu", "View", "Xbox", "A", "B", "X", "Y",
          "D-pad", "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Left Stick", "Right Stick", "LS", "RS" } };

    // GameController maps PlayStation's cross/circle/square/triangle onto A/B/X/Y
    const Names kPlayStation = {
        { "l1.rectangle.roundedbottom", "r1.rectangle.roundedbottom", "l2.rectangle.roundedtop", "r2.rectangle.roundedtop",
          "line.3.horizontal.circle", "rectangle.on.rectangle.circle", "logo.playstation", "xmark.circle", "circle.circle", "square.circle", "triangle.circle",
          "dpad", "dpad.up.filled", "dpad.down.filled", "dpad.left.filled", "dpad.right.filled",
          "l.joystick", "r.joystick", "l.joystick.press.down", "r.joystick.press.down" },
        { "L1", "R1", "L2", "R2", "Options", "Share", "PS", "Cross", "Circle", "Square", "Triangle",
          "D-pad", "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Left Stick", "Right Stick", "L3", "R3" } };

    // Nintendo's A/B and X/Y sit swapped; GameController reports their labels
    const Names kNintendo = {
        { "l.rectangle.roundedbottom", "r.rectangle.roundedbottom", "zl.rectangle.roundedtop", "zr.rectangle.roundedtop",
          "plus.circle", "minus.circle", "house.circle", "b.circle", "a.circle", "y.circle", "x.circle",
          "dpad", "dpad.up.filled", "dpad.down.filled", "dpad.left.filled", "dpad.right.filled",
          "l.joystick", "r.joystick", "l.joystick.press.down", "r.joystick.press.down" },
        { "L", "R", "ZL", "ZR", "+", "-", "Home", "B", "A", "Y", "X",
          "D-pad", "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Left Stick", "Right Stick", "L Stick", "R Stick" } };

    const Names& Current()
    {
        switch (Detect())
        {
        case Family::PlayStation: return kPlayStation;
        case Family::Nintendo: return kNintendo;
        default: return kXbox;
        }
    }

    char* Symbol(Button b) { return gosr::MarshalString(Current().symbol[b]); }
    char* Alias(Button b) { return gosr::MarshalString(Current().alias[b]); }
}

#define GOSR_BUTTON(name, button) \
    GOSR_EXPORT char* _get##name##SymbolName() { return Symbol(button); } \
    GOSR_EXPORT char* _get##name##Alias() { return Alias(button); }

GOSR_BUTTON(LeftShoulderButton, LShoulder)
GOSR_BUTTON(RightShoulderButton, RShoulder)
GOSR_BUTTON(LeftTriggerButton, LTrigger)
GOSR_BUTTON(RightTriggerButton, RTrigger)
GOSR_BUTTON(MenuButton, Menu)
GOSR_BUTTON(OptionsButton, Options)
GOSR_BUTTON(HomeButton, Home)
GOSR_BUTTON(AButton, A)
GOSR_BUTTON(BButton, B)
GOSR_BUTTON(XButton, X)
GOSR_BUTTON(YButton, Y)
GOSR_BUTTON(DPad, DPad)
GOSR_BUTTON(DPadUpButton, DUp)
GOSR_BUTTON(DPadDownButton, DDown)
GOSR_BUTTON(DPadLeftButton, DLeft)
GOSR_BUTTON(DPadRightButton, DRight)
GOSR_BUTTON(LeftThumbStick, LStick)
GOSR_BUTTON(RightThumbStick, RStick)
GOSR_BUTTON(LeftThumbStickButton, LStickButton)
GOSR_BUTTON(RightThumbStickButton, RStickButton)
