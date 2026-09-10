#include "app_paths.h"
#include "dbz_theme.h"

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QDir>

namespace dbz
{
    namespace
    {
        QString &families()
        {
            static QString fam;
            return fam;
        }
    } // namespace

    QString hudFontFamily()
    {
        return families();
    }

    QString stylesheet()
    {
        return QString(R"QSS(
        QWidget { background-color: %1; color: %2; }
        QMainWindow { background-color: %1; }

        QDialog {
            background-color: %1;
            border: 2px solid %5;
            border-radius: 6px;
        }
        QDialog QDialogButtonBox { background: transparent; }

        QPushButton {
            background-color: %2;
            border: 1px solid %3;
            border-radius: 2px;
            color: %4;
            padding: 6px 14px;
            font-weight: bold;
        }
        QPushButton:hover { background-color: rgba(255,158,26,0.18); }
        QPushButton:pressed { background-color: rgba(255,158,26,0.30); color: %5; }
        QPushButton:disabled { background-color: rgba(18,24,32,0.4); border-color: %6; color: %6; }

        QPushButton#playButton {
            background-color: %5;
            border: none;
            color: #1a1208;
            font-size: 20px;
            padding: 14px 40px;
        }
        QPushButton#playButton:hover { background-color: %7; }
        QPushButton#playButton:pressed { background-color: #e08a10; color: #1a1208; }

        QPushButton#settingsButton {
            background-color: transparent;
            border: 2px solid %5;
            color: %4;
            font-size: 16px;
            padding: 12px 30px;
        }
        QPushButton#settingsButton:hover { background-color: rgba(255,158,26,0.18); }
        QPushButton#settingsButton:pressed { background-color: rgba(255,158,26,0.30); }

        QPushButton#tabSave { background-color: %8; border-color: %8; color: %4; }
        QPushButton#tabSave:hover { background-color: #2f8a4d; border-color: #2f8a4d; }
        QPushButton#tabClose { background-color: %9; border-color: %9; color: %4; }
        QPushButton#tabClose:hover { background-color: #8c281f; border-color: #8c281f; }
        QPushButton#bindButton { background-color: #1a2129; border-color: %5; color: %4; padding: 2px 10px; }
        QPushButton#bindButton:hover { background-color: rgba(255,158,26,0.18); }
        QPushButton#browseBtn { background-color: %2; border-color: %3; padding: 6px 20px; }

        QTabWidget::pane { border: 1px solid %3; border-radius: 2px; top: -1px; }
        QTabBar::tab {
            background-color: transparent;
            border: 1px solid #1e2830;
            border-bottom: none;
            padding: 8px 22px;
            color: %4;
            font-weight: bold;
            border-top-left-radius: 2px;
            border-top-right-radius: 2px;
        }
        QTabBar::tab:selected { background-color: rgba(255,158,26,0.14); border-color: %3; }
        QTabBar::tab:hover:!selected { background-color: rgba(255,158,26,0.20); }

        QLabel { background: transparent; }
        QLabel#sectionLabel { color: %5; font-weight: bold; font-size: 13px; letter-spacing: 1px; }
        QLabel#hintLabel { color: %6; background: transparent; }
        QLabel#valueLabel { color: %4; background: transparent; min-width: 46px; }

        QGroupBox { border: 1px solid #1e2830; border-radius: 2px; margin-top: 14px; padding-top: 4px; }
        QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; color: %5; font-weight: bold; }

