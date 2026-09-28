#include "ps2_runtime.h"   // [fps60] ps2Set60Fps
#include "runtime/ps2_texreplace.h"
#include "ps2_settings_overlay.h"
#include "runtime/ps2_netplay.h"   // [netplay]
#include "runtime/ps2x_achieve.h"  // [ach]
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
#include "runtime/ps2_video_status.h"   // [video] the status dots
#include "runtime/ps2x_perf_status.h"   // [perf] the live fps / frame-time / GPU-busy readout   // UiSetup/Begin/End: rlImGui (GL) or imgui_impl_dx11 (PS2X_D3D11)
#include "runtime/ps2x_mainmenu.h"      // [mmpopup] phase names + the plate-count the gate is made of
#include "gfx/bt3gl_api.h"   // [B] bt3* API bridge
#include "gfx/image_io.h"    // [netplay] PNG decode for the icon art
#include <atomic>            // [netjump] g_netCurtainWant, raised by the transition
#include <cfloat>            // [netjump] FLT_MAX for CalcTextSizeA

#include "runtime/ps2_toml.h"
#include "runtime/ps2x_settings.h"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

static const char *kConfigFileName = "settings.toml";        // front-end + overlay + FMV share this
static const char *kLegacyConfigFileName = "bt3_settings.ini"; // 0.x format, migrated on first load
static const char *kDumpFileName = "bt3_settings_dump.log";

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
    // [surface] #001B39 -- RGB 0, 27, 57. Every background in the overlay is painted with this one
    // colour, so the settings panel, the main-menu popup, the popups, the tab strip, the table rows
    // and the scrollbar read as the same navy. They used to be five different near-blacks
    // (0.04/0.06/0.08 and friends) that were close enough to look like an accident rather than a
    // choice. One constant, so the next change is one edit.
    constexpr float kSurfR = 0.000f, kSurfG = 0.106f, kSurfB = 0.224f;   // 27/255, 57/255
    ImVec4 surface(float a = 1.0f) { return dbz(kSurfR, kSurfG, kSurfB, a); }

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
            ImGui::PushStyleColor(ImGuiCol_WindowBg,            surface(0.97f));
            ImGui::PushStyleColor(ImGuiCol_ChildBg,             surface(0.60f));
            ImGui::PushStyleColor(ImGuiCol_PopupBg,             surface(0.98f));
            ImGui::PushStyleColor(ImGuiCol_Border,              accent(0.55f));
            ImGui::PushStyleColor(ImGuiCol_BorderShadow,        dbz(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_TitleBg,             surface(1.0f));
            ImGui::PushStyleColor(ImGuiCol_TitleBgActive,       surface(1.0f));
            ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed,    surface(1.0f));
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
            ImGui::PushStyleColor(ImGuiCol_Tab,                 surface(0.0f));
            ImGui::PushStyleColor(ImGuiCol_TabHovered,          accent(0.20f));
            ImGui::PushStyleColor(ImGuiCol_TabActive,           accent(0.14f));
            ImGui::PushStyleColor(ImGuiCol_TabUnfocused,        surface(0.0f));
            ImGui::PushStyleColor(ImGuiCol_TabUnfocusedActive,  accent(0.10f));
            ImGui::PushStyleColor(ImGuiCol_TableHeaderBg,       surface(1.0f));
            ImGui::PushStyleColor(ImGuiCol_TableRowBg,          surface(0.50f));
            ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt,       dbz(1.00f, 0.62f, 0.10f, 0.04f));
            ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,         surface(0.60f));
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
           // [ach] netOverlay is absent from this comparison, and that is pre-existing: the
           // overlay writes it through m_dirty when the switch moves. This one is here because
           // the same tab owns the whole tracker and a change to it should read as unsaved.
           achievements == o.achievements &&
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
    m_envLocked = 0;
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
    m_settings.sfxVolume = std::clamp((float)doc.getD("audio.sfx_volume", m_settings.sfxVolume), 0.0f, 0.4f);

    {
            int r = nameToRenderer(doc.getS("video.renderer", rendererName(m_settings.renderer)), m_settings.renderer);
#if !defined(PS2X_HAVE_PGS)
            if (r == Settings::kRendererParallelGS)
            {
                r = Settings::kRendererOpenGL;
                // Build-capability fallback, not a migration: this build simply cannot run PGS, and
                // writing the fallback back would erase the choice for a build that can.
                m_envLocked |= ps2x_settings::kLockRenderer;
            }
#endif
            // [d3d11] Direct3D 11 is retired for now: an old settings file that picks it falls back to
            // the new OpenGL present. paraLLEl-GS is a normal option on every platform again.
            if (r == Settings::kRendererD3D11) r = Settings::kRendererOpenGL;
            if (r >= 0 && r <= 3) { m_settings.renderer = r; m_sawRendererKey = true; }
            // [display] window mode / monitor: the popup owns them, defaulted from the legacy fullscreen flag
            m_settings.windowMode = doc.getI("video.window_mode", m_settings.fullscreen ? 2 : 0);
            m_settings.monitor = doc.getI("video.monitor", m_settings.monitor);
        }
        // [netplay] The same key the front-end writes, so the launcher's Misc switch and this one
        // are the same setting. NET_OVERLAY stays the default for a run with no saved value, which
        // is why it is not env-locked the way the video switches are: a saved "off" is the user
        // saying no, and a saved "on" is the user saying yes, and an environment default should
        // not outvote either.
        m_settings.netOverlay = doc.getB("netplay.overlay", m_settings.netOverlay);
        // [ach] The same key the front-end's Misc page writes. Seeded here rather than only in
        // applySettings() so the switch shows the saved value before the tab is ever opened.
        m_settings.achievements = doc.getB("achievements.enabled", m_settings.achievements);
        if (envUserSet("PS2X_GLOW")) m_envLocked |= ps2x_settings::kLockGlow;
        else m_settings.glow = doc.getB("video.glow", m_settings.glow);
        if (envUserSet("PS2X_GLOWFIX")) m_envLocked |= ps2x_settings::kLockGlowFix;
        else m_settings.glowFix = doc.getB("video.glowfix", m_settings.glowFix);
        if (envUserSet("PS2X_INKSTRENGTH") || envUserSet("PS2X_ADGS"))
        {
            m_envLocked |= ps2x_settings::kLockInkStrength;
        }
        else
        {
            m_settings.inkStrength = std::clamp(doc.getI("video.ink_strength", m_settings.inkStrength), 100, 400);
        }
        m_settings.inkWidth = std::clamp(doc.getI("video.ink_width", m_settings.inkWidth), 25, 100);
        m_settings.inkColor = hexToColor(doc.getS("video.ink_color", colorToHex(m_settings.inkColor)), m_settings.inkColor);
        if (envUserSet("PS2X_BILINEAR")) m_envLocked |= ps2x_settings::kLockBilinear;
        else m_settings.bilinear = doc.getB("video.bilinear", m_settings.bilinear);
        if (envUserSet("PS2X_HALFTEXEL")) m_envLocked |= ps2x_settings::kLockHalfTexel;
        else m_settings.halfTexel = doc.getB("video.halftexel", m_settings.halfTexel);
        if (envUserSet("PS2X_SKIPPOST")) m_envLocked |= ps2x_settings::kLockSkipPost;
        else m_settings.skipPost = doc.getB("video.skippost", m_settings.skipPost);
        if (envUserSet("PS2X_SKIP_STALE_VRAM")) m_envLocked |= ps2x_settings::kLockSkipStale;
        else m_settings.skipStaleVram = doc.getB("video.skip_stale_vram", m_settings.skipStaleVram);
        if (envUserSet("PS2X_RENDER_SCALE"))
        {
            m_envLocked |= ps2x_settings::kLockRenderScale;
        }
        else
        {
            const int s = doc.getI("video.render_scale", m_settings.renderScale);
            m_settings.renderScale = (s >= 1 && s <= 4) ? s : 1;
        }
        if (envUserSet("PS2X_OUTLINE")) m_envLocked |= ps2x_settings::kLockOutline;
        else m_settings.outline = doc.getB("video.outline", m_settings.outline);
        if (envUserSet("PS2X_TEXPACK")) m_envLocked |= ps2x_settings::kLockTexPack;
        else m_settings.texPack = doc.getB("video.texture_pack", m_settings.texPack);
        if (envUserSet("PS2X_FMV_OVERRIDE")) m_envLocked |= ps2x_settings::kLockIntroVideo;
        else m_settings.introVideo = doc.getB("video.intro_video", m_settings.introVideo);
        if (envUserSet("PS2X_BUTTONS")) m_envLocked |= ps2x_settings::kLockButtonLay;
        else m_settings.buttonLayout = doc.getI("video.button_layout", m_settings.buttonLayout);
        if (envUserSet("PS2X_SHADOWS")) m_envLocked |= ps2x_settings::kLockShadows;
        else m_settings.shadows = doc.getB("video.shadows", m_settings.shadows);
        if (envUserSet("PS2X_DOFMASK")) m_envLocked |= ps2x_settings::kLockDofBlur;
        else m_settings.dofBlur = doc.getB("video.dof_blur", m_settings.dofBlur);
        if (envUserSet("PS2X_DOFZFAR")) m_envLocked |= ps2x_settings::kLockDofZFar;
        else m_settings.dofZFar = std::clamp(doc.getI("video.dof_zfar", m_settings.dofZFar), 20000, 800000);
    m_settings.fullscreen = doc.getB("video.fullscreen", m_settings.fullscreen);
    m_settings.widescreen = doc.getB("video.widescreen", m_settings.widescreen);
        m_settings.fps60 = doc.getB("video.fps60", m_settings.fps60);
        m_settings.showPerf = doc.getB("video.show_perf", m_settings.showPerf);
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
        // [ablend128] Applied here rather than per family: a texture replacement is uploaded
        // byte-for-byte and never gets the decoder's PS2->PC alpha expansion, so a full-range pack
        // needs the full GS blend factor or every replaced sprite reads translucent. The narrower
        // per-family scopes (PS2X_ABLEND128=2..5) stay reachable through the env.
        setEnvDefault("PS2X_ABLEND128", "1");
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
        // 0.x legacy INI: the front-end imports it and writes the TOML (dropping the old
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
    // [settings] ONE writer for settings.toml: the front-end and this overlay both serialize
    // through ps2x_settings, so the two cannot drift (the Qt launcher and this overlay did:
    // the old launcher's `texcache` key was silently dropped every time a play session ended).
    //
    // Read-modify-write, and that is the part that matters. Default-constructing `out` made every
    // key this overlay does not model snap back to its struct default on every save: video.gpu,
    // and the whole [frontend] section, so the shell forgot the window size it was left at and
    // the menu theme un-muted itself. Seeding from the file on disk carries those across, and also
    // makes a key added later survive by default instead of needing a line here on day one.
    ps2x_settings::Settings out;
    ps2x_settings::loadFromFile(out, m_configPath);

    // The overlay's live values, in the shared module's own struct so the field names line up.
    // device has no counterpart: the front-end has a single picker and P1 is what it writes.
    ps2x_settings::Settings live;
    live.netOverlay = m_settings.netOverlay;   // [netplay] the same key the front-end's Misc writes
    live.achievements = m_settings.achievements;   // [ach] likewise
    live.master = m_settings.masterVolume;
    live.music = m_settings.musicVolume;
    live.sfx = m_settings.sfxVolume;
    live.renderer = m_settings.renderer;
    live.glow = m_settings.glow;
    live.glowFix = m_settings.glowFix;
    live.inkStrength = m_settings.inkStrength;
    live.inkWidth = m_settings.inkWidth;
    live.inkColor = m_settings.inkColor;
    live.bilinear = m_settings.bilinear;
    live.halfTexel = m_settings.halfTexel;
    live.skipPost = m_settings.skipPost;
    live.skipStaleVram = m_settings.skipStaleVram;
    live.renderScale = m_settings.renderScale;
    live.outline = m_settings.outline;
    live.texPack = m_settings.texPack;
    live.introVideo = m_settings.introVideo;
    live.buttonLayout = m_settings.buttonLayout;
    live.shadows = m_settings.shadows;
    live.dofBlur = m_settings.dofBlur;
    live.dofZFar = m_settings.dofZFar;
    live.fullscreen = m_settings.fullscreen;
    live.windowMode = m_settings.windowMode;
    live.monitor = m_settings.monitor;
    live.widescreen = m_settings.widescreen;
    live.windowW = m_settings.windowW;
    live.windowH = m_settings.windowH;
    live.forceBilinear = m_settings.forceBilinear;
    live.fps60 = m_settings.fps60;
    live.showPerf = m_settings.showPerf;
    live.hudLayout = m_settings.hudLayout;
    live.hudOffL = m_settings.hudOffL;
    live.hudOffC = m_settings.hudOffC;
    live.hudOffR = m_settings.hudOffR;
    live.device = deviceIndexForPlayer(0);   // [paddev] P1 (the front-end has one picker)
    live.deadzone = m_settings.deadzone;
    live.overlayEnabled = m_settings.overlayEnabled;
    live.overlayPadBtns = ps2x_settings::formatIntCsv(m_settings.overlayPadBtns);
    live.overlayKeys = ps2x_settings::formatIntCsv(m_settings.overlayKeys);
    live.logLevel = m_settings.logLevel;
    live.dumpAudio = m_dumpAudio;
    live.dumpVideo = m_dumpVideo;
    live.dumpControllers = m_dumpControllers;
    live.dumpRuntime = m_dumpRuntime;
    live.dumpGamepad = m_dumpGamepad;

    ps2x_settings::applyOverlayValues(out, live, m_envLocked);

    ps2x_settings::saveToFile(out, m_configPath);
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
    // [ach] Both switches are applied here, not only where they are drawn, so a saved value takes
    // effect at boot without the player having to open the tab first. applySettings() runs on the
    // load path, so the tracker is armed before the first frame is evaluated.
    //
    // The ApplyDefault pair, not the plain setters: those are the player's click and give up the
    // environment, and applySettings() runs right after init -- sharing one entry point would make
    // NET_OVERLAY=0 and ACHIEVEMENTS=0 no-ops on the one boot they exist for.
    //
    // netOverlay is applied here too, which it was not before. It used to be pushed only from
    // drawNetplayTab(), which meant the environment was the effective value at boot while the tab
    // showed the saved one -- the two could disagree and nothing would say so.
    ps2xNetOverlayApplyDefault(m_settings.netOverlay);
    ps2xAchApplyDefault(m_settings.achievements);
    ps2x::SetPerfOverlayEnabled(m_settings.showPerf);   // [perf] arm the GPU timing queries at boot too
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

