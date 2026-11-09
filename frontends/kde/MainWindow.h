/*
 * The main window.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <functional>

#include <QLabel>
#include <QMainWindow>
#include <QTimer>

#include "colecosession.h"

class DisplayView;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(colecosession *session, QWidget *parent = nullptr);

    /* Starts the session (a cartridge given on the command line, else the
     * saved one). A refusal because there is no BIOS offers the import and
     * starts again with it; any other is a plain warning. */
    bool start(const QString &cartPath = QString());

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
    void updateStatus();
    /* the news the gamepad thread leaves: console actions and hot-plugs */
    void tick();
    void drainSysactions();
    void runSysaction(int sysact);
    void pollGamepads();
    void loadMedia(const QString &path);
    void toggleControllers();
    void useSoftwareDisplay();
    void applyPicture();
    void showSettings();
    void showAbout();
    void restartSession();
    void explainRefusal(const QString &title, const QString &why,
                        std::function<void()> retry);
    void importRoms(bool folder, const QString &path = QString());
    void afterImport(bool wasMissing);

    colecosession *m_session;
    DisplayView *m_display = nullptr;
    QLabel *m_status = nullptr;
    QTimer m_statusTimer;
    QTimer m_tickTimer;
    unsigned m_padGen = 0;
    bool m_sysactDown[COLECO_SYSACT_COUNT] = {};
};
