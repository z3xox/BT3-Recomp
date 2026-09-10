#include "settings_manager.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTextStream>
#include <optional>

namespace
{
    QString trim(const QString &s)
    {
        QString t = s;
        while (!t.isEmpty() && (t.front().isSpace() || t.front() == '\r'))
            t.remove(0, 1);
        while (!t.isEmpty() && (t.back().isSpace() || t.back() == '\r'))
            t.chop(1);
        return t;
    }

    int stoi(const QString &v, int def)
    {
        bool ok = false;
        const int r = v.toInt(&ok);
        return ok ? r : def;
    }

    float stof(const QString &v, float def)
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

QString SettingsManager::inipath() const
{
    if (m_dir.isEmpty())
        return QStringLiteral("bt3_settings.ini");
    QDir d(m_dir);
    return d.filePath(QStringLiteral("bt3_settings.ini"));
}

bool SettingsManager::load()
{
    const QString path = inipath();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    sections.clear();
    m_sawRenderer = false;
    QString curSection;
    QString openSection; // key of "first value encountered" per section (for order)
    QStringList order;
    QHash<QString, QString> kv;

    auto flush = [&]() {
        if (!curSection.isEmpty() || !kv.isEmpty())
        {
            Section s;
            s.kv = kv;
            s.order = order;
            sections.insert(curSection, s);
            kv.clear();
            order.clear();
        }
    };

    QTextStream in(&f);
    while (!in.atEnd())
    {
        const QString raw = in.readLine();
        const QString line = trim(raw);
        if (line.isEmpty() || line.startsWith('#') || line.startsWith(';'))
            continue;
        if (line.startsWith('['))
        {
            const int end = line.indexOf(']');
            if (end > 0)
            {
                flush();
                curSection = line.mid(1, end - 1).trimmed();
                continue;
            }
        }
        const int eq = line.indexOf('=');
        if (eq <= 0)
            continue;
        const QString key = trim(line.left(eq));
        const QString val = trim(line.mid(eq + 1));
        if (key.isEmpty())
            continue;
        if (!kv.contains(key))
            order.append(key);
        kv[key] = val;

        // Map recognised keys into typed members.
        if (curSection == "audio")
        {
            if (key == "master_volume") m_master = stof(val, 1.0f);
            else if (key == "music_volume") m_music = stof(val, 1.0f);
            else if (key == "sfx_volume") m_sfx = stof(val, 1.0f);
        }
        else if (curSection == "video")
        {
            const bool b = (val == "1" || val == "true");
            if (key == "renderer") { setRenderer(stoi(val, m_renderer)); m_sawRenderer = true; }
            else if (key == "gpu_renderer" && !m_sawRenderer) setGpuRenderer(b);
            else if (key == "glow") m_glow = b;
            else if (key == "glowfix") m_glowFix = b;
            else if (key == "postfx") m_postfx = b;
            else if (key == "bilinear") m_bilinear = b;
            else if (key == "halftexel") m_halfTexel = b;
            else if (key == "skippost") m_skipPost = b;
            else if (key == "skip_stale_vram") m_skipStaleVram = b;
            else if (key == "render_scale") m_renderScale = stoi(val, 1);
            else if (key == "outline") m_outline = b;
            else if (key == "shadows") m_shadows = b;
            else if (key == "dof_blur") m_dofBlur = b;
            else if (key == "dof_zfar") m_dofZFar = stoi(val, 200000);
            else if (key == "fullscreen") m_fullscreen = b;
            else if (key == "widescreen") m_widescreen = b;
            else if (key == "window_w") m_windowW = stoi(val, 0);
            else if (key == "window_h") m_windowH = stoi(val, 0);
            else if (key == "force_bilinear") m_forceBilinear = b;
            else if (key == "hud_layout") m_hudLayout = stoi(val, 0);
            else if (key == "hud_off_l") m_hudOffL = stoi(val, 0);
            else if (key == "hud_off_c") m_hudOffC = stoi(val, 0);
            else if (key == "hud_off_r") m_hudOffR = stoi(val, 0);
            else if (key == "ink_strength") m_inkStrength = stoi(val, 200);
        }
        else if (curSection == "controllers")
        {
            const bool b = (val == "1" || val == "true");
            if (key == "deadzone") m_deadzone = stof(val, 0.15f);
            else if (key == "device") m_device = stoi(val, 0);
            else if (key == "overlay_enabled") m_overlayEnabled = b;
            else if (key == "overlay_pad_btns") m_overlayPadBtns = val.toStdString();
            else if (key == "overlay_keys") m_overlayKeys = val.toStdString();
        }
        else if (curSection == "logging")
        {
            const bool b = (val == "1" || val == "true");
            if (key == "log_level") m_logLevel = stoi(val, 1);
            else if (key == "dump_audio") m_dumpAudio = b;
            else if (key == "dump_video") m_dumpVideo = b;
            else if (key == "dump_controllers") m_dumpControllers = b;
            else if (key == "dump_runtime") m_dumpRuntime = b;
            else if (key == "dump_gamepad") m_dumpGamepad = b;
        }
    }
    flush();
    return true;
}

bool SettingsManager::save()
{
    const QString path = inipath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    // Rebuild typed values into the raw map, preserving order/unknowns.
    auto upsert = [&](const QString &sec, const QString &key, const QString &val) {
        Section &s = sections[sec];
        if (!s.kv.contains(key))
            s.order.append(key);
        s.kv[key] = val;
    };

    upsert(QStringLiteral("audio"), QStringLiteral("master_volume"),
           QString::number(m_master));
    upsert(QStringLiteral("audio"), QStringLiteral("music_volume"),
           QString::number(m_music));
    upsert(QStringLiteral("audio"), QStringLiteral("sfx_volume"),
           QString::number(m_sfx));

    upsert("video", "renderer", QString::number(m_renderer));
    upsert("video", "gpu_renderer", gpuRenderer() ? "1" : "0");
    upsert("video", "glow", m_glow ? "1" : "0");
    upsert("video", "glowfix", m_glowFix ? "1" : "0");
    upsert("video", "ink_strength", QString::number(m_inkStrength));
    upsert("video", "postfx", m_postfx ? "1" : "0");
    upsert("video", "bilinear", m_bilinear ? "1" : "0");
    upsert("video", "halftexel", m_halfTexel ? "1" : "0");
    upsert("video", "skippost", m_skipPost ? "1" : "0");
    upsert("video", "skip_stale_vram", m_skipStaleVram ? "1" : "0");
    upsert("video", "render_scale", QString::number(m_renderScale));
    upsert("video", "outline", m_outline ? "1" : "0");
    upsert("video", "shadows", m_shadows ? "1" : "0");
    upsert("video", "dof_blur", m_dofBlur ? "1" : "0");
    upsert("video", "dof_zfar", QString::number(m_dofZFar));
    upsert("video", "fullscreen", m_fullscreen ? "1" : "0");
    upsert("video", "widescreen", m_widescreen ? "1" : "0");
    upsert("video", "window_w", QString::number(m_windowW));
    upsert("video", "window_h", QString::number(m_windowH));
    upsert("video", "force_bilinear", m_forceBilinear ? "1" : "0");
    upsert("video", "hud_layout", QString::number(m_hudLayout));
    upsert("video", "hud_off_l", QString::number(m_hudOffL));
    upsert("video", "hud_off_c", QString::number(m_hudOffC));
    upsert("video", "hud_off_r", QString::number(m_hudOffR));

    upsert("controllers", "deadzone", QString::number(m_deadzone));
    upsert("controllers", "device", QString::number(m_device));
    upsert("controllers", "overlay_enabled", m_overlayEnabled ? "1" : "0");
    upsert("controllers", "overlay_pad_btns", QString::fromStdString(m_overlayPadBtns));
    upsert("controllers", "overlay_keys", QString::fromStdString(m_overlayKeys));

    upsert("logging", "log_level", QString::number(m_logLevel));
    upsert("logging", "dump_audio", m_dumpAudio ? "1" : "0");
    upsert("logging", "dump_video", m_dumpVideo ? "1" : "0");
    upsert("logging", "dump_controllers", m_dumpControllers ? "1" : "0");
    upsert("logging", "dump_runtime", m_dumpRuntime ? "1" : "0");
    upsert("logging", "dump_gamepad", m_dumpGamepad ? "1" : "0");

    // Write the ini directly. A tmp+rename round-trip left a stale .tmp file
    // behind and on some platforms failed to replace the existing ini, so the
    // "Save" button appeared to do nothing.
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        return false;
    QTextStream ts(&out);
    bool first = true;
    for (auto it = sections.begin(); it != sections.end(); ++it)
    {
        const Section &s = it.value();
        if (!first)
            ts << "\n";
        first = false;
        ts << "[" << it.key() << "]\n";
        for (const QString &k : s.order)
            ts << k << "=" << s.kv[k] << "\n";
    }
    ts.flush();
    return true;
}