// [mmpopup] The main-menu test plate.
//
// Its whole job is to prove the gate end to end: if this plate is on screen, the runtime believes
// the main menu is up, and it must vanish on exactly the frame the menu goes away. It is anchored
// to the game's own plate-build counter (menuObj+0x144, which the build loop at 0x3355b8
// increments once per entry until 11), NOT to a "menu is displayed" state field -- the addresses
// docs/MAIN-MENU.md section 7 gives for that do not resolve in this build (*(0x3B38D8) reads 0),
// so a gate built on them would never open.
//
// NET_OVERLAY, or the checkbox in the launcher's Misc page / this overlay's Netplay tab, turns the
// whole feature on: the label, this panel, and the automatic transition with its curtain. Off by
// default -- it is a new feature and a corner of someone's game screen is not something to put in
// front of them unasked.
extern std::atomic<uint32_t> g_bt3MenuShown;   // [mainmenu] ps2_runtime.cpp: 1 while the menu is up
extern std::atomic<uint32_t> g_bt3MenuPhase;   // [mainmenu] the ps2x::mainmenu::Phase value
extern std::atomic<uint32_t> g_bt3MenuPlates;  // [mainmenu] menuObj+0x144, the build counter
extern "C" int  ps2xNetJumpState();             // [netjump] the jump's phase: 1 armed, 2 settled, 3 returning
// [netjump] The curtain's TARGET. The level is integrated by drawNetCurtain() below, which is also
// where this is declared for its own use; the label and the panel need it too, to know whether to
// take input, and a window behind a curtain must not be clickable.
extern std::atomic<int> g_netCurtainWant;   // game_overrides.cpp: 1 = cover the screen

// [mmpopup] The panel's open state, plus the gate's previous value. File scope rather than a local
// so the closing edge can be seen from mainMenuPopupWanted(): the draw function only runs while the
// gate is OPEN, so by the time the menu is gone there is no longer a frame to notice it in.
static bool s_mmPopupOpen = false;
static bool s_mmGateWasOpen = false;

// [mmpopup] Where the panel actually is between 0 (retracted) and 1 (deployed), so it can move
// instead of appearing. s_mmPopupOpen stays the TARGET: the click and the gate's closing edge flip
// the target, and this chases it, which means a click mid-animation reverses rather than restarts.
//
// Exponential smoothing on DeltaTime, not a per-frame step: at 30 fps a constant step would deploy
// in half the time it takes at 144. The 0.11s constant is the time to close ~63% of the gap, so the
// panel is most of the way out at ~0.25s and effectively done by ~0.4s.
static float s_mmPanelAnim = 0.0f;

bool PS2SettingsOverlay::mainMenuPopupWanted()
{
    // Reading the atomic (not the whole snapshot) keeps the draw path free of guest-RAM reads: the
    // runtime samples once per frame and publishes, this only reads what it published.
    const bool open = g_bt3MenuShown.load(std::memory_order_relaxed) != 0u;

    // Leaving the menu closes the panel. On the closing edge only, not whenever the gate is shut:
    // otherwise the panel would be unable to stay open across the frames where it is drawn, and
    // "was open, now shut" is the one moment that means the menu is actually gone rather than
    // never having been up.
    if (s_mmGateWasOpen && !open)
        s_mmPopupOpen = false;
    s_mmGateWasOpen = open;

    // Read the switch every call, not once: the two settings UIs flip it while the game is running
    // and there is no frame to notice it in otherwise.
    return ps2xNetOverlayEnabled() && open;
}

// [mmpopup] The Netplay icon: the Namek planet with a cloud drifting around it.
//
// The artwork is the Dragon Net menu's own (assets/DragonNet/menu in the old tree, now
// assets/netplay/): a 464x524 planet and a 300x142 cloud. Both are plain RGBA8 PNGs, decoded once
// with the same GsDecodeImageRGBA8 the launcher uses for its background.
//
// The cloud's motion comes from the design's inline styles, which drive it along a CSS
// offset-path of two elliptical arcs -- "M48.3,262.5 A170,140 -18 1,1 371.7,157.5 A170,140 -18 1,1
// 48.3,262.5 Z" -- over 6s, linear, infinite, with the sprite mirrored. Both arcs are the same
// ellipse (a 170x140 arc is exactly a half-ellipse), so the whole path is one closed loop: centre
// (210,210), radii (161.7, 52.5), rotated -18 degrees, and the cloud is 78px wide inside a 420px
// card. Parametrising that ellipse is the same path, minus the browser.
struct NetplayIcon
{
    unsigned long long planet = 0;
    unsigned long long cloud  = 0;
    int pw = 0, ph = 0, cw = 0, ch = 0;
    bool tried = false;
    bool ok() const { return planet != 0 && cloud != 0; }
};

static NetplayIcon g_netplayIcon;

static void loadNetplayIcon()
{
    if (g_netplayIcon.tried)
        return;
    g_netplayIcon.tried = true;

    // Relative to the working directory, which is the deploy root: the front-end runs with that as
    // its working directory and CMake stages assets/ next to the executable.
    const char *envDir = std::getenv("PS2X_NETPLAY_ART");
    const std::string dir = (envDir && envDir[0]) ? envDir : "assets/netplay";

    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (ps2x::gfx::GsDecodeImageRGBA8((dir + "/netplanet.png").c_str(), rgba, w, h) && w > 0 && h > 0)
    {
        g_netplayIcon.planet = ps2x::gfx::UiLoadTextureRgba(rgba.data(), w, h);
        g_netplayIcon.pw = w;
        g_netplayIcon.ph = h;
    }
    if (ps2x::gfx::GsDecodeImageRGBA8((dir + "/netcloud.png").c_str(), rgba, w, h) && w > 0 && h > 0)
    {
        g_netplayIcon.cloud = ps2x::gfx::UiLoadTextureRgba(rgba.data(), w, h);
        g_netplayIcon.cw = w;
        g_netplayIcon.ch = h;
    }
    std::fprintf(stderr, "[netplay-icon] planet=%dx%d cloud=%dx%d loaded=%d\n",
                 g_netplayIcon.pw, g_netplayIcon.ph, g_netplayIcon.cw, g_netplayIcon.ch,
                 g_netplayIcon.ok() ? 1 : 0);
}

// The one point of the design's orbit path, at phase t in [0,1).
static void netOrbitPoint(float t, float cx, float cy, float rx, float ry, float rotDeg,
                          float &x, float &y)
{
    const float a = t * 6.28318530718f;
    const float ex = rx * std::cos(a);
    const float ey = ry * std::sin(a);
    const float r = rotDeg * 3.14159265359f / 180.0f;
    const float cs = std::cos(r), sn = std::sin(r);
    x = cx + ex * cs - ey * sn;
    y = cy + ex * sn + ey * cs;
}

