#include "runtime/pad_config.h"
#include "ps2_host_backend.h"

#if defined(__linux__)
#include "runtime/pad_evdev_linux.h"
#endif
#if !defined(__linux__)
struct PadEvdevStub   // stands in for PadEvdevLinux where there is no evdev: never available, reads nothing
{
    bool isAvailable() const { return false; }
    bool matchesName(const char *) const { return false; }
    float getAxis(int) const { return 0.0f; }
    bool isButtonDown(int) const { return false; }
};
#endif

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <vector>
#include <algorithm>

namespace ps2_stubs
{
    namespace
    {
        // PS2 active-low button layout (bit n = button n).
        constexpr uint16_t kBtnSelect = 1u << 0;
        constexpr uint16_t kBtnL3 = 1u << 1;
        constexpr uint16_t kBtnR3 = 1u << 2;
        constexpr uint16_t kBtnStart = 1u << 3;
        constexpr uint16_t kBtnUp = 1u << 4;
        constexpr uint16_t kBtnRight = 1u << 5;
        constexpr uint16_t kBtnDown = 1u << 6;
        constexpr uint16_t kBtnLeft = 1u << 7;
        constexpr uint16_t kBtnL2 = 1u << 8;
        constexpr uint16_t kBtnR2 = 1u << 9;
        constexpr uint16_t kBtnL1 = 1u << 10;
        constexpr uint16_t kBtnR1 = 1u << 11;
        constexpr uint16_t kBtnTriangle = 1u << 12;
        constexpr uint16_t kBtnCircle = 1u << 13;
        constexpr uint16_t kBtnCross = 1u << 14;
        constexpr uint16_t kBtnSquare = 1u << 15;

        uint16_t buttonMaskForAction(PadAction action)
        {
            switch (action)
            {
            case PadAction::Select: return kBtnSelect;
            case PadAction::L3: return kBtnL3;
            case PadAction::R3: return kBtnR3;
            case PadAction::Start: return kBtnStart;
            case PadAction::Up: return kBtnUp;
            case PadAction::Right: return kBtnRight;
            case PadAction::Down: return kBtnDown;
            case PadAction::Left: return kBtnLeft;
            case PadAction::L2: return kBtnL2;
            case PadAction::R2: return kBtnR2;
            case PadAction::L1: return kBtnL1;
            case PadAction::R1: return kBtnR1;
            case PadAction::Triangle: return kBtnTriangle;
            case PadAction::Circle: return kBtnCircle;
            case PadAction::Cross: return kBtnCross;
            case PadAction::Square: return kBtnSquare;
            default: return 0u;
            }
        }

        bool isButtonAction(PadAction action)
        {
            return buttonMaskForAction(action) != 0u;
        }

#if defined(__linux__)
        bool nativeGamepadMatches(const std::vector<int> &pads, const PadEvdevLinux &native)
        {
            for (int pad : pads)
            {
                const char *name = GetGamepadName(pad);
                if (name && native.matchesName(name))
                    return true;
            }
            return false;
        }
#endif

        uint8_t clampToByte(int v)
        {
            return static_cast<uint8_t>(std::clamp(v, 0, 255));
        }

        uint8_t floatToByte(float v)
        {
            v = std::clamp(v, -1.0f, 1.0f);
            return clampToByte(static_cast<int>(std::lround(128.0f + v * 127.0f)));
        }

        // One-time gamepad mapping setup (8BitDo Ultimate + PS2X_PAD_MAPPINGS file).
        void ensureGamepadMappings()
        {
            static bool s_mapped = false;
            if (s_mapped)
            {
                return;
            }
            s_mapped = true;
            SetGamepadMappings(
                "03000000c82d00000631000014010000,8BitDo Ultimate Wireless,platform:Linux,"
                "a:b0,b:b1,x:b2,y:b3,back:b6,start:b7,guide:b8,leftstick:b9,rightstick:b10,"
                "leftshoulder:b4,rightshoulder:b5,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,"
                "leftx:a0,lefty:a1,rightx:a3,righty:a4,lefttrigger:a2,righttrigger:a5");
            // Microsoft Xbox Controller via xone (0x045e:0x0b12): bus 06 means GLFW counts
            // buttons from BTN_MISC, so the button indices are the offset 0x30..0x3e from
            // BTN_A..BTN_THUMBR. The native evdev reader is the primary path, but this
            // mapping is a fallback in case another pad appears with the same GUID.
            SetGamepadMappings(
                "060000005e040000120b000017050000,Microsoft Xbox Controller,platform:Linux,"
                "a:b48,b:b49,x:b51,y:b52,back:b58,start:b59,guide:b60,leftstick:b61,rightstick:b62,"
                "leftshoulder:b54,rightshoulder:b55,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,"
                "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5");
            if (const char *mf = std::getenv("PS2X_PAD_MAPPINGS"))
            {
                if (FILE *f = std::fopen(mf, "rb"))
                {
                    std::fseek(f, 0, SEEK_END);
                    long sz = std::ftell(f);
                    std::fseek(f, 0, SEEK_SET);
                    if (sz > 0 && sz < 4 * 1024 * 1024)
                    {
                        char *buf = static_cast<char *>(std::malloc(static_cast<size_t>(sz) + 1));
                        if (buf && std::fread(buf, 1, static_cast<size_t>(sz), f) == static_cast<size_t>(sz))
                        {
                            buf[sz] = '\0';
                            SetGamepadMappings(buf);
                        }
                        std::free(buf);
                    }
                    std::fclose(f);
                }
            }
        }

        // Declared here rather than including GLFW/glfw3.h, which clashes with raylib.h.
        extern "C" int glfwJoystickIsGamepad(int jid);
        extern "C" const float *glfwGetJoystickAxes(int jid, int *count);
        extern "C" const unsigned char *glfwGetJoystickButtons(int jid, int *count);
        extern "C" const char *glfwGetJoystickName(int jid);
        bool slotLooksLikeGamepad(int g); // defined below

        // [padlog] PS2X_PADLOG=1: what does raylib actually see? The Device dropdown is built
        // from IsGamepadAvailable(), which only reports pads that have a GAMEPAD MAPPING -- a
        // controller the kernel exposes fine but GLFW has no mapping for never appears. That is
        // what happened to the 8BitDo (hence the hardcoded SetGamepadMappings above). Print
        // every slot once so a missing pad can be told apart from a mis-mapped one.
        void logGamepadSlotsOnce()
        {
            static const bool s_on = [](){ const char *v = std::getenv("PS2X_PADLOG"); return v && v[0] == '1'; }();
            static bool s_done = false;
            if (!s_on || s_done)
                return;
            // Only latch once a pad has actually appeared: GLFW enumerates joysticks during the
            // first frames, so logging at the very first call reports an empty list and hides
            // whatever turns up a moment later.
            bool any = false;
            for (int g = 0; g < 16 && !any; ++g) any = IsGamepadAvailable(g);
            if (!any)
                return;
            s_done = true;
            for (int g = 0; g < 16; ++g)
            {
                const bool avail = IsGamepadAvailable(g);
                const char *nm = avail ? glfwGetJoystickName(g) : nullptr; // raylib's buffer overflows
                if (avail)
                    std::fprintf(stderr, "[padlog] slot %d: AVAILABLE name='%s' axes=%d listed=%d\n",
                                 g, nm ? nm : "?", GetGamepadAxisCount(g),
                                 slotLooksLikeGamepad(g) ? 1 : 0);
                else
                    std::fprintf(stderr, "[padlog] slot %d: not available (no mapping, or empty)\n", g);
            }
        }

