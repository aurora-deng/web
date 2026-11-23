"""Persistent data source and editable dashboard definitions."""
import hashlib
import json
import math
import re
import uuid

from .events import Invalid
from .custom_alerts import OPS, REDUCERS, RULE_LIMIT
from .sources import PANELS
from .store import Conflict, encode
from .target_http import clean_url

BUILTIN = "nebula"
SOURCE_LIMIT, BOARD_LIMIT, PANEL_LIMIT = 8, 24, 20
VIEWS = {"line", "area", "stat", "gauge", "table"}
COLORS = {"green", "blue", "yellow", "orange", "purple", "pink"}
KEY = re.compile(r"[a-f0-9]{24}")
NAME = re.compile(r"[a-zA-Z_:][a-zA-Z0-9_:]{0,159}")


def required_text(value, field, limit, empty=False):
    if not isinstance(value,str) or len(value)>limit or (not empty and not value.strip()):
        raise Invalid(field+"格式或长度无效")
    return value.strip()


def key(value):
    if not isinstance(value,str) or not KEY.fullmatch(value):
        raise Invalid("记录 ID 无效")
    return value


class Workspace:
    def __init__(self, store):
        self.store = store
        with store.connect() as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS observer_configs(
                    kind TEXT NOT NULL, id TEXT NOT NULL, revision INTEGER NOT NULL,
                    body TEXT NOT NULL, PRIMARY KEY(kind,id));
                CREATE TABLE IF NOT EXISTS source_samples(
                    source TEXT NOT NULL, ts INTEGER NOT NULL, body TEXT NOT NULL,
                    PRIMARY KEY(source,ts));
                CREATE INDEX IF NOT EXISTS source_sample_time ON source_samples(ts);
            """)

    @staticmethod
    def record(row):
        return {**json.loads(row["body"]), "id":row["id"], "revision":row["revision"]}

    def list(self, kind):
        with self.store.read() as db:
            return self._list(db, kind)

    def _list(self, db, kind):
        return [self.record(row) for row in db.execute(
            "SELECT * FROM observer_configs WHERE kind=? ORDER BY rowid",(kind,))]

    def get(self, kind, ident):
        with self.store.read() as db:
            return self._get(db, kind, ident)

    def _get(self, db, kind, ident):
        row = db.execute("SELECT * FROM observer_configs WHERE kind=? AND id=?",(kind,ident)).fetchone()
        if not row:
            raise Invalid("记录不存在")
        return self.record(row)

    def save(self, kind, request):
        if not isinstance(request,dict):
            raise Invalid("需要配置对象")
        ident, version = request.get("id"), request.get("revision")
        if ident is not None and (type(version) is not int or version<1):
            raise Invalid("编辑需要配置版本")
        if ident is not None:
            key(ident)
        limit = {"source":SOURCE_LIMIT,"board":BOARD_LIMIT,"alert":RULE_LIMIT}[kind]
        with self.store.connect() as db:
            # Pin source existence and the version for this write.
            db.execute("BEGIN IMMEDIATE")
            if kind == "source":
                body = source_body(request)
            elif kind=="board":
                names = {src["id"]:src["type"] for src in self._list(db,"source")}
                names[BUILTIN] = BUILTIN
                body = board_body(request,names)
            else:
                body = self.alert(request, db)
            if ident is None:
                if db.execute("SELECT count(*) FROM observer_configs WHERE kind=?",(kind,)).fetchone()[0]>=limit:
                    raise Invalid("已达到配置数量上限")
                ident = uuid.uuid4().hex[:24]
                db.execute("INSERT INTO observer_configs VALUES (?,?,1,?)",(kind,ident,encode(body)))
                version = 0
            else:
                previous = db.execute("SELECT revision,body FROM observer_configs WHERE kind=? AND id=?",
                                      (kind,ident)).fetchone()
                if not previous or previous["revision"]!=version:
                    raise Conflict("配置已经变化，请重新打开后再编辑")
                if kind=="board" and json.loads(previous["body"]).get("provisioned"):
                    raise Conflict("此仪表盘由本地文件管理，请修改 JSON 后同步")
                if kind=="source":
                    old=json.loads(previous["body"])
                    if old["type"]!=body["type"] and self.referenced(db,ident):
                        raise Conflict("仪表盘或告警规则正在使用此数据源，不能改变类型")
                    if (old["type"],old["url"]) != (body["type"],body["url"]):
                        db.execute("DELETE FROM source_samples WHERE source=?",(ident,))
                db.execute("UPDATE observer_configs SET revision=?,body=? WHERE kind=? AND id=?",
                           (version+1,encode(body),kind,ident))
        return {**body,"id":ident,"revision":version+1}

    @staticmethod
    def referenced(db, ident):
        for row in db.execute("SELECT kind,body FROM observer_configs WHERE kind IN ('board','alert')"):
            body=json.loads(row["body"])
            if row["kind"]=="alert" and body["source"]==ident:
                return True
            if row["kind"]=="board" and any(panel["source"]==ident for panel in body["panels"]):
                return True
        return False

    def remove(self, kind, request):
        ident, revision = key(request.get("id")), request.get("revision")
        if type(revision) is not int:
            raise Invalid("删除需要配置版本")
        with self.store.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            if kind=="source" and self.referenced(db,ident):
                raise Conflict("此数据源仍被仪表盘或告警规则引用")
            if kind=="board":
                row=db.execute("SELECT body FROM observer_configs WHERE kind='board' AND id=?",(ident,)).fetchone()
                if row and json.loads(row["body"]).get("provisioned"):
                    raise Conflict("此仪表盘由本地文件管理")
            result=db.execute("DELETE FROM observer_configs WHERE kind=? AND id=? AND revision=?",(kind,ident,revision))
            if not result.rowcount:
                raise Conflict("配置已经变化，请重新打开后再删除")
            if kind=="source":
                db.execute("DELETE FROM source_samples WHERE source=?",(ident,))

    def alert(self, request, db=None):
        source=required_text(request.get("source"),"数据源",24)
        expression=required_text(request.get("expr"),"查询",1024)
        # A write's dependency lookup belongs to its existing transaction. Opening
        # another connection here can block behind a concurrent pending writer.
        kind=BUILTIN if source==BUILTIN else (self._get(db,"source",key(source)) if db is not None
                                           else self.get("source",key(source)))["type"]
        if (kind==BUILTIN and expression not in PANELS
                or kind=="metrics" and not NAME.fullmatch(expression)
                or kind=="http" and expression not in ("probe_success","probe_duration_ms","probe_http_status")
                or kind not in (BUILTIN,"prometheus","metrics","http")):
            raise Invalid("请输入所选数据源支持的指标或表达式")
        reducer,op=request.get("reducer"),request.get("op")
        threshold,window,pending=request.get("threshold"),request.get("windowSeconds"),request.get("pendingSeconds")
        enabled=request.get("enabled")
        severity=request.get("severity","warning")
        if (not isinstance(reducer,str) or reducer not in REDUCERS or not isinstance(op,str) or op not in OPS
                or type(threshold) not in (int,float) or abs(threshold)>1e15 or not math.isfinite(threshold)
                or type(window) is not int or window not in (30,60,300,900)
                or type(pending) is not int or not 0<=pending<=3600 or type(enabled) is not bool
                or severity not in ("info","warning","critical")):
            raise Invalid("统计方式、阈值、持续时间或等级无效")
        return {"name":required_text(request.get("name"),"告警名称",80),"source":source,"expr":expression,
                "reducer":reducer,"op":op,"threshold":threshold,"windowSeconds":window,
                "pendingSeconds":pending,"enabled":enabled,"severity":severity,
                "unit":required_text(request.get("unit",""),"单位",24,True),
                "impact":required_text(request.get("impact",""),"影响与处理",200,True)}
    def provision(self, definitions, check=False):
        results=[]
        with self.store.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            sources={row["id"]:row["type"] for row in self._list(db,"source")}
            sources[BUILTIN]=BUILTIN
            count=db.execute("SELECT count(*) FROM observer_configs WHERE kind='board'").fetchone()[0]
            for name,definition in definitions:
                ident=hashlib.sha256(("observer.provision.v1/"+name).encode()).hexdigest()[:24]
                spec=board_body(definition,sources)
                body=encode({**spec,"provisioned":name})
                previous=db.execute("SELECT revision,body FROM observer_configs WHERE kind='board' AND id=?",
                                    (ident,)).fetchone()
                if previous and not json.loads(previous["body"]).get("provisioned"):
                    raise Conflict("配置 ID 被网页创建的仪表盘占用")
                status="unchanged" if previous and previous["body"]==body else "updated" if previous else "created"
                revision=previous["revision"] if previous else 0
                if status!="unchanged":
                    revision+=1
                    count+=int(not previous)
                    if count>BOARD_LIMIT:
                        raise Invalid("已达到仪表盘数量上限")
                    if not check:
                        db.execute("INSERT INTO observer_configs VALUES ('board',?,?,?) "
                                   "ON CONFLICT(kind,id) DO UPDATE SET revision=excluded.revision,body=excluded.body",
                                   (ident,revision,body))
                results.append({"file":name,"id":ident,"revision":revision,"status":status})
        return results


def provision_files(directory, workspace, check=False):
    if not directory.is_dir():
        raise Invalid("仪表盘目录不存在")
    files=sorted(directory.glob("*.json"))
    if not files or len(files)>BOARD_LIMIT:
        raise Invalid("目录需包含 1 至 24 个仪表盘 JSON 文件")
    definitions=[]
    for path in files:
        if path.is_symlink() or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}",path.stem) or path.stat().st_size>256*1024:
            raise Invalid("无效的仪表盘文件 "+path.name)
        board=json.loads(path.read_text(encoding="utf-8-sig"))
        if not isinstance(board,dict) or board.get("schema")!="nebula-observer/1":
            raise Invalid("仪表盘 JSON 版本无效 "+path.name)
        definitions.append((path.name,board))
    return workspace.provision(definitions,check)


def source_body(row):
    url = clean_url(row.get("url"))
    name = required_text(row.get("name"),"数据源名称",70)
    kind = row.get("type")
    if kind not in ("prometheus","metrics","http","tempo","loki","jaeger","pyroscope"):
        raise Invalid("未知数据源类型")
    metrics = row.get("metrics",[])
    if not isinstance(metrics,list) or len(metrics)>24 or any(not isinstance(m,str) or not NAME.fullmatch(m) for m in metrics):
        raise Invalid("最多填写 24 个指标名")
    return {"name":name,"type":kind,"url":url,"metrics":list(dict.fromkeys(metrics)) if kind=="metrics" else []}


def board_body(row, sources):
    title = required_text(row.get("title"),"仪表盘名称",80)
    panels = row.get("panels")
    if not isinstance(panels,list) or len(panels)>PANEL_LIMIT:
        raise Invalid("每个仪表盘最多 20 个面板")
    result = []
    for panel in panels:
        if not isinstance(panel,dict):
            raise Invalid("需要面板配置")
        name=required_text(panel.get("title"),"面板名称",100)
        ident=required_text(panel.get("source"),"数据源",24)
        display,color=panel.get("view"),panel.get("color","green")
        width=panel.get("width",1)
        if ident not in sources or display not in VIEWS or color not in COLORS or type(width) is not int or width not in (1,2):
            raise Invalid("面板数据源、图形、颜色或宽度无效")
        limit=panel.get("max",100)
        if type(limit) not in (float,int) or not 0 < limit <= 1e12:
            raise Invalid("仪表最大值需为正数")
        queries=panel.get("queries")
        if not isinstance(queries,list) or not 1<=len(queries)<=4:
            raise Invalid("每个面板需要一至四个查询")
        items=[]
        for query in queries:
            if not isinstance(query,dict):
                raise Invalid("需要查询配置")
            expr=required_text(query.get("expr"),"查询",1024)
            alias=required_text(query.get("alias",""),"图例名称",60,True)
            kind=sources[ident]
            if kind in ("tempo","loki","jaeger","pyroscope"):
                raise Invalid("链路、日志和剖析数据源请在对应工作区使用")
            if kind==BUILTIN and expr not in PANELS:
                raise Invalid("内置指标不存在")
            if kind=="metrics" and not NAME.fullmatch(expr):
                raise Invalid("原始指标源请输入准确的指标名")
            if kind=="http" and expr not in ("probe_success","probe_duration_ms","probe_http_status"):
                raise Invalid("未知 HTTP 探测指标")
            items.append({"expr":expr,"alias":alias})
        result.append({"title":name,"source":ident,"view":display,"color":color,"width":width,
                       "unit":required_text(panel.get("unit",""),"单位",24,True),"max":limit,"queries":items})
    return {"title":title,"panels":result}