// [netplay] The form's values, shared by the settings tab and by the main-menu popup.
//
// They were function-local statics inside drawNetplayTab(), which quietly meant the two views could
// not agree: set a port in the settings tab, open the popup, and it showed a stale 7777 because it
// had no access to the other copy. One struct, two layouts, one set of values.
//
// Still not persisted -- these reset on every process start, so a game-mode change does not survive
// quitting. Only `peer` can be seeded from outside, and only through PS2X_NET_PEER.
struct NetplayForm
{
    char peer[64] = "127.0.0.1";
    int  port    = 7777;
    int  delay   = 2;      // frames; BT3 runs at 30 fps
    int  battle  = 0;      // 0 Single, 1 Team, 2 DP
    int  dp      = 0;      // 0 = 10 DP, 1 = 15, 2 = 20
    int  time    = 3;      // 0..3 = 60/90/180/240 s, 4 = no limit
    bool jump    = true;   // go to character select once connected
    bool seeded  = false;
    // The last connect attempt, for the popup's status monitor. ps2NetHost/ps2NetJoin already
    // answer false when the socket will not open, but nothing in the API remembers it afterwards,
    // so "failed to connect" would otherwise be indistinguishable from "never tried".
    bool attempted = false;
    bool faild     = false;

    void seed()
    {
        if (seeded)
            return;
        seeded = true;
        if (const char *e = std::getenv("PS2X_NET_PEER"))
            std::snprintf(peer, sizeof peer, "%s", e);
        if (port < 1 || port > 65535)
            port = 7777;
    }
};
static NetplayForm g_netForm;

// A label above its widget, for the popup's narrow columns. The tab draws labels inline to the
// left, which is fine at the 1080px the settings window is and does not fit the popup's 520.
static void netLabel(const char *label)
{
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1.0f);   // the widget takes the whole cell, not the 120px default
}

// [netplay] The popup's body: the same form the settings tab shows, in the geometry that fits a
// corner popup.
//
// Only the "Go to character select" checkbox is gone -- its value still lives in the shared form
// (the tab owns the checkbox), because ps2_netplay's g_autoJump starts false and nothing else sets
// it, so dropping it entirely would mean the game never jumps. Everything else is the tab's.
static void drawNetplayPopupBody()
{
    g_netForm.seed();

    // The status monitor, and the four states it can be in. This drives the header pill and is the
    // only thing in the popup that says whether the last button press did anything.
    struct Status { const char *text; ImVec4 color; int id; };
    Status st;
    if (!ps2NetActive())
    {
        // No session. If one was asked for and is not here, it did not come up.
        if (g_netForm.attempted) { st = {"FAILED TO CONNECT", {0.95f, 0.34f, 0.31f, 1.0f}, 3}; }
        else                     { st = {"WAITING",           {0.62f, 0.68f, 0.74f, 1.0f}, 0}; }
    }
    else if (!ps2NetPeerConnected())
    {
        st = {"CONNECTING", {1.00f, 0.72f, 0.20f, 1.0f}, 1};
    }
    else
    {
        st = {"CONNECTED", {0.35f, 0.88f, 0.45f, 1.0f}, 2};
    }

    // The pill's two animations. Both key off the state id, not off the strings, so a rename cannot
    // silently restart the flash.
    //
    //   CONNECTING breathes, because it is the one state that means "something is happening and
    //   the answer has not arrived". A steady amber pill reads as a setting; a pulsing one reads as
    //   a wait. The other three hold still, because they are answers, not waits.
    //   Any change of state flashes the new colour white and lets it decay, so a transition that
    //   happens off-screen (a peer that never showed up) is still visible when you look back.
    const float now = float(ImGui::GetTime());
    static int   s_lastId = -1;
    static float s_changedAt = -10.0f;
    if (st.id != s_lastId)
    {
        // The flash, and only the flash. The matching card and sound are raised by ps2NetFrame() in
        // the netplay module, which runs on every frame of the game; this function only runs while
        // the panel is open, so a session that connected during a fight would never be announced
        // from here. One edge detector, in the one place that is always awake.
        s_lastId = st.id;
        s_changedAt = now;
    }
    const float since = now - s_changedAt;
    const float flash = since < 0.45f ? (1.0f - since / 0.45f) : 0.0f;
    // 1.3s per breath, eased so it lingers at each end instead of sweeping linearly.
    const float breathe = st.id == 1
        ? 0.62f + 0.38f * (0.5f + 0.5f * std::sin(float(now / 1.3 * 6.28318530718)))
        : 1.0f;
    const float lum = (0.55f + 0.45f * flash) * breathe;

    // Header: the Namek mark, the word, and the status pill on the right. Drawn by hand rather than
    // laid out with SameLine, because the mark is an image on the draw list and the pill has to be
    // right-aligned to the panel's edge; doing it in one pass keeps all three on one baseline.
    const float hdrH  = 42.0f;
    const float hdrW  = ImGui::GetContentRegionAvail().x;
    const ImVec2 h0   = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(0.0f, hdrH));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec4 gold(1.00f, 0.80f, 0.30f, 1.0f);
    const float  midY = h0.y + hdrH * 0.5f;

    const float markW = 30.0f;
    if (g_netplayIcon.ok())
    {
        const float markH = markW * float(g_netplayIcon.ph) / float(g_netplayIcon.pw);
        dl->AddImage(ImTextureRef((ImTextureID)g_netplayIcon.planet),
                     ImVec2(h0.x, midY - markH * 0.5f),
                     ImVec2(h0.x + markW, midY + markH * 0.5f));
    }
    const ImVec2 titleSz = ImGui::CalcTextSize("Netplay");
    dl->AddText(ImVec2(h0.x + markW + 10.0f, midY - titleSz.y * 0.5f),
                ImGui::ColorConvertFloat4ToU32(gold), "Netplay");

    // The pill, flush right: a soft fill of the state colour, its border, and its name. The flash
    // lifts the colour toward white and grows the pill a little, and the breath scales the alpha, so
    // both show up in the border and the text and not only in the fill.
    ImVec4 sc(st.color.x, st.color.y, st.color.z, 1.0f);
    sc.x += (1.0f - sc.x) * flash * 0.75f;
    sc.y += (1.0f - sc.y) * flash * 0.75f;
    sc.z += (1.0f - sc.z) * flash * 0.75f;
    const float grow = flash * 2.0f;
    const ImVec2 stSz = ImGui::CalcTextSize(st.text);
    const float  pillH = stSz.y + 8.0f + grow;
    const float  pillW = stSz.x + 22.0f + grow * 2.0f;
    const ImVec2 p1(h0.x + hdrW - pillW * 0.5f, midY - pillH * 0.5f);
    const ImVec2 p0(p1.x - pillW, midY - pillH * 0.5f);
    if (flash > 0.0f)   // the halo, so the change is visible from across the screen
        dl->AddRect(ImVec2(p0.x - 3.0f, p0.y - 3.0f), ImVec2(p1.x + 3.0f, p1.y + 3.0f),
                    ImGui::ColorConvertFloat4ToU32(ImVec4(sc.x, sc.y, sc.z, flash * 0.35f)),
                    pillH * 0.5f + 3.0f, 0, 1.0f);
    dl->AddRectFilled(p0, p1, ImGui::ColorConvertFloat4ToU32(
                        ImVec4(sc.x, sc.y, sc.z, 0.16f * lum)), pillH * 0.5f);
    dl->AddRect(p0, p1, ImGui::ColorConvertFloat4ToU32(ImVec4(sc.x, sc.y, sc.z, lum)),
                pillH * 0.5f, 0, 1.0f + flash);
    dl->AddText(ImVec2(p0.x + 11.0f, p0.y + 4.0f),
                ImGui::ColorConvertFloat4ToU32(ImVec4(sc.x, sc.y, sc.z, lum)), st.text);

    ImGui::Separator();

    if (ps2NetActive())
    {
        ImGui::Text("You are player %d", ps2NetLocalPlayer());
        ImGui::Text("Input delay: %u frames (%u ms at 30 fps)", ps2NetDelay(),
                    ps2NetDelay() * 33u);
        { const char *bn[] = {"Single Battle", "Team Battle", "DP Battle"};
          const char *tn[] = {"60 s", "90 s", "180 s", "240 s", "no limit"};
          const char *dn[] = {"10 DP", "15 DP", "20 DP"};
          const int bt = ps2NetBattleType(), tl = ps2NetTimeLimit(), dp = ps2NetDpLimit();
          ImGui::Text("Game mode: %s%s%s   |   time limit: %s",
                      (bt >= 0 && bt < 3) ? bn[bt] : "?",
                      bt == 2 ? " / " : "", (bt == 2 && dp >= 0 && dp < 3) ? dn[dp] : "",
                      (tl >= 0 && tl < 5) ? tn[tl] : "?"); }
        ImGui::TextDisabled("Only buttons cross the wire. Each side renders its own player "
                            "full-screen.");
        ImGui::Spacing();
        if (ImGui::Button("Disconnect", ImVec2(-1.0f, 0.0f)))
        {
            ps2NetDisconnect("popup");
            g_netForm.attempted = false;   // back to WAITING, not left reading FAILED
            g_netForm.faild     = false;
        }
        return;
    }

    if (ImGui::BeginTable("##net_cols", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableNextColumn();
        netLabel("HOST ADDRESS (JOIN ONLY)");
        ImGui::InputText("##peer", g_netForm.peer, sizeof g_netForm.peer);
        netLabel("PORT");
        ImGui::InputInt("##port", &g_netForm.port);
        if (g_netForm.port < 1 || g_netForm.port > 65535)
            g_netForm.port = 7777;   // same clamp the tab does, and equally silent

        ImGui::TableNextColumn();
        netLabel("GAME MODE");
        { const char *kBattle[] = {"Single Battle", "Team Battle", "DP Battle"};
          ImGui::Combo("##battle", &g_netForm.battle, kBattle, 3); }
        netLabel("TIME LIMIT");
        { const char *kTime[] = {"60 seconds", "90 seconds", "180 seconds",
                                 "240 seconds (default)", "No limit"};
          ImGui::Combo("##time", &g_netForm.time, kTime, 5); }
        // DP Battle's point budget is a SEPARATE row of the versus menu (duelObj+0x118), so choosing
        // DP without it left the screen playing like Team Battle: the right type, no budget behind it.
        if (g_netForm.battle == 2)
        {
            netLabel("DP LIMIT");
            const char *kDp[] = {"10 DP", "15 DP", "20 DP"};
            ImGui::Combo("##dp", &g_netForm.dp, kDp, 3);
        }
        ImGui::EndTable();
    }

    netLabel("INPUT DELAY (FRAMES)");
    ImGui::SliderInt("##delay", &g_netForm.delay, 1, 10);
    ImGui::TextDisabled("The HOST's choices apply to both players.");

    ImGui::Separator();
    if (ImGui::Button("HOST  ·  you are Player 1", ImVec2(-1.0f, 0.0f)))
    {
        ps2NetSetAutoJump(g_netForm.jump);
        ps2NetSetDelay(g_netForm.delay);
        ps2NetSetBattleType(g_netForm.battle);
        ps2NetSetTimeLimit(g_netForm.time);
        ps2NetSetDpLimit(g_netForm.dp);
        g_netForm.attempted = true;
        g_netForm.faild     = !ps2NetHost(g_netForm.port, 1);
    }
    if (ImGui::Button("JOIN  ·  you are Player 2", ImVec2(-1.0f, 0.0f)))
    {
        ps2NetSetAutoJump(g_netForm.jump);
        ps2NetSetDelay(g_netForm.delay);      // the host's game mode wins
        char hp[96];
        std::snprintf(hp, sizeof hp, "%s:%d", g_netForm.peer, g_netForm.port);
        g_netForm.attempted = true;
        g_netForm.faild     = !ps2NetJoin(hp, 2);
    }
    ImGui::TextDisabled("HOST: press Host and give the other player your IP and this port.");
    ImGui::TextDisabled("JOIN: type the host's IP above, then press Join.");

}

