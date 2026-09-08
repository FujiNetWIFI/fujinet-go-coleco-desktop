/*
 * MainWindow -- see MainWindow.h.
 *
 * Plain Qt6 Widgets, deliberately not KDE Frameworks: it picks up Breeze
 * through the platform theme anyway, and staying framework-free keeps this
 * frontend usable outside a KDE session.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "MainWindow.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>

#include "SettingsDialog.h"
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QStatusBar>
#include <QUrl>

#include "DisplayWidget.h"
#include "KeyForward.h"
#include "KeypadWindow.h"
#include "debugger/DebuggerWindow.h"

MainWindow::MainWindow(colecosession *session, QWidget *parent)
    : QMainWindow(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("FujiNet Go ColecoVision"));
    /* 256x212 at 3x, in 4:3 -- big enough that the BIOS text is readable
     * without resizing on every launch. */
    resize(848, 636);
    setAcceptDrops(true);
    coleco_input_reset(&m_keys);

    m_display = new DisplayWidget(session, this);
    m_display->setTvAspect(colecosession_get_int(session, "tv_aspect", 1) != 0);
    m_display->setSmooth(colecosession_get_int(session, "smooth", 0) != 0);
    setCentralWidget(m_display);

    m_status = new QLabel(QStringLiteral("Starting..."));
    statusBar()->addWidget(m_status);

    buildMenus();

    connect(&m_statusTimer, &QTimer::timeout, this, &MainWindow::updateStatus);
    m_statusTimer.start(1000);
    updateStatus();

    /* COLECO_OPEN_KEYPAD / COLECO_OPEN_DEBUGGER, the same launch hooks the
     * GNOME frontend has and for the same reason: they are the way in when
     * the app misbehaves before a menu is reachable. */
    if (qEnvironmentVariableIsSet("COLECO_OPEN_KEYPAD"))
        toggleKeypad();
    if (qEnvironmentVariableIsSet("COLECO_OPEN_DEBUGGER"))
        DebuggerWindow::showFor(this, session);

    if (!colecosession_bios_available(session))
        statusBar()->showMessage(
            QStringLiteral("No ColecoVision BIOS — use Machine ▸ Import BIOS"),
            0);
}

void MainWindow::buildMenus()
{
    QMenu *machine = menuBar()->addMenu(QStringLiteral("&Machine"));
    machine->addAction(QStringLiteral("&Open Cartridge..."), this, [this] {
        const QString f = QFileDialog::getOpenFileName(
            this, QStringLiteral("Open Cartridge"), QString(),
            QStringLiteral("ColecoVision cartridges (*.rom *.col *.bin);;All files (*)"));
        if (!f.isEmpty()) loadMedia(f);
    });
    machine->addAction(QStringLiteral("&Eject Cartridge"), this, [this] {
        colecosession_set_str(m_session, "cart_path", "");
        colecosession_settings_flush(m_session);
        colecosession_reset_to_config(m_session);
    });
    machine->addSeparator();
    machine->addAction(QStringLiteral("&Reset Console"), this,
                       [this] { colecosession_reset(m_session); });
    machine->addAction(QStringLiteral("Reset to &CONFIG"), this,
                       [this] { colecosession_reset_to_config(m_session); });
    machine->addSeparator();
    machine->addAction(QStringLiteral("&Import BIOS..."), this, [this] {
        const QString f = QFileDialog::getOpenFileName(
            this, QStringLiteral("Import ColecoVision BIOS (OS7.rom)"));
        if (f.isEmpty()) return;
        if (colecosession_import_bios(m_session, f.toLocal8Bit().constData()) < 0)
            QMessageBox::warning(this, QStringLiteral("Import failed"),
                QString::fromUtf8(colecosession_last_error(m_session)));
        else
            statusBar()->showMessage(
                QStringLiteral("BIOS imported. Restart to boot it."), 5000);
    });
    machine->addSeparator();
    /* Ctrl+comma spelled out rather than QKeySequence::Preferences: that
     * standard key is empty on X11/Wayland (it is a macOS binding), so the
     * menu item would have shown no shortcut and the accelerator would
     * simply never have fired. */
    machine->addAction(QStringLiteral("&Preferences..."),
                       QKeySequence(Qt::CTRL | Qt::Key_Comma), this,
                       &MainWindow::showSettings);
    machine->addSeparator();
    machine->addAction(QStringLiteral("&Quit"), this, [this] { close(); });

    QMenu *view = menuBar()->addMenu(QStringLiteral("&View"));
    view->addAction(QStringLiteral("&Controllers"), QKeySequence(Qt::Key_F9),
                    this, &MainWindow::toggleKeypad);
    view->addAction(QStringLiteral("&Debugger"), QKeySequence(Qt::Key_F12),
                    this, [this] { DebuggerWindow::showFor(this, m_session); });
    view->addSeparator();
    QAction *tv = view->addAction(QStringLiteral("&TV Aspect (4:3)"));
    tv->setCheckable(true);
    tv->setChecked(colecosession_get_int(m_session, "tv_aspect", 1) != 0);
    connect(tv, &QAction::toggled, this, [this](bool on) {
        m_display->setTvAspect(on);
        colecosession_set_int(m_session, "tv_aspect", on ? 1 : 0);
    });
    QAction *sm = view->addAction(QStringLiteral("&Smooth Scaling"));
    sm->setCheckable(true);
    sm->setChecked(colecosession_get_int(m_session, "smooth", 0) != 0);
    connect(sm, &QAction::toggled, this, [this](bool on) {
        m_display->setSmooth(on);
        colecosession_set_int(m_session, "smooth", on ? 1 : 0);
    });

    QMenu *fuji = menuBar()->addMenu(QStringLiteral("&FujiNet"));
    fuji->addAction(QStringLiteral("&Configuration"), this, [this] {
        if (!colecosession_fujinet_running(m_session)) {
            statusBar()->showMessage(QStringLiteral("FujiNet is not running"),
                                     4000);
            return;
        }
        QDesktopServices::openUrl(QUrl(QString::fromUtf8(
            colecosession_fujinet_webui_url(m_session))));
    });
}

