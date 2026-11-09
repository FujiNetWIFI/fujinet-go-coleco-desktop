/*
 * ControllersWindow -- see ControllersWindow.h.
 *
 * The two ColecoVision hand controllers as a player sees them, side by side
 * (Controller 1 on the left) in a charcoal case under a thin brand strip: each
 * a black body with a short stick on a disc at the top ringed by eight
 * direction wedges, a fire button down either side and the 12-key phone keypad
 * in the frame of the overlay-card slot. Below the case, a strip with the
 * app's two console actions, Reset Console and Reset to CONFIG. Everything is
 * painted from the session's layout (colecosession_controls_layout, in units
 * of a keypad key's width), so every frontend shows the same parts in the same
 * places; the colours are this file's.
 *
 * Gestures act on mouse press AND release: the console samples its controllers
 * once a frame, so a press present only for the instant of a click would fall
 * between frames. A key or a fire button is held while the mouse is down on it
 * (dragging off lets it go); a press on the stick or a wedge tilts it, and a
 * drag moves on from one direction to the next. One contact at a time: chords
 * come from the keyboard or a pad. Hiding the window and losing the focus let
 * go of everything the mouse holds.
 *
 * What the console sees held -- host keyboard, gamepads and this window -- is
 * lit in the accent, so the window doubles as an input tester for a new pad.
 * Keys typed with it active go to the console as in the main window.
 *
 * FIXED SIZE: the drawing is a fixed number of pixels per layout unit, Map
 * legends are fitted into their parts, and gamepad names are elided, so
 * neither Map mode nor a long name resizes the window. A singleton, hidden
 * rather than destroyed, so a half-done remap survives closing it. A timer
 * runs only while it is visible.
 *
 * MAP MODE: Map arms a two-step rebind -- click a part (a wedge, a fire
 * button, a key, or one of the console actions below), then press a host key or
 * a gamepad button. Rebinding steals the key from whatever held it (named in
 * the hint). Escape cancels; while Map is armed nothing is typed into the
 * console.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ControllersWindow.h"

#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPolygonF>
#include <QPushButton>
#include <QRadialGradient>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QtMath>

#include <functional>
#include <utility>
#include <vector>

#include "../Accent.h"
#include "../KeyForward.h"
#include "../debugger/DebuggerWindow.h"
#include "../help/HelpWindow.h"

namespace {

constexpr qreal kUnit = 40.0;        /* pixels per layout unit (a keypad key's width) */
constexpr int kCasePad = 18;         /* the case's margin round the controls */
constexpr int kBrandH = 22;          /* the brand strip along the top */
constexpr int kTopGap = 12;
constexpr int kTop = kBrandH + kTopGap;   /* where the layout's origin sits */
constexpr int kStripGap = 10;        /* between the case and the console strip */
constexpr int kStripH = 34;
constexpr int kStripW = 214;
constexpr int kStripSpace = 12;
constexpr int kStripTail = 6;
constexpr int kMaxPadRows = 3;
constexpr qreal kDimAlpha = 0.4;     /* an unpicked part while Map waits for a key */
constexpr qreal kCapReach = 0.14;    /* the stick cap's travel, in disc radii */

const QColor kInk(0xE4, 0xE0, 0xD6);        /* light text on the case */
const QColor kDimInk(0x9A, 0x96, 0x8C);
const QColor kCaseTop(0x3A, 0x3A, 0x3D);    /* the case's charcoal */
const QColor kCaseBottom(0x1F, 0x1F, 0x21);
const QColor kCaseEdge(0x0B, 0x0B, 0x0C);
const QColor kCaseLight(0x58, 0x58, 0x5C);
const QColor kBrandBar(0x15, 0x15, 0x17);
const QColor kBodyTop(0x2B, 0x2B, 0x2E);    /* a controller's black body */
const QColor kBodyBottom(0x15, 0x15, 0x17);
const QColor kBodyEdge(0x05, 0x05, 0x05);
const QColor kBodyLight(0x3C, 0x3C, 0x40);
const QColor kPlate(0x0D, 0x0D, 0x0E);      /* the stick's recessed plate */
const QColor kPlateEdge(0x18, 0x18, 0x1A);
const QColor kHub(0x14, 0x14, 0x15);
const QColor kWedge(0x2A, 0x2A, 0x2E);
const QColor kWedgeHover(0x3A, 0x3A, 0x40);
const QColor kWedgeEdge(0x0A, 0x0A, 0x0B);
const QColor kCapLight(0x55, 0x55, 0x5B);
const QColor kCapDark(0x1C, 0x1C, 0x1F);
const QColor kFireTop(0xD9, 0x46, 0x1E);    /* the orange-red side buttons */
const QColor kFireSide(0x7C, 0x24, 0x0F);
const QColor kFireHover(0xE8, 0x5A, 0x30);
const QColor kCardTop(0xB9, 0xBB, 0xBE);    /* the overlay card's silver faceplate */
const QColor kCardBottom(0x8E, 0x90, 0x93);
const QColor kCardEdge(0x4A, 0x4A, 0x4E);
const QColor kSlot(0x0B, 0x0B, 0x0C);
const QColor kKeyTop(0x1D, 0x1D, 0x20);     /* the keypad's black rubber caps */
const QColor kKeyHover(0x2A, 0x2A, 0x2E);
const QColor kKeySide(0x0A, 0x0A, 0x0B);
const QColor kKeyInk(0xF2, 0xF2, 0xF2);
const QColor kStripCap(0x3C, 0x3C, 0x41);   /* the console strip's buttons */
const QColor kStripHover(0x54, 0x54, 0x5B);
const QColor kStripSide(0x16, 0x16, 0x18);

QString local(const char *text) { return QString::fromUtf8(text ? text : ""); }

QFont scaledFont(QFont fnt, qreal scale, bool bold = false)
{
    fnt.setPointSizeF((fnt.pointSizeF() > 0 ? fnt.pointSizeF() : 10) * scale);
    fnt.setBold(bold);
    return fnt;
}

QFont pixelFont(QFont fnt, int pixels, bool bold)
{
    fnt.setPixelSize(pixels);
    fnt.setBold(bold);
    return fnt;
}

