/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#define CORE_INTERNAL
#include "Debugger.hpp"
#include "Emulation.hpp"
#include "Error.hpp"
#include "Library.hpp"

#include "m64p/Api.hpp"
#include "m64p/api/m64p_types.h"

#include <algorithm>

#include <cstring>
#include <vector>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// Debugger function pointers (resolved at runtime)
typedef void (*ptr_DebugDecodeOp)(unsigned int, char*, char*, int);
typedef unsigned char (*ptr_DebugMemRead8)(unsigned int);
typedef unsigned short (*ptr_DebugMemRead16)(unsigned int);
typedef unsigned int (*ptr_DebugMemRead32)(unsigned int);
typedef unsigned long long (*ptr_DebugMemRead64)(unsigned int);
typedef void (*ptr_DebugMemWrite8)(unsigned int, unsigned char);
typedef void (*ptr_DebugMemWrite16)(unsigned int, unsigned short);
typedef void (*ptr_DebugMemWrite32)(unsigned int, unsigned int);
typedef void (*ptr_DebugMemWrite64)(unsigned int, unsigned long long);
typedef void* (*ptr_DebugGetCPUDataPtr)(int);
typedef int (*ptr_DebugBreakpointLookup)(unsigned int, unsigned int, unsigned int);
typedef int (*ptr_DebugBreakpointCommand)(int, unsigned int, void*);
typedef int (*ptr_DebugGetState)(int);
typedef int (*ptr_DebugStep)(void);
typedef int (*ptr_DebugSetCallbacks)(void (*)(void), void (*)(unsigned int), void (*)(void));
typedef int (*ptr_DebugSetRunState)(int);
typedef void (*ptr_DebugInitDebugger)(void);

// CPU emulator mode (from mupen64plus core)
enum class M64PDebugCPUMode : int
{
    PureInterpreter = 0,
    CachedInterpreter = 1,
    DynamicRecompiler = 2
};

// Breakpoint command enum (from mupen64plus)
enum class M64PDebugBkpCommand : int
{
    Add = 1,
    AddStruct,
    Replace,
    RemoveAddr,
    RemoveIdx,
    Enable,
    Disable,
    Check
};

// Breakpoint structure (from mupen64plus)
struct M64PBreakpoint
{
    uint32_t address;
    uint32_t endaddr;
    unsigned int flags;
};

// CPU data pointers (from mupen64plus)
enum class M64PCPUData : int
{
    PC = 1,
    GPRReg,
    HI,
    LO,
    COP0,
    COP1DoublePtr,
    COP1SimplePtr,
    COP1FGR64,
    TLB
};

// Cached function pointers
static ptr_DebugDecodeOp g_DebugDecodeOp = nullptr;
static ptr_DebugMemRead8 g_DebugMemRead8 = nullptr;
static ptr_DebugMemRead16 g_DebugMemRead16 = nullptr;
static ptr_DebugMemRead32 g_DebugMemRead32 = nullptr;
static ptr_DebugMemRead64 g_DebugMemRead64 = nullptr;
static ptr_DebugMemWrite8 g_DebugMemWrite8 = nullptr;
static ptr_DebugMemWrite16 g_DebugMemWrite16 = nullptr;
static ptr_DebugMemWrite32 g_DebugMemWrite32 = nullptr;
static ptr_DebugMemWrite64 g_DebugMemWrite64 = nullptr;
static ptr_DebugGetCPUDataPtr g_DebugGetCPUDataPtr = nullptr;
static ptr_DebugBreakpointLookup g_DebugBreakpointLookup = nullptr;
static ptr_DebugBreakpointCommand g_DebugBreakpointCommand = nullptr;
static ptr_DebugGetState g_DebugGetState = nullptr;
static ptr_DebugStep g_DebugStep = nullptr;
static ptr_DebugSetCallbacks g_DebugSetCallbacks = nullptr;
static ptr_DebugSetRunState g_DebugSetRunState = nullptr;
static ptr_DebugInitDebugger g_DebugInitDebugger = nullptr;

// Flag to track if symbols were resolved
static bool g_DebugSymbolsResolved = false;
static bool g_DebugSymbolsAvailable = false;