// [netjump] The curtain: a black rectangle over the game with "Loading..." on it, while the netplay
// transition walks the menus from the main menu to character select.
//
// It answers g_netCurtainWant, which bt3NetJumpCharSelect raises and lowers. The WANT is the
// contract; the LEVEL is integrated here, because a fade needs a frame clock and the state machine
// that raises it is the guest's, running at its own pace. Same exponential-on-DeltaTime chase as the
// panel's, so the two never disagree about how fast things move, and a raise during a lower (or the
// reverse) reverses rather than queueing.
//
// Drawn from BOTH frame paths. The early one is the usual in-game case -- no settings panel open --
// and the late one is when the player had the panel up, where the curtain has to land on top of it
// or the transition happens behind a settings window full of controls.
//
// A rectangle, not a swapFrame() hold: the hold stops the frame being published, which freezes the
// picture on whatever was there. This covers it with something that says what is happening, and it
// costs one full-screen quad. The game keeps rendering underneath, which is the cost of choosing
// the overlay over the renderer.
// The integrated level, at namespace scope so the frame function can ask whether the curtain still
// needs drawing. It is the whole reason the curtain is not gated behind the popup test env any more:
// see netCurtainBusy() below.
static float s_curtainLevel = 0.0f;

// True while the curtain is up OR still on its way down. The fade has to keep running after the
// target drops, so "the target is 0" is not the same question as "is there nothing to draw".
static bool netCurtainBusy()
{
    extern std::atomic<int> g_netCurtainWant;   // game_overrides.cpp: the target, not the level
    return s_curtainLevel > 0.0f || g_netCurtainWant.load(std::memory_order_relaxed) != 0;
}

static void drawNetCurtain()
{
    const float want = g_netCurtainWant.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
    const float dt = ImGui::GetIO().DeltaTime;
    s_curtainLevel += (want - s_curtainLevel) * (1.0f - std::exp(-dt / 0.28f));
    if (std::fabs(s_curtainLevel - want) < 0.004f)
        s_curtainLevel = want;   // settle, so the last frame of a fade is exactly opaque
    const float s_level = s_curtainLevel;

    // [netjump] The game is silenced for as long as the curtain is up. Deliberately not a volume
    // change: the settings panel owns masterVolume and rewrites it from the ini, so this is a
    // separate factor the mixer multiplies in. BGM and SFX both, because the point is not to hear
    // the menus being operated under the black.
    //
    // BEFORE the early return below, which is the whole fix: the level settles to exactly 0 on the
    // last frame of a fade, that frame returned before this block, and so the unmute never ran --
    // the game came back from the transition silent and stayed that way. The mute has to be
    // released on the frame the curtain reaches zero, which is the frame that draws nothing.
    {
        static bool s_muted = false;
        const bool wantMute = s_level > 0.001f;
        if (wantMute != s_muted)
        {
            s_muted = wantMute;
            PS2AudioBackend::setCurtainMute(wantMute);   // global namespace: see ps2_audio.h
        }
    }
    if (s_level <= 0.0f)
        return;

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const ImVec2 a = vp->Pos;
    const ImVec2 b(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);

    // [mmpopup] Hidden from ImGui, drawn with the FRONT draw list. A window cannot do this job: a
    // fullscreen NoDecoration window still gets whatever WindowBg the theme set, and the theme's
    // alpha is not ours to push to 1 without unbalancing the style stack the theme pushed.
    ImDrawList *dl = ImGui::GetForegroundDrawList();   // the main viewport, which is the one we sized
    if (!dl)
        return;
    dl->AddRectFilled(a, b, ImGui::ColorConvertFloat4ToU32(ImVec4(0.0f, 0.0f, 0.0f, s_level)));

    // Both strings ride the level, and their alpha leads the black slightly on the way in and trails
    // it on the way out, so the text does not sit on a half-black screen looking like a rendering
    // fault.
    const float ta = std::min(1.0f, s_level * 1.6f) * 0.55f;   // under half opacity at full black
    const ImU32 goldA = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.80f, 0.30f, ta));
    const ImU32 greyA = ImGui::ColorConvertFloat4ToU32(ImVec4(0.72f, 0.76f, 0.80f, ta * 0.85f));
    ImFont *font = ImGui::GetFont();
    const float  cx   = (a.x + b.x) * 0.5f;
    const float  cy   = (a.y + b.y) * 0.5f;
    const float  edge = 34.0f;   // the same inset the corner label uses, so they line up

    // "Loading...", dead centre, with the dots cycling at two per second.
    // [netjump] Which half of the journey this is. g_netJumpState is the jump's own phase, and 3 is
    // the one that means "heading back to the main menu" -- set by every give-up, whether it was
    // the player, a lost session, a desync or the timeout. Saying so matters: a black screen that
    // says "Loading..." while the game is walking backwards is the player wondering whether
    // anything is happening, and "Aborting..." is the difference between a wait and a mistake.
    // Through the accessor, not the variable: g_netJumpState lives in game_overrides.cpp's anonymous
    // namespace, so it has internal linkage and there is nothing to link against.
    const bool returning = ps2xNetJumpState() == 3;
    const char *dots[4] = {"", ".", "..", "..."};
    static const double s_t0 = ImGui::GetTime();
    const int n = int((ImGui::GetTime() - s_t0) * 2.0) & 3;
    char load[32];
    std::snprintf(load, sizeof load, "%s%s", returning ? "Aborting" : "Loading", dots[n]);
    // The size argument needs a face to draw with, not a number: the theme registered one font, so
    // this scales that one rather than asking for a size that does not exist.
    const float fsize = ImGui::GetFontSize() * 1.6f;
    const ImVec2 lsz = font->CalcTextSizeA(fsize, FLT_MAX, 0.0f, load);
    dl->AddText(font, fsize, ImVec2(cx - lsz.x * 0.5f, cy - lsz.y * 0.5f), goldA, load);

    // "Press O circle to cancel", bottom right. Grey rather than gold: it is an instruction, not
    // the state of things, and the eye should go to the centre first. Only drawn while the curtain
    // is going UP or fully up -- once it starts coming down the transition is over one way or the
    // other and offering a cancel would be a lie.
    //
    // Circle is true now: the transition arms the project's own pad gate with CIRCLE as the only
    // allowed bit, so it is the one key the player has, and the seam hands its press to the jump
    // instead of the game.
    //
    // F10 is the third way out, and it is on screen because it has to be: the test hook was a button
    // in this panel, which the curtain covers. It is a key for the same reason -- you cannot click
    // what you cannot see.
    if (want > 0.0f)
    {
        // Two different hints, because the two phases ask for different things.
        //
        // Two lines because the two phases ask for different things.
        //
        // Going: one press asks for the trip back.
        //
        // Returning: TWO, and it says what the second one cancels. The first press keeps the curtain
        // up while the game walks back through the Duel Menu, and a player who cannot get out of a
        // black screen by pressing the same button again has been told to wait for a watchdog.
        // Naming the screen is the part that makes it legible: "cancel" alone does not say what is
        // being cancelled.
        static const char *kCancel = returning
            ? "press 2 times O to cancel (Go to Duel Menu)"
            : "Press O circle to cancel";
        const float csz = ImGui::GetFontSize();
        const ImVec2 cs = font->CalcTextSizeA(csz, FLT_MAX, 0.0f, kCancel);
        dl->AddText(font, csz, ImVec2(b.x - edge - cs.x, b.y - edge - cs.y), greyA, kCancel);
    }
}

