"""Run destructive fixtures ONLY in the existing isolated _test DB, then restore daily service."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[3]
daily=root/'scripts/function3/runtime/service.py'
isolated=root/'scripts/function3/validation/run_server.py'
def run(*args):subprocess.run(['python3',*[str(x) for x in args]],cwd=root,check=True)
run(daily,'stop')
try:
    run(isolated,'start','--tts')
    run(root/'tests/function3/integration/runtime_blackbox.py','--allow-isolated-admin',
        '--webm-fixture-dir','/home/pikachu/phase11-data/function3/validation/media-fixtures')
finally:
    try:run(isolated,'stop')
    finally:run(daily,'start')
