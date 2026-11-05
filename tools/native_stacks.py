"""Prints the symbolized native stacks of every thread of a running process.

    python tools/native_stacks.py <pid> [extra symbol folders...]

For a hung usdcc or test program when no debugger is installed: suspends each
thread in turn, walks its stack with dbghelp's StackWalk64 and names the frames
from the PDBs (found through the paths the binaries record, or the folders
given). Uses the Windows SDK's dbghelp.dll ("Debugging Tools" not needed, the
DLL comes with the SDK). Windows x64 only. See docs/PLUGINS.md section 5.
"""

import ctypes
import ctypes.wintypes as wt
import os
import sys

# The SDK's copy is newer than System32's; either reads PDBs.
DBGHELP = r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\dbghelp.dll"
dbghelp = ctypes.WinDLL(DBGHELP if os.path.exists(DBGHELP) else "dbghelp")
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

PROCESS_ALL_ACCESS = 0x1F0FFF
THREAD_ALL_ACCESS = 0x1F03FF
TH32CS_SNAPTHREAD = 0x4
CONTEXT_FULL = 0x10000B
IMAGE_FILE_MACHINE_AMD64 = 0x8664
SYMOPT_UNDNAME, SYMOPT_DEFERRED_LOADS, SYMOPT_LOAD_LINES = 0x2, 0x4, 0x10


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", wt.LONG), ("tpDeltaPri", wt.LONG),
                ("dwFlags", wt.DWORD)]


class ADDRESS64(ctypes.Structure):
    _fields_ = [("Offset", ctypes.c_uint64), ("Segment", wt.WORD), ("Mode", wt.DWORD)]


class STACKFRAME64(ctypes.Structure):
    _fields_ = [("AddrPC", ADDRESS64), ("AddrReturn", ADDRESS64), ("AddrFrame", ADDRESS64),
                ("AddrStack", ADDRESS64), ("AddrBStore", ADDRESS64), ("FuncTableEntry", ctypes.c_void_p),
                ("Params", ctypes.c_uint64 * 4), ("Far", wt.BOOL), ("Virtual", wt.BOOL),
                ("Reserved", ctypes.c_uint64 * 3), ("KdHelp", ctypes.c_byte * 512)]


dbghelp.SymFunctionTableAccess64.restype = ctypes.c_void_p
dbghelp.SymFunctionTableAccess64.argtypes = [wt.HANDLE, ctypes.c_uint64]
dbghelp.SymGetModuleBase64.restype = ctypes.c_uint64
dbghelp.SymGetModuleBase64.argtypes = [wt.HANDLE, ctypes.c_uint64]
FTA = ctypes.WINFUNCTYPE(ctypes.c_void_p, wt.HANDLE, ctypes.c_uint64)(("SymFunctionTableAccess64", dbghelp))
GMB = ctypes.WINFUNCTYPE(ctypes.c_uint64, wt.HANDLE, ctypes.c_uint64)(("SymGetModuleBase64", dbghelp))
dbghelp.StackWalk64.argtypes = [wt.DWORD, wt.HANDLE, wt.HANDLE, ctypes.POINTER(STACKFRAME64), ctypes.c_void_p,
                                ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
dbghelp.SymFromAddrW.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.c_void_p]
dbghelp.SymGetLineFromAddrW64.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(wt.DWORD), ctypes.c_void_p]
dbghelp.SymInitializeW.argtypes = [wt.HANDLE, wt.LPCWSTR, wt.BOOL]
kernel32.OpenProcess.restype = wt.HANDLE
kernel32.OpenThread.restype = wt.HANDLE
kernel32.CreateToolhelp32Snapshot.restype = wt.HANDLE
kernel32.GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
kernel32.SuspendThread.argtypes = [wt.HANDLE]
kernel32.ResumeThread.argtypes = [wt.HANDLE]


def symbol(process, address):
    buffer = ctypes.create_string_buffer(88 + 2 * 1024)
    ctypes.c_uint32.from_buffer(buffer, 0).value = 88       # SizeOfStruct
    ctypes.c_uint32.from_buffer(buffer, 80).value = 1024    # MaxNameLen
    displacement = ctypes.c_uint64()
    if not dbghelp.SymFromAddrW(process, address, ctypes.byref(displacement), buffer):
        return f"0x{address:x}"
    name_len = ctypes.c_uint32.from_buffer(buffer, 76).value
    name = ctypes.wstring_at(ctypes.addressof(buffer) + 84, name_len)
    line = ctypes.create_string_buffer(40)
    ctypes.c_uint32.from_buffer(line, 0).value = 40
    line_disp = wt.DWORD()
    where = ""
    if dbghelp.SymGetLineFromAddrW64(process, address, ctypes.byref(line_disp), line):
        file = ctypes.wstring_at(ctypes.c_void_p.from_buffer(line, 24).value)
        where = f"  [{file.split(chr(92))[-1]}:{ctypes.c_uint32.from_buffer(line, 16).value}]"
    return f"{name}+0x{displacement.value:x}{where}"


def main():
    pid = int(sys.argv[1])
    process = kernel32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
    dbghelp.SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES)
    dbghelp.SymInitializeW(process, ";".join(sys.argv[2:]) or None, True)

    snapshot = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    entry = THREADENTRY32()
    entry.dwSize = ctypes.sizeof(THREADENTRY32)
    threads = []
    ok = kernel32.Thread32First(snapshot, ctypes.byref(entry))
    while ok:
        if entry.th32OwnerProcessID == pid:
            threads.append(entry.th32ThreadID)
        ok = kernel32.Thread32Next(snapshot, ctypes.byref(entry))

    for tid in threads:
        thread = kernel32.OpenThread(THREAD_ALL_ACCESS, False, tid)
        kernel32.SuspendThread(thread)
        raw = ctypes.create_string_buffer(1232 + 16)
        base = (ctypes.addressof(raw) + 15) & ~15
        ctypes.c_uint32.from_address(base + 0x30).value = CONTEXT_FULL
        if not kernel32.GetThreadContext(thread, base):
            kernel32.ResumeThread(thread)
            continue
        frame = STACKFRAME64()
        frame.AddrPC.Offset = ctypes.c_uint64.from_address(base + 0xF8).value
        frame.AddrStack.Offset = ctypes.c_uint64.from_address(base + 0x98).value
        frame.AddrFrame.Offset = ctypes.c_uint64.from_address(base + 0xA0).value
        frame.AddrPC.Mode = frame.AddrStack.Mode = frame.AddrFrame.Mode = 3  # flat
        print(f"=== thread {tid}")
        for _ in range(60):
            if not dbghelp.StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, ctypes.byref(frame),
                                       ctypes.c_void_p(base), None, FTA, GMB, None):
                break
            if frame.AddrPC.Offset == 0:
                break
            print("   ", symbol(process, frame.AddrPC.Offset))
        kernel32.ResumeThread(thread)


if __name__ == "__main__":
    main()
