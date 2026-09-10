#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QSlider;
class QStackedWidget;

// Toggle that looks like the in-game ON/OFF badge: a checkbox styled via the
// DBZ QSS, matching the ImGui ToggleSwitch rows.
class VideoTab : public QWidget
{
    Q_OBJECT
public:
    explicit VideoTab(QWidget *parent = nullptr);

private slots:
    void onOutline(bool);
    void onDof(bool);
    void onWidescreen(bool);
    void onHudLayout(int);

private:
    QComboBox *m_renderer = nullptr;
    QCheckBox *m_outline = nullptr;
    QWidget *m_inkRow = nullptr;
    QSlider *m_ink = nullptr;
    QLabel *m_inkVal = nullptr;
    QCheckBox *m_shadows = nullptr;
    QCheckBox *m_dof = nullptr;
    QWidget *m_dofRow = nullptr;
    QSlider *m_dofReach = nullptr;
    QLabel *m_dofVal = nullptr;
    QCheckBox *m_postfx = nullptr;
    QCheckBox *m_glow = nullptr;
    QComboBox *m_res = nullptr;
    QCheckBox *m_bilinear = nullptr;
    QCheckBox *m_forceBilinear = nullptr;
    QCheckBox *m_fullscreen = nullptr;
    QCheckBox *m_widescreen = nullptr;
    QWidget *m_hudRow = nullptr;
    QComboBox *m_hudLayout = nullptr;
    QWidget *m_hudCustom = nullptr;
    QSlider *m_offL = nullptr, *m_offC = nullptr, *m_offR = nullptr;
    QLabel *m_offLVal = nullptr, *m_offCVal = nullptr, *m_offRVal = nullptr;
    QComboBox *m_winSize = nullptr;

    QWidget *makeTogglePair(const QString &label, QCheckBox **cb);
};