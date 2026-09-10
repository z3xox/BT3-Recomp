#include "app_paths.h"
#include "install_wizard_dialog.h"

#include "afs_extract_worker.h"
#include "extract_worker.h"
#include "iso9660.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QVBoxLayout>

#include <utility>

namespace
{
constexpr qint64 kMiB = 1024 * 1024;

QString fmtMb(qint64 bytes)
{
    return QString::number(bytes / kMiB);
}

QString pickUnpacker()
{
    static const char *kTools[] = {"7zz", "7z", "unrar", "bsdtar", "tar"};
    for (const char *t : kTools)
    {
        const QString tool = QLatin1String(t);
        if (!QStandardPaths::findExecutable(tool).isEmpty())
            return tool;
    }
    return QString();
}

bool runUnpacker(const QString &tool, const QString &archive, const QString &dest, QString *err)
{
    QStringList args;
    if (tool == QLatin1String("7zz") || tool == QLatin1String("7z"))
        args = {QStringLiteral("x"), QStringLiteral("-y"), QStringLiteral("-o") + dest, archive};
    else if (tool == QLatin1String("unrar"))
        args = {QStringLiteral("x"), QStringLiteral("-y"), archive, dest + QLatin1Char('/')};
    else if (tool == QLatin1String("bsdtar") || tool == QLatin1String("tar"))
        args = {QStringLiteral("-xf"), archive, QStringLiteral("-C"), dest};
    else
    {
        *err = QStringLiteral("The extraction failed. Please extract the ISO manually.");
        return false;
    }

    QProcess p;
    p.start(tool, args);
    while (p.state() != QProcess::NotRunning)
    {
        if (!p.waitForFinished(50))
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0)
    {
        *err = QStringLiteral("The extraction failed. Please extract the ISO manually.");
        return false;
    }
    return true;
}

QString findImageRecursive(const QString &root, int depth)
{
    if (depth > 3)
        return QString();
    QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        it.next();
        const QString n = it.fileName().toLower();
        if (n.endsWith(QLatin1String(".iso")) || n.endsWith(QLatin1String(".img"))
            || n.endsWith(QLatin1String(".bin")))
            return it.filePath();
    }
    return QString();
}

// Any PZS3US*.AFS left next to the extracted game data (their parent folder is
// the deploy dir the runtime reads from).
QStringList findAfsContainers(const QString &dataDir)
{
    QStringList out;
    QDirIterator it(dataDir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        it.next();
        const QString n = it.fileName().toUpper();
        if (n.startsWith(QLatin1String("PZS3US")) && n.endsWith(QLatin1String(".AFS")))
            out << it.filePath();
    }
    out.sort();
    return out;
}

} // namespace

InstallWizardDialog::InstallWizardDialog(QWidget *parent, bool reinstall)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Install Wizard"));
    resize(520, 330);
    setModal(true);
    buildUi();
    if (reinstall)
        setIndex(1); // straight to the disc dump selection
}

InstallWizardDialog::~InstallWizardDialog()
{
    delete m_tmp;
}

