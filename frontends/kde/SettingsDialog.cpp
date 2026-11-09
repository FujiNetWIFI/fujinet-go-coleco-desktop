/*
 * SettingsDialog -- see SettingsDialog.h.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SettingsDialog.h"
#include "help/HelpWindow.h"

#include <QTimer>
#include <functional>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QShortcut>
#include <QShortcut>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QPushButton>
#include <QTabWidget>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QVBoxLayout>

/* Same four TMS9928A palettes the GNOME dialog offers, in the same order:
 * the index is written straight into the shared settings store, so the two
 * frontends must agree on what index 2 means. */
static const char *const kPalettes[] = {"Default (TMS9928A)", "Palette 2",
                                        "Palette 3", "Palette 4", nullptr};

namespace {

QCheckBox *check(const char *text, const char *tip, colecosession *s,
                 const char *key, int def)
{
    auto *b = new QCheckBox(QString::fromUtf8(text));
    b->setToolTip(QString::fromUtf8(tip));
    b->setChecked(colecosession_get_int(s, key, def) != 0);
    return b;
}

/* Writes back only what actually differs, so `changed` reflects a real
 * edit rather than the dialog having been opened. */
bool commit(colecosession *s, const char *key, int def, int value)
{
    if (colecosession_get_int(s, key, def) == value)
        return false;
    colecosession_set_int(s, key, value);
    return true;
}

const char *const kRomFilter =
    "BIOS images and ROM sets (*.zip *.rom *.bin *.ZIP *.ROM *.BIN);;All files (*)";

} // namespace

int SettingsDialog::importRoms(QWidget *parent, colecosession *session,
                               bool folder, const QString &path)
{
    QStringList inputs;
    if (!path.isEmpty()) {
        inputs << path;
    } else if (folder) {
        const QString dir = QFileDialog::getExistingDirectory(
            parent, QStringLiteral("Import BIOS from a Folder"));
        if (dir.isEmpty()) return -1;
        inputs << dir;
    } else {
        inputs = QFileDialog::getOpenFileNames(
            parent, QStringLiteral("Import BIOS"), QString(),
            QString::fromUtf8(kRomFilter));
        if (inputs.isEmpty()) return -1;
    }

    int total = 0;
    bool trouble = false;
    QStringList said;
    for (const QString &in : inputs) {
        char msg[1024] = "";
        const int rc = colecosession_rom_import(
            session, in.toLocal8Bit().constData(), msg, sizeof msg);
        if (rc > 0) total += rc;
        if (rc <= 0) trouble = true;
        QString text = QString::fromUtf8(
            msg[0] ? msg : colecosession_last_error(session));
        if (inputs.size() > 1)
            text = QStringLiteral("%1: %2").arg(QFileInfo(in).fileName(), text);
        said << text;
    }
    /* the session's message always says what happened: show it whole */
    const QString text = said.join(QStringLiteral("\n\n"));
    if (total == 0)
        QMessageBox::warning(parent, QStringLiteral("Nothing Imported"), text);
    else if (trouble)
        QMessageBox::warning(parent, QStringLiteral("BIOS Imported"), text);
    else
        QMessageBox::information(parent, QStringLiteral("BIOS Imported"), text);
    return total;
}

