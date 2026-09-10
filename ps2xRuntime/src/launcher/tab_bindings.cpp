#include "tab_bindings.h"

#include "app_paths.h"
#include "input_reader.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QKeyEvent>
#include <QFont>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <cstdio>

namespace
{
    // evin button index -> raylib GAMEPAD_BUTTON_* code stored in pad.conf.
    // The runtime polls binds via IsGamepadButtonDown(gp, value), so the
    // launcher must translate its reader indices into raylib codes.
    int evinToGamepadButton(int rb)
    {
        static const int kMap[16] = {
            5,   // 0 BtnY      -> RIGHT_FACE_UP    (Y/Triangle)
            6,   // 1 BtnB      -> RIGHT_FACE_RIGHT (B/Circle)
            7,   // 2 BtnA      -> RIGHT_FACE_DOWN  (A/Cross)
            8,   // 3 BtnX      -> RIGHT_FACE_LEFT  (X/Square)
            10,  // 4 BtnLT     -> LEFT_TRIGGER_2   (LT/L2)
            12,  // 5 BtnRT     -> RIGHT_TRIGGER_2  (RT/R2)
            9,   // 6 BtnLB     -> LEFT_TRIGGER_1   (LB/L1)
            11,  // 7 BtnRB     -> RIGHT_TRIGGER_1  (RB/R1)
            13,  // 8 BtnSelect -> MIDDLE_LEFT      (Back/Select)
            15,  // 9 BtnStart  -> MIDDLE_RIGHT     (Start)
            16,  // 10 BtnLS    -> LEFT_THUMB       (L3)
            17,  // 11 BtnRS    -> RIGHT_THUMB      (R3)
            1,   // 12 DpadUp   -> LEFT_FACE_UP
            3,   // 13 DpadDown -> LEFT_FACE_DOWN
            4,   // 14 DpadLeft -> LEFT_FACE_LEFT
            2,   // 15 DpadRight-> LEFT_FACE_RIGHT
        };
        return (rb >= 0 && rb < 16) ? kMap[rb] : rb;
    }

    // Does this player have at least one key bind / one pad (button/axis) bind?
    // Used to keep the default layout in sync with the assigned device: switching
    // a player from gamepad to keyboard must not leave stale button binds behind,
    // and vice versa.
    bool hasKeyBinds(const padconf::Player &p)
    {
        for (auto &b : p.binds)
            if (b.kind == padconf::BindKind::Key)
                return true;
        return false;
    }

    bool hasPadBinds(const padconf::Player &p)
    {
        for (auto &b : p.binds)
            if (b.kind == padconf::BindKind::Button || b.kind == padconf::BindKind::Axis)
                return true;
        return false;
    }

    // Qt key code -> raylib KEY_* code. The runtime stores raylib codes in
    // pad.conf and polls with IsKeyDown(value), so capture must translate.
    int qtKeyToRaylib(int key)
    {
        static const struct { int qt; int rl; } map[] = {
            {Qt::Key_Escape, 256}, {Qt::Key_Return, 257}, {Qt::Key_Enter, 257},
            {Qt::Key_Tab, 258}, {Qt::Key_Backspace, 259},
            {Qt::Key_Space, 32},
            {Qt::Key_Up, 265}, {Qt::Key_Down, 264}, {Qt::Key_Left, 263}, {Qt::Key_Right, 262},
            {Qt::Key_Control, 341}, {Qt::Key_Shift, 340}, {Qt::Key_Alt, 342}, {Qt::Key_Meta, 343},
            {Qt::Key_1, 49}, {Qt::Key_2, 50}, {Qt::Key_3, 51}, {Qt::Key_4, 52}, {Qt::Key_5, 53},
            {Qt::Key_6, 54}, {Qt::Key_7, 55}, {Qt::Key_8, 56}, {Qt::Key_9, 57}, {Qt::Key_0, 48},
            {Qt::Key_Q, 81}, {Qt::Key_W, 87}, {Qt::Key_E, 69}, {Qt::Key_R, 82}, {Qt::Key_T, 84},
            {Qt::Key_Y, 89}, {Qt::Key_U, 85}, {Qt::Key_I, 73}, {Qt::Key_O, 79}, {Qt::Key_P, 80},
            {Qt::Key_A, 65}, {Qt::Key_S, 83}, {Qt::Key_D, 68}, {Qt::Key_F, 70}, {Qt::Key_G, 71},
            {Qt::Key_H, 72}, {Qt::Key_J, 74}, {Qt::Key_K, 75}, {Qt::Key_L, 76},
            {Qt::Key_Z, 90}, {Qt::Key_X, 88}, {Qt::Key_C, 67}, {Qt::Key_V, 86}, {Qt::Key_B, 66},
            {Qt::Key_N, 78}, {Qt::Key_M, 77},
        };
        for (auto &m : map)
            if (m.qt == key)
                return m.rl;
        return 0;
    }

