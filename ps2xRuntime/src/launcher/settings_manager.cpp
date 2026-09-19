#include "settings_manager.h"

#include "runtime/ps2_toml.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>

namespace
{
    const char *kTomlHeader =
        "# Dragon Ball Z: Budokai Tenkaichi 3 - Recompiled\n"
        "# User settings - written by the launcher and the in-game overlay.\n"
        "# Delete this file to reset everything to defaults.\n";

    const char *rendererName(int r)
    {
        switch (r)
        {
            case 0: return "opengl";
            case 1: return "software";
            case 2: return "parallel-gs";
            case 3: return "d3d11";
            default: return "opengl";
        }
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
        std::stringstream ss(s);
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
            if (i) r += ",";
            r += std::to_string(v[i]);
        }
        return r;
    }

    // --- legacy INI helpers ---------------------------------------------------
    QString itrim(const QString &s)
    {
        QString t = s;
        while (!t.isEmpty() && (t.front().isSpace() || t.front() == '\r'))
            t.remove(0, 1);
        while (!t.isEmpty() && (t.back().isSpace() || t.back() == '\r'))
            t.chop(1);
        return t;
    }

    int ist(const QString &v, int def)
    {
        bool ok = false;
        const int r = v.toInt(&ok);
        return ok ? r : def;
    }

    float fst(const QString &v, float def)
    {
        bool ok = false;
        const float r = v.toFloat(&ok);
        return ok ? r : def;
    }
} // namespace

SettingsManager &SettingsManager::instance()
{
    static SettingsManager s;
    return s;
}

void SettingsManager::setConfigDir(const QString &dir)
{
    m_dir = dir;
}

QString SettingsManager::configpath() const
{
    if (m_dir.isEmpty())
        return QStringLiteral("settings.toml");
    return QDir(m_dir).filePath(QStringLiteral("settings.toml"));
}

bool SettingsManager::load()
{
    const QString path = configpath();
    if (QFile::exists(path))
        return loadToml(path);

    // One-time migration from the 0.x INI: import it, write settings.toml, drop the INI.
    const QString legacy = m_dir.isEmpty()
        ? QStringLiteral("bt3_settings.ini")
        : QDir(m_dir).filePath(QStringLiteral("bt3_settings.ini"));
    if (QFile::exists(legacy))
    {
        const bool ok = loadIniLegacy(legacy);
        save();
        std::remove(QFile::encodeName(legacy).constData());
        return ok;
    }
    return false;
}

bool SettingsManager::loadToml(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray bytes = f.readAll();
    f.close();

    std::istringstream is(bytes.toStdString());
    ps2x_toml::Document doc;
    doc.parse(is);

    m_sawRenderer = false;

    m_master = static_cast<float>(doc.getD("audio.master_volume", m_master));
    m_music = static_cast<float>(doc.getD("audio.music_volume", m_music));
    m_sfx = static_cast<float>(doc.getD("audio.sfx_volume", m_sfx));

    {
        int r = nameToRenderer(doc.getS("video.renderer", rendererName(m_renderer)), m_renderer);
        // [pgswin] Direct3D 11 is retired on every platform: an old "d3d11" becomes OpenGL (New),
        // matching the runtime. paraLLEl-GS is kept as-is -- the Windows "PGS -> d3d11" remap
        // turned a saved paraLLEl-GS into OpenGL and wrote that back on every Play.
        if (r == kRendererD3D11) r = kRendererOpenGL;
        if (r >= 0 && r <= 2) { m_renderer = r; m_sawRenderer = true; }
    }
    m_glow = doc.getB("video.glow", m_glow);
    m_glowFix = doc.getB("video.glowfix", m_glowFix);
    m_inkStrength = doc.getI("video.ink_strength", m_inkStrength);
    m_inkWidth = doc.getI("video.ink_width", m_inkWidth);
    m_inkColor = hexToColor(doc.getS("video.ink_color", colorToHex(m_inkColor)), m_inkColor);
    m_bilinear = doc.getB("video.bilinear", m_bilinear);
    m_halfTexel = doc.getB("video.halftexel", m_halfTexel);
    m_skipPost = doc.getB("video.skippost", m_skipPost);
    m_skipStaleVram = doc.getB("video.skip_stale_vram", m_skipStaleVram);
    m_renderScale = doc.getI("video.render_scale", m_renderScale);
    m_outline = doc.getB("video.outline", m_outline);
    m_texPack = doc.getB("video.texture_pack", m_texPack);
    m_introVideo = doc.getB("video.intro_video", m_introVideo);
    m_buttonLayout = doc.getI("video.button_layout", m_buttonLayout);
    m_texcache = doc.getB("video.texcache", m_texcache);
    m_shadows = doc.getB("video.shadows", m_shadows);
    m_dofBlur = doc.getB("video.dof_blur", m_dofBlur);
    m_dofZFar = doc.getI("video.dof_zfar", m_dofZFar);
    m_fullscreen = doc.getB("video.fullscreen", m_fullscreen);
    // [video.mode] window mode owns fullscreen now; the legacy bool is kept in sync for old readers.
    m_windowMode = doc.getI("video.window_mode", m_fullscreen ? 2 : 0);
    m_monitor = doc.getI("video.monitor", 0);
    m_widescreen = doc.getB("video.widescreen", m_widescreen);
    m_windowW = doc.getI("video.window_w", m_windowW);
    m_windowH = doc.getI("video.window_h", m_windowH);
    m_forceBilinear = doc.getB("video.force_bilinear", m_forceBilinear);
    m_fps60 = doc.getB("video.fps60", m_fps60);

    m_hudLayout = doc.getI("video.hud.layout", m_hudLayout);
    m_hudOffL = doc.getI("video.hud.offset_left", m_hudOffL);
    m_hudOffC = doc.getI("video.hud.offset_center", m_hudOffC);
    m_hudOffR = doc.getI("video.hud.offset_right", m_hudOffR);

    m_device = doc.getI("controllers.device", m_device);
    m_deadzone = static_cast<float>(doc.getD("controllers.deadzone", m_deadzone));
    m_overlayEnabled = doc.getB("controllers.overlay_enabled", m_overlayEnabled);
    m_overlayPadBtns = intsToCsv(doc.getIA("controllers.hotkey.pad_btns", {13, 15}));
    m_overlayKeys = intsToCsv(doc.getIA("controllers.hotkey.keys", {340, 258}));

    m_logLevel = doc.getI("logging.log_level", m_logLevel);
    m_dumpAudio = doc.getB("logging.dump_audio", m_dumpAudio);
    m_dumpVideo = doc.getB("logging.dump_video", m_dumpVideo);
    m_dumpControllers = doc.getB("logging.dump_controllers", m_dumpControllers);
    m_dumpRuntime = doc.getB("logging.dump_runtime", m_dumpRuntime);
    m_dumpGamepad = doc.getB("logging.dump_gamepad", m_dumpGamepad);

    return true;
}

