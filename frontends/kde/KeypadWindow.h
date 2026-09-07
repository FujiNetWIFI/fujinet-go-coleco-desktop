/*
 * The clickable controller panel: both hand controllers side by side.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QPushButton>
#include <QWidget>
#include <vector>

#include "colecosession.h"

/* A button that reports press and release, not "clicked". A keypad key is
 * HELD: the emulator samples the controller once per frame, so a value
 * present only for the instant of a click falls between frames and a polling
 * game -- which is how essentially every ColecoVision title reads its skill
 * select -- never sees it. */
class PadButton : public QPushButton {
    Q_OBJECT
public:
    PadButton(const QString &face, int target, QWidget *parent = nullptr);
    int target() const { return m_target; }
    QString face() const { return m_face; }
signals:
    void pressedTarget(int target);
    void releasedTarget(int target);
protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;
private:
    int m_target;
    QString m_face;
    bool m_down = false;
};

class KeypadWindow : public QWidget {
    Q_OBJECT
public:
    explicit KeypadWindow(colecosession *session, QWidget *parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;

private:
    QWidget *buildController(int port);
    void onPressed(int target);
    void onReleased(int target);
    void setMapState(int state);
    void refreshLabels();

    colecosession *m_session;
    std::vector<PadButton *> m_controls;
    QPushButton *m_mapButton = nullptr;
    class QLabel *m_hint = nullptr;
    coleco_input_state m_keys{};
    /* -2 idle, -1 armed and waiting for a target, >=0 waiting for a key. */
    int m_mapState = -2;
};
