"""Real SkillRuntime -> C++ gRPC proposal/approval -> Edge DurableWorkflow -> SQLite trace."""
import argparse
from contextlib import closing
from pathlib import Path
import sqlite3
import sys
import tempfile
import time
import subprocess
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT));sys.path.insert(0,str(Path(__file__).parent))
# Import only the lightweight Lab definitions; the full fleet_process module pulls the
# observability catalog, which this focused integration does not exercise.
import types
fleet_source=(Path(__file__).with_name("fleet_process.py")).read_text(encoding="utf-8")
start=fleet_source.index("def wait_until")
end=fleet_source.index("def ledger_rows")
module=types.ModuleType("fleet_process_light")
module.__dict__.update({"os":__import__("os"),"secrets":__import__("secrets"),"subprocess":__import__("subprocess"),"time":__import__("time"),"json":__import__("json"),"Path":Path})
exec(fleet_source[start:end],module.__dict__)
Lab=module.Lab; wait_until=module.wait_until
from backend.agent.tool_runtime.registry import default_registry
from backend.agent.tool_runtime.runtime import ToolRuntime,SqliteAuditSink
from backend.agent.tool_runtime.skills import SkillRuntime,FleetSimulationBridge
from backend.agent.tool_runtime.workspace import WorkspaceStore,FleetSnapshot,NativeFleetPort,bind_workspace

def main():
    parser=argparse.ArgumentParser()
    for name in ('coordinator','edge','ctl','agent','peer'):parser.add_argument('--'+name,required=True)
    args=parser.parse_args()
    root=Path(tempfile.mkdtemp(prefix='agent-skill-process-',dir=Path.cwd()))
    lab=Lab(args,root)
    try:
        lab.coordinator(agent_sim=True)
        lab.call('submit','preview-done','alpha');lab.edge('alpha',1);lab.phase('preview-done','succeeded')
        queued=lab.call('submit','preview-queued','beta')
        env=dict(lab.env,**{'NEBULA_FLEET_AGENT_'+role+'_TOKEN':token for role,token in lab.agent_tokens.items()})
        readenv=dict(lab.env,NEBULA_FLEET_OPERATOR_TOKEN=lab.agent_tokens['READ'])
        snapshot=NativeFleetPort(args.ctl,lab.address,readenv,root/'alpha.sqlite.workflow.sqlite.snapshot.json')
        store=WorkspaceStore(root/'agent.sqlite','owner',snapshot);store.seed_demo()
        tools=ToolRuntime(default_registry(),SqliteAuditSink(root/'audit.sqlite'));bind_workspace(tools,store)
        bridge=FleetSimulationBridge(args.agent,lab.address,env)
        skills=SkillRuntime(tools,store,bridge)
        listed=tools.call('fleet.list_tasks',{'limit':16},actor_id='owner',trace_id='list-test')
        assert listed['ok'] and len(listed['value'])==2,listed
        draft=skills.invoke('fleet.propose_sim_reschedule',{'taskId':'preview-queued'},actor_id='owner')
        assert draft['ok'],draft
        assert lab.call('status','preview-queued')['phase']=='queued','proposal acted before approval'
        decision=skills.decide(draft['value']['proposalId'],actor_id='owner',approve=True)
        assert decision['state']=='approved',decision
        replacement=decision['simulationAction']['replacementTaskId']
        lab.edge('beta',2);lab.phase(replacement,'succeeded')
        assert lab.call('status','preview-queued')['phase']=='failed'
        trace=skills.invoke('workflow.run.trace',{'runId':'preview-done','nodeLimit':16},actor_id='owner')
        assert trace['ok'] and len(trace['value'])==5,trace
        assert all(n['result_kind']==1 and n['attempts']==1 for n in trace['value']),trace
        parameters={'date':'2026-11-25','subject':'systems','itemId':'q1','minutes':25,'idempotencyKey':'confirmed-once'}
        denied=skills.invoke('study.log_progress',parameters,actor_id='owner');assert denied['error']['code']=='denied'
        proof=skills.confirm('owner','study.log_progress',parameters)
        first=skills.invoke('study.log_progress',parameters,actor_id='owner',confirmation=proof)
        again=skills.invoke('study.log_progress',parameters,actor_id='owner',confirmation=proof)
        assert first['ok'] and first['value']==again['value'],(first,again)
        print('REAL end-to-end: skill parameters, gRPC human approval, simulated replacement, durable Edge checkpoints, atomic external write replay')
    finally:lab.teardown()
if __name__=='__main__':main()