void PS2SettingsOverlay::drawMainMenuPopup()
{
    // [mmpopup] The LABEL: the corner affordance that unfolds the panel. It is the word "Netplay"
    // and the Namek plate inside one rounded gold box, and the whole thing is the button -- the
    // plate is not a control of its own, so the label is what you call it. Small thing on screen,
    // panel on demand; nothing about the menu behind it is covered until you ask for it.
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const float margin  = 18.0f;
    const ImVec2 br(vp->Pos.x + vp->Size.x - margin, vp->Pos.y + vp->Size.y - margin);

    // NoBackground is what keeps ImGui's own frame out of it. A zero alpha only hides the fill;
    // ImGui strokes the window's border out of the same colour, so with NoDecoration alone the
    // label still came out with a second gold line around it that nothing here had asked for. The
    // one rectangle on screen is the pill drawn below.
    // [netjump] NoInputs while the curtain is up. The curtain is a rectangle on the foreground draw
    // list, which is paint only -- ImGui hit-tests by window rectangle, not by z-order -- so
    // without this the label and the panel stay clickable while being invisible behind the black.
    // A click there would land on a control nobody can see.
    const bool curtainUp = g_netCurtainWant.load(std::memory_order_relaxed) != 0;
    ImGuiWindowFlags labelFlags = ImGuiWindowFlags_NoDecoration |
                                  ImGuiWindowFlags_NoBackground |
                                  ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoSavedSettings |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus |
                                  ImGuiWindowFlags_NoNav |
                                  ImGuiWindowFlags_AlwaysAutoResize;
    if (curtainUp)
        labelFlags |= ImGuiWindowFlags_NoInputs;

    loadNetplayIcon();

    // [mmpopup] The metrics live out here, not inside the window: the panel below has to know how
    // tall the label is to sit above it, and CalcTextSize needs a font, which only exists once the
    // overlay's frame is open.
    static const char *kLabel = "Netplay";
    const ImVec2 labelSz = ImGui::CalcTextSize(kLabel);
    const float padX  = 15.0f;
    const float gap   = 10.0f;
    const float artSz = 34.0f;   // planet width; its 464x524 aspect makes it taller than this
    const float pillH  = 48.0f;
    const float pillW  = padX + labelSz.x + gap + artSz + padX;
    // A pill, not a rounded box: the radius is half the height, so the ends are semicircles.
    const float pillR = pillH * 0.5f;

    bool clicked = false;
    ImGui::SetNextWindowPos(br, ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    if (ImGui::Begin("##mm_popup_label", nullptr, labelFlags))
    {
        // One InvisibleButton for the whole pill, so the label is clickable too and not just the
        // artwork -- a 34px planet is a poor thing to ask someone to hit on its own.
        ImGui::InvisibleButton("##icon", ImVec2(pillW, pillH));
        clicked = ImGui::IsItemClicked();
        const bool hovered = ImGui::IsItemHovered();

        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImU32 accent = ImGui::GetColorU32(ImVec4(1.00f, 0.80f, 0.30f,
                                                       hovered ? 1.00f : 0.72f));
        // [mmpopup] The latent glow: a soft halo of the same gold, breathing on a 3.5s sine, always
        // on. This is not the status pill's flash -- that one answers a change of state. This one is
        // the idle "there is something down here", the thing a corner affordance needs to be
        // noticed at all when the menu behind it is already busy. Kept cheap on purpose: three 1px
        // rounded strokes, no shader, no texture, nothing to allocate.
        //
        // It dims to a third while the panel is out. Open, the label has already done its job and a
        // glow behind a panel you are reading is just noise.
        const float breath = 0.5f + 0.5f * std::sin(float(ImGui::GetTime() / 3.5 * 6.28318530718));
        const float glow   = (0.09f + 0.11f * breath) * (s_mmPopupOpen ? 0.34f : 1.0f);
        for (int i = 3; i >= 1; --i)
        {
            const float e = 1.5f * float(i);
            dl->AddRect(ImVec2(a.x - e, a.y - e), ImVec2(b.x + e, b.y + e),
                        ImGui::ColorConvertFloat4ToU32(
                            ImVec4(1.00f, 0.80f, 0.30f, glow * (4.0f - float(i)) / 3.0f)),
                        pillR + e, 0, 1.0f);
        }

        // The box itself: border only. The menu shows through, same as the plate did on its own --
        // a filled box here would cover the rows the panel is offering to jump to. The stroke picks
        // up a little of the breath so the border and its halo are one object.
        dl->AddRect(a, b, ImGui::GetColorU32(ImVec4(1.00f, 0.80f, 0.30f,
                                                     (hovered ? 1.00f : 0.72f) + 0.10f * breath)),
                    pillR, 0, s_mmPopupOpen ? 2.0f : 1.0f);

        // The label, left of the artwork, vertically centred on the pill.
        dl->AddText(ImVec2(a.x + padX, (a.y + b.y) * 0.5f - labelSz.y * 0.5f), accent, kLabel);

        // The plate, in the right-hand slot of the pill.
        const float cx = b.x - padX - artSz * 0.5f;
        const float cy = (a.y + b.y) * 0.5f;
        if (g_netplayIcon.ok())
        {
            const float ph = artSz * float(g_netplayIcon.ph) / float(g_netplayIcon.pw);
            const ImVec2 pp(cx - artSz * 0.5f, cy - ph * 0.5f);
            // Clipped to the slot, not to the pill: the orbit reaches past the planet's own width,
            // and unclipped the cloud would print over the label and over the border.
            dl->PushClipRect(ImVec2(cx - artSz * 0.5f, cy - ph * 0.5f),
                             ImVec2(cx + artSz * 0.5f, cy + ph * 0.5f), true);
            dl->AddImage(ImTextureRef((ImTextureID)g_netplayIcon.planet), pp,
                         ImVec2(pp.x + artSz, pp.y + ph));

            // The same cloud on the same orbit the panel uses, with the design's ratios kept:
            // 78px of cloud and a 323x105 ellipse inside a 232px planet.
            const float cw = artSz * 0.336f;
            const float ch = cw * float(g_netplayIcon.ch) / float(g_netplayIcon.cw);
            const float t = float(ImGui::GetTime() / 6.0) - float((int)(ImGui::GetTime() / 6.0));
            float ox = 0.0f, oy = 0.0f;
            netOrbitPoint(t, cx, cy, artSz * 0.697f, artSz * 0.226f, -18.0f, ox, oy);
            dl->AddImage(ImTextureRef((ImTextureID)g_netplayIcon.cloud),
                         ImVec2(ox - cw * 0.5f, oy - ch * 0.5f),
                         ImVec2(ox + cw * 0.5f, oy + ch * 0.5f),
                         ImVec2(1.0f, 0.0f), ImVec2(0.0f, 1.0f));
            dl->PopClipRect();
        }
        else
        {
            // No art: three bars where the planet goes, rather than an empty slot.
            const float w = artSz * 0.44f, h = 2.5f;
            for (int i = 0; i < 3; ++i)
            {
                const float y = cy + (float(i) - 1.0f) * 7.0f;
                dl->AddRectFilled(ImVec2(cx - w, y - h), ImVec2(cx + w, y + h), accent, h);
            }
        }
    }
    ImGui::End();
    if (clicked)
        s_mmPopupOpen = !s_mmPopupOpen;

    // [mmpopup] Chase the target. A click in the middle of the animation reverses it, which is what
    // makes a toggle feel like a toggle and not a queue of two animations.
    {
        const float dt = ImGui::GetIO().DeltaTime;
        const float k  = 1.0f - std::exp(-dt / 0.11f);
        s_mmPanelAnim += ((s_mmPopupOpen ? 1.0f : 0.0f) - s_mmPanelAnim) * k;
        if (std::fabs(s_mmPanelAnim - (s_mmPopupOpen ? 1.0f : 0.0f)) < 0.004f)
            s_mmPanelAnim = s_mmPopupOpen ? 1.0f : 0.0f;   // settle, so the last frame is exact
    }
    // Fully retracted: stop drawing. The panel has to keep being drawn on the way OUT (that is the
    // animation), but once it is at 0 there is nothing left to show and this is the frame that lets
    // the gate shut without the panel being a window that never ends.
    if (s_mmPanelAnim <= 0.0f)
        return;

    // The panel unfolds above and to the left of the pill, so it grows into the screen instead of
    // off the bottom-right edge. 520 is what the two-column form needs: the settings window this
    // content came from is min(1080, vpW*0.96), and inline labels do not survive in anything
    // narrower, which is why the popup's labels sit above their widgets instead.
    const ImVec2 panelSize(520.0f, 0.0f);
    // It comes UP out of the pill: 30px of travel, so it reads as the panel rising off the button
    // that opened it rather than as a window fading in place.
    const float slide = (1.0f - s_mmPanelAnim) * 30.0f;
    ImGui::SetNextWindowPos(ImVec2(br.x, br.y - pillH - 10.0f + slide),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSize(panelSize, ImGuiCond_Always);
    ImGuiWindowFlags panelFlags = ImGuiWindowFlags_NoDecoration |
                                  ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoSavedSettings |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus |
                                  ImGuiWindowFlags_NoNav |
                                  ImGuiWindowFlags_AlwaysAutoResize;
    if (curtainUp)   // [netjump] same reason as the label: behind the curtain, unreachable
        panelFlags |= ImGuiWindowFlags_NoInputs;
    // Opaque, or the game shows through and the readings are unreadable over moving artwork; the
    // alpha rides the animation so the panel fades in with its travel instead of appearing at full
    // strength and then sliding.
    // ImGuiStyleVar_Alpha is what fades the CONTENT. There is no global alpha in ImGui, and fading
    // each widget's colour by hand would mean finding every one of them; this multiplies them all.
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, s_mmPanelAnim);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, surface(0.96f * s_mmPanelAnim));
    if (!ImGui::Begin("##mm_popup_panel", nullptr, panelFlags))
    {
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        return;
    }

    loadNetplayIcon();
    drawNetplayPopupBody();
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void PS2SettingsOverlay::drawPerfHud()
{
    // [perf] Medidor de esquina, arriba a la derecha. Solo el numero de presents por segundo: es lo
    // unico que se lee de un vistazo sin tapar nada. Lo demas (p50/p95, gpu, cpu) esta en la pestana
    // Video, que se abre cuando uno quiere el detalle.
    //
    // El dato se actualiza una vez por segundo, asi que no tiene sentido cambiar el texto 60 veces
    // por segundo. Se limita la ACTUALIZACION a 4 Hz, no el dibujado: una ventana de ImGui que no
    // se abre un frame simplemente no existe ese frame, asi que throttlear con un return temprano
    // la hacia parpadear. Se dibuja siempre, con el ultimo valor conocido.
    static double s_lastShown = -1.0;
    static float s_shownAt = -1.0f;

    const ps2x::PerfStatus pf = ps2x::GetPerfStatus();
    if (!pf.valid)
        return;

    const float now = ImGui::GetTime();
    if (s_shownAt < 0.0f || now - s_shownAt >= 0.25f)
    {
        s_shownAt = now;
        s_lastShown = pf.displayFps;
    }

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const float margin = ImGui::GetFontSize() * 0.75f;
    // Pivote (1,0): la BORDE derecho de la ventana cae en el del viewport, asi el ancho variable de
    // AlwaysAutoResize nunca la empuja fuera de la pantalla.
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - margin, vp->Pos.y + margin),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));

    // NoInputs es lo que garantiza que no se coma clics: sin eso la ventana se traga el raton en la
    // esquina. NoDecoration quita borde, titulo y boton de cerrar.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration
                                 | ImGuiWindowFlags_NoInputs
                                 | ImGuiWindowFlags_AlwaysAutoResize
                                 | ImGuiWindowFlags_NoSavedSettings
                                 | ImGuiWindowFlags_NoFocusOnAppearing;
    if (!ImGui::Begin("##bt3_perf_hud", nullptr, flags))
    {
        ImGui::End();
        return;
    }

    // Acento para el numero y apagado para la unidad, igual que el resto del panel.
    ImGui::PushStyleColor(ImGuiCol_Text, accent());
    ImGui::Text("%.1f", s_lastShown);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 3.0f);
    ImGui::TextDisabled("fps");
    if (pf.displayRefreshHz > 0)
    {
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextDisabled("/ %d Hz", pf.displayRefreshHz);
    }

    ImGui::End();
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

    // [perf] Rama propia para cuando el panel esta retraido, que es el estado normal. El HUD y el
    // panel NUNCA coexisten, y por eso esto no necesita un segundo frame: UiBegin() hace
    // ImGui::NewFrame() y UiEnd() hace ImGui::Render(), o sea que el par es un frame completo y
    // solo puede haber uno por iteracion. La rama del panel de mas abajo queda intacta.
    if (m_animT <= 0.0001f)
    {
        // [mmpopup] The main-menu test plate needs a frame of its own when the panel is retracted
        // and the perf HUD is off, which is the usual state on the main menu. Without this the
        // function returns before UiBegin() and the plate never appears at all.
        const bool mmPopup = mainMenuPopupWanted();
        // [netjump] The curtain is NOT behind the popup test env. It was, which meant that in a real
        // two-instance session -- where PS2X_MAINMENU_POPUP_TEST is not set -- mainMenuPopupWanted()
        // was false, this function returned before UiBegin(), and the transition ran with no curtain
        // and no "Loading..." at all. A test switch controlling a shipping behaviour is the wrong way
        // round; the curtain answers the netjump and nothing else.
        const bool curtain = netCurtainBusy();
        // [notify] A card on screen is a reason to open a frame here even with nothing else to
        // draw: an unlock can land in a fight, where the panel is retracted and the main-menu popup
        // does not apply at all, and a session can connect while the player is anywhere.
        if (!m_settings.showPerf && !mmPopup && !curtain && m_cards.empty() &&
            ps2xNotifyPending() == 0)
            return;
        try
        {
            ps2x::gfx::UiBegin();
            pushDbzTheme();
            DbzThemeScope dbzTheme;   // pops all 40 style colours on scope exit
            if (m_settings.showPerf)
                drawPerfHud();
            if (mmPopup)
                drawMainMenuPopup();
            // [netjump] Last, so it covers the label and the panel: during the transition the player
            // must not be able to see, or click, the popup that started it.
            drawNetCurtain();
            // [notify] Above the curtain, on purpose, and this is the one place the two overlap.
            // The curtain is the netplay transition and it deliberately hides the thing that
            // started it; but "your session connected" is the reason the curtain is up, and hiding
            // that would leave the player looking at a black screen with no explanation. The card is
            // in the opposite corner from the netplay label, so nothing of the curtain's own
            // furniture is uncovered.
            drawNotifyStack();
        }
        catch (...)
        {
            // Misma politica que el panel: un fallo dibujando nunca debe matar el juego.
        }
        ps2x::gfx::UiEnd();
        return;
    }

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

        // [perf] El HUD va ANTES del fade: ScopedStyleVar animAlpha tiene scope hasta el final del
        // try, asi que cualquier ventana abierta despues heredaria la opacidad del panel y el medidor
        // se desvaneceria con el. Acá va a opacidad completa siempre.
        if (m_settings.showPerf)
            drawPerfHud();

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
                if (ImGui::BeginTabItem("  Achievements"))
                {
                    m_activeTab = 5;
                    drawAchTab();
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
        drawNetCurtain();   // [netjump] over the settings panel too
        // [notify] Outside the alpha scope above, for the same reason the perf meter is: a card
        // would otherwise inherit the panel's deploy fade and be unreadable while the panel is
        // animating, and the two things it reports are not the panel's business.
        drawNotifyStack();
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

    // [notify] The notification sounds ride the SFX slider, so they belong in the same section as
    // the thing they ride rather than in a tab named after whichever feature happens to raise the
    // most of them. There is one switch for all of them: a player who wants the game quiet does not
    // want a chime from a feature they have never turned on.
    ImGui::Spacing();
    {
        bool sound = ps2xNotifySoundEnabled();
        if (toggleSwitch("Notification sounds", &sound))
        {
            ps2xNotifySetSoundEnabled(sound);
            m_dirty = true;
        }
        ImGui::TextDisabled("The short tones that go with the popups in the top-left corner "
                            "(netplay, achievements). On by default; follows the SFX volume above.");
    }

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

        // [perf] Live frame pacing. The nominal refresh is already in hand from the Monitor row, and
        // showing it NEXT TO the measured rate is the whole trick: a bare "60" is unreadable (vsync
        // locked? or a coincidence?), while "59.8 / 60 Hz" says at a glance whether the present is
        // being held to the panel's rate or is running free.
        if (m_settings.showPerf)
        {
            const ps2x::PerfStatus pf = ps2x::GetPerfStatus();
            if (!pf.valid)
            {
                std::snprintf(val, sizeof val, "waiting for the first frame");
                dot(ps2x::VideoState::Fallback, "FPS", val, "");
            }
            else
            {
                std::snprintf(val, sizeof val, "%.1f", pf.displayFps);
                std::snprintf(note, sizeof note, "%s",
                              pf.displayRefreshHz > 0 ? "" : "  (monitor refresh unknown)");
                if (pf.displayRefreshHz > 0)
                    std::snprintf(note, sizeof note, "de %d Hz", pf.displayRefreshHz);
                dot(ps2x::VideoState::Ok, "FPS", val, note);

                // p50 alone hides stutter completely: a mean of 16.7 can be all 16 ms frames plus
                // one 80 ms hitch, which is exactly what the 60fps patch and the Windows
                // micro-freezes look like. p95 is the frame that actually happened.
                std::snprintf(val, sizeof val, "%.1f / %.1f / %.1f ms",
                              static_cast<double>(pf.frameMsP50), static_cast<double>(pf.frameMsP95),
                              static_cast<double>(pf.frameMsMax));
                std::snprintf(note, sizeof note, "p50 / p95 / max  de %d frames", pf.frameSamples);
                dot(pf.frameMsP95 > 1000.0 / 45.0 ? ps2x::VideoState::Fallback : ps2x::VideoState::Ok,
                    "Frame", val, note);

                // The GPU line names its own source and coverage. Without that, a 0 here is
                // unreadable -- it used to mean "not measured" on two of the three backends.
                const bool noGpu = pf.gpuSource == ps2x::GpuSource::SoftwareCpu
                                || pf.gpuSource == ps2x::GpuSource::None;
                if (noGpu)
                {
                    std::snprintf(val, sizeof val, "n/d");
                    std::snprintf(note, sizeof note, "%s",
                                  pf.gpuSource == ps2x::GpuSource::SoftwareCpu
                                      ? "rasterized by CPU, there is no GPU to measure"
                                      : "the renderer reports no GPU timings");
                    dot(ps2x::VideoState::Fail, "GPU", val, note);
                }
                else if (!pf.gpuMeasured())
                {
                    // The backend is live but collected nothing this window. Measured live: three
                    // windows in a row went 61% -> 0.65% -> 0.61% only because the guest stopped
                    // issuing draw lists. A 0% here would read as an idle GPU, which is the opposite
                    // of the truth, so say what actually happened.
                    std::snprintf(val, sizeof val, "no samples");
                    std::snprintf(note, sizeof note, "the game emitted no draw list in this second");
                    dot(ps2x::VideoState::Fallback, "GPU", val, note);
                }
                else
                {
                    std::snprintf(val, sizeof val, "%.0f %%", pf.gpuBusyPct);
                    const char *src = pf.gpuSource == ps2x::GpuSource::VulkanTimestamps ? "Vulkan, frame completo"
                                    : pf.gpuSource == ps2x::GpuSource::OpenGL ? "OpenGL" : "?";
                    if (pf.gpuCoverage >= 1.0)
                        std::snprintf(note, sizeof note, "%s  -  %.1f ms/frame, %d muestras", src, pf.gpuMsPerFrame, pf.gpuSamples);
                    else
                        std::snprintf(note, sizeof note,
                                      "%s, draw-list only: it is a minimum  -  %.1f ms/frame, %d samples",
                                      src, pf.gpuMsPerFrame, pf.gpuSamples);
                    dot(ps2x::VideoState::Ok, "GPU", val, note);
                }

                std::snprintf(val, sizeof val, "invitado %.0f %%  submit %.0f %%", pf.guestPct, pf.submitPct);
                std::snprintf(note, sizeof note, "of CPU time, over the real time");
                dot(ps2x::VideoState::Ok, "CPU", val, note);
            }
        }
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
        // [perf] Toggling this also arms the runtime, which is what turns the GPU timing queries on:
        // they are not free, so nothing should pay for them unless someone is reading the numbers.
        if (toggleSwitch("FPS meter (corner)", &m_settings.showPerf))
        {
            ps2x::SetPerfOverlayEnabled(m_settings.showPerf);
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
                ImGui::TextDisabled("Install one from the front-end (Misc tab) or set PS2X_TEXREPLACE=<dir>.");
            }
            ImGui::Separator();
            if (!havePack) ImGui::BeginDisabled();
            if (toggleSwitch("4K intro video", &m_settings.introVideo))
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
            // edited here survive a restart and reach the front-end's Bindings tab.
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
    // The values live in NetplayForm (see above drawMainMenuPopup), shared with the main-menu
    // popup: they were statics local to this function, so the two views could not see each other.
    g_netForm.seed();

    ImGui::TextUnformatted("Online play (deterministic lockstep)");
    ImGui::Separator();

    // [netplay] The overlay switch, first thing in the tab, because everything else on this screen
    // only matters if it is on: the main-menu label, the panel and the automatic transition. Reads
    // and writes the same saved setting as the launcher's Misc page, so the two cannot disagree.
    if (toggleSwitch("Netplay overlay", &m_settings.netOverlay))
    {
        ps2xSetNetOverlayEnabled(m_settings.netOverlay);
        m_dirty = true;
    }
    ImGui::TextDisabled("A Netplay label in the corner of the main menu, and the walk to character "
                        "select with a black curtain when a session connects. Default: NET_OVERLAY.");

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

    // [rollback] The rollback window and state-sync controls are hidden until rollback ships (2026-09-17):
    // netplay is lockstep. The environment defaults (PS2X_NETROLLBACK / state sync) still reach the
    // connect calls below, so developers can keep testing without the UI advertising it.
    static int  s_rollback = ps2NetRollbackSetting();
    static bool s_sync = ps2NetSyncSetting();
    ImGui::Checkbox("Go to character select once connected", &g_netForm.jump);
    ImGui::TextDisabled("The HOST's choice applies to both; the menus are hidden while it happens.");
    ImGui::Separator();
    // Only Join uses the address: hosting binds the port and learns the peer from its first
    // packet, which is why only one side needs a reachable port.
    ImGui::InputText("Host address (Join only)", g_netForm.peer, sizeof g_netForm.peer);
    ImGui::InputInt("Port", &g_netForm.port);
    const char *kBattle[] = { "Single Battle", "Team Battle", "DP Battle" };
    ImGui::Combo("Game mode", &g_netForm.battle, kBattle, 3);
    // DP Battle's point budget is a SEPARATE row of the versus menu (duelObj+0x118, committed to
    // stateObj+0x630 = RetroAchievements' 0x6af7b0). Selecting DP without it left the screen
    // playing like Team Battle: the right type with no budget behind it.
    if (g_netForm.battle == 2)
    {
        const char *kDp[] = { "10 DP", "15 DP", "20 DP" };
        ImGui::Combo("DP limit", &g_netForm.dp, kDp, 3);
    }
    // Battle Settings time-limit indices, confirmed in game:
    //   0 = 60 s, 1 = 90 s, 2 = 180 s, 3 = 240 s (default), 4 = no limit
    const char *kTime[] = { "60 seconds", "90 seconds", "180 seconds", "240 seconds (default)", "No limit" };
    ImGui::Combo("Time limit", &g_netForm.time, kTime, 5);
    ImGui::TextDisabled("The HOST's choices apply to both players.");
    ImGui::SliderInt("Input delay (frames)", &g_netForm.delay, 1, 10);
    ImGui::TextDisabled("BT3 runs at 30 fps, so each frame is 33 ms. Use 1 on the same machine,");
    ImGui::TextDisabled("2 on a LAN. Raise it only if you see stalls.");
    if (g_netForm.port < 1 || g_netForm.port > 65535) g_netForm.port = 7777;

    if (ImGui::Button("Host (you are Player 1)"))
    {
        ps2NetSetAutoJump(g_netForm.jump); ps2NetSetDelay(g_netForm.delay);
        ps2NetSetBattleType(g_netForm.battle);
        ps2NetSetTimeLimit(g_netForm.time); ps2NetSetDpLimit(g_netForm.dp);
        ps2NetSetRollback(s_rollback); ps2NetSetSync(s_sync);
        ps2NetHost(g_netForm.port, 1);
    }
    ImGui::SameLine();
    if (ImGui::Button("Join (you are Player 2)"))
    {
        ps2NetSetAutoJump(g_netForm.jump); ps2NetSetDelay(g_netForm.delay);  // host's mode wins
        ps2NetSetRollback(s_rollback); ps2NetSetSync(s_sync);
        char hp[96]; std::snprintf(hp, sizeof hp, "%s:%d", g_netForm.peer, g_netForm.port);
        ps2NetJoin(hp, 2);
    }
    ImGui::Separator();
    ImGui::TextWrapped("HOST: just press Host -- leave the address blank, give the other player "
                       "your IP and this port. JOIN: type the host's IP above, then press Join. "
                       "Only the host needs the UDP port reachable.");
}

