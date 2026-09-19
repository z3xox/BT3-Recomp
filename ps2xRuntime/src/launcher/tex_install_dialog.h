#pragma once
// [texui] Install dialog: the launcher no longer downloads the archive. It shows the
// hardcoded download page for the chosen variant (Lite/Full), offers "Open in browser"
// and a clipboard copy as a fallback, and installs a locally downloaded archive with
// Browse (verified + extracted into data/Textures).

#include <QDialog>

#include "archive_extract.h"
#include "tex_pack.h"

class QLabel;
class QProgressBar;
class QPushButton;
class QThread;

class TexInstallDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TexInstallDialog(QWidget *parent = nullptr, int pack = texpack::kPackFull);
    ~TexInstallDialog() override;

signals:
    void installed();   // emitted once an extraction finished successfully

protected:
    void closeEvent(QCloseEvent *e) override;

private slots:
    void onBrowse();
    void onOpenBrowser();
    void onCopyLink();
    void onExtractProgress(qint64 done, qint64 total);
    void onExtractDone(bool ok, const QString &msg);

private:
    void setStatus(const QString &text);
    void beginExtract(const QString &archivePath);
    void fail(const QString &text);
    void abortExtract();

    int m_pack = texpack::kPackFull;
    QPushButton *m_browse = nullptr;
    QPushButton *m_openWeb = nullptr;
    QPushButton *m_copy = nullptr;
    QPushButton *m_close = nullptr;
    QLabel *m_status = nullptr;
    QProgressBar *m_exBar = nullptr;

    QThread *m_extThread = nullptr;
    ArchiveExtractWorker *m_worker = nullptr;
    QString m_dest;
    bool m_ok = false;        // set once extraction finished cleanly
    bool m_aborting = false;  // suppress the error box while closing
};
