#pragma once

#include <QDialog>

#include "tex_pack.h"

class QLabel;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QThread;
class QTemporaryDir;
class ExtractWorker;
class AfsExtractWorker;

// End-user install wizard: detects missing/corrupt game data, lets the user
// point at their own disc dump (ISO or a container that wraps the ISO) and,
// after the embedded SHA-256 of SLUS_216.78 matches, extracts the game data
// tree into the deployment folder so the runner can boot it.
class InstallWizardDialog : public QDialog
{
    Q_OBJECT
public:
    explicit InstallWizardDialog(QWidget *parent = nullptr, bool reinstall = false);
    ~InstallWizardDialog() override;

    bool installed() const { return m_installed; }
    // True when the user chose to install a texture pack on the final
    // recommendation page. The caller opens the texture installer.
    bool wantTexturePack() const { return m_wantTexPack; }
    // Which variant was chosen: texpack::kPackLite (2D only) or texpack::kPackFull (3D + 2D).
    int texturePackChoice() const { return m_packChoice; }

private slots:
    void onNextMissing();
    void onBrowse();
    void onRetryDump();
    void onInstall();
    void onRetryInstall();
    void onExtractProgress(qint64 done, qint64 total);
    void onExtractDone(bool ok, const QString &msg);
    void onAfsStatus(const QString &text);
    void onAfsProgress(qint64 done, qint64 total);
    void onAfsDone(bool ok, const QString &msg);

private:
    void buildUi();
    void setIndex(int index);
    void setVerified(bool ok, const QString &text);
    void attemptVerify(const QString &dumpPath);
    // Unwraps a container (.rar/.7z/.zip/.tar...) and returns the inner disc
    // image path, or an empty string. On failure err holds the user message.
    QString resolveInnerImage(const QString &dumpPath, QString *err);
    void startExtraction();
    // Second install phase: turn every extracted PZS3US*.AFS into folder slots
    // (+ .idx) and drop the container, leaving folders as the only data source.
    void startAfsConversion();
    void applyInstallResult(bool ok, const QString &msg);

    QStackedWidget *m_stack = nullptr;

    QPushButton *m_nextMissing = nullptr; // page A
    QLabel *m_selected = nullptr;         // page B
    QPushButton *m_browse = nullptr;
    QLabel *m_dot = nullptr;
    QLabel *m_dotText = nullptr;
    QPushButton *m_nextDump = nullptr;
    QPushButton *m_retryDump = nullptr;

    QLabel *m_progressText = nullptr;  // page C
    QProgressBar *m_bar = nullptr;
    QLabel *m_activity = nullptr;      // live "what is happening now" line
    QLabel *m_doneLabel = nullptr;
    QPushButton *m_close = nullptr;
    QPushButton *m_retryInstall = nullptr;

    // Page D: post-install texture-pack recommendation (first install only).
    QPushButton *m_recLite = nullptr;
    QPushButton *m_recFull = nullptr;
    int m_packChoice = texpack::kPackFull;

    QTemporaryDir *m_tmp = nullptr;
    QString m_dumpPath;
    QString m_isoPath; // verified image ready for extraction
    bool m_reinstall = false;
    bool m_verified = false;
    bool m_installed = false;
    bool m_inAfsPhase = false; // retry re-runs the AFS phase only
    bool m_wantTexPack = false; // user pressed Next on the recommendation page

    QThread *m_thread = nullptr;
    ExtractWorker *m_worker = nullptr;
    QThread *m_afsThread = nullptr;
    AfsExtractWorker *m_afsWorker = nullptr;
};