/* Lines centred in rect, the font shrunk until they fit. */
void drawFitted(QPainter &pnt, const QRectF &rect, const QString &text, QFont fnt)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (;;) {
        const QFontMetricsF metrics(fnt);
        qreal wide = 0;
        for (const QString &line : lines) wide = qMax(wide, metrics.horizontalAdvance(line));
        const qreal size = fnt.pixelSize() > 0 ? fnt.pixelSize() : fnt.pointSizeF();
        if ((wide <= rect.width() && metrics.lineSpacing() * lines.size() <= rect.height() + 2) || size <= 5.0)
            break;
        if (fnt.pixelSize() > 0) fnt.setPixelSize(fnt.pixelSize() - 1);
        else fnt.setPointSizeF(fnt.pointSizeF() - 0.5);
    }
    pnt.setFont(fnt);
    pnt.drawText(rect, Qt::AlignCenter, text);
}

/* A part's key as the window writes it, or a dash for none. */
QString keyName(colecosession *sess, int target)
{
    const coleco_binding bind = colecosession_binding_get(sess, target);
    char name[48] = "";
    if (bind.keysym) colecosession_keysym_name(bind.keysym, name, sizeof name);
    return local(name);
}

/* The legend on a part: long key names shortened to fit (tooltips keep the full name). */
QString legendName(QString key)
{
    if (key.startsWith(QLatin1String("Keypad "))) return QStringLiteral("KP ") + key.mid(7);
    if (key.startsWith(QLatin1String("Left "))) return QStringLiteral("L ") + key.mid(5);
    if (key.startsWith(QLatin1String("Right "))) return QStringLiteral("R ") + key.mid(6);
    if (key == QLatin1String("Backspace")) return QStringLiteral("BkSp");
    if (key == QLatin1String("Escape")) return QStringLiteral("Esc");
    return key;
}

QString bindingText(colecosession *sess, int target)
{
    const QString key = keyName(sess, target);
    return key.isEmpty() ? QStringLiteral("—") : legendName(key);
}

QString bindingTip(colecosession *sess, int target)
{
    const coleco_binding bind = colecosession_binding_get(sess, target);
    const QString key = keyName(sess, target);
    const QString pad = bind.button != COLECO_PAD_BTN_NONE ? local(coleco_pad_button_name(bind.button))
                                                           : QStringLiteral("none");
    return QStringLiteral("%1\nKey: %2    Gamepad: %3")
        .arg(local(coleco_target_name(target)), key.isEmpty() ? QStringLiteral("none") : key, pad);
}

QPointF unit(qreal deg)   /* a direction, degrees counter-clockwise from 3 o'clock, as drawn */
{
    return QPointF(std::cos(qDegreesToRadians(deg)), -std::sin(qDegreesToRadians(deg)));
}

QColor mixWhite(const QColor &base, qreal part)
{
    return QColor(int(base.red() + (255 - base.red()) * part), int(base.green() + (255 - base.green()) * part),
                  int(base.blue() + (255 - base.blue()) * part));
}

/* ---- ElidedLabel ------------------------------------------------------------ */

/* A label that never widens the window: one line elides, more wrap into a
 * fixed height. */
class ElidedLabel : public QWidget {
public:
    explicit ElidedLabel(bool dim, int lines = 1) : m_dim(dim), m_lines(lines)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    void setText(const QString &text)
    {
        if (text == m_text) return;
        m_text = text;
        update();
    }
    QSize sizeHint() const override { return QSize(0, fontMetrics().lineSpacing() * m_lines); }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setPen(m_dim ? QColor(Qt::gray) : palette().color(QPalette::WindowText));
        if (m_lines > 1)
            pnt.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, m_text);
        else
            pnt.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(m_text, Qt::ElideRight, width()));
    }

private:
    QString m_text;
    bool m_dim;
    int m_lines;
};

/* ---- ConsoleWidget: the two controllers and the console strip ------------------------- */

class ConsoleWidget : public QWidget {
public:
    /* onPick: a part was chosen in Map mode (its target); onHint: a message for the hint line */
    ConsoleWidget(colecosession *session, std::function<void(int)> onPick, std::function<void(const QString &)> onHint)
        : m_session(session), m_onPick(std::move(onPick)), m_onHint(std::move(onHint))
    {
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        load();
    }

    /* Lets go of everything the mouse holds (also on focus loss and rebuild). */
    void releaseAll()
    {
        setPointer(0, 0);
        setPointer(1, 0);
        m_hold = None;
        m_holdIdx = -1;
        update();
    }

    /* -2 idle, -1 armed, >= 0 waiting for a key or pad button for that target */
    void setMap(int state)
    {
        m_map = state;
        update();
    }
    void setShowBindings(bool visible)
    {
        m_showBindings = visible;
        update();
    }

    /* What the console sees held (keyboard, gamepads, the mouse), and the
     * stick caps easing toward it; repaints only on a change. */
    void pollHeld()
    {
        bool changed = false;
        for (int port = 0; port < COLECO_PORTS; ++port) {
            const unsigned held = colecosession_buttons_held(m_session, port);
            if (held != m_held[port]) {
                m_held[port] = held;
                changed = true;
            }
            if (easeCap(port)) changed = true;
        }
        if (changed) update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setRenderHint(QPainter::Antialiasing);
        paintCase(pnt);
        for (int port = 0; port < COLECO_PORTS; ++port) paintController(pnt, port);
        for (int sysact = 0; sysact < COLECO_SYSACT_COUNT; ++sysact) paintStrip(pnt, sysact);
    }

