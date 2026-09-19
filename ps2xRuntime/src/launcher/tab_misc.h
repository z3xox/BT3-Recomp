#pragma once

#include <QWidget>

class QCheckBox;
class QLabel;
class QPushButton;

// "Misc" settings tab: game data status (size + validity dot), the Install
// Wizard, and the texture-pack section (enable toggle, status dot, install).
class MiscTab : public QWidget
{
    Q_OBJECT
public:
    explicit MiscTab(QWidget *parent = nullptr);
    void refresh();

private slots:
    void onReinstallMode(bool on);
    void onInstallWizard();
    void onOpenFolder();
    void onTexPackToggled(bool on);
    void onInstallPack();
    void onTexPackFolder();
    void onDeleteTexCache();
    void onTexCacheToggled(bool on);

private:
    QLabel *m_size = nullptr;
    QLabel *m_dot = nullptr;
    QLabel *m_dotText = nullptr;
    QCheckBox *m_reinstall = nullptr;
    QPushButton *m_wizardBtn = nullptr;
    QPushButton *m_browse = nullptr;

    // [texreplace]
    QCheckBox *m_texCheck = nullptr;
    QLabel *m_texDot = nullptr;
    QLabel *m_texDotText = nullptr;
    QPushButton *m_texInstall = nullptr;
    QPushButton *m_texFolder = nullptr;

    // [texcache]
    QCheckBox *m_tcCheck = nullptr;
    QLabel *m_tcStatus = nullptr;
    QPushButton *m_tcDelete = nullptr;
};
