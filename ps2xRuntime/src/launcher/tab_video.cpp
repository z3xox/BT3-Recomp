#include "tab_video.h"

#include "dbz_theme.h"
#include "settings_manager.h"
#include "tex_pack.h"   // [texui] pack status for the Texture Replacement dialog

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QScrollArea>
#include <QSlider>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace
{
    QLabel *sectionLabel(const QString &text)
    {
        auto *l = new QLabel(text);
        l->setObjectName(QStringLiteral("sectionLabel"));
        return l;
    }

    QLabel *hintRow(const QString &text)
    {
        auto *l = new QLabel(text);
        l->setObjectName(QStringLiteral("hintLabel"));
        l->setWordWrap(true);
        return l;
    }

    QLabel *valueLabel(const QString &text = QString())
    {
        auto *l = new QLabel(text);
        l->setObjectName(QStringLiteral("valueLabel"));
        return l;
    }

    QWidget *sliderPair(const QString &label, QSlider **sl, QLabel **val,
                        int lo, int hi, int cur, const char *fmt = "%d")
    {
        auto *row = new QWidget;
        auto *lay = new QHBoxLayout(row);
        lay->setContentsMargins(8, 2, 8, 2);
        lay->setSpacing(10);
        auto *lbl = valueLabel(label);
        lbl->setMinimumWidth(110);
        *sl = new QSlider(Qt::Horizontal);
        (*sl)->setRange(lo, hi);
        (*sl)->setValue(cur);
        *val = valueLabel(QString::asprintf(fmt, cur));
        (*val)->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        (*val)->setMinimumWidth(56);
        lay->addWidget(lbl);
        lay->addWidget(*sl, 1);
        lay->addWidget(*val);
        return row;
    }

    QWidget *toggleRow(const QString &label, QCheckBox **cb, bool checked,
                       const QString &tooltip = QString())
    {
        auto *row = new QWidget;
        auto *lay = new QHBoxLayout(row);
        lay->setContentsMargins(8, 2, 8, 2);
        auto *lbl = valueLabel(label);
        *cb = new QCheckBox;
        (*cb)->setChecked(checked);
        if (!tooltip.isEmpty())
        {
            lbl->setToolTip(tooltip);
            (*cb)->setToolTip(tooltip);
        }
        lay->addWidget(lbl, 1);
        lay->addWidget(*cb);
        return row;
    }

    QWidget *comboRow(const QString &label, QComboBox **combo, const QStringList &items, int cur)
    {
        auto *row = new QWidget;
        auto *lay = new QHBoxLayout(row);
        lay->setContentsMargins(8, 2, 8, 2);
        lay->setSpacing(10);
        auto *lbl = valueLabel(label);
        lbl->setMinimumWidth(110);
        *combo = new QComboBox;
        (*combo)->addItems(items);
        (*combo)->setCurrentIndex(cur);
        (*combo)->setMinimumWidth(220);
        lay->addWidget(lbl);
        lay->addWidget(*combo, 1);
        return row;
    }

    // Window-size presets shared with the in-game overlay.
    constexpr int kWinW[] = {1024, 1280, 1360, 1366, 1440, 1600, 1920, 2560, 3440, 3840};
    constexpr int kWinH[] = {768, 720, 768, 768, 900, 900, 1080, 1440, 1440, 2160};
    constexpr int kWinCount = 10;

    constexpr const char *kDotColor[3] = {"#3fba4f", "#d1991f", "#f75245"};   // ok / fallback / fail

    void paintDot(QLabel *l, int state)
    {
        l->setStyleSheet(QStringLiteral("color:%1;font-weight:bold;")
                             .arg(QString::fromLatin1(kDotColor[std::clamp(state, 0, 2)])));
    }

    QString monitorValue(int idx)
    {
        const auto screens = QGuiApplication::screens();
        if (idx < 0 || idx >= screens.size() || !screens[idx])
            return QStringLiteral("%1 - ?").arg(idx + 1);
        const QScreen *sc = screens[idx];
        const QString name = sc->model().isEmpty() ? sc->name() : sc->model();
        return QStringLiteral("%1 - %2").arg(idx + 1).arg(name.isEmpty() ? QStringLiteral("?") : name);
    }

    QString monitorNote(int idx)
    {
        const auto screens = QGuiApplication::screens();
        if (idx < 0 || idx >= screens.size() || !screens[idx])
            return QStringLiteral("not detected");
        const QScreen *sc = screens[idx];
        const QRect g = sc->geometry();
        return QStringLiteral("%1x%2 @%3Hz")
            .arg(g.width()).arg(g.height()).arg(qRound(sc->refreshRate()));
    }

    QStringList monitorItems()
    {
        QStringList items;
        const auto screens = QGuiApplication::screens();
        for (int i = 0; i < screens.size(); ++i)
            items << QStringLiteral("%1 - %2 - %3").arg(monitorValue(i)).arg(monitorNote(i));
        if (items.isEmpty()) items << QStringLiteral("1 - Primary");
        return items;
    }

    // Shared shell for the two settings dialogs: Apply = write through to SettingsManager (in memory),
    // Save = + persist, Reset = back to the values it opened with, Close = discard and close.
    class VideoDialog : public QDialog
    {
    public:
        VideoDialog(const QString &title, QWidget *parent) : QDialog(parent)
        {
            setWindowTitle(title);
            setModal(true);
            setMinimumWidth(520);
        }

        std::function<void()> onApplied;

    protected:
        virtual void loadOpened() = 0;       // snapshot -> widgets
        virtual void writeThrough() = 0;     // widgets -> SettingsManager
        virtual void snapshot() = 0;         // widgets -> snapshot

        QHBoxLayout *buttonRow()
        {
            auto *row = new QHBoxLayout;
            auto *reset = new QPushButton(QStringLiteral("Reset"));
            auto *closeBtn = new QPushButton(QStringLiteral("Close"));
            auto *apply = new QPushButton(QStringLiteral("Apply"));
            auto *save = new QPushButton(QStringLiteral("Save"));
            for (auto *b : {reset, closeBtn, apply, save}) b->setMinimumWidth(96);
            connect(reset, &QPushButton::clicked, this, [this] { loadOpened(); });
            connect(closeBtn, &QPushButton::clicked, this, [this] { loadOpened(); close(); });
            connect(apply, &QPushButton::clicked, this, [this] {
                writeThrough();
                if (onApplied) onApplied();
            });
            connect(save, &QPushButton::clicked, this, [this] {
                writeThrough();
                SettingsManager::instance().save();
                if (onApplied) onApplied();
                close();
            });
            row->addWidget(reset);
            row->addWidget(closeBtn);
            row->addStretch(1);
            row->addWidget(apply);
            row->addWidget(save);
            return row;
        }
    };

    // ---------------------------------------------------------------------------------------------
    // Display settings: resolution, render scale, monitor, window mode.
    // ---------------------------------------------------------------------------------------------
    class DisplayDialog : public VideoDialog
    {
    public:
        explicit DisplayDialog(QWidget *parent) : VideoDialog(QStringLiteral("Display settings"), parent)
        {
            auto &s = SettingsManager::instance();
            auto *root = new QVBoxLayout(this);

            root->addWidget(sectionLabel(QStringLiteral("RESOLUTION")));
            m_res = new QComboBox;
            for (int i = 0; i < kWinCount; ++i)
            {
                m_res->addItem(QStringLiteral("%1 x %2").arg(kWinW[i]).arg(kWinH[i]));
                m_resW << kWinW[i];
                m_resH << kWinH[i];
            }
            {
                QString custom = QStringLiteral("%1 x %2 (custom)").arg(s.windowW()).arg(s.windowH());
                int found = -1;
                for (int i = 0; i < kWinCount; ++i)
                    if (kWinW[i] == s.windowW() && kWinH[i] == s.windowH()) { found = i; break; }
                if (found < 0 && s.windowW() > 0 && s.windowH() > 0)
                {
                    m_res->addItem(custom);
                    m_resW << s.windowW();
                    m_resH << s.windowH();
                }
            }
            root->addWidget(m_res);
            root->addWidget(hintRow(QStringLiteral("The projection FOV follows the window aspect, so a wider "
                                                   "window genuinely shows more stage. Windowed mode only.")));

            root->addWidget(sectionLabel(QStringLiteral("RENDER SCALE")));
            {
                auto *row = new QHBoxLayout;
                auto *box = new QWidget;
                auto *bl = new QHBoxLayout(box);
                bl->setContentsMargins(8, 2, 8, 2);
                m_x1 = new QRadioButton(QStringLiteral("x1"));
                m_x2 = new QRadioButton(QStringLiteral("x2"));
                m_x3 = new QRadioButton(QStringLiteral("x3"));
                bl->addWidget(m_x1);
                bl->addWidget(m_x2);
                bl->addWidget(m_x3);
                bl->addStretch(1);
                row->addWidget(box, 1);
                root->addLayout(row);
            }
            root->addWidget(hintRow(QStringLiteral("paraLLEl-GS: 1x / 2x / 3x = 1 / 4 / 8 samples per pixel, "
                                                   "applies live. OpenGL (New) applies on restart.")));

            root->addWidget(sectionLabel(QStringLiteral("MONITOR")));
            m_mon = new QComboBox;
            m_mon->addItems(monitorItems());
            root->addWidget(m_mon);

            root->addWidget(sectionLabel(QStringLiteral("WINDOW MODE")));
            {
                auto *box = new QWidget;
                auto *bl = new QHBoxLayout(box);
                bl->setContentsMargins(8, 2, 8, 2);
                m_win = new QRadioButton(QStringLiteral("Windowed (resizable)"));
                m_borderless = new QRadioButton(QStringLiteral("Borderless"));
                m_full = new QRadioButton(QStringLiteral("Fullscreen"));
                bl->addWidget(m_win);
                bl->addWidget(m_borderless);
                bl->addWidget(m_full);
                bl->addStretch(1);
                root->addWidget(box);
            }
            root->addWidget(hintRow(QStringLiteral("Windowed and borderless use the resolution above; fullscreen "
                                                   "uses the monitor's current mode.")));

            root->addLayout(buttonRow());
            // Preselect from the saved settings: without this the widgets keep their construction
            // defaults (monitor index 0, first resolution, no radio checked) and the dialog showed --
            // and Saved -- the wrong values, so a chosen monitor never appeared to stick.
            {
                int res = 0;
                for (int i = 0; i < kWinCount; ++i)
                    if (kWinW[i] == s.windowW() && kWinH[i] == s.windowH()) { res = i; break; }
                m_res->setCurrentIndex(res);
                const int scale = std::clamp(s.renderScale(), 1, 3);
                (scale >= 3 ? m_x3 : scale == 2 ? m_x2 : m_x1)->setChecked(true);
                m_mon->setCurrentIndex(std::clamp(s.monitor(), 0, std::max(0, m_mon->count() - 1)));
                const int mode = s.windowMode();
                (mode == 2 ? m_full : mode == 1 ? m_borderless : m_win)->setChecked(true);
            }
            m_opened = capture();
            loadOpened();
        }

    private:
        struct Snap
        {
            int res = 0, scale = 1, mon = 0, mode = 0;
        };

        Snap capture() const
        {
            Snap v;
            v.res = m_res->currentIndex();
            v.scale = m_x1->isChecked() ? 1 : m_x2->isChecked() ? 2 : 3;
            v.mon = m_mon->currentIndex();
            v.mode = m_win->isChecked() ? 0 : m_borderless->isChecked() ? 1 : 2;
            return v;
        }

        void loadOpened() override
        {
            m_res->setCurrentIndex(std::clamp(m_opened.res, 0, m_res->count() - 1));
            m_mon->setCurrentIndex(std::clamp(m_opened.mon, 0, m_mon->count() - 1));
            (m_opened.scale >= 3 ? m_x3 : m_opened.scale == 2 ? m_x2 : m_x1)->setChecked(true);
            (m_opened.mode == 2 ? m_full : m_opened.mode == 1 ? m_borderless : m_win)->setChecked(true);
        }

        void snapshot() override { m_opened = capture(); }
        void writeThrough() override
        {
            auto &s = SettingsManager::instance();
            const Snap v = capture();
            if (v.res >= 0 && v.res < m_resW.size()) s.setWindowSize(m_resW[v.res], m_resH[v.res]);
            s.setMonitor(v.mon);
            s.setWindowMode(v.mode);
            s.setFullscreen(v.mode == 2);
            s.setRenderScale(std::clamp(v.scale, 1, 3));
        }

        QComboBox *m_res = nullptr, *m_mon = nullptr;
        QRadioButton *m_x1 = nullptr, *m_x2 = nullptr, *m_x3 = nullptr;
        QRadioButton *m_win = nullptr, *m_borderless = nullptr, *m_full = nullptr;
        QList<int> m_resW, m_resH;
        Snap m_opened;
    };

    // ---------------------------------------------------------------------------------------------
    // Visual effects: the renderer effects (same names as the overlay) + the filtering toggles.
    // ---------------------------------------------------------------------------------------------
    class EffectsDialog : public VideoDialog
    {
    public:
        explicit EffectsDialog(QWidget *parent) : VideoDialog(QStringLiteral("Visual Effects"), parent)
        {
            auto &s = SettingsManager::instance();
            auto *root = new QVBoxLayout(this);

            root->addWidget(sectionLabel(QStringLiteral("EFFECTS")));
            root->addWidget(toggleRow(QStringLiteral("Cel Outline"), &m_outline, s.outline()));
            m_inkRow = sliderPair(QStringLiteral("Ink Strength"), &m_ink, &m_inkVal, 100, 260,
                                  s.inkStrength(), "%d %");
            root->addWidget(m_inkRow);
            root->addWidget(hintRow(QStringLiteral("199% matches the console line. Lower = thinner/lighter ink.")));
            root->addWidget(toggleRow(QStringLiteral("Character Shadows"), &m_shadows, s.shadows()));
            root->addWidget(toggleRow(QStringLiteral("Depth-of-Field Blur"), &m_dof, s.dofBlur()));
            m_dofRow = sliderPair(QStringLiteral("Blur Reach"), &m_dofReach, &m_dofVal, 50, 400,
                                  s.dofZFar() / 1000, "%d k");
            root->addWidget(m_dofRow);
            root->addWidget(hintRow(QStringLiteral("Lower = blur reaches nearer to the camera. 200k matches the "
                                                   "console look.")));
            root->addWidget(toggleRow(QStringLiteral("Glow (Kaioken aura)"), &m_glow, s.glow(),
                                      QStringLiteral("Character/attack bloom. Applies on restart.")));

            root->addWidget(sectionLabel(QStringLiteral("FILTERING")));
            root->addWidget(toggleRow(QStringLiteral("Bilinear Filter"), &m_bilinear, s.bilinear()));
            root->addWidget(toggleRow(QStringLiteral("Force Filtering (smooth terrain)"), &m_forceBilinear,
                                      s.forceBilinear()));

            root->addLayout(buttonRow());

            connect(m_outline, &QCheckBox::toggled, this, [this](bool on) { m_inkRow->setVisible(on); });
            connect(m_dof, &QCheckBox::toggled, this, [this](bool on) { m_dofRow->setVisible(on); });
            connect(m_ink, &QSlider::valueChanged, this, [this](int v) {
                m_inkVal->setText(QString::number(v) + QStringLiteral(" %"));
            });
            connect(m_dofReach, &QSlider::valueChanged, this, [this](int v) {
                m_dofVal->setText(QString::number(v) + QStringLiteral(" k"));
            });

            m_opened = capture();
            loadOpened();
        }

    private:
        struct Snap
        {
            bool outline = true, shadows = true, dof = true, glow = true, bilinear = true, force = true;
            int ink = 199, reach = 200;
        };

        Snap capture() const
        {
            Snap v;
            v.outline = m_outline->isChecked();
            v.shadows = m_shadows->isChecked();
            v.dof = m_dof->isChecked();
            v.glow = m_glow->isChecked();
            v.bilinear = m_bilinear->isChecked();
            v.force = m_forceBilinear->isChecked();
            v.ink = m_ink->value();
            v.reach = m_dofReach->value();
            return v;
        }

        void loadOpened() override
        {
            m_outline->setChecked(m_opened.outline);
            m_shadows->setChecked(m_opened.shadows);
            m_dof->setChecked(m_opened.dof);
            m_glow->setChecked(m_opened.glow);
            m_bilinear->setChecked(m_opened.bilinear);
            m_forceBilinear->setChecked(m_opened.force);
            m_ink->setValue(m_opened.ink);
            m_dofReach->setValue(m_opened.reach);
            m_inkRow->setVisible(m_opened.outline);
            m_dofRow->setVisible(m_opened.dof);
        }

        void snapshot() override { m_opened = capture(); }
        void writeThrough() override
        {
            auto &s = SettingsManager::instance();
            const Snap v = capture();
            s.setOutline(v.outline);
            s.setInkStrength(v.ink);
            s.setShadows(v.shadows);
            s.setDofBlur(v.dof);
            s.setDofZFar(v.reach * 1000);
            s.setGlow(v.glow);
            s.setBilinear(v.bilinear);
            s.setForceBilinear(v.force);
        }

        QCheckBox *m_outline = nullptr, *m_shadows = nullptr, *m_dof = nullptr, *m_glow = nullptr;
        QCheckBox *m_bilinear = nullptr, *m_forceBilinear = nullptr;
        QWidget *m_inkRow = nullptr, *m_dofRow = nullptr;
        QSlider *m_ink = nullptr, *m_dofReach = nullptr;
        QLabel *m_inkVal = nullptr, *m_dofVal = nullptr;
        Snap m_opened;
    };

    // ---------------------------------------------------------------------------------------------
    // [texui] Texture Replacement: pack status + the pack options. The install/activate flow stays
    // in the Misc tab; this dialog configures the already-installed pack.
    // ---------------------------------------------------------------------------------------------
    class TexPackDialog : public VideoDialog
    {
    public:
        explicit TexPackDialog(QWidget *parent) : VideoDialog(QStringLiteral("Texture Replacement"), parent)
        {
            auto &s = SettingsManager::instance();
            auto *root = new QVBoxLayout(this);

            root->addWidget(sectionLabel(QStringLiteral("PACK STATUS")));

            const quint64 n = texpack::countReplacements();
            const QString texDir = texpack::dir();
            const bool installed = n > 0;
            {
                auto *row = new QWidget;
                auto *lay = new QHBoxLayout(row);
                lay->setContentsMargins(8, 2, 8, 2);
                lay->setSpacing(8);
                auto *dot = new QLabel(QStringLiteral("*"));
                dot->setFixedWidth(12);
                dot->setStyleSheet(QStringLiteral("color:%1;font-weight:bold;")
                                       .arg(installed ? QStringLiteral("#22c55e") : QStringLiteral("#ef4444")));
                auto *txt = valueLabel(installed
                                           ? QStringLiteral("Installed - %1 replacements").arg(n)
                                           : QStringLiteral("No texture pack indexed"));
                lay->addWidget(dot);
                lay->addWidget(txt, 1);
                root->addWidget(row);
            }
            if (installed)
            {
                const bool full = QDir(texDir + QStringLiteral("/replacements/Characters/Body")).exists();
                root->addWidget(hintRow(full ? QStringLiteral("Full pack: 3D + 2D")
                                             : QStringLiteral("Lite pack: 2D only")));
            }
            root->addWidget(hintRow(texDir));
            if (!installed)
                root->addWidget(hintRow(QStringLiteral(
                    "Install one from the Misc tab, or point PS2X_TEXREPLACE=<dir> at a pack.")));

            root->addWidget(sectionLabel(QStringLiteral("OPTIONS")));
            auto *box = new QWidget;
            auto *bl = new QVBoxLayout(box);
            bl->setContentsMargins(0, 0, 0, 0);
            bl->addWidget(toggleRow(QStringLiteral("Video overlay (4K intro)"), &m_intro, s.introVideo()));
            bl->addWidget(comboRow(QStringLiteral("Buttons style"), &m_buttons,
                                   {QStringLiteral("PS2"), QStringLiteral("Xbox")}, s.buttonLayout()));
            box->setEnabled(installed);
            root->addWidget(box);
            root->addWidget(hintRow(QStringLiteral(
                "Video overlay replaces the opening movie with the pack's 4K clip. Both options "
                "apply on restart. Enable/disable the pack from the Misc tab.")));

            root->addLayout(buttonRow());
            m_opened = capture();
            loadOpened();
        }

    private:
        struct Snap { bool intro = true; int buttons = 1; };

        Snap capture() const { return { m_intro->isChecked(), m_buttons->currentIndex() }; }
        void loadOpened() override { m_intro->setChecked(m_opened.intro); m_buttons->setCurrentIndex(m_opened.buttons); }
        void snapshot() override { m_opened = capture(); }
        void writeThrough() override
        {
            auto &s = SettingsManager::instance();
            const Snap v = capture();
            s.setIntroVideo(v.intro);
            s.setButtonLayout(v.buttons);
        }

        QCheckBox *m_intro = nullptr;
        QComboBox *m_buttons = nullptr;
        Snap m_opened;
    };
} // namespace

