#pragma once

#include "ps2_runtime.h"
#include "runtime/pad_config.h"
#include <string>
#include <vector>
#include <array>
#include <cstdint>

struct ImFont;

class PS2SettingsOverlay
{
public:
    struct Settings
    {
        float masterVolume = 1.0f;
        float musicVolume = 1.0f;
        float sfxVolume = 1.0f;
        bool gpuRenderer = false;
        bool glow = true;
        bool postfx = false;
        bool glowFix = true;   // [glowfix] BT3's bloom/glow chain (Kaioken aura); applies on restart
        bool bilinear = true;
        bool halfTexel = true;
        bool skipPost = true;
        bool skipStaleVram = true;
        int renderScale = 1; // internal resolution multiplier: 1/2/3/4 (see GsGpuRenderer::renderScale)
        float deadzone = 0.15f;
        bool fullscreen = false;  // [defaults-sync] do not force fullscreen on first launch (rig + user surprise); one toggle away in the overlay
        bool widescreen = false;
        bool outline = true;
        bool texPack = true;   // [texreplace] use the PCSX2 replacement pack when one is indexed
        int inkStrength = 199;   // [inkstrength] cel-outline darkener, % of Cs (199 = hardware 255/128)
        bool shadows = true;
        bool dofBlur = true;
        int dofZFar = 200000;
        int windowW = 0, windowH = 0; // 0 = keep the default host window size
        bool forceBilinear = true;    // PCSX2-style forced texture filtering (default ON)
        int hudLayout = 0;            // widescreen HUD: 0=centered 4:3, 1=edge-pinned, 2=custom
        int hudOffL = 0, hudOffC = 0, hudOffR = 0;  // custom layout x-offsets (512-space px)
        // Overlay launch / close binding. Each is a SET of inputs that must ALL be held
        // together (1+ entries). Captured with a 3s hold during binding.
        std::vector<int> overlayPadBtns = {13, 15};  // default: Select (Back) + Start
        std::vector<int> overlayKeys   = {340, 258}; // default: Left Shift + Tab
    };

    static bool isWidescreen() { return s_widescreen; }

    static void preloadSettings();
    void syncFromRuntime();   // seed m_settings from live runtime state
    // Set the directory that holds bt3_settings.ini (portable dist: <root>/savedata).
    static void setConfigDirectory(const std::string &dir);

    struct DeviceInfo
    {
        std::string name;          // "Microsoft Xbox Controller (event257)"
        int glfwSlot = -1;         // GLFW slot index, -1 if not GLFW
        bool isEvdev = false;      // true if evdev native matches this device
        ps2_stubs::PadDeviceKind kind = ps2_stubs::PadDeviceKind::None;
    };

    void initialize();
    void draw(PS2Runtime &runtime);
    void shutdown();

private:
    static bool s_widescreen;
    static std::string s_configDir;

    bool m_visible = false;
    bool m_initialized = false;
    bool m_dirty = false;
    Settings m_settings;
    std::string m_configPath;
    int m_activeTab = 0;

    // Open/close deploy animation. m_animT is an eased 0..1 value: 0 = fully
    // closed/hidden, 1 = fully open. It chases m_visible every frame, so the
    // panel keeps rendering (fading + sliding out) for a few frames AFTER
    // m_visible flips to false, instead of popping away instantly.
    float m_animT = 0.0f;

    // Capsule HUD display font (Russo One), loaded once in initialize().
    // Null (falls back to the default ImGui font) if the .ttf couldn't be found.
    ImFont *m_fontHudTitle = nullptr; // title bar
    ImFont *m_fontHudLabel = nullptr; // tab labels + section headers

    // Settings dump logging — which areas are captured to the dump log.
    bool m_dumpAudio = true;
    bool m_dumpVideo = true;
    bool m_dumpControllers = true;
    bool m_dumpRuntime = true;
    bool m_dumpGamepad = false;   // live test state, only meaningful while capture is read

    // Device selection
    std::vector<DeviceInfo> m_deviceList;
    int m_selectedDevice = 0;

    // Binding capture state
    int m_captureAction = -1;
    bool m_captureWaitRelease = false;
    int m_editPlayer = 0;

    // Edge detection state (always updated, even when not capturing)
    std::array<uint8_t, 32> m_prevBtnDown{};
    std::array<float, 6> m_prevAxis{};

    // Overlay-toggle combo capture state (3s hold to bind one or more inputs).
    int m_captureComboSlot = -1;          // 0 = gamepad, 1 = keyboard, -1 = none
    float m_captureComboTimer = 0.0f;     // seconds remaining in the capture window
    bool m_captureComboHadAny = false;    // whether something was captured yet
    std::vector<int> m_capturedBtns;      // buttons held during capture
    std::vector<int> m_capturedKeys;      // keys held during capture
    std::array<uint8_t, 32> m_prevComboBtn{};

    // Gamepad toggle combo edge state
    bool m_prevToggleCombo = false;

    // Whether the Bindings / Overlay-settings sub-window (popup) is open.
    bool m_showBindingsPopup = false;

    void toggleVisible();
    void resetCaptureState();
    void loadSettings();
    void saveSettings() const;
    void applySettings();
    void applyDeadzone();
    void buildDeviceList();
    void readGamepadStateForDevice(const DeviceInfo &dev,
                                   std::array<uint8_t, 32> &btnDown,
                                   std::array<float, 6> &axis);
    void drawAudioTab();
    void drawVideoTab();
    void drawControllersTab();
    void drawLoggingTab();
    void drawGamepadTestArea(const std::array<uint8_t, 32> &btnDown,
                             const std::array<float, 6> &axis);
    void drawBindingsTable();
    void drawOverlayToggle();
    void drawBindingsPopup();
    void dumpSettingsToFile();
};