        // GLFW knows what is a controller; raylib does not expose it.
        //
        // raylib reports MAX_GAMEPAD_AXIS for every slot, so its axis count cannot tell a
        // DualSense from a keyboard -- both come back as 6. GLFW reports the truth: 1 axis for
        // the "KBDFans System Control"/"Consumer Control" devices that udev mislabels with
        // ID_INPUT_JOYSTICK, 6 for a real pad. It also knows whether a slot has an SDL gamepad
        // mapping. raylib links GLFW statically, so query it directly.
        //
        // A slot is offered if it has a gamepad mapping OR enough real axes to be a controller.
        // The mapping test alone is not enough: pads whose GUID is missing from the database
        // (the 8BitDo Ultimate here) report no mapping yet are perfectly usable once bound.
        bool slotLooksLikeGamepad(int g)
        {
            if (!IsGamepadAvailable(g))
                return false;
            static const bool s_all = [](){ const char *v = std::getenv("PS2X_PAD_ALLDEV"); return v && v[0] == '1'; }();
            if (s_all) // escape hatch if this ever rejects a legitimate pad
                return true;
            if (glfwJoystickIsGamepad(g))
                return true;
            // Axes alone are not enough: a DualSense also publishes separate "Motion Sensors"
            // and "Touchpad" joysticks that report 6 axes each. Buttons separate them -- those
            // have 0 and 4, a real pad has 15-17, and the mislabelled keyboards have 1-2 axes.
            int nAxes = 0, nButtons = 0;
            glfwGetJoystickAxes(g, &nAxes);
            glfwGetJoystickButtons(g, &nButtons);
            return nAxes >= 4 && nButtons >= 8;
        }

        std::vector<int> availableGamepads();

        // Map a "Gamepad N" index (how the Qt launcher names pads: 0 = first
        // gamepad detected) to a concrete GLFW/raylib slot, or -1 if out of range.
        // Accepts a raw GLFW slot as a fallback so configs written by the older
        // in-game overlay (which stored dev.glfwSlot directly) keep working.
        int gamepadIndexToSlot(int index)
        {
            const std::vector<int> avail = availableGamepads();
            if (index >= 0 && index < static_cast<int>(avail.size()))
            {
                return avail[index];
            }
            if (IsGamepadAvailable(index))
            {
                return index;
            }
            return -1;
        }

        // Reverse map: where does this GLFW slot sit in the "Gamepad N" list
        // (the position the launcher and pad_pN.conf use), or -1 if not a pad.
        int gamepadSlotToIndex(int glfwSlot)
        {
            if (glfwSlot < 0)
            {
                return -1;
            }
            const std::vector<int> avail = availableGamepads();
            for (size_t i = 0; i < avail.size(); ++i)
            {
                if (avail[i] == glfwSlot)
                {
                    return static_cast<int>(i);
                }
            }
            return IsGamepadAvailable(glfwSlot) ? glfwSlot : -1;
        }

        std::vector<int> availableGamepads()
        {
            logGamepadSlotsOnce();
            static const int s_forcedPad = []()
            {
                const char *v = std::getenv("PS2X_PAD");
                return v ? std::atoi(v) : -1;
            }();
            std::vector<int> pads;
            for (int g = 0; g < 16; ++g)
            {
                if (s_forcedPad >= 0 && g != s_forcedPad)
                {
                    continue;
                }
                if (slotLooksLikeGamepad(g))
                {
                    pads.push_back(g);
                }
            }
            return pads;
        }

