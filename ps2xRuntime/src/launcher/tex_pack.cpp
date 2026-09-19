#include "tex_pack.h"

#include "app_paths.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

namespace texpack
{
namespace
{
const char *const kLiteUrl  = "https://pixeldrain.com/u/sobTVFCk";
const char *const kFullUrl  = "https://pixeldrain.com/u/PYJZe4Jd";
const char *const kLiteName = "4K 2D Textures Lite";
const char *const kFullName = "4k Texture Pack";
const char *const kLiteFile = "4K 2D Textures Lite.7z";
const char *const kFullFile = "4k Texture Pack.7z";
} // namespace

const char *packName(int kind)     { return kind == kPackLite ? kLiteName : kFullName; }
const char *packUrl(int kind)      { return kind == kPackLite ? kLiteUrl : kFullUrl; }
const char *packFileName(int kind) { return kind == kPackLite ? kLiteFile : kFullFile; }

QString dir()
{
    return apppaths::userRoot() + QStringLiteral("/data/Textures");
}

QString sha256File(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f))
        return QString();
    return QString::fromLatin1(h.result().toHex());
}

quint64 countReplacements()
{
    const QString d = dir();
    if (!QDir(d).exists())
        return 0;
    quint64 n = 0;
    QDirIterator it(d, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString e = QFileInfo(it.next()).suffix().toLower();
        if (e == QLatin1String("png") || e == QLatin1String("dds"))
            ++n;
    }
    return n;
}

bool installedIsFull()
{
    return QDir(dir() + QStringLiteral("/replacements/Characters/Body")).exists();
}
} // namespace texpack
