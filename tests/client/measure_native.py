"""Windows child-process working set, excluding browser and server; no extra packages."""
import ctypes
from ctypes import wintypes
import threading

class Counters(ctypes.Structure):
    _fields_=[('cb',wintypes.DWORD),('PageFaultCount',wintypes.DWORD)]+[(name,ctypes.c_size_t) for name in ('PeakWorkingSetSize','WorkingSetSize','QuotaPeakPagedPoolUsage','QuotaPagedPoolUsage','QuotaPeakNonPagedPoolUsage','QuotaNonPagedPoolUsage','PagefileUsage','PeakPagefileUsage')]

class NativeMemory:
    def __init__(self,pid):
        self.values=[];self.peak=0;self.stop_event=threading.Event();self.pid=pid
        ctypes.windll.kernel32.OpenProcess.restype=wintypes.HANDLE
        ctypes.windll.kernel32.CloseHandle.argtypes=[wintypes.HANDLE]
        ctypes.windll.psapi.GetProcessMemoryInfo.argtypes=[wintypes.HANDLE,ctypes.POINTER(Counters),wintypes.DWORD]
        self.handle=ctypes.windll.kernel32.OpenProcess(0x410,False,pid)
        if not self.handle:raise ctypes.WinError()
        self.thread=threading.Thread(target=self.run,daemon=True);self.thread.start()
    def sample(self):
        counters=Counters();counters.cb=ctypes.sizeof(counters)
        if not ctypes.windll.psapi.GetProcessMemoryInfo(self.handle,ctypes.byref(counters),counters.cb):raise ctypes.WinError()
        self.peak=max(self.peak,counters.PeakWorkingSetSize)
        return counters.WorkingSetSize
    def run(self):
        while not self.stop_event.wait(.25):
            try:self.values.append(self.sample())
            except OSError:return
    def stop(self):
        self.stop_event.set();self.thread.join(timeout=2);ctypes.windll.kernel32.CloseHandle(self.handle)
        return {'samples':len(self.values),'peakWorkingSetMiB':round(self.peak/1024**2,3),'scope':'native companion process only; browser excluded'}
