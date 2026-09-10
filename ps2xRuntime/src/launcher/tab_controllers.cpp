#include "tab_controllers.h"

#include "input_reader.h"
#include "settings_manager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

StickWidget::StickWidget()
{
    setFixedSize(64, 64);
    setAlignment(Qt::AlignCenter);
    m_dot = QPointF(32, 32);
}

void StickWidget::setDot(qreal x, qreal y)
{
    m_dot = QPointF(32 + x * 26.0, 32 + y * 26.0);
    update();
}

void StickWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(120, 120, 150), 1));
    p.setBrush(QColor(30, 30, 45));
    p.drawEllipse(rect().adjusted(3, 3, -3, -3));
    p.setPen(QPen(QColor(255, 220, 120), 1));
    p.setBrush(QColor(240, 160, 48));
    p.drawEllipse(m_dot, 7, 7);
}

Gauge::Gauge()
{
    setFixedSize(14, 56);
}

void Gauge::setValue(float v)
{
    if (m_v != v)
    {
        m_v = v;
        update();
    }
}

void Gauge::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const QRectF r = rect();
    p.setPen(QPen(QColor(120, 120, 150), 1));
    p.setBrush(QColor(30, 30, 45));
    p.drawRect(r.adjusted(0, 0, -1, -1));
    p.setBrush(QColor(255, 158, 26));
    const float h = r.height() * m_v;
    p.drawRect(QRectF(r.left() + 1, r.top() + 1, r.width() - 2, static_cast<qreal>(h) * 0.9F));
}