// [ach] Badges for the achievement list. The PNGs are downloaded by scripts/gen_ach_patch.py into
// assets/badges/<id>.png -- at build time, because nothing in the runtime is allowed to touch the
// network -- and decoded here once per id.
//
// Keyed by ACHIEVEMENT ID, not by list index. The list is sorted (earned first, then by id) and the
// id is the only thing that survives a re-sort, a patch reload and a rebuild; an index-keyed cache
// would show one achievement's badge on another's row the moment the order changed.
struct AchBadge
{
    unsigned long long tex = 0;   // what UiLoadTextureRgba returns; 0 = none
    bool tried = false;
};

namespace {
constexpr float kBadge = 26.0f;   // a badge is 64x64 on RA; this is a row, not a gallery
std::vector<AchBadge> g_achBadges;
std::unordered_map<uint32_t, size_t> g_achBadgeIndex;
}   // namespace

static const AchBadge &achBadgeAt(int index)
{
    static const AchBadge kNone;
    if (index < 0 || index >= static_cast<int>(g_achBadges.size()))
        return kNone;
    return g_achBadges[static_cast<size_t>(index)];
}

// Decodes anything that has not been attempted yet, and re-uses the existing entry otherwise.
static void ensureAchBadges(int total)
{
    if (static_cast<int>(g_achBadges.size()) < total)
        g_achBadges.resize(static_cast<size_t>(total));

    std::error_code ec;
    const char *envDir = std::getenv("PS2X_ACH_BADGES");
    const std::string dir = (envDir && envDir[0]) ? envDir : "assets/badges";

    for (int i = 0; i < total; ++i)
    {
        AchBadge &slot = g_achBadges[static_cast<size_t>(i)];
        if (slot.tried)
            continue;
        slot.tried = true;

        const uint32_t id = ps2xAchIdAt(i);
        if (!id)
            continue;

        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        const std::string path = dir + "/" + std::to_string(id) + ".png";
        if (ps2x::gfx::GsDecodeImageRGBA8(path.c_str(), rgba, w, h) && w > 0 && h > 0)
            slot.tex = ps2x::gfx::UiLoadTextureRgba(rgba.data(), w, h);
        // A missing badge is not a failure worth a log line every frame: a custom achievement has
        // none by definition, and the row falls back to its bullet.
    }
}

