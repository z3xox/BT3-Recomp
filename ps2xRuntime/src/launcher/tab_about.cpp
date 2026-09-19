#include "tab_about.h"

#include "dbz_theme.h"

#include <QFont>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

namespace
{
    QLabel *richLabel(const QString &html)
    {
        auto *l = new QLabel(html);
        l->setWordWrap(true);
        l->setTextFormat(Qt::RichText);
        l->setOpenExternalLinks(true);
        return l;
    }
}

AboutTab::AboutTab(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("Dragon Ball Z: Budokai Tenkaichi 3 — Recompiled"));
    {
        QFont f = title->font();
        f.setPointSizeF(f.pointSizeF() + 4.0);
        f.setBold(true);
        title->setFont(f);
        title->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(dbz::kAccent)));
    }
    root->addWidget(title);

    root->addWidget(richLabel(QStringLiteral(
        "A statically recompiled, native PC port of <i>Dragon Ball Z: Budokai Tenkaichi 3</i> "
        "(PS2, USA, SLUS-21678), built on "
        "<a href=\"https://github.com/ran-j/PS2Recomp\">PS2Recomp</a>. The game's MIPS code is "
        "translated to C++ at build time from your own disc image — this repository ships no "
        "game code, assets or media.")));

    auto *credits = new QGroupBox(QStringLiteral("Credits"));
    auto *creditsLay = new QVBoxLayout(credits);
    creditsLay->addWidget(richLabel(QStringLiteral(
        "<b>z3xox</b> — owner / lead developer<br>"
        "&nbsp;&nbsp;&nbsp;recompiler, runtime (EE/GS/VU1/scheduler), renderer, game overrides, generators")));
    creditsLay->addWidget(richLabel(QStringLiteral(
        "<b>RexxColder</b> — support / collaborator<br>"
        "&nbsp;&nbsp;&nbsp;optimization (perf/async, batching), launcher + install wizard + ISO9660, "
        "input &amp; gamepads, build/release (floor gate, packaging), deploy, "
        "game-data (AFS/AFL), docs")));
    creditsLay->addWidget(richLabel(QStringLiteral(
        "<b>valenvivaldi</b> — collaborator<br>"
        "&nbsp;&nbsp;&nbsp;port macOS arm64, packaging, audio")));
    root->addWidget(credits);

    auto *third = new QGroupBox(QStringLiteral("Third-party"));
    auto *thirdLay = new QVBoxLayout(third);
    thirdLay->addWidget(richLabel(QStringLiteral(
        "<a href=\"https://github.com/ran-j/PS2Recomp\">ran-j/PS2Recomp</a> — static recompiler (upstream, GPL-3.0)<br>"
        "ViveTheModder — NTSC-U AFS file lists (Apache-2.0)<br>"
        "<a href=\"https://github.com/Arntzen-Software/parallel-gs\">Arntzen Software</a> — paraLLEl-GS (LGPL-3.0-or-later)")));
    root->addWidget(third);

    root->addWidget(richLabel(QStringLiteral(
        "Licensed GPL-3.0. Not affiliated with Spike or Bandai Namco, and not endorsed by them. "
        "<a href=\"https://github.com/z3xox/BT3-Recomp\">github.com/z3xox/BT3-Recomp</a>")));

    root->addStretch(1);
}
