"""CPU time and resident memory for the isolated processes owned by a lab."""

import os
from pathlib import Path


def sample(pid):
    if os.name == "nt":
        return _sample_windows(pid)
    stat = Path(f"/proc/{pid}/stat").read_text(encoding="ascii").rsplit(")", 1)[1].split()
    return {"cpuSeconds": (int(stat[11]) + int(stat[12])) / os.sysconf("SC_CLK_TCK"),
            "rssBytes": int(stat[21]) * os.sysconf("SC_PAGE_SIZE")}


def _sample_windows(pid):
    import ctypes
    from ctypes import wintypes

    class Memory(ctypes.Structure):
        _fields_ = [("size", wintypes.DWORD), ("faults", wintypes.DWORD), ("peakWorkingSet", ctypes.c_size_t),
                    ("workingSet", ctypes.c_size_t), ("quotas", ctypes.c_size_t * 4),
                    ("pagefile", ctypes.c_size_t), ("peakPagefile", ctypes.c_size_t)]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel.K32GetProcessMemoryInfo.argtypes = (wintypes.HANDLE, ctypes.POINTER(Memory), wintypes.DWORD)
    kernel.GetProcessTimes.argtypes = (wintypes.HANDLE, *([ctypes.POINTER(wintypes.FILETIME)] * 4))
    handle = kernel.OpenProcess(0x1000 | 0x10, False, pid)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        working = Memory()
        working.size = ctypes.sizeof(working)
        times = [wintypes.FILETIME() for _ in range(4)]
        if (not kernel.K32GetProcessMemoryInfo(handle, ctypes.byref(working), working.size) or
                not kernel.GetProcessTimes(handle, *(ctypes.byref(item) for item in times))):
            raise ctypes.WinError(ctypes.get_last_error())
        cpu = sum((item.dwHighDateTime << 32) | item.dwLowDateTime for item in times[2:]) / 10_000_000
        return {"cpuSeconds": cpu, "rssBytes": working.workingSet}
    finally:
        kernel.CloseHandle(handle)