// Resolve debug symbols from mupen64plus core
static bool ResolveDebugSymbols(void)
{
    if (g_DebugSymbolsResolved)
        return g_DebugSymbolsAvailable;

    g_DebugSymbolsResolved = true;

    // Get core library handle
    CoreLibraryHandle coreHandle = m64p::Core.GetHandle();
    if (!coreHandle)
        return false;

    // Attempt to resolve debug symbols
    auto resolveSymbol = [coreHandle](const char* name) -> void* {
        return CoreGetLibrarySymbol(coreHandle, name);
    };

    g_DebugDecodeOp = (ptr_DebugDecodeOp)resolveSymbol("DebugDecodeOp");
    g_DebugMemRead8 = (ptr_DebugMemRead8)resolveSymbol("DebugMemRead8");
    g_DebugMemRead16 = (ptr_DebugMemRead16)resolveSymbol("DebugMemRead16");
    g_DebugMemRead32 = (ptr_DebugMemRead32)resolveSymbol("DebugMemRead32");
    g_DebugMemRead64 = (ptr_DebugMemRead64)resolveSymbol("DebugMemRead64");
    g_DebugMemWrite8 = (ptr_DebugMemWrite8)resolveSymbol("DebugMemWrite8");
    g_DebugMemWrite16 = (ptr_DebugMemWrite16)resolveSymbol("DebugMemWrite16");
    g_DebugMemWrite32 = (ptr_DebugMemWrite32)resolveSymbol("DebugMemWrite32");
    g_DebugMemWrite64 = (ptr_DebugMemWrite64)resolveSymbol("DebugMemWrite64");
    g_DebugGetCPUDataPtr = (ptr_DebugGetCPUDataPtr)resolveSymbol("DebugGetCPUDataPtr");
    g_DebugBreakpointLookup = (ptr_DebugBreakpointLookup)resolveSymbol("DebugBreakpointLookup");
    g_DebugBreakpointCommand = (ptr_DebugBreakpointCommand)resolveSymbol("DebugBreakpointCommand");
    g_DebugGetState = (ptr_DebugGetState)resolveSymbol("DebugGetState");
    g_DebugStep = (ptr_DebugStep)resolveSymbol("DebugStep");
    g_DebugSetCallbacks = (ptr_DebugSetCallbacks)resolveSymbol("DebugSetCallbacks");
    g_DebugSetRunState = (ptr_DebugSetRunState)resolveSymbol("DebugSetRunState");
    g_DebugInitDebugger = (ptr_DebugInitDebugger)resolveSymbol("DebugInitDebugger");

    // Check if all critical symbols were resolved
    g_DebugSymbolsAvailable = g_DebugDecodeOp && g_DebugMemRead32 && g_DebugGetCPUDataPtr && g_DebugGetState;

    return g_DebugSymbolsAvailable;
}

bool CoreDebugIsAvailable(void)
{
    return ResolveDebugSymbols();
}

CoreDebugger::CPUMode CoreDebugGetCPUMode(void)
{
    if (!g_DebugGetState)
    {
        ResolveDebugSymbols();
        if (!g_DebugGetState)
            return CoreDebugger::CPUMode::DynamicRecompiler;
    }

    // M64P_DBG_CPU_DYNACORE = 4; returns 0=pure interp, 1=cached interp, 2=dynarec
    int mode = g_DebugGetState(4);
    return static_cast<CoreDebugger::CPUMode>(mode);
}

void CoreDebugDecodeOp(uint32_t instr, char* outOp, char* outArgs, uint32_t pc)
{
    if (!g_DebugDecodeOp)
    {
        ResolveDebugSymbols();
        if (!g_DebugDecodeOp)
        {
            outOp[0] = '\0';
            outArgs[0] = '\0';
            return;
        }
    }

    g_DebugDecodeOp(instr, outOp, outArgs, (int)pc);
}

uint8_t CoreDebugMemRead8(uint32_t address)
{
    if (!g_DebugMemRead8)
    {
        ResolveDebugSymbols();
        if (!g_DebugMemRead8)
            return 0;
    }

    return g_DebugMemRead8(address);
}