    void mousePressEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        const QPointF pos = evt->position();
        if (m_map != -2) {
            pick(pos);
            return;
        }
        if (m_hold != None) return;          /* one thing at a time */
        const int strip = stripAt(pos);
        if (strip >= 0) {
            m_hold = Strip;
            m_holdIdx = strip;
            m_stripOn = true;
            update();
            return;
        }
        const int idx = hitAt(pos);
        if (idx < 0) return;
        const coleco_control &ctl = m_layout[idx];
        switch (ctl.kind) {
        case COLECO_CTL_KEY:
        case COLECO_CTL_FIRE:
            m_hold = Part;
            m_holdIdx = idx;
            setPointer(ctl.port, ctl.act_mask);
            break;
        case COLECO_CTL_STICK:
        case COLECO_CTL_DIRECTION:
            m_hold = Disc;
            m_dragPort = ctl.port;
            setPointer(ctl.port, discMask(ctl.port, pos));
            break;
        default: return;
        }
        update();
    }

    /* Dragging off a key or a fire button lets it go (and back on, holds it
     * again); on the stick it moves from one direction to the next. */
    void mouseMoveEvent(QMouseEvent *evt) override
    {
        const QPointF pos = evt->position();
        const int hover = m_hold == None ? hoverAt(pos) : -1;
        if (hover != m_hover) {
            m_hover = hover;
            update();
        }
        if (m_hold == Disc) {
            setPointer(m_dragPort, discMask(m_dragPort, pos));
            update();
        } else if (m_hold == Part) {
            const bool over = hitAt(pos) == m_holdIdx;
            const coleco_control &ctl = m_layout[m_holdIdx];
            setPointer(ctl.port, over ? ctl.act_mask : 0);
        } else if (m_hold == Strip) {
            m_stripOn = stripAt(pos) == m_holdIdx;
            update();
        }
    }

    void mouseReleaseEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        if (m_hold == Strip && stripAt(evt->position()) == m_holdIdx) {
            /* fires on release, like a real button: dragging off cancels. The
             * main window's queue runs it, as it does for a key or a pad. */
            colecosession_sysaction_post(m_session, m_holdIdx);
        }
        if (m_hold != None) releaseAll();
        m_hover = hoverAt(evt->position());
        update();
    }

    void leaveEvent(QEvent *evt) override
    {
        if (m_hover != -1) {
            m_hover = -1;
            update();
        }
        QWidget::leaveEvent(evt);
    }

    bool event(QEvent *evt) override
    {
        if (evt->type() == QEvent::ToolTip) {
            auto *help = static_cast<QHelpEvent *>(evt);
            const QString tip = tipAt(help->pos());
            if (tip.isEmpty())
                QToolTip::hideText();
            else
                QToolTip::showText(help->globalPos(), tip, this);
            return true;
        }
        return QWidget::event(evt);
    }