ControllersTab::ControllersTab(std::array<padconf::Player, 2> *shared, QWidget *parent)
    : QWidget(parent)
    , m_players(shared)
{
    auto &s = SettingsManager::instance();

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 14, 18, 14);
    root->setSpacing(10);

    // DEVICE section
    auto *devLabel = new QLabel(QStringLiteral("DEVICE"));
    devLabel->setObjectName(QStringLiteral("sectionLabel"));
    root->addWidget(devLabel);

    auto *playerRow = new QWidget;
    auto *playerLay = new QHBoxLayout(playerRow);
    playerLay->setContentsMargins(8, 2, 8, 2);
    auto *pl = new QLabel(QStringLiteral("Player"));
    pl->setObjectName(QStringLiteral("valueLabel"));
    m_player = new QComboBox;
    m_player->addItems({QStringLiteral("P1"), QStringLiteral("P2")});
    playerLay->addWidget(pl, 1);
    playerLay->addWidget(m_player, 0);
    root->addWidget(playerRow);

    auto *devRow = new QWidget;
    auto *devLay = new QHBoxLayout(devRow);
    devLay->setContentsMargins(8, 2, 8, 2);
    auto *dl = new QLabel(QStringLiteral("Device"));
    dl->setObjectName(QStringLiteral("valueLabel"));
    m_device = new QComboBox;
    devLay->addWidget(dl, 1);
    devLay->addWidget(m_device, 0);
    m_device->setMinimumWidth(260);
    root->addWidget(devRow);

    // Deadzone
    auto *dzRow = new QWidget;
    auto *dzLay = new QHBoxLayout(dzRow);
    dzLay->setContentsMargins(8, 2, 8, 2);
    auto *zl = new QLabel(QStringLiteral("Deadzone"));
    zl->setObjectName(QStringLiteral("valueLabel"));
    zl->setMinimumWidth(110);
    m_deadzone = new QSlider(Qt::Horizontal);
    m_deadzone->setRange(0, 50);
    m_deadzone->setValue(int(s.deadzone() * 100.0f));
    m_deadzoneVal = new QLabel(QStringLiteral("15%"));
    m_deadzoneVal->setObjectName(QStringLiteral("valueLabel"));
    dzLay->addWidget(zl);
    dzLay->addWidget(m_deadzone, 1);
    dzLay->addWidget(m_deadzoneVal);
    root->addWidget(dzRow);

    // OVERLAY master switch
    auto *ovLabel = new QLabel(QStringLiteral("OVERLAY"));
    ovLabel->setObjectName(QStringLiteral("sectionLabel"));
    root->addWidget(ovLabel);
    auto *ovRow = new QWidget;
    auto *ovLay = new QHBoxLayout(ovRow);
    ovLay->setContentsMargins(8, 2, 8, 2);
    auto *ol = new QLabel(QStringLiteral("In-Game Settings Overlay"));
    ol->setObjectName(QStringLiteral("valueLabel"));
    m_overlayEnabled = new QCheckBox;
    m_overlayEnabled->setChecked(s.overlayEnabled());
    ovLay->addWidget(ol, 1);
    ovLay->addWidget(m_overlayEnabled);
    root->addWidget(ovRow);
    auto *ovHint = new QLabel(QStringLiteral(
        "Holds the open-hint combo (Shift+Tab / Select+Start). Off = the overlay never opens in-game."));
    ovHint->setObjectName(QStringLiteral("hintLabel"));
    ovHint->setWordWrap(true);
    root->addWidget(ovHint);

    // GAMEPAD TEST
    auto *testLabel = new QLabel(QStringLiteral("GAMEPAD TEST"));
    testLabel->setObjectName(QStringLiteral("sectionLabel"));
    root->addWidget(testLabel);

    auto *btnGrid = new QWidget;
    auto *grid = new QGridLayout(btnGrid);
    grid->setContentsMargins(8, 2, 8, 2);
    grid->setSpacing(6);

    static const struct { const char *label; int idx; } kbtns[] = {
        {"Select", evin::BtnSelect}, {"Start", evin::BtnStart}, {"Guide", -1},
        {"D-Up", evin::BtnDpadUp}, {"D-Down", evin::BtnDpadDown},
        {"D-Left", evin::BtnDpadLeft}, {"D-Right", evin::BtnDpadRight},
        {"Y / △", evin::BtnY}, {"B / ○", evin::BtnB}, {"A / ×", evin::BtnA},
        {"X / □", evin::BtnX}, {"LB / L1", evin::BtnLB}, {"RB / R1", evin::BtnRB},
        {"LT / L2", evin::BtnLT}, {"RT / R2", evin::BtnRT}, {"L3", evin::BtnLS},
        {"R3", evin::BtnRS}};
    int col = 0, crow = 0;
    for (auto &b : kbtns)
    {
        auto *lbl = new QLabel(QString::fromLatin1(b.label));
        lbl->setFrameShape(QFrame::StyledPanel);
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setMinimumSize(64, 24);
        lbl->setStyleSheet(
            QStringLiteral("QLabel { background-color: #212128; border: 1px solid #3a3a4a; border-radius: 2px; color: #8c8c9e; }"));
        m_buttons.push_back({lbl, b.idx});
        grid->addWidget(lbl, crow, col++);
        if (col == 8) { col = 0; ++crow; }
    }
    root->addWidget(btnGrid);

    // Sticks + triggers
    auto *sticksRow = new QWidget;
    auto *sticksLay = new QHBoxLayout(sticksRow);
    sticksLay->setContentsMargins(8, 2, 8, 2);
    sticksLay->setSpacing(16);
    m_stickL = new StickWidget;
    m_stickR = new StickWidget;
    m_lt = new Gauge;
    m_rt = new Gauge;
    auto makeG = [](const QString &n, Gauge *g) {
        auto *w = new QWidget;
        auto *l = new QVBoxLayout(w);
        auto *lab = new QLabel(n);
        lab->setObjectName(QStringLiteral("hintLabel"));
        lab->setAlignment(Qt::AlignCenter);
        l->addWidget(lab);
        l->addWidget(g, 0, Qt::AlignCenter);
        return w;
    };
    sticksLay->addWidget(m_stickL, 0, Qt::AlignCenter);
    sticksLay->addWidget(makeG(QStringLiteral("LT"), m_lt), 0, Qt::AlignCenter);
    sticksLay->addWidget(m_stickR, 0, Qt::AlignCenter);
    sticksLay->addWidget(makeG(QStringLiteral("RT"), m_rt), 0, Qt::AlignCenter);
    sticksLay->addStretch(1);
    root->addWidget(sticksRow);

    m_axisReadout = new QLabel(QStringLiteral("LX %0.2f   RX %0.2f   LY %0.2f   RY %0.2f"));
    m_axisReadout->setObjectName(QStringLiteral("hintLabel"));
    root->addWidget(m_axisReadout);
    root->addStretch(1);

    // --- wiring ---
    connect(m_overlayEnabled, &QCheckBox::toggled, this, [](bool v) {
        SettingsManager::instance().setOverlayEnabled(v);
    });
    connect(m_deadzone, &QSlider::valueChanged, this, &ControllersTab::onDeadzone);
    connect(m_player, &QComboBox::currentIndexChanged, this, &ControllersTab::onPlayerChanged);
    connect(m_device, &QComboBox::currentIndexChanged, this, &ControllersTab::onDeviceChanged);

    refreshDevices();
    m_timer = new QTimer(this);
    m_timer->setInterval(16);
    connect(m_timer, &QTimer::timeout, this, &ControllersTab::pollGamepad);
    m_timer->start();
}

void ControllersTab::onDeadzone(int v)
{
    m_deadzoneVal->setText(QString::number(v) + QStringLiteral("%"));
    SettingsManager::instance().setDeadzone(v / 100.0f);
}

void ControllersTab::refreshDevices()
{
    // The combo is semantic, exactly like the Bindings tab: (auto) / Keyboard /
    // Gamepad N. It must NOT depend on physical-device probing (keyd virtual
    // keyboards etc. are often invisible to evdev), otherwise a saved Keyboard
    // selection could never be restored and would fall back to (auto).
    m_device->blockSignals(true);
    m_device->clear();
    m_filteredDevices.clear();
    const auto devs = evin::listDevices();
    for (auto &d : devs)
    {
        // Keep only useful input devices (matters for the live test node).
        if (!d.isGamepad && !d.isKeyboard && !d.isMouse)
            continue;
        m_filteredDevices.push_back(d);
    }

    m_device->addItem(QStringLiteral("(auto)"));
    m_device->addItem(QStringLiteral("Keyboard"));
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
    m_device->blockSignals(false);
    onPlayerChanged();
}