        void setDefaultGamepadBinds(PadPlayerConfig &cfg)
        {
            auto btn = [&cfg](PadAction a, int button)
            {
                cfg.binds[static_cast<size_t>(a)] = PadBind{PadBindKind::Button, button, 1.0f, 0.15f};
            };
            auto axis = [&cfg](PadAction a, int axis, float sign)
            {
                cfg.binds[static_cast<size_t>(a)] = PadBind{PadBindKind::Axis, axis, sign, 0.15f};
            };
            btn(PadAction::Up, GAMEPAD_BUTTON_LEFT_FACE_UP);
            btn(PadAction::Down, GAMEPAD_BUTTON_LEFT_FACE_DOWN);
            btn(PadAction::Left, GAMEPAD_BUTTON_LEFT_FACE_LEFT);
            btn(PadAction::Right, GAMEPAD_BUTTON_LEFT_FACE_RIGHT);
            btn(PadAction::Square, GAMEPAD_BUTTON_RIGHT_FACE_LEFT);
            btn(PadAction::Cross, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
            btn(PadAction::Circle, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT);
            btn(PadAction::Triangle, GAMEPAD_BUTTON_RIGHT_FACE_UP);
            btn(PadAction::L1, GAMEPAD_BUTTON_LEFT_TRIGGER_1);
            btn(PadAction::R1, GAMEPAD_BUTTON_RIGHT_TRIGGER_1);
            btn(PadAction::L2, GAMEPAD_BUTTON_LEFT_TRIGGER_2);
            btn(PadAction::R2, GAMEPAD_BUTTON_RIGHT_TRIGGER_2);
            btn(PadAction::L3, GAMEPAD_BUTTON_LEFT_THUMB);
            btn(PadAction::R3, GAMEPAD_BUTTON_RIGHT_THUMB);
            btn(PadAction::Select, GAMEPAD_BUTTON_MIDDLE_LEFT);
            btn(PadAction::Start, GAMEPAD_BUTTON_MIDDLE_RIGHT);
            axis(PadAction::LStickXNeg, GAMEPAD_AXIS_LEFT_X, -1.0f);
            axis(PadAction::LStickXPos, GAMEPAD_AXIS_LEFT_X, 1.0f);
            axis(PadAction::LStickYNeg, GAMEPAD_AXIS_LEFT_Y, -1.0f);
            axis(PadAction::LStickYPos, GAMEPAD_AXIS_LEFT_Y, 1.0f);
            axis(PadAction::RStickXNeg, GAMEPAD_AXIS_RIGHT_X, -1.0f);
            axis(PadAction::RStickXPos, GAMEPAD_AXIS_RIGHT_X, 1.0f);
            axis(PadAction::RStickYNeg, GAMEPAD_AXIS_RIGHT_Y, -1.0f);
            axis(PadAction::RStickYPos, GAMEPAD_AXIS_RIGHT_Y, 1.0f);
        }

        void setDefaultKeyboardBinds(PadPlayerConfig &cfg)
        {
            auto key = [&cfg](PadAction a, int key)
            {
                cfg.binds[static_cast<size_t>(a)] = PadBind{PadBindKind::Key, key, 1.0f, 0.0f};
            };
            key(PadAction::Up, KEY_UP);
            key(PadAction::Down, KEY_DOWN);
            key(PadAction::Left, KEY_LEFT);
            key(PadAction::Right, KEY_RIGHT);
            key(PadAction::Square, KEY_Z);
            key(PadAction::Cross, KEY_X);
            key(PadAction::Circle, KEY_C);
            key(PadAction::Triangle, KEY_V);
            key(PadAction::L1, KEY_Q);
            key(PadAction::R1, KEY_E);
            key(PadAction::L2, KEY_ONE);
            key(PadAction::R2, KEY_THREE);
            key(PadAction::L3, KEY_LEFT_CONTROL);
            key(PadAction::R3, KEY_RIGHT_CONTROL);
            key(PadAction::Select, KEY_RIGHT_SHIFT);
            key(PadAction::Start, KEY_ENTER);
            // Stick-direction keys need a SIGN: the Neg actions must contribute -1 to the
            // axis sum (both W and S pushed the stick the same way with the flat +1 default).
            auto stickKey = [&cfg](PadAction a, int k, float sign)
            {
                cfg.binds[static_cast<size_t>(a)] = PadBind{PadBindKind::Key, k, sign, 0.0f};
            };
            stickKey(PadAction::LStickXNeg, KEY_A, -1.0f);
            stickKey(PadAction::LStickXPos, KEY_D, 1.0f);
            stickKey(PadAction::LStickYNeg, KEY_W, -1.0f);
            stickKey(PadAction::LStickYPos, KEY_S, 1.0f);
        }

        PadPlayerConfig makeDefaultPlayer()
        {
            PadPlayerConfig cfg;
            setDefaultGamepadBinds(cfg);
            return cfg;
        }

        // Legacy "auto" poll: distribute devices per player instead of merging
        // every pad+keyboard into one packet (that bug made P1 drive both sides).
        //   >= 2 gamepads:  player 0 -> gamepad[0], player 1 -> gamepad[1]
        //   1 gamepad + kb: player 0 -> gamepad[0], player 1 -> keyboard
        //   no gamepad:     both players -> keyboard
        void pollLegacyAuto(PadPacket &pkt, size_t player)
        {
            const std::vector<int> allPads = availableGamepads();
            // This player's gamepads: dedicate one when enough are present,
            // otherwise none (falls back to keyboard for that player).
            std::vector<int> pads;
            if (!allPads.empty() && (player == 0 || allPads.size() >= 2))
            {
                pads.push_back(allPads[player < allPads.size() ? player : 0]);
            }
            const bool kbdDrives = pads.empty();   // no pad for this player
#if defined(__linux__)
            const PadEvdevLinux &native = PadEvdevLinux::instance();
            const bool nativeOk = native.isAvailable() && nativeGamepadMatches(pads, native);
#else
            const bool nativeOk = false;
            static const PadEvdevStub native;   // no evdev here
#endif
            auto mergeAxis = [&](int pad, int axis, float &dst)
            {
                float v = GetGamepadAxisMovement(pad, axis);
#if defined(__linux__)
                if (nativeOk)
                {
                    const float nv = native.getAxis(axis);
                    if (std::fabs(nv) > std::fabs(v))
                    {
                        v = nv;
                    }
                }
#endif
                if (std::fabs(v) > std::fabs(dst))
                {
                    dst = v;
                }
            };
            float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
            for (int pad : pads)
            {
                mergeAxis(pad, GAMEPAD_AXIS_LEFT_X, lx);
                mergeAxis(pad, GAMEPAD_AXIS_LEFT_Y, ly);
                mergeAxis(pad, GAMEPAD_AXIS_RIGHT_X, rx);
                mergeAxis(pad, GAMEPAD_AXIS_RIGHT_Y, ry);

                auto btn = [&](PadAction a, int button)
                {
                    if (IsGamepadButtonDown(pad, button) ||
                        (nativeOk && native.isButtonDown(button)))
                    {
                        pkt.buttons = static_cast<uint16_t>(pkt.buttons & ~buttonMaskForAction(a));
                    }
                };
                btn(PadAction::Up, GAMEPAD_BUTTON_LEFT_FACE_UP);
                btn(PadAction::Down, GAMEPAD_BUTTON_LEFT_FACE_DOWN);
                btn(PadAction::Left, GAMEPAD_BUTTON_LEFT_FACE_LEFT);
                btn(PadAction::Right, GAMEPAD_BUTTON_LEFT_FACE_RIGHT);
                btn(PadAction::Cross, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
                btn(PadAction::Circle, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT);
                btn(PadAction::Square, GAMEPAD_BUTTON_RIGHT_FACE_LEFT);
                btn(PadAction::Triangle, GAMEPAD_BUTTON_RIGHT_FACE_UP);
                btn(PadAction::L1, GAMEPAD_BUTTON_LEFT_TRIGGER_1);
                btn(PadAction::R1, GAMEPAD_BUTTON_RIGHT_TRIGGER_1);
                btn(PadAction::L2, GAMEPAD_BUTTON_LEFT_TRIGGER_2);
                btn(PadAction::R2, GAMEPAD_BUTTON_RIGHT_TRIGGER_2);
                btn(PadAction::L3, GAMEPAD_BUTTON_LEFT_THUMB);
                btn(PadAction::R3, GAMEPAD_BUTTON_RIGHT_THUMB);
                btn(PadAction::Select, GAMEPAD_BUTTON_MIDDLE_LEFT);
                btn(PadAction::Start, GAMEPAD_BUTTON_MIDDLE_RIGHT);
            }
            // Read native axes even when GLFW has no mapping for the
            // controller (pads empty).  Only the first player inherits the
            // unmapped native device; player 1 falls back to keyboard instead
            // of sharing P1's pad.
            if (pads.empty() && nativeOk && player == 0)
            {
                lx = native.getAxis(GAMEPAD_AXIS_LEFT_X);
                ly = native.getAxis(GAMEPAD_AXIS_LEFT_Y);
                rx = native.getAxis(GAMEPAD_AXIS_RIGHT_X);
                ry = native.getAxis(GAMEPAD_AXIS_RIGHT_Y);
            }
            if (!pads.empty() || (nativeOk && player == 0))
            {
                pkt.lx = floatToByte(lx);
                pkt.ly = floatToByte(ly);
                pkt.rx = floatToByte(rx);
                pkt.ry = floatToByte(ry);
            }

            // PS2X_NOKB=1: ignore the keyboard entirely for pad input. Needed when a
            // remote-streaming host (Sunshine "Keyboard passthrough") injects gamepad-
            // as-keyboard events — its X spam maps to Cross and skip-mashes cutscenes.
            static const bool s_noKb = [](){ const char *v = std::getenv("PS2X_NOKB"); return v && v[0] == '1'; }();
            auto key = [&pkt](PadAction a, int key)
            {
                if (!s_noKb && IsKeyDown(key))
                {
                    pkt.buttons = static_cast<uint16_t>(pkt.buttons & ~buttonMaskForAction(a));
                }
            };
            if (!s_noKb && kbdDrives)
            {
                key(PadAction::Up, KEY_UP);
                key(PadAction::Down, KEY_DOWN);
                key(PadAction::Left, KEY_LEFT);
                key(PadAction::Right, KEY_RIGHT);
                key(PadAction::Square, KEY_Z);
                key(PadAction::Cross, KEY_X);
                key(PadAction::Circle, KEY_C);
                key(PadAction::Triangle, KEY_V);
                key(PadAction::L1, KEY_Q);
                key(PadAction::R1, KEY_E);
                key(PadAction::L2, KEY_ONE);
                key(PadAction::R2, KEY_THREE);
                key(PadAction::L3, KEY_LEFT_CONTROL);
                key(PadAction::R3, KEY_RIGHT_CONTROL);
                key(PadAction::Select, KEY_RIGHT_SHIFT);
                key(PadAction::Start, KEY_ENTER);

                float ax = 0.0f;
                float ay = 0.0f;
                if (IsKeyDown(KEY_D)) ax += 1.0f;
                if (IsKeyDown(KEY_A)) ax -= 1.0f;
                if (IsKeyDown(KEY_S)) ay += 1.0f;
                if (IsKeyDown(KEY_W)) ay -= 1.0f;
                if (ax != 0.0f || ay != 0.0f)
                {
                    pkt.lx = floatToByte(ax);
                    pkt.ly = floatToByte(ay);
                }
            }
        }
    }

    const char *padActionName(PadAction action)
    {
        switch (action)
        {
        case PadAction::Select: return "Select";
        case PadAction::L3: return "L3";
        case PadAction::R3: return "R3";
        case PadAction::Start: return "Start";
        case PadAction::Up: return "D-Pad Up";
        case PadAction::Right: return "D-Pad Right";
        case PadAction::Down: return "D-Pad Down";
        case PadAction::Left: return "D-Pad Left";
        case PadAction::L2: return "L2";
        case PadAction::R2: return "R2";
        case PadAction::L1: return "L1";
        case PadAction::R1: return "R1";
        case PadAction::Triangle: return "Triangle";
        case PadAction::Circle: return "Circle";
        case PadAction::Cross: return "Cross";
        case PadAction::Square: return "Square";
        case PadAction::LStickXNeg: return "L Stick X -";
        case PadAction::LStickXPos: return "L Stick X +";
        case PadAction::LStickYNeg: return "L Stick Y -";
        case PadAction::LStickYPos: return "L Stick Y +";
        case PadAction::RStickXNeg: return "R Stick X -";
        case PadAction::RStickXPos: return "R Stick X +";
        case PadAction::RStickYNeg: return "R Stick Y -";
        case PadAction::RStickYPos: return "R Stick Y +";
        default: return "?";
        }
    }

    const char *padBindKindName(PadBindKind kind)
    {
        switch (kind)
        {
        case PadBindKind::Key: return "Key";
        case PadBindKind::Button: return "Button";
        case PadBindKind::Axis: return "Axis";
        default: return "None";
        }
    }

    namespace
    {
        std::string keyName(int key)
        {
            switch (key)
            {
            case KEY_UP: return "Up";
            case KEY_DOWN: return "Down";
            case KEY_LEFT: return "Left";
            case KEY_RIGHT: return "Right";
            case KEY_A: return "A";
            case KEY_B: return "B";
            case KEY_C: return "C";
            case KEY_D: return "D";
            case KEY_E: return "E";
            case KEY_Q: return "Q";
            case KEY_S: return "S";
            case KEY_V: return "V";
            case KEY_W: return "W";
            case KEY_X: return "X";
            case KEY_Z: return "Z";
            case KEY_ONE: return "1";
            case KEY_TWO: return "2";
            case KEY_THREE: return "3";
            case KEY_FOUR: return "4";
            case KEY_FIVE: return "5";
            case KEY_SIX: return "6";
            case KEY_SEVEN: return "7";
            case KEY_EIGHT: return "8";
            case KEY_NINE: return "9";
            case KEY_ZERO: return "0";
            case KEY_ENTER: return "Enter";
            case KEY_SPACE: return "Space";
            case KEY_TAB: return "Tab";
            case KEY_ESCAPE: return "Esc";
            case KEY_LEFT_SHIFT: return "LShift";
            case KEY_RIGHT_SHIFT: return "RShift";
            case KEY_LEFT_CONTROL: return "LCtrl";
            case KEY_RIGHT_CONTROL: return "RCtrl";
            case KEY_LEFT_ALT: return "LAlt";
            case KEY_RIGHT_ALT: return "RAlt";
            case KEY_LEFT_SUPER: return "LMeta";
            case KEY_RIGHT_SUPER: return "RMeta";
            default:
            {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "KEY_%d", key);
                return buf;
            }
            }
        }

        const char *gamepadButtonName(int button)
        {
            switch (button)
            {
            case GAMEPAD_BUTTON_LEFT_FACE_UP: return "DPad Up";
            case GAMEPAD_BUTTON_LEFT_FACE_RIGHT: return "DPad Right";
            case GAMEPAD_BUTTON_LEFT_FACE_DOWN: return "DPad Down";
            case GAMEPAD_BUTTON_LEFT_FACE_LEFT: return "DPad Left";
            case GAMEPAD_BUTTON_RIGHT_FACE_UP: return "Y/Triangle";
            case GAMEPAD_BUTTON_RIGHT_FACE_RIGHT: return "B/Circle";
            case GAMEPAD_BUTTON_RIGHT_FACE_DOWN: return "A/Cross";
            case GAMEPAD_BUTTON_RIGHT_FACE_LEFT: return "X/Square";
            case GAMEPAD_BUTTON_LEFT_TRIGGER_1: return "LB/L1";
            case GAMEPAD_BUTTON_LEFT_TRIGGER_2: return "LT/L2";
            case GAMEPAD_BUTTON_RIGHT_TRIGGER_1: return "RB/R1";
            case GAMEPAD_BUTTON_RIGHT_TRIGGER_2: return "RT/R2";
            case GAMEPAD_BUTTON_MIDDLE_LEFT: return "Back/Select";
            case GAMEPAD_BUTTON_MIDDLE: return "Guide";
            case GAMEPAD_BUTTON_MIDDLE_RIGHT: return "Start";
            case GAMEPAD_BUTTON_LEFT_THUMB: return "L3";
            case GAMEPAD_BUTTON_RIGHT_THUMB: return "R3";
            default:
            {
                static char buf[32];
                std::snprintf(buf, sizeof(buf), "B%d", button);
                return buf;
            }
            }
        }

        const char *gamepadAxisName(int axis)
        {
            switch (axis)
            {
            case GAMEPAD_AXIS_LEFT_X: return "LStick X";
            case GAMEPAD_AXIS_LEFT_Y: return "LStick Y";
            case GAMEPAD_AXIS_RIGHT_X: return "RStick X";
            case GAMEPAD_AXIS_RIGHT_Y: return "RStick Y";
            case GAMEPAD_AXIS_LEFT_TRIGGER: return "LT";
            case GAMEPAD_AXIS_RIGHT_TRIGGER: return "RT";
            default:
            {
                static char buf[32];
                std::snprintf(buf, sizeof(buf), "Axis%d", axis);
                return buf;
            }
            }
        }
    }

