#include "runtime/ps2x_settings.h"

#include "runtime/ps2_toml.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace
{
    using ps2x_settings::Settings;
    using ps2x_settings::kRendererD3D11;
    using ps2x_settings::kRendererOpenGL;
    using ps2x_settings::kRendererParallelGS;
    using ps2x_settings::kRendererSoftware;

    const char *kHeader =
        "# Dragon Ball Z: Budokai Tenkaichi 3 - Recompiled\n"
        "# User settings - written by the launcher and the in-game overlay.\n"
        "# Delete this file to reset everything to defaults.\n";

    std::string colorToHex(unsigned c)
    {
        char b[8];
        std::snprintf(b, sizeof b, "#%06x", c & 0xFFFFFFu);
        return b;
    }

    unsigned hexToColor(const std::string &s, unsigned def)
    {
        if (s.empty())
            return def;
        try
        {
            return static_cast<unsigned>(std::stoul(s[0] == '#' ? s.substr(1) : s, nullptr, 16)) & 0xFFFFFFu;
        }
        catch (...)
        {
            return def;
        }
    }

    std::vector<int> csvToInts(const std::string &s)
    {
        std::vector<int> out;
        std::istringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, ','))
            if (!tok.empty())
                try { out.push_back(std::stoi(tok)); } catch (...) {}
        return out;
    }

    std::string intsToCsv(const std::vector<int> &v)
    {
        std::string r;
        for (size_t i = 0; i < v.size(); ++i)
        {
            if (i)
                r += ",";
            r += std::to_string(v[i]);
        }
        return r;
    }

    int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
    float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

    // The runtime's own limits: the overlay clamps to these, so the front-end must too or the
    // value it writes would be silently changed the next time the overlay reads the file.
    void clamp(Settings &s)
    {
        s.master = clampf(s.master, 0.0f, 1.0f);
        s.music = clampf(s.music, 0.0f, 1.0f);
        s.sfx = clampf(s.sfx, 0.0f, 0.4f);
        s.inkStrength = clampi(s.inkStrength, 100, 400);
        s.inkWidth = clampi(s.inkWidth, 25, 100);
        s.renderScale = clampi(s.renderScale, 1, 4);
        s.dofZFar = clampi(s.dofZFar, 20000, 800000);
        s.windowMode = clampi(s.windowMode, 0, 2);
        s.monitor = clampi(s.monitor, 0, 16);
        s.device = clampi(s.device, 0, 100);
        s.deadzone = clampf(s.deadzone, 0.0f, 0.5f);
        s.logLevel = clampi(s.logLevel, 0, 3);
        s.buttonLayout = clampi(s.buttonLayout, 0, 1);
        s.hudLayout = clampi(s.hudLayout, 0, 2);
    }

    bool applyRenderer(Settings &s, int r)
    {
        // Direct3D 11 is retired on every platform, so a stale "d3d11" becomes the new OpenGL
        // present instead of selecting a backend that no longer exists.
        if (r == kRendererD3D11)
            r = kRendererOpenGL;
#if !defined(PS2X_HAVE_PGS)
        if (r == kRendererParallelGS)
            r = kRendererOpenGL;
#endif
        if (r < 0 || r > kRendererD3D11)
            return false;
        s.renderer = r;
        return true;
    }

    void loadHotkeys(Settings &s, const ps2x_toml::Document &doc)
    {
        std::vector<int> pb = doc.getIA("controllers.hotkey.pad_btns", csvToInts(s.overlayPadBtns));
        for (int &b : pb)
            b = clampi(b, 0, 31);
        if (!pb.empty())
            s.overlayPadBtns = intsToCsv(pb);

        std::vector<int> keys = doc.getIA("controllers.hotkey.keys", csvToInts(s.overlayKeys));
        for (int &k : keys)
            k = clampi(k, 32, 348);
        if (!keys.empty())
            s.overlayKeys = intsToCsv(keys);
    }

    bool loadToml(Settings &s, const std::string &path)
    {
        std::ifstream file(path);
        if (!file.is_open())
            return false;

        ps2x_toml::Document doc;
        doc.parse(file);

        s.master = static_cast<float>(doc.getD("audio.master_volume", s.master));
        s.music = static_cast<float>(doc.getD("audio.music_volume", s.music));
        s.sfx = static_cast<float>(doc.getD("audio.sfx_volume", s.sfx));

        applyRenderer(s, ps2x_settings::nameToRenderer(
                            doc.getS("video.renderer", ps2x_settings::rendererName(s.renderer)),
                            s.renderer));
        // [netplay] The environment is the default and the checkbox is the override, so a run with
        // NET_OVERLAY=1 and a saved "off" still starts off -- the setting is the user's last word.
        s.netOverlay = doc.getB("netplay.overlay", s.netOverlay);
        // [ach] Same relationship as netplay.overlay: the environment is the default and the
        // checkbox is the override, so ACHIEVEMENTS=0 with a saved "on" still starts off. Both ship
        // on, which is why the modules read those variables by presence rather than truthiness.
        s.achievements = doc.getB("achievements.enabled", s.achievements);
        s.glow = doc.getB("video.glow", s.glow);
        s.glowFix = doc.getB("video.glowfix", s.glowFix);
        s.inkStrength = doc.getI("video.ink_strength", s.inkStrength);
        s.inkWidth = doc.getI("video.ink_width", s.inkWidth);
        s.inkColor = hexToColor(doc.getS("video.ink_color", colorToHex(s.inkColor)), s.inkColor);
        s.bilinear = doc.getB("video.bilinear", s.bilinear);
        s.halfTexel = doc.getB("video.halftexel", s.halfTexel);
        s.skipPost = doc.getB("video.skippost", s.skipPost);
        s.skipStaleVram = doc.getB("video.skip_stale_vram", s.skipStaleVram);
        s.renderScale = doc.getI("video.render_scale", s.renderScale);
        s.outline = doc.getB("video.outline", s.outline);
        s.texPack = doc.getB("video.texture_pack", s.texPack);
        s.introVideo = doc.getB("video.intro_video", s.introVideo);
        s.buttonLayout = doc.getI("video.button_layout", s.buttonLayout);
        s.shadows = doc.getB("video.shadows", s.shadows);
        s.dofBlur = doc.getB("video.dof_blur", s.dofBlur);
        s.dofZFar = doc.getI("video.dof_zfar", s.dofZFar);
        s.fullscreen = doc.getB("video.fullscreen", s.fullscreen);
        s.windowMode = doc.getI("video.window_mode", s.fullscreen ? 2 : 0);
        s.monitor = doc.getI("video.monitor", s.monitor);
        s.gpu = doc.getS("video.gpu", s.gpu);
        s.widescreen = doc.getB("video.widescreen", s.widescreen);
        s.windowW = doc.getI("video.window_w", s.windowW);
    s.feWidth = clampi(doc.getI("frontend.width", s.feWidth), 640, 7680);
    s.feHeight = clampi(doc.getI("frontend.height", s.feHeight), 480, 4320);
    s.musicMuted = doc.getB("frontend.music_muted", s.musicMuted);
        s.windowH = doc.getI("video.window_h", s.windowH);
        s.forceBilinear = doc.getB("video.force_bilinear", s.forceBilinear);
        s.ablend128 = doc.getB("video.ablend128", s.ablend128);

        s.fps60 = doc.getB("video.fps60", s.fps60);
        s.showPerf = doc.getB("video.show_perf", s.showPerf);

        s.hudLayout = doc.getI("video.hud.layout", s.hudLayout);
        s.hudOffL = doc.getI("video.hud.offset_left", s.hudOffL);
        s.hudOffC = doc.getI("video.hud.offset_center", s.hudOffC);
        s.hudOffR = doc.getI("video.hud.offset_right", s.hudOffR);

        s.device = doc.getI("controllers.device", s.device);
        s.deadzone = static_cast<float>(doc.getD("controllers.deadzone", s.deadzone));
        s.overlayEnabled = doc.getB("controllers.overlay_enabled", s.overlayEnabled);
        loadHotkeys(s, doc);

        s.logLevel = doc.getI("logging.log_level", s.logLevel);
        s.dumpAudio = doc.getB("logging.dump_audio", s.dumpAudio);
        s.dumpVideo = doc.getB("logging.dump_video", s.dumpVideo);
        s.dumpControllers = doc.getB("logging.dump_controllers", s.dumpControllers);
        s.dumpRuntime = doc.getB("logging.dump_runtime", s.dumpRuntime);
        s.dumpGamepad = doc.getB("logging.dump_gamepad", s.dumpGamepad);

        clamp(s);
        return true;
    }

    bool loadIniLegacy(Settings &s, const std::string &path)
    {
        std::ifstream in(path);
        if (!in.is_open())
            return false;

        bool sawRenderer = false;
        std::string section;
        std::string line;
        while (std::getline(in, line))
        {
            const std::string t = ps2x_toml::trim(line);
            if (t.empty() || t[0] == '#' || t[0] == ';')
                continue;
            if (t[0] == '[')
            {
                const size_t end = t.find(']');
                if (end != std::string::npos)
                    section = ps2x_toml::trim(t.substr(1, end - 1));
                continue;
            }
            const size_t eq = t.find('=');
            if (eq == std::string::npos || eq == 0)
                continue;
            const std::string key = ps2x_toml::trim(t.substr(0, eq));
            const std::string val = ps2x_toml::trim(t.substr(eq + 1));
            const auto asBool = [&val] { return val == "1" || val == "true"; };
            const auto asInt = [&val](int def)
            {
                try { return static_cast<int>(std::stol(val)); } catch (...) { return def; }
            };
            const auto asFloat = [&val](float def)
            {
                try { return std::stof(val); } catch (...) { return def; }
            };

            if (section == "audio")
            {
                if (key == "master_volume") s.master = asFloat(s.master);
                else if (key == "music_volume") s.music = asFloat(s.music);
                else if (key == "sfx_volume") s.sfx = asFloat(s.sfx);
            }
            else if (section == "netplay")
            {
                if (key == "overlay") s.netOverlay = asBool();
            }
            else if (section == "achievements")
            {
                if (key == "enabled") s.achievements = asBool();
            }
            else if (section == "video")
            {
                const bool b = asBool();
                if (key == "renderer") { applyRenderer(s, asInt(s.renderer)); sawRenderer = true; }
                else if (key == "gpu_renderer" && !sawRenderer)
                    applyRenderer(s, b ? kRendererParallelGS : kRendererSoftware);
                else if (key == "glow") s.glow = b;
                else if (key == "glowfix") s.glowFix = b;
                else if (key == "bilinear") s.bilinear = b;
                else if (key == "halftexel") s.halfTexel = b;
                else if (key == "skippost") s.skipPost = b;
                else if (key == "skip_stale_vram") s.skipStaleVram = b;
                else if (key == "render_scale") s.renderScale = asInt(s.renderScale);
                else if (key == "outline") s.outline = b;
                else if (key == "shadows") s.shadows = b;
                else if (key == "dof_blur") s.dofBlur = b;
                else if (key == "dof_zfar") s.dofZFar = asInt(s.dofZFar);
                else if (key == "fullscreen") { s.fullscreen = b; s.windowMode = b ? 2 : 0; }
                else if (key == "widescreen") s.widescreen = b;
                else if (key == "window_w") s.windowW = asInt(s.windowW);
                else if (key == "window_h") s.windowH = asInt(s.windowH);
                else if (key == "force_bilinear") s.forceBilinear = b;
                else if (key == "ablend128") s.ablend128 = b;
                else if (key == "texture_pack") s.texPack = b;
                else if (key == "intro_video") s.introVideo = b;
                else if (key == "button_layout") s.buttonLayout = asInt(s.buttonLayout);
                else if (key == "fps60") s.fps60 = b;
                else if (key == "hud_layout") s.hudLayout = asInt(s.hudLayout);
                else if (key == "hud_off_l") s.hudOffL = asInt(s.hudOffL);
                else if (key == "hud_off_c") s.hudOffC = asInt(s.hudOffC);
                else if (key == "hud_off_r") s.hudOffR = asInt(s.hudOffR);
                else if (key == "ink_strength") s.inkStrength = asInt(s.inkStrength);
                else if (key == "ink_width") s.inkWidth = asInt(s.inkWidth);
                else if (key == "ink_color") s.inkColor = hexToColor(val, s.inkColor);
            }
            else if (section == "controllers")
            {
                const bool b = asBool();
                if (key == "deadzone") s.deadzone = asFloat(s.deadzone);
                else if (key == "device") s.device = asInt(s.device);
                else if (key == "overlay_enabled") s.overlayEnabled = b;
                else if (key == "overlay_pad_btns") s.overlayPadBtns = val;
                else if (key == "overlay_keys") s.overlayKeys = val;
            }
            else if (section == "logging")
            {
                const bool b = asBool();
                if (key == "log_level") s.logLevel = asInt(s.logLevel);
                else if (key == "dump_audio") s.dumpAudio = b;
                else if (key == "dump_video") s.dumpVideo = b;
                else if (key == "dump_controllers") s.dumpControllers = b;
                else if (key == "dump_runtime") s.dumpRuntime = b;
                else if (key == "dump_gamepad") s.dumpGamepad = b;
            }
        }
        clamp(s);
        return true;
    }
}

