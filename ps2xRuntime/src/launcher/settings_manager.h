#pragma once

#include <QString>
#include <QStringList>
#include <QHash>
#include <string>

// Portable read/write of bt3_settings.ini — the exact same format the in-game
// overlay persists (ps2_settings_overlay.cpp), so the launcher and the
// runtime share one config. Unknown keys are preserved on save.
class SettingsManager
{
public:
    static SettingsManager &instance();

    // Directory that holds the INI (the deploy root's savedata/). Set once at
    // startup; load()/save() use it.
    void setConfigDir(const QString &dir);
    QString configDir() const { return m_dir; }
    QString inipath() const;

    bool load();   // read savedata/bt3_settings.ini (missing file = defaults)
    bool save();   // atomic write (tmp + rename)

    // --- [audio] ---
    float masterVolume() const { return m_master; }
    float musicVolume() const { return m_music; }
    float sfxVolume() const { return m_sfx; }
    void setMasterVolume(float v) { m_master = v; }
    void setMusicVolume(float v) { m_music = v; }
    void setSfxVolume(float v) { m_sfx = v; }

    // --- [video] ---
    // [renderer] 0 = OpenGL, 1 = software rasterizer, 2 = paraLLEl-GS (Vulkan).
    // Mirrors PS2SettingsOverlay::Settings; the legacy bool gpuRenderer stays in
    // sync (true when renderer != 1) for the old readers.
    static constexpr int kRendererOpenGL = 0, kRendererSoftware = 1, kRendererParallelGS = 2;
    int renderer() const { return m_renderer; }
    bool gpuRenderer() const { return m_renderer != kRendererSoftware; }
    bool glow() const { return m_glow; }
    bool glowFix() const { return m_glowFix; }
    bool postfx() const { return m_postfx; }
    bool bilinear() const { return m_bilinear; }
    bool halfTexel() const { return m_halfTexel; }
    bool skipPost() const { return m_skipPost; }
    bool skipStaleVram() const { return m_skipStaleVram; }
    int renderScale() const { return m_renderScale; }
    bool outline() const { return m_outline; }
    int inkStrength() const { return m_inkStrength; }
    bool shadows() const { return m_shadows; }
    bool dofBlur() const { return m_dofBlur; }
    int dofZFar() const { return m_dofZFar; }
    bool fullscreen() const { return m_fullscreen; }
    bool widescreen() const { return m_widescreen; }
    int windowW() const { return m_windowW; }
    int windowH() const { return m_windowH; }
    bool forceBilinear() const { return m_forceBilinear; }
    int hudLayout() const { return m_hudLayout; }
    int hudOffL() const { return m_hudOffL; }
    int hudOffC() const { return m_hudOffC; }
    int hudOffR() const { return m_hudOffR; }

    void setRenderer(int v) { m_renderer = v; }
    void setGpuRenderer(bool v) { m_renderer = (v != false) ? kRendererParallelGS : kRendererSoftware; }
    void setGlow(bool v) { m_glow = v; }
    void setGlowFix(bool v) { m_glowFix = v; }
    void setPostfx(bool v) { m_postfx = v; }
    void setBilinear(bool v) { m_bilinear = v; }
    void setHalfTexel(bool v) { m_halfTexel = v; }
    void setSkipPost(bool v) { m_skipPost = v; }
    void setSkipStaleVram(bool v) { m_skipStaleVram = v; }
    void setRenderScale(int v) { m_renderScale = v; }
    void setOutline(bool v) { m_outline = v; }
    void setInkStrength(int v) { m_inkStrength = v; }
    void setShadows(bool v) { m_shadows = v; }
    void setDofBlur(bool v) { m_dofBlur = v; }
    void setDofZFar(int v) { m_dofZFar = v; }
    void setFullscreen(bool v) { m_fullscreen = v; }
    void setWidescreen(bool v) { m_widescreen = v; }
    void setWindowSize(int w, int h) { m_windowW = w; m_windowH = h; }
    void setForceBilinear(bool v) { m_forceBilinear = v; }
    void setHudLayout(int v) { m_hudLayout = v; }
    void setHudOffsets(int l, int c, int r) { m_hudOffL = l; m_hudOffC = c; m_hudOffR = r; }

    // --- [controllers] ---
    int selectedDevice() const { return m_device; }
    float deadzone() const { return m_deadzone; }
    bool overlayEnabled() const { return m_overlayEnabled; }
    std::string overlayPadBtns() const { return m_overlayPadBtns; }
    std::string overlayKeys() const { return m_overlayKeys; }
    void setSelectedDevice(int v) { m_device = v; }
    void setDeadzone(float v) { m_deadzone = v; }
    void setOverlayEnabled(bool v) { m_overlayEnabled = v; }
    void setOverlayPadBtns(const std::string &v) { m_overlayPadBtns = v; }
    void setOverlayKeys(const std::string &v) { m_overlayKeys = v; }

    // --- [logging] ---
    int logLevel() const { return m_logLevel; }
    bool dumpAudio() const { return m_dumpAudio; }
    bool dumpVideo() const { return m_dumpVideo; }
    bool dumpControllers() const { return m_dumpControllers; }
    bool dumpRuntime() const { return m_dumpRuntime; }
    bool dumpGamepad() const { return m_dumpGamepad; }
    void setLogLevel(int v) { m_logLevel = v; }

public:
    // Per-section value map for round-tripping unknown/extra keys unchanged.
    struct Section { QStringList order; QHash<QString, QString> kv; };
    QHash<QString, Section> sections;

private:
    SettingsManager() = default;
    bool m_sawRenderer = false;   // [renderer] set when the ini had an explicit `renderer` key
    QString m_dir;
    float m_master = 1.0f, m_music = 1.0f, m_sfx = 1.0f;
    int m_renderer = kRendererParallelGS;
    bool m_glow = true, m_glowFix = true, m_postfx = false;
    bool m_bilinear = true, m_halfTexel = true, m_skipPost = true, m_skipStaleVram = true;
    int m_renderScale = 1;
    bool m_outline = true, m_shadows = true, m_dofBlur = true;
    int m_inkStrength = 199, m_dofZFar = 200000;
    bool m_fullscreen = false, m_widescreen = false, m_forceBilinear = true;
    int m_windowW = 0, m_windowH = 0;
    int m_hudLayout = 0, m_hudOffL = 0, m_hudOffC = 0, m_hudOffR = 0;
    int m_device = 0;
    float m_deadzone = 0.15f;
    bool m_overlayEnabled = true;
    std::string m_overlayPadBtns = "13,15";
    std::string m_overlayKeys = "340,258";
    int m_logLevel = 1;
    bool m_dumpAudio = true, m_dumpVideo = true, m_dumpControllers = true;
    bool m_dumpRuntime = true, m_dumpGamepad = false;
};