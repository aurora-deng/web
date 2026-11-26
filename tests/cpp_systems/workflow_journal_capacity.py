"""Isolated synthetic journal contracts; preserves evidence and never cleans existing data."""
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

exe = str(Path(sys.argv[1]).resolve(strict=True))
root = Path(tempfile.mkdtemp(prefix="workflow-journal-capacity-", dir=Path.cwd()))
print("JOURNAL_CAPACITY_EVIDENCE", root, flush=True)
for phase in ("populate", "replay", "capacity"):
    subprocess.run([exe, phase], cwd=root, check=True, timeout=120)
    packet = json.loads((root/"archive.snapshot.json").read_text(encoding="utf-8"))
    assert packet["format"] == 1 and packet["truncated"] is True
    assert len(packet["runs"]) <= 32 and len(packet["nodes"]) <= 4096
    assert (root/"archive.snapshot.json").stat().st_size < 2*1024*1024
    if phase == "populate":
        assert "run-0" not in packet["runs"]
    else:
        assert "run-0" in packet["runs"] and any(row["run_id"]=="run-0" and row["node_id"]=="inspect" for row in packet["nodes"])
    assert len((root/"effects.txt").read_text().splitlines()) == 260
with sqlite3.connect((root/"archive.sqlite").as_uri()+"?mode=ro", uri=True) as db:
    assert db.execute("SELECT count(*) FROM workflow_runs").fetchone()[0] == 261
    assert db.execute("SELECT count(*) FROM workflow_nodes").fetchone()[0] == 522
    assert db.execute("SELECT attempts,result_kind FROM workflow_nodes WHERE run_id='run-0' AND node_id='effect'").fetchone() == (1,1)
assert (root/"bytes.sqlite").stat().st_size <= 4*1024*1024
print(json.dumps({"passed":True,"completedRuns":260,"effects":260,"restartReplay":True,"capacityWorkerHealthy":True,"noRowsDeleted":True}))