bool SettingsDialog::run(QWidget *parent, colecosession *session, bool *openControllers)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("Preferences"));
    /* Without this the dialog shrinks to the widest control and the option
     * descriptions wrap into an unreadable column. */
    dlg.setMinimumWidth(460);
    auto *outer = new QVBoxLayout(&dlg);
    auto *tabs = new QTabWidget;
    outer->addWidget(tabs);
    auto *machinePage = new QWidget;
    auto *pageLayout = new QVBoxLayout(machinePage);
    tabs->addTab(machinePage, QStringLiteral("Machine"));

    auto *machine = new QGroupBox(QStringLiteral("Machine"));
    auto *mform = new QFormLayout(machine);
    QCheckBox *sgm = check("Super Game Module",
                           "Fit the Opcode SGM: an AY-3-8910 and 24K of RAM. "
                           "Cartridges that do not use it are unaffected.",
                           session, "sgm", 1);
    auto *palette = new QComboBox;
    for (int i = 0; kPalettes[i]; ++i)
        palette->addItem(QString::fromUtf8(kPalettes[i]));
    palette->setCurrentIndex(colecosession_get_int(session, "palette", 0));
    mform->addRow(sgm);
    mform->addRow(QStringLiteral("Palette"), palette);
    pageLayout->addWidget(machine);

    auto *net = new QGroupBox(QStringLiteral("FujiNet"));
    auto *nform = new QFormLayout(net);
    QCheckBox *fuji = check("Enable FujiNet",
                            "Run the in-process FujiNet the cartridge dials "
                            "into. Off means no network and no CONFIG client.",
                            session, "enable_fujinet", 1);
    nform->addRow(fuji);
    QCheckBox *external = check("Use external FujiNet",
                                "Skip the built-in FujiNet and dial another "
                                "instance at this address. Set that "
                                "instance's BoIP listener to match.",
                                session, "fujinet_external", 0);
    nform->addRow(external);
    auto *externalAddr = new QLineEdit(QString::fromUtf8(
        colecosession_get_str(session, "fujinet_external_addr", "")));
    externalAddr->setPlaceholderText(QStringLiteral("127.0.0.1:11502"));
    externalAddr->setEnabled(external->isChecked());
    QObject::connect(external, &QCheckBox::toggled, externalAddr,
                     &QWidget::setEnabled);
    nform->addRow(QStringLiteral("Address:port"), externalAddr);
    pageLayout->addWidget(net);

    auto *host = new QGroupBox(QStringLiteral("Host"));
    auto *hform = new QFormLayout(host);
    QCheckBox *audio = check("Audio", "Open the system audio device", session,
                             "enable_audio", 1);
    hform->addRow(audio);
    pageLayout->addWidget(host);

    pageLayout->addStretch(1);

    /* ---- Input ---- */
    auto *inputPage = new QWidget;
    auto *inputLayout = new QVBoxLayout(inputPage);
    auto *controllers = new QGroupBox(QStringLiteral("Controllers"));
    auto *cform = new QFormLayout(controllers);
    QCheckBox *pads = check("Enable gamepads", "Poll USB/Bluetooth gamepads", session,
                            "enable_gamepad", 1);
    QCheckBox *swap = check("Swap controller buttons",
                            "Exchange the left and right fire buttons on both "
                            "controllers",
                            session, "swap_buttons", 0);
    cform->addRow(pads);
    cform->addRow(swap);
    inputLayout->addWidget(controllers);

    /* one row per pad, rebuilt as they come and go; an assignment applies at
     * once and is remembered, so it is not part of what OK restarts for */
    auto *padsBox = new QGroupBox(QStringLiteral("Gamepads"));
    auto *padsLayout = new QVBoxLayout(padsBox);
    auto *padsNote = new QLabel(QStringLiteral(
        "The first pad is Controller 1, the next Controller 2, unless chosen here."));
    padsNote->setWordWrap(true);
    padsLayout->addWidget(padsNote);
    auto *padForm = new QFormLayout;
    padsLayout->addLayout(padForm);
    inputLayout->addWidget(padsBox);
    std::vector<QWidget *> padRows;
    unsigned padGen = colecosession_gamepad_generation(session);
    std::function<void()> rebuildPads = [&] {
        for (QWidget *row : padRows) padForm->removeRow(row);
        padRows.clear();
        const int count = colecosession_gamepad_count(session);
        if (count == 0) {
            auto *none = new QLabel(QStringLiteral(
                "No gamepads connected - plug one in: it is picked up as it appears."));
            none->setWordWrap(true);
            none->setStyleSheet(QStringLiteral("color: gray;"));
            padForm->addRow(none);
            padRows.push_back(none);
            return;
        }
        for (int idx = 0; idx < count && idx < 8; ++idx) {
            char name[128];
            colecosession_gamepad_name(session, idx, name, sizeof name);
            auto *box = new QComboBox;
            box->addItems({QStringLiteral("Automatic"), QStringLiteral("Controller 1"),
                           QStringLiteral("Controller 2")});
            const int assign = colecosession_gamepad_assignment(session, idx);
            box->setCurrentIndex(assign < 0 || assign > 1 ? 0 : assign + 1);
            const int eff = colecosession_gamepad_effective_port(session, idx);
            box->setToolTip(eff >= 0 ? QStringLiteral("Driving controller %1").arg(eff + 1)
                                     : QStringLiteral("Driving nothing"));
            QObject::connect(box, &QComboBox::currentIndexChanged, box,
                             [session, idx, box](int sel) {
                colecosession_gamepad_assign(session, idx, sel <= 0 ? -1 : sel - 1);
                const int now = colecosession_gamepad_effective_port(session, idx);
                box->setToolTip(now >= 0 ? QStringLiteral("Driving controller %1").arg(now + 1)
                                         : QStringLiteral("Driving nothing"));
            });
            padForm->addRow(QString::fromUtf8(name), box);
            padRows.push_back(box);
        }
    };
    rebuildPads();
    QTimer padTimer;
    QObject::connect(&padTimer, &QTimer::timeout, &dlg, [&] {
        const unsigned gen = colecosession_gamepad_generation(session);
        if (gen == padGen) return;
        padGen = gen;
        rebuildPads();
    });
    padTimer.start(1000);

    auto *controls = new QGroupBox(QStringLiteral("Controls"));
    auto *controlsLayout = new QVBoxLayout(controls);
    auto *controlsNote = new QLabel(QStringLiteral(
        "Keys and gamepad buttons are re-mapped in the Controllers window (Map)."));
    controlsNote->setWordWrap(true);
    controlsLayout->addWidget(controlsNote);
    auto *controlsRow = new QHBoxLayout;
    auto *resetControls = new QPushButton(QStringLiteral("Reset Controls"));
    resetControls->setToolTip(QStringLiteral("Put every key and gamepad button back to its default"));
    auto *openWindow = new QPushButton(QStringLiteral("Open Controllers"));
    openWindow->setToolTip(QStringLiteral("Close Preferences and open the Controllers window"));
    controlsRow->addWidget(resetControls);
    controlsRow->addWidget(openWindow);
    controlsRow->addStretch(1);
    controlsLayout->addLayout(controlsRow);
    auto *controlsState = new QLabel;
    controlsLayout->addWidget(controlsState);
    inputLayout->addWidget(controls);
    inputLayout->addStretch(1);
    QObject::connect(resetControls, &QPushButton::clicked, &dlg, [&, controlsState] {
        colecosession_bindings_reset(session);
        controlsState->setText(QStringLiteral("Every key and gamepad button is back to its default."));
    });
    bool wantControllers = false;
    QObject::connect(openWindow, &QPushButton::clicked, &dlg, [&] {
        wantControllers = true;
        dlg.accept();
    });
    tabs->addTab(inputPage, QStringLiteral("Input"));

    /* ---- ROMs ---- */
    bool imported = false;
    auto *romsPage = new QWidget;
    auto *rv = new QVBoxLayout(romsPage);
    auto *intro = new QLabel(QStringLiteral(
        "The ColecoVision BIOS is not part of the app. Import your own dump "
        "from a file, a zip (MAME's coleco.zip works) or a folder; it is "
        "recognised by its size and checksum, whatever it is called. Kept in:"));
    intro->setWordWrap(true);
    rv->addWidget(intro);
    auto *romPath = new QLabel(QString::fromLocal8Bit(
        colecosession_roms_path(session)));
    romPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    romPath->setWordWrap(true);
    rv->addWidget(romPath);
    auto *romRow = new QHBoxLayout;
    auto *importFiles = new QPushButton(QStringLiteral("Import BIOS..."));
    auto *importFolder =
        new QPushButton(QStringLiteral("Import from a Folder..."));
    romRow->addWidget(importFiles);
    romRow->addWidget(importFolder);
    romRow->addStretch(1);
    rv->addLayout(romRow);
    auto *romBox = new QGroupBox(QStringLiteral("Console"));
    auto *romForm = new QFormLayout(romBox);
    auto *romName = new QLabel(QStringLiteral("ColecoVision BIOS (OS7.rom)"));
    romName->setToolTip(QStringLiteral("OS7.rom, 8192 bytes"));
    auto *romState = new QLabel;
    romForm->addRow(romName, romState);
    rv->addWidget(romBox);
    rv->addStretch(1);
    auto refreshRoms = [&] {
        const bool have = colecosession_bios_available(session) != 0;
        romState->setText(have ? QStringLiteral("Imported")
                               : QStringLiteral("Not imported"));
        romState->setStyleSheet(
            have ? QString()
                 : QStringLiteral("color: palette(placeholder-text);"));
    };
    refreshRoms();
    QObject::connect(importFiles, &QPushButton::clicked, &dlg, [&] {
        if (importRoms(&dlg, session, false) > 0) imported = true;
        refreshRoms();
    });
    QObject::connect(importFolder, &QPushButton::clicked, &dlg, [&] {
        if (importRoms(&dlg, session, true) > 0) imported = true;
        refreshRoms();
    });
    tabs->addTab(romsPage, QStringLiteral("ROMs"));

    auto *note = new QLabel(
        QStringLiteral("Applied by restarting the session."));
    note->setWordWrap(true);
    outer->addWidget(note);

    /* Help, F1 and Ctrl+Shift+H (the app's shortcuts don't reach a modal dialog) */
    auto pageContext = [tabs](int page) {
        return page == 2 ? QStringLiteral("prefs/ROMs")
             : page == 1 ? QStringLiteral("prefs/Input") : QStringLiteral("prefs/Machine");
    };
    dlg.setProperty("colecoHelpContext", pageContext(tabs->currentIndex()));
    QObject::connect(tabs, &QTabWidget::currentChanged, &dlg,
                     [&dlg, pageContext](int page) { dlg.setProperty("colecoHelpContext", pageContext(page)); });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
                                         QDialogButtonBox::Help);
    QObject::connect(buttons, &QDialogButtonBox::helpRequested, &dlg, [&dlg] { colecoHelpFor(&dlg); });
    for (const char *key : { "F1", "Ctrl+Shift+H" })
        new QShortcut(QKeySequence(QString::fromLatin1(key)), &dlg, [&dlg] { colecoHelpFor(&dlg); });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
        char host[64];
        int port;
        if (external->isChecked() &&
            colecosession_parse_hostport(
                externalAddr->text().trimmed().toUtf8().constData(), host,
                sizeof host, &port) != 0) {
            QMessageBox::warning(
                &dlg, QStringLiteral("External FujiNet"),
                QStringLiteral("\"%1\" is not an address:port. Example: "
                               "127.0.0.1:11502")
                    .arg(externalAddr->text()));
            externalAddr->setFocus();
            return;
        }
        dlg.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg,
                     &QDialog::reject);
    outer->addWidget(buttons);

    const int result = dlg.exec();
    padTimer.stop();
    if (result != QDialog::Accepted)
        return imported;    /* an import is already on disk, Cancel or not */
    if (openControllers) *openControllers = wantControllers;

    bool changed = imported;
    changed |= commit(session, "sgm", 1, sgm->isChecked() ? 1 : 0);
    changed |= commit(session, "palette", 0, palette->currentIndex());
    changed |= commit(session, "swap_buttons", 0, swap->isChecked() ? 1 : 0);
    changed |= commit(session, "enable_fujinet", 1, fuji->isChecked() ? 1 : 0);
    changed |= commit(session, "fujinet_external", 0,
                      external->isChecked() ? 1 : 0);
    {
        const QByteArray addr = externalAddr->text().trimmed().toUtf8();
        if (addr != QByteArray(colecosession_get_str(
                        session, "fujinet_external_addr", ""))) {
            colecosession_set_str(session, "fujinet_external_addr",
                                  addr.constData());
            changed = true;
        }
    }
    changed |= commit(session, "enable_audio", 1, audio->isChecked() ? 1 : 0);
    changed |= commit(session, "enable_gamepad", 1, pads->isChecked() ? 1 : 0);
    return changed;
}
