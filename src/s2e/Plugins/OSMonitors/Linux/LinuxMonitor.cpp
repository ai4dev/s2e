///
/// Copyright (C) 2014-2017, Dependable Systems Laboratory, EPFL
/// Copyright (C) 2014-2017, Cyberhaven
/// All rights reserved.
///
/// Licensed under the Cyberhaven Research License Agreement.
///

#include <s2e/ConfigFile.h>
#include <s2e/S2E.h>

#include <s2e/Plugins/OSMonitors/Support/MemoryMap.h>

#include "LinuxMonitor.h"

using namespace klee;

namespace s2e {
namespace plugins {

S2E_DEFINE_PLUGIN(LinuxMonitor, "LinuxMonitor S2E plugin", "OSMonitor", "BaseInstructions", "Vmi");

void LinuxMonitor::initialize() {
    ConfigFile *cfg = s2e()->getConfig();

    m_vmi = s2e()->getPlugin<Vmi>();

    // XXX: this is a circular dependency, will require further refactoring
    m_map = s2e()->getPlugin<MemoryMap>();
    if (!m_map) {
        getWarningsStream() << "Requires MemoryMap\n";
        exit(-1);
    }

    m_terminateOnSegfault = cfg->getBool(getConfigKey() + ".terminateOnSegfault", true);
    m_terminateOnTrap = cfg->getBool(getConfigKey() + ".terminateOnTrap", true);

    m_commandSize = sizeof(S2E_LINUXMON_COMMAND);
    m_commandVersion = S2E_LINUXMON_COMMAND_VERSION;
}

///
/// \brief Get the process id for the current state
///
/// In the Linux kernel, each thread has its own task_struct that contains:
///  * Its own identifier, the process identifier (PID)
///  * The identifier of the process that started the thread, the thread group
///    (TGID)
///
/// Therefore the getPid method returns the TGID and getTid returns the PID.
///
/// \return The process id
///
uint64_t LinuxMonitor::getPid(S2EExecutionState *state) {
    target_ulong currentTask;

    if (!state->mem()->read(m_currentTaskAddr, &currentTask, sizeof(currentTask))) {
        return -1;
    }

    // In the kernel the `pid_t` type is just a typedef for `int` (see include/uapi/asm-generic/posix_types.h)
    int pid;
    target_ulong pidAddress = currentTask + m_taskStructTgidOffset;

    if (!state->mem()->read(pidAddress, &pid, sizeof(pid))) {
        return -1;
    } else {
        return pid;
    }
}

uint64_t LinuxMonitor::getTid(S2EExecutionState *state) {
    target_ulong currentTask;

    if (!state->mem()->read(m_currentTaskAddr, &currentTask, sizeof(currentTask))) {
        return -1;
    }

    target_ulong tid;
    target_ulong tidAddress = m_currentTaskAddr + m_taskStructPidOffset;

    if (!state->mem()->read(tidAddress, &tid, sizeof(tid))) {
        return -1;
    } else {
        return tid;
    }
}

void LinuxMonitor::handleSegfault(S2EExecutionState *state, const S2E_LINUXMON_COMMAND &cmd) {
    getWarningsStream(state) << "Received segfault"
                             << " type=" << cmd.SegFault.fault << " pagedir=" << hexval(state->regs()->getPageDir())
                             << " pid=" << hexval(cmd.currentPid) << " pc=" << hexval(cmd.SegFault.pc)
                             << " addr=" << hexval(cmd.SegFault.address) << "\n";

    // Don't switch states until it finishes and gets killed by
    // bootstrap.
    //
    // Need to print a message here to avoid confusion and needless
    // debugging, wondering why the searcher doesn't work anymore.
    getDebugStream(state) << "Blocking searcher until state is terminated\n";
    state->setStateSwitchForbidden(true);

    state->disassemble(getDebugStream(state), cmd.SegFault.pc, 256);

    onSegFault.emit(state, cmd.currentPid, cmd.SegFault.pc);

    if (m_terminateOnSegfault) {
        getDebugStream(state) << "Terminating state: received segfault\n";
        s2e()->getExecutor()->terminateStateEarly(*state, "Segfault");
    }
}

void LinuxMonitor::handleProcessLoad(S2EExecutionState *state, const S2E_LINUXMON_COMMAND &cmd) {
    completeInitialization(state);

    std::string processPath;
    if (!state->mem()->readString(cmd.ProcessLoad.process_path, processPath)) {
        getWarningsStream(state) << "could not read process path of pid " << hexval(cmd.currentPid) << "\n";
    }

    getDebugStream(state) << "Process " << processPath << " loaded"
                          << " pid=" << hexval(cmd.currentPid) << "\n";

    llvm::StringRef file(processPath);

    onProcessLoad.emit(state, state->regs()->getPageDir(), cmd.currentPid, llvm::sys::path::filename(file));
}

void LinuxMonitor::handleModuleLoad(S2EExecutionState *state, const S2E_LINUXMON_COMMAND &cmd) {
    ModuleDescriptor module;

    if (!state->mem()->readString(cmd.ModuleLoad.module_path, module.Path)) {
        getWarningsStream(state) << "could not read module path\n";
        return;
    }

    module.Name = llvm::sys::path::filename(module.Path);
    module.Size = cmd.ModuleLoad.size;

    Vmi::BinData data = m_vmi->getFromDisk(module, true);
    if (!data.ef) {
        getWarningsStream(state) << "Could not load " << module.Path << " from disk. Check your guestfs settings.\n";
    } else {
        Vmi::toModuleDescriptor(module, data.ef);
    }

    module.AddressSpace = state->regs()->getPageDir();
    module.Pid = cmd.currentPid;
    module.LoadBase = cmd.ModuleLoad.load_base;

    getDebugStream(state) << module << '\n';

    onModuleLoad.emit(state, module);
}

void LinuxMonitor::handleProcessExit(S2EExecutionState *state, const S2E_LINUXMON_COMMAND &cmd) {
    auto pd = state->regs()->getPageDir();
    getDebugStream(state) << "Removing task (pid=" << hexval(cmd.currentPid) << ", cr3=" << hexval(pd)
                          << ", exitCode=" << cmd.ProcessExit.code << ").\n";

    onProcessUnload.emit(state, pd, cmd.currentPid, cmd.ProcessExit.code);
}

void LinuxMonitor::handleTrap(S2EExecutionState *state, const S2E_LINUXMON_COMMAND &cmd) {
    getWarningsStream(state) << "Received trap"
                             << " pid=" << hexval(cmd.currentPid) << " pc=" << hexval(cmd.Trap.pc)
                             << " trapnr=" << hexval(cmd.Trap.trapnr) << " signr=" << hexval(cmd.Trap.signr)
                             << " err_code=" << cmd.Trap.error_code << "\n";

    getDebugStream(state) << "Blocking searcher until state is terminated\n";
    state->setStateSwitchForbidden(true);

    onTrap.emit(state, cmd.currentPid, cmd.Trap.pc, cmd.Trap.trapnr);

    if (m_terminateOnTrap) {
        getDebugStream(state) << "Terminating state: received trap\n";
        s2e()->getExecutor()->terminateStateEarly(*state, "Trap");
    }
}

void LinuxMonitor::handleInit(S2EExecutionState *state, const S2E_LINUXMON_COMMAND &cmd) {
    getDebugStream(state) << "Received kernel init"
                          << " page_offset=" << hexval(cmd.Init.page_offset)
                          << " &current_task=" << hexval(cmd.Init.current_task_address)
                          << " task_struct.pid offset=" << cmd.Init.task_struct_pid_offset
                          << " task_struct.tgid offset=" << cmd.Init.task_struct_tgid_offset << "\n";

    m_kernelStartAddress = cmd.Init.page_offset;
    m_currentTaskAddr = cmd.Init.current_task_address;
    m_taskStructPidOffset = cmd.Init.task_struct_pid_offset;
    m_taskStructTgidOffset = cmd.Init.task_struct_tgid_offset;

    completeInitialization(state);

    loadKernelImage(state, cmd.Init.start_kernel);
}

void LinuxMonitor::handleMemMap(S2EExecutionState *state, const S2E_LINUXMON_COMMAND &cmd) {
    getDebugStream(state) << "mmap pid=" << hexval(cmd.currentPid) << " addr=" << hexval(cmd.MemMap.address)
                          << " size=" << hexval(cmd.MemMap.size) << " prot=" << hexval(cmd.MemMap.prot)
                          << " flag=" << hexval(cmd.MemMap.flag) << " pgoff=" << hexval(cmd.MemMap.pgoff) << "\n";

    onMemoryMap.emit(state, cmd.currentPid, cmd.MemMap.address, cmd.MemMap.size, cmd.MemMap.prot);
}

void LinuxMonitor::handleMemUnmap(S2EExecutionState *state, const S2E_LINUXMON_COMMAND &cmd) {
    getDebugStream(state) << "munmap pid=" << hexval(cmd.currentPid) << " start=" << hexval(cmd.MemUnmap.start)
                          << " end=" << hexval(cmd.MemUnmap.end) << "\n";

    uint64_t size = cmd.MemUnmap.end - cmd.MemUnmap.start;
    onMemoryUnmap.emit(state, cmd.currentPid, cmd.MemUnmap.start, size);
}

void LinuxMonitor::handleMemProtect(S2EExecutionState *state, const S2E_LINUXMON_COMMAND &cmd) {
    getDebugStream(state) << "mprotect pid=" << hexval(cmd.currentPid) << " start=" << hexval(cmd.MemProtect.start)
                          << " size=" << hexval(cmd.MemProtect.size) << " prot=" << hexval(cmd.MemProtect.prot) << "\n";

    onMemoryProtect.emit(state, cmd.currentPid, cmd.MemProtect.start, cmd.MemProtect.size, cmd.MemProtect.prot);
}

void LinuxMonitor::handleCommand(S2EExecutionState *state, uint64_t guestDataPtr, uint64_t guestDataSize, void *_cmd) {
    S2E_LINUXMON_COMMAND &cmd = *(S2E_LINUXMON_COMMAND *) _cmd;
    switch (cmd.Command) {
        case LINUX_SEGFAULT:
            handleSegfault(state, cmd);
            break;

        case LINUX_PROCESS_LOAD:
            handleProcessLoad(state, cmd);
            break;

        case LINUX_MODULE_LOAD:
            handleModuleLoad(state, cmd);
            break;

        case LINUX_TRAP:
            handleTrap(state, cmd);
            break;

        case LINUX_PROCESS_EXIT:
            handleProcessExit(state, cmd);
            break;

        case LINUX_INIT:
            handleInit(state, cmd);
            break;

        case LINUX_KERNEL_PANIC:
            handleKernelPanic(state, cmd.Panic.message, cmd.Panic.message_size);
            break;

        case LINUX_MEMORY_MAP:
            handleMemMap(state, cmd);
            break;

        case LINUX_MEMORY_UNMAP:
            handleMemUnmap(state, cmd);
            break;

        case LINUX_MEMORY_PROTECT:
            handleMemProtect(state, cmd);
            break;
    }
}

} // namespace plugins
} // namespace s2e
