#pragma once
// [texui] Shared texture-pack constants/helpers for the launcher. The pack lives in
// <deploy>/data/Textures. The launcher no longer downloads the archive: it opens the
// pixeldrain page (with a clipboard fallback if the browser won't open) and installs a
// locally downloaded file via Browse.

#include <QString>

namespace texpack
{
    // Pack variants. kPackLite = 2D textures only; kPackFull = 3D + 2D.
    enum PackKind { kPackLite = 0, kPackFull = 1 };

    // User-facing name, download page and suggested archive name for each variant.
    const char *packName(int kind);      // "4K 2D Textures Lite" / "4k Texture Pack"
    const char *packUrl(int kind);       // https://pixeldrain.com/u/...
    const char *packFileName(int kind);  // suggested archive file name

    // <deploy>/data/Textures
    QString dir();

    // Lowercase hex sha256 of a file, or empty on error.
    QString sha256File(const QString &path);

    // Count replacement files (.png/.dds) under dir(), recursively.
    quint64 countReplacements();

    // True when the installed pack looks like the full one (3D characters present).
    bool installedIsFull();
} // namespace texpack