uint16_t CoreDebugMemRead16(uint32_t address)
{
    if (!g_DebugMemRead16)
    {
        ResolveDebugSymbols();
        if (!g_DebugMemRead16)
            return 0;
    }

    return g_DebugMemRead16(address);
}

uint32_t CoreDebugMemRead32(uint32_t address)
{
    if (!g_DebugMemRead32)
    {
        ResolveDebugSymbols();
        if (!g_DebugMemRead32)
            return 0xFFFFFFFF;
    }

    return g_DebugMemRead32(address);
}

uint64_t CoreDebugMemRead64(uint32_t address)
{
    if (!g_DebugMemRead64)
    {
        ResolveDebugSymbols();
        if (!g_DebugMemRead64)
            return 0xFFFFFFFFFFFFFFFFULL;
    }

    return g_DebugMemRead64(address);
}

void CoreDebugMemWrite8(uint32_t address, uint8_t value)
{
    if (!g_DebugMemWrite8)
    {
        ResolveDebugSymbols();
        if (!g_DebugMemWrite8)
            return;
    }

    g_DebugMemWrite8(address, value);
}

void CoreDebugMemWrite16(uint32_t address, uint16_t value)
{
    if (!g_DebugMemWrite16)
    {
        ResolveDebugSymbols();
        if (!g_DebugMemWrite16)
            return;
    }

    g_DebugMemWrite16(address, value);
}

void CoreDebugMemWrite32(uint32_t address, uint32_t value)
{
    if (!g_DebugMemWrite32)
    {
        ResolveDebugSymbols();
        if (!g_DebugMemWrite32)
            return;
    }

    g_DebugMemWrite32(address, value);
}

void CoreDebugMemWrite64(uint32_t address, uint64_t value)
{
    if (!g_DebugMemWrite64)
    {
        ResolveDebugSymbols();
        if (!g_DebugMemWrite64)
            return;
    }

    g_DebugMemWrite64(address, value);
}

uint32_t CoreDebugGetPC(void)
{
    if (!g_DebugGetCPUDataPtr)
    {
        ResolveDebugSymbols();
        if (!g_DebugGetCPUDataPtr)
            return 0;
    }

    uint32_t* pcPtr = (uint32_t*)g_DebugGetCPUDataPtr((int)M64PCPUData::PC);
    if (!pcPtr)
        return 0;

    return *pcPtr;
}

// ── CoreDebugPreExecuteSetup / CoreDebugSetUIUpdateCallback ───────────────

static void (*s_uiUpdateHandler)(unsigned int) = nullptr;

// Called by mupen64plus init_debugger() at emulation start.
// Immediately resumes so the game runs at full speed until the user pauses.
static void preExecAutoInitCb(void)
{
    fprintf(stderr, "[DEBUGGER] preExecAutoInitCb called\n");
    CoreDebugSetRunState(CoreDebugRunState::Running);
}

static void preExecAutoViCb(void)
{
    fprintf(stderr, "[DEBUGGER] VI callback fired\n");
}

// Called by mupen64plus on every instruction while paused, or when a
// breakpoint is hit. Routes to whatever the UI registered.
static void preExecAutoUpdateCb(unsigned int pc)
{
    fprintf(stderr, "[DEBUGGER] preExecAutoUpdateCb called at PC=0x%08x, handler=%p\n", pc, s_uiUpdateHandler);
    if (s_uiUpdateHandler)
    {
        s_uiUpdateHandler(pc);
    }
    else
    {
        // Auto-resume when no handler is connected (e.g., debugger dialog not open)
        fprintf(stderr, "[DEBUGGER] No handler, auto-resuming\n");
        CoreDebugSetRunState(CoreDebugRunState::Running);
    }
}

void CoreDebugSetUIUpdateCallback(void (*handler)(unsigned int))
{
    s_uiUpdateHandler = handler;
}

