#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Cross-platform input for the launcher's gamepad test + bind capture.
//
// Joysticks are read through GLFW (glfwJoystickIsGamepad / glfwGetJoystick*),
// the same backend and mapping database the runtime uses via raylib, and the
// mapped button/axis layout matches raylib's GAMEPAD_BUTTON_*/AXIS_* codes, so
// a bind captured here is meaningful in-game.
//
// The keyboard comes from Qt key events: the tab that owns the capture installs
// a window event filter and pushes each pressed key into Reader::captureKey()
// as an already-translated raylib KEY_* code. There is no /dev/input, evdev or
// linux/input.h anywhere; the same code builds on Linux, Windows and macOS.
namespace evin
{
    // raylib-style button indices for the standard controller layout.
    enum : int
    {
        BtnY     = 0, // Y/Triangle
        BtnB     = 1, // B/Circle
        BtnA     = 2, // A/Cross
        BtnX     = 3, // X/Square
        BtnLT    = 4,
        BtnRT    = 5,
        BtnLB    = 6,
        BtnRB    = 7,
        BtnSelect= 8,
        BtnStart = 9,
        BtnLS    = 10,
        BtnRS    = 11,
        BtnDpadUp= 12,
        BtnDpadDown = 13,
        BtnDpadLeft = 14,
        BtnDpadRight = 15,
    };

    enum : int
    {
        AxisLX = 0,
        AxisLY = 1,
        AxisRX = 2,
        AxisRY = 3,
        AxisLT = 4,
        AxisRT = 5,
    };

    // GLFW exposes its joystick slots as raw ids; the keyboard is a Qt-managed
    // pseudo-device (no physical node). Both are addressed by this stable id.
    inline const char *const kKeyboardId = "keyboard";
    inline const char *const kGlfwPrefix = "glfw:";

    struct DeviceInfo
    {
        std::string node;    // glfw:<id> or "keyboard"
        std::string name;    // GLFW joystick name
        int axes = 0;
        int buttons = 0;
        bool isGamepad = false;  // has a GLFW gamepad mapping (or looks like one)
        bool isKeyboard = false; // Qt keyboard pseudo-device
        bool isMouse = false;    // never (GLFW joysticks only)
    };

    // GLFW joysticks; only device entries a capture could use are returned.
    std::vector<DeviceInfo> listDevices();
    // The Qt keyboard pseudo-device is always available: return kKeyboardId.
    std::string pickKeyboardNode(const std::vector<DeviceInfo> &devices);

    class Reader
    {
    public:
        bool open(const std::string &node);
        void close() { m_mode = Mode::Closed; }
        bool isOpen() const { return m_mode != Mode::Closed; }
        bool update(); // glfwPollEvents + refresh joystick state

        bool buttonDown(int rb) const { return rb >= 0 && rb < 32 && (m_btns[rb] != 0); }
        float axis(int ra) const { return ra >= 0 && ra < 8 ? m_axes[ra] : 0.0f; }
        // Most recent key pushed via captureKey() (raylib KEY_* code), cleared
        // by takeLastKey(). -1 when none.
        int takeLastKey();
        void clearLastKey() { m_lastKey = -1; }
        // Qt key event pump: set the last-key from a raylib KEY_* code.
        void captureKey(int raylibKey) { m_lastKey = raylibKey; }
        // True when the reader is in keyboard mode (Qt key capture).
        bool isKeyboardDevice() const { return m_mode == Mode::Keyboard; }

    private:
        enum class Mode { Closed, Gamepad, Keyboard };
        Mode m_mode = Mode::Closed;
        int m_jid = -1;              // GLFW joystick id (-1 when not gamepad)
        std::array<float, 8> m_axes{};
        std::array<uint8_t, 32> m_btns{};
        int m_lastKey = -1;
        void refreshGamepad();
    };
} // namespace evin