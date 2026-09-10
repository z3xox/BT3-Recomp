#include "app_paths.h"
#include "dbz_theme.h"
#include "launcher_window.h"

#include <QApplication>
#include <QGuiApplication>
#include <QScreen>

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
    // macOS can restore a stale window position from a disconnected monitor.
    // Always bring the launcher back onto the current primary display.
    if (QScreen *screen = QGuiApplication::primaryScreen())
    {
        const QRect area = screen->availableGeometry();
        win.move(area.center() - QPoint(win.width() / 2, win.height() / 2));
    }
    win.raise();
    win.activateWindow();
    return app.exec();
}