    // Combo item semantics for per-player device assignment. pad.conf stores
    // None / Keyboard / Gamepad <index>; Auto = None (runtime auto-detect).
    constexpr int kDevAuto = 0;
    constexpr int kDevKeyboard = 1;
    constexpr int kDevGamepadBase = 2;

    // Axis descriptor shared by the capture and the idle (wait-release) check.
    // evin axis indices already match GAMEPAD_AXIS_* codes, so values are
    // stored as-is by the runtime.
    struct AxisCapture
    {
        int axis;
        float need;
    };
    const AxisCapture kAxisCaptures[] = {
        {evin::AxisLX, 0.55f}, {evin::AxisLY, 0.55f}, {evin::AxisRX, 0.55f},
        {evin::AxisRY, 0.55f}, {evin::AxisLT, 0.55f}, {evin::AxisRT, 0.55f},
    };
    const float kAxisIdleThreshold = 0.3f;
} // namespace

BindingsTab::BindingsTab(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(8);

    auto *head = new QWidget;
    auto *headLay = new QHBoxLayout(head);
    headLay->setContentsMargins(0, 0, 0, 0);
    headLay->setSpacing(10);
    auto *pl = new QLabel(QStringLiteral("Player"));
    pl->setObjectName(QStringLiteral("valueLabel"));
    m_player = new QComboBox;
    m_player->addItems({QStringLiteral("P1"), QStringLiteral("P2")});
    headLay->addWidget(pl);
    headLay->addWidget(m_player);
    headLay->addSpacing(18);
    auto *dl = new QLabel(QStringLiteral("Device"));
    dl->setObjectName(QStringLiteral("valueLabel"));
    m_device = new QComboBox;
    headLay->addWidget(dl);
    headLay->addWidget(m_device, 1);
    headLay->addSpacing(10);
    auto *refresh = new QPushButton(QStringLiteral("Refresh"));
    refresh->setObjectName(QStringLiteral("dialogButton"));
    connect(refresh, &QPushButton::clicked, this, &BindingsTab::refreshDevices);
    headLay->addWidget(refresh);
    auto *loadDefaults = new QPushButton(QStringLiteral("Load Defaults"));
    loadDefaults->setObjectName(QStringLiteral("dialogButton"));
    connect(loadDefaults, &QPushButton::clicked, this, &BindingsTab::onLoadDefaults);
    headLay->addWidget(loadDefaults);
    root->addWidget(head);

    m_table = new QTableWidget(24, 3, this);
    m_table->setHorizontalHeaderLabels({QStringLiteral("Action"), QStringLiteral("Current Bind"), QString()});
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setMinimumSectionSize(48);
    m_table->setColumnWidth(2, 72);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setFocusPolicy(Qt::NoFocus);
    for (int row = 0; row < 24; ++row)
    {
        auto *act = new QTableWidgetItem(QString::fromUtf8(padconf::actionName(row)));
        m_table->setItem(row, 0, act);
        auto *cur = new QTableWidgetItem(QStringLiteral("None"));
        m_table->setItem(row, 1, cur);
        auto *btn = new QPushButton(QStringLiteral("Set"));
        btn->setProperty("row", row);
        btn->setMinimumWidth(64);
        QFont f = btn->font();
        f.setPointSizeF(f.pointSizeF() * 0.8f);
        btn->setFont(f);
        m_table->setCellWidget(row, 2, btn);
        connect(btn, &QPushButton::clicked, this, &BindingsTab::onBindClicked);
    }
    root->addWidget(m_table, 1);

    m_captureStatus = new QLabel(QStringLiteral(
        "Click Set, then press a button or move a stick/trigger on the selected device. Esc aborts."));
    m_captureStatus->setObjectName(QStringLiteral("hintLabel"));
    m_captureStatus->setWordWrap(true);
    root->addWidget(m_captureStatus);

    connect(m_player, &QComboBox::currentIndexChanged, this, &BindingsTab::onPlayerChanged);
    connect(m_device, &QComboBox::currentIndexChanged, this, &BindingsTab::onDeviceChanged);
    // Refresh after the table and signal wiring exist: onDeviceChanged() ->
    // refreshRow() touches the table, which must already be built.
    refreshDevices();
    // Open the initial selection.
    openCaptureDevice();

    m_timer = new QTimer(this);
    m_timer->setInterval(16);
    connect(m_timer, &QTimer::timeout, this, &BindingsTab::pollCapture);

    // The keyboard is Qt-managed: a window-level filter feeds captured keys to
    // the reader (instead of a /dev/input node).
    qApp->installEventFilter(this);
}

