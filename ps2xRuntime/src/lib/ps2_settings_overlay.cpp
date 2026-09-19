#include "ps2_runtime.h"   // [fps60] ps2Set60Fps
#include "runtime/ps2_texreplace.h"
#include "ps2_settings_overlay.h"
#include "runtime/ps2_netplay.h"   // [netplay]
#include "runtime/ps2_gs_pgs.h"   // [pgsink] backend ink width
#include "runtime/ps2_gs_gpu_renderer.h"
#include "runtime/ps2_render_scale.h"
#include "runtime/ps2_audio.h"
#include "runtime/pad_config.h"
#include "runtime/ps2_host_pad.h"
#if defined(__linux__)
#include "runtime/pad_evdev_linux.h"
#endif

#include "imgui.h"
#include "gfx/ps2x_ui.h"
#include "runtime/ps2_video_status.h"   // [video] the status dots   // UiSetup/Begin/End: rlImGui (GL) or imgui_impl_dx11 (PS2X_D3D11)
#include "gfx/bt3gl_api.h"   // [B] bt3* API bridge

#include "runtime/ps2_toml.h"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

static const char *kConfigFileName = "settings.toml";        // launcher + overlay + FMV share this
static const char *kLegacyConfigFileName = "bt3_settings.ini"; // 0.x format, migrated on first load
static const char *kDumpFileName = "bt3_settings_dump.log";

static const char *kConfigHeader =
    "# Dragon Ball Z: Budokai Tenkaichi 3 - Recompiled\n"
    "# User settings - written by the launcher and the in-game overlay.\n"
    "# Delete this file to reset everything to defaults.\n\n";

namespace
{
    // --- settings.toml helpers -----------------------------------------------
    const char *rendererName(int r)
    {
        switch (r) { case 0: return "opengl"; case 1: return "software"; case 2: return "parallel-gs"; case 3: return "d3d11"; default: return "opengl"; }
    }
    int nameToRenderer(const std::string &s, int def)
    {
        if (s == "opengl" || s == "gl") return 0;
        if (s == "software" || s == "sw") return 1;
        if (s == "parallel-gs" || s == "parallel_gs" || s == "pgs") return 2;
        if (s == "d3d11" || s == "dx11" || s == "d3d") return 3;
        return def;
    }
    std::string colorToHex(unsigned c)
    {
        char b[8]; std::snprintf(b, sizeof b, "#%06x", c & 0xFFFFFFu); return b;
    }
    unsigned hexToColor(const std::string &s, unsigned def)
    {
        if (s.empty()) return def;
        try { return static_cast<unsigned>(std::stoul(s[0] == '#' ? s.substr(1) : s, nullptr, 16)) & 0xFFFFFFu; }
        catch (...) { return def; }
    }
} // namespace

// Deploy/retract animation timing for the overlay window (see PS2SettingsOverlay::draw).
static constexpr float kOverlayAnimDuration = 0.16f; // seconds, open or close