void InstallWizardDialog::buildUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    m_stack = new QStackedWidget(this);
    mainLayout->addWidget(m_stack);

    // --- Page A: missing / corrupt data -------------------------------------
    auto *pageA = new QWidget;
    {
        auto *l = new QVBoxLayout(pageA);
        l->setContentsMargins(28, 24, 28, 24);
        auto *head = new QLabel(QStringLiteral("Game Data file are missing or corrupted"), pageA);
        head->setWordWrap(true);
        head->setStyleSheet(QStringLiteral("font-size: 17px; font-weight: 600; color: #ffd9a0;"));
        l->addWidget(head);
        l->addSpacing(8);
        auto *body = new QLabel(QStringLiteral("The game will need to reinstall game files."), pageA);
        body->setWordWrap(true);
        body->setStyleSheet(QStringLiteral("font-size: 13px; color: #c9ccd4;"));
        l->addWidget(body);
        l->addStretch(1);

        auto *row = new QHBoxLayout;
        row->addStretch(1);
        m_nextMissing = new QPushButton(QStringLiteral("Next"), pageA);
        m_nextMissing->setObjectName(QStringLiteral("wizardButton"));
        m_nextMissing->setCursor(Qt::PointingHandCursor);
        connect(m_nextMissing, &QPushButton::clicked, this, &InstallWizardDialog::onNextMissing);
        row->addWidget(m_nextMissing);
        l->addLayout(row);
    }
    m_stack->addWidget(pageA);

    // --- Page B: locate + validate the disc dump -----------------------------
    auto *pageB = new QWidget;
    {
        auto *l = new QVBoxLayout(pageB);
        l->setContentsMargins(28, 24, 28, 24);
        auto *head = new QLabel(QStringLiteral("Install game data"), pageB);
        head->setStyleSheet(QStringLiteral("font-size: 17px; font-weight: 600; color: #ffd9a0;"));
        l->addWidget(head);
        l->addSpacing(8);

        auto *prompt = new QLabel(
            QStringLiteral("Please introduce the directory to your own game disc dump to install:"), pageB);
        prompt->setWordWrap(true);
        prompt->setStyleSheet(QStringLiteral("font-size: 13px; color: #c9ccd4;"));
        l->addWidget(prompt);
        l->addSpacing(10);

        auto *row = new QHBoxLayout;
        m_selected = new QLabel(QStringLiteral("No file selected"), pageB);
        m_selected->setWordWrap(true);
        m_selected->setStyleSheet(QStringLiteral("font-size: 12px; color: #8b93a3;"));
        row->addWidget(m_selected, 1);
        m_browse = new QPushButton(QStringLiteral("Browse…"), pageB);
        m_browse->setObjectName(QStringLiteral("wizardButton"));
        m_browse->setCursor(Qt::PointingHandCursor);
        connect(m_browse, &QPushButton::clicked, this, &InstallWizardDialog::onBrowse);
        row->addWidget(m_browse);
        l->addLayout(row);
        l->addSpacing(12);

        auto *dots = new QHBoxLayout;
        m_dot = new QLabel(QStringLiteral("●"), pageB);
        m_dot->setStyleSheet(QStringLiteral("font-size: 15px; color: #8b93a3;"));
        dots->addWidget(m_dot);
        m_dotText = new QLabel(QStringLiteral("Awaiting a disc dump…"), pageB);
        m_dotText->setWordWrap(true);
        m_dotText->setStyleSheet(QStringLiteral("font-size: 13px; color: #8b93a3;"));
        dots->addWidget(m_dotText, 1);
        l->addLayout(dots);
        l->addStretch(1);

        auto *rowB = new QHBoxLayout;
        rowB->addStretch(1);
        m_retryDump = new QPushButton(QStringLiteral("Retry"), pageB);
        m_retryDump->setObjectName(QStringLiteral("wizardButton"));
        m_retryDump->setCursor(Qt::PointingHandCursor);
        m_retryDump->setVisible(false);
        connect(m_retryDump, &QPushButton::clicked, this, &InstallWizardDialog::onRetryDump);
        rowB->addWidget(m_retryDump);
        rowB->addSpacing(8);
        m_nextDump = new QPushButton(QStringLiteral("Next"), pageB);
        m_nextDump->setObjectName(QStringLiteral("wizardButton"));
        m_nextDump->setCursor(Qt::PointingHandCursor);
        m_nextDump->setEnabled(false);
        connect(m_nextDump, &QPushButton::clicked, this, &InstallWizardDialog::onInstall);
        rowB->addWidget(m_nextDump);
        l->addLayout(rowB);
    }
    m_stack->addWidget(pageB);

    // --- Page C: installation progress ---------------------------------------
    auto *pageC = new QWidget;
    {
        auto *l = new QVBoxLayout(pageC);
        l->setContentsMargins(28, 24, 28, 24);
        auto *head = new QLabel(QStringLiteral("Installation in progress"), pageC);
        head->setStyleSheet(QStringLiteral("font-size: 17px; font-weight: 600; color: #ffd9a0;"));
        l->addWidget(head);
        l->addSpacing(12);

        m_progressText = new QLabel(QStringLiteral("0 MB / 0 MB"), pageC);
        m_progressText->setStyleSheet(QStringLiteral("font-size: 13px; color: #c9ccd4;"));
        l->addWidget(m_progressText);

        m_bar = new QProgressBar(pageC);
        m_bar->setRange(0, 1);
        m_bar->setValue(0);
        m_bar->setTextVisible(false);
        m_bar->setStyleSheet(QStringLiteral(
            "QProgressBar { background:#1b222b; border:1px solid #2a3542; border-radius:4px; height:14px; }"
            "QProgressBar::chunk { background:#ff9e1a; border-radius:4px; }"));
        l->addWidget(m_bar);
        l->addSpacing(6);

        m_activity = new QLabel(QString(), pageC);
        m_activity->setWordWrap(true);
        m_activity->setStyleSheet(QStringLiteral("font-size: 12px; color: #8b93a3;"));
        l->addWidget(m_activity);
        l->addSpacing(6);

        m_doneLabel = new QLabel(QString(), pageC);
        m_doneLabel->setWordWrap(true);
        m_doneLabel->setStyleSheet(QStringLiteral("font-size: 13px; color: #c9ccd4;"));
        l->addWidget(m_doneLabel);
        l->addStretch(1);

        auto *rowC = new QHBoxLayout;
        rowC->addStretch(1);
        m_retryInstall = new QPushButton(QStringLiteral("Retry"), pageC);
        m_retryInstall->setObjectName(QStringLiteral("wizardButton"));
        m_retryInstall->setCursor(Qt::PointingHandCursor);
        m_retryInstall->setVisible(false);
        connect(m_retryInstall, &QPushButton::clicked, this, &InstallWizardDialog::onRetryInstall);
        rowC->addWidget(m_retryInstall);
        rowC->addSpacing(8);
        m_close = new QPushButton(QStringLiteral("Close"), pageC);
        m_close->setObjectName(QStringLiteral("wizardButton"));
        m_close->setCursor(Qt::PointingHandCursor);
        connect(m_close, &QPushButton::clicked, this, [this] {
            if (m_installed)
                accept();
            else
                reject();
        });
        rowC->addWidget(m_close);
        l->addLayout(rowC);
    }
    m_stack->addWidget(pageC);
}

