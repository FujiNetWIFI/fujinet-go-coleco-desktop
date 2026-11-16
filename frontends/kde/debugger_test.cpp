/*
 * debugger_test -- the Debugger window (debugger/DebuggerWindow.cpp) driven
 * with synthetic events on the offscreen platform.
 *
 * Without a machine (no ColecoVision BIOS, which is not shippable and so is
 * absent in CI; or DBG_TEST_NO_MACHINE=1) only what needs none is checked: the
 * twelve tabs and their order, the COLECO_DEBUGGER_TAB names and the older
 * aliases, the Help contexts, the minimum height, pop-out and dock, the
 * Symbols tab filling on first open. With a machine (OS7.rom in
 * COLECO_BIOS_DIR, or tools/roms) it is stopped, run on to the start of a
 * frame (a NMI cannot land in a step, and a CPU in HALT keeps a written PC
 * halted), and a small program of NOP-padded instructions is written into the
 * console RAM at $7000 and stepped through the window: the register strip,
 * the Poke bar, the console, a breakpoint with an expression condition, a
 * watch, the Video and Sound tabs, the Cart & I/O text and the env hooks.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QtTest>

#include <QApplication>
#include <QCheckBox>
#include <QFile>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStyleHints>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>

#include <cstdlib>
#include <cstring>

extern "C" {
#include "colecodebug.h"
#include "colecomemtools.h"
#include "colecosession.h"
}
#include "debugger/DebuggerWindow.h"
#include "debugger/VideoTab.h"

namespace {

/* In the 1K console RAM at $7000, NOP-padded so a backward disassembly guess
 * is stable: NOP x3, LD HL,$1234, NOP, LD ($7100),HL, NOP x3. */
const uint8_t kProg[] = { 0x00, 0x00, 0x00, 0x21, 0x34, 0x12, 0x00, 0x22, 0x00, 0x71, 0x00, 0x00, 0x00 };
const int kStart = 0x7000;
const int kLoad = 0x7003;

QStringList expectedTabs()
{
    return { QStringLiteral("Console"), QStringLiteral("Disassembly"), QStringLiteral("Memory"),
             QStringLiteral("Breakpoints"), QStringLiteral("Watches"), QStringLiteral("Cart & I/O"),
             QStringLiteral("Trace"), QStringLiteral("History"), QStringLiteral("Video"),
             QStringLiteral("Sound"), QStringLiteral("Symbols"), QStringLiteral("FujiNet") };
}

} // namespace

class DebuggerTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    colecosession *m_s = nullptr;
    colecodebug *m_d = nullptr;
    QMainWindow *m_win = nullptr;
    QTabWidget *m_tabs = nullptr;
    bool m_machine = false;

    static QMainWindow *findWindow()
    {
        for (QWidget *top : QApplication::topLevelWidgets())
            if (top->windowTitle() == QLatin1String("Debugger")) return qobject_cast<QMainWindow *>(top);
        return nullptr;
    }
    /* the window is kept when closed: hide it (which detaches the engine) and
     * delete it, so showFor() builds a fresh one that reads the env hooks */
    void discardWindow()
    {
        if (m_win) {
            m_win->hide();
            m_win->deleteLater();
        }
        m_win = nullptr;
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void openWindow()
    {
        DebuggerWindow::showFor(nullptr, m_s);
        m_win = findWindow();
        QVERIFY(m_win);
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
        m_tabs = m_win->findChild<QTabWidget *>();
        QVERIFY(m_tabs);
    }
    QString tabText(int idx) const { return m_tabs->tabText(idx).replace(QStringLiteral("&&"), QStringLiteral("&")); }
    QPushButton *button(const QString &start)
    {
        for (QPushButton *btn : m_win->findChildren<QPushButton *>())
            if (btn->text().startsWith(start)) return btn;
        return nullptr;
    }
    QLineEdit *entry(const QString &placeholderStart)
    {
        for (QLineEdit *edit : m_win->findChildren<QLineEdit *>())
            if (edit->placeholderText().startsWith(placeholderStart)) return edit;
        return nullptr;
    }
    QPushButton *addButton(int tab)   /* the tab's own Add */
    {
        for (QPushButton *btn : m_tabs->widget(tab)->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Add")) return btn;
        return nullptr;
    }
    QList<QLineEdit *> regFields()
    {
        QList<QLineEdit *> out;
        for (QLineEdit *edit : m_win->findChildren<QLineEdit *>())
            if (edit->toolTip().startsWith(QLatin1String("Enter sets it"))) out << edit;
        return out;
    }
    void waitStopped()
    {
        QTRY_VERIFY_WITH_TIMEOUT(colecodebug_is_paused(m_d), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(button(QStringLiteral("Run (F5)")) != nullptr, 3000);
    }
    int readPc()
    {
        adamcore_z80_regs regs;
        colecodebug_get_regs(m_d, &regs);
        return regs.pc;
    }
    adamcore_z80_regs regsNow()
    {
        adamcore_z80_regs regs;
        colecodebug_get_regs(m_d, &regs);
        return regs;
    }
    /* The console RAM is there from power-on; a CPU in HALT keeps a written PC
     * halted and an NMI cannot be allowed to land in a step, so the machine is
     * run on to the start of a frame first. Interrupts are off. */
    bool loadProgram()
    {
        uint8_t back[sizeof kProg];
        char err[200] = "";
        if (colecodebug_write_mem(m_d, kStart, kProg, int(sizeof kProg)) != int(sizeof kProg)) return false;
        if (colecodebug_read_mem(m_d, kStart, back, int(sizeof kProg)) != int(sizeof kProg) || memcmp(back, kProg, sizeof kProg)) return false;
        adamcore_z80_regs regs = regsNow();
        regs.sp = 0x7380;
        regs.iff1 = regs.iff2 = 0;
        regs.halted = 0;
        regs.pc = kStart;
        return colecodebug_regs_set(m_d, &regs, err, sizeof err) && readPc() == kStart;
    }
    QString consoleText()
    {
        auto *view = m_tabs->widget(0)->findChild<QPlainTextEdit *>();
        return view ? view->toPlainText() : QString();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qputenv("SDL_AUDIODRIVER", "dummy");
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        if (qEnvironmentVariableIsSet("DBG_TEST_DARK")) QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
#endif
        colecosession_paths paths = {};
        const QByteArray dir = m_dir.path().toUtf8();
        paths.config_dir = dir.constData();
        paths.data_dir = dir.constData();
        m_s = colecosession_new(&paths);
        QVERIFY(m_s);
        m_d = colecosession_debugger(m_s);
        QVERIFY(m_d);

        /* the BIOS is not shippable: COLECO_BIOS_DIR, else tools/roms; none, no machine */
        const QString romDir = qEnvironmentVariableIsSet("COLECO_BIOS_DIR") ? qEnvironmentVariable("COLECO_BIOS_DIR")
                                                                            : QStringLiteral(COLECO_TEST_ROMS);
        const QString bios = romDir + QStringLiteral("/OS7.rom");
        if (!qEnvironmentVariableIsSet("DBG_TEST_NO_MACHINE") && QFile::exists(bios)) {
            colecosession_import_bios(m_s, bios.toLocal8Bit().constData());
            colecosession_start_opts opts;
            colecosession_default_opts(m_s, &opts);
            opts.enable_fujinet = 0;
            opts.enable_audio = 0;
            opts.enable_gamepad = 0;
            opts.cart_path = nullptr;
            m_machine = colecosession_start(m_s, &opts) == 0;
            if (m_machine) {
                for (int waited = 0; waited < 15000 && colecosession_frame_count(m_s) < 30; waited += 50) QTest::qWait(50);
                m_machine = colecosession_frame_count(m_s) >= 30;
            }
        }

        /* the window opens on the tab COLECO_DEBUGGER_TAB names (the older "vdp" is the Video tab) */
        qputenv("COLECO_DEBUGGER_TAB", "vdp");
        DebuggerWindow::showFor(nullptr, m_s);
        qunsetenv("COLECO_DEBUGGER_TAB");
        m_win = findWindow();
        QVERIFY(m_win);
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
        m_tabs = m_win->findChild<QTabWidget *>();
        QVERIFY(m_tabs);
    }

    void cleanupTestCase()
    {
        if (m_win) m_win->hide();
        QCoreApplication::processEvents();
        if (m_s) {
            colecosession_stop(m_s);
            colecosession_free(m_s);
        }
    }

    void tabsInOrder()
    {
        QStringList names;
        for (int idx = 0; idx < m_tabs->count(); ++idx) names << tabText(idx);
        QCOMPARE(names, expectedTabs());
        QCOMPARE(m_tabs->currentIndex(), 8);   /* "vdp" is the Video tab */
        QVERIFY(m_win->findChild<VideoTab *>() != nullptr);
    }

    void tabsByName()
    {
        /* each name the env hook takes, in a window of its own */
        discardWindow();
        struct { const char *name; int index; } cases[] = {
            { "cpu", 1 }, { "registers", 1 }, { "io", 5 }, { "vram", 8 }, { "CART", 5 }, { "3", 3 },
            { "Sound", 9 }, { "console", 0 }, { "hist", 7 }, { "sym", 10 } };
        for (const auto &one : cases) {
            qputenv("COLECO_DEBUGGER_TAB", one.name);
            DebuggerWindow::showFor(nullptr, m_s);
            qunsetenv("COLECO_DEBUGGER_TAB");
            QMainWindow *win = findWindow();
            QVERIFY(win);
            QVERIFY2(win->findChild<QTabWidget *>()->currentIndex() == one.index, one.name);
            m_win = win;
            discardWindow();
        }
        openWindow();
    }

    /* DBG_TEST_SHOTS=<dir>: a PNG of every tab (DBG_TEST_DARK=1: in the dark scheme) */
    void shots()
    {
        const QString dir = qEnvironmentVariable("DBG_TEST_SHOTS");
        if (dir.isEmpty()) QSKIP("DBG_TEST_SHOTS not set");
        for (int idx = 0; idx < m_tabs->count(); ++idx) {
            m_tabs->setCurrentIndex(idx);
            QTest::qWait(250);
            QVERIFY(m_win->grab().save(QStringLiteral("%1/tab%2.png").arg(dir).arg(idx)));
        }
    }

    void helpContexts()
    {
        static const char *const kContexts[] = { "console", "disassembly", "memory", "breakpoints", "watches", "cart",
                                                 "trace", "history", "video", "sound", "symbols", "fujinet" };
        for (int idx = 0; idx < 12; ++idx) {
            m_tabs->setCurrentIndex(idx);
            QCOMPARE(m_win->property("colecoHelpContext").toString(),
                     QStringLiteral("debugger/%1").arg(QLatin1String(kContexts[idx])));
        }
    }

    void fitsASmallScreen()
    {
        QVERIFY2(m_win->minimumSizeHint().height() <= 800,
                 qPrintable(QStringLiteral("minimum height %1").arg(m_win->minimumSizeHint().height())));
    }

    /* the built-in BIOS names fill the list the first time the tab opens */
    void symbolsFillOnFirstOpen()
    {
        m_tabs->setCurrentIndex(10);
        QListWidget *list = m_tabs->widget(10)->findChild<QListWidget *>();
        QVERIFY(list);
        QTRY_VERIFY_WITH_TIMEOUT(list->count() > 0, 2000);
        QLineEdit *filter = m_tabs->widget(10)->findChild<QLineEdit *>();
        QVERIFY(filter);
        const int all = list->count();
        filter->setText(QStringLiteral("WRITE_VRAM"));
        QVERIFY(list->count() > 0 && list->count() < all);
        QVERIFY(list->item(0)->text().contains(QStringLiteral("WRITE_VRAM")));
        filter->clear();
        QCOMPARE(list->count(), all);
    }

    void popOutAndDock()
    {
        m_tabs->setCurrentIndex(9);
        QPushButton *pop = button(QStringLiteral("Pop Out"));
        QVERIFY(pop);
        pop->click();
        QCOMPARE(m_tabs->count(), 11);
        QWidget *window = nullptr;
        for (QWidget *top : QApplication::topLevelWidgets())
            if (top->windowTitle().endsWith(QLatin1String("Sound"))) window = top;
        QVERIFY(window);
        for (QPushButton *btn : window->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Dock")) btn->click();
        QCOMPARE(m_tabs->count(), 12);
        QCOMPARE(tabText(9), QStringLiteral("Sound"));
    }

    void consoleAndRegisters()
    {
        if (!m_machine) QSKIP("needs a machine (the ColecoVision BIOS)");
        if (!colecodebug_is_paused(m_d)) colecodebug_pause(m_d);
        waitStopped();
        const uint64_t before = colecodebug_stop_serial(m_d);
        colecodebug_frame(m_d);       /* to the start of a frame: no NMI lands in a step */
        QTRY_VERIFY_WITH_TIMEOUT(colecodebug_stop_serial(m_d) != before, 4000);
        waitStopped();
        QVERIFY2(loadProgram(), "the console RAM could not be loaded and started");

        /* the console runs the core's prompt */
        m_tabs->setCurrentIndex(0);
        QLineEdit *prompt = entry(QStringLiteral("command"));
        QVERIFY(prompt);
        prompt->setText(QStringLiteral("print $10 + 2"));
        QTest::keyClick(prompt, Qt::Key_Return);
        QTRY_VERIFY(consoleText().contains(QStringLiteral("$0012")));
        prompt->setText(QStringLiteral("regs"));
        QTest::keyClick(prompt, Qt::Key_Return);
        QVERIFY(consoleText().contains(QStringLiteral("HL")));

        /* step the NOPs, then the LD HL, with the toolbar button */
        QCOMPARE(readPc(), kStart);
        QPushButton *step = button(QStringLiteral("Step ("));
        QVERIFY(step);
        for (int idx = 1; idx <= 3; ++idx) {
            step->click();
            QTRY_COMPARE(readPc(), kStart + idx);
        }
        QCOMPARE(readPc(), kLoad);
        step->click();
        QTRY_COMPARE(readPc(), kLoad + 3);
        QCOMPARE(int(regsNow().h << 8 | regsNow().l), 0x1234);

        /* the strip: HL takes hex, DE an expression, only while stopped */
        const QList<QLineEdit *> regs = regFields();
        QCOMPARE(regs.size(), 15);
        regs[3]->setText(QStringLiteral("abcd"));
        QTest::keyClick(regs[3], Qt::Key_Return);
        QCOMPARE(int(regsNow().h << 8 | regsNow().l), 0xABCD);
        regs[2]->setText(QStringLiteral("hl + 1"));
        QTest::keyClick(regs[2], Qt::Key_Return);
        QCOMPARE(int(regsNow().d << 8 | regsNow().e), 0xABCE);
        /* the shadow set and IX */
        regs[8]->setText(QStringLiteral("1234"));
        QTest::keyClick(regs[8], Qt::Key_Return);
        QCOMPARE(int(regsNow().a2 << 8 | regsNow().f2), 0x1234);
        regs[4]->setText(QStringLiteral("$7100"));
        QTest::keyClick(regs[4], Qt::Key_Return);
        QCOMPARE(int(regsNow().ix), 0x7100);
        /* IM above 2 is refused by the engine and leaves it alone */
        const int imBefore = regsNow().im;
        regs[14]->setText(QStringLiteral("7"));
        QTest::keyClick(regs[14], Qt::Key_Return);
        QCOMPARE(int(regsNow().im), imBefore);
        /* a flag check box and IFF1 */
        QCheckBox *zero = nullptr, *iff1 = nullptr;
        for (QCheckBox *box : m_win->findChildren<QCheckBox *>()) {
            if (box->text() == QLatin1String("Z")) zero = box;
            if (box->text() == QLatin1String("IFF1")) iff1 = box;
        }
        QVERIFY(zero && iff1);
        const int zeroNow = (regsNow().f & 0x40) != 0;
        zero->click();
        QCOMPARE((regsNow().f & 0x40) != 0, !zeroNow);
        iff1->click();
        QCOMPARE(int(regsNow().iff1), 1);
        iff1->click();
        QCOMPARE(int(regsNow().iff1), 0);
        QVERIFY(loadProgram());
    }

    void pokeAndSearch()
    {
        if (!m_machine) QSKIP("needs a machine (the ColecoVision BIOS)");
        QVERIFY(colecodebug_is_paused(m_d));
        m_tabs->setCurrentIndex(2);
        QLineEdit *addr = entry(QStringLiteral("address or label"));
        QLineEdit *val = entry(QStringLiteral("value"));
        QVERIFY(addr && val);
        addr->setText(QStringLiteral("$7200"));
        val->setText(QStringLiteral("$5A"));
        QTest::keyClick(val, Qt::Key_Return);
        uint8_t byte = 0;
        colecodebug_read_mem(m_d, 0x7200, &byte, 1);
        QCOMPARE(int(byte), 0x5A);
        /* a value that is not a byte is refused */
        val->setText(QStringLiteral("$1FF"));
        QTest::keyClick(val, Qt::Key_Return);
        colecodebug_read_mem(m_d, 0x7200, &byte, 1);
        QCOMPARE(int(byte), 0x5A);
        /* Go to marks the row, and search finds the program's LD HL */
        QLineEdit *gotoBox = entry(QStringLiteral("$8000, label"));
        QVERIFY(gotoBox);
        gotoBox->setText(QStringLiteral("$7200"));
        QTest::keyClick(gotoBox, Qt::Key_Return);
        QVERIFY(!m_tabs->widget(2)->findChildren<QPlainTextEdit *>().isEmpty());
        QLineEdit *find = entry(QStringLiteral("bytes or text"));
        QVERIFY(find);
        find->setText(QStringLiteral("21 34 12 00 22"));
        QTest::keyClick(find, Qt::Key_Return);
        QLabel *note = nullptr;
        for (QLabel *label : m_tabs->widget(2)->findChildren<QLabel *>())
            if (label->text().startsWith(QLatin1String("Found")) || label->text().startsWith(QLatin1String("Not found"))) note = label;
        QVERIFY(note);
        /* the 1K console RAM repeats every 1K from $6000, so the hit may be any mirror of $7003 */
        QVERIFY2(note->text().contains(QRegularExpression(QStringLiteral("Found at \\$[67][0-9A-F]03"))), qPrintable(note->text()));
    }

    void breakpointWithCondition()
    {
        if (!m_machine) QSKIP("needs a machine (the ColecoVision BIOS)");
        colecodebug_breakpoint_clear(m_d);
        m_tabs->setCurrentIndex(3);
        QLineEdit *addr = entry(QStringLiteral("start ("));
        QLineEdit *cond = entry(QStringLiteral("condition"));
        QVERIFY(addr && cond);
        addr->setText(QStringLiteral("$7100"));
        cond->setText(QStringLiteral("value == $34"));
        QComboBox *type = m_tabs->widget(3)->findChild<QComboBox *>();
        QVERIFY(type);
        QCOMPARE(type->itemText(3), QStringLiteral("Access"));
        type->setCurrentIndex(2);   /* Write */
        addButton(3)->click();
        colecodebug_breakpoint bps[4];
        QCOMPARE(colecodebug_breakpoint_list(m_d, bps, 4), 1);
        QCOMPARE(bps[0].type, int(COLECODEBUG_BP_WRITE));
        QCOMPARE(QString::fromUtf8(bps[0].condition), QStringLiteral("value == $34"));

        /* a condition that does not parse is refused */
        addr->setText(QStringLiteral("$C101"));
        cond->setText(QStringLiteral("value =="));
        addButton(3)->click();
        QCOMPARE(colecodebug_breakpoint_list(m_d, bps, 4), 1);

        /* an Access entry is one row */
        addr->setText(QStringLiteral("$7300"));
        cond->clear();
        type->setCurrentIndex(3);
        addButton(3)->click();
        QCOMPARE(colecodebug_breakpoint_list(m_d, bps, 4), 2);
        QCOMPARE(bps[1].type, int(COLECODEBUG_BP_ACCESS));
        colecodebug_breakpoint_remove(m_d, bps[1].id);

        /* run the program: it stops after the store, the byte in the core's words */
        QVERIFY(loadProgram());
        colecodebug_resume(m_d);
        QTRY_VERIFY_WITH_TIMEOUT(colecodebug_is_paused(m_d), 4000);
        colecodebug_snapshot snap;
        colecodebug_stop_snapshot(m_d, &snap);
        QVERIFY2(QString::fromUtf8(snap.text).contains(QStringLiteral("$34")), snap.text);
        QTRY_VERIFY_WITH_TIMEOUT(button(QStringLiteral("Run (F5)")) != nullptr, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(consoleText().contains(QStringLiteral("$34")), 2000);
        colecodebug_breakpoint_clear(m_d);
    }

    void watchesAndBreakOnChange()
    {
        if (!m_machine) QSKIP("needs a machine (the ColecoVision BIOS)");
        QVERIFY(colecodebug_is_paused(m_d));
        colecowatch_clear();
        m_tabs->setCurrentIndex(4);
        QLineEdit *entryBox = m_tabs->widget(4)->findChild<QLineEdit *>();
        QVERIFY(entryBox);
        entryBox->setText(QStringLiteral("$7100"));
        QTest::keyClick(entryBox, Qt::Key_Return);
        QCOMPARE(colecowatch_count(), 1);
        QListWidget *list = m_tabs->widget(4)->findChild<QListWidget *>();
        QVERIFY(list);
        QTRY_COMPARE(list->count(), 1);
        QVERIFY(list->item(0)->text().contains(QStringLiteral("7100")));
        list->setCurrentRow(0);
        for (QPushButton *btn : m_tabs->widget(4)->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Break on Change")) btn->click();
        colecodebug_breakpoint bps[4];
        QTRY_COMPARE(colecodebug_breakpoint_list(m_d, bps, 4), 1);
        QCOMPARE(QString::fromUtf8(bps[0].condition), QStringLiteral("value != old"));
        colecodebug_breakpoint_clear(m_d);
        colecowatch_clear();
    }

    void traceAndHistory()
    {
        if (!m_machine) QSKIP("needs a machine (the ColecoVision BIOS)");
        QVERIFY(colecodebug_is_paused(m_d));
        m_tabs->setCurrentIndex(7);
        QPushButton *record = nullptr;
        for (QPushButton *btn : m_tabs->widget(7)->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Record")) record = btn;
        QVERIFY(record);
        record->click();
        QVERIFY(colecodebug_history_enabled(m_d));
        QVERIFY(loadProgram());
        for (int idx = 0; idx < 4; ++idx) {
            colecodebug_step_into(m_d);
            QTest::qWait(60);
        }
        QTRY_VERIFY_WITH_TIMEOUT(colecodebug_history_count(m_d) >= 4, 3000);
        m_tabs->setCurrentIndex(0);
        m_tabs->setCurrentIndex(7);
        auto *text = m_tabs->widget(7)->findChild<QPlainTextEdit *>();
        QVERIFY(text);
        QTRY_VERIFY_WITH_TIMEOUT(text->toPlainText().contains(QStringLiteral("700")), 3000);
        record->click();
        QVERIFY(!colecodebug_history_enabled(m_d));
        colecodebug_history_clear(m_d);
    }

    void videoSoundAndCart()
    {
        if (!m_machine) QSKIP("needs a machine (the ColecoVision BIOS)");
        QVERIFY(colecodebug_is_paused(m_d));
        m_tabs->setCurrentIndex(8);
        VideoTab *video = m_win->findChild<VideoTab *>();
        QVERIFY(video);
        video->refresh(true);
        QVERIFY(video->findChild<QComboBox *>() != nullptr);
        m_tabs->setCurrentIndex(9);
        auto soundText = [this] {
            auto *view = qobject_cast<QPlainTextEdit *>(m_tabs->widget(9));
            return view ? view->toPlainText() : QString();
        };
        QTRY_VERIFY2_WITH_TIMEOUT(soundText().contains(QStringLiteral("SN76489")), qPrintable(soundText()), 3000);
        m_tabs->setCurrentIndex(5);
        QString ioText;
        QTRY_VERIFY_WITH_TIMEOUT(!m_tabs->widget(5)->findChildren<QPlainTextEdit *>().isEmpty(), 2000);
        for (QPlainTextEdit *view : m_tabs->widget(5)->findChildren<QPlainTextEdit *>()) ioText += view->toPlainText();
        QVERIFY2(ioText.contains(QStringLiteral("Memory map")), qPrintable(ioText));
        QVERIFY(ioText.contains(QStringLiteral("OS7")));
        QVERIFY(ioText.contains(QStringLiteral("Controllers")));
        QVERIFY(ioText.contains(QStringLiteral("VDP")));
    }

    /* a PC written while the CPU is in HALT leaves it halted: the strip says so */
    void haltNote()
    {
        if (!m_machine) QSKIP("needs a machine (the ColecoVision BIOS)");
        QVERIFY(colecodebug_is_paused(m_d));
        const uint8_t halt = 0x76;
        QVERIFY(colecodebug_write_mem(m_d, kStart + 0x10, &halt, 1) == 1);
        adamcore_z80_regs regs = regsNow();
        char err[200] = "";
        regs.pc = kStart + 0x10;
        regs.iff1 = regs.iff2 = 0;
        QVERIFY(colecodebug_regs_set(m_d, &regs, err, sizeof err));
        const uint64_t before = colecodebug_stop_serial(m_d);
        colecodebug_step_into(m_d);
        QTRY_VERIFY_WITH_TIMEOUT(colecodebug_stop_serial(m_d) != before && colecodebug_is_paused(m_d), 3000);
        QVERIFY(regsNow().halted);
        const QList<QLineEdit *> fields = regFields();
        fields[7]->setText(QStringLiteral("$7000"));
        QTest::keyClick(fields[7], Qt::Key_Return);
        QVERIFY(regsNow().halted);
        const QString shown = m_win->findChild<QStatusBar *>()->currentMessage();
        QVERIFY2(shown.contains(QStringLiteral("halt"), Qt::CaseInsensitive), qPrintable(shown));
        QVERIFY(loadProgram());
    }

    /* a breakpoint that fires while the window is hidden brings it back */
    void hiddenBreakpointShowsWindow()
    {
        if (!m_machine) QSKIP("needs a machine (the ColecoVision BIOS)");
        colecodebug_breakpoint_clear(m_d);
        QVERIFY(loadProgram());
        char err[200] = "";
        QVERIFY(colecodebug_breakpoint_add(m_d, COLECODEBUG_BP_EXEC, kStart + 2, kStart + 2, "", err, sizeof err) > 0);
        /* NOP; NOP; ... the third NOP is the breakpoint; hiding lets the machine run on */
        m_win->hide();
        QTRY_VERIFY_WITH_TIMEOUT(m_win->isVisible(), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(colecodebug_is_paused(m_d), 3000);
        QCOMPARE(readPc(), int(kStart + 2));
        colecodebug_breakpoint_clear(m_d);
    }

    /* COLECO_DEBUGGER_POPOUT peels a tab off at the start */
    void popoutEnv()
    {
        discardWindow();
        qputenv("COLECO_DEBUGGER_POPOUT", "sound");
        DebuggerWindow::showFor(nullptr, m_s);
        qunsetenv("COLECO_DEBUGGER_POPOUT");
        m_win = findWindow();
        QVERIFY(m_win);
        m_tabs = m_win->findChild<QTabWidget *>();
        QCOMPARE(m_tabs->count(), 11);
        QWidget *window = nullptr;
        for (QWidget *top : QApplication::topLevelWidgets())
            if (top->windowTitle().endsWith(QLatin1String("Sound"))) window = top;
        QVERIFY(window);
        discardWindow();
        for (QWidget *top : QApplication::topLevelWidgets())
            if (top->windowTitle().endsWith(QLatin1String("Sound"))) top->close();
        openWindow();
    }

    /* COLECO_DEBUGGER_WATCH, COLECO_DEBUGGER_WATCH_BREAK, COLECO_DEBUGGER_MEM_GOTO and COLECO_DEBUGGER_HISTORY act in a fresh window */
    void envHooks()
    {
        if (!m_machine) QSKIP("needs a machine (the ColecoVision BIOS)");
        colecodebug_breakpoint_clear(m_d);
        colecowatch_clear();
        discardWindow();
        qputenv("COLECO_DEBUGGER_TAB", "watches");
        qputenv("COLECO_DEBUGGER_WATCH", "$7300");
        qputenv("COLECO_DEBUGGER_WATCH_BREAK", "$7300");
        qputenv("COLECO_DEBUGGER_MEM_GOTO", "$7300");
        qputenv("COLECO_DEBUGGER_HISTORY", "1");
        DebuggerWindow::showFor(nullptr, m_s);
        qunsetenv("COLECO_DEBUGGER_TAB");
        qunsetenv("COLECO_DEBUGGER_WATCH");
        qunsetenv("COLECO_DEBUGGER_WATCH_BREAK");
        qunsetenv("COLECO_DEBUGGER_MEM_GOTO");
        qunsetenv("COLECO_DEBUGGER_HISTORY");
        m_win = findWindow();
        QVERIFY(m_win);
        m_tabs = m_win->findChild<QTabWidget *>();
        QTRY_COMPARE_WITH_TIMEOUT(colecowatch_count(), 1, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(colecodebug_history_enabled(m_d), 3000);
        colecodebug_breakpoint bps[4];
        QTRY_COMPARE_WITH_TIMEOUT(colecodebug_breakpoint_list(m_d, bps, 4), 1, 3000);
        QCOMPARE(QString::fromUtf8(bps[0].condition), QStringLiteral("value != old"));
        /* MEM_GOTO put its address in the Memory view (the tab stays the one asked for) */
        QCOMPARE(m_tabs->currentIndex(), 4);
        m_tabs->setCurrentIndex(2);
        auto *dump = m_tabs->widget(2)->findChildren<QPlainTextEdit *>().last();
        QTRY_VERIFY_WITH_TIMEOUT(dump->toPlainText().contains(QStringLiteral("$7300:")), 3000);
        colecodebug_history_enable(m_d, 0);
        colecodebug_breakpoint_clear(m_d);
        colecowatch_clear();
    }
};

QTEST_MAIN(DebuggerTest)
#include "debugger_test.moc"