        QFrame[frameShape="4"] { color: #1e2830; } /* QFrame::HLine */

        QSlider::groove:horizontal {
            background-color: %2;
            height: 4px;
            border-radius: 2px;
            border: 1px solid #1e2830;
        }
        QSlider::sub-page:horizontal { background-color: %5; border-radius: 2px; }
        QSlider::handle:horizontal {
            background-color: %5;
            width: 14px;
            height: 14px;
            margin: -6px 0;
            border-radius: 7px;
        }
        QSlider::handle:horizontal:hover { background-color: %7; }

        QComboBox {
            background-color: %2;
            border: 1px solid %3;
            border-radius: 2px;
            padding: 4px 8px;
            color: %4;
        }
        QComboBox:hover { border-color: %5; }
        QComboBox::drop-down { border: none; width: 20px; }
        QComboBox::down-arrow {
            border-left: 4px solid transparent;
            border-right: 4px solid transparent;
            border-top: 5px solid %5;
            margin-right: 6px;
        }
        QComboBox QAbstractItemView {
            background-color: %1;
            border: 1px solid %3;
            color: %4;
            selection-background-color: rgba(255,158,26,0.20);
            selection-color: %4;
            outline: none;
        }

        QCheckBox { color: %4; background: transparent; spacing: 8px; }
        QCheckBox::indicator { width: 18px; height: 18px; border: 1px solid %3; border-radius: 2px; background-color: %2; }
        QCheckBox::indicator:checked { background-color: %5; border-color: %5; }
        QCheckBox::indicator:hover { border-color: %5; }

        QTableWidget {
            background-color: %1;
            border: 1px solid %3;
            gridline-color: #1e2830;
            color: %4;
        }
        QTableWidget::item { padding: 3px 6px; }
        QTableWidget::item:selected { background-color: rgba(255,158,26,0.20); color: %4; }
        QHeaderView::section {
            background-color: #141b22;
            border: none;
            border-bottom: 1px solid #1e2830;
            padding: 5px;
            color: %4;
            font-weight: bold;
        }

        QScrollBar:vertical { background-color: transparent; width: 12px; }
        QScrollBar::handle:vertical { background-color: rgba(255,158,26,0.45); border-radius: 2px; min-height: 24px; }
        QScrollBar::handle:vertical:hover { background-color: rgba(255,158,26,0.70); }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar:horizontal { background-color: transparent; height: 12px; }
        QScrollBar::handle:horizontal { background-color: rgba(255,158,26,0.45); border-radius: 2px; }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }

        QToolTip { background-color: %1; color: %4; border: 1px solid %3; }

        /* File picker (Browse… for the disc dump): the default dark window-bg
           made it near-illegible. Use a lighter slate + white text. */
        QFileDialog {
            background-color: #1d2530;
            color: #ffffff;
        }
        QFileDialog QDialogButtonBox { background: transparent; }
        QFileDialog QLabel { background: transparent; color: #ffffff; }
        QFileDialog QListWidget,
        QFileDialog QTreeView,
        QFileDialog QListView,
        QFileDialog QAbstractItemView {
            background-color: #232c38;
            alternate-background-color: #1e2733;
            border: 1px solid %3;
            color: #ffffff;
            outline: none;
        }
        QFileDialog QListWidget::item,
        QFileDialog QTreeView::item,
        QFileDialog QListView::item,
        QFileDialog QAbstractItemView::item {
            color: #ffffff;
            padding: 3px 4px;
        }
        QFileDialog QListWidget::item:selected,
        QFileDialog QTreeView::item:selected,
        QFileDialog QListView::item:selected,
        QFileDialog QAbstractItemView::item:selected {
            background-color: rgba(255,158,26,0.20);
            color: %7;
        }
        QFileDialog QLineEdit {
            background-color: #1b232e;
            border: 1px solid %3;
            color: #ffffff;
            selection-background-color: rgba(255,158,26,0.30);
        }
        QFileDialog QComboBox {
            background-color: #1b232e;
            border: 1px solid %3;
            color: #ffffff;
        }
        )QSS")
            .arg(QString(kWindowBg), QString(kFrameBg), QString(kBorder), QString(kText),
                 QString(kAccent),  QString(kTextDisabled), QString(kGold),
                 QString(kSaveGreen), QString(kCloseRed));
    }

    QString loadHudFont()
    {
        QDir appDir(apppaths::assets());
        const QString fontPath = appDir.filePath("fonts/RussoOne-Regular.ttf");
        if (QFile::exists(fontPath))
        {
            const int id = QFontDatabase::addApplicationFont(fontPath);
            if (id >= 0)
            {
                const QStringList fams = QFontDatabase::applicationFontFamilies(id);
                if (!fams.isEmpty())
                {
                    families() = fams.first();
                    QFont f(fams.first());
                    f.setPointSizeF(f.pointSizeF() > 0.0 ? f.pointSizeF() : 12.0);
                    f.setStyleStrategy(QFont::PreferAntialias);
                    QApplication::setFont(f);
                    return fams.first();
                }
            }
        }
        return QString();
    }
} // namespace dbz