namespace ps2x_settings
{
    bool operator==(const Settings &a, const Settings &b)
    {
        // feWidth/feHeight are intentionally NOT compared: they track the shell
        // window the user dragged, so they would report "cambios sin guardar" for
        // something that is not a game setting.
        // [ach] compared, so the front-end's unsaved-changes check sees a toggle of the tracker.
        // netOverlay is deliberately NOT here: the overlay sets m_dirty itself when that switch
        // moves, and adding it would make the shell report unsaved changes for a setting the
        // overlay has already written. Left alone rather than tidied up -- that is a separate
        // question about the netplay switch, not about this one.
        return a.master == b.master && a.music == b.music && a.sfx == b.sfx &&
               a.renderer == b.renderer && a.achievements == b.achievements &&
               a.glow == b.glow && a.glowFix == b.glowFix &&
               a.bilinear == b.bilinear && a.halfTexel == b.halfTexel && a.skipPost == b.skipPost &&
               a.skipStaleVram == b.skipStaleVram && a.renderScale == b.renderScale &&
               a.outline == b.outline && a.inkStrength == b.inkStrength && a.inkWidth == b.inkWidth &&
               a.inkColor == b.inkColor && a.shadows == b.shadows && a.dofBlur == b.dofBlur &&
               a.dofZFar == b.dofZFar && a.fullscreen == b.fullscreen && a.windowMode == b.windowMode &&
               a.monitor == b.monitor && a.gpu == b.gpu && a.widescreen == b.widescreen && a.windowW == b.windowW &&
               a.windowH == b.windowH && a.forceBilinear == b.forceBilinear && a.ablend128 == b.ablend128 && a.texPack == b.texPack &&
               a.introVideo == b.introVideo &&
               a.buttonLayout == b.buttonLayout && a.fps60 == b.fps60 && a.showPerf == b.showPerf &&
               a.hudOffL == b.hudOffL && a.hudOffC == b.hudOffC && a.hudOffR == b.hudOffR &&
               a.device == b.device && a.deadzone == b.deadzone &&
               a.overlayEnabled == b.overlayEnabled && a.overlayPadBtns == b.overlayPadBtns &&
               a.overlayKeys == b.overlayKeys && a.logLevel == b.logLevel &&
               a.dumpAudio == b.dumpAudio && a.dumpVideo == b.dumpVideo &&
               a.dumpControllers == b.dumpControllers && a.dumpRuntime == b.dumpRuntime &&
               a.dumpGamepad == b.dumpGamepad;
    }