bool SettingsManager::loadIniLegacy(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    m_sawRenderer = false;
    QString curSection;

    QTextStream in(&f);
    while (!in.atEnd())
    {
        const QString line = itrim(in.readLine());
        if (line.isEmpty() || line.startsWith('#') || line.startsWith(';'))
            continue;
        if (line.startsWith('['))
        {
            const int end = line.indexOf(']');
            if (end > 0)
                curSection = line.mid(1, end - 1).trimmed();
            continue;
        }
        const int eq = line.indexOf('=');
        if (eq <= 0)
            continue;
        const QString key = itrim(line.left(eq));
        const QString val = itrim(line.mid(eq + 1));
        if (key.isEmpty())
            continue;

        if (curSection == "audio")
        {
            if (key == "master_volume") m_master = fst(val, 1.0f);
            else if (key == "music_volume") m_music = fst(val, 1.0f);
            else if (key == "sfx_volume") m_sfx = fst(val, 1.0f);
        }
        else if (curSection == "video")
        {
            const bool b = (val == "1" || val == "true");
            if (key == "renderer") { setRenderer(ist(val, m_renderer)); m_sawRenderer = true; }
            else if (key == "gpu_renderer" && !m_sawRenderer) setGpuRenderer(b);
            else if (key == "glow") m_glow = b;
            else if (key == "glowfix") m_glowFix = b;
            else if (key == "bilinear") m_bilinear = b;
            else if (key == "halftexel") m_halfTexel = b;
            else if (key == "skippost") m_skipPost = b;
            else if (key == "skip_stale_vram") m_skipStaleVram = b;
            else if (key == "render_scale") m_renderScale = ist(val, 1);
            else if (key == "outline") m_outline = b;
            else if (key == "shadows") m_shadows = b;
            else if (key == "dof_blur") m_dofBlur = b;
            else if (key == "dof_zfar") m_dofZFar = ist(val, 200000);
            else if (key == "fullscreen") { m_fullscreen = b; m_windowMode = b ? 2 : 0; }
            else if (key == "widescreen") m_widescreen = b;
            else if (key == "window_w") m_windowW = ist(val, 0);
            else if (key == "window_h") m_windowH = ist(val, 0);
            else if (key == "force_bilinear") m_forceBilinear = b;
            else if (key == "texture_pack") m_texPack = b;
            else if (key == "intro_video") m_introVideo = b;
            else if (key == "button_layout") m_buttonLayout = ist(val, 1);
            else if (key == "texcache") m_texcache = b;
            else if (key == "fps60") m_fps60 = b;
            else if (key == "hud_layout") m_hudLayout = ist(val, 0);
            else if (key == "hud_off_l") m_hudOffL = ist(val, 0);
            else if (key == "hud_off_c") m_hudOffC = ist(val, 0);
            else if (key == "hud_off_r") m_hudOffR = ist(val, 0);
            else if (key == "ink_strength") m_inkStrength = ist(val, 200);
            else if (key == "ink_width") m_inkWidth = ist(val, 100);
            else if (key == "ink_color") m_inkColor = hexToColor(val.toStdString(), m_inkColor);
        }
        else if (curSection == "controllers")
        {
            const bool b = (val == "1" || val == "true");
            if (key == "deadzone") m_deadzone = fst(val, 0.15f);
            else if (key == "device") m_device = ist(val, 0);
            else if (key == "overlay_enabled") m_overlayEnabled = b;
            else if (key == "overlay_pad_btns") m_overlayPadBtns = val.toStdString();
            else if (key == "overlay_keys") m_overlayKeys = val.toStdString();
        }
        else if (curSection == "logging")
        {
            const bool b = (val == "1" || val == "true");
            if (key == "log_level") m_logLevel = ist(val, 1);
            else if (key == "dump_audio") m_dumpAudio = b;
            else if (key == "dump_video") m_dumpVideo = b;
            else if (key == "dump_controllers") m_dumpControllers = b;
            else if (key == "dump_runtime") m_dumpRuntime = b;
            else if (key == "dump_gamepad") m_dumpGamepad = b;
        }
    }
    return true;
}