void MainWindow::showSettings()
{
    if (SettingsDialog::run(this, m_session))
        restartSession();
}

/* Stop, re-read the settings store, start. Every option the Preferences
 * dialog writes is read by colecosession_default_opts(), so this is the only
 * way any of them can take effect. */
void MainWindow::restartSession()
{
    colecosession_start_opts o;
    colecosession_settings_flush(m_session);
    colecosession_default_opts(m_session, &o);
    colecosession_stop(m_session);
    if (colecosession_start(m_session, &o) != 0) {
        QMessageBox::warning(
            this, QStringLiteral("Restart failed"),
            QString::fromUtf8(colecosession_last_error(m_session)));
        return;
    }
    statusBar()->showMessage(
        QStringLiteral("Machine options applied (session restarted)"), 5000);
}

void MainWindow::toggleKeypad()
{
    if (!m_keypad) m_keypad = new KeypadWindow(m_session, this);
    /* Hide, do not destroy: a remap in progress and the window's position
     * both survive closing it. */
    if (m_keypad->isVisible()) m_keypad->hide();
    else m_keypad->show();
}

void MainWindow::updateStatus()
{
    const char *text;
    if (!colecosession_is_running(m_session))            text = "Stopped";
    else if (!colecosession_cart_mailbox_live(m_session)) text = "No FujiNet cartridge";
    else if (colecosession_cart_link_up(m_session))       text = "FujiNet connected";
    else                                                  text = "FujiNet: link down";
    m_status->setText(QString::fromUtf8(text));
}

void MainWindow::loadMedia(const QString &path)
{
    char dest[1024];
    if (colecosession_import_media(m_session, path.toLocal8Bit().constData(),
                                   dest, sizeof dest) != 0) {
        QMessageBox::warning(this, QStringLiteral("Import failed"),
            QString::fromUtf8(colecosession_last_error(m_session)));
        return;
    }
    if (!colecosession_media_is_cartridge(dest)) {
        statusBar()->showMessage(
            QStringLiteral("Copied to FujiNet's SD folder — mount it from the "
                           "CONFIG client"), 6000);
        return;
    }
    colecosession_set_str(m_session, "cart_path", dest);
    colecosession_settings_flush(m_session);

    colecosession_start_opts o;
    colecosession_default_opts(m_session, &o);
    colecosession_stop(m_session);
    if (colecosession_start(m_session, &o) != 0)
        QMessageBox::warning(this, QStringLiteral("Could not start"),
            QString::fromUtf8(colecosession_last_error(m_session)));
}

void MainWindow::pushInput()
{
    /* Push both ports every time: the controller word is a snapshot of
     * everything held, so sending only the port that changed would be an
     * optimisation with a bug in it the first time keys on both are down. */
    colecosession_joystick_raw(m_session, 0, coleco_input_word(&m_keys, 0));
    colecosession_joystick_raw(m_session, 1, coleco_input_word(&m_keys, 1));
}

void MainWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat()) return;
    if (e->key() == Qt::Key_F9) { toggleKeypad(); return; }
    if (e->key() == Qt::Key_F12) {
        DebuggerWindow::showFor(this, m_session);
        return;
    }

    const uint32_t ks = colecoKeysymFromQt(e);
    if (!ks) { QMainWindow::keyPressEvent(e); return; }

    const int sa = coleco_input_key_sysaction(ks);
    if (sa >= 0) { colecosession_sysaction(m_session, sa); return; }
    if (coleco_input_key(&m_keys, ks, 1)) pushInput();
    else QMainWindow::keyPressEvent(e);
}

void MainWindow::keyReleaseEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat()) return;
    const uint32_t ks = colecoKeysymFromQt(e);
    if (ks && coleco_input_key(&m_keys, ks, 0)) pushInput();
    else QMainWindow::keyReleaseEvent(e);
}

bool MainWindow::event(QEvent *e)
{
    if (e->type() == QEvent::WindowDeactivate) {
        coleco_input_reset(&m_keys);
        pushInput();
    }
    return QMainWindow::event(e);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    const QList<QUrl> urls = e->mimeData()->urls();
    if (urls.isEmpty()) return;
    const QString path = urls.first().toLocalFile();
    if (!path.isEmpty()) loadMedia(path);
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    m_statusTimer.stop();
    QMainWindow::closeEvent(e);
}
