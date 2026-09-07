/*
 * KeypadWindow -- see KeypadWindow.h. The Qt twin of the GNOME panel, and
 * deliberately the same shape: both controllers side by side, held-not-pulsed
 * presses, a System row and a Map row.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KeypadWindow.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QFont>
#include <QLabel>
#include <QMouseEvent>
#include <QVBoxLayout>

#include "KeyForward.h"

PadButton::PadButton(const QString &face, int target, QWidget *parent)
    : QPushButton(face, parent), m_target(target), m_face(face)
{
    setMinimumSize(48, 34);
    /* Not focusable: clicking a pad button must not steal focus, and tabbing
     * through thirty-eight buttons is nobody's idea of input. */
    setFocusPolicy(Qt::NoFocus);
}

void PadButton::mousePressEvent(QMouseEvent *e)
{
    QPushButton::mousePressEvent(e);
    if (!m_down) { m_down = true; emit pressedTarget(m_target); }
}

void PadButton::mouseReleaseEvent(QMouseEvent *e)
{
    QPushButton::mouseReleaseEvent(e);
    if (m_down) { m_down = false; emit releasedTarget(m_target); }
}

/* Dragging off the button must release it, or the machine believes it is
 * held forever. */
void PadButton::leaveEvent(QEvent *e)
{
    QPushButton::leaveEvent(e);
    if (m_down) { m_down = false; emit releasedTarget(m_target); }
}

static const char *const kKeypadFace[COLECO_KEYPAD_KEYS] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "#"
};
/* The physical 3x4 layout: 1-9 in reading order, then * 0 #. */
static const int kKeypadOrder[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 11 };

KeypadWindow::KeypadWindow(colecosession *session, QWidget *parent)
    /* Qt::Dialog, not Qt::Window: a plain window is an ordinary top-level and
     * a tiling window manager will tile it, which stretches a panel of
     * fixed-size buttons across half the screen. A dialog is a utility
     * window -- floated by every WM, kept above its parent, and off the
     * tiling grid. */
    : QWidget(parent, Qt::Dialog), m_session(session)
{
    setWindowTitle(QStringLiteral("Controllers"));
    coleco_input_reset(&m_keys);

    auto *root = new QVBoxLayout(this);
    /* Size to the controls and stay there. Without this the panel is
     * resizable, and stretching it just pulls the buttons out of shape --
     * there is nothing here that benefits from more room. */
    root->setSizeConstraint(QLayout::SetFixedSize);
    auto *ports = new QHBoxLayout;
    ports->addWidget(buildController(0));
    ports->addWidget(buildController(1));
    root->addLayout(ports);

    auto *sys = new QHBoxLayout;
    for (int sa = 0; sa < COLECO_SYSACT_COUNT; sa++) {
        auto *b = new PadButton(
            QString::fromUtf8(coleco_binding_label(COLECO_TARGET_SYSACT(sa))),
            COLECO_TARGET_SYSACT(sa));
        connect(b, &PadButton::pressedTarget, this, &KeypadWindow::onPressed);
        connect(b, &PadButton::releasedTarget, this, &KeypadWindow::onReleased);
        m_controls.push_back(b);
        sys->addWidget(b);
    }
    root->addLayout(sys);

    auto *maprow = new QHBoxLayout;
    m_mapButton = new QPushButton(QStringLiteral("Map"));
    connect(m_mapButton, &QPushButton::clicked, this,
            [this] { setMapState(m_mapState == -2 ? -1 : -2); });
    auto *defaults = new QPushButton(QStringLiteral("Defaults"));
    connect(defaults, &QPushButton::clicked, this, [this] {
        coleco_bindings_reset_defaults(m_session);
        refreshLabels();
    });
    m_hint = new QLabel;
    maprow->addWidget(m_mapButton);
    maprow->addWidget(defaults);
    maprow->addWidget(m_hint, 1);
    root->addLayout(maprow);

    setMapState(-2);
}