void InstallWizardDialog::setIndex(int index)
{
    if (index == 1)
        m_retryDump->setVisible(false);
    m_stack->setCurrentIndex(index);
}

void InstallWizardDialog::onNextMissing()
{
    setIndex(1);
}

void InstallWizardDialog::onBrowse()
{
    static const QString kFilter =
        QStringLiteral("Game disc dump (*.iso *.img *.rar *.7z *.zip *.tar *.tar.gz);;All files (*)");
    // Qt's own dialog (not the native GTK/portal one) so the app's DBZ theme
    // applies: lighter slate background + white text, readable over dark desks.
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Select your own game disc dump"), QDir::homePath(), kFilter,
        nullptr, QFileDialog::DontUseNativeDialog);
    if (path.isEmpty())
        return;
    m_selected->setText(QDir::toNativeSeparators(path));
    attemptVerify(path);
}

void InstallWizardDialog::onRetryDump()
{
    attemptVerify(m_dumpPath);
}

void InstallWizardDialog::setVerified(bool ok, const QString &text)
{
    m_verified = ok;
    m_dot->setStyleSheet(QStringLiteral("font-size: 15px; color: %1;")
                             .arg(ok ? QStringLiteral("#22c55e") : QStringLiteral("#ef4444")));
    m_dotText->setStyleSheet(QStringLiteral("font-size: 13px; color: %1;")
                                 .arg(ok ? QStringLiteral("#22c55e") : QStringLiteral("#ef4444")));
    m_dotText->setText(text);
    m_nextDump->setEnabled(ok);
}

