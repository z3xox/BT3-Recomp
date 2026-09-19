#pragma once

#include <QWidget>

class QComboBox;
class QLabel;

// Video tab: renderer combo + STATUS summary (same dot language as the in-game overlay) + the two
// settings dialogs. The dialogs write through SettingsManager: Apply = in-memory, Save = + persist to
// savedata/settings.toml, Reset = back to the values it opened with, Close = discard.
class VideoTab : public QWidget
{
    Q_OBJECT
public:
    explicit VideoTab(QWidget *parent = nullptr);

public slots:
    void refreshStatus();

private:
    void openDisplayDialog();
    void openVisualEffectsDialog();
    void openTexPackDialog();

    QComboBox *m_renderer = nullptr;
    QLabel *m_dot[4] = {};
    QLabel *m_val[4] = {};
    QLabel *m_note[4] = {};
};