bool SettingsManager::save()
{
    const QString path = configpath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    using ps2x_toml::fmtBool;
    using ps2x_toml::fmtDbl;
    using ps2x_toml::fmtInt;
    using ps2x_toml::fmtIntArray;
    using ps2x_toml::fmtStr;

    std::ostringstream os;
    os << kTomlHeader << "\n";

    os << "[audio]\n";
    os << "master_volume = " << fmtDbl(m_master) << "\n";
    os << "music_volume = " << fmtDbl(m_music) << "\n";
    os << "sfx_volume = " << fmtDbl(m_sfx) << "\n\n";

    os << "[video]\n";
    os << "renderer = " << fmtStr(rendererName(m_renderer)) << "\n";
    os << "glow = " << fmtBool(m_glow) << "\n";
    os << "glowfix = " << fmtBool(m_glowFix) << "\n";
    os << "ink_strength = " << fmtInt(m_inkStrength) << "\n";
    os << "ink_width = " << fmtInt(m_inkWidth) << "\n";
    os << "ink_color = " << fmtStr(colorToHex(m_inkColor)) << "\n";
    os << "bilinear = " << fmtBool(m_bilinear) << "\n";
    os << "halftexel = " << fmtBool(m_halfTexel) << "\n";
    os << "skippost = " << fmtBool(m_skipPost) << "\n";
    os << "skip_stale_vram = " << fmtBool(m_skipStaleVram) << "\n";
    os << "render_scale = " << fmtInt(m_renderScale) << "\n";
    os << "outline = " << fmtBool(m_outline) << "\n";
    os << "texture_pack = " << fmtBool(m_texPack) << "\n";
    os << "intro_video = " << fmtBool(m_introVideo) << "\n";
    os << "button_layout = " << fmtInt(m_buttonLayout) << "\n";
    os << "texcache = " << fmtBool(m_texcache) << "\n";
    os << "shadows = " << fmtBool(m_shadows) << "\n";
    os << "dof_blur = " << fmtBool(m_dofBlur) << "\n";
    os << "dof_zfar = " << fmtInt(m_dofZFar) << "\n";
    os << "fullscreen = " << fmtBool(m_fullscreen) << "\n";
    os << "window_mode = " << fmtInt(m_windowMode) << "\n";
    os << "monitor = " << fmtInt(m_monitor) << "\n";
    os << "widescreen = " << fmtBool(m_widescreen) << "\n";
    os << "window_w = " << fmtInt(m_windowW) << "\n";
    os << "window_h = " << fmtInt(m_windowH) << "\n";
    os << "force_bilinear = " << fmtBool(m_forceBilinear) << "\n";
    os << "fps60 = " << fmtBool(m_fps60) << "\n\n";

    os << "[video.hud]\n";
    os << "layout = " << fmtInt(m_hudLayout) << "\n";
    os << "offset_left = " << fmtInt(m_hudOffL) << "\n";
    os << "offset_center = " << fmtInt(m_hudOffC) << "\n";
    os << "offset_right = " << fmtInt(m_hudOffR) << "\n\n";

    os << "[controllers]\n";
    os << "device = " << fmtInt(m_device) << "\n";
    os << "deadzone = " << fmtDbl(m_deadzone) << "\n";
    os << "overlay_enabled = " << fmtBool(m_overlayEnabled) << "\n\n";

    os << "[controllers.hotkey]\n";
    os << "pad_btns = " << fmtIntArray(csvToInts(m_overlayPadBtns)) << "\n";
    os << "keys = " << fmtIntArray(csvToInts(m_overlayKeys)) << "\n\n";

    os << "[logging]\n";
    os << "log_level = " << fmtInt(m_logLevel) << "\n";
    os << "dump_audio = " << fmtBool(m_dumpAudio) << "\n";
    os << "dump_video = " << fmtBool(m_dumpVideo) << "\n";
    os << "dump_controllers = " << fmtBool(m_dumpControllers) << "\n";
    os << "dump_runtime = " << fmtBool(m_dumpRuntime) << "\n";
    os << "dump_gamepad = " << fmtBool(m_dumpGamepad) << "\n";

    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const std::string s = os.str();
    out.write(s.data(), static_cast<qint64>(s.size()));
    return true;
}