void CoreDebugPreExecuteSetup(void)
{
    fprintf(stderr, "[DEBUGGER] PreExecuteSetup called\n");
    if (!ResolveDebugSymbols()) {
        fprintf(stderr, "[DEBUGGER] ResolveDebugSymbols failed\n");
        return;
    }
    if (!g_DebugSetCallbacks) {
        fprintf(stderr, "[DEBUGGER] g_DebugSetCallbacks is NULL\n");
        return;
    }

    // Enable debugger in mupen64plus Core config - required for init_debugger() to run
    m64p_handle coreConfig = nullptr;
    if (m64p::Config.OpenSection("Core", &coreConfig) == M64ERR_SUCCESS && coreConfig)
    {
        int enabled = 1;
        m64p::Config.SetParameter(coreConfig, "EnableDebugger", M64TYPE_BOOL, &enabled);
        fprintf(stderr, "[DEBUGGER] EnableDebugger set to true\n");
    }
    else
    {
        fprintf(stderr, "[DEBUGGER] Failed to open Core config section\n");
    }

    fprintf(stderr, "[DEBUGGER] Registering callbacks with g_DebugSetCallbacks\n");
    g_DebugSetCallbacks(preExecAutoInitCb, preExecAutoUpdateCb, preExecAutoViCb);
    fprintf(stderr, "[DEBUGGER] Callbacks registered\n");
}

void CoreDebuggerInit(void)
{
    if (!ResolveDebugSymbols() || !g_DebugInitDebugger)
        return;

    g_DebugInitDebugger();
}

void CoreDebugSetCallbacks(void (*initCallback)(void), void (*updateCallback)(unsigned int), void (*viCallback)(void))
{
    if (!g_DebugSetCallbacks)
    {
        ResolveDebugSymbols();
        if (!g_DebugSetCallbacks)
            return;
    }

    g_DebugSetCallbacks(initCallback, updateCallback, viCallback);
}

void CoreDebugSetRunState(CoreDebugRunState state)
{
    if (!g_DebugSetRunState)
    {
        ResolveDebugSymbols();
        if (!g_DebugSetRunState)
            return;
    }

    g_DebugSetRunState((int)state);
}

void CoreDebugStep(void)
{
    if (!g_DebugStep)
    {
        ResolveDebugSymbols();
        if (!g_DebugStep)
            return;
    }

    g_DebugStep();
}

int64_t CoreDebugGetGPRRegister(CoreDebugger::GPRRegister reg)
{
    if (!g_DebugGetCPUDataPtr)
    {
        ResolveDebugSymbols();
        if (!g_DebugGetCPUDataPtr)
            return 0;
    }

    int64_t* regPtr = (int64_t*)g_DebugGetCPUDataPtr((int)M64PCPUData::GPRReg);
    if (!regPtr)
        return 0;

    return regPtr[(int)reg];
}

void CoreDebugSetGPRRegister(CoreDebugger::GPRRegister reg, int64_t value)
{
    if (!g_DebugGetCPUDataPtr)
    {
        ResolveDebugSymbols();
        if (!g_DebugGetCPUDataPtr)
            return;
    }

    int64_t* regPtr = (int64_t*)g_DebugGetCPUDataPtr((int)M64PCPUData::GPRReg);
    if (!regPtr)
        return;

    regPtr[(int)reg] = value;
}

double CoreDebugGetFPRegister(CoreDebugger::FPRegister reg)
{
    if (!g_DebugGetCPUDataPtr)
    {
        ResolveDebugSymbols();
        if (!g_DebugGetCPUDataPtr)
            return 0.0;
    }

    double* regPtr = (double*)g_DebugGetCPUDataPtr((int)M64PCPUData::COP1FGR64);
    if (!regPtr)
        return 0.0;

    return regPtr[(int)reg];
}

void CoreDebugSetFPRegister(CoreDebugger::FPRegister reg, double value)
{
    if (!g_DebugGetCPUDataPtr)
    {
        ResolveDebugSymbols();
        if (!g_DebugGetCPUDataPtr)
            return;
    }

    double* regPtr = (double*)g_DebugGetCPUDataPtr((int)M64PCPUData::COP1FGR64);
    if (!regPtr)
        return;

    regPtr[(int)reg] = value;
}

