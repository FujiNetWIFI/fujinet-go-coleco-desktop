/*
 * The main window.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QLabel>
#include <QMainWindow>
#include <QTimer>

#include "colecosession.h"

class DisplayWidget;
class KeypadWindow;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(colecosession *session, QWidget *parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    /* Losing focus with keys held would leave the machine believing they are
     * still down -- alt-tabbing mid-jump and coming back to a character
     * walking into a wall is the classic symptom. */
    bool event(QEvent *e) override;

private:
    void buildMenus();
    void pushInput();
    void updateStatus();
    void loadMedia(const QString &path);
    void toggleKeypad();

    colecosession *m_session;
    DisplayWidget *m_display = nullptr;
    KeypadWindow *m_keypad = nullptr;
    QLabel *m_status = nullptr;
    QTimer m_statusTimer;
    coleco_input_state m_keys{};
};
