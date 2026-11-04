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
#include <QPushButton>

#include "FujiNetLogWindow.h"
#include "SettingsDialog.h"
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QStatusBar>
#include <QUrl>

#include "DisplayWidget.h"
#include "SoftDisplayWidget.h"
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

    auto *gpu = new DisplayWidget(session, this);
    m_display = gpu;
    applyPicture();
    setCentralWidget(gpu);
    /* No usable GPU device: fall back to painting in software */
    connect(gpu, &QRhiWidget::renderFailed, this, &MainWindow::useSoftwareDisplay,
            Qt::QueuedConnection);

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
    /* COLECO_OPEN_SETTINGS opens Preferences once the event loop runs (the
     * dialog is modal). Modifier shortcuts cannot be injected into a Qt
     * Wayland window, so this is how a headless check reaches it. */
    if (qEnvironmentVariableIsSet("COLECO_OPEN_SETTINGS"))
        QTimer::singleShot(0, this, &MainWindow::showSettings);
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
    machine->addAction(QStringLiteral("Import &BIOS..."), this,
                       [this] { importRoms(false); });
    machine->addAction(QStringLiteral("Import BIOS from a &Folder..."), this,
                       [this] { importRoms(true); });
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
        if (colecosession_fujinet_external(m_session)) {
            statusBar()->showMessage(
                QStringLiteral("FujiNet is external: use its own web UI"),
                4000);
            return;
        }
        if (!colecosession_fujinet_running(m_session)) {
            statusBar()->showMessage(QStringLiteral("FujiNet is not running"),
                                     4000);
            return;
        }
        QDesktopServices::openUrl(QUrl(QString::fromUtf8(
            colecosession_fujinet_webui_url(m_session))));
    });
    fuji->addAction(QStringLiteral("Console &Log"), this, [this] {
        if (colecosession_fujinet_external(m_session)) {
            statusBar()->showMessage(
                QStringLiteral("FujiNet is external: see its own console"),
                4000);
            return;
        }
        fujinet_log_show(this, m_session);
    });
}

void MainWindow::showSettings()
{
    if (SettingsDialog::run(this, m_session))
        restartSession();
}

/* ---- the BIOS ------------------------------------------------------------ */

/* The session's refusals that an import cures all say "Import BIOS". */
void MainWindow::explainRefusal(const QString &title, const QString &why,
                                std::function<void()> retry)
{
    if (!why.contains(QStringLiteral("Import BIOS"))) {
        QMessageBox::warning(this, title, why);
        return;
    }
    /* not exec(): a nested event loop could be re-entered from a timer */
    auto *box = new QMessageBox(
        QMessageBox::Information, title,
        QStringLiteral("The ColecoVision BIOS is needed"),
        QMessageBox::NoButton, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setInformativeText(QStringLiteral(
        "The console runs on its OS-7 BIOS, which is not part of this app. "
        "Import your own copy (a file, a zip such as MAME's coleco.zip, or a "
        "folder) and the console starts."));
    QPushButton *import = box->addButton(QStringLiteral("Import BIOS..."),
                                         QMessageBox::AcceptRole);
    box->addButton(QStringLiteral("Not Now"), QMessageBox::RejectRole);
    box->setDefaultButton(import);
    connect(box, &QMessageBox::buttonClicked, this,
            [this, import, retry](QAbstractButton *b) {
        if (b != import) return;
        if (SettingsDialog::importRoms(this, m_session, false) <= 0) return;
        if (retry) retry();
    });
    box->open();
}

bool MainWindow::start(const QString &cartPath)
{
    colecosession_start_opts o;
    const QByteArray cart = cartPath.toLocal8Bit();
    colecosession_default_opts(m_session, &o);
    if (!cartPath.isEmpty()) o.cart_path = cart.constData();
    if (colecosession_start(m_session, &o) == 0) {
        updateStatus();
        return true;
    }
    explainRefusal(QStringLiteral("Cannot Start the Console"),
                   QString::fromUtf8(colecosession_last_error(m_session)),
                   [this, cartPath] { start(cartPath); });
    return false;
}

void MainWindow::importRoms(bool folder, const QString &path)
{
    const bool wasMissing = !colecosession_bios_available(m_session);
    if (SettingsDialog::importRoms(this, m_session, folder, path) > 0)
        afterImport(wasMissing);
}

/* The BIOS is read at power-up: power-cycle onto what was running (the saved
 * cartridge, else CONFIG) when it was the missing piece. */
void MainWindow::afterImport(bool wasMissing)
{
    if (!colecosession_is_running(m_session)) {
        start();
        return;
    }
    if (!wasMissing) {
        statusBar()->showMessage(
            QStringLiteral("BIOS imported: it takes effect at the next power cycle"), 6000);
        return;
    }
    colecosession_stop(m_session);
    if (start())
        statusBar()->showMessage(
            QStringLiteral("ROMs imported: the console was power-cycled to use them"), 6000);
}

/* Stop, re-read the settings store, start. Every option the Preferences
 * dialog writes is read by colecosession_default_opts(), so this is the only
 * way any of them can take effect. */
void MainWindow::restartSession()
{
    colecosession_settings_flush(m_session);
    colecosession_stop(m_session);
    if (start())
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

void MainWindow::useSoftwareDisplay()
{
    if (dynamic_cast<SoftDisplayWidget *>(m_display))
        return;
    auto *soft = new SoftDisplayWidget(m_session, this);
    m_display = soft;
    setCentralWidget(soft);   /* deletes the GPU display, which stops its feed */
    applyPicture();
}

void MainWindow::applyPicture()
{
    m_display->setTvAspect(colecosession_get_int(m_session, "tv_aspect", 1) != 0);
    m_display->setSmooth(colecosession_get_int(m_session, "smooth", 0) != 0);
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
    /* A BIOS dump is a ROM, not a cartridge, though it is a .rom or .bin:
     * import it straight away, no chooser. */
    if (colecosession_media_is_rom(path.toLocal8Bit().constData())) {
        importRoms(false, path);
        return;
    }
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

    colecosession_stop(m_session);
    start();
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
