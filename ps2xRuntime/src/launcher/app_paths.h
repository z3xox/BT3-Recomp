#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

namespace apppaths
{
inline QString binaries() { return QCoreApplication::applicationDirPath(); }

inline bool bundled()
{
#if defined(Q_OS_MACOS)
    return QDir(binaries()).dirName() == QStringLiteral("MacOS") &&
           QDir(binaries() + QStringLiteral("/..")).exists(QStringLiteral("Info.plist"));
#else
    return false;
#endif
}

inline QString userRoot()
{
    if (bundled())
        return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
               QStringLiteral("/BT3-Recomp");
    return binaries();
}

inline QString assets()
{
    return bundled() ? QDir(binaries()).absoluteFilePath(QStringLiteral("../Resources/assets"))
                     : binaries() + QStringLiteral("/assets");
}
}