// ── Breakpoint tracking ───────────────────────────────────────────────────
// We maintain a local list of breakpoints for UI display
static std::vector<CoreDebugger::Breakpoint> g_TrackedBreakpoints;

std::vector<CoreDebugger::Breakpoint> CoreDebugGetBreakpoints(void)
{
    return g_TrackedBreakpoints;
}

bool CoreDebugAddFetchBreakpoint(uint32_t address)
{
    fprintf(stderr, "[DEBUGGER] AddFetchBreakpoint at 0x%08x\n", address);
    if (!g_DebugBreakpointCommand)
    {
        ResolveDebugSymbols();
        if (!g_DebugBreakpointCommand)
            return false;
    }

    // M64P_BKP_CMD_ADD_ADDR: 'index' parameter IS the address to break on
    int result = g_DebugBreakpointCommand((int)M64PDebugBkpCommand::Add, address, nullptr);
    if (result < 0)
        return false;

    CoreDebugger::Breakpoint bpt;
    bpt.address    = address;
    bpt.endAddress = address;
    bpt.flags      = 0x09; // ENABLED | EXEC
    g_TrackedBreakpoints.push_back(bpt);
    fprintf(stderr, "[DEBUGGER] Breakpoint added, total: %zu\n", g_TrackedBreakpoints.size());
    return true;
}

bool CoreDebugAddReadBreakpoint(uint32_t address, uint32_t endAddress)
{
    if (!g_DebugBreakpointCommand)
    {
        ResolveDebugSymbols();
        if (!g_DebugBreakpointCommand)
            return false;
    }

    M64PBreakpoint bpt = {};
    bpt.address  = address;
    bpt.endaddr  = endAddress;
    bpt.flags    = 0x03; // ENABLED | READ

    int result = g_DebugBreakpointCommand((int)M64PDebugBkpCommand::AddStruct, 0, &bpt);
    if (result < 0)
        return false;

    CoreDebugger::Breakpoint coreBpt;
    coreBpt.address    = address;
    coreBpt.endAddress = endAddress;
    coreBpt.flags      = bpt.flags;
    g_TrackedBreakpoints.push_back(coreBpt);
    return true;
}

bool CoreDebugAddWriteBreakpoint(uint32_t address, uint32_t endAddress)
{
    if (!g_DebugBreakpointCommand)
    {
        ResolveDebugSymbols();
        if (!g_DebugBreakpointCommand)
            return false;
    }

    M64PBreakpoint bpt = {};
    bpt.address  = address;
    bpt.endaddr  = endAddress;
    bpt.flags    = 0x05; // ENABLED | WRITE

    int result = g_DebugBreakpointCommand((int)M64PDebugBkpCommand::AddStruct, 0, &bpt);
    if (result < 0)
        return false;

    CoreDebugger::Breakpoint coreBpt;
    coreBpt.address    = address;
    coreBpt.endAddress = endAddress;
    coreBpt.flags      = bpt.flags;
    g_TrackedBreakpoints.push_back(coreBpt);
    return true;
}

bool CoreDebugRemoveBreakpoint(uint32_t address)
{
    if (!g_DebugBreakpointCommand)
    {
        ResolveDebugSymbols();
        if (!g_DebugBreakpointCommand)
            return false;
    }

    // M64P_BKP_CMD_REMOVE_ADDR: 'index' parameter IS the address to remove
    g_DebugBreakpointCommand((int)M64PDebugBkpCommand::RemoveAddr, address, nullptr);

    // Remove from our mirror list
    auto it = std::remove_if(g_TrackedBreakpoints.begin(), g_TrackedBreakpoints.end(),
        [address](const CoreDebugger::Breakpoint& b) { return b.address == address; });
    g_TrackedBreakpoints.erase(it, g_TrackedBreakpoints.end());
    return true;
}

bool CoreDebugClearBreakpoints(void)
{
    std::vector<uint32_t> addrs;
    for (const auto& b : g_TrackedBreakpoints)
        addrs.push_back(b.address);
    for (uint32_t addr : addrs)
        CoreDebugRemoveBreakpoint(addr);
    return true;
}