private:
    enum Hold { None, Disc, Part, Strip };
    static constexpr int kStripBase = 1000;   /* m_hover value of a strip button, plus its sysaction */

    /* one controller's parts, by index into the layout */
    struct Pad {
        int caption = -1, overlay = -1, stick = -1;
        int dir[8];
        int fire[2] = { -1, -1 };
        int keycap[COLECO_KEYPAD_KEYS];
        QPointF centre;
        qreal radius = 0;
        QRectF body;
    };

    void load()
    {
        float wide = 0, tall = 0;
        m_layout = colecosession_controls_layout(&m_count, &wide, &tall);
        for (Pad &pad : m_pad) {
            for (int &slot : pad.dir) slot = -1;
            for (int &slot : pad.keycap) slot = -1;
        }
        int nextDir[COLECO_PORTS] = {};
        int nextKey[COLECO_PORTS] = {};
        for (int idx = 0; idx < m_count; ++idx) {
            const coleco_control &ctl = m_layout[idx];
            if (ctl.port < 0 || ctl.port >= COLECO_PORTS) continue;
            Pad &pad = m_pad[ctl.port];
            switch (ctl.kind) {
            case COLECO_CTL_CAPTION: pad.caption = idx; break;
            case COLECO_CTL_OVERLAY: pad.overlay = idx; break;
            case COLECO_CTL_STICK: pad.stick = idx; break;
            case COLECO_CTL_DIRECTION:
                if (nextDir[ctl.port] < 8) pad.dir[nextDir[ctl.port]++] = idx;
                break;
            case COLECO_CTL_FIRE: pad.fire[(ctl.act_mask & (1u << COLECO_ACT_FIRE_L)) ? 0 : 1] = idx; break;
            case COLECO_CTL_KEY:
                if (nextKey[ctl.port] < COLECO_KEYPAD_KEYS) pad.keycap[nextKey[ctl.port]++] = idx;
                break;
            default: break;
            }
        }
        for (Pad &pad : m_pad) {
            if (pad.stick < 0 || pad.caption < 0) continue;
            const QRectF disc = rectOf(pad.stick);
            pad.centre = disc.center();
            pad.radius = disc.width() / 2;
            /* the body sits inside the fire buttons, which straddle its edges */
            const QRectF cap = rectOf(pad.caption);
            pad.body = QRectF(cap.left() + 0.35 * kUnit, cap.bottom(), cap.width() - 0.7 * kUnit,
                              (tall - 0.1 - m_layout[pad.caption].height) * kUnit);
        }
        m_caseH = kTop + qCeil(tall * kUnit) + kCasePad;
        m_panelW = qCeil(wide * kUnit) + 2 * kCasePad;
        m_panelH = m_caseH + kStripGap + kStripH + kStripTail;
        setFixedSize(m_panelW, m_panelH);
    }

    QRectF rectOf(int idx) const
    {
        const coleco_control &ctl = m_layout[idx];
        return QRectF(kCasePad + ctl.xpos * kUnit, kTop + ctl.ypos * kUnit, ctl.width * kUnit, ctl.height * kUnit);
    }

    int hitAt(const QPointF &pos) const
    {
        return colecosession_controls_hit(float((pos.x() - kCasePad) / kUnit), float((pos.y() - kTop) / kUnit));
    }

    /* the console strip's button under pos (its sysaction), or -1 */
    int stripAt(const QPointF &pos) const
    {
        for (int sysact = 0; sysact < COLECO_SYSACT_COUNT; ++sysact)
            if (stripRect(sysact).contains(pos)) return sysact;
        return -1;
    }

    /* what the pointer is over, for the hover tint: a layout index, or kStripBase + a sysaction */
    int hoverAt(const QPointF &pos) const
    {
        const int strip = stripAt(pos);
        return strip >= 0 ? kStripBase + strip : hitAt(pos);
    }

    QRectF stripRect(int sysact) const
    {
        const qreal total = COLECO_SYSACT_COUNT * kStripW + (COLECO_SYSACT_COUNT - 1) * kStripSpace;
        return QRectF((width() - total) / 2 + sysact * (kStripW + kStripSpace), m_caseH + kStripGap, kStripW,
                      kStripH);
    }

    void setPointer(int port, unsigned mask)
    {
        if (port < 0 || port >= COLECO_PORTS || m_ptrMask[port] == mask) return;
        m_ptrMask[port] = mask;
        colecosession_pointer_set(m_session, port, mask);
    }

    /* The direction a pointer at pos holds on the stick it began on: nothing
     * near the middle, and past the rim still tilts that way. */
    unsigned discMask(int port, const QPointF &pos) const
    {
        const Pad &pad = m_pad[port];
        const QPointF delta = pos - pad.centre;
        return colecosession_stick_from_vector(float(delta.x()), float(delta.y()), float(pad.radius));
    }

    /* Map mode: a click on a part picks its target (a diagonal drives two
     * contacts, so it has none). */
    void pick(const QPointF &pos)
    {
        const int strip = stripAt(pos);
        if (strip >= 0) {
            m_onPick(COLECO_TARGET_SYSACT(strip));
            return;
        }
        const int idx = hitAt(pos);
        if (idx < 0) return;
        const coleco_control &ctl = m_layout[idx];
        if (ctl.target >= 0) {
            m_onPick(ctl.target);
        } else if (ctl.kind == COLECO_CTL_DIRECTION) {
            m_onHint(QStringLiteral("A diagonal holds two directions: map Up, Down, Left and Right."));
        } else if (ctl.kind == COLECO_CTL_STICK) {
            m_onHint(QStringLiteral("Map the stick's four wedges: Up, Down, Left and Right."));
        }
    }

    QString tipAt(const QPoint &pos) const
    {
        const int strip = stripAt(pos);
        if (strip >= 0) return bindingTip(m_session, COLECO_TARGET_SYSACT(strip));
        const int idx = hitAt(pos);
        if (idx < 0) return QString();
        const coleco_control &ctl = m_layout[idx];
        if (ctl.target >= 0) return bindingTip(m_session, ctl.target);
        switch (ctl.kind) {
        case COLECO_CTL_STICK: return QStringLiteral("Drag to tilt the stick");
        case COLECO_CTL_DIRECTION:
            return QStringLiteral("%1 (a diagonal holds two directions)").arg(local(ctl.label));
        default: return QString();
        }
    }

    bool mapping() const { return m_map != -2; }
    bool showBind() const { return mapping() || m_showBindings; }

    /* A part's lit state: held by anyone, or the one Map picked. */
    bool lit(const coleco_control &ctl) const
    {
        if (mapping()) return ctl.target >= 0 && ctl.target == m_map;
        switch (ctl.kind) {
        case COLECO_CTL_KEY:
        case COLECO_CTL_FIRE: return (m_held[ctl.port] & ctl.act_mask) != 0;
        case COLECO_CTL_DIRECTION: return (m_held[ctl.port] & ctl.act_mask) == ctl.act_mask;
        default: return false;
        }
    }

    /* While Map waits for a key, every part but the picked one steps back. */
    bool dimmed(const coleco_control &ctl) const { return m_map >= 0 && ctl.target != m_map; }

    /* The cap eases toward the held tilt (0.4 per tick, snapping when close). */
    bool easeCap(int port)
    {
        const unsigned held = mapping() ? 0u : m_held[port];
        auto bit = [held](int act) { return int((held >> act) & 1u); };
        QPointF goal(bit(COLECO_ACT_RIGHT) - bit(COLECO_ACT_LEFT), bit(COLECO_ACT_DOWN) - bit(COLECO_ACT_UP));
        const qreal len = std::hypot(goal.x(), goal.y());
        if (len > 1) goal /= len;
        QPointF &cur = m_cap[port];
        if (cur == goal) return false;
        const QPointF next = cur + (goal - cur) * 0.4;
        cur = std::hypot(goal.x() - next.x(), goal.y() - next.y()) < 0.004 ? goal : next;
        return true;
    }

    /* ---- painting ---- */

    void paintCase(QPainter &pnt)
    {
        const QRectF body(0.5, 0.5, width() - 1.0, m_caseH - 1.0);
        QLinearGradient charcoal(0, 0, 0, m_caseH);
        charcoal.setColorAt(0, kCaseTop);
        charcoal.setColorAt(1, kCaseBottom);
        pnt.setPen(kCaseEdge);
        pnt.setBrush(charcoal);
        pnt.drawRoundedRect(body, 16, 16);

        /* the thin highlight along the top edge */
        pnt.setBrush(Qt::NoBrush);
        pnt.setPen(QPen(kCaseLight, 1.5));
        pnt.drawLine(QPointF(18, 1.5), QPointF(width() - 18, 1.5));

        /* the brand strip, with the accent under it */
        QPainterPath clip;
        clip.addRoundedRect(body, 16, 16);
        pnt.save();
        pnt.setClipPath(clip);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(kBrandBar);
        pnt.drawRect(QRectF(0, 0, width(), kBrandH));
        pnt.setBrush(colecoAccentColor());
        pnt.drawRect(QRectF(0, kBrandH - 2, width(), 2));
        pnt.restore();
        QFont brand = scaledFont(font(), 0.85, true);
        brand.setLetterSpacing(QFont::AbsoluteSpacing, 3);
        pnt.setFont(brand);
        pnt.setPen(mixWhite(colecoAccentColor(), 0.35));
        pnt.drawText(QRectF(0, 0, width(), kBrandH - 2), Qt::AlignCenter, QStringLiteral("COLECOVISION"));
    }

    /* an annular sector round the disc: direction sector * 45 degrees, a little under 45 wide */
    static QPainterPath wedgePath(const QPointF &centre, qreal rad0, qreal rad1, int sector)
    {
        const qreal start = sector * 45.0 - 22.5 + 1.0, sweep = 43.0;
        const QRectF outer(centre.x() - rad1, centre.y() - rad1, 2 * rad1, 2 * rad1);
        const QRectF inner(centre.x() - rad0, centre.y() - rad0, 2 * rad0, 2 * rad0);
        QPainterPath path;
        path.arcMoveTo(outer, start);
        path.arcTo(outer, start, sweep);
        path.arcTo(inner, start + sweep, -sweep);
        path.closeSubpath();
        return path;
    }

    void paintController(QPainter &pnt, int port)
    {
        const Pad &pad = m_pad[port];
        if (pad.stick < 0) return;

        /* the black body, the caption above it */
        QLinearGradient shell(pad.body.topLeft(), pad.body.bottomLeft());
        shell.setColorAt(0, kBodyTop);
        shell.setColorAt(1, kBodyBottom);
        pnt.setPen(QPen(kBodyEdge, 1));
        pnt.setBrush(shell);
        pnt.drawRoundedRect(pad.body, 14, 14);
        pnt.setPen(QPen(kBodyLight, 1));
        pnt.drawLine(pad.body.topLeft() + QPointF(14, 1.5), pad.body.topRight() + QPointF(-14, 1.5));
        pnt.setPen(kInk);
        pnt.setFont(scaledFont(font(), 0.9, true));
        pnt.drawText(rectOf(pad.caption), Qt::AlignCenter, local(m_layout[pad.caption].label));

        paintCard(pnt, pad);
        paintStick(pnt, pad, port);

        for (int slot = 0; slot < 2; ++slot)
            if (pad.fire[slot] >= 0) paintFire(pnt, pad.fire[slot]);
        for (int slot = 0; slot < COLECO_KEYPAD_KEYS; ++slot)
            if (pad.keycap[slot] >= 0) paintKey(pnt, pad.keycap[slot]);
    }

    /* the overlay card's frame behind the keys: a silver faceplate with its slot */
    void paintCard(QPainter &pnt, const Pad &pad)
    {
        if (pad.overlay < 0) return;
        const QRectF rect = rectOf(pad.overlay);
        QLinearGradient silver(rect.topLeft(), rect.bottomLeft());
        silver.setColorAt(0, kCardTop);
        silver.setColorAt(1, kCardBottom);
        pnt.setPen(QPen(kCardEdge, 1));
        pnt.setBrush(silver);
        pnt.drawRoundedRect(rect, 8, 8);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(kCardEdge);
        pnt.drawRoundedRect(QRectF(rect.left() + 10, rect.top() + 4, rect.width() - 20, 5), 2, 2);
        pnt.setBrush(kSlot);
        pnt.drawRoundedRect(QRectF(rect.left() + 11, rect.top() + 4, rect.width() - 22, 3), 1.5, 1.5);
    }

    void paintStick(QPainter &pnt, const Pad &pad, int port)
    {
        const qreal outer = pad.radius, inner = outer * 0.5;
        pnt.setPen(QPen(kPlateEdge, 1.5));
        pnt.setBrush(kPlate);
        pnt.drawEllipse(pad.centre, outer + 3, outer + 3);
        for (int sector = 0; sector < 8; ++sector) {
            if (pad.dir[sector] < 0) continue;
            const coleco_control &ctl = m_layout[pad.dir[sector]];
            const bool held = lit(ctl);
            const bool over = !held && m_hover == pad.dir[sector];
            pnt.setOpacity(dimmed(ctl) ? kDimAlpha : 1.0);
            pnt.setPen(QPen(held ? colecoAccentDarkColor() : kWedgeEdge, 1));
            pnt.setBrush(held ? colecoAccentColor() : (over ? kWedgeHover : kWedge));
            pnt.drawPath(wedgePath(pad.centre, inner + 1, outer, sector));
            const QPointF mid = pad.centre + unit(sector * 45.0) * ((inner + outer) / 2);
            if (showBind() && ctl.target >= 0) {
                pnt.setPen(held ? colecoAccentInk() : kInk);
                drawFitted(pnt, QRectF(mid.x() - 24, mid.y() - 13, 48, 26), bindingText(m_session, ctl.target),
                           scaledFont(font(), 0.7, true));
            } else {
                /* a chevron pointing out */
                const QPointF dir = unit(sector * 45.0), perp(-dir.y(), dir.x());
                const qreal size = sector % 2 ? 3.5 : 5;
                QPolygonF chevron;
                chevron << mid + dir * size << mid - dir * size + perp * (size * 1.3)
                        << mid - dir * size - perp * (size * 1.3);
                pnt.setPen(Qt::NoPen);
                pnt.setBrush(held ? colecoAccentInk() : kDimInk);
                pnt.drawPolygon(chevron);
            }
            pnt.setOpacity(1.0);
        }
        /* the hub the cap sits in, then the cap */
        pnt.setPen(QPen(kPlateEdge, 1.5));
        pnt.setBrush(kHub);
        pnt.drawEllipse(pad.centre, inner - 1, inner - 1);
        paintCap(pnt, pad, port);
    }

    /* the stick's cap from above: a short stub with a disc top, shifted toward the tilt */
    void paintCap(QPainter &pnt, const Pad &pad, int port)
    {
        const qreal capRad = pad.radius * 0.5 * 0.82;
        const QPointF tilt = m_cap[port];
        const bool tilted = std::hypot(tilt.x(), tilt.y()) > 0.01;
        const QPointF cen = pad.centre + tilt * (pad.radius * kCapReach);

        /* its shadow lengthens with the tilt */
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(QColor(0, 0, 0, 110));
        pnt.drawEllipse(cen + QPointF(1.5, 2.5) + tilt * 3.0, capRad + 1, capRad + 1);

        QRadialGradient dome(cen + QPointF(-capRad * 0.3, -capRad * 0.35), capRad * 1.5);
        dome.setColorAt(0, kCapLight);
        dome.setColorAt(1, kCapDark);
        pnt.setBrush(dome);
        pnt.setPen(QPen(tilted ? colecoAccentColor() : kWedgeEdge, tilted ? 2.5 : 2.0));
        pnt.drawEllipse(cen, capRad, capRad);

        /* the light crescent on its upper left */
        const qreal glow = capRad * 0.72;
        QPainterPath crescent;
        crescent.arcMoveTo(QRectF(cen.x() - glow, cen.y() - glow, 2 * glow, 2 * glow), 110);
        crescent.arcTo(QRectF(cen.x() - glow, cen.y() - glow, 2 * glow, 2 * glow), 110, 70);
        pnt.setBrush(Qt::NoBrush);
        pnt.setPen(QPen(QColor(0xA8, 0xA8, 0xB0, 150), 2.0, Qt::SolidLine, Qt::RoundCap));
        pnt.drawPath(crescent);
    }

    void paintFire(QPainter &pnt, int idx)
    {
        const coleco_control &ctl = m_layout[idx];
        const bool held = lit(ctl);
        const bool over = !held && m_hover == idx;
        const QRectF full = rectOf(idx).adjusted(1, 0, -1, 0);
        pnt.setOpacity(dimmed(ctl) ? kDimAlpha : 1.0);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(kFireSide);
        pnt.drawRoundedRect(full, 7, 7);
        pnt.setBrush(held ? colecoAccentColor() : (over ? kFireHover : kFireTop));
        pnt.setPen(held ? QPen(colecoAccentInk(), 2) : QPen(kBodyEdge, 0.8));
        pnt.drawRoundedRect(full.adjusted(1, 1, -1, -4), 6, 6);
        /* the real buttons are blank; the binding shows while mapping */
        if (showBind()) {
            pnt.setPen(colecoAccentInk());
            QString text = bindingText(m_session, ctl.target);
            text.replace(QLatin1Char(' '), QLatin1Char('\n'));      /* "Left Ctrl" in two lines */
            drawFitted(pnt, full.adjusted(1, 2, -1, -5), text, pixelFont(font(), 11, true));
        }
        pnt.setOpacity(1.0);
    }

    void paintKey(QPainter &pnt, int idx)
    {
        const coleco_control &ctl = m_layout[idx];
        const bool held = lit(ctl);
        const bool over = !held && m_hover == idx;
        const QRectF full = rectOf(idx);
        pnt.setOpacity(dimmed(ctl) ? kDimAlpha : 1.0);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(kKeySide);
        pnt.drawRoundedRect(full, 4, 4);
        const QRectF face = full.adjusted(1, 1, -1, -3);
        pnt.setBrush(held ? colecoAccentColor() : (over ? kKeyHover : kKeyTop));
        pnt.setPen(held ? QPen(colecoAccentInk(), 2) : QPen(kBodyEdge, 0.8));
        pnt.drawRoundedRect(face, 4, 4);
        pnt.setPen(held ? colecoAccentInk() : kKeyInk);
        if (showBind()) {
            /* the digit smaller, the key it is bound to below it */
            pnt.setFont(pixelFont(font(), 12, true));
            pnt.drawText(face.adjusted(0, 0, 0, -face.height() * 0.42), Qt::AlignCenter, local(ctl.label));
            drawFitted(pnt, face.adjusted(1, face.height() * 0.55, -1, 0), bindingText(m_session, ctl.target),
                       pixelFont(font(), 9, true));
        } else {
            pnt.setFont(pixelFont(font(), 15, true));
            pnt.drawText(face, Qt::AlignCenter, local(ctl.label));
        }
        pnt.setOpacity(1.0);
    }

    /* The console actions, along the bottom. */
    void paintStrip(QPainter &pnt, int sysact)
    {
        const int target = COLECO_TARGET_SYSACT(sysact);
        const bool down = m_hold == Strip && m_holdIdx == sysact && m_stripOn;
        const bool picked = mapping() && m_map == target;
        const bool over = !down && m_hover == kStripBase + sysact;
        const QRectF full = stripRect(sysact);
        pnt.setOpacity(m_map >= 0 && !picked ? kDimAlpha : 1.0);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(kStripSide);
        pnt.drawRoundedRect(full, 8, 8);
        const QRectF face = down ? full.adjusted(0, 3, 0, 0) : full.adjusted(0, 0, 0, -3);
        pnt.setBrush(down || picked ? colecoAccentColor() : (over ? kStripHover : kStripCap));
        pnt.setPen(down || picked ? QPen(colecoAccentInk(), 1.5) : QPen(kBodyEdge, 0.8));
        pnt.drawRoundedRect(face, 8, 8);
        pnt.setPen(down || picked ? colecoAccentInk() : kInk);
        QString text = local(coleco_target_short_name(target));
        if (showBind()) text += QStringLiteral("  ") + bindingText(m_session, target);
        drawFitted(pnt, face.adjusted(8, 0, -8, 0), text, scaledFont(font(), 0.9, true));
        pnt.setOpacity(1.0);
    }

    colecosession *m_session;
    std::function<void(int)> m_onPick;
    std::function<void(const QString &)> m_onHint;
    const coleco_control *m_layout = nullptr;
    int m_count = 0;
    Pad m_pad[COLECO_PORTS];
    unsigned m_held[COLECO_PORTS] = {};
    QPointF m_cap[COLECO_PORTS];         /* each stick cap's eased tilt, -1..1 */
    unsigned m_ptrMask[COLECO_PORTS] = {};
    bool m_showBindings = false;
    int m_map = -2;
    Hold m_hold = None;
    int m_holdIdx = -1;          /* the part the mouse holds (a sysaction for the strip) */
    int m_dragPort = -1;         /* the stick the mouse began on */
    bool m_stripOn = false;      /* the pointer is still over the strip button it pressed */
    int m_hover = -1;
    int m_caseH = 0;
    int m_panelW = 0, m_panelH = 0;
};