// Ease-out cubic: fast start, gentle settle. Used both ways (open + close) so the
// deploy and retract read as mirror images of the same motion.
static float overlayAnimEase(float t)
{
    t = std::max(0.0f, std::min(1.0f, t));
    const float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

// RAII guard for ImGui style pushes: pops exactly what it pushed on destruction,
// even across exceptions/early returns — this is what prevents the overlay from
// corrupting ImGui's style stack and crashing.
struct ScopedStyleColor
{
    ScopedStyleColor(ImGuiCol idx, const ImVec4 &col) { ImGui::PushStyleColor(idx, col); }
    ScopedStyleColor(ImGuiCol idx, ImU32 col) { ImGui::PushStyleColor(idx, col); }
    ~ScopedStyleColor() { ImGui::PopStyleColor(); }
};

struct ScopedStyleVar
{
    ScopedStyleVar(ImGuiStyleVar idx, float v) { ImGui::PushStyleVar(idx, v); }
    ScopedStyleVar(ImGuiStyleVar idx, const ImVec2 &v) { ImGui::PushStyleVar(idx, v); }
    ~ScopedStyleVar() { ImGui::PopStyleVar(); }
};

namespace
{
    std::string nowTimestamp()
    {
        const std::time_t t = std::time(nullptr);
        char buf[64];
        std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        return buf;
    }

    // True when none of the given buttons are currently held.
    bool allButtonsReleased(const std::array<uint8_t, 32> &curBtnDown,
                            const std::vector<int> &btns)
    {
        for (int b : btns)
            if (b >= 0 && b < 32 && curBtnDown[b]) return false;
        return true;
    }

    // True when none of the given keys are currently held.
    bool allKeysReleased(const std::vector<int> &keys)
    {
        for (int k : keys)
            if (bt3IsKeyDown(k)) return false;
        return true;
    }
}

// --- "Capsule HUD" accent palette -------------------------------------------
// Dark near-black tech panel with a single orange accent (still Dragon Ball Z's
// orange/gold, just applied in a flatter, squared-off, HUD-readout style instead
// of the earlier rounded "energy glow" look).
namespace
{
    constexpr float DBZ_R = 1.00f, DBZ_G = 0.62f, DBZ_B = 0.10f;   // HUD orange
    constexpr float GOLD_R = 1.00f, GOLD_G = 0.80f, GOLD_B = 0.30f;

    ImVec4 dbz(float r, float g, float b, float a = 1.0f)
    {
        return ImVec4(r, g, b, a);
    }
    ImVec4 accent(float a = 1.0f) { return dbz(DBZ_R, DBZ_G, DBZ_B, a); }
    ImVec4 gold(float a = 1.0f)   { return dbz(GOLD_R, GOLD_G, GOLD_B, a); }

    void pushDbzTheme()
    {
        ImGuiStyle &s = ImGui::GetStyle();
        s.WindowPadding    = ImVec2(14, 10);
        s.FramePadding     = ImVec2(6, 4);
        s.ItemSpacing      = ImVec2(8, 6);
        s.ItemInnerSpacing = ImVec2(5, 4);
        s.ScrollbarSize    = 12.0f;
        // Capsule HUD: squared-off panel, thin border, no soft rounding anywhere —
        // reads as a technical readout rather than a glowing energy aura.
        s.WindowRounding   = 2.0f;
        s.FrameRounding    = 2.0f;
        s.GrabRounding     = 1.0f;
        s.TabRounding      = 0.0f;
        s.ScrollbarRounding= 2.0f;
        s.WindowBorderSize = 1.0f;
        s.FrameBorderSize  = 1.0f;
        s.TabBarBorderSize = 1.0f;
        s.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    }

    // RAII scope for the whole DBZ theme: pushes every style colour and pops exactly
    // that many on destruction, so the style stack can never leak (the previous code
    // pushed 40 but popped only 36, corrupting ImGui's stack and crashing the overlay).
    struct DbzThemeScope
    {
        static constexpr int kColors = 40;
        DbzThemeScope()
        {
            // Capsule HUD: near-black navy panel, thin orange edges, flat readout
            // rows instead of the previous purple-tinted "glow" surfaces.
            ImGui::PushStyleColor(ImGuiCol_WindowBg,            dbz(0.04f, 0.06f, 0.08f, 0.97f));
            ImGui::PushStyleColor(ImGuiCol_ChildBg,             dbz(0.06f, 0.08f, 0.10f, 0.60f));
            ImGui::PushStyleColor(ImGuiCol_PopupBg,             dbz(0.04f, 0.06f, 0.08f, 0.98f));
            ImGui::PushStyleColor(ImGuiCol_Border,              accent(0.55f));
            ImGui::PushStyleColor(ImGuiCol_BorderShadow,        dbz(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_TitleBg,             dbz(0.04f, 0.06f, 0.08f));
            ImGui::PushStyleColor(ImGuiCol_TitleBgActive,       dbz(0.04f, 0.06f, 0.08f));
            ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed,    dbz(0.04f, 0.06f, 0.08f));
            ImGui::PushStyleColor(ImGuiCol_Text,                dbz(0.84f, 0.89f, 0.92f));
            ImGui::PushStyleColor(ImGuiCol_TextDisabled,        dbz(0.29f, 0.39f, 0.44f));
            ImGui::PushStyleColor(ImGuiCol_TextSelectedBg,      accent(0.30f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg,             dbz(0.07f, 0.09f, 0.11f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,      dbz(0.10f, 0.13f, 0.15f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive,       dbz(1.00f, 0.62f, 0.10f, 0.20f));
            ImGui::PushStyleColor(ImGuiCol_SliderGrab,          accent());
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,    gold());
            ImGui::PushStyleColor(ImGuiCol_CheckMark,           accent());
            ImGui::PushStyleColor(ImGuiCol_Button,              dbz(0.07f, 0.09f, 0.11f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,       dbz(1.00f, 0.62f, 0.10f, 0.18f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,        dbz(1.00f, 0.62f, 0.10f, 0.30f));
            ImGui::PushStyleColor(ImGuiCol_Header,              dbz(0.09f, 0.11f, 0.13f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered,       accent(0.22f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive,        accent(0.35f));
            ImGui::PushStyleColor(ImGuiCol_Separator,           dbz(0.12f, 0.16f, 0.19f));
            ImGui::PushStyleColor(ImGuiCol_SeparatorHovered,    accent(0.55f));
            ImGui::PushStyleColor(ImGuiCol_SeparatorActive,     gold());
            // Flat "ghost" tabs with a thin orange border (TabBarBorderSize above)
            // instead of a filled rounded-pill active tab — reads as a HUD section
            // switcher rather than a browser-style tab strip.
            ImGui::PushStyleColor(ImGuiCol_Tab,                 dbz(0.04f, 0.06f, 0.08f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_TabHovered,          accent(0.20f));
            ImGui::PushStyleColor(ImGuiCol_TabActive,           accent(0.14f));
            ImGui::PushStyleColor(ImGuiCol_TabUnfocused,        dbz(0.04f, 0.06f, 0.08f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_TabUnfocusedActive,  accent(0.10f));
            ImGui::PushStyleColor(ImGuiCol_TableHeaderBg,       dbz(0.08f, 0.10f, 0.12f));
            ImGui::PushStyleColor(ImGuiCol_TableRowBg,          dbz(0.05f, 0.07f, 0.09f, 0.50f));
            ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt,       dbz(1.00f, 0.62f, 0.10f, 0.04f));
            ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,         dbz(0.04f, 0.06f, 0.08f, 0.60f));
            ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,       accent(0.45f));
            ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered,accent(0.70f));
            ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, gold());
            ImGui::PushStyleColor(ImGuiCol_PlotLines,           accent());
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram,       accent());
        }
        ~DbzThemeScope() { ImGui::PopStyleColor(kColors); }
        DbzThemeScope(const DbzThemeScope &) = delete;
        DbzThemeScope &operator=(const DbzThemeScope &) = delete;
    };

    // --- Capsule HUD font loading -------------------------------------------
    // Resolved font path lives in a file-scope static so loadOverlayFonts()
    // (called from initialize()) can read it without user-data arguments.
    std::string s_hudFontPath;
    bool s_fontLoaded = false;

    void loadOverlayFonts()
    {
        ImGuiIO &io = ImGui::GetIO();
        // Wipe the atlas built by rlImGui (default font + FontAwesome) and
        // replace it with a single Russo One face.  Because Russo One becomes
        // Fonts[0] (the ImGui "default" font), every piece of text in the
        // overlay — title, tabs, section headers, body — renders in it
        // automatically without any PushFont calls.
        io.Fonts->Clear();
        if (!s_hudFontPath.empty())
        {
            io.Fonts->AddFontFromFileTTF(s_hudFontPath.c_str(), 16.0f);
            s_fontLoaded = true;
        }
        else
        {
            io.Fonts->AddFontDefault();
            s_fontLoaded = false;
        }
    }

    // Blinking alpha for "Press..." capture hints.
    float blinkAlpha()
    {
        return 0.5f + 0.5f * std::sin(ImGui::bt3GetTime() * 6.0f);
    }

    // Section header helper: small uppercase accent text (Russo One, when loaded)
    // with a leading orange bar.
    void sectionHeader(const char *label)
    {
        ImGui::Spacing();
        ImGui::Spacing();
        {
            ScopedStyleColor c(ImGuiCol_Text, accent());
            ImGui::TextUnformatted(label);
        }
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 100);
        {
            ScopedStyleColor c(ImGuiCol_Separator, accent(0.35f));
            ImGui::Separator();
        }
        ImGui::Spacing();
    }

    // Simple toggle-switch look: checkbox with an ON/OFF trailing badge.
    bool toggleSwitch(const char *label, bool *v)
    {
        ImGui::PushID(label);   // unique ID per toggle -> no ON/OFF ID conflicts
        bool changed = ImGui::Checkbox(label, v);
        ImGui::SameLine();
        const bool on = *v;
        {
            ScopedStyleColor c0(ImGuiCol_Button, on ? accent() : dbz(0.25f, 0.25f, 0.32f));
            ScopedStyleColor c1(ImGuiCol_ButtonHovered, on ? gold() : dbz(0.30f, 0.30f, 0.38f));
            ScopedStyleColor c2(ImGuiCol_ButtonActive, on ? gold() : dbz(0.30f, 0.30f, 0.38f));
            ScopedStyleColor c3(ImGuiCol_Text, on ? dbz(0.10f, 0.07f, 0.03f) : dbz(0.85f, 0.85f, 0.85f));
            ImGui::SmallButton(on ? "ON" : "OFF");
        }
        ImGui::PopID();
        return changed;
    }
}

bool PS2SettingsOverlay::s_widescreen = false;

// [fsnative] Fullscreen at the MONITOR's resolution. raylib's bt3ToggleFullscreen() keeps the window's current size as
// the video mode (1024x768 from the INI); on Wayland the compositor then stretches that 4:3 surface across the 16:9
// panel while the game still sees a 4:3 screen -- neither the true-widescreen FOV patch nor the HUD squeeze engage
// and the whole picture is stretched. Size the window to the monitor first; restore the saved size on the way out.
// monitor = the CONFIGURED display (m_settings.monitor), not bt3GetCurrentMonitor(): the latter is wherever the
// window happens to be, which made "make it fullscreen" move the game to the other monitor.
// PS2X_FSNATIVE=0 restores the old toggle.
static void ps2xSetFullscreen(bool on, int windowW, int windowH, int monitor)
{
    static const bool s_native = [](){ const char *v = std::getenv("PS2X_FSNATIVE"); return !(v && v[0] == '0'); }();
    if (!s_native) { bt3ToggleFullscreen(); return; }
    if (on)
    {
        if (bt3IsWindowFullscreen()) return;
        const int mc = bt3GetMonitorCount();
        const int m = (monitor >= 0 && monitor < mc) ? monitor : 0;
        if (mc > 0) bt3SetWindowMonitor(m);
        const int mw = bt3GetMonitorWidth(m), mh = bt3GetMonitorHeight(m);
        if (mw >= 320 && mh >= 240) bt3SetWindowSize(mw, mh);
        bt3ToggleFullscreen();
    }
    else
    {
        if (bt3IsWindowFullscreen()) bt3ToggleFullscreen();
        if (windowW >= 320 && windowH >= 240) bt3SetWindowSize(windowW, windowH);
    }
}
// [wshudmap] live HUD-layout state, defined in ps2_gs_gpu_renderer.cpp
extern std::atomic<int> g_wsHudLayout;
extern std::atomic<int> g_wsHudOffLQ, g_wsHudOffCQ, g_wsHudOffRQ;
static void pushHudLayout(const PS2SettingsOverlay::Settings &st)
{
    // an explicit PS2X_WSHUDLAYOUT env (rig lever) outranks the INI/UI
    if (std::getenv("PS2X_WSHUDLAYOUT")) return;
    g_wsHudOffLQ.store(st.hudOffL * 16, std::memory_order_relaxed);
    g_wsHudOffCQ.store(st.hudOffC * 16, std::memory_order_relaxed);
    g_wsHudOffRQ.store(st.hudOffR * 16, std::memory_order_relaxed);
    g_wsHudLayout.store(st.hudLayout, std::memory_order_relaxed);
}
std::string PS2SettingsOverlay::s_configDir;
int PS2SettingsOverlay::s_logLevel = 1;        // [loglevel] default: profile+mclog+sched (see header)
int PS2SettingsOverlay::s_startupLogLevel = 1; // [loglevel] captured by preloadSettings for main()

void PS2SettingsOverlay::setConfigDirectory(const std::string &dir)
{
    s_configDir = dir;
    if (!s_configDir.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(s_configDir, ec);
    }
}

void PS2SettingsOverlay::initialize()
{
    if (m_initialized)
        return;
    // --- Capsule HUD display font (Russo One) -------------------------------
    // Shipped under assets/fonts next to the executable (portable dist), same
    // layout convention as savedata/ (see getExecutableDirectory() in main.cpp).
    // s_configDir is "<exeDir>/savedata", so its parent is <exeDir>.
    //
    // rlImGui builds its font atlas inside rlImGuiSetup() -> rlImGuiBeginInitImGui(),
    // which calls AddFontDefault() for us UNLESS we register our own callback via
    // rlImGuiSetLoadFontsCallback() first (rlImGui has no separate "reload fonts"
    // entry point — the callback is the supported hook for adding extra fonts). So
    // the path must be resolved and the callback installed BEFORE rlImGuiSetup runs.
    if (!s_configDir.empty())
    {
        const char *assetDir = std::getenv("PS2X_ASSETDIR");
        const std::filesystem::path assets = assetDir && *assetDir
            ? std::filesystem::path(assetDir)
            : std::filesystem::path(s_configDir).parent_path() / "assets";
        const std::filesystem::path fontPath = assets / "fonts" / "RussoOne-Regular.ttf";
        std::error_code ec;
        if (std::filesystem::is_regular_file(fontPath, ec) && !ec)
            s_hudFontPath = fontPath.string();
        else
            std::fprintf(stderr, "[overlay] Russo One font not found at %s, falling back to default font\n",
                        fontPath.string().c_str());
    }
    ps2x::gfx::UiSetup();
    // The rlImGui version used here has no rlImGuiSetLoadFontsCallback() hook, so load
    // the Capsule HUD fonts directly after setup — ImGui rebuilds the atlas lazily on
    // the first frame.
    loadOverlayFonts();

    ImGuiIO &io = ImGui::GetIO();
    // Do NOT enable NavEnableGamepad: raylib feeds the connected gamepad's axes/buttons
    // straight into ImGui, so with the nav flag on a joystick at rest would yank sliders
    // (e.g. volume) to 0 the instant they are clicked. The overlay is mouse/keyboard only.
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;

    m_initialized = true;
    m_configPath = s_configDir.empty()
        ? (std::filesystem::current_path() / kConfigFileName).string()
        : (std::filesystem::path(s_configDir) / kConfigFileName).string();
    loadSettings();
    // The runtime already applied window_mode/monitor before the window was mapped (see the [winmode]
    // block in ps2_runtime.cpp). Re-applying the legacy size+fullscreen here fought that: it resized
    // the fullscreen window back to the saved window size and re-entered fullscreen on whichever
    // monitor the window happened to be (usually the wrong one). Only the windowed mode needs the
    // saved size restored, and only if it differs.
    if (m_settings.windowMode == 0 && m_settings.windowW >= 320 && m_settings.windowH >= 240
        && (bt3GetScreenWidth() != m_settings.windowW || bt3GetScreenHeight() != m_settings.windowH))
        bt3SetWindowSize(m_settings.windowW, m_settings.windowH);
    // Build the device list up front so the gamepad toggle combo works before the
    // overlay is opened for the first time (m_deviceList is otherwise only populated
    // when the overlay opens via resetCaptureState/buildDeviceList).
    buildDeviceList();
    if (m_selectedDevice > 0) applyDeviceToPlayer(0, m_selectedDevice);   // [paddev] legacy single ini index = P1's
    m_selectedDevice = deviceIndexForPlayer(m_editPlayer);
    // Apply all loaded settings (glow, volume, etc.) at startup.
    applySettings();
    // Snapshot the persisted settings so shutdown() only rewrites the ini when the
    // session actually changed something (keeps launcher-authored values intact).
    m_settingsAtBoot = m_settings;
}

void PS2SettingsOverlay::shutdown()
{
    if (!m_initialized)
        return;
    // Persist only when something changed this session OR no ini exists yet
    // (first boot seeds the default file). Otherwise leave the existing
    // ini untouched so launcher-authored settings survive a play session.
    if (!(m_settings == m_settingsAtBoot) || !std::filesystem::exists(m_configPath))
        saveSettings();
    ps2x::gfx::UiShutdown();
    m_initialized = false;
}

bool PS2SettingsOverlay::Settings::operator==(const Settings &o) const
{
    return masterVolume == o.masterVolume &&
           musicVolume == o.musicVolume &&
           sfxVolume == o.sfxVolume &&
           gpuRenderer == o.gpuRenderer &&
           renderer == o.renderer && windowMode == o.windowMode && monitor == o.monitor &&
           glow == o.glow &&
           glowFix == o.glowFix &&
           bilinear == o.bilinear &&
           halfTexel == o.halfTexel &&
           skipPost == o.skipPost &&
           skipStaleVram == o.skipStaleVram &&
           renderScale == o.renderScale &&
           deadzone == o.deadzone &&
           fullscreen == o.fullscreen &&
           widescreen == o.widescreen &&
           outline == o.outline &&
           texPack == o.texPack &&
           introVideo == o.introVideo &&
           buttonLayout == o.buttonLayout &&
           fps60 == o.fps60 &&
           inkStrength == o.inkStrength &&
           shadows == o.shadows &&
           dofBlur == o.dofBlur &&
           dofZFar == o.dofZFar &&
           windowW == o.windowW &&
           windowH == o.windowH &&
           forceBilinear == o.forceBilinear &&
           overlayEnabled == o.overlayEnabled &&
           hudLayout == o.hudLayout &&
           hudOffL == o.hudOffL &&
           hudOffC == o.hudOffC &&
           hudOffR == o.hudOffR &&
           overlayPadBtns == o.overlayPadBtns &&
           overlayKeys == o.overlayKeys &&
           logLevel == o.logLevel;
}

void PS2SettingsOverlay::resetCaptureState()
{
    m_captureAction = -1;
    m_captureWaitRelease = false;
    m_prevBtnDown = {};
    m_prevAxis = {};
    buildDeviceList();
    m_selectedDevice = deviceIndexForPlayer(m_editPlayer);   // [paddev]
}

void PS2SettingsOverlay::toggleVisible()
{
    m_visible = !m_visible;
    ps2_stubs::PadConfig::setInputSuspended(m_visible);
    if (m_visible)
        resetCaptureState();
    else
    {   // [noapply] closing = save (settings + per-action bindings)
        saveSettings();
        ps2_stubs::PadConfig::instance().save();
    }
}

// [envwins] An env var the USER set explicitly (present, and not one main() defaulted --
// PS2X_DEFAULTED lists those) outranks the saved INI: play.sh-style launches and A/B runs
// must behave as commanded regardless of what the overlay saved last session.
static bool m_sawRendererKey = false;   // [renderer] set while parsing the ini
static bool envUserSet(const char *name)
{
    if (!std::getenv(name)) return false;
    const char *d = std::getenv("PS2X_DEFAULTED");
    if (!d) return true;
    std::string needle = std::string(",") + name + ",";
    return std::string(d).find(needle) == std::string::npos;
}

void PS2SettingsOverlay::loadSettings()
{
    m_sawRendererKey = false;
    // [defaults-sync] Seed from LIVE runtime state (env + main()'s baked defaults) so a
    // missing INI -- or a key the INI doesn't mention -- never pushes this struct's
    // hardcoded values over the validated configuration.
    syncFromRuntime();

    std::ifstream file(m_configPath);
    if (!file.is_open())
        return;

    ps2x_toml::Document doc;
    doc.parse(file);

    m_settings.masterVolume = std::clamp((float)doc.getD("audio.master_volume", m_settings.masterVolume), 0.0f, 1.0f);
    m_settings.musicVolume = std::clamp((float)doc.getD("audio.music_volume", m_settings.musicVolume), 0.0f, 1.0f);
    m_settings.sfxVolume = std::clamp((float)doc.getD("audio.sfx_volume", m_settings.sfxVolume), 0.0f, 1.0f);

    {
            int r = nameToRenderer(doc.getS("video.renderer", rendererName(m_settings.renderer)), m_settings.renderer);
#if !defined(PS2X_HAVE_PGS)
            if (r == Settings::kRendererParallelGS) r = Settings::kRendererOpenGL;
#endif
            // [d3d11] Direct3D 11 is retired for now: an old settings file that picks it falls back to
            // the new OpenGL present. paraLLEl-GS is a normal option on every platform again.
            if (r == Settings::kRendererD3D11) r = Settings::kRendererOpenGL;
            if (r >= 0 && r <= 3) { m_settings.renderer = r; m_sawRendererKey = true; }
            // [display] window mode / monitor: the popup owns them, defaulted from the legacy fullscreen flag
            m_settings.windowMode = doc.getI("video.window_mode", m_settings.fullscreen ? 2 : 0);
            m_settings.monitor = doc.getI("video.monitor", 0);
    }
    if (!envUserSet("PS2X_GLOW")) m_settings.glow = doc.getB("video.glow", m_settings.glow);
    if (!envUserSet("PS2X_GLOWFIX")) m_settings.glowFix = doc.getB("video.glowfix", m_settings.glowFix);
    if (!envUserSet("PS2X_INKSTRENGTH") && !envUserSet("PS2X_ADGS"))
        m_settings.inkStrength = std::clamp(doc.getI("video.ink_strength", m_settings.inkStrength), 100, 400);
    m_settings.inkWidth = std::clamp(doc.getI("video.ink_width", m_settings.inkWidth), 25, 100);
    m_settings.inkColor = hexToColor(doc.getS("video.ink_color", colorToHex(m_settings.inkColor)), m_settings.inkColor);
    if (!envUserSet("PS2X_BILINEAR")) m_settings.bilinear = doc.getB("video.bilinear", m_settings.bilinear);
    if (!envUserSet("PS2X_HALFTEXEL")) m_settings.halfTexel = doc.getB("video.halftexel", m_settings.halfTexel);
    if (!envUserSet("PS2X_SKIPPOST")) m_settings.skipPost = doc.getB("video.skippost", m_settings.skipPost);
    if (!envUserSet("PS2X_SKIP_STALE_VRAM")) m_settings.skipStaleVram = doc.getB("video.skip_stale_vram", m_settings.skipStaleVram);
    if (!envUserSet("PS2X_RENDER_SCALE"))
    {
        const int s = doc.getI("video.render_scale", m_settings.renderScale);
        m_settings.renderScale = (s >= 1 && s <= 4) ? s : 1;
    }
    if (!envUserSet("PS2X_OUTLINE")) m_settings.outline = doc.getB("video.outline", m_settings.outline);
    if (!envUserSet("PS2X_TEXPACK")) m_settings.texPack = doc.getB("video.texture_pack", m_settings.texPack);
    if (!envUserSet("PS2X_FMV_OVERRIDE")) m_settings.introVideo = doc.getB("video.intro_video", m_settings.introVideo);
    if (!envUserSet("PS2X_BUTTONS")) m_settings.buttonLayout = doc.getI("video.button_layout", m_settings.buttonLayout);
    if (!envUserSet("PS2X_SHADOWS")) m_settings.shadows = doc.getB("video.shadows", m_settings.shadows);
    if (!envUserSet("PS2X_DOFMASK")) m_settings.dofBlur = doc.getB("video.dof_blur", m_settings.dofBlur);
    if (!envUserSet("PS2X_DOFZFAR")) m_settings.dofZFar = std::clamp(doc.getI("video.dof_zfar", m_settings.dofZFar), 20000, 800000);
    m_settings.fullscreen = doc.getB("video.fullscreen", m_settings.fullscreen);
    m_settings.widescreen = doc.getB("video.widescreen", m_settings.widescreen);
    m_settings.fps60 = doc.getB("video.fps60", m_settings.fps60);
    m_settings.windowW = doc.getI("video.window_w", m_settings.windowW);
    m_settings.windowH = doc.getI("video.window_h", m_settings.windowH);
    m_settings.forceBilinear = doc.getB("video.force_bilinear", m_settings.forceBilinear);
    m_settings.hudLayout = doc.getI("video.hud.layout", m_settings.hudLayout);
    m_settings.hudOffL = doc.getI("video.hud.offset_left", m_settings.hudOffL);
    m_settings.hudOffC = doc.getI("video.hud.offset_center", m_settings.hudOffC);
    m_settings.hudOffR = doc.getI("video.hud.offset_right", m_settings.hudOffR);

    m_settings.deadzone = std::clamp((float)doc.getD("controllers.deadzone", m_settings.deadzone), 0.0f, 0.5f);
    m_settings.overlayEnabled = doc.getB("controllers.overlay_enabled", m_settings.overlayEnabled);
    m_selectedDevice = std::clamp(doc.getI("controllers.device", m_selectedDevice), 0, 100);
    {
        std::vector<int> pb = doc.getIA("controllers.hotkey.pad_btns", m_settings.overlayPadBtns);
        for (int &b : pb) b = std::clamp(b, 0, 31);
        if (!pb.empty()) m_settings.overlayPadBtns = pb;
        std::vector<int> keys = doc.getIA("controllers.hotkey.keys", m_settings.overlayKeys);
        for (int &k : keys) k = std::clamp(k, 32, 348);
        if (!keys.empty()) m_settings.overlayKeys = keys;
    }

    m_dumpAudio = doc.getB("logging.dump_audio", m_dumpAudio);
    m_dumpVideo = doc.getB("logging.dump_video", m_dumpVideo);
    m_dumpControllers = doc.getB("logging.dump_controllers", m_dumpControllers);
    m_dumpRuntime = doc.getB("logging.dump_runtime", m_dumpRuntime);
    m_dumpGamepad = doc.getB("logging.dump_gamepad", m_dumpGamepad);
    m_settings.logLevel = std::clamp(doc.getI("logging.log_level", m_settings.logLevel), 0, 3);
    s_logLevel = m_settings.logLevel;

    // [renderer] keep gpuRenderer coherent for the older readers.
    m_settings.gpuRenderer = (m_settings.renderer != Settings::kRendererSoftware);
}

// [renderer] The paraLLEl-GS backend reads PS2X_PGS* lazily at its first packet, so the ini choice is exported as
// environment DEFAULTS here (setenv(...,0): an explicit user override still wins). renderer 2 = backend on, exclusive
// (our GS parse skipped) unless a texture pack is on, where the backend needs our state-only parse for the hashes.
static void setEnvDefault(const char *name, const char *value)
{   // never overwrites: an explicit user env override wins
    if (std::getenv(name)) return;
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 0);
#endif
}
static void exportRendererEnv(int renderer, bool texPack, bool forceBilinear)
{
#if defined(_WIN32)
    // [d3d11] The D3D11 present is retired for now: nothing selects it any more, and the flag is
    // forced off so a stale PS2X_D3D11 in the environment cannot bring back the old path.
    setEnvDefault("PS2X_D3D11", "0");
#endif
    // [opengl-new] renderer 0 is the NEW OpenGL present (gfx::gl / altGL). The old raylib GL present
    // is gone, so this is the only OpenGL option the UI offers.
    if (renderer == 0)
    {
        setEnvDefault("PS2X_ALTGL", "1");
        setEnvDefault("PS2X_PGS", "0");
        return;
    }
#if defined(PS2X_HAVE_PGS)
    if (renderer == 2)
    {
        setEnvDefault("PS2X_PGS", "1");
        // [pgslive] pack mode only when a pack is actually INDEXED (PS2X_TEXREPLACE or data/Textures): the Texture
        // Replacement switch is greyed out without one, and pack mode costs a second packet walk per frame (a laptop
        // 4060 log showed 17-26 ms/swap of backend CPU at 4x with the switch on and NO pack). With a pack the switch
        // still flips live in either direction. PS2X_PGS_PACK=0 in the env forces the exclusive path.
        setEnvDefault("PS2X_PGS_PACK", ps2tex::replacementsEnabled() ? "1" : "0");
        { const char *pk = std::getenv("PS2X_PGS_PACK"); if (!(pk && pk[0] == '1')) setEnvDefault("PS2X_PGS_EXCLUSIVE", "1"); }
        if (forceBilinear) setEnvDefault("PS2X_PGS_FORCE_BILINEAR", "1");
    }
    else
        setEnvDefault("PS2X_PGS", "0");
#else
    (void)renderer; (void)texPack; (void)forceBilinear;
#endif
}

extern "C" const char *ps2xExeDirC();   // [mergefix] main.cpp: <exeDir> (honors PS2X_EXEDIR)

void PS2SettingsOverlay::preloadSettings()
{
    // [cfgpath] The deploy keeps settings.toml in <exeDir>/savedata, but the overlay only
    // looked in the CWD unless setConfigDirectory() had been called (never, in practice), so a
    // launch that did not set the CWD to the deploy silently dropped every setting -- notably
    // texture_pack, i.e. "the texture pack does not load". Prefer an existing file: CWD first,
    // then <exeDir>/savedata, then <exeDir>; fall back to the CWD path.
    std::filesystem::path cfgPath;
    if (!s_configDir.empty())
        cfgPath = std::filesystem::path(s_configDir) / kConfigFileName;
    else
    {
        const std::filesystem::path cwd = std::filesystem::current_path() / kConfigFileName;
        const char *xd = ps2xExeDirC();
        std::error_code ec;
        const std::filesystem::path exeSaved = (xd && xd[0]) ? (std::filesystem::path(xd) / "savedata" / kConfigFileName) : std::filesystem::path();
        const std::filesystem::path exeRoot = (xd && xd[0]) ? (std::filesystem::path(xd) / kConfigFileName) : std::filesystem::path();
        if (std::filesystem::exists(cwd, ec)) cfgPath = cwd;
        else if (!exeSaved.empty() && std::filesystem::exists(exeSaved, ec)) cfgPath = exeSaved;
        else if (!exeRoot.empty() && std::filesystem::exists(exeRoot, ec)) cfgPath = exeRoot;
        else cfgPath = cwd;
    }
    const std::string configPath = cfgPath.string();
    s_configDir = cfgPath.parent_path().string();
    int rendererPre = Settings::kRendererDefault;   // [renderer] exported below even when no toml exists yet
    bool texPackPre = false;
    bool forceBilinearPre = true;

    std::ifstream file(configPath);
    if (!file.is_open())
    {
        // 0.x legacy INI: the launcher imports it and writes the TOML (dropping the old
        // file). Running the runner directly, just clear a stray leftover.
        const std::string legacy = s_configDir.empty()
            ? (std::filesystem::current_path() / kLegacyConfigFileName).string()
            : (std::filesystem::path(s_configDir) / kLegacyConfigFileName).string();
        std::error_code ec;
        std::filesystem::remove(legacy, ec);
        exportRendererEnv(rendererPre, texPackPre, forceBilinearPre);
        return;
    }

    ps2x_toml::Document doc;
    doc.parse(file);

    s_widescreen = doc.getB("video.widescreen", s_widescreen);
    rendererPre = nameToRenderer(doc.getS("video.renderer", rendererName(rendererPre)), rendererPre);
    texPackPre = doc.getB("video.texture_pack", texPackPre);
    forceBilinearPre = doc.getB("video.force_bilinear", forceBilinearPre);
    {   // [rscale] authoritative startup application -- runs before anything reads the
        // live scale, so the TOML value wins the lazy-init race.
        const int rs = doc.getI("video.render_scale", 0);
        if (rs >= 1 && rs <= 4 && !envUserSet("PS2X_RENDER_SCALE") && !envUserSet("PS2X_RENDERSCALE"))
        {
            GsGpuRenderer::setRenderScale(rs);
            if (!envUserSet("PS2X_PGS_SSAA")) ps2x_pgs::setRenderScale(rs);   // [pgslive] backend starts at the file scale
        }
    }
    {
        // [loglevel] capture for main(): must be visible before runtime init so the
        // PS2X_* diagnostic env vars (and the stderr redirect to logs/bt3.log) apply.
        const int lvl = std::clamp(doc.getI("logging.log_level", s_startupLogLevel), 0, 3);
        s_logLevel = lvl;
        s_startupLogLevel = lvl;
    }
    exportRendererEnv(rendererPre, texPackPre, forceBilinearPre);
}

void PS2SettingsOverlay::saveSettings() const
{
    using ps2x_toml::fmtBool;
    using ps2x_toml::fmtDbl;
    using ps2x_toml::fmtInt;
    using ps2x_toml::fmtIntArray;
    using ps2x_toml::fmtStr;

    std::ostringstream os;
    os << kConfigHeader << "\n";

    os << "[audio]\n";
    os << "master_volume = " << fmtDbl(m_settings.masterVolume) << "\n";
    os << "music_volume = " << fmtDbl(m_settings.musicVolume) << "\n";
    os << "sfx_volume = " << fmtDbl(m_settings.sfxVolume) << "\n\n";

    os << "[video]\n";
    os << "renderer = " << fmtStr(rendererName(m_settings.renderer)) << "\n";
    os << "glow = " << fmtBool(m_settings.glow) << "\n";
    os << "glowfix = " << fmtBool(m_settings.glowFix) << "\n";
    os << "ink_strength = " << fmtInt(m_settings.inkStrength) << "\n";
    os << "ink_width = " << fmtInt(m_settings.inkWidth) << "\n";
    os << "ink_color = " << fmtStr(colorToHex(m_settings.inkColor)) << "\n";
    os << "bilinear = " << fmtBool(m_settings.bilinear) << "\n";
    os << "halftexel = " << fmtBool(m_settings.halfTexel) << "\n";
    os << "skippost = " << fmtBool(m_settings.skipPost) << "\n";
    os << "skip_stale_vram = " << fmtBool(m_settings.skipStaleVram) << "\n";
    os << "render_scale = " << fmtInt(m_settings.renderScale) << "\n";
    os << "outline = " << fmtBool(m_settings.outline) << "\n";
    os << "texture_pack = " << fmtBool(m_settings.texPack) << "\n";
    os << "intro_video = " << fmtBool(m_settings.introVideo) << "\n";
    os << "button_layout = " << fmtInt(m_settings.buttonLayout) << "\n";
    os << "shadows = " << fmtBool(m_settings.shadows) << "\n";
    os << "dof_blur = " << fmtBool(m_settings.dofBlur) << "\n";
    os << "dof_zfar = " << fmtInt(m_settings.dofZFar) << "\n";
    os << "fullscreen = " << fmtBool(m_settings.fullscreen) << "\n";
    os << "widescreen = " << fmtBool(m_settings.widescreen) << "\n";
    os << "window_w = " << fmtInt(m_settings.windowW) << "\n";
    os << "window_h = " << fmtInt(m_settings.windowH) << "\n";
    os << "force_bilinear = " << fmtBool(m_settings.forceBilinear) << "\n";
    os << "window_mode = " << m_settings.windowMode << "\n";
            os << "monitor = " << m_settings.monitor << "\n";
            os << "fps60 = " << fmtBool(m_settings.fps60) << "\n\n";

    os << "[video.hud]\n";
    os << "layout = " << fmtInt(m_settings.hudLayout) << "\n";
    os << "offset_left = " << fmtInt(m_settings.hudOffL) << "\n";
    os << "offset_center = " << fmtInt(m_settings.hudOffC) << "\n";
    os << "offset_right = " << fmtInt(m_settings.hudOffR) << "\n\n";

    os << "[controllers]\n";
    os << "device = " << fmtInt(deviceIndexForPlayer(0)) << "\n";   // [paddev] P1 (the launcher has one picker)
    os << "deadzone = " << fmtDbl(m_settings.deadzone) << "\n";
    os << "overlay_enabled = " << fmtBool(m_settings.overlayEnabled) << "\n\n";

    os << "[controllers.hotkey]\n";
    os << "pad_btns = " << fmtIntArray(m_settings.overlayPadBtns) << "\n";
    os << "keys = " << fmtIntArray(m_settings.overlayKeys) << "\n\n";

    os << "[logging]\n";
    os << "log_level = " << fmtInt(m_settings.logLevel) << "\n";
    os << "dump_audio = " << fmtBool(m_dumpAudio) << "\n";
    os << "dump_video = " << fmtBool(m_dumpVideo) << "\n";
    os << "dump_controllers = " << fmtBool(m_dumpControllers) << "\n";
    os << "dump_runtime = " << fmtBool(m_dumpRuntime) << "\n";
    os << "dump_gamepad = " << fmtBool(m_dumpGamepad) << "\n";

    std::ofstream file(m_configPath, std::ios::trunc);
    if (!file.is_open())
        return;
    file << os.str();
}

void PS2SettingsOverlay::applyDeadzone()
{
    auto &pcfg = ps2_stubs::PadConfig::instance();
    for (size_t p = 0; p < ps2_stubs::PadConfig::kPlayerCount; ++p)
    {
        auto cfg = pcfg.snapshot(p);
        bool changed = false;
        for (size_t a = 0; a < static_cast<size_t>(ps2_stubs::PadAction::Count); ++a)
        {
            if (cfg.binds[a].deadzone != m_settings.deadzone)
            {
                cfg.binds[a].deadzone = m_settings.deadzone;
                changed = true;
            }
        }
        if (changed)
        {
            for (size_t a = 0; a < static_cast<size_t>(ps2_stubs::PadAction::Count); ++a)
                pcfg.setBind(p, static_cast<ps2_stubs::PadAction>(a), cfg.binds[a]);
        }
    }
}

void PS2SettingsOverlay::syncFromRuntime()
{
    m_settings.masterVolume = PS2AudioBackend::masterVolume();
    m_settings.musicVolume = PS2AudioBackend::musicVolume();
    m_settings.sfxVolume = PS2AudioBackend::sfxVolume();
    m_settings.gpuRenderer = GsGpuRenderer::enabled();
    m_settings.glow = GsGpuRenderer::glowEnabled();
    m_settings.glowFix = GsGpuRenderer::glowFixEnabled();
    m_settings.inkStrength = GsGpuRenderer::inkStrengthPct();
    m_settings.bilinear = GsGpuRenderer::bilinearEnabled();
    m_settings.halfTexel = GsGpuRenderer::halfTexelEnabled();
    m_settings.skipPost = GsGpuRenderer::skipPostEnabled();
    m_settings.skipStaleVram = GsGpuRenderer::skipStaleVramEnabled();
    m_settings.renderScale = GsGpuRenderer::renderScale();
    m_settings.outline = GsGpuRenderer::outlineEnabled();
    m_settings.texPack = GsGpuRenderer::texPackEnabled();
    m_settings.shadows = GsGpuRenderer::shadowsEnabled();
    m_settings.dofBlur = GsGpuRenderer::dofBlurEnabled();
    m_settings.dofZFar = GsGpuRenderer::dofZFar();
}

void PS2SettingsOverlay::applySettings()
{
    ps2Set60Fps(m_settings.fps60, nullptr);   // [fps60]
    s_widescreen = m_settings.widescreen;
    PS2AudioBackend::setMasterVolume(m_settings.masterVolume);
    PS2AudioBackend::setMusicVolume(m_settings.musicVolume);
    PS2AudioBackend::setSfxVolume(m_settings.sfxVolume);
    m_settings.gpuRenderer = (m_settings.renderer != Settings::kRendererSoftware);   // [renderer]
    GsGpuRenderer::setEnabled(m_settings.gpuRenderer);
    GsGpuRenderer::setGlow(m_settings.glow);
    // [glowfix] applied at STARTUP only: two of its four parts (the fbp224/fbp336 size caps)
    // are decided when the FBO is allocated, so flipping it mid-run would leave a half-applied
    // state -- and a partial glow fix is a REGRESSION (it washes the frame out).
    GsGpuRenderer::setGlowFix(m_settings.glowFix);
    GsGpuRenderer::setInkStrengthPct(m_settings.inkStrength);   // [inkstrength] live: it is one shader uniform
    ps2x_pgs::setInkWidthPct(m_settings.inkWidth);   // [pgsink] backend stroke width
    ps2x_pgs::setInkColor(m_settings.inkColor);       // [pgsink] backend stroke colour
    GsGpuRenderer::setBilinear(m_settings.bilinear);
    GsGpuRenderer::setHalfTexel(m_settings.halfTexel);
    GsGpuRenderer::setSkipPost(m_settings.skipPost);
    GsGpuRenderer::setSkipStaleVram(m_settings.skipStaleVram);
    // [rscale] NOT applied here -- see preloadSettings (startup-only). applySettings runs
    // on live changes too, and a live scale store would desync the pipeline.
    GsGpuRenderer::setOutline(m_settings.outline);
    GsGpuRenderer::setTexPack(m_settings.texPack);
    ps2x_pgs::setPackEnabled(m_settings.texPack);    // [pgslive]
    if (!envUserSet("PS2X_PGS_SSAA")) ps2x_pgs::setRenderScale(m_settings.renderScale); // [pgslive] (an explicit launcher SSAA wins until the combo is touched)
    GsGpuRenderer::setShadows(m_settings.shadows);
    GsGpuRenderer::setDofBlur(m_settings.dofBlur);
    GsGpuRenderer::setDofZFar(m_settings.dofZFar);
    { extern void ps2xSetForceBilinear(bool); ps2xSetForceBilinear(m_settings.forceBilinear); }
    pushHudLayout(m_settings);
    applyDeadzone();

    // [paddev] The Device combo is applied per player from the Controllers tab (applyDeviceToPlayer); it used
    // to be applied to EVERY player here -- picking a pad for P2 rebound P1 as well, and each boot re-applied
    // the single ini index over pad_pN.conf. At startup only P1 follows the ini's legacy `device` index.

    // Dump current settings whenever they're applied.
    dumpSettingsToFile();
}

void PS2SettingsOverlay::buildDeviceList()
{
    m_deviceList.clear();

    // 0: Auto (Any)
    m_deviceList.push_back({"Auto (gamepads in order, keyboard fallback)", -1, false, ps2_stubs::PadDeviceKind::None});

    // 1: Keyboard
    m_deviceList.push_back({"Keyboard", -1, false, ps2_stubs::PadDeviceKind::Keyboard});

    // 2+: host gamepad slots
    for (int g = 0; g < ps2x_pad::kMaxSlots; ++g)
    {
        if (!ps2x_pad::available(g))
            continue;

        const char *name = ps2x_pad::name(g);
        std::string devName = (name && name[0]) ? name : ("Gamepad slot " + std::to_string(g));

#if defined(__linux__)
        bool evdevMatch = false;
        auto &native = ps2_stubs::PadEvdevLinux::instance();
        if (native.isAvailable() && native.matchesName(name))
        {
            evdevMatch = true;
            devName += " (" + native.node() + ")";
        }
        m_deviceList.push_back({devName, g, evdevMatch, ps2_stubs::PadDeviceKind::Gamepad});
#else
        m_deviceList.push_back({devName, g, false, ps2_stubs::PadDeviceKind::Gamepad});
#endif
    }

#if defined(__linux__)
    // If evdev is available but doesn't match any GLFW slot, add it separately
    auto &native = ps2_stubs::PadEvdevLinux::instance();
    if (native.isAvailable())
    {
        bool found = false;
        for (auto &d : m_deviceList)
        {
            if (d.isEvdev) { found = true; break; }
        }
        if (!found)
        {
            std::string evdevName = native.name() + " (" + native.node() + ")";
            m_deviceList.push_back({evdevName, -1, true, ps2_stubs::PadDeviceKind::Gamepad});
        }
    }
#endif

    // Clamp selection
    if (m_selectedDevice < 0 || m_selectedDevice >= static_cast<int>(m_deviceList.size()))
        m_selectedDevice = 0;
}

int PS2SettingsOverlay::deviceIndexForPlayer(int player) const
{   // [paddev]
    if (player < 0 || player >= (int)ps2_stubs::PadConfig::kPlayerCount) return 0;
    const auto cfg = ps2_stubs::PadConfig::instance().snapshot((size_t)player);
    if (cfg.device.kind == ps2_stubs::PadDeviceKind::None) return 0;
    for (int i = 0; i < (int)m_deviceList.size(); ++i)
    {
        const auto &d = m_deviceList[i];
        if (d.kind != cfg.device.kind) continue;
        if (d.kind == ps2_stubs::PadDeviceKind::Keyboard) return i;
        if (ps2_stubs::padGamepadIndex(d.glfwSlot) == cfg.device.gamepad) return i;
    }
    return 0;   // assigned pad not present right now: show Auto rather than someone else's device
}

void PS2SettingsOverlay::applyDeviceToPlayer(int player, int devIdx)
{   // [paddev]
    if (player < 0 || player >= (int)ps2_stubs::PadConfig::kPlayerCount) return;
    if (devIdx < 0 || devIdx >= (int)m_deviceList.size()) return;
    const auto &dev = m_deviceList[devIdx];
    auto &pcfg = ps2_stubs::PadConfig::instance();
    const size_t p = (size_t)player;
    const auto cfg = pcfg.snapshot(p);
    // Persist the launcher's index convention ("Gamepad N"), never the raw slot: the slot layout != what
    // pad_pN.conf names, and a bare slot made pad_pN.conf point at a dead controller.
    const int idx = ps2_stubs::padGamepadIndex(dev.glfwSlot);
    if (dev.kind == ps2_stubs::PadDeviceKind::Gamepad && idx < 0) return;   // slot not (yet) a controller
    if (cfg.device.kind != dev.kind)
    {   // [padbinds] a different KIND of device gets that kind's default bindings
        pcfg.setPlayerDefaults(p, dev.kind);
        pcfg.setDevice(p, ps2_stubs::PadDevice{dev.kind, idx});
    }
    else if (cfg.device.gamepad != idx)
        pcfg.setDevice(p, ps2_stubs::PadDevice{dev.kind, idx});
}

void PS2SettingsOverlay::readGamepadStateForDevice(
    const DeviceInfo &dev,
    std::array<uint8_t, 32> &btnDown,
    std::array<float, 6> &axis)
{
    btnDown = {};
    axis = {};

    if (dev.kind == ps2_stubs::PadDeviceKind::None)
    {
        // Auto: merge all host gamepads + evdev
        for (int g = 0; g < ps2x_pad::kMaxSlots; ++g)
        {
            if (!ps2x_pad::available(g))
                continue;
            for (int b = 0; b < 32; ++b)
                if (ps2x_pad::buttonDown(g, b))
                    btnDown[b] = 1;
            for (int a = 0; a < 6; ++a)
            {
                float v = ps2x_pad::axis(g, a);
                if (std::fabs(v) > std::fabs(axis[a]))
                    axis[a] = v;
            }
        }
#if defined(__linux__)
        auto &native = ps2_stubs::PadEvdevLinux::instance();
        if (native.isAvailable())
        {
            for (int b = 0; b < 32; ++b)
                if (native.isButtonDown(b))
                    btnDown[b] = 1;
            for (int a = 0; a < 6; ++a)
            {
                float v = native.getRawAxis(a);
                if (std::fabs(v) > std::fabs(axis[a]))
                    axis[a] = v;
            }
        }
#endif
    }
    else if (dev.kind == ps2_stubs::PadDeviceKind::Gamepad)
    {
        // Read from the specific host slot
        if (dev.glfwSlot >= 0 && ps2x_pad::available(dev.glfwSlot))
        {
            for (int b = 0; b < 32; ++b)
                if (ps2x_pad::buttonDown(dev.glfwSlot, b))
                    btnDown[b] = 1;
            for (int a = 0; a < 6; ++a)
                axis[a] = ps2x_pad::axis(dev.glfwSlot, a);
        }
        // Also read from evdev if it matches
        if (dev.isEvdev)
        {
#if defined(__linux__)
            auto &native = ps2_stubs::PadEvdevLinux::instance();
            if (native.isAvailable())
            {
                for (int b = 0; b < 32; ++b)
                    if (native.isButtonDown(b))
                        btnDown[b] = 1;
                for (int a = 0; a < 6; ++a)
                {
                    float v = native.getRawAxis(a);
                    if (std::fabs(v) > std::fabs(axis[a]))
                        axis[a] = v;
                }
            }
#endif
        }
    }
    // Keyboard: no gamepad axes/buttons to read
}

void PS2SettingsOverlay::draw(PS2Runtime &runtime)
{
    if (!m_initialized)
        return;

    // [launcher] In-game overlay master switch: when disabled the panel never
    // deploys (toggle combos are also skipped below via m_visible staying false),
    // so a play-session config can keep the HUD out entirely.
    if (!m_settings.overlayEnabled)
        return;

    if (std::getenv("PS2X_COMBO_DIAG") && m_selectedDevice >= 0 &&
        m_selectedDevice < static_cast<int>(m_deviceList.size()))
    {
        static int s_lastDev = -2;
        if (s_lastDev != m_selectedDevice)
        {
            s_lastDev = m_selectedDevice;
            std::fprintf(stderr, "[combo] selectedDevice=%d kind=%d glfwSlot=%d evdev=%d name=%s\n",
                         m_selectedDevice, (int)m_deviceList[m_selectedDevice].kind,
                         m_deviceList[m_selectedDevice].glfwSlot,
                         m_deviceList[m_selectedDevice].isEvdev ? 1 : 0,
                         m_deviceList[m_selectedDevice].name.c_str());
        }
    }

    // --- Toggle: keyboard combo (configurable; all bound keys held + edge on last) ---
    if (!m_settings.overlayKeys.empty())
    {
        bool allDown = true;
        for (int k : m_settings.overlayKeys)
            if (!bt3IsKeyDown(k)) { allDown = false; break; }
        const int lastKey = m_settings.overlayKeys.back();
        if (allDown && bt3IsKeyPressed(lastKey))
            toggleVisible();
    }

    // --- F11: toggle fullscreen / windowed ---
    if (bt3IsKeyPressed(BT3_KEY_F11))
    {
        m_settings.fullscreen = !m_settings.fullscreen;
        ps2xSetFullscreen(m_settings.fullscreen, m_settings.windowW, m_settings.windowH, m_settings.monitor);
        m_settings.windowMode = m_settings.fullscreen ? 2 : 0;   // keep the mode in sync with the toggle
        m_dirty = true;
    }

    // --- Toggle: gamepad combo (configurable; all bound buttons held) ---
    {
        bool comboDown = false;
        if (!m_settings.overlayPadBtns.empty())
        {
            std::array<uint8_t, 32> cb;
            std::array<float, 6> ca;
            if (m_selectedDevice >= 0 && m_selectedDevice < static_cast<int>(m_deviceList.size()))
                readGamepadStateForDevice(m_deviceList[m_selectedDevice], cb, ca);
            else
                cb = {};
            comboDown = true;
            for (int b : m_settings.overlayPadBtns)
                if (b < 0 || b >= 32 || !cb[b]) { comboDown = false; break; }
        }
        if (comboDown && !m_prevToggleCombo)
            toggleVisible();
        m_prevToggleCombo = comboDown;
    }

    // [noapply] Every change applies in full the moment it is made (this used to apply only volume /
    // renderer / glow here and leave the rest to an Apply button, so some switches worked instantly and
    // others waited). Settings are written to the ini when the overlay closes and at shutdown; the
    // startup-only items (renderer, glow fix, OpenGL render scale) say so next to their controls.
    if (m_dirty)
    {
        applySettings();
        m_dirty = false;
    }

    // --- Deploy / retract animation ------------------------------------------
    // m_animT eases toward 1 (open) or 0 (closed) every frame instead of snapping,
    // so closing still needs a few frames to fade + slide away even though m_visible
    // has already flipped to false below. Keep advancing it even while !m_visible so
    // the retract animation can finish; only bail once it's fully settled at 0.
    {
        const float dt = ImGui::GetIO().DeltaTime;
        const float target = m_visible ? 1.0f : 0.0f;
        const float step = dt > 0.0f ? (dt / kOverlayAnimDuration) : 1.0f;
        if (m_animT < target)
            m_animT = std::min(target, m_animT + step);
        else if (m_animT > target)
            m_animT = std::max(target, m_animT - step);
    }

    if (m_animT <= 0.0001f)
        return;

    const float animEase = overlayAnimEase(m_animT);

    // --- Draw overlay ---
    // Capture the pre-frame visibility so we can detect a true->false transition caused
    // by the Close button or the title-bar X (both set *p_open directly, bypassing
    // toggleVisible()). Must be read BEFORE the button handlers can flip m_visible.
    const bool wasVisible = m_visible;
    try
    {
        ps2x::gfx::UiBegin();

        pushDbzTheme();
        DbzThemeScope dbzTheme;   // pops all 40 style colours on scope exit

        // Fade the whole window (and the bindings popup, if open) in/out with the
        // deploy animation.
        ScopedStyleVar animAlpha(ImGuiStyleVar_Alpha, animEase);

        // [fitscale] Clamp the panel to the CURRENT viewport: the logical screen can be
        // anything from the 640x448 classic window to a fullscreen stretch of it, and a
        // fixed 1080px panel hangs off-screen there (user report).
        const float vpW = ImGui::GetMainViewport()->Size.x;
        const float panelW = std::min(1080.0f, std::max(320.0f, vpW * 0.96f));
        const ImVec2 winSize(panelW, 0);
        ImGui::SetNextWindowSize(winSize, ImGuiCond_Always);
        {
            const ImVec2 vp = ImGui::GetMainViewport()->Size;
            const float maxH = std::max(180.0f, vp.y * 0.92f);
            ImGui::SetNextWindowSizeConstraints(ImVec2(panelW, 180), ImVec2(panelW, maxH));
        }

        // Slide the panel in from just above centre as it deploys (and back up as it
        // retracts) instead of popping in place — reads like the HUD "powering up"
        // rather than an abrupt toggle.
        ImVec2 pos = ImGui::GetMainViewport()->GetCenter();
        pos.y += (1.0f - animEase) * -26.0f;
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

        ImGui::Begin("DBZ BT3 // SETTINGS##overlay", &m_visible,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);

        // Ignore clicks while the panel is only mid-retract (fading/sliding out after
        // Close/X was already pressed) so a stray click can't land on a control that's
        // about to disappear.
        ImGui::BeginDisabled(!m_visible);

        // --- Capsule HUD corner brackets ---
        {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            if (dl)
            {
                const ImVec2 wMin = ImGui::GetWindowPos();
                const ImVec2 wMax = ImVec2(wMin.x + ImGui::GetWindowWidth(), wMin.y + ImGui::GetWindowHeight());
                const float bl = 14.0f;
                const ImU32 bracketCol = IM_COL32(255, 158, 26, 200);
                dl->AddLine(ImVec2(wMin.x + 6, wMin.y + 6), ImVec2(wMin.x + 6 + bl, wMin.y + 6), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMin.x + 6, wMin.y + 6), ImVec2(wMin.x + 6, wMin.y + 6 + bl), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMax.x - 6, wMin.y + 6), ImVec2(wMax.x - 6 - bl, wMin.y + 6), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMax.x - 6, wMin.y + 6), ImVec2(wMax.x - 6, wMin.y + 6 + bl), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMin.x + 6, wMax.y - 6), ImVec2(wMin.x + 6 + bl, wMax.y - 6), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMin.x + 6, wMax.y - 6), ImVec2(wMin.x + 6, wMax.y - 6 - bl), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMax.x - 6, wMax.y - 6), ImVec2(wMax.x - 6 - bl, wMax.y - 6), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMax.x - 6, wMax.y - 6), ImVec2(wMax.x - 6, wMax.y - 6 - bl), bracketCol, 2.0f);
            }
        }
        ImGui::Spacing();

        // --- Tabs ---
        // BeginTabItem draws its header label immediately (using whatever font is
        // bound at that call); the tab's *content* is only drawn afterwards, once we
        // call drawXTab(). So the HUD font is pushed/popped tightly around each
        // BeginTabItem call only — section headers inside each tab push it again
        // themselves (see sectionHeader()) — leaving the body text in the default font.
        {
            ScopedStyleVar v(ImGuiStyleVar_TabBorderSize, 0.0f);
            if (ImGui::BeginTabBar("SettingsTabs"))
            {
                if (ImGui::BeginTabItem("  Audio"))
                {
                    m_activeTab = 0;
                    drawAudioTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("  Video"))
                {
                    m_activeTab = 1;
                    drawVideoTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("  Controllers"))
                {
                    m_activeTab = 2;
                    drawControllersTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("  Netplay"))
                {
                    m_activeTab = 4;
                    drawNetplayTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("  Logging"))
                {
                    m_activeTab = 3;
                    drawLoggingTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("  About"))
                {
                    m_activeTab = 4;
                    drawAboutTab();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }

        // --- Footer ---
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        {
            const char *hint = "Shift+Tab / Select+Start to toggle";
            float tw = ImGui::CalcTextSize(hint).x;
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() / 2.0f - tw / 2.0f - 8);
            {
                ScopedStyleColor c(ImGuiCol_Text, dbz(0.6f, 0.6f, 0.7f));
                ImGui::TextUnformatted(hint);
            }

            // [noapply] no Apply / Close buttons: changes apply as they are made, the hotkey closes and saves
        }

        // Bindings / Overlay-settings sub-window (opened from the Controllers tab).
        // OpenPopup must be called in the same ID scope as BeginPopupModal (after the
        // tab bar) — calling it inside the tab item would prefix the popup ID with the
        // tab's own ID, so BeginPopupModal could never find it.
        if (m_showBindingsPopup && !ImGui::IsPopupOpen("Controller Bindings"))
            ImGui::OpenPopup("Controller Bindings");
        drawBindingsPopup();

        ImGui::EndDisabled();

        // Detect closing via the Close button or the title-bar X: ImGui sets *p_open
        // (m_visible) to false directly. That path doesn't go through toggleVisible(),
        // so the input-suspended flag would stay true and the game would never capture
        // the controller again. Release it whenever m_visible transitions true -> false.
        ImGui::End();
        if (wasVisible && !m_visible)
            ps2_stubs::PadConfig::setInputSuspended(false);
    }
    catch (...)
    {
        // Never let an overlay rendering fault kill the whole game.
        m_visible = false;
    }
    ps2x::gfx::UiEnd();
}

