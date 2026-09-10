#pragma once

#include "input_reader.h"
#include "pad_config_reader.h"

#include <QLabel>
#include <QWidget>
#include <array>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QSlider;
class QTimer;

// Live stick visual (axes dot), used by the gamepad test area.
class StickWidget : public QLabel
{
public:
    StickWidget();
    void setDot(qreal x, qreal y);
protected:
    void paintEvent(QPaintEvent *) override;
private:
    QPointF m_dot;
};

// Vertical trigger gauge.
class Gauge : public QLabel
{
public:
    Gauge();
    void setValue(float v);
protected:
    void paintEvent(QPaintEvent *) override;
private:
    float m_v = 0.0f;
};

// Gamepad live test widget: button grid + sticks + triggers, polling the
// selected joystick on a 16ms timer. Reuses the same numbering convention as
// the in-game overlay's test area.
class ControllersTab : public QWidget
{
    Q_OBJECT
public:
    explicit ControllersTab(std::array<padconf::Player, 2> *shared,
                            QWidget *parent = nullptr);

    // Re-read the shared per-player pad devices after BindingsTab::load() ran.
    void syncFromPads();

private:
    QComboBox *m_player = nullptr;
    QComboBox *m_device = nullptr;
    QSlider *m_deadzone = nullptr;
    QLabel *m_deadzoneVal = nullptr;
    QCheckBox *m_overlayEnabled = nullptr;
    QTimer *m_timer = nullptr;

    // Gamepad test widgets
    struct BtnWidgets { QLabel *widget; int code; };
    std::vector<BtnWidgets> m_buttons;
    StickWidget *m_stickL = nullptr, *m_stickR = nullptr;
    Gauge *m_lt = nullptr, *m_rt = nullptr;
    QLabel *m_axisReadout = nullptr;

    // Persisted input reader (opened once per device selection). Polling an
    // already-open reader is cheap; recreating Reader + listDevices() every
    // 16 ms is what turned the Controllers tab (and the whole dialog) sluggish.
    evin::Reader m_reader;
    std::string m_openedNode;
    bool m_devWasOpen = false;

    // Physical devices (filtered to gamepads), used only to resolve a semantic
    // combo choice to a joystick node for the live test.
    std::vector<evin::DeviceInfo> m_filteredDevices;

    // Shared per-player state from BindingsTab; device combo loads/saves the
    // selected player's pad.device here (same semantics as the Bindings tab).
    std::array<padconf::Player, 2> *m_players = nullptr;

    void openDevice();
    void pollGamepad();
    void refreshDevices();
    void onDeadzone(int);
    void onPlayerChanged();
    void onDeviceChanged();
};