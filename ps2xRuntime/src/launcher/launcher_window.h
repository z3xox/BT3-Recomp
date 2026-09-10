#pragma once

#include <QMainWindow>
#include <QString>

class QLabel;
class QPushButton;
class QProcess;
class QWidget;

class LauncherWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit LauncherWindow(QWidget *parent = nullptr);

    // Absolute path to the playable game ELF (self-extracting BT3SELFX binary)
    // found next to the launcher. Empty if none detected.
    static QString findGameElf();

private slots:
    void onPlayClicked();
    void onSettingsClicked();

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;

private:
    void loadBackground();
    void resolveLaunchTarget();
    void updateHint();
    void checkGameData();
    bool openInstallWizard();

    QLabel *m_hint = nullptr;
    QPushButton *m_play = nullptr;
    QPushButton *m_settings = nullptr;
    QWidget *m_bottomBar = nullptr;
    QProcess *m_gameProc = nullptr;

    QString m_gameElf;
    QString m_bgPath;
    QString m_savedataDir;
    QString m_dataDir;
    bool m_plainRunner = false;
    bool m_gameDataValid = false;
    bool m_wizardShown = false;
};