void PS2SettingsOverlay::drawAudioTab()
{
    ImGui::Spacing();

    auto volumeSlider = [&](const char *label, float *v)
    {
        char buf[16];
        snprintf(buf, sizeof buf, "%.0f%%", (*v) * 100.0f);
        ImGui::TextUnformatted(label);
        ImGui::SameLine(120);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70.0f);
        if (ImGui::SliderFloat(("##" + std::string(label)).c_str(), v, 0.0f, 1.0f, buf,
                            ImGuiSliderFlags_NoRoundToFormat))
            m_dirty = true;
    };

    sectionHeader("MASTER VOLUME");
    volumeSlider("Master", &m_settings.masterVolume);
    ImGui::TextDisabled("Global output volume.");

    sectionHeader("MIXER");
    volumeSlider("Music", &m_settings.musicVolume);
    volumeSlider("SFX", &m_settings.sfxVolume);
    ImGui::TextDisabled("Music = BGM streams. SFX = voices, effects and one-shots.");

    ImGui::Spacing();
}

void PS2SettingsOverlay::drawVideoTab()
{
    ImGui::Spacing();

    // Renderer + Effects (flat, compact — no card borders)
    // [video] STATUS: what is ACTUALLY running (see runtime/ps2_video_status.h). Green = as configured,
    // amber = running but downgraded (another renderer, a clamped monitor/resolution, or a change that
    // needs a restart), red = unavailable. The launcher shows the same four rows as its summary.
    {
        sectionHeader("STATUS");
        const ps2x::VideoStatus vs = ps2x::GetVideoStatus();
        auto dot = [](ps2x::VideoState st, const char *label, const char *value, const char *note)
        {
            const ImVec4 col = st == ps2x::VideoState::Ok       ? ImVec4(0.25f, 0.73f, 0.31f, 1.0f)
                             : st == ps2x::VideoState::Fallback ? ImVec4(0.82f, 0.60f, 0.13f, 1.0f)
                                                                : ImVec4(0.97f, 0.32f, 0.29f, 1.0f);
            ImGui::TextColored(col, "*");   // filled dot; ASCII so it never depends on the font's glyphs
            ImGui::SameLine(0.0f, 8.0f);
            ImGui::Text("%-11s %-18s", label, value);
            ImGui::SameLine(0.0f, 8.0f);
            ImGui::TextDisabled("%s", note);
        };
        char val[128], note[128];
        dot(vs.renderer, "Renderer", vs.rendererName,
            vs.renderer == ps2x::VideoState::Fallback ? "fell back to another renderer"
          : vs.renderer == ps2x::VideoState::Ok       ? "present ok" : "no present");
        std::snprintf(val, sizeof val, "Monitor %d - %s", vs.monitorIndex + 1, vs.monitorName);
        std::snprintf(note, sizeof note, "%dx%d @%dHz%s", vs.monitorWidth, vs.monitorHeight, vs.monitorRefresh,
                      vs.monitorRequested != vs.monitorIndex ? "  (requested monitor missing: clamped)" : "");
        dot(vs.monitor, "Monitor", val, note);
        std::snprintf(val, sizeof val, "%dx%d", vs.winW, vs.winH);
        dot(vs.resolution, "Resolution", val, vs.resolution == ps2x::VideoState::Ok ? "matches the configured window"
                                                                                    : "does not match (clamped or custom)");
        std::snprintf(val, sizeof val, "x%d", vs.scaleActive);
        dot(vs.upscale, "Upscale", val,
            vs.upscale == ps2x::VideoState::Ok ? "active"
          : vs.scaleNeedsRestart               ? "applies on restart"
                                               : "not available (software renderer)");
    }

    // [display] Display settings live in a popup so the tab stays short. Apply = live only; Save = live
    // and persisted (m_dirty, the overlay writes on close); Reset = back to the values it opened with;
    // Close = discard. Advanced settings (the effect toggles) follows the same pattern next.
    {
        static const int kW[] = {1024, 1280, 1360, 1366, 1440, 1600, 1920, 2560, 3440, 3840};
        static const int kH[] = { 768,  720,  768,  768,  900,  900, 1080, 1440, 1440, 2160};
        static const char *const kRes[] = {"1024 x 768", "1280 x 720", "1360 x 768", "1366 x 768", "1440 x 900",
                                           "1600 x 900", "1920 x 1080", "2560 x 1440", "3440 x 1440", "3840 x 2160"};
        if (ImGui::Button("Display settings...", ImVec2(200.0f, 0.0f))) ImGui::OpenPopup("Display settings");
        // [visualfx] The effect toggles (renderer + filtering) live in this popup so the tab stays short (the rest of the
        // effects follow the same pattern). Apply = live where the runtime has a hook; Save = + persist
        // (m_dirty, written when the overlay closes); Reset = back to the values it opened with; Close =
        // discard.
        ImGui::SameLine();
        if (ImGui::Button("Visual Effects...", ImVec2(200.0f, 0.0f))) ImGui::OpenPopup("Visual Effects");
        // [texui] Pack status + options live in their own popup (same pattern as the two above).
        ImGui::SameLine();
        if (ImGui::Button("Texture Replacement...", ImVec2(200.0f, 0.0f))) ImGui::OpenPopup("Texture Replacement");
        static bool *const kAdvB[] = {
            &m_settings.bilinear, &m_settings.forceBilinear, &m_settings.halfTexel,
            &m_settings.skipPost, &m_settings.skipStaleVram,
        };
        static const char *const kAdvN[] = {
            "Bilinear Filter", "Force Filtering (smooth terrain)", "Half-texel",
            "Skip post pass", "Skip stale VRAM",
        };
        static bool sAdvOpen = false, sB[5];
        if (ImGui::BeginPopupModal("Visual Effects", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (!sAdvOpen)
            {
                for (int i = 0; i < 5; ++i) sB[i] = *kAdvB[i];
                sAdvOpen = true;
            }
            for (int i = 0; i < 5; ++i) ImGui::Checkbox(kAdvN[i], kAdvB[i]);
        if (toggleSwitch("Cel Outline", &m_settings.outline))
            m_dirty = true;
        if (m_settings.outline)
        {   // the ink controls belong to the outline: shown under it, only while it is on
            ImGui::Indent(12.0f);
            // [inkstrength] how hard the outline darkener subtracts. 199% is the exact GS
            // strength (it divides Ad by 128 where GL divides by 255); 100% is the old,
            // washed-out line. Applies live -- it is a single shader uniform.
            ImGui::Text("Ink Strength");
            ImGui::SameLine(120);
            ImGui::SetNextItemWidth(220);
            if (ImGui::SliderInt("##inkstrength", &m_settings.inkStrength, 100, 400, "%d %%",
                                 ImGuiSliderFlags_AlwaysClamp))
            {
                GsGpuRenderer::setInkStrengthPct(m_settings.inkStrength);   // live preview
                m_dirty = true;
            }
            ImGui::TextDisabled("199%% matches the console line. Higher = darker ink.");
            if (m_settings.renderer == 2)
            {   // [pgsink] paraLLEl-GS: the stroke width is the outline chain's edge-detect shift, rewritten in the stream
                ImGui::Text("Ink Width");
                ImGui::SameLine(120);
                ImGui::SetNextItemWidth(220);
                if (ImGui::SliderInt("##inkwidth", &m_settings.inkWidth, 25, 100, "%d %%", ImGuiSliderFlags_AlwaysClamp))
                {
                    ps2x_pgs::setInkWidthPct(m_settings.inkWidth);   // live
                    m_dirty = true;
                }
                ImGui::TextDisabled("100%% = the console's one-pixel stroke; lower = thinner (paraLLEl-GS only).");
                {   // [pgsink] the darkener subtracts its colour from the scene, so the picker sets the complement it keeps
                    float rgb[3] = { ((m_settings.inkColor >> 16) & 0xFFu) / 255.0f, ((m_settings.inkColor >> 8) & 0xFFu) / 255.0f, (m_settings.inkColor & 0xFFu) / 255.0f };
                    ImGui::Text("Ink Color");
                    ImGui::SameLine(120);
                    ImGui::SetNextItemWidth(220);
                    if (ImGui::ColorEdit3("##inkcolor", rgb, ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_Uint8))
                    {
                        auto b = [](float f) { return static_cast<unsigned>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); };
                        m_settings.inkColor = (b(rgb[0]) << 16) | (b(rgb[1]) << 8) | b(rgb[2]);
                        ps2x_pgs::setInkColor(m_settings.inkColor);   // live
                        m_dirty = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Black")) { m_settings.inkColor = 0; ps2x_pgs::setInkColor(0); m_dirty = true; }
                    ImGui::TextDisabled("Exact on light backgrounds; darker scenes tint toward it (paraLLEl-GS only).");
                }
            }
            ImGui::Unindent(12.0f);
        }
        // [texui] The Texture Replacement toggle moved into its own popup (see below).
        if (toggleSwitch("60 FPS (experimental)", &m_settings.fps60))
        {   // [fps60] step 1 + the pacing table; the runtime applies it between fights, never mid-fight
            ps2Set60Fps(m_settings.fps60, nullptr);
            m_dirty = true;
        }
        if (toggleSwitch("Character Shadows", &m_settings.shadows))
            m_dirty = true;
        if (toggleSwitch("Depth-of-Field Blur", &m_settings.dofBlur))
            m_dirty = true;
        if (m_settings.dofBlur)
        {
            ImGui::Text("Blur Reach");
            ImGui::SameLine(120);
            ImGui::SetNextItemWidth(220);
            int reach = m_settings.dofZFar / 1000;   // present in "k" units for a readable slider
            if (ImGui::SliderInt("##dofreach", &reach, 50, 400, "%d k", ImGuiSliderFlags_AlwaysClamp))
            {
                m_settings.dofZFar = reach * 1000;
                m_dirty = true;
            }
            if (m_settings.renderer == 2) ImGui::TextDisabled("paraLLEl-GS: off keeps the aura glow (the game blurs through the same pass,\nso a soft halo stays around a charging aura); reach is OpenGL-only.");
            ImGui::TextDisabled("Lower = blur reaches nearer to the camera. 200k matches the console look.");
        }
        // (Glow / Skip Post / Half-Texel / Skip Stale VRAM toggles removed: replay A/B
        //  measured them at 0.000 frame diff in fights -- their draw classes are
        //  superseded by the current serving pipeline. Env vars still work for devs.)
        {   // [glowfix] BT3's bloom/glow chain -- the Kaioken aura and every attack glow.
            const bool was = m_settings.glowFix;
            if (toggleSwitch("Glow (Kaioken aura)", &m_settings.glowFix))
                m_dirty = true;
            if (m_settings.glowFix != GsGpuRenderer::glowFixEnabled())
                ImGui::TextDisabled("(applies on restart)");
            else if (was) ImGui::TextDisabled("Character/attack bloom. Off = the pre-fix look.");
        }
    
            auto applyLive = [&]() { ps2x_pgs::setForceBilinear(m_settings.forceBilinear); };
            if (ImGui::Button("Reset")) { for (int i = 0; i < 5; ++i) *kAdvB[i] = sB[i]; applyLive(); }
            ImGui::SameLine();
            if (ImGui::Button("Close")) { sAdvOpen = false; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine(0.0f, 24.0f);
            if (ImGui::Button("Apply")) { applyLive(); m_dirty = true; }
            ImGui::SameLine();
            if (ImGui::Button("Save")) { applyLive(); m_dirty = true; sAdvOpen = false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        // [texui] Texture Replacement popup: pack status + the pack options. Opened by the
        // "Texture Replacement..." button above (same ID scope). Video overlay applies on restart
        // (the native PSS/ADX swap happens at loadELF -- see ps2_fmv_override.cpp).
        if (ImGui::BeginPopupModal("Texture Replacement", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            const bool havePack = ps2tex::replacementsEnabled();
            if (havePack)
            {
                const size_t n = ps2tex::replacementsCount();
                const bool full = ps2tex::replacementsHave3D();
                char buf[256];
                std::snprintf(buf, sizeof buf, "Installed - %zu replacements (%s)",
                              n, full ? "Full: 3D + 2D" : "Lite: 2D only");
                ImGui::TextColored(ImVec4(0.25f, 0.73f, 0.31f, 1.0f), "*");
                ImGui::SameLine(0.0f, 8.0f);
                ImGui::TextUnformatted(buf);
                ImGui::TextDisabled("%s", ps2tex::replacementsRoot());
            }
            else
            {
                ImGui::TextColored(ImVec4(0.97f, 0.32f, 0.29f, 1.0f), "*");
                ImGui::SameLine(0.0f, 8.0f);
                ImGui::TextUnformatted("No texture pack indexed");
                ImGui::TextDisabled("Install one from the launcher (Misc tab) or set PS2X_TEXREPLACE=<dir>.");
            }
            ImGui::Separator();
            if (!havePack) ImGui::BeginDisabled();
            if (toggleSwitch("Video overlay (4K intro)", &m_settings.introVideo))
                m_dirty = true;
            ImGui::TextDisabled("Video overlay replaces the opening movie; applies on restart.");
            ImGui::Spacing();
            // [texui] Button style: picks which variant folder the texture index prefers.
            ImGui::TextUnformatted("Buttons style");
            ImGui::SameLine(120);
            if (ImGui::RadioButton("PS2", &m_settings.buttonLayout, 0)) m_dirty = true;
            ImGui::SameLine();
            if (ImGui::RadioButton("Xbox", &m_settings.buttonLayout, 1)) m_dirty = true;
            ImGui::TextDisabled("Applies on restart.");
            if (!havePack) ImGui::EndDisabled();
            ImGui::Spacing();
            if (ImGui::Button("Close", ImVec2(96.0f, 0.0f))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }


        static bool eInit = false;
        static int eMode = 0, eMon = 0, eScale = 1, eRes = 0;
        // OpenPopup and BeginPopupModal must share the ID scope (same rule as the Controller Bindings popup).
        if (ImGui::BeginPopupModal("Display settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (!eInit)
            {
                eMode = m_settings.windowMode; eMon = m_settings.monitor;
                eScale = std::clamp(m_settings.renderScale, 1, 3); eRes = 0;
                for (int i = 0; i < 10; ++i) if (kW[i] == m_settings.windowW && kH[i] == m_settings.windowH) eRes = i;
                eInit = true;
            }
            ImGui::TextUnformatted("Resolution");
            ImGui::SetNextItemWidth(260.0f);
            ImGui::Combo("##res", &eRes, kRes, 10);
            ImGui::TextUnformatted("Render scale");
            for (int s = 1; s <= 3; ++s)   // radio group: only one is valid at a time
            {
                if (s > 1) ImGui::SameLine();
                char lb[8]; std::snprintf(lb, sizeof lb, "x%d", s);
                if (ImGui::RadioButton(lb, eScale == s)) eScale = s;
            }
            ImGui::TextUnformatted("Monitor");
            const int mc = std::max(1, bt3GetMonitorCount());
            char cur[192];
            std::snprintf(cur, sizeof cur, "%d - %s - %dx%d @%dHz", eMon + 1,
                          bt3GetMonitorName(eMon) ? bt3GetMonitorName(eMon) : "?",
                          bt3GetMonitorWidth(eMon), bt3GetMonitorHeight(eMon), bt3GetMonitorRefreshRate(eMon));
            if (ImGui::BeginCombo("##mon", cur))
            {
                for (int i = 0; i < mc; ++i)
                {
                    char lbl[192];
                    std::snprintf(lbl, sizeof lbl, "%d - %s - %dx%d @%dHz", i + 1,
                                  bt3GetMonitorName(i) ? bt3GetMonitorName(i) : "?",
                                  bt3GetMonitorWidth(i), bt3GetMonitorHeight(i), bt3GetMonitorRefreshRate(i));
                    if (ImGui::Selectable(lbl, i == eMon)) eMon = i;
                }
                ImGui::EndCombo();
            }
            ImGui::TextUnformatted("Window mode");
            ImGui::RadioButton("Windowed (resizable)", &eMode, 0); ImGui::SameLine();
            ImGui::RadioButton("Borderless", &eMode, 1);           ImGui::SameLine();
            ImGui::RadioButton("Fullscreen", &eMode, 2);
        if (toggleSwitch("Widescreen (true FOV)", &m_settings.widescreen))
        {
            s_widescreen = m_settings.widescreen;
            m_dirty = true;
        }
        if (m_settings.widescreen)
        {   // widescreen HUD layout: where the corrected-proportion HUD sits on the wide frame
            static const char *kHudLayouts[] = {"Centered (4:3 block)", "Edge-pinned (wide)", "Custom (sliders)"};
            ImGui::TextUnformatted("HUD Layout");
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            int hl = m_settings.hudLayout;
            if (ImGui::Combo("##hudlayout", &hl, kHudLayouts, 3))
            {
                m_settings.hudLayout = hl;
                pushHudLayout(m_settings);
                m_dirty = true;
            }
            if (m_settings.hudLayout == 2)
            {
                bool ch = false;
                ch |= ImGui::SliderInt("Left cluster", &m_settings.hudOffL, -120, 120, "%d px");
                ch |= ImGui::SliderInt("Timer", &m_settings.hudOffC, -120, 120, "%d px");
                ch |= ImGui::SliderInt("Right cluster", &m_settings.hudOffR, -120, 120, "%d px");
                if (ch) { pushHudLayout(m_settings); m_dirty = true; }
                ImGui::TextDisabled("offsets from the edge-pinned layout; bars auto-stretch");
            }
            if (m_settings.hudLayout != 0)
                ImGui::TextDisabled("note: in stretched layouts the damage flash can briefly show at both bar ends");
        }
    
            auto applyLive = [&]()
            {
                // [winmode] Fullscreen and borderless are window STATES, not flags you can just add on
                // top: switching back to windowed has to CLEAR them, otherwise the window keeps the
                // borderless chrome -- no title bar, nothing to drag, nothing to resize (that was the
                // old behaviour). Windowed = resizable + decorated; borderless = monitor-sized, no
                // chrome; fullscreen = the monitor's own mode.
                if (eMode == 2)
                {
                    bt3ClearWindowState(BT3_FLAG_BORDERLESS_WINDOWED_MODE | BT3_FLAG_WINDOW_UNDECORATED);
                    bt3SetWindowMonitor(eMon);
                    bt3SetWindowSize(kW[eRes], kH[eRes]);
                    bt3SetWindowState(BT3_FLAG_FULLSCREEN_MODE);
                }
                else if (eMode == 1)
                {
                    bt3ClearWindowState(BT3_FLAG_FULLSCREEN_MODE);
                    bt3SetWindowMonitor(eMon);
                    const int mw = bt3GetMonitorWidth(eMon), mh = bt3GetMonitorHeight(eMon);
                    if (mw >= 320 && mh >= 240) bt3SetWindowSize(mw, mh);
                    bt3SetWindowState(BT3_FLAG_BORDERLESS_WINDOWED_MODE | BT3_FLAG_WINDOW_UNDECORATED);
                }
                else
                {
                    bt3ClearWindowState(BT3_FLAG_FULLSCREEN_MODE | BT3_FLAG_BORDERLESS_WINDOWED_MODE |
                                        BT3_FLAG_WINDOW_UNDECORATED);
                    bt3SetWindowMonitor(eMon);
                    bt3SetWindowSize(kW[eRes], kH[eRes]);
                    bt3SetWindowState(BT3_FLAG_WINDOW_RESIZABLE);
                }
                if (!envUserSet("PS2X_PGS_SSAA")) ps2x_pgs::setRenderScale(eScale);   // live on paraLLEl-GS
            };
            if (ImGui::Button("Reset")) eInit = false;
            ImGui::SameLine();
            if (ImGui::Button("Close")) { eInit = false; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine(0.0f, 24.0f);
            if (ImGui::Button("Apply")) applyLive();
            ImGui::SameLine();
            if (ImGui::Button("Save"))
            {
                applyLive();
                m_settings.windowMode = eMode; m_settings.monitor = eMon;
                m_settings.renderScale = eScale;
                m_settings.windowW = kW[eRes]; m_settings.windowH = kH[eRes];
                m_settings.fullscreen = (eMode == 2);
                m_dirty = true;   // persisted when the overlay closes
                eInit = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    sectionHeader("RENDERER");
    {   // [renderer] backend dropdown
        static const char *const kLabels[] = { "OpenGL (New)", "Software rasterizer",
#if defined(PS2X_HAVE_PGS)
            "paraLLEl-GS (Vulkan compute)",
#endif
        };
        static const int kValues[] = { 0, 1,
#if defined(PS2X_HAVE_PGS)
            2,
#endif
        };
        const int nRenderers = (int)(sizeof(kValues) / sizeof(kValues[0]));
        int cur = 0;
        for (int i = 0; i < nRenderers; ++i) if (kValues[i] == m_settings.renderer) { cur = i; break; }
        ImGui::TextUnformatted("Renderer");
        ImGui::SameLine(180.0f);
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::Combo("##renderer", &cur, kLabels, nRenderers))
        {
            m_settings.renderer = kValues[cur];
            m_settings.gpuRenderer = (m_settings.renderer != Settings::kRendererSoftware);
            m_dirty = true;
        }
    }
    ImGui::TextDisabled("Takes full effect after restart.");
}

void PS2SettingsOverlay::drawControllersTab()
{
    ImGui::Spacing();

    // [padui] Same shape as the Video tab: a compact STATUS summary with dots, the actions in popups,
    // and the live test inline (it is what you watch while binding). Apply = live only; Save = live and
    // persisted; Reset = back to the values it opened with; Close = discard.
    {
        sectionHeader("STATUS");
        int pads = 0;
        for (int g = 0; g < ps2x_pad::kMaxSlots; ++g)
            if (ps2x_pad::available(g)) ++pads;
        const bool haveDev = !m_deviceList.empty() && m_selectedDevice >= 0 && m_selectedDevice < (int)m_deviceList.size();
        const char *devName = haveDev ? m_deviceList[m_selectedDevice].name.c_str() : "Auto (keyboard)";
        const ImVec4 col = pads > 0 ? ImVec4(0.25f, 0.73f, 0.31f, 1.0f)   // green: a gamepad is being read
                                    : ImVec4(0.82f, 0.60f, 0.13f, 1.0f);  // amber: keyboard fallback
        auto dot = [&](const char *label, const char *value, const char *note)
        {
            ImGui::TextColored(col, "*");
            ImGui::SameLine(0.0f, 8.0f);
            ImGui::Text("%-11s %-18s", label, value);
            ImGui::SameLine(0.0f, 8.0f);
            ImGui::TextDisabled("%s", note);
        };
        char val[128], note[256];
        std::snprintf(val, sizeof val, "P%d", m_editPlayer + 1);
        dot("Player", val, m_editPlayer == 0 ? "first gamepad, else the keyboard" : "second gamepad, else the keyboard");
        std::snprintf(val, sizeof val, "%.0f%%", m_settings.deadzone * 100.0f);
        std::snprintf(note, sizeof note, "%s | %s", devName,
                      pads > 0 ? "gamepad detected" : "no gamepad: keyboard fallback");
        dot("Deadzone", val, note);

        ImGui::Spacing();
        if (ImGui::Button("Player & Device...", ImVec2(200.0f, 0.0f))) ImGui::OpenPopup("Player & Device");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, dbz(0.20f, 0.17f, 0.12f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accent());
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, dbz(0.85f, 0.55f, 0.15f));
        if (ImGui::Button("Button Bindings...", ImVec2(200.0f, 0.0f))) m_showBindingsPopup = true;
        ImGui::PopStyleColor(3);

        static bool pInit = false;
        static int  pPlayer = 0, pDev = 0;
        static float pDz = 0.12f;
        // OpenPopup and BeginPopupModal must share the ID scope (same rule as the other popups).
        if (ImGui::BeginPopupModal("Player & Device", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (!pInit)
            {
                pPlayer = m_editPlayer; pDev = m_selectedDevice; pDz = m_settings.deadzone;
                pInit = true;
            }
            ImGui::TextUnformatted("Player");
            ImGui::RadioButton("P1", &pPlayer, 0); ImGui::SameLine();
            ImGui::RadioButton("P2", &pPlayer, 1);
            ImGui::TextUnformatted("Device");
            ImGui::SetNextItemWidth(360.0f);
            std::vector<const char *> labels;
            labels.reserve(m_deviceList.size());
            for (auto &d : m_deviceList) labels.push_back(d.name.c_str());
            if (labels.empty()) ImGui::TextDisabled("No devices detected.");
            else ImGui::Combo("##pdev", &pDev, labels.data(), (int)labels.size());
            ImGui::TextUnformatted("Deadzone");
            ImGui::SetNextItemWidth(260.0f);
            ImGui::SliderFloat("##pdz", &pDz, 0.0f, 0.5f, "%.2f");
            ImGui::SameLine();
            ImGui::TextDisabled("(%.0f%%)", pDz * 100.0f);
            ImGui::TextDisabled("Auto uses the first gamepad, or the keyboard if none is plugged in.");
            auto applyLive = [&]()
            {
                m_editPlayer = pPlayer;
                m_selectedDevice = pDev;
                if (pDev >= 0 && pDev < (int)m_deviceList.size()) applyDeviceToPlayer(pPlayer, pDev);
                m_settings.deadzone = pDz;
                applyDeadzone();
            };
            if (ImGui::Button("Reset")) pInit = false;
            ImGui::SameLine();
            if (ImGui::Button("Close")) { pInit = false; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine(0.0f, 24.0f);
            if (ImGui::Button("Apply")) { applyLive(); m_dirty = true; }
            ImGui::SameLine();
            if (ImGui::Button("Save")) { applyLive(); saveSettings(); m_dirty = true; pInit = false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
    }

    // --- Capture logic ---
    // ALWAYS read gamepad state and update edge tracking (independent of UI visibility)
    std::array<uint8_t, 32> curBtnDown;
    std::array<float, 6> curAxis;
    {
        if (m_selectedDevice >= 0 && m_selectedDevice < static_cast<int>(m_deviceList.size()))
            readGamepadStateForDevice(m_deviceList[m_selectedDevice], curBtnDown, curAxis);
        else
        {
            curBtnDown = {};
            curAxis = {};
        }

        const bool capturingAction = m_captureAction >= 0 && m_captureAction < static_cast<int>(ps2_stubs::PadAction::Count);
        const bool capturingCombo = m_captureComboSlot >= 0;
        if (capturingAction)
        {
            if (m_captureWaitRelease)
            {
                bool anyDown = false;
                for (int b = 0; b < 32 && !anyDown; ++b)
                    if (curBtnDown[b]) anyDown = true;
                for (int a = 0; a < 6 && !anyDown; ++a)
                    if (std::fabs(curAxis[a]) > 0.3f) anyDown = true;
                for (int k = 32; k <= 348 && !anyDown; ++k)
                    if (bt3IsKeyDown(k)) anyDown = true;
                if (!anyDown)
                    m_captureWaitRelease = false;
            }
            else
            {
                ps2_stubs::PadBind bind;
                int capturedKey = -1;

                // 1. Keyboard
                for (int k = 32; k <= 348 && bind.kind == ps2_stubs::PadBindKind::None; ++k)
                    if (bt3IsKeyPressed(k))
                    {
                        bind = ps2_stubs::PadBind{ps2_stubs::PadBindKind::Key, k, 1.0f, m_settings.deadzone};
                        capturedKey = k;
                    }

                // 2. Gamepad buttons — edge
                for (int b = 0; b < 32 && bind.kind == ps2_stubs::PadBindKind::None; ++b)
                    if (curBtnDown[b] && !m_prevBtnDown[b])
                        bind = ps2_stubs::PadBind{ps2_stubs::PadBindKind::Button, b, 1.0f, m_settings.deadzone};

                // 3. Gamepad axes — edge
                for (int a = 0; a < 6 && bind.kind == ps2_stubs::PadBindKind::None; ++a)
                    if (std::fabs(curAxis[a]) > 0.5f && std::fabs(m_prevAxis[a]) <= 0.5f)
                        bind = ps2_stubs::PadBind{ps2_stubs::PadBindKind::Axis, a, curAxis[a] > 0 ? 1.0f : -1.0f, m_settings.deadzone};

                if (bind.kind != ps2_stubs::PadBindKind::None)
                {
                    auto &pcfg = ps2_stubs::PadConfig::instance();
                    const auto act = static_cast<ps2_stubs::PadAction>(m_captureAction);
                    pcfg.setBind(m_editPlayer, act, bind);
                    resetCaptureState();
                }
            }
        }
        else if (capturingCombo)
        {
            // 3-second capture window: record every input held during it, then save.
            const float dt = ImGui::GetIO().DeltaTime;
            m_captureComboTimer -= dt;

            if (m_captureComboSlot == 0)
            {
                // Gamepad: gather buttons currently held.
                for (int b = 0; b < 32; ++b)
                    if (curBtnDown[b])
                    {
                        bool present = false;
                        for (int x : m_capturedBtns) if (x == b) { present = true; break; }
                        if (!present) m_capturedBtns.push_back(b);
                    }
            }
            else
            {
                // Keyboard: gather keys currently held.
                for (int k = 32; k <= 348; ++k)
                    if (bt3IsKeyDown(k))
                    {
                        bool present = false;
                        for (int x : m_capturedKeys) if (x == k) { present = true; break; }
                        if (!present) m_capturedKeys.push_back(k);
                    }
            }

            if (m_capturedBtns.size() > 0 || m_capturedKeys.size() > 0)
                m_captureComboHadAny = true;

            // Commit when the window expires or, if nothing was captured yet, once the
            // user lets go of everything (so a tap on a single button still works).
            const bool expired = (m_captureComboTimer <= 0.0f);
            const bool released = (m_captureComboSlot == 0 && !m_capturedBtns.empty())
                                ? allButtonsReleased(curBtnDown, m_capturedBtns)
                                : (m_captureComboSlot == 1 && !m_capturedKeys.empty()
                                   ? allKeysReleased(m_capturedKeys)
                                   : false);
            if (expired || (m_captureComboHadAny && released))
            {
                if (m_captureComboSlot == 0 && !m_capturedBtns.empty())
                    m_settings.overlayPadBtns = m_capturedBtns;
                else if (m_captureComboSlot == 1 && !m_capturedKeys.empty())
                    m_settings.overlayKeys = m_capturedKeys;

                m_dirty = true;
                saveSettings();
                m_captureComboSlot = -1;
                m_captureComboTimer = 0.0f;
                m_captureComboHadAny = false;
                m_capturedBtns.clear();
                m_capturedKeys.clear();
                m_captureWaitRelease = true;
            }
        }

        // ALWAYS update edge state for next frame
        m_prevBtnDown = curBtnDown;
        m_prevAxis = curAxis;
    }

    // --- Gamepad test area (always visible) ---
    sectionHeader("GAMEPAD TEST");
    drawGamepadTestArea(curBtnDown, curAxis);

    // --- Reload (re-scan devices) ---
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x / 2.0f - 50.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, dbz(0.30f, 0.30f, 0.36f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, dbz(0.40f, 0.40f, 0.46f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, dbz(0.35f, 0.35f, 0.40f));
    if (ImGui::Button("Reload devices", ImVec2(100, 30)))
        resetCaptureState();
    ImGui::PopStyleColor(3);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("Overlay Shortcuts");
    ImGui::Text("Keyboard:  Shift + Tab");
    bool anyPad = false;
    for (int g = 0; g < ps2x_pad::kMaxSlots && !anyPad; ++g)
        anyPad = ps2x_pad::available(g);
    if (anyPad)
        ImGui::Text("Gamepad:   Select + Start");
    else
        ImGui::TextDisabled("Gamepad:   (no gamepad detected)");
}

// --- Bindings table (shown inside the Bindings popup) ---
void PS2SettingsOverlay::drawBindingsTable()
{
    auto &pcfg = ps2_stubs::PadConfig::instance();
    auto cfg = pcfg.snapshot(m_editPlayer);

    if (ImGui::BeginTable("##binds", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 260)))
    {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Binding", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##btn", ImGuiTableColumnFlags_WidthFixed, 56);
        ImGui::TableSetupScrollFreeze(0, 1);
        // Capsule HUD: readout-style column headers (Russo One, when loaded).
        ImGui::TableHeadersRow();

        for (size_t a = 0; a < static_cast<size_t>(ps2_stubs::PadAction::Count); ++a)
        {
            const auto action = static_cast<ps2_stubs::PadAction>(a);
            const bool capturing = (m_captureAction == static_cast<int>(a));

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (capturing)
                ImGui::TextColored(gold(), "%s", ps2_stubs::padActionName(action));
            else
                ImGui::TextUnformatted(ps2_stubs::padActionName(action));

            ImGui::TableNextColumn();
            if (capturing)
            {
                const float a = blinkAlpha();
                ImGui::TextColored(accent(0.5f + 0.5f * a), "Press key / button / stick...");
            }
            else
                ImGui::TextUnformatted(ps2_stubs::padBindDisplay(cfg.binds[a]).c_str());

            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(a));
            if (capturing)
            {
                if (ImGui::SmallButton("Cancel"))
                    m_captureAction = -1;
            }
            else
            {
                if (ImGui::SmallButton("Bind"))
                {
                    m_captureAction = static_cast<int>(a);
                    if (m_selectedDevice >= 0 && m_selectedDevice < static_cast<int>(m_deviceList.size()))
                        readGamepadStateForDevice(m_deviceList[m_selectedDevice], m_prevBtnDown, m_prevAxis);
                    else
                    {
                        m_prevBtnDown = {};
                        m_prevAxis = {};
                    }
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// --- Overlay toggle combo (shown inside the Bindings popup) ---
void PS2SettingsOverlay::drawOverlayToggle()
{
    auto btnsLabel = [](const std::vector<int> &btns) -> std::string
    {
        if (btns.empty()) return "None";
        std::string s;
        for (size_t i = 0; i < btns.size(); ++i)
            s += (i ? " + " : "") + std::string("Button ") + std::to_string(btns[i]);
        return s;
    };
    auto keysLabel = [](const std::vector<int> &keys) -> std::string
    {
        if (keys.empty()) return "None";
        std::string s;
        for (size_t i = 0; i < keys.size(); ++i)
            s += (i ? " + " : "") + std::string("Key ") + std::to_string(keys[i]);
        return s;
    };

    auto slotRow = [&](int slot, const char *label, const std::string &display)
    {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(150);
        const bool capturing = (m_captureComboSlot == slot);
        if (capturing)
        {
            const float t = std::max(0.0f, m_captureComboTimer);
            ImGui::TextColored(accent(0.5f + 0.5f * blinkAlpha()), "Hold input... %.1fs", t);
        }
        else
        {
            ImGui::TextUnformatted(display.c_str());
        }
        ImGui::SameLine(420);
        ImGui::PushID(slot + 200);
        if (capturing)
        {
            if (ImGui::SmallButton("Cancel"))
            {
                m_captureComboSlot = -1;
                m_captureComboTimer = 0.0f;
                m_captureComboHadAny = false;
                m_capturedBtns.clear();
                m_capturedKeys.clear();
            }
        }
        else
        {
            if (ImGui::SmallButton("Bind"))
            {
                m_captureComboSlot = slot;
                m_captureComboTimer = 3.0f;   // 3-second capture window
                m_captureComboHadAny = false;
                m_capturedBtns.clear();
                m_capturedKeys.clear();
                if (m_selectedDevice >= 0 && m_selectedDevice < static_cast<int>(m_deviceList.size()))
                    readGamepadStateForDevice(m_deviceList[m_selectedDevice], m_prevBtnDown, m_prevAxis);
                else
                    m_prevBtnDown = {};
            }
        }
        ImGui::PopID();
    };

    ImGui::TextWrapped("Hold the button(s) / key(s) for 3 seconds to bind. All held inputs "
                       "must be pressed together to open or close the overlay.");
    ImGui::Spacing();
    slotRow(0, "Gamepad Launch", btnsLabel(m_settings.overlayPadBtns));
    ImGui::Spacing();
    slotRow(1, "Alt Launch (Keys)", keysLabel(m_settings.overlayKeys));
}

// --- Bindings popup modal (Bindings + Overlay Settings in tabs) ---
void PS2SettingsOverlay::drawBindingsPopup()
{
    if (!m_showBindingsPopup)
        return;

    {   // [fitscale] clamp + centre the modal inside the viewport
        const ImVec2 vp = ImGui::GetMainViewport()->Size;
        ImGui::SetNextWindowSize(ImVec2(std::min(560.0f, vp.x * 0.92f),
                                        std::min(380.0f, vp.y * 0.88f)), ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    }
    if (ImGui::BeginPopupModal("Controller Bindings", &m_showBindingsPopup,
                               ImGuiWindowFlags_NoResize))
    {
        // Capsule HUD corner brackets, matching the main panel — keeps the popup
        // reading as part of the same HUD rather than a plain generic dialog.
        {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            if (dl)
            {
                const ImVec2 wMin = ImGui::GetWindowPos();
                const ImVec2 wMax = ImVec2(wMin.x + ImGui::GetWindowWidth(), wMin.y + ImGui::GetWindowHeight());
                const float bl = 12.0f;
                const ImU32 bracketCol = IM_COL32(255, 158, 26, 170);
                dl->AddLine(ImVec2(wMin.x + 5, wMin.y + 5), ImVec2(wMin.x + 5 + bl, wMin.y + 5), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMin.x + 5, wMin.y + 5), ImVec2(wMin.x + 5, wMin.y + 5 + bl), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMax.x - 5, wMin.y + 5), ImVec2(wMax.x - 5 - bl, wMin.y + 5), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMax.x - 5, wMin.y + 5), ImVec2(wMax.x - 5, wMin.y + 5 + bl), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMin.x + 5, wMax.y - 5), ImVec2(wMin.x + 5 + bl, wMax.y - 5), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMin.x + 5, wMax.y - 5), ImVec2(wMin.x + 5, wMax.y - 5 - bl), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMax.x - 5, wMax.y - 5), ImVec2(wMax.x - 5 - bl, wMax.y - 5), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMax.x - 5, wMax.y - 5), ImVec2(wMax.x - 5, wMax.y - 5 - bl), bracketCol, 2.0f);
            }
        }

        if (ImGui::BeginTabBar("##bindtabs"))
        {
            // Same pattern as the main tab bar: push the HUD font only around the
            // tab label itself, not the tab's content.
            if (ImGui::BeginTabItem("Bindings"))
            {
                ImGui::Spacing();
                drawBindingsTable();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Overlay Settings"))
            {
                ImGui::Spacing();
                drawOverlayToggle();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        // Flat HUD buttons — dark navy body, thin orange border via the theme's
        // default Border colour, orange tint on hover/active — replacing the old
        // grey "generic dialog button" look so this matches the main panel.
        ImGui::PushStyleColor(ImGuiCol_Button, dbz(0.07f, 0.09f, 0.11f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, dbz(1.00f, 0.62f, 0.10f, 0.18f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, dbz(1.00f, 0.62f, 0.10f, 0.30f));
        if (ImGui::Button("Close", ImVec2(100, 30)))
        {   // [noapply] closing the popup saves: settings + per-action bindings (pad.conf), so bindings
            // edited here survive a restart and reach the Qt launcher's Bindings tab.
            m_showBindingsPopup = false;
            applySettings();
            saveSettings();
            ps2_stubs::PadConfig::instance().save();
        }
        ImGui::PopStyleColor(3);

        ImGui::EndPopup();
    }
}

// [netplay] Host / Join without a terminal. The peer's address is remembered in the ini --
// typing an IP on a gamepad is miserable, so recall matters more than a text field.
// Join does BOTH things the user asked for: it connects AND, on the host, kicks off the canned
// menu sequence so both sides land on character select together (see ps2NetBeginAutoStart).
void PS2SettingsOverlay::drawNetplayTab()
{
    static char s_peer[64] = "127.0.0.1";
    static int  s_port = 7777;
    static bool s_loaded = false;
    if (!s_loaded)
    {
        s_loaded = true;
        if (const char *e = std::getenv("PS2X_NET_PEER")) { std::snprintf(s_peer, sizeof s_peer, "%s", e); }
    }

    ImGui::TextUnformatted("Online play (deterministic lockstep)");
    ImGui::Separator();

    if (ps2NetActive())
    {
        ImGui::Text("Status: %s", ps2NetPeerConnected() ? "CONNECTED" : "waiting for peer...");
        ImGui::Text("You are player %d", ps2NetLocalPlayer());
        ImGui::Text("Input delay: %u frames (%u ms at 30 fps)", ps2NetDelay(), ps2NetDelay() * 33u);
        { const char *bn[] = {"Single Battle","Team Battle","DP Battle"};
          const int bt = ps2NetBattleType();
          const char *tn[] = {"60 s","90 s","180 s","240 s","no limit"};
          const int tl = ps2NetTimeLimit();
          const char *dn[] = {"10 DP","15 DP","20 DP"};
          const int dp = ps2NetDpLimit();
          ImGui::Text("Game mode: %s%s%s   |   time limit: %s",
                      (bt >= 0 && bt < 3) ? bn[bt] : "?",
                      bt == 2 ? " / " : "", (bt == 2 && dp >= 0 && dp < 3) ? dn[dp] : "",
                      (tl >= 0 && tl < 5) ? tn[tl] : "?"); }
        if (ps2NetAutoJump()) ImGui::TextUnformatted("Will jump to character select on connect.");
        // [rollback] not shipped: the connected view says nothing about it unless a developer turned it on
        // through the environment (then the line is true and worth seeing).
        if (ps2NetRollbackWindow()) ImGui::Text("Rollback window: %u frames%s", ps2NetRollbackWindow(),
                                                ps2NetSyncPending() ? "   |   state sync in progress..." : (ps2NetSyncOn() ? "   |   state synced" : ""));
        ImGui::Separator();
        if (ImGui::Button("Disconnect"))
            ps2NetDisconnect("overlay");
        ImGui::SameLine();
        ImGui::TextDisabled("restores local pads and splitscreen");
        ImGui::Separator();
        ImGui::TextWrapped("Only buttons cross the wire. Each side renders its own player "
                           "full-screen.");
        return;
    }

    static int  s_delay = 2;
    static int  s_battle = 0;
    static int  s_time = 3;
    static int  s_dp = 0;          // DP Battle budget: 0 = 10 DP, 1 = 15, 2 = 20
    static bool s_jump = true;
    // [rollback] The rollback window and state-sync controls are hidden until rollback ships (2026-09-17):
    // netplay is lockstep. The environment defaults (PS2X_NETROLLBACK / state sync) still reach the
    // connect calls below, so developers can keep testing without the UI advertising it.
    static int  s_rollback = ps2NetRollbackSetting();
    static bool s_sync = ps2NetSyncSetting();
    ImGui::Checkbox("Go to character select once connected", &s_jump);
    ImGui::TextDisabled("The HOST's choice applies to both; the menus are hidden while it happens.");
    ImGui::Separator();
    // Only Join uses the address: hosting binds the port and learns the peer from its first
    // packet, which is why only one side needs a reachable port.
    ImGui::InputText("Host address (Join only)", s_peer, sizeof s_peer);
    ImGui::InputInt("Port", &s_port);
    const char *kBattle[] = { "Single Battle", "Team Battle", "DP Battle" };
    ImGui::Combo("Game mode", &s_battle, kBattle, 3);
    // DP Battle's point budget is a SEPARATE row of the versus menu (duelObj+0x118, committed to
    // stateObj+0x630 = RetroAchievements' 0x6af7b0). Selecting DP without it left the screen
    // playing like Team Battle: the right type with no budget behind it.
    if (s_battle == 2)
    {
        const char *kDp[] = { "10 DP", "15 DP", "20 DP" };
        ImGui::Combo("DP limit", &s_dp, kDp, 3);
    }
    // Battle Settings time-limit indices, confirmed in game:
    //   0 = 60 s, 1 = 90 s, 2 = 180 s, 3 = 240 s (default), 4 = no limit
    const char *kTime[] = { "60 seconds", "90 seconds", "180 seconds", "240 seconds (default)", "No limit" };
    ImGui::Combo("Time limit", &s_time, kTime, 5);
    ImGui::TextDisabled("The HOST's choices apply to both players.");
    ImGui::SliderInt("Input delay (frames)", &s_delay, 1, 10);
    ImGui::TextDisabled("BT3 runs at 30 fps, so each frame is 33 ms. Use 1 on the same machine,");
    ImGui::TextDisabled("2 on a LAN. Raise it only if you see stalls.");
    if (s_port < 1 || s_port > 65535) s_port = 7777;

    if (ImGui::Button("Host (you are Player 1)"))
    {
        ps2NetSetAutoJump(s_jump); ps2NetSetDelay(s_delay); ps2NetSetBattleType(s_battle);
        ps2NetSetTimeLimit(s_time); ps2NetSetDpLimit(s_dp);
        ps2NetSetRollback(s_rollback); ps2NetSetSync(s_sync);
        ps2NetHost(s_port, 1);
    }
    ImGui::SameLine();
    if (ImGui::Button("Join (you are Player 2)"))
    {
        ps2NetSetAutoJump(s_jump); ps2NetSetDelay(s_delay);   // the host's game mode wins
        ps2NetSetRollback(s_rollback); ps2NetSetSync(s_sync);
        char hp[96]; std::snprintf(hp, sizeof hp, "%s:%d", s_peer, s_port);
        ps2NetJoin(hp, 2);
    }
    ImGui::Separator();
    ImGui::TextWrapped("HOST: just press Host -- leave the address blank, give the other player "
                       "your IP and this port. JOIN: type the host's IP above, then press Join. "
                       "Only the host needs the UDP port reachable.");
}

void PS2SettingsOverlay::drawLoggingTab()
{
    ImGui::Spacing();

    sectionHeader("SETTINGS DUMP LOG");

    ImGui::TextWrapped(
        "When a setting changes (Apply / Save), the current state of the enabled areas "
        "below is written to \"%s\" in the savedata folder. Enable the areas you want "
        "captured, then hit Save — from the next run everything is captured automatically.",
        kDumpFileName);
    ImGui::Spacing();

    if (ImGui::BeginChild("##logging", ImVec2(-1, 0), ImGuiChildFlags_Borders))
    {
        ImGui::Spacing();
        if (ImGui::Checkbox("Audio", &m_dumpAudio))
            m_dirty = true;
        if (ImGui::Checkbox("Video", &m_dumpVideo))
            m_dirty = true;
        if (ImGui::Checkbox("Controllers (players + bindings)", &m_dumpControllers))
            m_dirty = true;
        if (ImGui::Checkbox("Runtime (renderer state)", &m_dumpRuntime))
            m_dirty = true;
        if (ImGui::Checkbox("Live Gamepad state", &m_dumpGamepad))
            m_dirty = true;
        ImGui::Spacing();
    }
    ImGui::EndChild();

    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Button, dbz(0.15f, 0.35f, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, dbz(0.20f, 0.45f, 0.70f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, dbz(0.18f, 0.40f, 0.60f));
    if (ImGui::Button("Dump Now", ImVec2(-1, 34)))
        dumpSettingsToFile();
    ImGui::PopStyleColor(3);

    ImGui::Spacing();
    ImGui::TextDisabled("The dump file is appended with a timestamp on every write.");
}

void PS2SettingsOverlay::drawAboutTab()
{
    ImGui::Spacing();

    sectionHeader("ABOUT");
    ImGui::TextWrapped("Dragon Ball Z: Budokai Tenkaichi 3 - Recompiled");
    ImGui::TextWrapped(
        "A statically recompiled, native PC port built on PS2Recomp. The game's MIPS code "
        "is translated to C++ at build time from your own disc image; no game content is "
        "distributed with this project.");
    ImGui::Spacing();

    sectionHeader("CREDITS");
    ImGui::TextWrapped("z3xox - owner / lead developer");
    ImGui::TextDisabled("  recompiler, runtime (EE/GS/VU1/scheduler), renderer, game overrides, generators");
    ImGui::TextWrapped("RexxColder - supporter / colaborador");
    ImGui::TextDisabled("  optimizacion (perf/async), launcher + install wizard, input & gamepads, "
                        "build/release, deploy, game-data (AFS/AFL), docs");
    ImGui::TextWrapped("valenvivaldi - colaborador");
    ImGui::TextDisabled("  port macOS arm64, packaging, audio");
    ImGui::Spacing();

    sectionHeader("THIRD-PARTY");
    if (ImGui::BeginChild("##about_third", ImVec2(-1, 0), ImGuiChildFlags_Borders))
    {
        ImGui::TextWrapped("ran-j/PS2Recomp - static recompiler (upstream, GPL-3.0)");
        ImGui::TextWrapped("ViveTheModder - NTSC-U AFS file lists (Apache-2.0)");
        ImGui::TextWrapped("Arntzen Software - paraLLEl-GS (LGPL-3.0-or-later)");
        ImGui::Spacing();
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::TextDisabled("GPL-3.0. Not affiliated with Spike or Bandai Namco.");
    ImGui::TextDisabled("github.com/z3xox/BT3-Recomp");
}

void PS2SettingsOverlay::dumpSettingsToFile()
{
    const std::string dumpPath = s_configDir.empty()
        ? (std::filesystem::current_path() / kDumpFileName).string()
        : (std::filesystem::path(s_configDir) / kDumpFileName).string();

    std::ofstream file(dumpPath, std::ios::app);
    if (!file.is_open())
    {
        std::fprintf(stderr, "[dump] could not open %s\n", dumpPath.c_str());
        return;
    }

    file << "\n";
    file << "============================== " << nowTimestamp() << " ==============================\n";

    if (m_dumpAudio)
    {
        file << "[Audio]\n";
        file << "  master_volume = " << m_settings.masterVolume << "\n";
        file << "  music_volume  = " << m_settings.musicVolume << "\n";
        file << "  sfx_volume    = " << m_settings.sfxVolume << "\n";
        file << "\n";
    }

    if (m_dumpVideo)
    {
        file << "[Video]\n";
        file << "  gpu_renderer = " << (m_settings.gpuRenderer ? "1" : "0") << "\n";
        file << "  glow         = " << (m_settings.glow ? "1" : "0") << "\n";
        file << "  bilinear     = " << (m_settings.bilinear ? "1" : "0") << "\n";
        file << "  halftexel    = " << (m_settings.halfTexel ? "1" : "0") << "\n";
        file << "  skip_post    = " << (m_settings.skipPost ? "1" : "0") << "\n";
        file << "  skip_stale   = " << (m_settings.skipStaleVram ? "1" : "0") << "\n";
        file << "  render_scale = " << m_settings.renderScale << "\n";
        file << "  fullscreen   = " << (m_settings.fullscreen ? "1" : "0") << "\n";
        file << "  widescreen   = " << (m_settings.widescreen ? "1" : "0") << "\n";
        file << "\n";
    }

    if (m_dumpControllers)
    {
        file << "[Controllers]\n";
        file << "  selected_device = " << m_selectedDevice
             << " (" << (m_selectedDevice >= 0 && m_selectedDevice < (int)m_deviceList.size()
                         ? m_deviceList[m_selectedDevice].name : "?") << ")\n";
        file << "  edit_player     = " << m_editPlayer << "\n";
        file << "  deadzone        = " << m_settings.deadzone << "\n";
        file << "  overlay_gamepad = ";
        for (size_t i = 0; i < m_settings.overlayPadBtns.size(); ++i)
            file << (i ? "+" : "") << m_settings.overlayPadBtns[i];
        file << "\n";
        file << "  overlay_keys    = ";
        for (size_t i = 0; i < m_settings.overlayKeys.size(); ++i)
            file << (i ? "+" : "") << m_settings.overlayKeys[i];
        file << "\n";
        file << "\n";

        auto &pcfg = ps2_stubs::PadConfig::instance();
        for (size_t p = 0; p < ps2_stubs::PadConfig::kPlayerCount; ++p)
        {
            auto cfg = pcfg.snapshot(p);
            file << "  [Player " << (p + 1) << "]\n";
            file << "    device       = " << ps2_stubs::padDeviceDisplay(cfg.device) << "\n";
            for (size_t a = 0; a < static_cast<size_t>(ps2_stubs::PadAction::Count); ++a)
            {
                const auto act = static_cast<ps2_stubs::PadAction>(a);
                file << "    " << ps2_stubs::padActionName(act) << " = "
                     << ps2_stubs::padBindDisplay(cfg.binds[a]) << "\n";
            }
            file << "\n";
        }
    }

    if (m_dumpRuntime)
    {
        file << "[Runtime]\n";
        file << "  gpu_renderer_enabled = " << (GsGpuRenderer::enabled() ? "1" : "0") << "\n";
        file << "  glow_enabled         = " << (GsGpuRenderer::glowEnabled() ? "1" : "0") << "\n";
        file << "  bilinear_enabled     = " << (GsGpuRenderer::bilinearEnabled() ? "1" : "0") << "\n";
        file << "  halftexel_enabled    = " << (GsGpuRenderer::halfTexelEnabled() ? "1" : "0") << "\n";
        file << "  skip_post_enabled    = " << (GsGpuRenderer::skipPostEnabled() ? "1" : "0") << "\n";
        file << "  skip_stale_enabled   = " << (GsGpuRenderer::skipStaleVramEnabled() ? "1" : "0") << "\n";
        file << "  render_scale         = " << GsGpuRenderer::renderScale() << "\n";
        file << "  master_volume        = " << PS2AudioBackend::masterVolume() << "\n";
        file << "  music_volume         = " << PS2AudioBackend::musicVolume() << "\n";
        file << "  sfx_volume           = " << PS2AudioBackend::sfxVolume() << "\n";
        file << "  widescreen           = " << (PS2SettingsOverlay::isWidescreen() ? "1" : "0") << "\n";
        file << "\n";
    }

    if (m_dumpGamepad)
    {
        file << "[Gamepad Live]\n";
        std::array<uint8_t, 32> btn;
        std::array<float, 6> axis;
        if (m_selectedDevice >= 0 && m_selectedDevice < (int)m_deviceList.size())
            readGamepadStateForDevice(m_deviceList[m_selectedDevice], btn, axis);
        else
        {
            btn = {};
            axis = {};
        }
        file << "  buttons =";
        for (int b = 0; b < 32; ++b)
            if (btn[b]) file << " " << b;
        file << "\n";
        static const char *axN[6] = { "LX", "LY", "RX", "RY", "LT", "RT" };
        for (int a = 0; a < 6; ++a)
            file << "  " << axN[a] << " = " << axis[a] << "\n";
        file << "\n";
    }

    file << "========================================================================\n";
    file.close();

    std::fprintf(stderr, "[dump] settings written to %s\n", dumpPath.c_str());
}

void PS2SettingsOverlay::drawGamepadTestArea(const std::array<uint8_t, 32> &btnDown,
                                             const std::array<float, 6> &axis)
{
    // Buttons: [idx] label — matches the evdev codeToButton mapping used by the game.
    static const struct { int idx; const char *label; } kButtons[] = {
        { 13, "Select" }, { 15, "Start" }, { 14, "Guide" },
        { 1, "D-Up" },    { 3, "D-Down" }, { 4, "D-Left" }, { 2, "D-Right" },
        { 5, "Y / Tri" }, { 6, "B / Cir" }, { 7, "A / X" }, { 8, "X / Sq" },
        { 9, "LB / L1" }, { 11, "RB / R1" },
        { 16, "L3" },     { 17, "R3" },
    };
    static const int kNumBtns = static_cast<int>(sizeof(kButtons) / sizeof(kButtons[0]));

    const bool hasDevice = (m_selectedDevice >= 0 && m_selectedDevice < static_cast<int>(m_deviceList.size()));

    if (!hasDevice || m_deviceList[m_selectedDevice].kind == ps2_stubs::PadDeviceKind::Keyboard)
    {
        ImGui::TextDisabled("Select a gamepad device to test inputs.");
        return;
    }

    if (ImGui::BeginChild("##gpad_test", ImVec2(-1, 0), ImGuiChildFlags_Borders))
    {
        ImGui::Spacing();

        // --- Button grid ---
        ImGui::Text("Buttons");
        ImGui::Spacing();
        const float availX = ImGui::GetContentRegionAvail().x;
        const float btnW = std::max(24.0f, (availX - 8.0f * 9) / 8.0f);
        const float btnH = 22.0f;
        int col = 0;
        for (int i = 0; i < kNumBtns; ++i)
        {
            const bool down = kButtons[i].idx >= 0 && kButtons[i].idx < 32 && btnDown[kButtons[i].idx] != 0;
            const ImVec2 sz(btnW, btnH);

            if (down)
            {
                ScopedStyleColor c0(ImGuiCol_Button, accent());
                ScopedStyleColor c1(ImGuiCol_ButtonHovered, gold());
                ScopedStyleColor c2(ImGuiCol_ButtonActive, gold());
                ScopedStyleColor c3(ImGuiCol_Text, dbz(0.10f, 0.07f, 0.03f));
                ImGui::Button(kButtons[i].label, sz);
            }
            else
            {
                ScopedStyleColor c0(ImGuiCol_Button, dbz(0.13f, 0.13f, 0.20f));
                ScopedStyleColor c1(ImGuiCol_ButtonHovered, dbz(0.18f, 0.18f, 0.26f));
                ScopedStyleColor c2(ImGuiCol_ButtonActive, dbz(0.20f, 0.20f, 0.28f));
                ScopedStyleColor c3(ImGuiCol_Text, dbz(0.55f, 0.55f, 0.62f));
                ImGui::Button(kButtons[i].label, sz);
            }

            ++col;
            if (col < 8)
                ImGui::SameLine();
            else
                col = 0;
        }
        ImGui::Spacing();

        // --- Unified stick / trigger visual ---
        ImGui::Text("Axes");
        ImGui::Spacing();

        const float stickR = 28.0f;
        const float gap = 14.0f;
        const float stickBox = stickR * 2.0f;
        ImDrawList *dl = ImGui::GetWindowDrawList();

        // --- Sticks: reserve a square per stick with Dummy, draw the circle inside it,
        //     keep labels in normal ImGui flow so the child window sizes correctly. ---
        auto drawStick = [&](int axX, int axY) {
            const float sx = axis[axX], sy = axis[axY];
            ImGui::Dummy(ImVec2(stickBox, stickBox));
            const ImVec2 boxMin = ImGui::GetItemRectMin();
            const ImVec2 boxMax = ImGui::GetItemRectMax();
            const ImVec2 c((boxMin.x + boxMax.x) * 0.5f, (boxMin.y + boxMax.y) * 0.5f);
            const float r = stickR;

            // Background circle + crosshair
            dl->AddCircleFilled(c, r, IM_COL32(24, 24, 38, 255), 48);
            dl->AddCircle(c, r, IM_COL32(255, 255, 255, 50), 48, 1.5f);
            dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), IM_COL32(255, 255, 255, 40), 1.0f);
            dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), IM_COL32(255, 255, 255, 40), 1.0f);

            // Position dot (clamped to circle)
            const float mag = std::sqrt(sx * sx + sy * sy);
            float dx = sx, dy = sy;
            if (mag > 1.0f) { dx /= mag; dy /= mag; }
            const ImVec2 pos(c.x + dx * r * 0.85f, c.y + dy * r * 0.85f);
            dl->AddCircleFilled(pos, 7.0f, IM_COL32(240, 160, 48, 255), 24);
            dl->AddCircle(pos, 7.0f, IM_COL32(255, 220, 120, 255), 24, 1.5f);
        };

        // Stick row (centered)
        const float rowW = stickBox * 2 + gap;
        const float rowOff = (ImGui::GetContentRegionAvail().x - rowW) / 2.0f;
        ImGui::Dummy(ImVec2(rowOff, 0));
        ImGui::SameLine();
        drawStick(0, 1);   // LX, LY
        ImGui::SameLine();
        drawStick(2, 3);   // RX, RY

        // Stick labels row (normal flow, centered)
        ImGui::Spacing();
        {
            char lbl[64];
            snprintf(lbl, sizeof lbl, "LX %+0.2f   RX %+0.2f", axis[0], axis[2]);
            float tw = ImGui::CalcTextSize(lbl).x;
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - tw) / 2.0f);
            ImGui::TextColored(gold(0.9f), "%s", lbl);
            snprintf(lbl, sizeof lbl, "LY %+0.2f   RY %+0.2f", axis[1], axis[3]);
            tw = ImGui::CalcTextSize(lbl).x;
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - tw) / 2.0f);
            ImGui::TextColored(gold(0.9f), "%s", lbl);
        }
        ImGui::Spacing();

        // --- Triggers: vertical bars, each with a Dummy for its box, labels normal. ---
        const float trH = 42.0f, trW = 16.0f;
        const float trigRowW = trW * 2 + gap;
        const float trigOff = (ImGui::GetContentRegionAvail().x - trigRowW) / 2.0f;
        ImGui::Dummy(ImVec2(trigOff, 0));
        ImGui::SameLine();

        auto drawTrigger = [&](int t) {
            const float v = axis[4 + t];
            ImGui::Dummy(ImVec2(trW, trH));
            const ImVec2 boxMin = ImGui::GetItemRectMin();
            const ImVec2 boxMax = ImGui::GetItemRectMax();

            // Track (bottom-up fill)
            dl->AddRectFilled(boxMin, boxMax, IM_COL32(30, 30, 45, 255), 4.0f);
            const float fillH = trH * std::min(1.0f, std::fabs(v));
            if (fillH > 1.0f)
                dl->AddRectFilled(ImVec2(boxMin.x, boxMax.y - fillH),
                                  boxMax,
                                  ImGui::ColorConvertFloat4ToU32(accent(0.6f + 0.4f * std::fabs(v))), 4.0f);
        };

        drawTrigger(0);   // LT
        ImGui::SameLine();
        drawTrigger(1);   // RT

        // Trigger labels (normal flow, centered)
        ImGui::Spacing();
        {
            char lbl[32];
            snprintf(lbl, sizeof lbl, "LT %.2f    RT %.2f", axis[4], axis[5]);
            float tw = ImGui::CalcTextSize(lbl).x;
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - tw) / 2.0f);
            ImGui::TextColored(gold(0.9f), "%s", lbl);
        }

        ImGui::Spacing();
        ImGui::Spacing();
    }
    ImGui::EndChild();
}
