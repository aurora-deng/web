"""Cross-process test of the actual Fleet TaskWorkflow, retaining evidence and databases."""
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
base=Path.cwd()/"task-workflow-recovery"
base.mkdir(exist_ok=True)
executable=str(Path(sys.argv[1]).resolve())
for crash in (False,True):
    root=Path(tempfile.mkdtemp(prefix="crash-" if crash else "settled-",dir=base))
    def run(mode,epoch,code):
        result=subprocess.run([executable,mode,str(epoch)],cwd=root,capture_output=True,text=True,timeout=10)
        assert result.returncode==code,(result.returncode,result.stdout,result.stderr)
        return result.stdout
    run("crash" if crash else "complete",1,86 if crash else 0)
    outcome=run("complete",1,0)
    assert outcome.startswith("uncertain" if crash else "succeeded"),outcome
    assert (root/"actuations.txt").read_text().splitlines()==["navigate"],"external effect was repeated"
    run("complete",2,3)
    with sqlite3.connect(root/"task.workflow.sqlite") as db:
        nodes=db.execute("SELECT node_id,attempts,result_kind FROM workflow_nodes WHERE run_id='recovery-task'").fetchall()
    assert any(n[0]=="navigate" and n[1]==1 for n in nodes),nodes
    assert all(n[2]==1 for n in nodes if n[0] in {"validate","status","capability"}),nodes
print("Fleet TaskWorkflow persisted checkpoints; settled replay and unknown-outcome restart did not repeat navigation")
