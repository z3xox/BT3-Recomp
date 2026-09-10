#include "app_paths.h"
#include "dbz_theme.h"
#include "launcher_window.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("bt3-launcher"));

    for (const char *dir : {"data", "savedata", "logs", "textures", "mods"})
        QDir().mkpath(apppaths::userRoot() + "/" + QString::fromLatin1(dir));

    QApplication::setStyle(QStringLiteral("Fusion"));
    app.setStyleSheet(dbz::stylesheet());

    const QString font = dbz::loadHudFont();
    if (!font.isEmpty())
    {
        QFont f(font, 11);
        app.setFont(f);
    }

    LauncherWindow win;
    win.show();
    return app.exec();
}