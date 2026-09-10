#include "input_reader.h"

#include <GLFW/glfw3.h>

#include <cstdlib>

namespace evin
{
    namespace
    {
        // GLFW reports joystick state through its mapping DB (SDL-style). Map the
        // GLFW gamepad button indices into the launcher's logical evin layout
        // (the same order tab_bindings translates to raylib codes with
        // evinToGamepadButton). -1 = unmapped (Guide button, ignored).
        const int kGlfwToEvinButton[17] = {
            2,   // GLFW_GAMEPAD_BUTTON_A      -> BtnA
            1,   // GLFW_GAMEPAD_BUTTON_B      -> BtnB
            3,   // GLFW_GAMEPAD_BUTTON_X      -> BtnX
            0,   // GLFW_GAMEPAD_BUTTON_Y      -> BtnY
            6,   // GLFW_GAMEPAD_BUTTON_LEFT_BUMPER  -> BtnLB
            7,   // GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER -> BtnRB
            8,   // GLFW_GAMEPAD_BUTTON_BACK         -> BtnSelect
            9,   // GLFW_GAMEPAD_BUTTON_START        -> BtnStart
            -1,  // GLFW_GAMEPAD_BUTTON_GUIDE
            10,  // GLFW_GAMEPAD_BUTTON_LEFT_THUMB   -> BtnLS
            11,  // GLFW_GAMEPAD_BUTTON_RIGHT_THUMB  -> BtnRS
            12,  // GLFW_GAMEPAD_BUTTON_DPAD_UP      -> BtnDpadUp
            15,  // GLFW_GAMEPAD_BUTTON_DPAD_RIGHT   -> BtnDpadRight
            13,  // GLFW_GAMEPAD_BUTTON_DPAD_DOWN    -> BtnDpadDown
            14,  // GLFW_GAMEPAD_BUTTON_DPAD_LEFT    -> BtnDpadLeft
            4,   // GLFW_GAMEPAD_BUTTON_LEFT_TRIGGER -> BtnLT
            5,   // GLFW_GAMEPAD_BUTTON_RIGHT_TRIGGER -> BtnRT
        };

        // GLFW_GAMEPAD_AXIS_* already use the evin AxisLX..AxisRT ordering.
        bool once() { return glfwInit() == GLFW_TRUE; }
    } // namespace

    std::vector<DeviceInfo> listDevices()
    {
        std::vector<DeviceInfo> out;
        if (!once())
            return out;
        for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid)
        {
            if (glfwJoystickPresent(jid) != GLFW_TRUE)
                continue;
            DeviceInfo d;
            d.node = std::string(kGlfwPrefix) + std::to_string(jid);
            const char *name = glfwGetJoystickName(jid);
            d.name = name ? name : "";
            int nAxes = 0, nBtns = 0;
            glfwGetJoystickAxes(jid, &nAxes);
            glfwGetJoystickButtons(jid, &nBtns);
            d.axes = nAxes;
            d.buttons = nBtns;
            d.isGamepad = glfwJoystickIsGamepad(jid) == GLFW_TRUE ||
                          (nAxes >= 2 && nBtns >= 8);
            out.push_back(d);
        }
        return out;
    }

    std::string pickKeyboardNode(const std::vector<DeviceInfo> &)
    {
        // The keyboard is a Qt-injected pseudo-device, always available.
        return kKeyboardId;
    }

    bool Reader::open(const std::string &node)
    {
        if (node == kKeyboardId)
        {
            m_mode = Mode::Keyboard;
            m_lastKey = -1;
            return true;
        }
        if (node.rfind(kGlfwPrefix, 0) != 0)
            return false;
        if (!once())
            return false;
        const int jid = std::atoi(node.c_str() + std::string(kGlfwPrefix).size());
        if (jid < GLFW_JOYSTICK_1 || jid > GLFW_JOYSTICK_LAST ||
            glfwJoystickPresent(jid) != GLFW_TRUE)
            return false;
        m_jid = jid;
        m_mode = Mode::Gamepad;
        refreshGamepad();
        return true;
    }

    bool Reader::update()
    {
        if (m_mode == Mode::Keyboard)
            return false;
        if (m_mode != Mode::Gamepad || m_jid < 0)
            return false;
        if (!once())
            return false;
        glfwPollEvents();
        refreshGamepad();
        return false;
    }

    void Reader::refreshGamepad()
    {
        std::fill(m_axes.begin(), m_axes.end(), 0.0f);
        std::fill(m_btns.begin(), m_btns.end(), 0);

        GLFWgamepadstate st;
        if (glfwJoystickIsGamepad(m_jid) == GLFW_TRUE &&
            glfwGetGamepadState(m_jid, &st) == GLFW_TRUE)
        {
            for (int gb = 0; gb <= GLFW_GAMEPAD_BUTTON_LAST; ++gb)
            {
                const int evinIdx = kGlfwToEvinButton[gb];
                if (evinIdx >= 0 && st.buttons[gb] == GLFW_PRESS)
                    m_btns[evinIdx] = 1;
            }
            for (int ga = 0; ga <= GLFW_GAMEPAD_AXIS_LAST && ga < (int)m_axes.size(); ++ga)
                m_axes[ga] = st.axes[ga];
            return;
        }

        // Unmapped joystick: expose raw buttons/axes best-effort.
        int n = 0;
        const unsigned char *rawBtns = glfwGetJoystickButtons(m_jid, &n);
        for (int b = 0; b < n && b < (int)m_btns.size(); ++b)
            m_btns[b] = rawBtns[b] == GLFW_PRESS ? 1 : 0;
        int na = 0;
        const float *rawAxes = glfwGetJoystickAxes(m_jid, &na);
        for (int a = 0; a < na && a < (int)m_axes.size(); ++a)
            m_axes[a] = rawAxes[a];
    }

    int Reader::takeLastKey()
    {
        if (m_mode != Mode::Keyboard)
            return -1;
        const int k = m_lastKey;
        m_lastKey = -1;
        return k;
    }
} // namespace evin