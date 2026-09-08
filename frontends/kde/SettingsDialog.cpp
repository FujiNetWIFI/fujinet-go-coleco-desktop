/*
 * SettingsDialog -- see SettingsDialog.h.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
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

} // namespace

bool SettingsDialog::run(QWidget *parent, colecosession *session)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("Preferences"));
    /* Without this the dialog shrinks to the widest control and the option
     * descriptions wrap into an unreadable column. */
    dlg.setMinimumWidth(460);
    auto *outer = new QVBoxLayout(&dlg);

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
    QCheckBox *swap = check("Swap controller buttons",
                            "Exchange the left and right fire buttons on both "
                            "ports",
                            session, "swap_buttons", 0);
    mform->addRow(sgm);
    mform->addRow(QStringLiteral("Palette"), palette);
    mform->addRow(swap);
    outer->addWidget(machine);

    auto *net = new QGroupBox(QStringLiteral("FujiNet"));
    auto *nform = new QFormLayout(net);
    QCheckBox *fuji = check("Enable FujiNet",
                            "Run the in-process FujiNet the cartridge dials "
                            "into. Off means no network and no CONFIG client.",
                            session, "enable_fujinet", 1);
    nform->addRow(fuji);
    outer->addWidget(net);

    auto *host = new QGroupBox(QStringLiteral("Host"));
    auto *hform = new QFormLayout(host);
    QCheckBox *audio = check("Audio", "Open the system audio device", session,
                             "enable_audio", 1);
    QCheckBox *pads = check("Gamepads", "Poll USB/Bluetooth gamepads", session,
                            "enable_gamepad", 1);
    hform->addRow(audio);
    hform->addRow(pads);
    outer->addWidget(host);

    auto *note = new QLabel(
        QStringLiteral("Applied by restarting the session."));
    note->setWordWrap(true);
    outer->addWidget(note);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                         QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg,
                     &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg,
                     &QDialog::reject);
    outer->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return false;

    bool changed = false;
    changed |= commit(session, "sgm", 1, sgm->isChecked() ? 1 : 0);
    changed |= commit(session, "palette", 0, palette->currentIndex());
    changed |= commit(session, "swap_buttons", 0, swap->isChecked() ? 1 : 0);
    changed |= commit(session, "enable_fujinet", 1, fuji->isChecked() ? 1 : 0);
    changed |= commit(session, "enable_audio", 1, audio->isChecked() ? 1 : 0);
    changed |= commit(session, "enable_gamepad", 1, pads->isChecked() ? 1 : 0);
    return changed;
}