QWidget *KeypadWindow::buildController(int port)
{
    /* The heading is an explicit centred label rather than the group box's
     * own title. QGroupBox::setAlignment(Qt::AlignHCenter) is advisory --
     * several styles, including the one this renders under, draw the title
     * left regardless -- and a heading that sits over its controls in one
     * theme and off to the side in another is not a heading. */
    auto *box = new QGroupBox;
    auto *v = new QVBoxLayout(box);

    auto *title = new QLabel(QStringLiteral("Controller %1").arg(port + 1));
    title->setAlignment(Qt::AlignHCenter);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    v->addWidget(title);

    auto add = [&](const QString &face, int act) {
        auto *b = new PadButton(face, COLECO_TARGET_PORT(port, act));
        connect(b, &PadButton::pressedTarget, this, &KeypadWindow::onPressed);
        connect(b, &PadButton::releasedTarget, this, &KeypadWindow::onReleased);
        m_controls.push_back(b);
        return b;
    };

    auto *pad = new QGridLayout;
    for (int i = 0; i < 12; i++) {
        const int key = kKeypadOrder[i];
        pad->addWidget(add(QString::fromUtf8(kKeypadFace[key]),
                           COLECO_ACT_KEYPAD + key), i / 3, i % 3);
    }
    v->addLayout(pad);

    auto *dpad = new QGridLayout;
    dpad->addWidget(add(QStringLiteral("▲"), COLECO_ACT_UP), 0, 1);
    dpad->addWidget(add(QStringLiteral("◀"), COLECO_ACT_LEFT), 1, 0);
    dpad->addWidget(add(QStringLiteral("▶"), COLECO_ACT_RIGHT), 1, 2);
    dpad->addWidget(add(QStringLiteral("▼"), COLECO_ACT_DOWN), 2, 1);
    v->addLayout(dpad);

    auto *fires = new QHBoxLayout;
    fires->addWidget(add(QStringLiteral("Fire L"), COLECO_ACT_FIRE_L));
    fires->addWidget(add(QStringLiteral("Fire R"), COLECO_ACT_FIRE_R));
    v->addLayout(fires);
    return box;
}

void KeypadWindow::onPressed(int target)
{
    if (m_mapState == -1) { setMapState(target); return; }
    if (m_mapState >= 0) return;
    if (target >= COLECO_TARGET_SYSACT(0)) return;  /* fires on release */
    colecosession_press(m_session, target / COLECO_ACT_PER_PORT,
                        target % COLECO_ACT_PER_PORT, 1);
}

void KeypadWindow::onReleased(int target)
{
    if (m_mapState != -2) return;
    if (target >= COLECO_TARGET_SYSACT(0)) {
        /* System actions fire on release, like a real button: pressing and
         * dragging off should not reset the console. */
        colecosession_sysaction(m_session, target - COLECO_TARGET_SYSACT(0));
        return;
    }
    colecosession_press(m_session, target / COLECO_ACT_PER_PORT,
                        target % COLECO_ACT_PER_PORT, 0);
}

void KeypadWindow::setMapState(int state)
{
    m_mapState = state;
    if (state == -2) {
        m_mapButton->setText(QStringLiteral("Map"));
        m_hint->clear();
    } else if (state == -1) {
        m_mapButton->setText(QStringLiteral("Cancel"));
        m_hint->setText(QStringLiteral("Click a control to remap"));
    } else {
        m_hint->setText(QStringLiteral("Press a key for %1")
                            .arg(QString::fromUtf8(coleco_binding_label(state))));
    }
    refreshLabels();
}

void KeypadWindow::refreshLabels()
{
    for (PadButton *b : m_controls) {
        if (m_mapState != -2) {
            const char *k = coleco_binding_key_name(b->target());
            b->setText(*k ? QString::fromUtf8(k) : QStringLiteral("—"));
        } else {
            b->setText(b->face());
        }
    }
}

void KeypadWindow::keyPressEvent(QKeyEvent *e)
{
    const uint32_t ks = colecoKeysymFromQt(e);
    if (m_mapState >= 0) {
        if (ks) { coleco_binding_set(m_session, m_mapState, ks); setMapState(-1); }
        return;
    }
    if (m_mapState == -1) return;
    if (!ks) { QWidget::keyPressEvent(e); return; }

    const int sa = coleco_input_key_sysaction(ks);
    if (sa >= 0) { colecosession_sysaction(m_session, sa); return; }
    if (coleco_input_key(&m_keys, ks, 1)) {
        colecosession_joystick_raw(m_session, 0, coleco_input_word(&m_keys, 0));
        colecosession_joystick_raw(m_session, 1, coleco_input_word(&m_keys, 1));
    }
}

void KeypadWindow::keyReleaseEvent(QKeyEvent *e)
{
    if (m_mapState != -2) return;
    const uint32_t ks = colecoKeysymFromQt(e);
    if (ks && coleco_input_key(&m_keys, ks, 0)) {
        colecosession_joystick_raw(m_session, 0, coleco_input_word(&m_keys, 0));
        colecosession_joystick_raw(m_session, 1, coleco_input_word(&m_keys, 1));
    }
}