VideoTab::VideoTab(QWidget *parent)
    : QWidget(parent)
{
    auto &s = SettingsManager::instance();

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; }"
                                         "QScrollArea > QWidget > QWidget { background: transparent; }"));

    auto *content = new QWidget;
    auto *root = new QVBoxLayout(content);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(6);

    // STATUS: what the launcher will ask the runtime to run. Green = as configured, amber = downgraded
    // (clamped monitor, restart needed), red = unavailable.
    root->addWidget(sectionLabel(QStringLiteral("STATUS")));
    {
        auto *grid = new QGridLayout;
        grid->setContentsMargins(8, 4, 8, 4);
        grid->setHorizontalSpacing(10);
        grid->setVerticalSpacing(4);
        static const char *const kNames[4] = {"Renderer", "Monitor", "Resolution", "Upscale"};
        for (int r = 0; r < 4; ++r)
        {
            m_dot[r] = new QLabel(QStringLiteral("*"));
            m_dot[r]->setFixedWidth(12);
            auto *nm = valueLabel(QString::fromLatin1(kNames[r]));
            nm->setMinimumWidth(90);
            m_val[r] = valueLabel();
            m_val[r]->setMinimumWidth(190);
            m_note[r] = hintRow(QString());
            grid->addWidget(m_dot[r], r, 0);
            grid->addWidget(nm, r, 1);
            grid->addWidget(m_val[r], r, 2);
            grid->addWidget(m_note[r], r, 3);
        }
        grid->setColumnStretch(3, 1);
        root->addLayout(grid);
    }

    // The two dialogs, same shape as the in-game overlay.
    {
        auto *row = new QHBoxLayout;
        auto *disp = new QPushButton(QStringLiteral("Display settings..."));
        auto *fx = new QPushButton(QStringLiteral("Visual Effects..."));
        auto *tex = new QPushButton(QStringLiteral("Texture Replacement..."));
        disp->setMinimumWidth(200);
        fx->setMinimumWidth(200);
        tex->setMinimumWidth(200);
        connect(disp, &QPushButton::clicked, this, &VideoTab::openDisplayDialog);
        connect(fx, &QPushButton::clicked, this, &VideoTab::openVisualEffectsDialog);
        connect(tex, &QPushButton::clicked, this, &VideoTab::openTexPackDialog);
        row->addWidget(disp);
        row->addWidget(fx);
        row->addWidget(tex);
        row->addStretch(1);
        root->addLayout(row);
    }

    // RENDERER (stays on the tab: it is the one switch that changes everything else)
    root->addWidget(sectionLabel(QStringLiteral("RENDERER")));
    QStringList renderers = {QStringLiteral("OpenGL (New)"), QStringLiteral("Software (CPU)"),
                             QStringLiteral("paraLLEl-GS (Vulkan)")};
    const QList<int> rendererValues = {SettingsManager::kRendererOpenGL,
                                       SettingsManager::kRendererSoftware,
                                       SettingsManager::kRendererParallelGS};
    int curRenderer = 0;
    for (int i = 0; i < rendererValues.size(); ++i)
        if (rendererValues[i] == s.renderer()) { curRenderer = i; break; }
    root->addWidget(comboRow(QStringLiteral("Renderer"), &m_renderer, renderers, curRenderer));
    for (int i = 0; i < rendererValues.size(); ++i)
        m_renderer->setItemData(i, rendererValues[i]);
    root->addWidget(hintRow(QStringLiteral(
        "paraLLEl-GS is the default backend (Vulkan compute; falls back to OpenGL (New) if Vulkan is "
        "unavailable). OpenGL (New) presents through our own GL layer; Software uses the CPU rasterizer "
        "and has no upscale.")));
    root->addWidget(hintRow(QStringLiteral("Takes full effect after restart.")));

    root->addStretch(1);
    scroll->setWidget(content);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);

    connect(m_renderer, &QComboBox::currentIndexChanged, this, [this](int) {
        SettingsManager::instance().setRenderer(m_renderer->currentData().toInt());
        refreshStatus();
    });

    refreshStatus();
}