// [ach] The Achievements tab. The switch, the counts, and the list of what is actually being
// evaluated -- which is deliberately a short list, and the tab says so rather than showing 154
// rows with 149 of them greyed out. A list of everything the database holds would be a promise the
// build cannot keep: those conditions point at addresses this recomp does not use, and evaluating
// them would report unlocks that did not happen.
void PS2SettingsOverlay::drawAchTab()
{
    ImGui::Spacing();

    sectionHeader("ACHIEVEMENTS");

    if (toggleSwitch("Achievements", &m_settings.achievements))
    {
        ps2xSetAchEnabled(m_settings.achievements);
        m_dirty = true;
    }
    ImGui::TextDisabled("Tracked locally: the definitions are a file in assets/, your progress is "
                        "a file in savedata/achievements.progress. No account, no network. "
                        "Default: ACHIEVEMENTS.");

    // [ach] Attribution, and it is not decoration. The 154 achievements and their names, points and
    // conditions are RetroAchievements' work, not ours; the badges are their images too. This build
    // reads their public definitions and evaluates them locally, which is a thing they support --
    // rcheevos is their own library, vendored under its MIT licence. Saying so here, where the
    // list is, is the right place for it: the player is looking at their content and should know
    // where it came from.
    ImGui::Spacing();
    {
        ScopedStyleColor c(ImGuiCol_Text, dbz(0.50f, 0.55f, 0.62f));
        ImGui::TextWrapped("Achievements and badges are the work of RetroAchievements.org, "
                           "used here under their public definitions. The rcheevos library that "
                           "evaluates them is theirs too, MIT licensed. Thank you.");
    }

    ImGui::Spacing();

    const int total = ps2xAchTotal();
    if (total <= 0)
    {
        ImGui::TextWrapped("%s",
                           m_settings.achievements
                               ? "Nothing loaded yet. The counts appear once the game has run a "
                                 "frame -- the patch is read at that point, not before."
                               : "Turn the switch on to start tracking. It costs one condition "
                                 "evaluation per presented frame.");
        return;
    }

    char head[220];
    int verified = 0;
    for (int i = 0; i < total; ++i)
        verified += ps2xAchAddrVerified(i) ? 1 : 0;
    std::snprintf(head, sizeof head, "%d of %d earned  --  %d of %d points  --  %d with a mapped address",
                  ps2xAchUnlocked(), total, ps2xAchPointsEarned(), ps2xAchPointsTotal(), verified);
    ImGui::TextUnformatted(head);
    if (verified < total)
    {
        ImGui::TextColored(dbz(0.78f, 0.62f, 0.28f, 1.0f),
                           "%d of them still read the address RetroAchievements wrote, which points "
                           "at unrelated memory here, so they cannot be earned by playing yet. An "
                           "unlock on one of those rows is not something you did.",
                           total - verified);
    }
    ImGui::Spacing();

    if (ImGui::BeginChild("##achlist", ImVec2(-1, 0), ImGuiChildFlags_Borders))
    {
        ImGui::Spacing();
        // [ach] Badges, decoded once, lazily, the first frame this tab is drawn. Doing it per frame
        // would decode four PNGs to redraw them unchanged; doing it at patch load would decode images
        // for a list nobody opened.
        ensureAchBadges(total);
        // Earned first, and within each group the patch's own order (by id, which is the order the
        // authors numbered them). Two passes over the list rather than a sort, because the order
        // belongs to the module -- the overlay draws it, it does not decide it -- and because
        // sorting a five-entry list on every frame to put a bool first is not a thing to do for a
        // cosmetic preference.
        for (int pass = 0; pass < 2; ++pass)
        {
            const bool wantUnlocked = (pass == 0);
            for (int i = 0; i < total; ++i)
            {
                char title[160] = {};
                char desc[320] = {};
                int points = 0;
                bool unlocked = false;
                if (!ps2xAchListGet(i, title, sizeof title, desc, sizeof desc, &points, &unlocked))
                    continue;
                if (unlocked != wantUnlocked)
                    continue;

                ImGui::PushID(i);
                {
                    // The badge, or a text bullet when there is none -- a custom achievement has no
                    // badge by definition. Both occupy the same 26 px column so the titles line up
                    // either way: a column that shifts when an image is missing is worse than no
                    // image, and the bullet is what carries the state when there is no art to.
                    const AchBadge &b = achBadgeAt(i);
                    if (b.tex)
                    {
                        ImGui::Image((ImTextureID)b.tex, ImVec2(kBadge, kBadge));
                    }
                    else
                    {
                        ImGui::Dummy(ImVec2(kBadge, kBadge));
                        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x,
                                                        ImGui::GetCursorScreenPos().y +
                                                            (kBadge - ImGui::GetTextLineHeight()) * 0.5f));
                        // Green for earned, dim for not.
                        ScopedStyleColor c(ImGuiCol_Text, unlocked ? dbz(0.45f, 0.85f, 0.50f)
                                                                    : dbz(0.62f, 0.62f, 0.66f));
                        ImGui::TextUnformatted(unlocked ? "*" : "-");
                    }
                }
                ImGui::SameLine();
                {
                    ScopedStyleColor c(ImGuiCol_Text, unlocked ? dbz(0.93f, 0.96f, 0.94f)
                                                                : dbz(0.62f, 0.62f, 0.66f));
                    ImGui::TextUnformatted(title);
                }
                // [ach] The honesty marker. 149 of these still read RetroAchievements' own
                // addresses, which in this build point at unrelated memory -- so an unlock on one of
                // them is not evidence the player did the thing. Marked in the row, because the
                // alternative is a list where an accidental unlock is indistinguishable from a real
                // one, and the whole point of loading them was to find out which is which.
                if (!ps2xAchAddrVerified(i))
                {
                    ScopedStyleColor c(ImGuiCol_Text, dbz(0.78f, 0.62f, 0.28f));
                    ImGui::SameLine();
                    ImGui::TextUnformatted("  (addr not mapped)");
                }
                {
                    ScopedStyleColor c(ImGuiCol_Text, gold());
                    const std::string pts = std::to_string(points) + " pts";
                    const float w = ImGui::CalcTextSize(pts.c_str()).x;
                    ImGui::SameLine(ImGui::GetContentRegionMax().x - w - 12.0f);
                    ImGui::TextUnformatted(pts.c_str());
                }
                if (desc[0])
                {
                    ImGui::Indent(24.0f);
                    {
                        ScopedStyleColor c(ImGuiCol_Text, dbz(0.45f, 0.45f, 0.50f));
                        ImGui::TextUnformatted(desc);
                    }
                    ImGui::Unindent(24.0f);
                }
                ImGui::Spacing();
                ImGui::PopID();
            }
        }
        ImGui::Spacing();
    }
    ImGui::EndChild();
}