/* ---- ControllersWindow --------------------------------------------------------------- */

class ControllersWindow : public QWidget {
public:
    ControllersWindow(colecosession *session, QWidget *parent);
    void present();
    void refresh();

protected:
    void keyPressEvent(QKeyEvent *evt) override;
    void keyReleaseEvent(QKeyEvent *evt) override;
    void showEvent(QShowEvent *evt) override;
    void hideEvent(QHideEvent *evt) override;
    void changeEvent(QEvent *evt) override;
    /* Tab is a key of the console's, not focus traversal. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    void letGo();
    void setMapState(int state);
    void bound(int target, const QString &what, const char *stolen);
    void pollCapture();
    void pollSlow();
    void rebuildPads();
    void refreshPadPorts();

    colecosession *m_session;
    ConsoleWidget *m_console = nullptr;
    QWidget *m_padArea = nullptr;
    QVBoxLayout *m_padBox = nullptr;
    std::vector<QWidget *> m_padRows;
    std::vector<QLabel *> m_padEff;       /* each pad's "drives controller N" */
    unsigned m_padGen = ~0u;
    QPushButton *m_mapButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    ElidedLabel *m_hint = nullptr;
    QTimer m_captureTimer;
    QTimer m_heldTimer;
    QTimer m_slowTimer;
    uint32_t m_swallow = 0;               /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a target, >= 0 waiting for a key or
     * gamepad button for that target. */
    int m_mapState = -2;
};