    std::string padBindDisplay(const PadBind &bind)
    {
        switch (bind.kind)
        {
        case PadBindKind::Key:
            return keyName(bind.value);
        case PadBindKind::Button:
            return gamepadButtonName(bind.value);
        case PadBindKind::Axis:
            return std::string(gamepadAxisName(bind.value)) + (bind.sign < 0.0f ? " -" : " +");
        default:
            return "-";
        }
    }

    std::string padDeviceDisplay(const PadDevice &device)
    {
        switch (device.kind)
        {
        case PadDeviceKind::Keyboard:
            return "Keyboard";
        case PadDeviceKind::Gamepad:
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Gamepad %d", device.gamepad);
            return buf;
        }
        default:
            return "Auto (any pad + keyboard)";
        }
    }

    void padConfigInit(const std::string &elfDir)
    {
        PadConfig &cfg = PadConfig::instance();
        cfg.setDefaultDir(elfDir);
        cfg.load();
    }

    // Public wrappers so the in-game overlay persists the *index* naming the
    // launcher uses ("Gamepad 0/1/..." in pad_pN.conf), never the raw GLFW slot.
    // The slot layout differs from the evdev enumeration the launcher shows, and
    // is what made "Gamepad 1" silently point at a dead slot.
    int padGamepadSlot(int index)
    {
        return gamepadIndexToSlot(index);
    }

