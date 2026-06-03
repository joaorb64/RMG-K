/*
/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "DebuggerDialog.hpp"

#include <RMG-Core/Debugger.hpp>
#include <RMG-Core/Emulation.hpp>

#include <QFont>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QMessageBox>
#include <QMetaObject>
#include <QString>
#include <QKeySequence>
#include <QApplication>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QMenu>
#include <QAction>
#include <QScrollBar>

#ifdef DEBUGGER_ENABLED
#include <keystone/keystone.h>
#endif

using namespace UserInterface::Dialog;

// ── Gutter delegate: paints a red circle when a breakpoint is set ─────────
// Qt::UserRole = has breakpoint, Qt::UserRole+1 = is enabled
class GutterDelegate : public QStyledItemDelegate
{
public:
    explicit GutterDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}

    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override
    {
        QStyledItemDelegate::paint(p, opt, idx);
        if (!idx.data(Qt::UserRole).toBool()) return;

        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const int margin = 2;
        int sz = qMin(opt.rect.width(), opt.rect.height()) - margin * 2;
        if (sz < 1) { p->restore(); return; }
        QRect cr(opt.rect.left() + (opt.rect.width()  - sz) / 2,
                 opt.rect.top()  + (opt.rect.height() - sz) / 2,
                 sz, sz);

        bool enabled = idx.data(Qt::UserRole + 1).toBool();
        if (enabled) {
            p->setBrush(QColor(220, 40, 40));
            p->setPen(QColor(160, 0, 0));
        } else {
            p->setBrush(QColor(220, 40, 40, 100)); // semi-transparent
            p->setPen(QColor(160, 0, 0, 100));
        }
        p->drawEllipse(cr);
        p->restore();
    }
};

// ── Format assembly output: remove $ and convert immediates to hex ─────────
static QString formatAsmOutput(const char* opStr, const char* argsStr)
{
    QString result = QString("%1 %2").arg(opStr, argsStr);

    // Remove all $ signs
    result.replace("$", "");

    // Convert decimal numbers to hex: ,-96 → ,-0x60, ,255 → ,0xff etc.
    QString output;
    int i = 0;
    while (i < result.length()) {
        if ((result[i] == ',' || result[i] == ' ') && i + 1 < result.length() &&
            (result[i+1].isDigit() || (result[i+1] == '-' && i + 2 < result.length() && result[i+2].isDigit()))) {
            // Found a number, extract it
            int start = i + 1;
            int end = start;
            if (result[start] == '-') end++;
            while (end < result.length() && result[end].isDigit()) end++;

            QString numStr = result.mid(start, end - start);
            bool ok;
            int value = numStr.toInt(&ok);
            if (ok && value != 0) { // Don't convert 0
                output += result[i]; // comma or space
                if (value < 0) {
                    output += QString::asprintf("-0x%x", -value);
                } else {
                    output += QString::asprintf("0x%x", value);
                }
                i = end;
            } else {
                output += result[i];
                i++;
            }
        } else {
            output += result[i];
            i++;
        }
    }

    return output;
}

// ── GPR register names ────────────────────────────────────────────────────
static const char* kGPRNames[32] = {
    "zero", "at", "v0", "v1",
    "a0",   "a1", "a2", "a3",
    "t0",   "t1", "t2", "t3",  "t4", "t5", "t6", "t7",
    "s0",   "s1", "s2", "s3",  "s4", "s5", "s6", "s7",
    "t8",   "t9", "k0", "k1",
    "gp",   "sp", "fp", "ra"
};

// ── Constructor / Destructor ──────────────────────────────────────────────
DebuggerDialog::DebuggerDialog(QWidget* parent) : QDialog(parent)
{
    // Initialize the debugger engine now that the dialog is being opened
    // (this was deferred from startup to avoid savestate crashes)
    CoreDebuggerInit();

    buildUI();

    this->refreshTimer = new QTimer(this);
    this->refreshTimer->setInterval(250);
    connect(this->refreshTimer, &QTimer::timeout, this, &DebuggerDialog::onRefreshTimer);

    this->asmTable->viewport()->installEventFilter(this);
    this->asmTable->installEventFilter(this);

    // Show KSEG0 base immediately so the table isn't empty before first pause
    refreshDisasm(this->disasmBaseAddr);
}

DebuggerDialog::~DebuggerDialog()
{
    // Do NOT clear the mupen64plus callbacks here — the pre-execute stub
    // callbacks must remain active while emulation runs so that g_DebuggerActive
    // stays 1 and breakpoints keep working. The UI handler and run-state are
    // managed by on_Action_View_Debugger's destroyed-signal lambda.
    this->refreshTimer->stop();
}

// ── buildUI ───────────────────────────────────────────────────────────────
void DebuggerDialog::buildUI()
{
    this->setWindowTitle("N64 Debugger");
    this->setWindowIcon(QIcon(":Resource/RMG.png"));
    this->setWindowFlags(this->windowFlags() | Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint);
    this->resize(1150, 700);

    QFont mono("monospace");
    mono.setPointSize(8);
#ifdef _WIN32
    mono.setStyleHint(QFont::TypeWriter);
#endif

    // ── Root layout ────────────────────────────────────────────────────────
    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(4);

    // ── Toolbar row ────────────────────────────────────────────────────────
    QHBoxLayout* toolbarRow = new QHBoxLayout();
    toolbarRow->setSpacing(4);

    this->pcLabel = new QLabel("PC: 0x00000000", this);
    this->pcLabel->setFont(mono);
    this->pcLabel->setMinimumWidth(160);
    toolbarRow->addWidget(this->pcLabel);

    toolbarRow->addWidget(new QLabel("Jump:", this));
    this->jumpAddressEdit = new QLineEdit(this);
    this->jumpAddressEdit->setPlaceholderText("0x80000000");
    this->jumpAddressEdit->setMaximumWidth(120);
    this->jumpAddressEdit->setFont(mono);
    toolbarRow->addWidget(this->jumpAddressEdit);

    this->jumpButton = new QPushButton("Go", this);
    this->jumpButton->setMaximumWidth(36);
    toolbarRow->addWidget(this->jumpButton);

    toolbarRow->addStretch();

    this->stepButton  = new QPushButton("Step (F10)", this);
    this->runButton   = new QPushButton("Run", this);
    this->pauseButton = new QPushButton("Pause", this);
    this->bpToggleButton = new QPushButton("Breakpoints: ON", this);
    toolbarRow->addWidget(this->stepButton);
    toolbarRow->addWidget(this->runButton);
    toolbarRow->addWidget(this->pauseButton);
    toolbarRow->addWidget(this->bpToggleButton);

    rootLayout->addLayout(toolbarRow);

    // ── Splitter ──────────────────────────────────────────────────────────
    QSplitter* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);

    // ── Disassembly table ─────────────────────────────────────────────────
    // Columns: Gutter | Address | Hex | Instruction (mnemonic+args, editable)
    this->asmTable = new QTableWidget(kDisasmRows, COL_COUNT, this);
    this->asmTable->setFont(mono);
    this->asmTable->setHorizontalHeaderLabels({"", "Address", "Hex", "Instruction"});

    this->asmTable->horizontalHeader()->setSectionResizeMode(COL_GUTTER, QHeaderView::Fixed);
    this->asmTable->horizontalHeader()->setSectionResizeMode(COL_ADDR,   QHeaderView::Fixed);
    this->asmTable->horizontalHeader()->setSectionResizeMode(COL_HEX,    QHeaderView::Fixed);
    this->asmTable->horizontalHeader()->setSectionResizeMode(COL_INSTR,  QHeaderView::Stretch);
    this->asmTable->setColumnWidth(COL_GUTTER, kGutterWidth);
    this->asmTable->setColumnWidth(COL_ADDR,   kAddrColWidth);
    this->asmTable->setColumnWidth(COL_HEX,    kHexColWidth);

    this->asmTable->setItemDelegateForColumn(COL_GUTTER, new GutterDelegate(this));
    this->asmTable->verticalHeader()->setVisible(false);
    this->asmTable->verticalHeader()->setDefaultSectionSize(kRowHeight);
    this->asmTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    this->asmTable->setSelectionMode(QAbstractItemView::SingleSelection);
    this->asmTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    this->asmTable->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->asmTable->setContextMenuPolicy(Qt::CustomContextMenu);

    splitter->addWidget(this->asmTable);

    // ── Right panel ───────────────────────────────────────────────────────
    this->rightTabs = new QTabWidget(this);
    this->rightTabs->setMinimumWidth(240);

    // ---- Breakpoints tab ----
    QWidget* bpWidget = new QWidget(this);
    QVBoxLayout* bpLayout = new QVBoxLayout(bpWidget);
    bpLayout->setContentsMargins(4, 4, 4, 4);
    bpLayout->setSpacing(3);

    this->breakpointList = new QListWidget(bpWidget);
    this->breakpointList->setFont(mono);
    bpLayout->addWidget(this->breakpointList);

    QHBoxLayout* bpBtns = new QHBoxLayout();
    this->removeBpButton = new QPushButton("Remove", bpWidget);
    bpBtns->addWidget(this->removeBpButton);
    bpBtns->addStretch();
    bpLayout->addLayout(bpBtns);
    this->rightTabs->addTab(bpWidget, "Breakpoints");

    // ---- GPR registers tab ----
    this->gprTable = new QTableWidget(34, 2, this); // 32 GPR + HI + LO
    this->gprTable->setFont(mono);
    this->gprTable->setHorizontalHeaderLabels({"Reg", "Value"});
    this->gprTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    this->gprTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    this->gprTable->setColumnWidth(0, 44);
    this->gprTable->verticalHeader()->setVisible(false);
    this->gprTable->verticalHeader()->setDefaultSectionSize(kRowHeight);
    this->gprTable->setSelectionMode(QAbstractItemView::SingleSelection);
    this->gprTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);

    for (int i = 0; i < 32; ++i)
    {
        QTableWidgetItem* nameItem = new QTableWidgetItem(kGPRNames[i]);
        nameItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        this->gprTable->setItem(i, 0, nameItem);
        this->gprTable->setItem(i, 1, new QTableWidgetItem("0x0000000000000000"));
    }
    for (int i = 32; i <= 33; ++i) // HI, LO — read-only
    {
        QTableWidgetItem* n = new QTableWidgetItem(i == 32 ? "hi" : "lo");
        n->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        this->gprTable->setItem(i, 0, n);
        QTableWidgetItem* v = new QTableWidgetItem("0x0000000000000000");
        v->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        this->gprTable->setItem(i, 1, v);
    }
    this->rightTabs->addTab(this->gprTable, "GPR");

    // ---- FP registers tab ----
    this->fpTable = new QTableWidget(32, 2, this);
    this->fpTable->setFont(mono);
    this->fpTable->setHorizontalHeaderLabels({"Reg", "Value"});
    this->fpTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    this->fpTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    this->fpTable->setColumnWidth(0, 30);
    this->fpTable->verticalHeader()->setVisible(false);
    this->fpTable->verticalHeader()->setDefaultSectionSize(kRowHeight);
    this->fpTable->setSelectionMode(QAbstractItemView::SingleSelection);
    this->fpTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);

    for (int i = 0; i < 32; ++i)
    {
        QTableWidgetItem* n = new QTableWidgetItem(QString("f%1").arg(i));
        n->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        this->fpTable->setItem(i, 0, n);
        this->fpTable->setItem(i, 1, new QTableWidgetItem("0.0"));
    }
    this->rightTabs->addTab(this->fpTable, "FP");

    splitter->addWidget(this->rightTabs);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    rootLayout->addWidget(splitter, 1);

    // ── Connections ────────────────────────────────────────────────────────
    connect(this->stepButton,  &QPushButton::clicked, this, &DebuggerDialog::onStepClicked);
    connect(this->runButton,   &QPushButton::clicked, this, &DebuggerDialog::onRunClicked);
    connect(this->pauseButton, &QPushButton::clicked, this, &DebuggerDialog::onPauseClicked);
    connect(this->bpToggleButton, &QPushButton::clicked, this, &DebuggerDialog::onToggleBreakpointsClicked);
    connect(this->jumpButton,  &QPushButton::clicked, this, &DebuggerDialog::onJumpToAddress);
    connect(this->jumpAddressEdit, &QLineEdit::returnPressed, this, &DebuggerDialog::onJumpToAddress);
    connect(this->removeBpButton, &QPushButton::clicked, this, &DebuggerDialog::onRemoveBreakpoint);
    connect(this->asmTable, &QTableWidget::cellChanged, this, &DebuggerDialog::onInstrCellChanged);
    connect(this->asmTable, &QTableWidget::cellClicked, this, &DebuggerDialog::onAsmCellClicked);
    connect(this->asmTable, &QTableWidget::customContextMenuRequested,
            this, &DebuggerDialog::onAsmContextMenu);
    connect(this->gprTable, &QTableWidget::itemChanged, this, &DebuggerDialog::onGPRItemChanged);
    connect(this->fpTable,  &QTableWidget::itemChanged, this, &DebuggerDialog::onFPItemChanged);

    // Assembly status line (shown below the splitter)
    this->asmStatusLabel = new QLabel(this);
    this->asmStatusLabel->setFont(mono);
    rootLayout->addWidget(this->asmStatusLabel);

    QAction* stepAction = new QAction(this);
    stepAction->setShortcut(QKeySequence(Qt::Key_F10));
    connect(stepAction, &QAction::triggered, this, &DebuggerDialog::onStepClicked);
    this->addAction(stepAction);

    setButtonStates(false);
}

// ── Event filter: wheel + arrow keys scroll the memory view ──────────────
bool DebuggerDialog::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == this->asmTable->viewport() && event->type() == QEvent::Wheel)
    {
        QWheelEvent* we = static_cast<QWheelEvent*>(event);
        scrollAsmView(-we->angleDelta().y() / 120);
        return true;
    }
    if (obj == this->asmTable && event->type() == QEvent::KeyPress)
    {
        QKeyEvent* ke = static_cast<QKeyEvent*>(event);
        switch (ke->key())
        {
            case Qt::Key_Up:       scrollAsmView(-1);          return true;
            case Qt::Key_Down:     scrollAsmView(+1);          return true;
            case Qt::Key_PageUp:   scrollAsmView(-kDisasmRows); return true;
            case Qt::Key_PageDown: scrollAsmView(+kDisasmRows); return true;
            default: break;
        }
    }
    return QDialog::eventFilter(obj, event);
}

void DebuggerDialog::scrollAsmView(int steps)
{
    refreshDisasm((this->disasmBaseAddr + (uint32_t)(steps * 4)) & ~3u);
}

// Refresh table without changing scroll position
void DebuggerDialog::updateAsmTableHighlight()
{
    // Rebuild the table in-place to update PC highlight without scrolling
    this->asmTable->blockSignals(true);
    for (int row = 0; row < 40; ++row)
    {
        uint32_t addr = this->disasmBaseAddr + (uint32_t)(row * 4);
        uint32_t instr = CoreDebugMemRead32(addr);

        char op[64]    = {};
        char args[256] = {};
        CoreDebugDecodeOp(instr, op, args, addr);

        bool isPC      = (addr == this->currentPC);
        bool isChanged = this->m_changedInstructions.count(addr) > 0;

        QColor bg = Qt::transparent;
        if (isPC)           bg = QColor(40, 100, 40);
        else if (isChanged) bg = QColor(90, 70, 0);

        auto makeCell = [&](const QString& text, bool readOnly) -> QTableWidgetItem* {
            QTableWidgetItem* item = new QTableWidgetItem(text);
            Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
            if (!readOnly) flags |= Qt::ItemIsEditable;
            item->setFlags(flags);
            item->setBackground(bg);
            return item;
        };

        QString formattedAsm = formatAsmOutput(op, args);
        QString instrStr = formattedAsm;

        if (auto* item = this->asmTable->item(row, COL_INSTR)) {
            item->setBackground(bg);
        }
        if (auto* item = this->asmTable->item(row, COL_HEX)) {
            item->setBackground(bg);
        }
        if (auto* item = this->asmTable->item(row, COL_ADDR)) {
            item->setBackground(bg);
        }
    }
    this->asmTable->blockSignals(false);
    this->asmTable->viewport()->repaint();
}

// ── OnDebuggerUpdate (received on UI thread via queued invoke) ────────────
void DebuggerDialog::OnDebuggerUpdate(unsigned int pc)
{
    // If breakpoints are globally disabled, resume running
    if (!m_breakpointsEnabled) {
        CoreDebugSetRunState(CoreDebugRunState::Running);
        CoreDebugStep(); // Post semaphore to unblock emulation thread
        return;
    }

    // If we hit a disabled breakpoint, resume running without showing the UI
    auto it = this->m_breakpointAddrs.find(pc);
    if (it != this->m_breakpointAddrs.end() && !it->second) {
        CoreDebugSetRunState(CoreDebugRunState::Running);
        CoreDebugStep(); // Post semaphore to unblock emulation thread
        return;
    }

    // Pause emulation when breakpoint is hit
    // Do NOT call CoreDebugStep() here — the emulation thread is already
    // blocked on SDL_SemWait inside update_debugger(). Only Step/Run should unblock it.
    CoreDebugSetRunState(CoreDebugRunState::Paused);

    this->currentPC = pc;
    this->m_isPaused = true;
    updatePCLabel(pc);

    // Find last visible row by checking which row is at the bottom of the viewport
    int lastVisibleRow = this->asmTable->rowAt(this->asmTable->viewport()->height() - 1);
    if (lastVisibleRow < 0) lastVisibleRow = kDisasmRows - 1;
    lastVisibleRow--; // Adjust for overshooting

    uint32_t minAddr = this->disasmBaseAddr;
    uint32_t maxAddr = this->disasmBaseAddr + (lastVisibleRow * 4);

    if (pc < minAddr) {
        // PC is above visible range - put it at row 0 (top)
        uint32_t newBase = pc & ~3u;
        refreshDisasm(newBase);
    } else if (pc > maxAddr) {
        // PC is below visible range - put it at the last visible row
        uint32_t newBase = (pc >= lastVisibleRow * 4) ? (pc - lastVisibleRow * 4) : 0;
        newBase = newBase & ~3u;
        refreshDisasm(newBase);
    } else {
        // PC is on-screen, just update highlight
        updateAsmTableHighlight();
    }

    refreshRegisters();
    refreshBreakpoints();
    setButtonStates(true);
    this->refreshTimer->stop();
}

void DebuggerDialog::updatePCLabel(uint32_t pc)
{
    this->pcLabel->setText(QString("PC: 0x%1").arg(pc, 8, 16, QChar('0')).toUpper());
}

// ── refreshDisasm ─────────────────────────────────────────────────────────
void DebuggerDialog::refreshDisasm(uint32_t baseAddr)
{
    this->disasmBaseAddr = baseAddr;
    // Keep the jump field in sync so the user can copy the visible address
    this->jumpAddressEdit->setText(QString("0x%1").arg(baseAddr, 8, 16, QChar('0')).toUpper());
    this->asmTable->blockSignals(true);

    for (int row = 0; row < kDisasmRows; ++row)
    {
        uint32_t addr  = baseAddr + (uint32_t)(row * 4);
        uint32_t instr = CoreDebugMemRead32(addr);

        char op[64]    = {};
        char args[256] = {};
        CoreDebugDecodeOp(instr, op, args, addr);

        bool isPC      = (addr == this->currentPC);
        bool isChanged = this->m_changedInstructions.count(addr) > 0;
        bool isBp      = this->m_breakpointAddrs.count(addr) > 0;

        QColor bg = Qt::transparent;
        if (isPC)           bg = QColor(40, 100, 40);
        else if (isChanged) bg = QColor(90, 70, 0);

        QString formattedAsm = formatAsmOutput(op, args);

        auto makeCell = [&](const QString& text, bool readOnly) -> QTableWidgetItem* {
            QTableWidgetItem* item = new QTableWidgetItem(text);
            Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
            if (!readOnly) flags |= Qt::ItemIsEditable;
            item->setFlags(flags);
            item->setBackground(bg);
            return item;
        };

        // Use formatted assembly (no $ prefix, immediates in hex)
        QString instrStr = formattedAsm;

        QTableWidgetItem* gutterItem = new QTableWidgetItem();
        gutterItem->setFlags(Qt::ItemIsEnabled);
        gutterItem->setData(Qt::UserRole, isBp);
        if (isBp) {
            bool isEnabled = m_breakpointAddrs[addr] && this->m_breakpointsEnabled;
            gutterItem->setData(Qt::UserRole + 1, isEnabled);
        }
        gutterItem->setBackground(bg);

        this->asmTable->setItem(row, COL_GUTTER, gutterItem);
        this->asmTable->setItem(row, COL_ADDR,   makeCell(QString("0x%1").arg(addr, 8, 16, QChar('0')).toUpper(), true));
        this->asmTable->setItem(row, COL_HEX,    makeCell(QString("%1").arg(instr, 8, 16, QChar('0')).toUpper(), true));
        this->asmTable->setItem(row, COL_INSTR,  makeCell(instrStr, false));
    }

    this->asmTable->blockSignals(false);
    this->asmTable->viewport()->update();
}

// ── refreshRegisters ──────────────────────────────────────────────────────
void DebuggerDialog::refreshRegisters()
{
    this->m_updatingRegs = true;
    for (int i = 0; i < 32; ++i)
    {
        int64_t val = CoreDebugGetGPRRegister((CoreDebugger::GPRRegister)i);
        if (auto* it = this->gprTable->item(i, 1))
            it->setText(QString("0x%1").arg((uint64_t)val, 16, 16, QChar('0')).toUpper());
    }
    for (int i = 0; i < 32; ++i)
    {
        double val = CoreDebugGetFPRegister((CoreDebugger::FPRegister)i);
        if (auto* it = this->fpTable->item(i, 1))
            it->setText(QString::number(val, 'g', 10));
    }
    this->m_updatingRegs = false;
}

// ── refreshBreakpoints ────────────────────────────────────────────────────
void DebuggerDialog::refreshBreakpoints()
{
    this->breakpointList->clear();
    this->m_breakpointAddrs.clear();

    for (const auto& bpt : CoreDebugGetBreakpoints())
    {
        this->m_breakpointAddrs[bpt.address] = true; // breakpoints from mupen64plus are enabled
        QString typeStr;
        if (bpt.isFetch())  typeStr += "X";
        if (bpt.isRead())   typeStr += "R";
        if (bpt.isWrite())  typeStr += "W";
        if (!bpt.isEnabled()) typeStr += "(off)";
        auto* item = new QListWidgetItem(
            QString("[%1] 0x%2").arg(typeStr).arg(bpt.address, 8, 16, QChar('0')).toUpper(),
            this->breakpointList);
        item->setData(Qt::UserRole, bpt.address);
    }

    // Refresh gutter column to reflect updated breakpoint set
    this->asmTable->blockSignals(true);
    for (int row = 0; row < kDisasmRows; ++row)
    {
        uint32_t addr = this->disasmBaseAddr + (uint32_t)(row * 4);
        if (auto* g = this->asmTable->item(row, COL_GUTTER))
        {
            bool hasBp = this->m_breakpointAddrs.count(addr) > 0;
            g->setData(Qt::UserRole, hasBp);
            if (hasBp) {
                // Enabled only if both the breakpoint and global flag are enabled
                bool enabledState = this->m_breakpointAddrs[addr] && this->m_breakpointsEnabled;
                g->setData(Qt::UserRole + 1, enabledState);
            }
        }
    }
    this->asmTable->blockSignals(false);
    this->asmTable->viewport()->update();
}

// ── setButtonStates ───────────────────────────────────────────────────────
void DebuggerDialog::setButtonStates(bool paused)
{
    bool running = CoreIsEmulationRunning();
    this->stepButton->setEnabled(paused && running);
    this->runButton->setEnabled(paused && running);
    this->pauseButton->setEnabled(!paused && running);
}

// ── Step / Run / Pause ────────────────────────────────────────────────────
void DebuggerDialog::onStepClicked()
{
    CoreDebugStep();
    this->refreshTimer->start();
}

void DebuggerDialog::onRunClicked()
{
    this->m_isPaused = false;
    CoreDebugSetRunState(CoreDebugRunState::Running);
    // The emulation thread is blocked on SDL_SemWait(sem_pending_steps).
    // Setting run state alone only changes the flag — DebugStep() posts
    // the semaphore so the thread actually unblocks.
    CoreDebugStep();
    setButtonStates(false);
    this->refreshTimer->stop();
}

void DebuggerDialog::onPauseClicked()
{
    CoreDebugSetRunState(CoreDebugRunState::Paused);
    this->refreshTimer->start();
}

void DebuggerDialog::onToggleBreakpointsClicked()
{
    this->m_breakpointsEnabled = !this->m_breakpointsEnabled;
    this->bpToggleButton->setText(this->m_breakpointsEnabled ? "Breakpoints: ON" : "Breakpoints: OFF");

    // Update visual state (semi-transparent when disabled)
    refreshBreakpoints();
}

// ── Jump to address ───────────────────────────────────────────────────────
void DebuggerDialog::onJumpToAddress()
{
    QString text = this->jumpAddressEdit->text().trimmed();
    if (text.isEmpty()) return;

    bool ok = false;
    QString hex = text.startsWith("0x", Qt::CaseInsensitive) ? text.mid(2) : text;
    uint32_t addr = hex.toUInt(&ok, 16);
    if (!ok)
    {
        QMessageBox::warning(this, "Invalid address", "Enter a valid hex address (e.g. 0x80131EB0).");
        return;
    }
    refreshDisasm(addr & ~3u);
}

// ── Breakpoint add/remove via panel ──────────────────────────────────────
void DebuggerDialog::onAddBreakpoint()
{
    fprintf(stderr, "[DEBUGGER] onAddBreakpoint clicked\n");
    bool ok = false;
    QString text = QInputDialog::getText(this, "Add Breakpoint", "Address (hex):",
                                         QLineEdit::Normal, QString(), &ok);
    fprintf(stderr, "[DEBUGGER] Dialog: ok=%d, text='%s'\n", ok, text.toStdString().c_str());
    if (!ok || text.isEmpty()) return;

    bool parseOk = false;
    QString hex = text.startsWith("0x", Qt::CaseInsensitive) ? text.mid(2) : text;
    uint32_t addr = hex.toUInt(&parseOk, 16);
    fprintf(stderr, "[DEBUGGER] Parse: ok=%d, addr=0x%08x\n", parseOk, addr);
    if (!parseOk)
    {
        QMessageBox::warning(this, "Invalid address", "Enter a valid hex address.");
        return;
    }
    fprintf(stderr, "[DEBUGGER] Calling CoreDebugAddFetchBreakpoint\n");
    CoreDebugAddFetchBreakpoint(addr);
    refreshBreakpoints();
}

void DebuggerDialog::onRemoveBreakpoint()
{
    if (auto* item = this->breakpointList->currentItem())
    {
        CoreDebugRemoveBreakpoint(item->data(Qt::UserRole).toUInt());
        refreshBreakpoints();
    }
}

// ── Gutter click: toggle breakpoint ──────────────────────────────────────
void DebuggerDialog::onAsmCellClicked(int row, int col)
{
    if (col != COL_GUTTER) return;
    uint32_t addr = this->disasmBaseAddr + (uint32_t)(row * 4);

    if (this->m_breakpointAddrs.count(addr)) {
        // Has breakpoint -> remove it
        CoreDebugRemoveBreakpoint(addr);
        this->m_breakpointAddrs.erase(addr);
    } else {
        // No breakpoint -> add it
        CoreDebugAddFetchBreakpoint(addr);
        this->m_breakpointAddrs[addr] = true;
    }
    refreshBreakpoints();
}

// ── MIPS assembly preprocessor ────────────────────────────────────────────
// instrAddr: virtual address of the instruction being assembled (needed to
//            convert absolute branch targets to PC-relative word offsets).
static QString preprocessMipsAsm(const QString& line, uint32_t instrAddr)
{
    static const char* const kRegs[] = {
        "zero","r0","at","v0","v1",
        "a0","a1","a2","a3",
        "t0","t1","t2","t3","t4","t5","t6","t7",
        "s0","s1","s2","s3","s4","s5","s6","s7",
        "t8","t9","k0","k1","gp","sp","s8","fp","ra",
        nullptr
    };

    QString out = line.trimmed();

    // 'b target' is now a native Keystone pseudo — no expansion needed here.
    // Just leave it as-is so Keystone's own handler runs.

    // Add $ to bare register names
    for (int i = 0; kRegs[i]; ++i)
    {
        QRegularExpression re(
            QString(R"((?<!\$)\b(%1)\b)").arg(kRegs[i]),
            QRegularExpression::CaseInsensitiveOption);
        out.replace(re, "$\\1");
    }

    // Branch offset conversion is now handled inside Keystone's 'b' pseudo handler.

    return out;
}

// ── Instruction cell edit: assemble + write to memory ────────────────────
void DebuggerDialog::onInstrCellChanged(int row, int col)
{
    if (col != COL_INSTR) return;

#ifdef DEBUGGER_ENABLED
    QTableWidgetItem* instrItem = this->asmTable->item(row, COL_INSTR);
    if (!instrItem) return;

    QString asmLine = instrItem->text().trimmed();
    uint32_t addr   = this->disasmBaseAddr + (uint32_t)(row * 4);

    // Read current (original) value from memory before we overwrite it
    uint32_t currentInstr = CoreDebugMemRead32(addr);

    // Normalise: add $, expand b, convert absolute targets to PC-relative offsets
    QString normalised = preprocessMipsAsm(asmLine, addr);

    ks_engine* ks = nullptr;
    if (ks_open(KS_ARCH_MIPS, KS_MODE_MIPS64 | KS_MODE_BIG_ENDIAN, &ks) != KS_ERR_OK)
        return;

    unsigned char* encode = nullptr;
    size_t size = 0, count = 0;
    std::string asmStr = normalised.toStdString();

    if (ks_asm(ks, asmStr.c_str(), addr, &encode, &size, &count) != KS_ERR_OK || size != 4)
    {
        // Show what Keystone rejected
        QString errMsg = (size != 4 && encode)
            ? QString("Expected 4 bytes, got %1 (wrong instruction?)").arg((int)size)
            : QString("Assembly error: %1  (tried: %2)")
                  .arg(ks_strerror(ks_errno(ks)))
                  .arg(normalised);
        this->asmStatusLabel->setText(errMsg);
        this->asmStatusLabel->setStyleSheet("color: red;");

        // Restore cell to what's in memory
        this->asmTable->blockSignals(true);
        char op[64] = {}, args[256] = {};
        CoreDebugDecodeOp(currentInstr, op, args, addr);
        instrItem->setText(args[0] == '\0' ? QString(op) : (QString(op) + " " + QString(args)));
        this->asmTable->blockSignals(false);
        ks_free(encode);
        ks_close(ks);
        return;
    }
    // Clear any previous error
    this->asmStatusLabel->clear();

    uint32_t newInstr = ((uint32_t)encode[0] << 24) | ((uint32_t)encode[1] << 16)
                      | ((uint32_t)encode[2] << 8)  | encode[3];
    ks_free(encode);
    ks_close(ks);

    // Track original instruction (only save once, before first edit at this address)
    bool alreadyTracked = this->m_changedInstructions.count(addr) > 0;
    if (!alreadyTracked && newInstr != currentInstr)
        this->m_changedInstructions[addr] = currentInstr;
    // If user typed back the original value, remove from changed set
    if (alreadyTracked && newInstr == this->m_changedInstructions.at(addr))
        this->m_changedInstructions.erase(addr);

    CoreDebugMemWrite32(addr, newInstr);

    // Update hex cell + row color without triggering another cellChanged
    this->asmTable->blockSignals(true);
    if (auto* hexItem = this->asmTable->item(row, COL_HEX))
        hexItem->setText(QString("%1").arg(newInstr, 8, 16, QChar('0')).toUpper());

    bool isPC      = (addr == this->currentPC);
    bool isChanged = this->m_changedInstructions.count(addr) > 0;
    QColor bg = isPC ? QColor(40, 100, 40) : (isChanged ? QColor(90, 70, 0) : QColor(Qt::transparent));
    for (int c = 0; c < COL_COUNT; ++c)
        if (auto* it = this->asmTable->item(row, c)) it->setBackground(bg);

    this->asmTable->blockSignals(false);
#endif
}

// ── Right-click context menu ──────────────────────────────────────────────
void DebuggerDialog::onAsmContextMenu(const QPoint& pos)
{
    int row = this->asmTable->rowAt(pos.y());
    if (row < 0) return;
    uint32_t addr = this->disasmBaseAddr + (uint32_t)(row * 4);

    QMenu menu(this);
    QAction* bpAction     = menu.addAction(this->m_breakpointAddrs.count(addr)
                                           ? "Remove breakpoint" : "Add breakpoint");
    menu.addSeparator();
    QAction* nopAction    = menu.addAction("Replace with NOP");
    QAction* revertAction = menu.addAction("Revert to original");
    revertAction->setEnabled(this->m_changedInstructions.count(addr) > 0);

    QAction* chosen = menu.exec(this->asmTable->viewport()->mapToGlobal(pos));
    if (!chosen) return;
    if (chosen == bpAction)          onAsmCellClicked(row, COL_GUTTER);
    else if (chosen == nopAction)    writeNop(row);
    else if (chosen == revertAction) revertLine(row);
}

void DebuggerDialog::writeNop(int row)
{
    uint32_t addr = this->disasmBaseAddr + (uint32_t)(row * 4);

    // NOP = SLL $0,$0,0 = 0x00000000
    constexpr uint32_t NOP = 0x00000000;

    // Save original if first edit at this address
    if (!this->m_changedInstructions.count(addr))
    {
        uint32_t orig = CoreDebugMemRead32(addr);
        if (orig != NOP)
            this->m_changedInstructions[addr] = orig;
    }
    else if (this->m_changedInstructions.at(addr) == NOP)
    {
        // Original was already NOP — reverted state
        this->m_changedInstructions.erase(addr);
    }

    CoreDebugMemWrite32(addr, NOP);

    this->asmTable->blockSignals(true);
    bool isPC = (addr == this->currentPC);
    bool isChanged = this->m_changedInstructions.count(addr) > 0;
    QColor bg = isPC ? QColor(40, 100, 40) : (isChanged ? QColor(90, 70, 0) : QColor(Qt::transparent));

    if (auto* h = this->asmTable->item(row, COL_HEX))   { h->setText("00000000");      h->setBackground(bg); }
    if (auto* i = this->asmTable->item(row, COL_INSTR)) { i->setText("nop");            i->setBackground(bg); }
    if (auto* g = this->asmTable->item(row, COL_GUTTER))  g->setBackground(bg);
    this->asmTable->blockSignals(false);
}

void DebuggerDialog::revertLine(int row)
{
    uint32_t addr = this->disasmBaseAddr + (uint32_t)(row * 4);
    auto it = this->m_changedInstructions.find(addr);
    if (it == this->m_changedInstructions.end()) return;

    uint32_t original = it->second;
    CoreDebugMemWrite32(addr, original);
    this->m_changedInstructions.erase(it);

    this->asmTable->blockSignals(true);
    char op[64] = {}, args[256] = {};
    CoreDebugDecodeOp(original, op, args, addr);
    QString instrStr = (args[0] == '\0') ? QString(op) : (QString(op) + " " + QString(args));

    bool isPC = (addr == this->currentPC);
    QColor bg = isPC ? QColor(40, 100, 40) : QColor(Qt::transparent);

    auto setCell = [&](int c, const QString& text) {
        if (auto* it2 = this->asmTable->item(row, c)) { it2->setText(text); it2->setBackground(bg); }
    };
    setCell(COL_HEX,   QString("%1").arg(original, 8, 16, QChar('0')).toUpper());
    setCell(COL_INSTR, instrStr);
    if (auto* g = this->asmTable->item(row, COL_GUTTER)) g->setBackground(bg);
    this->asmTable->blockSignals(false);
}

// ── Register editing ──────────────────────────────────────────────────────
void DebuggerDialog::onGPRItemChanged(QTableWidgetItem* item)
{
    if (this->m_updatingRegs || item->column() != 1 || item->row() >= 32) return;
    bool ok = false;
    QString hex = item->text().trimmed();
    if (hex.startsWith("0x", Qt::CaseInsensitive)) hex = hex.mid(2);
    uint64_t val = hex.toULongLong(&ok, 16);
    if (ok) CoreDebugSetGPRRegister((CoreDebugger::GPRRegister)item->row(), (int64_t)val);
}

void DebuggerDialog::onFPItemChanged(QTableWidgetItem* item)
{
    if (this->m_updatingRegs || item->column() != 1) return;
    bool ok = false;
    double val = item->text().toDouble(&ok);
    if (ok) CoreDebugSetFPRegister((CoreDebugger::FPRegister)item->row(), val);
}

// ── Refresh timer ─────────────────────────────────────────────────────────
void DebuggerDialog::onRefreshTimer()
{
    if (!CoreIsEmulationRunning())
    {
        this->refreshTimer->stop();
        setButtonStates(false);
        return;
    }

    // Only update UI if paused at a breakpoint, not while game is running
    if (!this->m_isPaused) {
        return;
    }

    uint32_t pc = CoreDebugGetPC();
    if (pc != this->currentPC)
    {
        this->currentPC = pc;
        updatePCLabel(pc);

        // Find last visible row by checking which row is at the bottom of the viewport
        int lastVisibleRow = this->asmTable->rowAt(this->asmTable->viewport()->height() - 1);
        if (lastVisibleRow < 0) lastVisibleRow = kDisasmRows - 1;
        lastVisibleRow--; // Adjust for overshooting

        uint32_t minAddr = this->disasmBaseAddr;
        uint32_t maxAddr = this->disasmBaseAddr + (lastVisibleRow * 4);

        if (pc < minAddr) {
            // PC is above visible range - put it at row 0 (top)
            uint32_t newBase = pc & ~3u;
            refreshDisasm(newBase);
        } else if (pc > maxAddr) {
            // PC is below visible range - put it at the last visible row
            uint32_t newBase = (pc >= lastVisibleRow * 4) ? (pc - lastVisibleRow * 4) : 0;
            newBase = newBase & ~3u;
            refreshDisasm(newBase);
        } else {
            // PC is on-screen, just update highlight without scrolling
            updateAsmTableHighlight();
        }

        refreshRegisters();
    }
}