    const char *rendererName(int renderer)
    {
        switch (renderer)
        {
            case kRendererOpenGL: return "opengl";
            case kRendererSoftware: return "software";
            case kRendererParallelGS: return "parallel-gs";
            case kRendererD3D11: return "d3d11";
            default: return "opengl";
        }
    }

    int nameToRenderer(const std::string &name, int fallback)
    {
        if (name == "opengl" || name == "gl") return kRendererOpenGL;
        if (name == "software" || name == "sw") return kRendererSoftware;
        if (name == "parallel-gs" || name == "parallel_gs" || name == "pgs") return kRendererParallelGS;
        if (name == "d3d11" || name == "dx11" || name == "d3d") return kRendererD3D11;
        return fallback;
    }

    std::string configPath(const std::string &configDir)
    {
        if (configDir.empty())
            return kConfigFileName;
        return (std::filesystem::path(configDir) / kConfigFileName).string();
    }

    bool loadFromFile(Settings &out, const std::string &path)
    {
        return loadToml(out, path);
    }

    bool load(Settings &out, const std::string &configDir)
    {
        const std::string path = configPath(configDir);
        std::error_code ec;
        if (std::filesystem::exists(path, ec))
            return loadFromFile(out, path);

        const std::string legacy =
            configDir.empty() ? std::string(kLegacyConfigFileName)
                              : (std::filesystem::path(configDir) / kLegacyConfigFileName).string();
        if (std::filesystem::exists(legacy, ec))
        {
            const bool ok = loadIniLegacy(out, legacy);
            save(out, configDir);
            std::error_code rm;
            std::filesystem::remove(legacy, rm);
            return ok;
        }

        // Neither file exists: write the defaults now so the file is there from the first run
        // (the runtime and the in-game overlay read the same one).
        save(out, configDir);
        return false;
    }