void VideoTab::refreshStatus()
{
    const auto &s = SettingsManager::instance();
    const auto screens = QGuiApplication::screens();

    // Renderer
    {
        const int r = s.renderer();
        const char *name = "OpenGL (New)";
        if (r == SettingsManager::kRendererSoftware) name = "Software rasterizer";
        else if (r == SettingsManager::kRendererParallelGS) name = "paraLLEl-GS (Vulkan)";
        else if (r == SettingsManager::kRendererD3D11) name = "Direct3D 11";
        const bool soft = (r == SettingsManager::kRendererSoftware);
        paintDot(m_dot[0], soft ? 1 : 0);
        m_val[0]->setText(QString::fromLatin1(name));
        m_note[0]->setText(soft ? QStringLiteral("CPU path: no upscale")
                                : (r == SettingsManager::kRendererParallelGS
                                       ? QStringLiteral("Vulkan compute (default)")
                                       : QStringLiteral("own GL present")));
    }

    // Monitor
    {
        const int asked = s.monitor();
        const int count = screens.size();
        const int idx = count > 0 ? std::clamp(asked, 0, count - 1) : 0;
        const bool clamped = count > 0 && asked != idx;
        paintDot(m_dot[1], count > 0 ? (clamped ? 1 : 0) : 2);
        m_val[1]->setText(monitorValue(idx));
        m_note[1]->setText(monitorNote(idx) + (clamped ? QStringLiteral("  (requested monitor missing: clamped)")
                                                       : QString()));
    }

    // Resolution + window mode
    {
        const int mode = s.windowMode();
        QString val, note;
        if (mode == 2)
        {
            const QScreen *sc = screens.isEmpty() ? nullptr
                                                  : screens[std::clamp(s.monitor(), 0, int(screens.size()) - 1)];
            const QRect g = sc ? sc->geometry() : QRect();
            val = g.isValid() ? QStringLiteral("%1 x %2").arg(g.width()).arg(g.height())
                              : QStringLiteral("monitor mode");
            note = QStringLiteral("fullscreen");
        }
        else
        {
            const QScreen *sc = QGuiApplication::primaryScreen();
            const QRect g = sc ? sc->geometry() : QRect();
            const int w = s.windowW() > 0 ? s.windowW() : g.width();
            const int h = s.windowH() > 0 ? s.windowH() : g.height();
            val = QStringLiteral("%1 x %2").arg(w).arg(h);
            note = mode == 1 ? QStringLiteral("borderless windowed") : QStringLiteral("windowed");
        }
        paintDot(m_dot[2], 0);
        m_val[2]->setText(val);
        m_note[2]->setText(note);
    }

    // Upscale
    {
        const int r = s.renderer();
        const int scale = std::clamp(s.renderScale(), 1, 4);
        paintDot(m_dot[3], r == SettingsManager::kRendererSoftware ? 2
                             : r == SettingsManager::kRendererParallelGS ? 0 : 1);
        m_val[3]->setText(QStringLiteral("x%1").arg(scale));
        m_note[3]->setText(r == SettingsManager::kRendererSoftware
                               ? QStringLiteral("not available (software renderer)")
                           : r == SettingsManager::kRendererParallelGS
                               ? QStringLiteral("active (samples per pixel)")
                               : QStringLiteral("applies on restart"));
    }
}

void VideoTab::openDisplayDialog()
{
    DisplayDialog dlg(this);
    dlg.onApplied = [this] { refreshStatus(); };
    dlg.exec();
}

void VideoTab::openVisualEffectsDialog()
{
    EffectsDialog dlg(this);
    dlg.onApplied = [this] { refreshStatus(); };
    dlg.exec();
}

void VideoTab::openTexPackDialog()
{
    TexPackDialog dlg(this);
    dlg.onApplied = [this] { refreshStatus(); };
    dlg.exec();
}
