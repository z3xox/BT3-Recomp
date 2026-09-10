#include "tab_video.h"

#include "settings_manager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSlider>
#include <QVBoxLayout>
#include <algorithm>

namespace
{
    QWidget *sectionLabel(const QString &text)
    {
        auto *l = new QLabel(text);
        l->setObjectName(QStringLiteral("sectionLabel"));
        return l;
    }

    QWidget *hintRow(const QString &text)
    {
        auto *l = new QLabel(text);
        l->setObjectName(QStringLiteral("hintLabel"));
        l->setWordWrap(true);
        return l;
    }

    QWidget *sliderPair(const QString &label, QSlider **sl, QLabel **val,
                        int lo, int hi, int cur, const char *fmt = "%d")
    {
        auto *row = new QWidget;
        auto *lay = new QHBoxLayout(row);
        lay->setContentsMargins(8, 2, 8, 2);
        lay->setSpacing(10);
        auto *lbl = new QLabel(label);
        lbl->setObjectName(QStringLiteral("valueLabel"));
        lbl->setMinimumWidth(110);
        *sl = new QSlider(Qt::Horizontal);
        (*sl)->setRange(lo, hi);
        (*sl)->setValue(cur);
        *val = new QLabel(QString::asprintf(fmt, cur));
        (*val)->setObjectName(QStringLiteral("valueLabel"));
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
        auto *lbl = new QLabel(label);
        lbl->setObjectName(QStringLiteral("valueLabel"));
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

    QWidget *comboRow(const QString &label, QComboBox **combo, const QStringList &items,
                      int cur)
    {
        auto *row = new QWidget;
        auto *lay = new QHBoxLayout(row);
        lay->setContentsMargins(8, 2, 8, 2);
        lay->setSpacing(10);
        auto *lbl = new QLabel(label);
        lbl->setObjectName(QStringLiteral("valueLabel"));
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
    constexpr int kWinW[] = {1024, 1280, 1360, 1366, 1440, 1600, 1920, 2560, 3440};
    constexpr int kWinH[] = {768, 720, 768, 768, 900, 900, 1080, 1440, 1440};
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

    // RENDERER
    root->addWidget(sectionLabel(QStringLiteral("RENDERER")));
    QStringList renderers = {
        QStringLiteral("OpenGL"),
        QStringLiteral("Software (CPU)"),
        QStringLiteral("paraLLEl-GS (Vulkan)")};
    const int curRenderer = std::min(std::max(s.renderer(), 0), 2);
    root->addWidget(comboRow(QStringLiteral("Renderer"), &m_renderer, renderers, curRenderer));
    root->addWidget(hintRow(QStringLiteral("paraLLEl-GS is the default backend. Falls back to OpenGL if Vulkan is unavailable.")));
    root->addWidget(toggleRow(QStringLiteral("Cel Outline"), &m_outline, s.outline()));
    m_inkRow = sliderPair(QStringLiteral("Ink Strength"), &m_ink, &m_inkVal, 100, 260,
                          s.inkStrength(), "%d %%");
    root->addWidget(m_inkRow);
    m_inkRow->setVisible(s.outline());
    root->addWidget(hintRow(QStringLiteral("199%% matches the console line. Lower = thinner/lighter ink.")));
    root->addWidget(toggleRow(QStringLiteral("Character Shadows"), &m_shadows, s.shadows()));
    root->addWidget(toggleRow(QStringLiteral("Depth-of-Field Blur"), &m_dof, s.dofBlur()));
    m_dofRow = sliderPair(QStringLiteral("Blur Reach"), &m_dofReach, &m_dofVal, 50, 400,
                          s.dofZFar() / 1000, "%d k");
    root->addWidget(m_dofRow);
    m_dofRow->setVisible(s.dofBlur());
    root->addWidget(hintRow(QStringLiteral("Lower = blur reaches nearer to the camera. 200k matches the console look.")));
    root->addWidget(toggleRow(QStringLiteral("Post-FX"), &m_postfx, s.postfx()));
    root->addWidget(toggleRow(QStringLiteral("Glow (Kaioken aura)"), &m_glow, s.glow(),
                              QStringLiteral("Character/attack bloom. Applies on restart.")));

    // QUALITY
    root->addWidget(sectionLabel(QStringLiteral("QUALITY")));
    QStringList resItems;
    for (int i = 1; i <= 4; ++i)
        resItems << (i == 1 ? QStringLiteral("Native (1x)") : QString::number(i) + QStringLiteral("x"));
    root->addWidget(comboRow(QStringLiteral("Internal Resolution"), &m_res, resItems,
                             s.renderScale() - 1));
    root->addWidget(hintRow(QStringLiteral("Applies on restart.")));

    // FILTERING
    root->addWidget(sectionLabel(QStringLiteral("FILTERING")));
    root->addWidget(toggleRow(QStringLiteral("Bilinear Filter"), &m_bilinear, s.bilinear()));
    root->addWidget(toggleRow(QStringLiteral("Force Filtering (smooth terrain)"), &m_forceBilinear,
                              s.forceBilinear()));

    // DISPLAY
    root->addWidget(sectionLabel(QStringLiteral("DISPLAY")));
    root->addWidget(toggleRow(QStringLiteral("Fullscreen"), &m_fullscreen, s.fullscreen()));
    root->addWidget(toggleRow(QStringLiteral("Widescreen (true FOV)"), &m_widescreen, s.widescreen()));
    m_hudRow = comboRow(QStringLiteral("HUD Layout"),
                        &m_hudLayout,
                        {QStringLiteral("Centered (4:3 block)"), QStringLiteral("Edge-pinned (wide)"),
                         QStringLiteral("Custom (sliders)")},
                        s.hudLayout());
    root->addWidget(m_hudRow);
    m_hudRow->setVisible(s.widescreen());

    m_hudCustom = new QWidget;
    auto *hudLay = new QVBoxLayout(m_hudCustom);
    hudLay->setContentsMargins(8, 0, 8, 0);
    hudLay->setSpacing(2);
    hudLay->addWidget(sliderPair(QStringLiteral("Left cluster"), &m_offL, &m_offLVal,
                                 -120, 120, s.hudOffL(), "%d px"));
    hudLay->addWidget(sliderPair(QStringLiteral("Timer"), &m_offC, &m_offCVal,
                                 -120, 120, s.hudOffC(), "%d px"));
    hudLay->addWidget(sliderPair(QStringLiteral("Right cluster"), &m_offR, &m_offRVal,
                                 -120, 120, s.hudOffR(), "%d px"));
    hudLay->addWidget(hintRow(QStringLiteral("offsets from the edge-pinned layout; bars auto-stretch")));
    root->addWidget(m_hudCustom);
    m_hudCustom->setVisible(s.widescreen() && s.hudLayout() == 2);

    QStringList winItems = {
        QStringLiteral("1024 x 768 (4:3)"), QStringLiteral("1280 x 720"),
        QStringLiteral("1360 x 768"), QStringLiteral("1366 x 768"),
        QStringLiteral("1440 x 900"), QStringLiteral("1600 x 900"),
        QStringLiteral("1920 x 1080"), QStringLiteral("2560 x 1440"),
        QStringLiteral("3440 x 1440 (ultrawide)")};
    int curWin = -1;
    for (int i = 0; i < 9; ++i)
        if (kWinW[i] == s.windowW() && kWinH[i] == s.windowH())
        {
            curWin = i;
            break;
        }
    auto *winRow = new QWidget;
    auto *winLay = new QHBoxLayout(winRow);
    winLay->setContentsMargins(8, 2, 8, 2);
    winLay->setSpacing(10);
    auto *wl = new QLabel(QStringLiteral("Window Size"));
    wl->setObjectName(QStringLiteral("valueLabel"));
    wl->setMinimumWidth(100);
    m_winSize = new QComboBox;
    m_winSize->addItems(winItems);
    m_winSize->setCurrentIndex(curWin < 0 ? 0 : curWin);
    m_winSize->setMinimumWidth(220);
    winLay->addWidget(wl);
    winLay->addWidget(m_winSize, 1);
    root->addWidget(winRow);
    root->addWidget(hintRow(QStringLiteral("Key F11 in-game toggles fullscreen; resolve presets apply here.")));

    root->addStretch(1);
    scroll->setWidget(content);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);

    // Live write-through into SettingsManager (Save persists to INI).
    connect(m_renderer, &QComboBox::currentIndexChanged, this, [](int v) {
        SettingsManager::instance().setRenderer(v);
    });
    connect(m_outline, &QCheckBox::toggled, this, &VideoTab::onOutline);
    connect(m_ink, &QSlider::valueChanged, this, [this](int v) {
        m_inkVal->setText(QString::number(v) + QStringLiteral("%%"));
        SettingsManager::instance().setInkStrength(v);
    });
    connect(m_shadows, &QCheckBox::toggled, this, [](bool v) { SettingsManager::instance().setShadows(v); });
    connect(m_dof, &QCheckBox::toggled, this, &VideoTab::onDof);
    connect(m_dofReach, &QSlider::valueChanged, this, [this](int v) {
        m_dofVal->setText(QString::number(v) + QStringLiteral(" k"));
        SettingsManager::instance().setDofZFar(v * 1000);
    });
    connect(m_postfx, &QCheckBox::toggled, this, [](bool v) { SettingsManager::instance().setPostfx(v); });
    connect(m_glow, &QCheckBox::toggled, this, [](bool v) { SettingsManager::instance().setGlow(v); });
    connect(m_res, &QComboBox::currentIndexChanged, this, [](int i) {
        SettingsManager::instance().setRenderScale(i + 1);
    });
    connect(m_bilinear, &QCheckBox::toggled, this, [](bool v) { SettingsManager::instance().setBilinear(v); });
    connect(m_forceBilinear, &QCheckBox::toggled, this, [](bool v) { SettingsManager::instance().setForceBilinear(v); });
    connect(m_fullscreen, &QCheckBox::toggled, this, [](bool v) { SettingsManager::instance().setFullscreen(v); });
    connect(m_widescreen, &QCheckBox::toggled, this, &VideoTab::onWidescreen);
    connect(m_hudLayout, &QComboBox::currentIndexChanged, this, &VideoTab::onHudLayout);
    connect(m_offL, &QSlider::valueChanged, this, [this](int v) {
        m_offLVal->setText(QString::number(v) + QStringLiteral(" px"));
        SettingsManager::instance().setHudOffsets(v,
            SettingsManager::instance().hudOffC(), SettingsManager::instance().hudOffR());
    });
    connect(m_offC, &QSlider::valueChanged, this, [this](int v) {
        m_offCVal->setText(QString::number(v) + QStringLiteral(" px"));
        SettingsManager::instance().setHudOffsets(
            SettingsManager::instance().hudOffL(), v, SettingsManager::instance().hudOffR());
    });
    connect(m_offR, &QSlider::valueChanged, this, [this](int v) {
        m_offRVal->setText(QString::number(v) + QStringLiteral(" px"));
        SettingsManager::instance().setHudOffsets(
            SettingsManager::instance().hudOffL(), SettingsManager::instance().hudOffC(), v);
    });
    connect(m_winSize, &QComboBox::currentIndexChanged, this, [](int i) {
        SettingsManager::instance().setWindowSize(kWinW[i], kWinH[i]);
    });
}

void VideoTab::onOutline(bool on)
{
    m_inkRow->setVisible(on);
    SettingsManager::instance().setOutline(on);
}

void VideoTab::onDof(bool on)
{
    m_dofRow->setVisible(on);
    SettingsManager::instance().setDofBlur(on);
}

void VideoTab::onWidescreen(bool on)
{
    m_hudRow->setVisible(on);
    m_hudCustom->setVisible(on && m_hudLayout->currentIndex() == 2);
    SettingsManager::instance().setWidescreen(on);
}

void VideoTab::onHudLayout(int idx)
{
    m_hudCustom->setVisible(m_widescreen->isChecked() && idx == 2);
    SettingsManager::instance().setHudLayout(idx);
}