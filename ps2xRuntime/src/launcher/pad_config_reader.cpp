#include "pad_config_reader.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef ERROR
#undef interface
#endif

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <sstream>

namespace padconf
{
    static const char *kActionNames[24] = {
        "Select", "L3", "R3", "Start",
        "D-Pad Up", "D-Pad Right", "D-Pad Down", "D-Pad Left",
        "L2", "R2", "L1", "R1",
        "Triangle", "Circle", "Cross", "Square",
        "L Stick X -", "L Stick X +", "L Stick Y -", "L Stick Y +",
        "R Stick X -", "R Stick X +", "R Stick Y -", "R Stick Y +",
    };

    int actionFromName(const std::string &name)
    {
        for (int i = 0; i < 24; ++i)
            if (name == kActionNames[i])
                return i;
        return -1;
    }

    const char *actionName(int action) { return kActionNames[action]; }

    const char *kindName(BindKind kind)
    {
        switch (kind)
        {
        case BindKind::Key: return "Key";
        case BindKind::Button: return "Button";
        case BindKind::Axis: return "Axis";
        default: return "None";
        }
    }

    static std::string keyDisplay(int key)
    {
        // Common raylib KEY_* codes the game uses; fallback is KEY_<n>.
        static const struct { int k; const char *n; } map[] = {
            {265, "Up"}, {264, "Down"}, {263, "Left"}, {262, "Right"},
            {65, "A"}, {66, "B"}, {67, "C"}, {68, "D"}, {69, "E"},
            {81, "Q"}, {83, "S"}, {86, "V"}, {87, "W"}, {88, "X"}, {90, "Z"},
            {49, "1"}, {50, "2"}, {51, "3"}, {52, "4"}, {53, "5"}, {54, "6"},
            {55, "7"}, {56, "8"}, {57, "9"}, {48, "0"},
            {257, "Enter"}, {32, "Space"}, {258, "Tab"}, {256, "Esc"},
            {340, "LShift"}, {344, "RShift"}, {341, "LCtrl"}, {345, "RCtrl"},
            {342, "LAlt"}, {346, "RAlt"}, {343, "LMeta"}, {347, "RMeta"},
        };
        for (auto &m : map)
            if (m.k == key)
                return m.n;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "KEY_%d", key);
        return buf;
    }

    static std::string buttonDisplay(int btn)
    {
        // Values are raylib GAMEPAD_BUTTON_* codes (same as the runtime):
        // DPad=1..4, face=5..8, shoulders=9..12, middle=13..15, thumbs=16..17.
        static const struct { int b; const char *n; } map[] = {
            {1, "DPad Up"}, {2, "DPad Right"}, {3, "DPad Down"}, {4, "DPad Left"},
            {5, "Y"}, {6, "B"}, {7, "A"}, {8, "X"},
            {9, "LB"}, {10, "LT"}, {11, "RB"}, {12, "RT"},
            {13, "Select"}, {14, "Guide"}, {15, "Start"},
            {16, "L3"}, {17, "R3"},
        };
        for (auto &m : map)
            if (m.b == btn)
                return m.n;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "B%d", btn);
        return buf;
    }

    static std::string axisDisplay(int ax)
    {
        static const struct { int a; const char *n; } map[] = {
            {0, "LStick X"}, {1, "LStick Y"}, {2, "RStick X"}, {3, "RStick Y"},
            {4, "LT"}, {5, "RT"},
        };
        for (auto &m : map)
            if (m.a == ax)
                return m.n;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "Axis%d", ax);
        return buf;
    }

    std::string bindDisplay(const Bind &b)
    {
        switch (b.kind)
        {
        case BindKind::Key: return "Key " + keyDisplay(b.value);
        case BindKind::Button: return "Button " + buttonDisplay(b.value);
        case BindKind::Axis:
            return "Axis " + axisDisplay(b.value) + " " + (b.sign < 0.0f ? "-" : "+");
        default: return "None";
        }
    }

    void applyDefaultGamepadBinds(Player &p)
    {
        auto btn = [&p](int action, int button)
        {
            p.binds[action] = Bind{BindKind::Button, button, 1.0f, 0.15f};
        };
        auto axis = [&p](int action, int ax, float sign)
        {
            p.binds[action] = Bind{BindKind::Axis, ax, sign, 0.15f};
        };
        // Action order == PadAction: Select, L3, R3, Start, Up, Right, Down, Left,
        // L2, R2, L1, R1, Triangle, Circle, Cross, Square, then LStick/RStick.
        // Values are raylib GAMEPAD_BUTTON_*/GAMEPAD_AXIS_* codes.
        btn(4, 1);  // Up -> LEFT_FACE_UP
        btn(5, 2);  // Right -> LEFT_FACE_RIGHT
        btn(6, 3);  // Down -> LEFT_FACE_DOWN
        btn(7, 4);  // Left -> LEFT_FACE_LEFT
        btn(15, 8); // Square -> RIGHT_FACE_LEFT
        btn(14, 7); // Cross -> RIGHT_FACE_DOWN
        btn(13, 6); // Circle -> RIGHT_FACE_RIGHT
        btn(12, 5); // Triangle -> RIGHT_FACE_UP
        btn(10, 9); // L1 -> LEFT_TRIGGER_1
        btn(11, 11);// R1 -> RIGHT_TRIGGER_1
        btn(8, 10); // L2 -> LEFT_TRIGGER_2
        btn(9, 12); // R2 -> RIGHT_TRIGGER_2
        btn(1, 16); // L3 -> LEFT_THUMB
        btn(2, 17); // R3 -> RIGHT_THUMB
        btn(0, 13); // Select -> MIDDLE_LEFT
        btn(3, 15); // Start -> MIDDLE_RIGHT
        axis(16, 0, -1.0f); axis(17, 0, 1.0f);  // LStickX
        axis(18, 1, -1.0f); axis(19, 1, 1.0f);  // LStickY
        axis(20, 2, -1.0f); axis(21, 2, 1.0f);  // RStickX
        axis(22, 3, -1.0f); axis(23, 3, 1.0f);  // RStickY
    }

    void applyDefaultKeyboardBinds(Player &p)
    {
        auto key = [&p](int action, int k, float sign = 1.0f)
        {
            p.binds[action] = Bind{BindKind::Key, k, sign, 0.0f};
        };
        // Mirrors runtime setDefaultKeyboardBinds (raylib KEY_* codes).
        key(4, 265);  // Up
        key(5, 262);  // Right
        key(6, 264);  // Down
        key(7, 263);  // Left
        key(15, 90);  // Square -> Z
        key(14, 88);  // Cross -> X
        key(13, 67);  // Circle -> C
        key(12, 86);  // Triangle -> V
        key(10, 81);  // L1 -> Q
        key(11, 69);  // R1 -> E
        key(8, 49);   // L2 -> 1
        key(9, 51);   // R2 -> 3
        key(1, 341);  // L3 -> LCtrl
        key(2, 345);  // R3 -> RCtrl
        key(0, 344);  // Select -> RShift
        key(3, 257);  // Start -> Enter
        key(16, 65, -1.0f); key(17, 68, 1.0f);  // LStickX: A / D
        key(18, 87, -1.0f); key(19, 83, 1.0f);  // LStickY: W / S
    }

    bool load(const std::string &p1Path, const std::string &p2Path, std::array<Player, 2> &out)
    {
        const std::string paths[2] = {p1Path, p2Path};
        bool any = false;
        bool foundNew = false;
        for (size_t p = 0; p < 2; ++p)
        {
            auto &dst = out[p];
            dst = Player{};
            padconf::applyDefaultGamepadBinds(dst);
            std::ifstream in(paths[p]);
            if (!in.is_open())
                continue;
            foundNew = true;

            std::string line;
            while (std::getline(in, line))
            {
                std::istringstream ss(line);
                std::string tok;
                if (!(ss >> tok) || tok != "player")
                    continue;
                int idx = -1;
                if (!(ss >> idx) || idx < 0 || idx > 1)
                    continue;
                if (!(ss >> tok))
                    continue;

                // Each file holds exactly one player; ignore lines for others.
                if (idx != static_cast<int>(p))
                    continue;

                if (tok == "device")
                {
                    std::string kind;
                    if (!(ss >> kind))
                        continue;
                    if (kind == "Keyboard")
                    {
                        dst.device = {DevKind::Keyboard, -1};
                        applyDefaultKeyboardBinds(dst);
                    }
                    else if (kind == "Gamepad")
                    {
                        int g = -1;
                        if (ss >> g)
                            dst.device = {DevKind::Gamepad, g};
                    }
                    else
                    {
                        dst.device = {DevKind::None, -1};
                    }
                    any = true;
                }
                else if (tok == "bind")
                {
                    std::string actionName, kindN;
                    if (!(ss >> actionName >> kindN))
                        continue;
                    int value = -1;
                    if (!(ss >> value))
                        continue;
                    float sign = 1.0f;
                    ss >> sign;

                    const int action = actionFromName(actionName);
                    BindKind kind = BindKind::None;
                    if (kindN == "Key") kind = BindKind::Key;
                    else if (kindN == "Button") kind = BindKind::Button;
                    else if (kindN == "Axis") kind = BindKind::Axis;
                    if (action < 0 || kind == BindKind::None)
                        continue;

                    Bind b;
                    b.kind = kind;
                    b.value = value;
                    b.sign = (std::fabs(sign) < 0.5f) ? -1.0f : 1.0f;
                    dst.binds[action] = b;
                    any = true;
                }
            }
        }
        return foundNew || any;
    }

    bool save(const std::string &p1Path, const std::string &p2Path, const std::array<Player, 2> &players)
    {
        const std::string paths[2] = {p1Path, p2Path};
        bool ok = true;
        for (size_t p = 0; p < 2; ++p)
        {
            const std::string &path = paths[p];
            const std::string tmp = path + ".tmp";
            {
                std::ofstream out(tmp, std::ios::trunc);
                if (!out.is_open())
                {
                    ok = false;
                    continue;
                }
                const Player &cfg = players[p];
                out << "# BT3-Recomp pad configuration - Player " << (p + 1) << "\n";
                out << "# player <N> device <None|Keyboard|Gamepad [index]>\n";
                out << "# player <N> bind <Action> <Key|Button|Axis> <value> [sign]\n";
                out << "player " << p << " device";
                switch (cfg.device.kind)
                {
                case DevKind::Keyboard: out << " Keyboard\n"; break;
                case DevKind::Gamepad: out << " Gamepad " << cfg.device.gamepad << "\n"; break;
                default: out << " None\n"; break;
                }
                for (int a = 0; a < 24; ++a)
                {
                    const Bind &b = cfg.binds[a];
                    if (b.kind == BindKind::None)
                        continue;
                    out << "player " << p << " bind " << actionName(a) << " "
                        << kindName(b.kind) << " " << b.value << " "
                        << (b.sign < 0.0f ? -1 : 1) << "\n";
                }
            }
            if (std::rename(tmp.c_str(), path.c_str()) != 0)
                ok = false;
#ifdef _WIN32
            // Windows std::rename refuses to replace an existing file. Retry
            // with the Win32 atomic-replace API (same semantics as POSIX).
            if (!ok && ::MoveFileExA(tmp.c_str(), path.c_str(),
                                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
            {
                ok = true;
            }
#endif
        }
        return ok;
    }
} // namespace padconf