void InstallWizardDialog::attemptVerify(const QString &dumpPath)
{
    m_dumpPath = dumpPath;
    m_retryDump->setVisible(false);
    m_dot->setStyleSheet(QStringLiteral("font-size: 15px; color: #8b93a3;"));
    m_dotText->setStyleSheet(QStringLiteral("font-size: 13px; color: #c9ccd4;"));
    m_dotText->setText(QStringLiteral("Checking disc dump…"));
    QCoreApplication::processEvents();

    const QString lower = dumpPath.toLower();
    const bool isImage = lower.endsWith(QLatin1String(".iso")) || lower.endsWith(QLatin1String(".img"));

    if (!isImage)
    {
        if (!m_tmp)
            m_tmp = new QTemporaryDir;
        QString err;
        m_isoPath = resolveInnerImage(dumpPath, &err);
        if (m_isoPath.isEmpty())
        {
            setVerified(false, err);
            m_retryDump->setVisible(true);
            return;
        }
    }
    else
    {
        m_isoPath = dumpPath;
    }

    if (DiscVerify::verifySlusFromIso(m_isoPath))
        setVerified(true, QStringLiteral("Game Disc Validated"));
    else
        setVerified(false, QStringLiteral("Cannot verify game disc. Is it the right version?"));
}

QString InstallWizardDialog::resolveInnerImage(const QString &dumpPath, QString *err)
{
    const QString tool = pickUnpacker();
    if (tool.isEmpty())
    {
        *err = QStringLiteral("The extraction failed. Please extract the ISO manually.");
        return QString();
    }

    const QString unpackDir = m_tmp->path() + QStringLiteral("/unpack");
    if (!QDir().mkpath(unpackDir))
    {
        *err = QStringLiteral("The extraction failed. Please extract the ISO manually.");
        return QString();
    }

    if (!runUnpacker(tool, dumpPath, unpackDir, err))
        return QString();

    const QString image = findImageRecursive(unpackDir, 3);
    if (image.isEmpty())
        *err = QStringLiteral("The extraction failed. Please extract the ISO manually.");
    return image;
}

void InstallWizardDialog::onInstall()
{
    if (!m_verified || m_isoPath.isEmpty())
        return;
    startExtraction();
}

void InstallWizardDialog::onRetryInstall()
{
    if (m_inAfsPhase)
    {
        startAfsConversion();
        return;
    }
    if (m_isoPath.isEmpty())
    {
        setIndex(1);
        return;
    }
    startExtraction();
}

void InstallWizardDialog::startExtraction()
{
    m_bar->setRange(0, 1);
    m_bar->setValue(0);
    m_progressText->setText(QStringLiteral("0 MB / 0 MB"));
    m_activity->clear();
    m_doneLabel->setText(QStringLiteral("Installation in progress…"));
    m_doneLabel->setStyleSheet(QStringLiteral("font-size: 13px; color: #c9ccd4;"));
    m_retryInstall->setVisible(false);
    m_close->setEnabled(false);
    setIndex(2);
    QCoreApplication::processEvents();

    m_thread = new QThread;
    m_worker = new ExtractWorker;
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    connect(m_worker, &ExtractWorker::progress, this, &InstallWizardDialog::onExtractProgress);
    connect(m_worker, &ExtractWorker::done, this, &InstallWizardDialog::onExtractDone);
    connect(m_worker, &ExtractWorker::done, m_thread, &QThread::quit);
    m_thread->start();

    const QString dataDir = apppaths::userRoot() + QStringLiteral("/data");
    QMetaObject::invokeMethod(m_worker, "doWork", Qt::QueuedConnection, Q_ARG(QString, m_isoPath),
                              Q_ARG(QString, dataDir));
}

void InstallWizardDialog::onExtractProgress(qint64 done, qint64 total)
{
    if (total > 0)
        m_bar->setRange(0, static_cast<int>(total / (64 * 1024)));
    m_bar->setValue(static_cast<int>(done / (64 * 1024)));
    m_progressText->setText(
        QStringLiteral("%1 MB / %2 MB").arg(fmtMb(done)).arg(fmtMb(total)));
}