ControllersWindow::ControllersWindow(colecosession *session, QWidget *parent)
    /* A dialog-type window, so a tiling compositor floats it: it is a panel
     * of fixed-size parts and nothing here wants more room. */
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint),
      m_session(session)
{
    setObjectName(QStringLiteral("controllers"));
    setWindowTitle(QStringLiteral("Controllers"));
    setProperty("colecoHelpContext", QStringLiteral("controllers"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    /* Sized to its parts and kept there: a resizable panel gets tiled and its
     * parts stretched. */
    root->setSizeConstraint(QLayout::SetFixedSize);

    m_console = new ConsoleWidget(
        session,
        [this](int target) { setMapState(target); },
        [this](const QString &text) { m_hint->setText(text); });
    root->addWidget(m_console, 0, Qt::AlignHCenter);

    /* Gamepads: one row per pad, rebuilt as they come and go, in room for a
     * fixed number so the window does not change size when one is plugged in */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads"));
    auto *padsBox = new QVBoxLayout(pads);
    m_padArea = new QWidget;
    m_padBox = new QVBoxLayout(m_padArea);
    m_padBox->setContentsMargins(0, 0, 0, 0);
    m_padBox->setSpacing(2);
    const int rowHeight = QComboBox().sizeHint().height() + 2;
    m_padArea->setFixedHeight(rowHeight * kMaxPadRows + m_padBox->spacing() * (kMaxPadRows - 1));
    m_padBox->setAlignment(Qt::AlignTop);
    padsBox->addWidget(m_padArea);
    root->addWidget(pads);

    auto *mapRow = new QHBoxLayout;
    m_mapButton = new QPushButton(QStringLiteral("Map"));
    m_mapButton->setFocusPolicy(Qt::NoFocus);
    m_mapButton->setFixedWidth(qMax(m_mapButton->sizeHint().width(),
                                    m_mapButton->fontMetrics().horizontalAdvance(QStringLiteral("Done")) + 32));
    m_mapButton->setToolTip(QStringLiteral("Pick a part, then press a key or a gamepad button to give it"));
    connect(m_mapButton, &QPushButton::clicked, this, [this] { setMapState(m_mapState == -2 ? -1 : -2); });
    m_clearButton = new QPushButton(QStringLiteral("Clear"));
    m_clearButton->setFocusPolicy(Qt::NoFocus);
    m_clearButton->setToolTip(QStringLiteral("Take the picked control's key and gamepad button away"));
    connect(m_clearButton, &QPushButton::clicked, this, [this] {
        if (m_mapState < 0) return;
        char stolen[128] = "";
        colecosession_binding_set_key(m_session, m_mapState, 0, stolen, sizeof stolen);
        colecosession_binding_set_button(m_session, m_mapState, COLECO_PAD_BTN_NONE, stolen, sizeof stolen);
        m_hint->setText(QStringLiteral("Cleared. Press a key or a gamepad button to bind it again"));
        m_console->update();
    });
    auto *defaults = new QPushButton(QStringLiteral("Defaults"));
    defaults->setFocusPolicy(Qt::NoFocus);
    defaults->setToolTip(QStringLiteral("Put every key and gamepad button back to its default"));
    connect(defaults, &QPushButton::clicked, this, [this] {
        colecosession_bindings_reset(m_session);
        setMapState(m_mapState == -2 ? -2 : -1);
        m_hint->setText(QStringLiteral("Every control is back to its default."));
    });
    auto *bindings = new QCheckBox(QStringLiteral("Show bindings"));
    bindings->setFocusPolicy(Qt::NoFocus);
    bindings->setToolTip(QStringLiteral("Show the key each part has (hover for its gamepad button)"));
    /* "controllers_show_bindings"; the older spellings are read when it is absent */
    int savedBindings = colecosession_get_int(session, "controllers_show_bindings", -1);
    if (savedBindings < 0) savedBindings = colecosession_get_int(session, "controllers_bindings", -1);
    if (savedBindings < 0) savedBindings = colecosession_get_int(session, "controllers_key_names", -1);
    if (savedBindings < 0) savedBindings = colecosession_get_int(session, "show_bindings", -1);
    bindings->setChecked(savedBindings > 0);
    connect(bindings, &QCheckBox::toggled, this, [this](bool checked) {
        colecosession_set_int(m_session, "controllers_show_bindings", checked ? 1 : 0);
        m_console->setShowBindings(checked);
    });
    m_console->setShowBindings(bindings->isChecked());
    mapRow->addWidget(m_mapButton);
    mapRow->addWidget(m_clearButton);
    mapRow->addWidget(defaults);
    mapRow->addWidget(bindings);
    mapRow->addStretch(1);
    mapRow->addWidget(colecoHelpButton(QStringLiteral("controllers")));
    root->addLayout(mapRow);
    m_hint = new ElidedLabel(true, 2);
    root->addWidget(m_hint);

    connect(&m_captureTimer, &QTimer::timeout, this, [this] { pollCapture(); });
    connect(&m_heldTimer, &QTimer::timeout, this, [this] { m_console->pollHeld(); });
    connect(&m_slowTimer, &QTimer::timeout, this, [this] { pollSlow(); });
    setMapState(-2);
    letGo();
    m_console->pollHeld();
}

/* Lets go of what the mouse holds in this window. */
void ControllersWindow::letGo()
{
    m_console->releaseAll();
}

/* ---- Map mode -------------------------------------------------------------------------- */

void ControllersWindow::setMapState(int state)
{
    m_mapState = state;
    colecoSetAccent(m_mapButton, state != -2);
    m_clearButton->setEnabled(state >= 0);
    if (state == -2) {
        m_captureTimer.stop();
        colecosession_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Map"));
        m_hint->setText(QStringLiteral("Press a part with the mouse to hold it. Drag on the stick to tilt it."));
    } else if (state == -1) {
        m_captureTimer.stop();
        colecosession_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Done"));
        m_hint->setText(QStringLiteral("Click a part to remap it. Escape leaves Map."));
    } else {
        letGo();
        m_hint->setText(QStringLiteral("Press a key or a gamepad button for %1 (Escape cancels).")
                            .arg(local(coleco_target_name(state))));
        colecosession_gamepad_capture_begin(m_session);
        m_captureTimer.start(50);
    }
    m_console->setMap(m_mapState);
    m_console->pollHeld();
}

/* A binding was made: stay armed, since remapping several in a row is normal. */
void ControllersWindow::bound(int target, const QString &what, const char *stolen)
{
    setMapState(-1);
    m_hint->setText(stolen[0] ? QStringLiteral("%1 is now %2 (taken from %3)")
                                    .arg(local(coleco_target_name(target)), what, local(stolen))
                              : QStringLiteral("%1 is now %2").arg(local(coleco_target_name(target)), what));
}

void ControllersWindow::pollCapture()
{
    int button;
    if (m_mapState < 0) { m_captureTimer.stop(); return; }
    if (!colecosession_gamepad_capture_poll(m_session, &button)) return;
    char stolen[128] = "";
    const int target = m_mapState;
    colecosession_binding_set_button(m_session, target, button, stolen, sizeof stolen);
    bound(target, local(coleco_pad_button_name(button)), stolen);
}

/* ---- polling ---------------------------------------------------------------------------- */

/* Hot-plugged pads and auto-assignment changes. */
void ControllersWindow::pollSlow()
{
    const unsigned gen = colecosession_gamepad_generation(m_session);
    if (gen != m_padGen) {
        m_padGen = gen;
        rebuildPads();
    } else {
        refreshPadPorts();
    }
}

/* ---- gamepads --------------------------------------------------------------------------- */

void ControllersWindow::refreshPadPorts()
{
    for (int idx = 0; idx < (int)m_padEff.size(); ++idx) {
        const int eff = colecosession_gamepad_effective_port(m_session, idx);
        const QString text = eff >= 0 ? QStringLiteral("drives controller %1").arg(eff + 1)
                                      : QStringLiteral("drives nothing");
        if (m_padEff[(size_t)idx]->text() != text) m_padEff[(size_t)idx]->setText(text);
    }
}

/* One row per pad: its name, the controller it drives and the one it is set to. */
void ControllersWindow::rebuildPads()
{
    for (QWidget *row : m_padRows) delete row;
    m_padRows.clear();
    m_padEff.clear();
    const int count = colecosession_gamepad_count(m_session);
    if (count == 0) {
        auto *none = new QLabel(QStringLiteral("No gamepads connected - plug one in"));
        none->setStyleSheet(QStringLiteral("color: gray;"));
        m_padBox->addWidget(none);
        m_padRows.push_back(none);
        return;
    }
    for (int idx = 0; idx < count && idx < kMaxPadRows; ++idx) {
        char name[128];
        colecosession_gamepad_name(m_session, idx, name, sizeof name);
        auto *row = new QWidget;
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        auto *label = new ElidedLabel(false);
        label->setText(local(name));
        label->setToolTip(local(name));
        auto *eff = new QLabel;
        eff->setStyleSheet(QStringLiteral("color: gray;"));
        /* -1 = automatic (connection order), 0-1 = that controller */
        auto *box = new QComboBox;
        box->setFocusPolicy(Qt::NoFocus);
        box->addItems({ QStringLiteral("Automatic"), QStringLiteral("Controller 1"), QStringLiteral("Controller 2") });
        const int assign = colecosession_gamepad_assignment(m_session, idx);
        box->setCurrentIndex(assign < 0 || assign > 1 ? 0 : assign + 1);
        connect(box, &QComboBox::currentIndexChanged, this, [this, idx](int sel) {
            colecosession_gamepad_assign(m_session, idx, sel <= 0 ? -1 : sel - 1);
            refreshPadPorts();
        });
        line->addWidget(label, 1);
        line->addWidget(eff);
        line->addWidget(box);
        m_padBox->addWidget(row);
        m_padRows.push_back(row);
        m_padEff.push_back(eff);
    }
    if (count > kMaxPadRows) {
        /* the rest are in Preferences ▸ Input */
        m_padEff.back()->setToolTip(QStringLiteral("%1 more gamepads are listed in Preferences").arg(count - kMaxPadRows));
    }
    refreshPadPorts();
}

/* ---- the window ------------------------------------------------------------------------- */

void ControllersWindow::present()
{
    m_padGen = colecosession_gamepad_generation(m_session);
    rebuildPads();
    show();
    raise();
}

void ControllersWindow::refresh()
{
    if (isVisible()) {
        letGo();
        m_console->pollHeld();
    }
}

void ControllersWindow::showEvent(QShowEvent *evt)
{
    m_console->pollHeld();
    m_heldTimer.start(20);
    m_slowTimer.start(250);
    QWidget::showEvent(evt);
}

void ControllersWindow::hideEvent(QHideEvent *evt)
{
    m_heldTimer.stop();
    m_slowTimer.stop();
    setMapState(-2);
    letGo();
    QWidget::hideEvent(evt);
}

/* Only what this window holds: the main window releases its own keys. */
void ControllersWindow::changeEvent(QEvent *evt)
{
    if (evt->type() == QEvent::ActivationChange && !isActiveWindow()) {
        letGo();
        colecosession_release_all(m_session);
        m_swallow = 0;
    }
    QWidget::changeEvent(evt);
}

void ControllersWindow::keyPressEvent(QKeyEvent *evt)
{
    if (evt->key() == Qt::Key_F9) {
        if (!evt->isAutoRepeat()) hide();
        return;
    }
    if (evt->key() == Qt::Key_F12) {
        if (!evt->isAutoRepeat()) DebuggerWindow::showFor(parentWidget(), m_session);
        return;
    }
    const uint32_t keysym = colecoKeysymFromQt(evt);
    if (m_swallow && keysym == m_swallow) return;

    if (m_mapState != -2) {
        if (evt->isAutoRepeat()) return;
        /* Esc backs out one step: a pending target, then Map itself */
        if (evt->key() == Qt::Key_Escape) {
            m_swallow = keysym;
            setMapState(m_mapState >= 0 ? -1 : -2);
            return;
        }
        if (m_mapState < 0 || !keysym) return;      /* armed, or nothing to bind: nothing is typed */
        if (colecoIsAppKey(evt)) {
            m_hint->setText(QStringLiteral("F9 and F12 are the app's and cannot be bound."));
            return;
        }
        char stolen[128] = "", name[48] = "";
        const int target = m_mapState;
        colecosession_binding_set_key(m_session, target, keysym, stolen, sizeof stolen);
        colecosession_keysym_name(keysym, name, sizeof name);
        m_swallow = keysym;
        bound(target, local(name), stolen);
        return;
    }
    /* Otherwise what the main window does with it; a console action goes to
     * the main window's queue, as a gamepad's does. */
    if (evt->isAutoRepeat()) return;
    if (!keysym || colecoIsAppKey(evt)) { QWidget::keyPressEvent(evt); return; }
    const int sysact = colecosession_key_sysaction(m_session, keysym);
    if (sysact >= 0) {
        colecosession_sysaction_post(m_session, sysact);
        return;
    }
    if (!colecosession_key(m_session, keysym, 1)) QWidget::keyPressEvent(evt);
}

void ControllersWindow::keyReleaseEvent(QKeyEvent *evt)
{
    if (evt->isAutoRepeat()) return;
    const uint32_t keysym = colecoKeysymFromQt(evt);
    if (m_swallow && keysym == m_swallow) {
        m_swallow = 0;
        return;
    }
    if (m_mapState != -2 || !keysym) return;
    if (!colecosession_key(m_session, keysym, 0)) QWidget::keyReleaseEvent(evt);
}

QPointer<ControllersWindow> g_window;

} // namespace

void colecoControllersToggle(QWidget *parent, colecosession *session)
{
    if (!g_window) g_window = new ControllersWindow(session, parent);
    if (g_window->isVisible()) g_window->hide();
    else g_window->present();
}

void colecoControllersShow(QWidget *parent, colecosession *session)
{
    if (!g_window) g_window = new ControllersWindow(session, parent);
    if (!g_window->isVisible()) g_window->present();
    else g_window->raise();
}

bool colecoControllersVisible()
{
    return g_window && g_window->isVisible();
}

void colecoControllersRefresh()
{
    if (g_window) g_window->refresh();
}