    int padGamepadIndex(int glfwSlot)
    {
        return gamepadSlotToIndex(glfwSlot);
    }

    PadPacket padPollPlayer(size_t player)
    {
        return PadConfig::instance().poll(player);
    }

    uint16_t ps2xLivePadButtons(int player, uint8_t &lx, uint8_t &ly, uint8_t &rx, uint8_t &ry)
    {
        const PadPacket pkt = PadConfig::instance().poll(static_cast<size_t>(player));
        lx = pkt.lx;
        ly = pkt.ly;
        rx = pkt.rx;
        ry = pkt.ry;
        return pkt.buttons;
    }

    PadConfig::PadConfig()
    {
        for (size_t i = 0; i < kPlayerCount; ++i)
        {
            m_players[i] = makeDefaultPlayer();
        }
    }

    PadConfig &PadConfig::instance()
    {
        static PadConfig cfg;
        return cfg;
    }

    bool PadConfig::s_inputSuspended = false;
    void PadConfig::setInputSuspended(bool suspended) { s_inputSuspended = suspended; }
    bool PadConfig::inputSuspended() { return s_inputSuspended; }

    PadPlayerConfig PadConfig::snapshot(size_t p) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return (p < kPlayerCount) ? m_players[p] : PadPlayerConfig{};
    }

    void PadConfig::setDevice(size_t p, const PadDevice &device)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (p < kPlayerCount)
        {
            m_players[p].device = device;
        }
    }

    void PadConfig::setBind(size_t p, PadAction action, const PadBind &bind)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (p < kPlayerCount && action != PadAction::Count)
        {
            m_players[p].binds[static_cast<size_t>(action)] = bind;
        }
    }

    void PadConfig::setPlayerDefaults(size_t p, PadDeviceKind kind)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (p >= kPlayerCount)
        {
            return;
        }
        m_players[p] = makeDefaultPlayer();
        m_players[p].device = PadDevice{kind, -1};
        if (kind == PadDeviceKind::Keyboard)
        {
            setDefaultKeyboardBinds(m_players[p]);
        }
    }

    void PadConfig::resetPlayer(size_t p)
    {
        setPlayerDefaults(p, PadDeviceKind::None);
    }

    void PadConfig::setDefaultDir(const std::string &elfDir)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_dir = elfDir;
    }

    std::string PadConfig::defaultPath() const
    {
        if (const char *env = std::getenv("PS2X_PAD_CONFIG"))
        {
            return env;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_dir.empty())
        {
            return m_dir + "/pad.conf";
        }
        return "./pad.conf";
    }

    // Savedata files live in <ELF dir>/savedata. Each player gets its own file
    // (pad_p1.conf / pad_p2.conf); the legacy single pad.conf is still read as a
    // fallback so pre-existing setups keep working (migrated to the new files).
    static const char *kPlayerFilePrefix = "pad_p";
    static const char *kLegacyFileName = "pad.conf";

    std::string PadConfig::playerConfigPath(size_t p) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::string dir = (m_dir.empty() ? std::string(".") : m_dir) + "/savedata";
        return dir + "/" + kPlayerFilePrefix + std::to_string(p + 1) + ".conf";
    }

    std::string PadConfig::legacyConfigPath() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return (m_dir.empty() ? std::string(".") : m_dir) + "/" + kLegacyFileName;
    }

    namespace
    {
        // Parse one player file (/pad_pN.conf): lines use the classic
        // "player <idx>" framing, but only one player lives in each file.
        bool parsePlayerFile(const std::string &path, size_t fixedPlayer,
                             PadPlayerConfig players[PadConfig::kPlayerCount], bool &any)
        {
            std::ifstream in(path);
            if (!in.is_open())
            {
                return false;
            }
            std::string line;
            while (std::getline(in, line))
            {
                std::istringstream ss(line);
                std::string tok;
                if (!(ss >> tok) || tok != "player")
                {
                    continue;
                }
                int idx = -1;
                if (!(ss >> idx) || idx < 0 || idx >= static_cast<int>(PadConfig::kPlayerCount))
                {
                    continue;
                }
                if (!(ss >> tok))
                {
                    continue;
                }
                PadPlayerConfig &dst = players[fixedPlayer];
                if (tok == "device")
                {
                    std::string kind;
                    if (!(ss >> kind))
                    {
                        continue;
                    }
                    PadDevice &dev = dst.device;
                    if (kind == "Keyboard")
                    {
                        dev = PadDevice{PadDeviceKind::Keyboard, -1};
                        setDefaultKeyboardBinds(dst);
                    }
                    else if (kind == "Gamepad")
                    {
                        int g = -1;
                        if (ss >> g)
                        {
                            dev = PadDevice{PadDeviceKind::Gamepad, g};
                        }
                    }
                    else
                    {
                        dev = PadDevice{PadDeviceKind::None, -1};
                    }
                    any = true;
                }
                else if (tok == "bind")
                {
                    // The action name may contain spaces ("D-Pad Up", "L Stick X -"): read tokens until
                    // the bind kind. The old single-token read silently dropped every such line, so a saved
                    // D-pad or stick binding never came back after a restart.
                    std::string actionName, kindName, tok2;
                    while (ss >> tok2)
                    {
                        if (tok2 == "Key" || tok2 == "Button" || tok2 == "Axis") { kindName = tok2; break; }
                        actionName += (actionName.empty() ? "" : " ") + tok2;
                    }
                    if (kindName.empty())
                    {
                        continue;
                    }
                    int value = -1;
                    if (!(ss >> value))
                    {
                        continue;
                    }
                    float sign = 1.0f;
                    ss >> sign;

                    PadBindKind kind = PadBindKind::None;
                    if (kindName == "Key") kind = PadBindKind::Key;
                    else if (kindName == "Button") kind = PadBindKind::Button;
                    else if (kindName == "Axis") kind = PadBindKind::Axis;

                    PadAction action = PadAction::Count;
                    for (size_t a = 0; a < static_cast<size_t>(PadAction::Count); ++a)
                    {
                        if (actionName == padActionName(static_cast<PadAction>(a)))
                        {
                            action = static_cast<PadAction>(a);
                            break;
                        }
                    }
                    if (action == PadAction::Count || kind == PadBindKind::None)
                    {
                        continue;
                    }
                    PadBind bind;
                    bind.kind = kind;
                    bind.value = value;
                    bind.sign = (sign < 0.0f) ? -1.0f : 1.0f;   // the file stores the sign as -1/1
                    dst.binds[static_cast<size_t>(action)] = bind;
                    any = true;
                }
            }
            return true;
        }
    } // namespace

    bool PadConfig::load()
    {
        PadPlayerConfig players[kPlayerCount];
        for (size_t i = 0; i < kPlayerCount; ++i)
        {
            players[i] = makeDefaultPlayer();
        }

        // Prefer one savedata file per player (savedata/pad_p1.conf, pad_p2.conf).
        bool any = false;
        bool foundNew = false;
        for (size_t p = 0; p < kPlayerCount; ++p)
        {
            const std::string path = playerConfigPath(p);
            const bool openedFile = parsePlayerFile(path, p, players, any);
            if (openedFile)
            {
                foundNew = true;
            }
        }

        // Migration: no per-player files yet -> read the old single pad.conf.
        if (!foundNew)
        {
            const std::string legacy = legacyConfigPath();
            {
                std::ifstream in(legacy);
                if (in.is_open())
                {
                    std::string line;
                    while (std::getline(in, line))
                    {
                        std::istringstream ss(line);
                        std::string tok;
                        if (!(ss >> tok) || tok != "player")
                        {
                            continue;
                        }
                        int idx = -1;
                        if (!(ss >> idx) || idx < 0 || idx >= static_cast<int>(kPlayerCount))
                        {
                            continue;
                        }
                        if (!(ss >> tok) || idx >= static_cast<int>(kPlayerCount))
                        {
                            continue;
                        }
                        if (tok == "device")
                        {
                            std::string kind;
                            if (!(ss >> kind))
                            {
                                continue;
                            }
                            PadDevice &dev = players[idx].device;
                            if (kind == "Keyboard")
                            {
                                dev = PadDevice{PadDeviceKind::Keyboard, -1};
                                setDefaultKeyboardBinds(players[idx]);
                            }
                            else if (kind == "Gamepad")
                            {
                                int g = -1;
                                if (ss >> g)
                                {
                                    dev = PadDevice{PadDeviceKind::Gamepad, g};
                                }
                            }
                            else
                            {
                                dev = PadDevice{PadDeviceKind::None, -1};
                            }
                            any = true;
                        }
                        else if (tok == "bind")
                        {
                            std::string actionName, kindName, tok2;   // multi-word action names (see above)
                            while (ss >> tok2)
                            {
                                if (tok2 == "Key" || tok2 == "Button" || tok2 == "Axis") { kindName = tok2; break; }
                                actionName += (actionName.empty() ? "" : " ") + tok2;
                            }
                            if (kindName.empty())
                            {
                                continue;
                            }
                            int value = -1;
                            if (!(ss >> value))
                            {
                                continue;
                            }
                            float sign = 1.0f;
                            ss >> sign;

                            PadBindKind kind = PadBindKind::None;
                            if (kindName == "Key") kind = PadBindKind::Key;
                            else if (kindName == "Button") kind = PadBindKind::Button;
                            else if (kindName == "Axis") kind = PadBindKind::Axis;

                            PadAction action = PadAction::Count;
                            for (size_t a = 0; a < static_cast<size_t>(PadAction::Count); ++a)
                            {
                                if (actionName == padActionName(static_cast<PadAction>(a)))
                                {
                                    action = static_cast<PadAction>(a);
                                    break;
                                }
                            }
                            if (action == PadAction::Count || kind == PadBindKind::None)
                            {
                                continue;
                            }
                            PadBind bind;
                            bind.kind = kind;
                            bind.value = value;
                            bind.sign = (sign < 0.0f) ? -1.0f : 1.0f;   // the file stores the sign as -1/1
                            players[idx].binds[static_cast<size_t>(action)] = bind;
                            any = true;
                        }
                    }
                }
            }
            if (any)
            {
                std::printf("[pad_config] migrating legacy pad.conf -> savedata/pad_pN.conf\n");
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (size_t i = 0; i < kPlayerCount; ++i)
            {
                m_players[i] = players[i];
            }
            m_loaded = true;
        }

        if (any)
        {
            // Persist the migrated / freshly-loaded config back into per-player
            // savedata files so the new layout is the one that sticks.
            (void)save();
        }
        std::printf("[pad_config] loaded (per-player savedata)\n");
        return any;
    }

    bool PadConfig::save() const
    {
        bool ok = true;
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::string dir = (m_dir.empty() ? std::string(".") : m_dir) + "/savedata";
        std::error_code mdEc;
        std::filesystem::create_directories(dir, mdEc);
        for (size_t p = 0; p < kPlayerCount; ++p)
        {
            const std::string path = dir + "/" + kPlayerFilePrefix + std::to_string(p + 1) + ".conf";
            const std::string tmp = path + ".tmp";
            {
                std::ofstream out(tmp, std::ios::trunc);
                if (!out.is_open())
                {
                    ok = false;
                    continue;
                }
                out << "# BT3-Recomp pad configuration - Player " << (p + 1) << "\n";
                out << "# player <N> device <None|Keyboard|Gamepad [index]>\n";
                out << "# player <N> bind <Action> <Key|Button|Axis> <value> [sign]\n";
                const PadPlayerConfig &cfg = m_players[p];
                out << "player " << p << " device";
                switch (cfg.device.kind)
                {
                case PadDeviceKind::Keyboard:
                    out << " Keyboard\n";
                    break;
                case PadDeviceKind::Gamepad:
                    out << " Gamepad " << cfg.device.gamepad << "\n";
                    break;
                default:
                    out << " None\n";
                    break;
                }
                for (size_t a = 0; a < static_cast<size_t>(PadAction::Count); ++a)
                {
                    const PadBind &bind = cfg.binds[a];
                    if (bind.kind == PadBindKind::None)
                    {
                        continue;
                    }
                    out << "player " << p << " bind " << padActionName(static_cast<PadAction>(a)) << " "
                        << padBindKindName(bind.kind) << " " << bind.value << " "
                        << (bind.sign < 0.0f ? -1 : 1) << "\n";
                }
            }
            std::error_code ec;
            std::filesystem::rename(tmp, path, ec);
            if (ec)
            {
                ok = false;
            }
            else
            {
                std::printf("[pad_config] saved %s\n", path.c_str());
            }
        }
        return ok;
    }

    PadPacket PadConfig::poll(size_t player) const
    {
        PadPacket pkt;
        if (player >= kPlayerCount)
        {
            return pkt;
        }

        // [overlay] settings overlay open: swallow pad input so it never reaches the game.
        if (s_inputSuspended)
        {
            return pkt;
        }
        ensureGamepadMappings();
        if (!IsWindowReady())
        {
            return pkt;
        }

        PadPlayerConfig cfg;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cfg = m_players[player];
        }

        const bool anyPad = cfg.device.kind == PadDeviceKind::None;
        if (anyPad)
        {
            pollLegacyAuto(pkt, player);
            return pkt;
        }

        std::vector<int> pads;
        if (cfg.device.kind == PadDeviceKind::Gamepad)
        {
            // pad_pN.conf stores the launcher's "Gamepad N" index (N-th pad that
            // reports as a controller), not the raw GLFW slot. Resolve it so a
            // single Xbox on GLFW slot 1 still matches "Gamepad 0".
            const int slot = padGamepadSlot(cfg.device.gamepad);
            if (slot >= 0 && IsGamepadAvailable(slot))
            {
                pads.push_back(slot);
            }
        }
        static const bool s_noKb2 = [](){ const char *v = std::getenv("PS2X_NOKB"); return v && v[0] == '1'; }();
        const bool keyboardAllowed = !s_noKb2 && cfg.device.kind != PadDeviceKind::Gamepad;

#if defined(__linux__)
        const PadEvdevLinux &native = PadEvdevLinux::instance();
        // The native reader is a singleton bound to ONE physical device (the
        // first pad found in /dev/input) = the launcher's "Gamepad 0". Only
        // that player may consult it; players on any other gamepad must rely
        // on their own GLFW slot, otherwise the singleton would feed every
        // name-matching player the same physical state (P2 mirrors P1).
        const bool nativeOwner = native.isAvailable() &&
                                 cfg.device.kind == PadDeviceKind::Gamepad &&
                                 cfg.device.gamepad == 0;
        // Several readers (buttons, triggers, axis fallback) pull from the
        // singleton: allowed for the owner, and only when these pads really
        // are that device (name match) — unless GLFW exposed no slots at all
        // (pads empty), where the singleton is the sole source for the owner.
        const bool nativeOk = nativeOwner &&
                              (pads.empty() || nativeGamepadMatches(pads, native));
#else
        const bool nativeOk = false;
        static const PadEvdevStub native;   // no evdev here
#endif

        // Buttons.
        for (size_t a = 0; a < static_cast<size_t>(PadAction::Count); ++a)
        {
            const PadBind &bind = cfg.binds[a];
            const PadAction action = static_cast<PadAction>(a);
            if (!isButtonAction(action) || bind.kind == PadBindKind::None)
            {
                continue;
            }
            bool pressed = false;
            if (bind.kind == PadBindKind::Key && keyboardAllowed)
            {
                pressed = IsKeyDown(bind.value);
            }
            else if (bind.kind == PadBindKind::Button)
            {
                for (int pad : pads)
                {
                    if (IsGamepadButtonDown(pad, bind.value) ||
                        (nativeOk && native.isButtonDown(bind.value)))
                    {
                        pressed = true;
                        break;
                    }
                }
            }
            else if (bind.kind == PadBindKind::Axis)
            {
                // Analog binds on button actions (e.g. L2/R2 saved as
                // "Axis 4/5" by capture): pressed once the axis passes its
                // deadzone. GLFW gives triggers 0..1, sticks -1..+1, so use
                // magnitude for the button state.
                for (int pad : pads)
                {
                    if (std::fabs(GetGamepadAxisMovement(pad, bind.value)) > bind.deadzone)
                    {
                        pressed = true;
                        break;
                    }
                }
                // Trigger axes with no GLFW mapping (only the native evdev
                // reader knows them): convert axis to the matching trigger
                // button code and forward the native button state.
                if (!pressed && nativeOk)
                {
                    const int btn = bind.value == GAMEPAD_AXIS_LEFT_TRIGGER ? GAMEPAD_BUTTON_LEFT_TRIGGER_2
                                   : bind.value == GAMEPAD_AXIS_RIGHT_TRIGGER ? GAMEPAD_BUTTON_RIGHT_TRIGGER_2
                                                                              : -1;
                    if (btn >= 0 && native.isButtonDown(btn))
                    {
                        pressed = true;
                    }
                }
            }
            if (pressed)
            {
                pkt.buttons = static_cast<uint16_t>(pkt.buttons & ~buttonMaskForAction(action));
            }
        }

        // Sticks: combine the -/+ slots for each of the four axes.
        auto slotValue = [&](const PadBind &bind) -> float
        {
            if (bind.kind == PadBindKind::Axis)
            {
                float best = 0.0f;
                for (int pad : pads)
                {
                    // Only this player's GLFW slot. The native evdev reader is a
                    // singleton bound to ONE physical device; injecting it here
                    // would feed every matching player the same physical axes,
                    // breaking per-player assignment when two controllers are
                    // used (sticks of P1 appear on P2).
                    float v = GetGamepadAxisMovement(pad, bind.value);
                    // Direction filter: only keep values in the bind's
                    // direction.  Without this, the opposing Neg/Pos slots
                    // both read the same underlying axis and cancel out
                    // (e.g. stick at +0.8 → Neg returns -0.8, Pos returns
                    // +0.8 → sum = 0).
                    if (bind.sign > 0.0f && v < 0.0f)
                        v = 0.0f;
                    else if (bind.sign < 0.0f && v > 0.0f)
                        v = 0.0f;
                    if (std::fabs(v) < bind.deadzone)
                    {
                        v = 0.0f;
                    }
                    if (std::fabs(v) > std::fabs(best))
                    {
                        best = v;
                    }
                }
                return best;
            }
            if (bind.kind == PadBindKind::Key && keyboardAllowed && IsKeyDown(bind.value))
            {
                return bind.sign;
            }
            return 0.0f;
        };

        float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
        lx = std::clamp(
            slotValue(cfg.binds[static_cast<size_t>(PadAction::LStickXNeg)]) +
                slotValue(cfg.binds[static_cast<size_t>(PadAction::LStickXPos)]),
            -1.0f, 1.0f);
        ly = std::clamp(
            slotValue(cfg.binds[static_cast<size_t>(PadAction::LStickYNeg)]) +
                slotValue(cfg.binds[static_cast<size_t>(PadAction::LStickYPos)]),
            -1.0f, 1.0f);
        rx = std::clamp(
            slotValue(cfg.binds[static_cast<size_t>(PadAction::RStickXNeg)]) +
                slotValue(cfg.binds[static_cast<size_t>(PadAction::RStickXPos)]),
            -1.0f, 1.0f);
        ry = std::clamp(
            slotValue(cfg.binds[static_cast<size_t>(PadAction::RStickYNeg)]) +
                slotValue(cfg.binds[static_cast<size_t>(PadAction::RStickYPos)]),
            -1.0f, 1.0f);
#if defined(__linux__)
        // Native evdev sticks: the singleton is one physical device = the launcher's
        // "Gamepad 0". When raylib has a GLFW mapping for it the digital side works
        // but a half-mapped SDL entry can zero the analogue axes (buttons/bumpers and
        // even triggers still arrive via their own paths), leaving the sticks dead:
        // slotValue() only reads raylib slots and the old fallback required pads to
        // be EMPTY. Blend the native deflection into each stick when it is stronger,
        // but only for the native owner (per-player leak guard, same rule as buttons).
        if (nativeOwner)
        {
            auto mergeStick = [&](float &dst, int axis, const PadBind &neg, const PadBind &pos)
            {
                const float nv = native.getAxis(axis);
                float v = 0.0f;
                if (neg.kind == PadBindKind::Axis && neg.sign < 0.0f && nv < 0.0f)
                    v = nv;
                else if (pos.kind == PadBindKind::Axis && pos.sign > 0.0f && nv > 0.0f)
                    v = nv;
                if (std::fabs(v) > std::fabs(dst))
                {
                    dst = v;
                }
            };
            mergeStick(lx, GAMEPAD_AXIS_LEFT_X,
                       cfg.binds[static_cast<size_t>(PadAction::LStickXNeg)],
                       cfg.binds[static_cast<size_t>(PadAction::LStickXPos)]);
            mergeStick(ly, GAMEPAD_AXIS_LEFT_Y,
                       cfg.binds[static_cast<size_t>(PadAction::LStickYNeg)],
                       cfg.binds[static_cast<size_t>(PadAction::LStickYPos)]);
            mergeStick(rx, GAMEPAD_AXIS_RIGHT_X,
                       cfg.binds[static_cast<size_t>(PadAction::RStickXNeg)],
                       cfg.binds[static_cast<size_t>(PadAction::RStickXPos)]);
            mergeStick(ry, GAMEPAD_AXIS_RIGHT_Y,
                       cfg.binds[static_cast<size_t>(PadAction::RStickYNeg)],
                       cfg.binds[static_cast<size_t>(PadAction::RStickYPos)]);
        }
        // [padprobe] PS2X_PADPROBE=1: once (60th frame) dump what each input world
        // sees for the pad, so a dead path is distinguishable from hardware state.
        static const bool s_probe = [](){ const char *v = std::getenv("PS2X_PADPROBE"); return v && v[0] == '1'; }();
        if (s_probe)
        {
            static int s_probeFrame = 0;
            if (++s_probeFrame == 60)
            {
                for (int pad : pads)
                {
                    std::fprintf(stdout, "[padprobe] slot=%d nativeAvail=%d nativeMatch=%d\n",
                                 pad, native.isAvailable() ? 1 : 0, nativeOk ? 1 : 0);
                    std::fprintf(stdout, "[padprobe] raylib axes 0..5: %.3f %.3f %.3f %.3f %.3f %.3f\n",
                                 GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_X),
                                 GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_Y),
                                 GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_X),
                                 GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_Y),
                                 GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_TRIGGER),
                                 GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_TRIGGER));
                    std::fprintf(stdout, "[padprobe] native  axes 0..5: %.3f %.3f %.3f %.3f %.3f %.3f\n",
                                 native.getAxis(GAMEPAD_AXIS_LEFT_X), native.getAxis(GAMEPAD_AXIS_LEFT_Y),
                                 native.getAxis(GAMEPAD_AXIS_RIGHT_X), native.getAxis(GAMEPAD_AXIS_RIGHT_Y),
                                 native.getAxis(GAMEPAD_AXIS_LEFT_TRIGGER), native.getAxis(GAMEPAD_AXIS_RIGHT_TRIGGER));
                    std::fprintf(stdout, "[padprobe] buttons raylib(9,10,16,17)=%d%d%d%d native(16,17)=%d%d\n",
                                 IsGamepadButtonDown(pad, 9), IsGamepadButtonDown(pad, 10),
                                 IsGamepadButtonDown(pad, 16), IsGamepadButtonDown(pad, 17),
                                 native.isButtonDown(16), native.isButtonDown(17));
                }
            }
        }
