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
#include "controllers/ControllersWindow.h"
#include "help/HelpWindow.h"
#include "debugger/DebuggerWindow.h"

MainWindow::MainWindow(colecosession *session, QWidget *parent)
    : QMainWindow(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("FujiNet Go ColecoVision"));
    setProperty("colecoHelpContext", QStringLiteral("main"));
    /* 256x212 at 3x, in 4:3 -- big enough that the BIOS text is readable
     * without resizing on every launch. */
    resize(848, 636);
    setAcceptDrops(true);

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

    /* One timer for the news the other threads leave: the gamepad thread
     * cannot call into Qt, so it posts console actions and hot-plug events
     * and this takes them. The baseline keeps pads already attached from
     * announcing themselves. */
    m_padGen = colecosession_gamepad_generation(session);
    connect(&m_tickTimer, &QTimer::timeout, this, &MainWindow::tick);
    m_tickTimer.start(50);

    /* COLECO_OPEN_CONTROLLERS (or its old name COLECO_OPEN_KEYPAD) /
     * COLECO_OPEN_DEBUGGER, the same launch hooks the GNOME frontend has and
     * for the same reason: they are the way in when the app misbehaves
     * before a menu is reachable. */
    if (qEnvironmentVariableIsSet("COLECO_OPEN_CONTROLLERS") || qEnvironmentVariableIsSet("COLECO_OPEN_KEYPAD"))
        QTimer::singleShot(0, this, &MainWindow::toggleControllers);
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
                       [this] { runSysaction(COLECO_SYSACT_RESET); });
    machine->addAction(QStringLiteral("Reset to &CONFIG"), this,
                       [this] { runSysaction(COLECO_SYSACT_RESET_CONFIG); });
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
                    this, &MainWindow::toggleControllers);
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

    QMenu *help = menuBar()->addMenu(QStringLiteral("&Help"));
    {
        /* works from every window of the app (the Controllers window and the
         * Debugger have the keyboard too) */
        QAction *helpAct = help->addAction(QStringLiteral("FujiNet Go ColecoVision &Help"));
        helpAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H));
        helpAct->setShortcutContext(Qt::ApplicationShortcut);
        addAction(helpAct);
        connect(helpAct, &QAction::triggered, this, [this] {
            colecoHelpFor(QApplication::activeWindow() ? QApplication::activeWindow() : this);
        });
    }
    help->addSeparator();
    help->addAction(QStringLiteral("&About FujiNet Go ColecoVision"), this,
                    &MainWindow::showAbout);
}

void MainWindow::showAbout()
{
    QMessageBox::about(
        this, QStringLiteral("About FujiNet Go ColecoVision"),
        QStringLiteral(
            "<b>FujiNet Go ColecoVision</b> " COLECO_VERSION_STRING "<br>"
            "Self-contained ColecoVision with built-in FujiNet.<br>"
            "Copyright \u00a9 2026 Thomas Cherryhomes \u2014 GPL-3.0-or-later<br>"
            "<a href=\"https://fujinet.online/\">fujinet.online</a>"));
}

void MainWindow::showSettings()
{
    bool openControllers = false;
    if (SettingsDialog::run(this, m_session, &openControllers))
        restartSession();
    colecoControllersRefresh();
    if (openControllers) colecoControllersShow(this, m_session);
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
    QPushButton *helpBtn = box->addButton(QStringLiteral("Help"), QMessageBox::HelpRole);
    box->setDefaultButton(import);
    /* Help keeps the box up: a HelpRole button does not close it */
    helpBtn->disconnect();
    connect(helpBtn, &QPushButton::clicked, this, [this] { colecoHelpShow(this, QStringLiteral("nobios")); });
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

void MainWindow::toggleControllers()
{
    /* Hidden, not destroyed: a remap in progress and the window's position
     * both survive closing it. */
    colecoControllersToggle(this, m_session);
}

/* ---- the timer ------------------------------------------------------------ */

void MainWindow::tick()
{
    drainSysactions();
    pollGamepads();
}

void MainWindow::drainSysactions()
{
    int sysact;
    while (colecosession_sysaction_take(m_session, &sysact)) runSysaction(sysact);
}

/* A console action, from a key, a pad button, the Controllers window or the
 * menu: always here, on the UI thread. */
void MainWindow::runSysaction(int sysact)
{
    switch (sysact) {
    case COLECO_SYSACT_RESET:
        colecosession_sysaction(m_session, sysact);
        statusBar()->showMessage(QStringLiteral("Console reset"), 3000);
        break;
    case COLECO_SYSACT_RESET_CONFIG:
        colecosession_sysaction(m_session, sysact);
        statusBar()->showMessage(QStringLiteral("Back to the FujiNet CONFIG"), 3000);
        updateStatus();
        break;
    default: break;
    }
}

void MainWindow::pollGamepads()
{
    const unsigned gen = colecosession_gamepad_generation(m_session);
    if (gen == m_padGen) return;
    m_padGen = gen;
    char msg[160];
    if (colecosession_gamepad_last_event(m_session, msg, sizeof msg) > 0)
        statusBar()->showMessage(QString::fromUtf8(msg), 4000);
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

void MainWindow::keyPressEvent(QKeyEvent *evt)
{
    if (evt->isAutoRepeat()) return;
    if (evt->key() == Qt::Key_F9) { toggleControllers(); return; }
    if (evt->key() == Qt::Key_F12) {
        DebuggerWindow::showFor(this, m_session);
        return;
    }

    const uint32_t keysym = colecoKeysymFromQt(evt);
    if (!keysym) { QMainWindow::keyPressEvent(evt); return; }

    /* No "a key typed with Ctrl is the app's" rule: Left Ctrl is Controller
     * 1's fire button, so steering with it held must reach the console. The
     * app's Ctrl chords are menu shortcuts, which Qt takes before a key
     * event exists. A console action runs on the leading edge only. */
    const int sysact = colecosession_key_sysaction(m_session, keysym);
    if (sysact >= 0) {
        if (!m_sysactDown[sysact]) { m_sysactDown[sysact] = true; runSysaction(sysact); }
        return;
    }
    if (!colecosession_key(m_session, keysym, 1)) QMainWindow::keyPressEvent(evt);
}

void MainWindow::keyReleaseEvent(QKeyEvent *evt)
{
    if (evt->isAutoRepeat()) return;
    const uint32_t keysym = colecoKeysymFromQt(evt);
    if (!keysym) { QMainWindow::keyReleaseEvent(evt); return; }
    const int sysact = colecosession_key_sysaction(m_session, keysym);
    if (sysact >= 0) { m_sysactDown[sysact] = false; return; }
    if (!colecosession_key(m_session, keysym, 0)) QMainWindow::keyReleaseEvent(evt);
}

bool MainWindow::event(QEvent *evt)
{
    if (evt->type() == QEvent::WindowDeactivate) {
        colecosession_release_all(m_session);
        for (bool &down : m_sysactDown) down = false;
    }
    return QMainWindow::event(evt);
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
    m_tickTimer.stop();
    QMainWindow::closeEvent(e);
}
