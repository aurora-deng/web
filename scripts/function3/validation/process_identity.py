"""Read-only identity check for a process created by the isolated launcher."""
from pathlib import Path

def owned_process(pid, ticks, executable, proc_root=Path('/proc')):
    process=proc_root/str(pid)
    try:
        # comm is parenthesized and may contain spaces; the suffix starts at state.
        fields=(process/'stat').read_text().rsplit(')',1)[1].split()
        if fields[0] in ('Z','X'):return None
        start=fields[19]
        command=(process/'cmdline').read_bytes().split(b'\0')[0]
    except (FileNotFoundError,ProcessLookupError):return None
    if start!=ticks:raise RuntimeError('PID identity mismatch; refusing to control it')
    # Between clearing argv and becoming a zombie, an exiting Linux process may
    # still have a stat entry. Its unchanged start time proves it is ending, not reused.
    if not command:return None
    if command.decode()!=str(executable):raise RuntimeError('PID identity mismatch; refusing to control it')
    return int(pid)
