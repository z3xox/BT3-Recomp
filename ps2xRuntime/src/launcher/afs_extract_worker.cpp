#include "app_paths.h"
#include "afs_extract_worker.h"

#include "afs_archive.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>

void AfsExtractWorker::doWork(const QStringList &afsFiles)
{
    qint64 totalBytes = 0;
    for (const QString &f : afsFiles)
        totalBytes += 2 * QFileInfo(f).size(); // one pass writes the slots, one verifies
    if (totalBytes <= 0)
    {
        emit done(false, QStringLiteral("No AFS containers to convert."));
        return;
    }

    const qint64 tickBytes = qMax<qint64>(256 * 1024, totalBytes / 1000); // ~0.1% UI throttle

    qint64 base = 0;
    for (const QString &f : afsFiles)
    {
        const QFileInfo info(f);
        const qint64 fileTotal = 2 * info.size();
        qint64 last = base;

        emit status(QStringLiteral("Converting %1…").arg(info.fileName()));

        const auto onStatus = [this](const std::string &s) {
            emit status(QString::fromStdString(s));
        };
        const auto onProgress = [this, &last, tickBytes, totalBytes, base](uint64_t d, uint64_t) {
            const qint64 cur = base + static_cast<qint64>(d);
            if (cur - last >= tickBytes)
            {
                last = cur;
                emit progress(cur, totalBytes);
            }
        };

        const AfsConvertResult r = convertAfsToFolder(
            f.toStdString(), info.absolutePath().toStdString(), onStatus, onProgress,
            apppaths::assets().toStdString());

        if (!r.ok)
        {
            emit progress(totalBytes, totalBytes);
            emit done(false, QString::fromStdString(r.error));
            return;
        }

        base += fileTotal;
        emit status(QStringLiteral("Removing %1…").arg(info.fileName()));
        QFile::remove(f);
        emit progress(base, totalBytes);
    }

    emit done(true, QString());
}