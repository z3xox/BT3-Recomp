#include "tex_install_dialog.h"

#include "app_paths.h"
#include "settings_manager.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

TexInstallDialog::TexInstallDialog(QWidget *parent, int pack)
    : QDialog(parent)
    , m_pack(pack)
{
    setWindowTitle(QStringLiteral("Install texture pack"));
    setMinimumWidth(480);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(8);

    auto *title = new QLabel(QString::fromLatin1(texpack::packName(m_pack)));
    title->setObjectName(QStringLiteral("sectionLabel"));
    root->addWidget(title);

    m_status = new QLabel(QStringLiteral(
        "The launcher no longer downloads the pack.\n"
        "1. Open the download page in your browser and save the archive.\n"
        "2. Click Browse… and select the downloaded archive to install it."));
    m_status->setObjectName(QStringLiteral("hintLabel"));
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    // Link row: a selectable read-only URL + Copy (clipboard fallback if the browser
    // will not open for any reason).
    {
        auto *row = new QHBoxLayout;
        auto *url = new QLineEdit(QString::fromLatin1(texpack::packUrl(m_pack)));
        url->setReadOnly(true);
        url->setCursorPosition(0);
        m_copy = new QPushButton(QStringLiteral("Copy link"));
        m_copy->setObjectName(QStringLiteral("wizardButton"));
        m_copy->setCursor(Qt::PointingHandCursor);
        row->addWidget(url, 1);
        row->addWidget(m_copy);
        root->addLayout(row);
    }

    m_exBar = new QProgressBar;
    m_exBar->setRange(0, 100);
    m_exBar->setValue(0);
    m_exBar->setVisible(false);
    root->addWidget(m_exBar);

    auto *row = new QHBoxLayout;
    m_browse = new QPushButton(QStringLiteral("Browse…"));
    m_browse->setObjectName(QStringLiteral("wizardButton"));
    m_browse->setCursor(Qt::PointingHandCursor);
    m_openWeb = new QPushButton(QStringLiteral("Open in browser"));
    m_openWeb->setObjectName(QStringLiteral("wizardButton"));
    m_openWeb->setCursor(Qt::PointingHandCursor);
    m_close = new QPushButton(QStringLiteral("Close"));
    m_close->setObjectName(QStringLiteral("wizardButton"));
    m_close->setCursor(Qt::PointingHandCursor);
    row->addWidget(m_browse);
    row->addWidget(m_openWeb);
    row->addStretch(1);
    row->addWidget(m_close);
    root->addLayout(row);

    connect(m_browse, &QPushButton::clicked, this, &TexInstallDialog::onBrowse);
    connect(m_openWeb, &QPushButton::clicked, this, &TexInstallDialog::onOpenBrowser);
    connect(m_copy, &QPushButton::clicked, this, &TexInstallDialog::onCopyLink);
    connect(m_close, &QPushButton::clicked, this, &QDialog::close);
}

TexInstallDialog::~TexInstallDialog()
{
    abortExtract();
}

void TexInstallDialog::setStatus(const QString &text)
{
    m_status->setText(text);
}

void TexInstallDialog::fail(const QString &text)
{
    setStatus(text);
    QMessageBox::warning(this, QStringLiteral("Install texture pack"), text);
    m_browse->setEnabled(true);
    m_openWeb->setEnabled(true);
}

// ---------------------------------------------------------------------------
// Link helpers
// ---------------------------------------------------------------------------

void TexInstallDialog::onOpenBrowser()
{
    QDesktopServices::openUrl(QUrl(QString::fromLatin1(texpack::packUrl(m_pack))));
    setStatus(QStringLiteral("Opened the download page. Save %1, then click Browse… to install it.")
                  .arg(QString::fromLatin1(texpack::packFileName(m_pack))));
}

void TexInstallDialog::onCopyLink()
{
    QApplication::clipboard()->setText(QString::fromLatin1(texpack::packUrl(m_pack)));
    setStatus(QStringLiteral("Link copied to the clipboard: %1")
                  .arg(QString::fromLatin1(texpack::packUrl(m_pack))));
}

// ---------------------------------------------------------------------------
// Browse + extract (the only install path now)
// ---------------------------------------------------------------------------

void TexInstallDialog::onBrowse()
{
    const QString file = QFileDialog::getOpenFileName(
        this, QStringLiteral("Select the downloaded texture pack"), QDir::homePath(),
        QStringLiteral("Texture packs (*.7z *.zip *.rar *.tar *.tar.gz);;All files (*)"));
    if (file.isEmpty())
        return;
    beginExtract(file);
}

void TexInstallDialog::beginExtract(const QString &archivePath)
{
    m_dest = texpack::dir();
    QDir().mkpath(m_dest);

    m_browse->setEnabled(false);
    m_openWeb->setEnabled(false);
    m_exBar->setVisible(true);
    m_exBar->setRange(0, 0);   // busy until the worker reports the total
    m_exBar->setValue(0);
    setStatus(QStringLiteral("Extracting to %1…").arg(m_dest));

    // libarchive runs in-process on a worker thread so the dialog stays live.
    m_extThread = new QThread;
    m_worker = new ArchiveExtractWorker;
    m_worker->moveToThread(m_extThread);
    connect(m_extThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_extThread, &QThread::finished, m_extThread, &QObject::deleteLater);
    connect(m_worker, &ArchiveExtractWorker::progress, this, &TexInstallDialog::onExtractProgress);
    connect(m_worker, &ArchiveExtractWorker::done, this, &TexInstallDialog::onExtractDone);
    connect(m_worker, &ArchiveExtractWorker::done, m_extThread, &QThread::quit);
    m_extThread->start();

    QMetaObject::invokeMethod(m_worker, "doWork", Qt::QueuedConnection,
                              Q_ARG(QString, archivePath), Q_ARG(QString, m_dest));
}

void TexInstallDialog::onExtractProgress(qint64 done, qint64 total)
{
    if (total <= 0)
        return;
    if (m_exBar->maximum() != 100)
        m_exBar->setRange(0, 100);
    m_exBar->setValue(static_cast<int>(done * 100 / total));
}

void TexInstallDialog::onExtractDone(bool ok, const QString &msg)
{
    // The thread and worker self-delete via finished() -> deleteLater().
    m_worker = nullptr;
    m_extThread = nullptr;

    if (ok)
    {
        m_ok = true;
        m_exBar->setValue(100);
        setStatus(QStringLiteral("Installed."));
        // The pack is now indexable: switch Texture Replacement on and persist it so
        // the next run actually uses it (shared [video] texture_pack key).
        SettingsManager::instance().setTexPack(true);
        SettingsManager::instance().save();
        emit installed();
    }
    else if (!m_aborting)
    {
        fail(msg.isEmpty() ? QStringLiteral("Extraction failed.") : msg);
    }
    m_browse->setEnabled(true);
    m_openWeb->setEnabled(true);
}

// ---------------------------------------------------------------------------
// Cancellation
// ---------------------------------------------------------------------------

void TexInstallDialog::abortExtract()
{
    if (!m_worker && !m_extThread)
        return;
    m_aborting = true;
    if (m_worker)
        m_worker->requestCancel();
    if (m_extThread)
    {
        m_extThread->quit();
        m_extThread->wait(5000);
    }
    m_worker = nullptr;
    m_extThread = nullptr;
}

void TexInstallDialog::closeEvent(QCloseEvent *e)
{
    abortExtract();
    e->accept();
}
