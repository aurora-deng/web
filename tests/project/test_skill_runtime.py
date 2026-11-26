"""Real local adapters/dispatcher contracts; source fixtures are synthetic and owner-scoped."""
from concurrent.futures import ThreadPoolExecutor
from contextlib import closing
from dataclasses import replace
from pathlib import Path
import json
import sqlite3
import sys
import tempfile
import time
import unittest

ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT))
from backend.agent.tool_runtime.registry import default_registry, Effect, Precondition
from backend.agent.tool_runtime.runtime import ToolRuntime, MemoryAuditSink, SqliteAuditSink, encode
from backend.agent.tool_runtime.skills import SkillRuntime
from backend.agent.tool_runtime.workspace import WorkspaceStore,FleetSnapshot,NativeFleetPort,bind_workspace,connect

class Skills(unittest.TestCase):
    def setUp(self):
        base=ROOT/"build/skill-runtime-tests";base.mkdir(parents=True,exist_ok=True)
        self.root=Path(tempfile.mkdtemp(prefix="contract-",dir=base))
        with connect(self.root/"fleet.sqlite") as db:
            db.executescript("CREATE TABLE tasks(task_id TEXT,trace_id TEXT,epoch INTEGER,robot_id TEXT,phase INTEGER,outcome TEXT);")
            db.executemany("INSERT INTO tasks VALUES (?,?,?,?,?,?)",[("queued","a"*32,1,"",1,""),("failed","b"*32,2,"sim",6,"precondition_failed")])
        with connect(self.root/"journal.sqlite") as db:
            db.executescript("CREATE TABLE workflow_runs(run_id TEXT); CREATE TABLE workflow_nodes(run_id TEXT,node_id TEXT,attempts INTEGER,effect INTEGER,result_kind INTEGER,error_code TEXT,value TEXT); INSERT INTO workflow_runs VALUES ('run1'); INSERT INTO workflow_nodes VALUES ('run1','validate',1,0,1,'','');")
        self.fleet=FleetSnapshot(self.root/"fleet.sqlite",self.root/"journal.sqlite")
        self.store=WorkspaceStore(self.root/"workspace.sqlite","owner",self.fleet);self.store.seed_demo()
        self.reset_runtime()

    def reset_runtime(self):
        self.audit=MemoryAuditSink(10000)
        self.tools=ToolRuntime(default_registry(),self.audit);bind_workspace(self.tools,self.store)
        self.skills=SkillRuntime(self.tools,self.store)

    def invoke(self,name,params,actor="owner",confirm=False):
        proof=self.skills.confirm(actor,name,params) if confirm else None
        return self.skills.invoke(name,params,actor_id=actor,confirmation=proof)

    def progress(self,key="once"): return {"date":"2026-11-25","subject":"systems","itemId":"q1","minutes":25,"idempotencyKey":key}

    def test_every_advertised_skill_has_a_real_recipe(self):
        self.assertEqual(set(self.skills.recipes),set(self.skills.registry.skills))
        self.assertEqual(len(self.skills.recipes),19)
        cases={
            "fleet.get_task_status":{"taskId":"queued"},"fleet.fleet.overview":{"entity":"tasks","limit":16},
            "workflow.run.trace":{"runId":"run1","nodeLimit":10},"metrics.query":{"names":["fleet.active_tasks"],"windowSeconds":30},
            "chat.recall":{"query":"workflow","scope":"workspace","limit":5},
            "chat.summarize":{"conversationId":"demo-chat","messageId":"m2","radius":1},
            "study.search_material":{"query":"workflow","subject":"systems","type":"note","limit":2},
            "study.explain":{"materialId":"demo-note","anchor":{"kind":"page","offset":1}},"study.quiz":{"subject":"systems"},
            "study.log_progress":self.progress(),"study.mistake_add":{"subject":"systems","itemId":"q1","text":"Review checkpoints","idempotencyKey":"m-once"},
            "fleet.review_failures":{"taskIds":["failed"]},"fleet.task.reassign":{"taskId":"queued","robotId":"sim"},
            "fleet.robot.quarantine":{"robotId":"sim","reason":"operator inspection"},
            "chat.draft_reply":{"conversationId":"demo-chat","messageId":"m2","radius":1},
            "chat.remind":{"conversationId":"demo-chat","messageId":"m2"},
            "chat.escalate":{"conversationId":"demo-chat","reason":"help"},"study.plan_day":{"date":"2026-11-25","subject":"systems"}}
        for name,p in cases.items():
            with self.subTest(skill=name):
                result=self.invoke(name,p,confirm=self.skills.registry.skill(name).effect==Effect.EXTERNAL)
                self.assertTrue(result["ok"],result)
                self.assertLessEqual(len(encode(result)),32768)
                if self.skills.registry.skill(name).effect==Effect.PROPOSAL:
                    self.assertEqual(result["value"]["state"],"pending")
                    self.assertFalse(result["value"]["automaticDispatch"])
        # The remaining recipe requires a real C++ gRPC approval bridge, exercised by agent_skill_process.py.
        self.assertEqual(self.invoke("fleet.propose_sim_reschedule",{"taskId":"queued"})["error"]["code"],"unavailable")

    def test_versions_and_duplicates(self):
        r=default_registry();s=r.skill("fleet.get_task_status")
        with self.assertRaises(KeyError):r.skill(s.name,2)
        r.add_skill(replace(s,version=2));self.assertEqual(r.skill(s.name,2).version,2)
        with self.assertRaises(ValueError):r.add_skill(s)
        tool=r.tool("fleet.get_task_status");r.add_tool(replace(tool,version=2))
        with self.assertRaises(ValueError):r.add_tool(tool)
        self.assertEqual(self.skills.invoke(s.name,{"taskId":"queued"},actor_id="owner",version=2)["error"]["code"],"not_found")

    def test_preconditions_are_typed_not_typo_prone(self):
        r=default_registry();s=r.skill("study.log_progress")
        for conditions in [("authorized_actor","confirmed_user_intent","atomic_idempotent_port"),
                           (Precondition.AUTHORIZED_ACTOR,Precondition.CONFIRMED_USER_INTENT),
                           (Precondition.AUTHORIZED_ACTOR,Precondition.ATOMIC_IDEMPOTENT_PORT)]:
            with self.assertRaises(ValueError):r.add_skill(replace(s,name="test.write",preconditions=conditions))

    def test_confirmation_is_bound_to_exact_actor_skill_and_payload(self):
        p=self.progress();proof=self.skills.confirm("owner","study.log_progress",p)
        self.assertEqual(self.invoke("study.log_progress",p)["error"]["code"],"denied")
        changed=dict(p,minutes=26)
        self.assertEqual(self.skills.invoke("study.log_progress",changed,actor_id="owner",confirmation=proof)["error"]["code"],"denied")
        forged={"actor":"owner","skill":"study.log_progress"}
        self.assertEqual(self.skills.invoke("study.log_progress",p,actor_id="owner",confirmation=forged)["error"]["code"],"denied")
        self.assertEqual(self.invoke("fleet.get_task_status",{"taskId":"queued"},actor="other")["error"]["code"],"denied")

    def test_study_writes_are_atomic_and_replayed_across_restart(self):
        first=self.invoke("study.log_progress",self.progress(),confirm=True);self.assertTrue(first["ok"],first)
        self.reset_runtime()
        replay=self.invoke("study.log_progress",self.progress(),confirm=True)
        self.assertEqual(first["value"],replay["value"])
        self.assertEqual(self.invoke("study.log_progress",dict(self.progress(),minutes=30),confirm=True)["error"]["code"],"conflict")
        with connect(self.store.database,True) as db:self.assertEqual(db.execute("SELECT COUNT(*) FROM progress").fetchone()[0],1)

    def test_concurrent_replay_one_business_effect(self):
        with ThreadPoolExecutor(max_workers=8) as pool:results=list(pool.map(lambda _:self.invoke("study.log_progress",self.progress(),confirm=True),range(8)))
        self.assertTrue(all(r["ok"] for r in results),results)
        self.assertEqual(len({r["value"]["recordId"] for r in results}),1)

    def test_receipt_failure_rolls_back_business_effect(self):
        with connect(self.store.database) as db:db.execute("CREATE TRIGGER abort_receipt BEFORE INSERT ON receipts BEGIN SELECT RAISE(ABORT,'fixture abort'); END")
        self.assertFalse(self.invoke("study.log_progress",self.progress(),confirm=True)["ok"])
        with connect(self.store.database,True) as db:self.assertEqual(db.execute("SELECT COUNT(*) FROM progress").fetchone()[0],0)

    def test_scope_bounds_and_required_filters(self):
        for p in [{"query":"x","limit":1},{"query":"x","scope":"conversation","limit":1},
                  {"query":"x","scope":"workspace","conversationId":"demo-chat","limit":1},{"query":"x","scope":"workspace","limit":51}]:
            self.assertEqual(self.invoke("chat.recall",p)["error"]["code"],"invalid")
        self.assertFalse(self.invoke("chat.summarize",{"conversationId":"demo-chat"})["ok"])
        self.assertFalse(self.invoke("study.search_material",{"query":"x"})["ok"])
        self.assertFalse(self.invoke("fleet.fleet.overview",{"entity":"robots","limit":16})["ok"])
        self.assertFalse(self.invoke("fleet.task.inspect",{"taskId":"queued"})["ok"])

    def test_symmetric_context_and_metadata_only_search(self):
        result=self.invoke("chat.summarize",{"conversationId":"demo-chat","messageId":"m2","radius":1})
        self.assertEqual(result["value"]["messageIds"],["m1","m2","m3"])
        found=self.invoke("study.search_material",{"query":"workflow","subject":"systems","type":"note","limit":1})
        self.assertNotIn("text",found["value"][0])
        self.assertEqual(self.invoke("study.explain",{"materialId":"demo-note","anchor":{"kind":"page","offset":0}})["error"]["code"],"invalid")

    def test_missing_metrics_and_snapshots_are_not_fabricated(self):
        self.assertEqual(self.invoke("metrics.query",{"names":["chat.ack_latency_ms"],"windowSeconds":30})["error"]["code"],"unavailable")
        self.store.fleet=None
        self.assertEqual(self.invoke("fleet.get_task_status",{"taskId":"queued"})["error"]["code"],"unavailable")

    def test_local_proposal_requires_separate_human_decision_and_never_dispatches(self):
        r=self.invoke("fleet.robot.quarantine",{"robotId":"sim","reason":"review"})
        identity=r["value"]["proposalId"]
        self.assertEqual(self.store.proposal("owner",identity)["state"],"pending")
        result=self.skills.decide(identity,actor_id="owner",approve=True)
        self.assertIsNone(result["simulationAction"])
        self.assertFalse(result["automaticDispatch"])
        self.assertTrue(self.skills.decide(identity,actor_id="owner",approve=True)["replayed"])
        with self.assertRaises(Exception):self.skills.decide(identity,actor_id="owner",approve=False)

    def test_planner_is_real_rules_router_and_audit_is_redacted(self):
        plan=self.skills.planner.plan("log_progress",self.progress())
        self.assertEqual(plan["skillId"],"study.log_progress")
        self.assertEqual(plan["planner"],"rules")
        p={"subject":"systems","itemId":"q1","text":"private-note-marker","idempotencyKey":"secret-key-marker"}
        self.assertTrue(self.invoke("study.mistake_add",p,confirm=True)["ok"])
        audit=repr(self.audit.records)
        self.assertNotIn(p["text"],audit);self.assertNotIn(p["idempotencyKey"],audit)

    def test_parent_deadline_is_propagated(self):
        result=self.tools.call("preference.get",{},actor_id="owner",trace_id="deadline",deadline=time.monotonic()-1)
        self.assertEqual(result["error"]["code"],"timeout")

    def test_hot_projection_distinguishes_absent_from_out_of_coverage(self):
        port=object.__new__(NativeFleetPort)
        port.checkpoint=self.root/"hot.json"
        p={"runId":"old-run","nodeLimit":8}
        port.checkpoint.write_text(json.dumps({"format":1,"truncated":True,"runs":["recent"],"nodes":[]}),encoding="utf-8")
        with self.assertRaises(Exception) as error:port.trace(p)
        self.assertEqual(error.exception.code,"unavailable")
        port.checkpoint.write_text(json.dumps({"format":1,"truncated":False,"runs":["recent"],"nodes":[]}),encoding="utf-8")
        with self.assertRaises(Exception) as error:port.trace(p)
        self.assertEqual(error.exception.code,"not_found")
        port.checkpoint.write_text(json.dumps({"format":1,"truncated":True,"runs":["old-run"],"nodes":[]}),encoding="utf-8")
        self.assertEqual(port.trace(p),[])

    def test_hot_projection_keeps_legacy_snapshots_and_bounds(self):
        port=object.__new__(NativeFleetPort)
        port.checkpoint=self.root/"hot.json"
        row={"run_id":"old-run","node_id":"navigate","attempts":1,"result_kind":1}
        p={"runId":"old-run","nodeLimit":1}
        port.checkpoint.write_text(json.dumps({"format":1,"nodes":[row,row]}),encoding="utf-8")
        self.assertEqual(port.trace(p),[{k:v for k,v in row.items() if k!="run_id"}])
        for packet in ({"format":2,"nodes":[]},{"format":1,"truncated":"false","nodes":[]},
                       {"format":1,"runs":["r"]*33,"nodes":[]},{"format":1,"nodes":[{}]},
                       {"format":1,"nodes":[row]*8193}):
            port.checkpoint.write_text(json.dumps(packet),encoding="utf-8")
            with self.assertRaises(Exception) as error:port.trace(p)
            self.assertEqual(error.exception.code,"invalid")

if __name__=="__main__":unittest.main(verbosity=2)