// [ach] The unlock notification. Bottom-centre, above everything except the curtain -- the curtain
// is the netplay transition and is allowed to cover it, because during that transition the player
// is not meant to be looking at the screen at all.
//
// ---------------------------------------------------------------------------------------------
// [notify] The popup stack. Top-left, newest on top.
// ---------------------------------------------------------------------------------------------
//
// The corner is not arbitrary and not free. The netplay label owns the bottom-right of the main
// menu, the perf meter owns the top-right during a match, and the settings panel is centred. That
// leaves the top-left, and it is the right place on its own terms: it is the corner the eye goes to
// for something that was just added to the page, it is the furthest from the thumbstick's usual
// resting arc so it does not sit under the player's hand, and it is far enough from the netplay
// label that a session change and an unlock arriving together are two separate cards rather than
// one smear.
//
// The cards carry their producer's identity -- a coloured edge, a small glyph, and a source label
// in the accent -- so a netplay card and an achievement card are distinguishable at a glance
// without reading them. That is the whole reason the two are separate kinds and not one "message"
// type: the sharing is in the position and the motion, never in the identity.
//
// Motion: each card slides in from the left while fading up, holds, then fades out. Cards below
// ease toward their slot rather than snapping, so a new arrival pushes the stack down smoothly
// instead of making everything jump. One card animating never disturbs the ImGui layout of another:
// they are all absolutely positioned from the viewport, and they take no input.
void PS2SettingsOverlay::drawNotifyStack()
{
    constexpr int   kMaxLive = 5;
    // Sized to the content, not to a panel's worth of content. A card carries a small source label,
    // a title and at most one line of body, so anything wider than this is a rectangle with a
    // sentence in it. 292 px fits the longest title in the patch ("Now Where's My Trucker Hat...")
    // and the longest body ("Coming back to the main menu after all this time. It has been a
    // while.") at the sizes below, which is what set the number rather than taste.
    constexpr float kW = 292.0f, kPadX = 11.0f, kPadY = 8.0f;
    constexpr float kEdge = 2.5f;              // the producer's accent, as a left rule
    constexpr float kIn = 0.20f, kOut = 0.45f;
    constexpr float kHoldAch = 3.6f, kHoldNet = 2.8f;
    constexpr float kGap = 6.0f;
    constexpr float kMarginL = 20.0f, kMarginT = 18.0f;
    // Three text rows at their own sizes, plus the padding, rather than a frame-height formula. The
    // label is small, the title is the body size, and the body is slightly under it; adding those
    // up is the only way the card is exactly as tall as what is on it.
    const float rowLabel = ImGui::GetFontSize() * 0.82f;
    const float rowTitle = ImGui::GetFontSize() * 1.00f;
    const float rowBody = ImGui::GetFontSize() * 0.88f;
    const float rowH = kPadY * 2.0f + rowLabel + rowTitle + rowBody + 6.0f;

    // Newest on top, so the thing that just happened is the thing you see. The queue is drained in
    // order and prepended, which is why the drain is a ring and not an index into a sorted list.
    NotifyEvent incoming[kMaxLive];
    const int n = ps2xNotifyDrain(incoming, kMaxLive);
    for (int i = 0; i < n; ++i)
    {
        Card c;
        c.kind = incoming[i].kind;
        c.title = incoming[i].title ? incoming[i].title : "";
        c.body = incoming[i].body ? incoming[i].body : "";
        c.age = 0.0f;
        c.hold = (incoming[i].kind == NotifyKind::Achievement) ? kHoldAch : kHoldNet;
        c.y = 0.0f;   // starts at the target and is pulled out by the slide
        m_cards.insert(m_cards.begin(), c);
    }
    while (static_cast<int>(m_cards.size()) > kMaxLive)
        m_cards.pop_back();

    for (size_t i = 0; i < m_cards.size(); ++i)
    {
        Card &c = m_cards[i];
        c.age += ImGui::GetIO().DeltaTime;
        c.y += ((static_cast<float>(i) * (rowH + kGap)) - c.y) *
               (1.0f - std::exp(-ImGui::GetIO().DeltaTime / 0.09f));
    }
    // Retire from the back: the tail is the oldest, and retiring the front would make the cards
    // below it jump a slot as they take the index of a card that is still on screen.
    while (!m_cards.empty() &&
           m_cards.back().age >= kIn + m_cards.back().hold + kOut)
        m_cards.pop_back();
    if (m_cards.empty())
        return;

    // The FOREGROUND draw list, not the window one. This runs outside any ImGui window -- the
    // retracted branch of draw() opens no window at all -- and GetWindowDrawList() with no current
    // window hands back the implicit "Debug##Default" one, which ImGui then draws as a large empty
    // frame. That is not a cosmetic mistake: it means the cards were being positioned in that
    // window's coordinate space instead of the screen's, so the top-left margin was measured from
    // wherever that window happened to be. GetForegroundDrawList() has no window of its own and its
    // coordinates are the viewport's, which is what "the top-left corner" means.
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const ImVec2 vp = ImGui::GetIO().DisplaySize;

    for (const Card &c : m_cards)
    {
        const float t = c.age;
        const float alpha = t < kIn ? (t / kIn)
                                    : (t < kIn + c.hold
                                           ? 1.0f
                                           : std::max(0.0f, 1.0f - (t - kIn - c.hold) / kOut));
        if (alpha <= 0.001f)
            continue;
        const float slide = t < kIn ? (1.0f - t / kIn) : 0.0f;   // 0 = settled, 1 = still coming

        // The draw list wants packed colours, not ImVec4, and every colour on this card shares the
        // one alpha -- so the conversion is a local lambda that folds it in. Written out per colour
        // it would be six places to get the alpha argument wrong, and a card that faded its border
        // but not its text is worse than one that does not fade at all.
        const auto col = [alpha](float r, float g, float b, float a) {
            return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a * alpha));
        };

        const ImVec4 accent4 = (c.kind == NotifyKind::Achievement) ? gold()
                                                                  : ImVec4(0.36f, 0.74f, 0.92f, 1.0f);

        const float x = kMarginL - slide * (kW * 0.35f);
        const float y = kMarginT + c.y;
        const ImVec2 a(x, y), b(x + kW, y + rowH);

        // The panel colour is the overlay's own background, not a new one: the card has to belong
        // to this UI, and #001B39 is what the settings panel and the netplay popup already are.
        dl->AddRectFilled(a, b, col(0.043f, 0.106f, 0.227f, 0.94f));
        // Border at 1 px and a low-contrast blue. At 2 px and full contrast it reads as a framed
        // panel, and a 292x60 card with a 2 px frame looks like a window rather than a notification.
        dl->AddRect(a, b, col(0.14f, 0.19f, 0.28f, 0.70f), 2.0f, 0, 1.0f);
        // The accent rule, at the very left. This is the producer's colour and the only saturated
        // thing on the card, which is what makes a netplay card and an achievement card tell
        // themselves apart at a glance.
        dl->AddRectFilled(ImVec2(a.x, a.y + 1.5f), ImVec2(a.x + kEdge, b.y - 1.5f),
                          col(accent4.x, accent4.y, accent4.z, 1.0f));

        const ImVec2 textL(a.x + kEdge + kPadX, a.y + kPadY);

        // Three rows, each positioned from the one above it rather than from a frame height, so the
        // text lands inside the box the height was computed for. The y offsets are the same numbers
        // rowH was summed from, which is why the card is exactly as tall as what is on it.
        const float yLabel = textL.y;
        const float yTitle = yLabel + rowLabel;
        const float yBody = yTitle + rowTitle;

        // Source label, in the accent, small. The card's title is the loudest thing on it; this is
        // the quietest, because it is the part you already know.
        //
        // ImGui::GetFont(), and deliberately not m_fontHudLabel: that member is never assigned
        // anywhere in the tree, so it is a null ImFont*, and AddText dereferences it. Russo One IS
        // the default font -- initialize() makes it Fonts[0] with io.Fonts->Clear() first -- so
        // GetFont() is the HUD face and nothing needs pushing.
        {
            const char *src = (c.kind == NotifyKind::Achievement) ? "ACHIEVEMENT" : "NETPLAY";
            dl->AddText(ImGui::GetFont(), rowLabel, ImVec2(textL.x, yLabel),
                        col(accent4.x, accent4.y, accent4.z, 0.92f), src);
        }
        dl->AddText(ImGui::GetFont(), rowTitle, ImVec2(textL.x, yTitle),
                    col(0.93f, 0.95f, 0.98f, 1.0f), c.title.c_str());
        // Body, one line. Deliberately not wrapped: a card whose height depends on its text would
        // make the whole stack reflow as it animates. The strings in the tree fit; a longer one from
        // the drop box gets clipped at the card's edge, which is the right failure -- the card does
        // not grow and shove the stack down.
        if (!c.body.empty())
            dl->AddText(ImGui::GetFont(), rowBody, ImVec2(textL.x, yBody),
                        col(0.62f, 0.66f, 0.72f, 1.0f), c.body.c_str());

        // The glyph, right-aligned: a filled diamond for netplay (its own shape language, matching
        // the planet art), a five-point star for an achievement. Drawn rather than loaded, because
        // there is no art for either and two lines of geometry beat a new asset.
        const ImVec2 g(b.x - 26.0f, (a.y + b.y) * 0.5f);
        if (c.kind == NotifyKind::Achievement)
        {
            ImVec2 star[10];
            for (int k = 0; k < 10; ++k)
            {
                const float ang = -3.14159265f * 0.5f + k * 3.14159265f / 5.0f;
                const float rad = (k & 1) ? 5.0f : 11.5f;
                star[k] = ImVec2(g.x + std::cos(ang) * rad, g.y + std::sin(ang) * rad);
            }
            dl->AddConvexPolyFilled(star, 10, col(accent4.x, accent4.y, accent4.z, 1.0f));
        }
        else
        {
            // Solid, not outlined: AddConvexPolyFilled() has no thickness parameter, and faking a
            // ring with a second inner polygon just makes a dot inside a diamond. At 18 px this is
            // read as a shape, not as an outline.
            const ImVec2 d[4] = {ImVec2(g.x, g.y - 11.0f), ImVec2(g.x + 9.0f, g.y),
                                 ImVec2(g.x, g.y + 11.0f), ImVec2(g.x - 9.0f, g.y)};
            dl->AddConvexPolyFilled(d, 4, col(accent4.x, accent4.y, accent4.z, 0.92f));
        }
    }
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
    ImGui::TextDisabled("  optimizacion (perf/async), front-end + install wizard, input & gamepads, "
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
