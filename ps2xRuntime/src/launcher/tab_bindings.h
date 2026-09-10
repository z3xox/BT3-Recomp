#pragma once

#include "input_reader.h"
#include "pad_config_reader.h"

#include <QElapsedTimer>
#include <QWidget>
#include <array>
#include <string>
#include <vector>

class QComboBox;
class QEvent;
class QLabel;
class QTableWidget;
class QTimer;

// Editable per-player bindings table (2 players). Reads/writes pad.conf using
// the exact runtime format; same 24 actions as the in-game bindings table.
class BindingsTab : public QWidget
{
    Q_OBJECT
public:
    explicit BindingsTab(QWidget *parent = nullptr);
    ~BindingsTab();
    bool load();
    bool save();

    // Shared player state (devices + binds). The Controllers tab reads/writes
    // the same array so a device chosen there matches the Bindings tab.
    std::array<padconf::Player, 2> &players() { return m_players; }

    // pad.conf lives beside the launcher/game ELF (deploy root), matching
    // PadConfig::legacyConfigPath() in the runtime; per-player files live in
    // the shared savedata/ folder (savedata/pad_p1.conf, pad_p2.conf).
    std::string padconfPath() const;
    std::string padconfLegacyPath() const;
    std::string playerConfigPath(int p) const;
    bool padconfFileExists(const std::string &path) const;

private:
    QComboBox *m_player = nullptr;
    QComboBox *m_device = nullptr;
    QTableWidget *m_table = nullptr;
    QTimer *m_timer = nullptr;
    QLabel *m_captureStatus = nullptr;

    std::array<padconf::Player, 2> m_players{};
    std::vector<evin::DeviceInfo> m_filteredDevices;

    // Reader for bind capture (opened once per device change).
    evin::Reader m_reader;
    std::string m_openedNode;

    int m_captureRow = -1;
    // Inputs already "down" when Set was clicked are locked out until they go
    // back to neutral, so a latched button from the previous capture (or a
    // drifting axis) can't immediately re-fire on the next row.
    std::array<uint8_t, 16> m_ignoredBtns{};
    std::array<uint8_t, 6> m_ignoredAxes{};
    QElapsedTimer m_elapsed;

    void openCaptureDevice();
    void refreshDevices();
    void onLoadDefaults();
    void onDeviceChanged(int g);
    int actionAtRow(int row) const { return row; }
    void refreshRow(int action);
    void onPlayerChanged(int);
    void onBindClicked();
    void pollCapture();
    void finishCapture();
    void setStatus(const QString &text);

    bool eventFilter(QObject *watched, QEvent *event) override;
};