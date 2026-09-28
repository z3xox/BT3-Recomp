#pragma once

#include <cstdint>
#include <string>
#include <vector>

// One settings.toml, one schema. The in-runtime front-end (src/frontend) and the in-game
// overlay (ps2_settings_overlay.cpp) both go through this module, so the two writers cannot
// drift apart the way the Qt launcher and the overlay did.
namespace ps2x_settings
{
    constexpr int kRendererOpenGL = 0;
    constexpr int kRendererSoftware = 1;
    constexpr int kRendererParallelGS = 2;
    constexpr int kRendererD3D11 = 3;

    struct Settings
    {
        float master = 1.0f;
        float music = 1.0f;
        float sfx = 0.4f;

#if defined(_WIN32)
        int renderer = kRendererOpenGL;
#else
        int renderer = kRendererParallelGS;
#endif
        // [netplay] The Netplay overlay: the corner label, the panel behind it and the automatic
        // character-select transition with its curtain. The checkbox lives in the launcher's Misc page
        // and in the overlay's own Netplay tab, so the switch is reachable without touching the
        // environment. NET_OVERLAY=0 still turns it off for one session, which is the point of having
        // an environment at all.
        bool netOverlay = true;
        // [ach] The local achievement tracker. On by default, like the Netplay overlay: the cost is
        // one condition evaluation per frame against RAM the game already has in cache, and the
        // switch is in the same two places. ACHIEVEMENTS=0 turns it off for one session.
        bool achievements = true;
        bool glow = true;
        bool glowFix = true;
        bool bilinear = true;
        bool halfTexel = true;
        bool skipPost = true;
        bool skipStaleVram = true;
        int renderScale = 1;
        bool outline = false;
        int inkStrength = 199;
        int inkWidth = 100;
        unsigned inkColor = 0;
        bool shadows = true;
        bool dofBlur = false;
        int dofZFar = 200000;
        bool fullscreen = false;
        int windowMode = 0;
        int monitor = 0;
        bool widescreen = false;
        // Preferred GPU, by DXGI adapter name. Empty means "let the OS decide". Only takes
        // effect on the next launch of the game window, since the OpenGL context is created by
        // SDL without an adapter argument.
        std::string gpu;
        int windowW = 0;
        int windowH = 0;
        // Front-end window size, remembered between runs. It lives here so the shell opens at
        // the size the user left it, and it is NOT what the game window uses: the game applies
        // windowW/windowH on PLAY. Deliberately left out of operator== (see ps2x_settings.cpp)
        // so resizing the window never marks the game settings as unsaved.
        int feWidth = 800;
        int feHeight = 480;   // was 600: the shell pages are all scrollable and the taller
                              // window just left empty space under the last row.
        // Menu-theme mute, also shell-only and also outside operator== for the same reason: the
        // toggle applies the moment it is pressed, so it must not read as an unsaved change.
        bool musicMuted = false;
        bool forceBilinear = true;
        // [ablend128] Full GS strength for texture-alpha blends (PS2X_ABLEND128). aBlend wants
        // GS_As/128 while texture alpha arrives as GSbyte/255, so without it every texture-alpha
        // blend runs at half strength (measured: a CLUT alpha-127 darkening applied at 0.498
        // instead of 0.992). A replacement is uploaded byte-for-byte and never passes through the
        // decoder's kAlpha128To255 expansion, so a full-range pack needs the full factor or every
        // replaced sprite reads translucent. The env still wins.
        bool ablend128 = true;
        bool texPack = false;
        bool introVideo = true;
            int buttonLayout = 1;
        bool fps60 = false;
        // [perf] Live fps / frame-time / GPU-busy readout in the overlay's Video tab. Off by default:
        // the GPU side costs a timing query per draw call, and nothing should pay that unless someone
        // is reading the numbers. Not env-guarded -- there is no env for it, it is purely a view.
        bool showPerf = false;
        int hudLayout = 0;
        int hudOffL = 0;
        int hudOffC = 0;
        int hudOffR = 0;

        int device = 0;
        float deadzone = 0.15f;
        bool overlayEnabled = true;
        std::string overlayPadBtns = "13,15";
        std::string overlayKeys = "340,258";

        int logLevel = 1;
        bool dumpAudio = true;
        bool dumpVideo = true;
        bool dumpControllers = true;
        bool dumpRuntime = true;
        bool dumpGamepad = false;
    };

    const char *rendererName(int renderer);
    int nameToRenderer(const std::string &name, int fallback);
    inline constexpr const char *kConfigFileName = "settings.toml";
    inline constexpr const char *kLegacyConfigFileName = "bt3_settings.ini";

    // Lets a caller keep a "last saved" snapshot and write only when something really moved.
    bool operator==(const Settings &a, const Settings &b);
    inline bool operator!=(const Settings &a, const Settings &b) { return !(a == b); }

    // <configDir>/settings.toml (bare "settings.toml" when configDir is empty).
    std::string configPath(const std::string &configDir);

    // Reads settings.toml; migrates a 0.x bt3_settings.ini once; writes the defaults when
    // neither exists. Values are clamped to the ranges the runtime accepts, so a hand-edited
    // file can never push the overlay out of range. Returns false when the file was missing
    // and the defaults had to be written instead.
    bool load(Settings &out, const std::string &configDir);

    // Pure read of one settings.toml: no migration, no writing, and `out` is left untouched when
    // the file is missing. A caller that only means to amend a few keys seeds itself with this
    // so every key it does not know about is carried over instead of snapping to a struct default.
    bool loadFromFile(Settings &out, const std::string &path);

    std::string serialize(const Settings &s);
    bool save(const Settings &s, const std::string &configDir);
    bool saveToFile(const Settings &s, const std::string &path);

    std::vector<int> parseIntCsv(const std::string &csv);
    std::string formatIntCsv(const std::vector<int> &values);

    // The keys whose effective value came from the environment instead of the file. PS2SettingsOverlay
    // sets these as it walks its own guards, so that walk stays the one definition of which keys are
    // env-overridable, and lives here only so applyOverlayValues() can be unit-tested without the
    // whole runtime.
    enum EnvLock : uint32_t
    {
        kLockGlow        = 1u << 0,
        kLockGlowFix     = 1u << 1,
        kLockInkStrength = 1u << 2,
        kLockBilinear    = 1u << 3,
        kLockHalfTexel   = 1u << 4,
        kLockSkipPost    = 1u << 5,
        kLockSkipStale   = 1u << 6,
        kLockRenderScale = 1u << 7,
        kLockOutline     = 1u << 8,
        kLockTexPack     = 1u << 9,
        kLockIntroVideo  = 1u << 10,
        kLockButtonLay   = 1u << 11,
        kLockShadows     = 1u << 12,
        kLockDofBlur     = 1u << 13,
        kLockDofZFar     = 1u << 14,
        // Set when video.renderer had to be bent because the build cannot do what the file asked
        // for. A retired renderer (d3d11) is a one-way migration and does get written back; a
        // build-capability fallback (paraLLEl-GS compiled out) must not, or re-enabling PGS later
        // finds the user's choice already overwritten.
        kLockRenderer    = 1u << 15,
    };

    // Folds the in-game overlay's live values into `out`, which the caller must have seeded from the
    // file (see loadFromFile). A locked key is simply not assigned, so the file's value stands:
    // PS2X_GLOW=0 for one session must not leave glow = false behind once the variable is gone.
    // Keys the overlay does not model are never mentioned here and ride along untouched.
    void applyOverlayValues(Settings &out, const Settings &live, uint32_t envLockedMask);
}