BindingsTab::~BindingsTab()
{
    qApp->removeEventFilter(this);
}

bool BindingsTab::eventFilter(QObject *watched, QEvent *event)
{
    // Only feed keys while an active keyboard capture is waiting.
    if (event->type() == QEvent::KeyPress && m_captureRow >= 0 &&
        m_reader.isKeyboardDevice())
    {
        const int rl = qtKeyToRaylib(static_cast<QKeyEvent *>(event)->key());
        if (rl != 0)
        {
            m_reader.captureKey(rl);
            return true; // consume the press so it doesn't double-bind
        }
    }
    return QWidget::eventFilter(watched, event);
}

bool BindingsTab::load()
{
    const std::string p1 = playerConfigPath(0);
    const std::string p2 = playerConfigPath(1);
    if (!padconfFileExists(p1) && !padconfFileExists(p2))
    {
        // No per-player files yet: migrate the legacy savedata root pad.conf
        // (or just seed defaults) so the table isn't all "None".
        const std::string legacy = padconfLegacyPath();
        m_players = {};
        for (auto &p : m_players)
            padconf::applyDefaultGamepadBinds(p);
        padconf::load(legacy, legacy, m_players);
    }
    else
    {
        m_players = {};
        padconf::load(p1, p2, m_players);
    }
    onPlayerChanged(0);
    return true;
}

bool BindingsTab::save()
{
    const std::string p1 = playerConfigPath(0);
    const std::string p2 = playerConfigPath(1);
    return padconf::save(p1, p2, m_players);
}

std::string BindingsTab::padconfPath() const
{
    // Legacy single pad.conf (deploy root), kept for migration.
    const QDir dir(apppaths::userRoot());
    return dir.filePath(QStringLiteral("pad.conf")).toStdString();
}

std::string BindingsTab::padconfLegacyPath() const { return padconfPath(); }

std::string BindingsTab::playerConfigPath(int p) const
{
    const QDir dir(apppaths::userRoot());
    return dir.filePath(QStringLiteral("savedata/pad_p%1.conf").arg(p + 1)).toStdString();
}

bool BindingsTab::padconfFileExists(const std::string &path) const
{
    return QFileInfo(QString::fromStdString(path)).exists();
}

void BindingsTab::openCaptureDevice()
{
    // Resolve the capture node from the player's assigned device (pad.conf
    // semantics): Auto -> first gamepad (fallback navbar: keyboard), Keyboard
    // -> first keyboard, Gamepad N -> the N-th gamepad in the filtered list.
    const int p = m_player->currentIndex();
    std::string node;
    if (p >= 0 && p < (int)m_players.size())
    {
        const auto &dev = m_players[p].device;
        if (dev.kind == padconf::DevKind::Keyboard)
        {
            node = evin::pickKeyboardNode(m_filteredDevices);
        }
        else if (dev.kind == padconf::DevKind::Gamepad)
        {
            int g = 0;
            for (auto &d : m_filteredDevices)
                if (d.isGamepad && g++ == dev.gamepad) { node = d.node; break; }
        }
    }
    if (node.empty())
    {
        for (auto &d : m_filteredDevices)
            if (d.isGamepad) { node = d.node; break; }
    }
    if (node != m_openedNode)
    {
        m_reader.close();
        m_openedNode.clear();
        if (!node.empty())
        {
            if (m_reader.open(node))
                m_openedNode = node;
        }
    }
}

void BindingsTab::refreshDevices()
{
    // Rebuild the appearance of the physical device list behind the combo.
    // The combo itself only exposes semantic entries (Auto / Keyboard /
    // Gamepad N): each player picks one, and pad.conf stores None/Keyboard/
    // Gamepad <index> exactly like the in-game device selector.
    const bool wasEmittingAuto = m_device->signalsBlocked() == false;
    const int prev = m_device->currentIndex();
    m_device->blockSignals(true);
    m_device->clear();
    m_filteredDevices.clear();
    const auto devs = evin::listDevices();
    for (auto &d : devs)
    {
        // Only actual input devices can be bound.
        if (!d.isGamepad && !d.isKeyboard && !d.isMouse)
            continue;
        m_filteredDevices.push_back(d);
    }

    m_device->addItem(QStringLiteral("(auto)"));
    m_device->addItem(QStringLiteral("Keyboard"));
    // Enumerate all gamepads (matching the runtime's availableGamepads()).
    int g = 0;
    for (auto &d : m_filteredDevices)
    {
        if (!d.isGamepad)
            continue;
        const QString label = QStringLiteral("Gamepad %1 (%2)")
                                  .arg(g)
                                  .arg(QString::fromStdString(d.name.empty() ? d.node : d.name));
        m_device->addItem(label);
        ++g;
    }
    if (prev >= 0 && prev < m_device->count())
        m_device->setCurrentIndex(prev);
    else
        m_device->setCurrentIndex(0);
    m_device->blockSignals(false);
    if (wasEmittingAuto)
        onDeviceChanged(m_device->currentIndex());
    else
        openCaptureDevice();
    setStatus(QStringLiteral("Devices refreshed."));
}