#endif

        // Fallback: if GLFW has no mapping for the controller (pads empty)
        // but the native evdev reader is available, read axes directly. Only
        // for a player actually assigned a gamepad: elsewhere (e.g. a Keyboard
        // player) the singleton native device would feed this player the same
        // physical sticks another player is using (shared-axes leak).
        if (lx == 0.0f && ly == 0.0f && rx == 0.0f && ry == 0.0f)
        {
#if defined(__linux__)
            // GLFW has no mapping for this gamepad (pads empty) but the native
            // reader knows the device. Same ownership rule as above: the
            // singleton is "Gamepad 0" of the launcher, so only that player
            // inherits its axes. A Keyboard player or any other gamepad player
            // must not read this physical device (shared-axes leak).
            if (cfg.device.kind == PadDeviceKind::Gamepad && cfg.device.gamepad == 0 &&
                pads.empty() && native.isAvailable())
            {
                lx = native.getAxis(GAMEPAD_AXIS_LEFT_X);
                ly = native.getAxis(GAMEPAD_AXIS_LEFT_Y);
                rx = native.getAxis(GAMEPAD_AXIS_RIGHT_X);
                ry = native.getAxis(GAMEPAD_AXIS_RIGHT_Y);
            }
#endif
        }

        if (lx != 0.0f || ly != 0.0f)
        {
            pkt.lx = floatToByte(lx);
            pkt.ly = floatToByte(ly);
        }
        if (rx != 0.0f || ry != 0.0f)
        {
            pkt.rx = floatToByte(rx);
            pkt.ry = floatToByte(ry);
        }
        return pkt;
    }
}

bool ps2_stubs::padSlotIsController(int g)
{
    return slotLooksLikeGamepad(g);
}
