#include "extract_worker.h"

#include "iso9660.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

void ExtractWorker::doWork(const QString &isoPath, const QString &dataDir)
{
    QDir d(dataDir);
    if (d.exists())
        d.removeRecursively();
    if (!QDir().mkpath(dataDir))
    {
        emit done(false, QStringLiteral("Failed to create the game data folder."));
        return;
    }

    Iso9660 iso;
    if (!iso.open(isoPath))
    {
        emit done(false, iso.error());
        return;
    }

    quint64 total = 0;
    const QList<Iso9660::File> &files = iso.files();
    for (const Iso9660::File &f : files)
    {
        if (!f.dir)
            total += f.size();
    }

    quint64 doneBytes = 0;
    for (const Iso9660::File &f : files)
    {
        if (f.dir)
            continue;
        const QString dest = dataDir + QLatin1Char('/') + f.path;
        const QFileInfo fi(dest);
        if (!QDir().mkpath(fi.absolutePath()))
        {
            emit done(false, QStringLiteral("Failed to create %1").arg(fi.absolutePath()));
            return;
        }
        QFile out(dest);
        if (!out.open(QIODevice::WriteOnly))
        {
            emit done(false, QStringLiteral("Failed to write %1").arg(dest));
            return;
        }
        const qint64 got = iso.readFile(f, [&](const QByteArray &chunk) {
            out.write(chunk);
            doneBytes += static_cast<quint64>(chunk.size());
            emit progress(static_cast<qint64>(doneBytes), static_cast<qint64>(total));
        });
        if (got < 0)
        {
            emit done(false, QStringLiteral("Failed to read %1 from the disc image").arg(f.path));
            return;
        }
    }

    // BIN/DBZP.BIN is opened read-write by the game; ISO extraction yields
    // read-only files, so lift the write bits (mirrors setup.py make_writable).
    // On Windows the writable attribute is default and the POSIX mask is moot.
    const QString dbzp = QDir(dataDir).filePath(QStringLiteral("BIN/DBZP.BIN"));
    if (QFile::exists(dbzp))
    {
#ifndef _WIN32
        QFile::setPermissions(dbzp, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                         | QFileDevice::ReadGroup | QFileDevice::WriteGroup
                                         | QFileDevice::ReadOther | QFileDevice::WriteOther);
#endif
    }

    emit done(true, QString());
}