void BindingsTab::onDeviceChanged(int)
{
    // Persist the selection into this player's pad.conf device entry.
    const int p = m_player->currentIndex();
    if (p < 0 || p >= (int)m_players.size())
        return;
    const int idx = m_device->currentIndex();
    padconf::Device dev;
    if (idx == kDevAuto)
    {
        dev = {padconf::DevKind::None, -1};
    }
    else if (idx == kDevKeyboard)
    {
        dev = {padconf::DevKind::Keyboard, -1};
    }
    else
    {
        const int g = idx - kDevGamepadBase;
        dev = {padconf::DevKind::Gamepad, g};
    }
    padconf::Player &player = m_players[p];
    // When the device kind changes, make sure the player ends up with a usable
    // layout: bind a keyboard layout on Keyboard and a gamepad layout on pads
    // (if the new kind has no binds of its own yet).
    const padconf::DevKind newKind = dev.kind;
    player.device = dev;
    if (newKind == padconf::DevKind::Keyboard && !hasKeyBinds(player))
    {
        padconf::applyDefaultKeyboardBinds(player);
        setStatus(QStringLiteral("Keyboard layout applied for Player %1.").arg(p + 1));
    }
    else if (newKind == padconf::DevKind::Gamepad && !hasPadBinds(player))
    {
        padconf::applyDefaultGamepadBinds(player);
        setStatus(QStringLiteral("Gamepad layout applied for Player %1.").arg(p + 1));
    }
    for (int a = 0; a < 24; ++a)
        refreshRow(a);
    openCaptureDevice();
}

void BindingsTab::onLoadDefaults()
{
    const int p = m_player->currentIndex();
    if (p < 0 || p >= (int)m_players.size())
        return;
    // Load the default layout that matches the current device kind, so
    // Keyboard players get WASD/arrows (not gamepad buttons).
    const padconf::DevKind kind = m_players[p].device.kind;
    m_players[p] = padconf::Player{};
    if (kind == padconf::DevKind::Keyboard)
    {
        m_players[p].device = {padconf::DevKind::Keyboard, -1};
        padconf::applyDefaultKeyboardBinds(m_players[p]);
        setStatus(QStringLiteral("Default keyboard layout loaded for Player %1.").arg(p + 1));
    }
    else
    {
        m_players[p].device = {padconf::DevKind::None, -1};
        padconf::applyDefaultGamepadBinds(m_players[p]);
        setStatus(QStringLiteral("Default gamepad layout loaded for Player %1.").arg(p + 1));
    }
    onPlayerChanged(p);
}

void BindingsTab::refreshRow(int action)
{
    const auto &b = m_players[m_player->currentIndex()].binds[action];
    m_table->item(action, 1)->setText(QString::fromStdString(b.kind == padconf::BindKind::None
        ? "None" : padconf::bindDisplay(b)));
}

void BindingsTab::onPlayerChanged(int)
{
    // Sync the device combo with this player's assigned device so P1/P2 can
    // each hold an independent device (e.g. P1 = Gamepad, P2 = Keyboard).
    const int p = m_player->currentIndex();
    if (p >= 0 && p < (int)m_players.size())
    {
        const auto &dev = m_players[p].device;
        int want = kDevAuto;
        if (dev.kind == padconf::DevKind::Keyboard)
            want = kDevKeyboard;
        else if (dev.kind == padconf::DevKind::Gamepad && dev.gamepad >= 0)
            want = kDevGamepadBase + dev.gamepad;
        m_device->blockSignals(true);
        if (want < m_device->count())
            m_device->setCurrentIndex(want);
        m_device->blockSignals(false);
    }
    for (int a = 0; a < 24; ++a)
        refreshRow(a);
    openCaptureDevice();
}

void BindingsTab::setStatus(const QString &text)
{
    if (m_captureStatus)
        m_captureStatus->setText(text);
}