    std::string serialize(const Settings &s)
    {
        using ps2x_toml::fmtBool;
        using ps2x_toml::fmtDbl;
        using ps2x_toml::fmtInt;
        using ps2x_toml::fmtIntArray;
        using ps2x_toml::fmtStr;

        std::ostringstream os;
        os << kHeader << "\n";

        os << "[audio]\n";
        os << "master_volume = " << fmtDbl(s.master) << "\n";
        os << "music_volume = " << fmtDbl(s.music) << "\n";
        os << "sfx_volume = " << fmtDbl(s.sfx) << "\n\n";

        os << "[netplay]\n";
        os << "# The corner label, the panel behind it, and the automatic character-select\n"
              "# transition with its curtain. NET_OVERLAY is the default when this key is absent.\n";
        os << "overlay = " << fmtBool(s.netOverlay) << "\n\n";

        os << "[achievements]\n";
        os << "# The local RetroAchievements tracker. It reads a patch from assets/ and writes\n"
              "# progress to savedata/; it never contacts a server. The achievements, their badges\n"
              "# and the rcheevos library that evaluates them are RetroAchievements' work. Most of\n"
              "# the 154 read an address this build does not use, so they are listed but cannot be\n"
              "# earned by playing yet -- see docs/ACHIEVEMENTS.md.\n"
              "# On by default; ACHIEVEMENTS=0 turns it off for one run.\n";
        os << "enabled = " << fmtBool(s.achievements) << "\n\n";

        os << "[video]\n";
        os << "renderer = " << fmtStr(rendererName(s.renderer)) << "\n";
        os << "glow = " << fmtBool(s.glow) << "\n";
        os << "glowfix = " << fmtBool(s.glowFix) << "\n";
        os << "ink_strength = " << fmtInt(s.inkStrength) << "\n";
        os << "ink_width = " << fmtInt(s.inkWidth) << "\n";
        os << "ink_color = " << fmtStr(colorToHex(s.inkColor)) << "\n";
        os << "bilinear = " << fmtBool(s.bilinear) << "\n";
        os << "halftexel = " << fmtBool(s.halfTexel) << "\n";
        os << "skippost = " << fmtBool(s.skipPost) << "\n";
        os << "skip_stale_vram = " << fmtBool(s.skipStaleVram) << "\n";
        os << "render_scale = " << fmtInt(s.renderScale) << "\n";
        os << "outline = " << fmtBool(s.outline) << "\n";
        os << "texture_pack = " << fmtBool(s.texPack) << "\n";
        os << "intro_video = " << fmtBool(s.introVideo) << "\n";
        os << "button_layout = " << fmtInt(s.buttonLayout) << "\n";
        os << "shadows = " << fmtBool(s.shadows) << "\n";
        os << "dof_blur = " << fmtBool(s.dofBlur) << "\n";
        os << "dof_zfar = " << fmtInt(s.dofZFar) << "\n";
        os << "fullscreen = " << fmtBool(s.fullscreen) << "\n";
        os << "window_mode = " << fmtInt(s.windowMode) << "\n";
        os << "monitor = " << fmtInt(s.monitor) << "\n";
        os << "gpu = " << fmtStr(s.gpu) << "\n";
        os << "widescreen = " << fmtBool(s.widescreen) << "\n";
        os << "window_w = " << fmtInt(s.windowW) << "\n";
        os << "window_h = " << fmtInt(s.windowH) << "\n";
        os << "force_bilinear = " << fmtBool(s.forceBilinear) << "\n";
        os << "ablend128 = " << fmtBool(s.ablend128) << "\n";
        os << "fps60 = " << fmtBool(s.fps60) << "\n";
        os << "show_perf = " << fmtBool(s.showPerf) << "\n\n";

        os << "[video.hud]\n";
        os << "layout = " << fmtInt(s.hudLayout) << "\n";
        os << "offset_left = " << fmtInt(s.hudOffL) << "\n";
        os << "offset_center = " << fmtInt(s.hudOffC) << "\n";
        os << "offset_right = " << fmtInt(s.hudOffR) << "\n\n";

        os << "[controllers]\n";
        os << "device = " << fmtInt(s.device) << "\n";
        os << "deadzone = " << fmtDbl(s.deadzone) << "\n";
        os << "overlay_enabled = " << fmtBool(s.overlayEnabled) << "\n\n";

        os << "[controllers.hotkey]\n";
        os << "pad_btns = " << fmtIntArray(csvToInts(s.overlayPadBtns)) << "\n";
        os << "keys = " << fmtIntArray(csvToInts(s.overlayKeys)) << "\n\n";

        os << "[logging]\n";
        os << "log_level = " << fmtInt(s.logLevel) << "\n";
        os << "dump_audio = " << fmtBool(s.dumpAudio) << "\n";
        os << "dump_video = " << fmtBool(s.dumpVideo) << "\n";
        os << "dump_controllers = " << fmtBool(s.dumpControllers) << "\n";
        os << "dump_runtime = " << fmtBool(s.dumpRuntime) << "\n";
        os << "dump_gamepad = " << fmtBool(s.dumpGamepad) << "\n\n";

        // [frontend] shell window only. The game never reads this: on PLAY it applies
        // [video] window_w/window_h to its own window, and this window just reopens at
        // whatever size the user left it.
        os << "[frontend]\n";
        os << "width = " << fmtInt(s.feWidth) << "\n";
        os << "height = " << fmtInt(s.feHeight) << "\n";
        os << "music_muted = " << fmtBool(s.musicMuted) << "\n";

        return os.str();
    }

