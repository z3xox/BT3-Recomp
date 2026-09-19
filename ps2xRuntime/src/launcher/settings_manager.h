#pragma once

#include <QString>
#include <QStringList>
#include <QHash>
#include <string>

// Portable read/write of savedata/settings.toml — the exact same file/format the
// in-game overlay (ps2_settings_overlay.cpp) uses, so the launcher and the
// runtime share one config. Keys the launcher does not model are preserved.
class SettingsManager
{
public:
    static SettingsManager &instance();

    // Directory that holds the TOML (the deploy root's savedata/). Set once at
    // startup; load()/save() use it.
    void setConfigDir(const QString &dir);
    QString configDir() const { return m_dir; }
    QString configpath() const;

    bool load();   // read savedata/settings.toml (missing file = defaults)
    bool save();   // write savedata/settings.toml

    // --- [audio] ---
    float masterVolume() const { return m_master; }
    float musicVolume() const { return m_music; }
    float sfxVolume() const { return m_sfx; }
    void setMasterVolume(float v) { m_master = v; }
    void setMusicVolume(float v) { m_music = v; }
    void setSfxVolume(float v) { m_sfx = v; }

    // --- [video] ---
    // [renderer] 0 = OpenGL, 1 = software rasterizer, 2 = paraLLEl-GS (Vulkan), 3 = Direct3D 11 native.
    // Mirrors PS2SettingsOverlay::Settings; the legacy bool gpuRenderer stays in
    // sync (true when renderer != 1) for the old readers.
    static constexpr int kRendererOpenGL = 0, kRendererSoftware = 1, kRendererParallelGS = 2, kRendererD3D11 = 3;
    int renderer() const { return m_renderer; }
    bool gpuRenderer() const { return m_renderer != kRendererSoftware; }
    bool glow() const { return m_glow; }
    bool glowFix() const { return m_glowFix; }
    bool bilinear() const { return m_bilinear; }
    bool halfTexel() const { return m_halfTexel; }
    bool skipPost() const { return m_skipPost; }
    bool skipStaleVram() const { return m_skipStaleVram; }
    int renderScale() const { return m_renderScale; }
    bool outline() const { return m_outline; }
    int inkStrength() const { return m_inkStrength; }
    int inkWidth() const { return m_inkWidth; }
    unsigned inkColor() const { return m_inkColor; }
    bool shadows() const { return m_shadows; }
    bool dofBlur() const { return m_dofBlur; }
    int dofZFar() const { return m_dofZFar; }
    bool fullscreen() const { return m_fullscreen; }
    // [video.mode] 0 = windowed (resizable), 1 = borderless, 2 = fullscreen. Owns the legacy
    // fullscreen bool, which stays in sync for old readers.
    int windowMode() const { return m_windowMode; }
    int monitor() const { return m_monitor; }
    bool widescreen() const { return m_widescreen; }
    int windowW() const { return m_windowW; }
    int windowH() const { return m_windowH; }
    bool forceBilinear() const { return m_forceBilinear; }
    bool texPack() const { return m_texPack; }   // [texreplace] shared with the in-game overlay
    bool introVideo() const { return m_introVideo; }   // [texui] 4K opening override (on restart)
    bool texcache() const { return m_texcache; }       // [texcache] persistent texture cache
    int buttonLayout() const { return m_buttonLayout; } // [texui] 0 = PS2, 1 = Xbox (on restart)
    bool fps60() const { return m_fps60; }        // [fps60] set in-game; the launcher preserves it
    int hudLayout() const { return m_hudLayout; }
    int hudOffL() const { return m_hudOffL; }
    int hudOffC() const { return m_hudOffC; }
    int hudOffR() const { return m_hudOffR; }

    void setRenderer(int v) { m_renderer = v; }
    // [pgswin] The launcher is never built with PS2X_HAVE_PGS (that is the runner's define), so the
    // #if here always picked OpenGL. The runner falls back to OpenGL itself when Vulkan is missing.
    void setGpuRenderer(bool v) { m_renderer = (v != false) ? kRendererParallelGS : kRendererSoftware; }
    void setGlow(bool v) { m_glow = v; }
    void setGlowFix(bool v) { m_glowFix = v; }
    void setBilinear(bool v) { m_bilinear = v; }
    void setHalfTexel(bool v) { m_halfTexel = v; }
    void setSkipPost(bool v) { m_skipPost = v; }
    void setSkipStaleVram(bool v) { m_skipStaleVram = v; }
    void setRenderScale(int v) { m_renderScale = v; }
    void setOutline(bool v) { m_outline = v; }
    void setInkStrength(int v) { m_inkStrength = v; }
    void setInkWidth(int v) { m_inkWidth = v; }
    void setInkColor(unsigned v) { m_inkColor = v; }
    void setShadows(bool v) { m_shadows = v; }
    void setDofBlur(bool v) { m_dofBlur = v; }
    void setDofZFar(int v) { m_dofZFar = v; }
    void setFullscreen(bool v) { m_fullscreen = v; }
    void setWindowMode(int v) { m_windowMode = v; }
    void setMonitor(int v) { m_monitor = v; }
    void setWidescreen(bool v) { m_widescreen = v; }
    void setWindowSize(int w, int h) { m_windowW = w; m_windowH = h; }
    void setForceBilinear(bool v) { m_forceBilinear = v; }
    void setTexPack(bool v) { m_texPack = v; }   // [texreplace]
    void setIntroVideo(bool v) { m_introVideo = v; }   // [texui]
    void setTexcache(bool v) { m_texcache = v; }       // [texcache]
    void setButtonLayout(int v) { m_buttonLayout = v; }   // [texui]
    void setFps60(bool v) { m_fps60 = v; }        // [fps60]
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

private:
    SettingsManager() = default;
    bool loadToml(const QString &path);
    bool loadIniLegacy(const QString &path);

    bool m_sawRenderer = false;   // [renderer] set when the ini had an explicit `renderer` key
    QString m_dir;
    float m_master = 1.0f, m_music = 1.0f, m_sfx = 1.0f;
    int m_renderer = kRendererParallelGS;   // [pgswin] every platform, as the Video tab's hint says
    bool m_glow = true, m_glowFix = true;
    bool m_bilinear = true, m_halfTexel = true, m_skipPost = true, m_skipStaleVram = true;
    int m_renderScale = 1;
    bool m_outline = true, m_shadows = true, m_dofBlur = true;
    int m_inkStrength = 199, m_dofZFar = 200000;
    int m_inkWidth = 100;
    unsigned m_inkColor = 0;
    bool m_fullscreen = false, m_widescreen = false, m_forceBilinear = true;
    int m_windowMode = 0, m_monitor = 0;   // [video.mode] shared with the in-game overlay
    bool m_texPack = false;   // [texreplace] default OFF
    bool m_introVideo = true; // [texui] default ON: use the pack's 4K opening when present
    bool m_texcache = true;   // [texcache] default ON: persist resolved textures between runs
    int m_buttonLayout = 1;   // [texui] 0 = PS2 (Original Buttons), 1 = Xbox (Xbox Layout)
    bool m_fps60 = false;     // [fps60] default OFF
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