void BindingsTab::onBindClicked()
{
    auto *src = qobject_cast<QPushButton *>(sender());
    if (!src || m_captureRow >= 0)
        return;
    m_captureRow = src->property("row").toInt();
    // Snapshot which inputs are already down at click time and lock them out
    // until they return to neutral, so a (still held) button just used to bind
    // the previous row can't instantly re-fire here, and a drifting axis can't
    // keep the capture waiting forever.
    if (m_reader.isOpen())
        m_reader.update();
    m_ignoredBtns.fill(0);
    m_ignoredAxes.fill(0);
    if (m_reader.isOpen())
    {
        for (int rb = 0; rb < 16; ++rb)
            m_ignoredBtns[rb] = m_reader.buttonDown(rb) ? 1 : 0;
        for (auto &ax : kAxisCaptures)
            m_ignoredAxes[ax.axis] =
                std::fabs(m_reader.axis(ax.axis)) > kAxisIdleThreshold ? 1 : 0;
    }
    m_elapsed.start();
    setStatus(m_reader.isKeyboardDevice()
        ? QStringLiteral("Press a key on %1 (5s timeout)...").arg(m_device->currentText())
        : QStringLiteral("Press a button or move a stick/trigger on %1 (5s timeout)...")
              .arg(m_device->currentText()));
    m_timer->start();
}

void BindingsTab::pollCapture()
{
    if (m_captureRow < 0)
        return;
    if (m_elapsed.elapsed() > 5000)
    {
        m_timer->stop();
        m_captureRow = -1;
        m_ignoredBtns.fill(0);
        m_ignoredAxes.fill(0);
        setStatus(QStringLiteral("Capture timed out. Click Set to retry."));
        return;
    }

    if (!m_reader.isOpen())
    {
        m_timer->stop();
        m_captureRow = -1;
        m_ignoredBtns.fill(0);
        m_ignoredAxes.fill(0);
        setStatus(QStringLiteral("Capture device unavailable. Check the selected device."));
        return;
    }
    m_reader.update();

    // Keyboard capture: keys are injected directly by the window event filter
    // (as raylib codes) via captureKey(); takeLastKey is edge-triggered, so no
    // latch problem like gamepad buttons.
    if (m_reader.isKeyboardDevice())
    {
        const int rl = m_reader.takeLastKey();
        if (rl > 0)
        {
            padconf::Bind b;
            b.kind = padconf::BindKind::Key;
            b.value = rl;
            m_players[m_player->currentIndex()].binds[m_captureRow] = b;
            finishCapture();
        }
        return;
    }

    // Release an ignore-lock as soon as that input goes back to neutral.
    for (int rb = 0; rb < 16; ++rb)
        if (m_ignoredBtns[rb] && !m_reader.buttonDown(rb))
            m_ignoredBtns[rb] = 0;
    for (auto &ax : kAxisCaptures)
        if (m_ignoredAxes[ax.axis] &&
            std::fabs(m_reader.axis(ax.axis)) <= kAxisIdleThreshold)
            m_ignoredAxes[ax.axis] = 0;

    for (int rb = 0; rb < 16; ++rb)
    {
        if (m_reader.buttonDown(rb) && !m_ignoredBtns[rb])
        {
            padconf::Bind b;
            b.kind = padconf::BindKind::Button;
            b.value = evinToGamepadButton(rb);
            m_players[m_player->currentIndex()].binds[m_captureRow] = b;
            finishCapture();
            return;
        }
    }
    for (auto &ax : kAxisCaptures)
    {
        const float v = m_reader.axis(ax.axis);
        if (std::fabs(v) > ax.need && !m_ignoredAxes[ax.axis])
        {
            padconf::Bind b;
            b.kind = padconf::BindKind::Axis;
            b.value = ax.axis;
            b.sign = (ax.axis == evin::AxisLT || ax.axis == evin::AxisRT) ? 1.0f
                     : (v < 0.0f ? -1.0f : 1.0f);
            m_players[m_player->currentIndex()].binds[m_captureRow] = b;
            finishCapture();
            return;
        }
    }
}

void BindingsTab::finishCapture()
{
    m_timer->stop();
    const int row = m_captureRow;
    m_captureRow = -1;
    m_ignoredBtns.fill(0);
    m_ignoredAxes.fill(0);
    refreshRow(row);
    setStatus(QStringLiteral("Captured Player %1: %2")
                  .arg(m_player->currentIndex() + 1)
                  .arg(QString::fromStdString(
                      padconf::bindDisplay(m_players[m_player->currentIndex()].binds[row]))));
}