void InstallWizardDialog::onExtractDone(bool ok, const QString &msg)
{
    if (ok)
    {
        const QString dataDir = apppaths::userRoot() + QStringLiteral("/data");
        if (!findAfsContainers(dataDir).isEmpty())
        {
            startAfsConversion();
            return;
        }
        applyInstallResult(true, QStringLiteral("Installation complete. Game disc validated."));
        return;
    }
    applyInstallResult(false, msg);
}

void InstallWizardDialog::startAfsConversion()
{
    const QString dataDir = apppaths::userRoot() + QStringLiteral("/data");
    const QStringList afs = findAfsContainers(dataDir);
    if (afs.isEmpty())
    {
        applyInstallResult(true, QStringLiteral("Installation complete. Game disc validated."));
        return;
    }

    m_inAfsPhase = true;
    m_retryInstall->setVisible(false);
    m_close->setEnabled(false);
    m_bar->setRange(0, 1);
    m_bar->setValue(0);
    m_progressText->setText(QStringLiteral("0 MB / 0 MB"));
    m_doneLabel->setText(QStringLiteral("Converting game data to folders…"));
    m_doneLabel->setStyleSheet(QStringLiteral("font-size: 13px; color: #c9ccd4;"));
    m_activity->setText(QStringLiteral("Preparing…"));
    QCoreApplication::processEvents();

    m_afsThread = new QThread;
    m_afsWorker = new AfsExtractWorker;
    m_afsWorker->moveToThread(m_afsThread);
    connect(m_afsThread, &QThread::finished, m_afsWorker, &QObject::deleteLater);
    connect(m_afsThread, &QThread::finished, m_afsThread, &QObject::deleteLater);
    connect(m_afsWorker, &AfsExtractWorker::status, this, &InstallWizardDialog::onAfsStatus);
    connect(m_afsWorker, &AfsExtractWorker::progress, this, &InstallWizardDialog::onAfsProgress);
    connect(m_afsWorker, &AfsExtractWorker::done, this, &InstallWizardDialog::onAfsDone);
    connect(m_afsWorker, &AfsExtractWorker::done, m_afsThread, &QThread::quit);
    m_afsThread->start();

    QMetaObject::invokeMethod(m_afsWorker, "doWork", Qt::QueuedConnection, Q_ARG(QStringList, afs));
}

void InstallWizardDialog::onAfsStatus(const QString &text)
{
    m_activity->setText(text);
}

void InstallWizardDialog::onAfsProgress(qint64 done, qint64 total)
{
    if (total > 0)
        m_bar->setRange(0, static_cast<int>(total / (64 * 1024)));
    m_bar->setValue(static_cast<int>(done / (64 * 1024)));
    m_progressText->setText(
        QStringLiteral("%1 MB / %2 MB").arg(fmtMb(done)).arg(fmtMb(total)));
}

void InstallWizardDialog::onAfsDone(bool ok, const QString &msg)
{
    m_inAfsPhase = false;
    m_activity->clear();
    if (ok)
        applyInstallResult(true, QStringLiteral("Installation complete. Game data converted to folders."));
    else
        applyInstallResult(false, msg);
}

void InstallWizardDialog::applyInstallResult(bool ok, const QString &msg)
{
    if (ok)
    {
        m_installed = true;
        m_doneLabel->setText(msg);
        m_doneLabel->setStyleSheet(QStringLiteral("font-size: 13px; color: #22c55e;"));
        m_bar->setValue(m_bar->maximum());
    }
    else
    {
        m_doneLabel->setText(msg.isEmpty() ? QStringLiteral("Installation failed.") : msg);
        m_doneLabel->setStyleSheet(QStringLiteral("font-size: 13px; color: #ef4444;"));
        m_retryInstall->setVisible(true);
    }
    m_close->setEnabled(true);
}