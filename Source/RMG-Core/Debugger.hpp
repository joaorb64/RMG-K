/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_DEBUGGER_HPP
#define CORE_DEBUGGER_HPP

#include "Library.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace CoreDebugger
{
    // CPU emulator mode
    enum class CPUMode : int
    {
        PureInterpreter = 0,
        CachedInterpreter = 1,
        DynamicRecompiler = 2
    };

    // Breakpoint info
    struct Breakpoint
    {
        uint32_t address;
        uint32_t endAddress;
        uint32_t flags;

        bool isEnabled() const { return (flags & 0x01) != 0; }
        bool isFetch() const { return (flags & 0x08) != 0; }
        bool isRead() const { return (flags & 0x02) != 0; }
        bool isWrite() const { return (flags & 0x04) != 0; }
    };

    // Register name to ID mapping
    enum class GPRRegister : int
    {
        Zero = 0, AT, V0, V1, A0, A1, A2, A3,
        T0, T1, T2, T3, T4, T5, T6, T7,
        S0, S1, S2, S3, S4, S5, S6, S7,
        T8, T9, K0, K1, GP, SP, FP, RA
    };

    // FP register name to ID mapping
    enum class FPRegister : int
    {
        F0, F1, F2, F3, F4, F5, F6, F7,
        F8, F9, F10, F11, F12, F13, F14, F15,
        F16, F17, F18, F19, F20, F21, F22, F23,
        F24, F25, F26, F27, F28, F29, F30, F31
    };
}

//
// Exported Functions
//

// Check if debugger support is available
CORE_EXPORT bool CoreDebugIsAvailable(void);

// Get current CPU emulator mode
// Returns PureInterpreter, CachedInterpreter, or DynamicRecompiler
CORE_EXPORT CoreDebugger::CPUMode CoreDebugGetCPUMode(void);

// Disassemble instruction at PC
// instr: instruction opcode
// outOp: output buffer for mnemonic (should be >= 32 bytes)
// outArgs: output buffer for arguments (should be >= 256 bytes)
// pc: program counter value for context
CORE_EXPORT void CoreDebugDecodeOp(uint32_t instr, char* outOp, char* outArgs, uint32_t pc);

// Read memory values from emulated N64 memory
CORE_EXPORT uint8_t CoreDebugMemRead8(uint32_t address);
CORE_EXPORT uint16_t CoreDebugMemRead16(uint32_t address);
CORE_EXPORT uint32_t CoreDebugMemRead32(uint32_t address);
CORE_EXPORT uint64_t CoreDebugMemRead64(uint32_t address);

// Write memory values to emulated N64 memory
CORE_EXPORT void CoreDebugMemWrite8(uint32_t address, uint8_t value);
CORE_EXPORT void CoreDebugMemWrite16(uint32_t address, uint16_t value);
CORE_EXPORT void CoreDebugMemWrite32(uint32_t address, uint32_t value);
CORE_EXPORT void CoreDebugMemWrite64(uint32_t address, uint64_t value);

// Get program counter
CORE_EXPORT uint32_t CoreDebugGetPC(void);

// Get general-purpose register value
CORE_EXPORT int64_t CoreDebugGetGPRRegister(CoreDebugger::GPRRegister reg);

// Set general-purpose register value
CORE_EXPORT void CoreDebugSetGPRRegister(CoreDebugger::GPRRegister reg, int64_t value);

// Get floating-point register value as double
CORE_EXPORT double CoreDebugGetFPRegister(CoreDebugger::FPRegister reg);

// Set floating-point register value as double
CORE_EXPORT void CoreDebugSetFPRegister(CoreDebugger::FPRegister reg, double value);

// Debugger run state
enum class CoreDebugRunState : int
{
    Paused = 0,
    Stepping = 1,
    Running = 2
};

// Set debugger callbacks (pass nullptr to disable)
// initCallback: called when debugger initializes
// updateCallback(pc): called after each CPU step when paused
// viCallback: called each vertical interrupt
CORE_EXPORT void CoreDebugSetCallbacks(void (*initCallback)(void), void (*updateCallback)(unsigned int), void (*viCallback)(void));

// Register a UI-layer function that receives PC updates when the debugger is paused.
// This is decoupled from the raw mupen64plus callbacks so the UI can change
// without re-registering C-function-pointer callbacks.
CORE_EXPORT void CoreDebugSetUIUpdateCallback(void (*handler)(unsigned int pc));

// Call this just before M64CMD_EXECUTE to register debugger callbacks.
// The actual debugger initialization is deferred to avoid savestate crashes.
CORE_EXPORT void CoreDebugPreExecuteSetup(void);

// Call this when the user opens the debugger dialog to initialize the debugger engine.
// Safe to call multiple times (no-op if already initialized).
CORE_EXPORT void CoreDebuggerInit(void);

// Set debugger run state (pause/step/run)
CORE_EXPORT void CoreDebugSetRunState(CoreDebugRunState state);

// Step CPU by one instruction (emulation must be paused first)
CORE_EXPORT void CoreDebugStep(void);

// Get all breakpoints
CORE_EXPORT std::vector<CoreDebugger::Breakpoint> CoreDebugGetBreakpoints(void);

// Add a fetch/execute breakpoint
CORE_EXPORT bool CoreDebugAddFetchBreakpoint(uint32_t address);

// Add a read breakpoint
CORE_EXPORT bool CoreDebugAddReadBreakpoint(uint32_t address, uint32_t endAddress);

// Add a write breakpoint
CORE_EXPORT bool CoreDebugAddWriteBreakpoint(uint32_t address, uint32_t endAddress);

// Remove a breakpoint by address
CORE_EXPORT bool CoreDebugRemoveBreakpoint(uint32_t address);

// Clear all breakpoints
CORE_EXPORT bool CoreDebugClearBreakpoints(void);

#endif // CORE_DEBUGGER_HPP