void ControllersTab::syncFromPads()
{
    onPlayerChanged();
}

void ControllersTab::onPlayerChanged()
{
    // Restore this player's assigned device (pad_conf semantics) into the
    // semantic combo, mirroring BindingsTab::onPlayerChanged().
    constexpr int kDevKeyboard = 1;
    constexpr int kDevGamepadBase = 2;
    const int p = m_player->currentIndex();
    int want = 0; // "(auto)"
    if (m_players && p >= 0 && p < (int)m_players->size())
    {
        const auto &dev = (*m_players)[p].device;
        switch (dev.kind)
        {
        case padconf::DevKind::Keyboard:
            want = kDevKeyboard;
            break;
        case padconf::DevKind::Gamepad:
            want = kDevGamepadBase + dev.gamepad;
            break;
        default:
            want = 0;
            break;
        }
    }
    m_device->blockSignals(true);
    if (want < m_device->count())
        m_device->setCurrentIndex(want);
    else
        m_device->setCurrentIndex(0);
    m_device->blockSignals(false);
    openDevice();
}

void ControllersTab::onDeviceChanged()
{
    // Persist the semantic selection back into this player's pad.device,
    // mirroring BindingsTab::onDeviceChanged()'s Auto/Keyboard/Gamepad N map.
    constexpr int kDevKeyboard = 1;
    constexpr int kDevGamepadBase = 2;
    if (!m_players)
        return;
    const int p = m_player->currentIndex();
    if (p < 0 || p >= (int)m_players->size())
        return;
    const int idx = m_device->currentIndex();
    padconf::Device dev;
    if (idx == 0)
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
    (*m_players)[p].device = dev;
    openDevice();
}

void ControllersTab::openDevice()
{
    // Resolve the semantic combo to a joystick node for the live test, like
    // BindingsTab::openCaptureDevice(): Auto -> first gamepad, Keyboard -> the
    // Qt pseudo-keyboard, Gamepad N -> the N-th gamepad.
    constexpr int kDevKeyboard = 1;
    constexpr int kDevGamepadBase = 2;
    const int idx = m_device->currentIndex();
    std::string node;
    if (idx == kDevKeyboard)
    {
        node = evin::pickKeyboardNode(m_filteredDevices);
    }
    else if (idx >= kDevGamepadBase)
    {
        const int g = idx - kDevGamepadBase;
        int n = 0;
        for (auto &d : m_filteredDevices)
            if (d.isGamepad && n++ == g) { node = d.node; break; }
    }
    else // (auto)
    {
        // Open the first available gamepad so the live test responds right
        // away, just like the runtime's device auto-detect.
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

void ControllersTab::pollGamepad()
{
    if (!m_reader.isOpen())
    {
        // No device selected/available: reset indicators once and bail.
        if (m_devWasOpen)
        {
            m_devWasOpen = false;
            for (auto &b : m_buttons)
                if (b.code >= 0)
                    b.widget->setStyleSheet(QStringLiteral("QLabel { background-color: #212128; border: 1px solid #3a3a4a; border-radius: 2px; color: #8c8c9e; }"));
            m_stickL->setDot(0, 0);
            m_stickR->setDot(0, 0);
            m_lt->setValue(0);
            m_rt->setValue(0);
            m_axisReadout->setText(QStringLiteral("LX %1  RX %2  LY %3  RY %4").arg(0.0, 6, 'f', 2).arg(0.0, 6, 'f', 2).arg(0.0, 6, 'f', 2).arg(0.0, 6, 'f', 2));
        }
        return;
    }
    m_devWasOpen = true;
    m_reader.update();

    for (auto &b : m_buttons)
    {
        if (b.code < 0)
            continue;
        const bool down = m_reader.buttonDown(b.code);
        b.widget->setStyleSheet(down
            ? QStringLiteral("QLabel { background-color: rgba(255,158,26,0.85); border: 1px solid #ff9e1a; border-radius: 2px; color: #1a1208; font-weight: bold; }")
            : QStringLiteral("QLabel { background-color: #212128; border: 1px solid #3a3a4a; border-radius: 2px; color: #8c8c9e; }"));
    }
    // Joystick Y is -1 up, +1 down; QPainter's y axis grows downward, so the
    // raw axis maps straight onto the widget without extra negation.
    m_stickL->setDot(m_reader.axis(evin::AxisLX), m_reader.axis(evin::AxisLY));
    m_stickR->setDot(m_reader.axis(evin::AxisRX), m_reader.axis(evin::AxisRY));
    m_lt->setValue(m_reader.axis(evin::AxisLT));
    m_rt->setValue(m_reader.axis(evin::AxisRT));
    m_axisReadout->setText(QStringLiteral("LX %1  RX %2  LY %3  RY %4")
        .arg(m_reader.axis(evin::AxisLX), 6, 'f', 2)
        .arg(m_reader.axis(evin::AxisRX), 6, 'f', 2)
        .arg(m_reader.axis(evin::AxisLY), 6, 'f', 2)
        .arg(m_reader.axis(evin::AxisRY), 6, 'f', 2));
}