    bool save(const Settings &s, const std::string &configDir)
    {
        return saveToFile(s, configPath(configDir));
    }

    bool saveToFile(const Settings &s, const std::string &path)
    {
        std::error_code ec;
        const std::filesystem::path parent = std::filesystem::path(path).parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, ec);

        std::ofstream file(path, std::ios::trunc);
        if (!file.is_open())
            return false;
        file << serialize(s);
        return file.good();
    }

    std::vector<int> parseIntCsv(const std::string &csv)
    {
        return csvToInts(csv);
    }

    std::string formatIntCsv(const std::vector<int> &values)
    {
        return intsToCsv(values);
    }

    void applyOverlayValues(Settings &out, const Settings &live, uint32_t envLockedMask)
    {
        // Env-guarded: a locked key is left at the file's value, because `out` came from the file.
        if (!(envLockedMask & kLockGlow))        out.glow = live.glow;
        if (!(envLockedMask & kLockGlowFix))     out.glowFix = live.glowFix;
        if (!(envLockedMask & kLockInkStrength)) out.inkStrength = live.inkStrength;
        if (!(envLockedMask & kLockBilinear))    out.bilinear = live.bilinear;
        if (!(envLockedMask & kLockHalfTexel))   out.halfTexel = live.halfTexel;
        if (!(envLockedMask & kLockSkipPost))    out.skipPost = live.skipPost;
        if (!(envLockedMask & kLockSkipStale))   out.skipStaleVram = live.skipStaleVram;
        if (!(envLockedMask & kLockRenderScale)) out.renderScale = live.renderScale;
        if (!(envLockedMask & kLockOutline))     out.outline = live.outline;
        if (!(envLockedMask & kLockTexPack))     out.texPack = live.texPack;
        if (!(envLockedMask & kLockIntroVideo))  out.introVideo = live.introVideo;
        if (!(envLockedMask & kLockButtonLay))   out.buttonLayout = live.buttonLayout;
        if (!(envLockedMask & kLockShadows))     out.shadows = live.shadows;
        if (!(envLockedMask & kLockDofBlur))     out.dofBlur = live.dofBlur;
        if (!(envLockedMask & kLockDofZFar))     out.dofZFar = live.dofZFar;
        if (!(envLockedMask & kLockRenderer))    out.renderer = live.renderer;

        // No env var reaches these, so the overlay always owns them.
        out.master = live.master;
        out.music = live.music;
        out.sfx = live.sfx;
        out.inkWidth = live.inkWidth;
        out.inkColor = live.inkColor;
            out.fullscreen = live.fullscreen;
        out.windowMode = live.windowMode;
        out.monitor = live.monitor;
        out.widescreen = live.widescreen;
        out.windowW = live.windowW;
        out.windowH = live.windowH;
        out.forceBilinear = live.forceBilinear;
        out.ablend128 = live.ablend128;
        out.fps60 = live.fps60;
        out.showPerf = live.showPerf;
        out.hudLayout = live.hudLayout;
        out.hudOffL = live.hudOffL;
        out.hudOffC = live.hudOffC;
        out.hudOffR = live.hudOffR;
        out.deadzone = live.deadzone;
        out.overlayEnabled = live.overlayEnabled;
        out.overlayPadBtns = live.overlayPadBtns;
        out.overlayKeys = live.overlayKeys;
        out.logLevel = live.logLevel;
    }
}
