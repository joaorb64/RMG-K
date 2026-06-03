/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef DEBUGGERDIALOG_HPP
#define DEBUGGERDIALOG_HPP

#include <QDialog>
#include <QTableWidget>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QSplitter>
#include <QTimer>
#include <QListWidget>
#include <QEvent>

#include <cstdint>
#include <map>
#include <set>

namespace UserInterface
{
namespace Dialog
{

class DebuggerDialog : public QDialog
{
    Q_OBJECT

  public:
    DebuggerDialog(QWidget* parent = nullptr);
    ~DebuggerDialog();

    void OnDebuggerUpdate(unsigned int pc);

  protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

  private slots:
    void onStepClicked(void);
    void onRunClicked(void);
    void onPauseClicked(void);
    void onToggleBreakpointsClicked(void);
    void onJumpToAddress(void);
    void onAddBreakpoint(void);
    void onRemoveBreakpoint(void);
    void onInstrCellChanged(int row, int col);
    void onAsmCellClicked(int row, int col);
    void onAsmContextMenu(const QPoint& pos);
    void onGPRItemChanged(QTableWidgetItem* item);
    void onFPItemChanged(QTableWidgetItem* item);
    void onRefreshTimer(void);

  private:
    // Toolbar row
    QLabel*      pcLabel         = nullptr;
    QLineEdit*   jumpAddressEdit = nullptr;
    QPushButton* jumpButton      = nullptr;
    QPushButton* stepButton      = nullptr;
    QPushButton* runButton       = nullptr;
    QPushButton* pauseButton     = nullptr;
    QPushButton* bpToggleButton  = nullptr;

    // Disassembly table (columns: Gutter | Address | Hex | Mnemonic | Args)
    QTableWidget* asmTable = nullptr;

    // Right-side panel
    QTabWidget*  rightTabs       = nullptr;
    QListWidget* breakpointList  = nullptr;
    QPushButton* addBpButton     = nullptr;
    QPushButton* removeBpButton  = nullptr;
    QTableWidget* gprTable       = nullptr;
    QTableWidget* fpTable        = nullptr;

    QTimer* refreshTimer   = nullptr;
    QLabel* asmStatusLabel = nullptr;

    // State
    uint32_t currentPC       = 0;
    uint32_t disasmBaseAddr  = 0x80000000; // KSEG0 base — where game code lives
    bool     m_updatingRegs  = false; // guard against recursive itemChanged
    bool     m_breakpointsEnabled = true; // global breakpoint enable/disable
    bool     m_isPaused      = false; // true when paused at a breakpoint

    // addr → original opcode (before user edited it)
    std::map<uint32_t, uint32_t> m_changedInstructions;

    // addr → enabled (true = stops, false = doesn't stop but shows semi-transparent)
    std::map<uint32_t, bool> m_breakpointAddrs;

    void buildUI(void);
    void refreshDisasm(uint32_t baseAddr);
    void refreshRegisters(void);
    void refreshBreakpoints(void);
    void updatePCLabel(uint32_t pc);
    void setButtonStates(bool paused);
    void scrollAsmView(int steps);  // steps * 4 bytes per step
    void updateAsmTableHighlight();  // refresh table for PC highlight without scrolling
    void revertLine(int row);
    void writeNop(int row);

    // Column indices: Gutter | Address | Hex | Instruction (mnemonic+args combined)
    static constexpr int COL_GUTTER = 0;
    static constexpr int COL_ADDR   = 1;
    static constexpr int COL_HEX    = 2;
    static constexpr int COL_INSTR  = 3;
    static constexpr int COL_COUNT  = 4;

    static constexpr int kDisasmRows   = 40;
    static constexpr int kRowHeight    = 16;
    static constexpr int kGutterWidth  = 16;
    static constexpr int kAddrColWidth = 92;
    static constexpr int kHexColWidth  = 88;
};

} // namespace Dialog
} // namespace UserInterface

#endif // DEBUGGERDIALOG_HPP
