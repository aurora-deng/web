const metrics=(title, expr, unit="", view="line", alias="")=>({title,unit,view,width:1,color:"green",max:100,
  queries:[{expr,alias}]});

export const templates=[
  {title:"NEBULA 服务与聊天",kind:"nebula",hint:"内置指标 · 请求、尾延迟与数据库",
    panels:[metrics("HTTP 请求速率","http","次/秒","area"),metrics("会话查询 P99","http_p99","ms"),
      metrics("历史查询 P99","history_p99","ms"),metrics("聊天 ACK 就绪 P99","chat_ack_ready","ms"),
      {...metrics("数据库连接与排队","queue"),queries:[{expr:"queue",alias:"任务排队"},{expr:"pool",alias:"借出连接"}]},
      metrics("Outbox 投递速率","outbox","条/秒")]},
  {title:"NEBULA 主机与连接",kind:"nebula",hint:"内置指标 · CPU、内存、线程与 WebSocket",
    panels:[metrics("CPU 使用率","cpu","%","gauge"),metrics("内存使用率","memory","%","gauge"),
      metrics("进程线程","threads","线程","stat"),metrics("WebSocket 在线","ws","连接") ]},
  {title:"HTTP 服务可用性",kind:"http",hint:"HTTP URL · 状态码、成功标记与延迟",
    panels:[metrics("最近是否成功","probe_success","","stat"),metrics("探测延迟","probe_duration_ms","ms","area"),
      metrics("状态码原始记录","probe_http_status","","table")]},
  {title:"Linux 主机 · Node Exporter",kind:"prometheus",hint:"Prometheus + Node Exporter",
    panels:[metrics("CPU 使用率",'100*(1-avg by(instance)(rate(node_cpu_seconds_total{mode="idle"}[5m])))',"%","gauge"),
      metrics("内存使用率","100*(1-node_memory_MemAvailable_bytes/node_memory_MemTotal_bytes)","%","gauge"),
      metrics("根盘剩余",'node_filesystem_avail_bytes{mountpoint="/"}/1073741824',"GiB"),
      metrics("网卡接收",'sum by(instance)(rate(node_network_receive_bytes_total{device!="lo"}[5m]))',"B/秒","area")]},
  {title:"Prometheus 抓取目标",kind:"prometheus",hint:"Prometheus · 可按实例及 job 筛选",
    panels:[metrics("目标可用性",'up{instance=~"${instance}",job=~"${job}"}',"","stat"),
      metrics("抓取耗时",'1000*scrape_duration_seconds{instance=~"${instance}",job=~"${job}"}',"ms"),
      metrics("抓取样本量",'scrape_samples_scraped{instance=~"${instance}",job=~"${job}"}',"条","area")]},
  {title:"多机进程资源 · Process Exporter",kind:"prometheus",hint:"要求 Process Exporter 并配置进程组，按 instance / groupname 比较",
    panels:[metrics("进程组 CPU",'100*sum by(instance,groupname)(rate(namedprocess_namegroup_cpu_seconds_total{instance=~"${instance}"}[2m]))',"%"),
      metrics("进程组 RSS",'sum by(instance,groupname)(namedprocess_namegroup_memory_bytes{memtype="resident",instance=~"${instance}"})/1048576',"MiB"),
      metrics("进程组线程数",'namedprocess_namegroup_num_threads{instance=~"${instance}"}',"线程")]},
  {title:"Kubernetes 容器资源",kind:"prometheus",hint:"要求 kubelet / cAdvisor，含容器 CPU、工作集、CPU 限流",
    panels:[metrics("容器 CPU · 单核=100%",'100*sum by(namespace,pod,container)(rate(container_cpu_usage_seconds_total{container!="",image!="",namespace=~"${namespace}",pod=~"${pod}"}[2m]))',"%"),
      metrics("容器内存工作集",'sum by(namespace,pod,container)(container_memory_working_set_bytes{container!="",image!="",namespace=~"${namespace}",pod=~"${pod}"})/1048576',"MiB"),
      metrics("容器 CPU 限流周期速率",'sum by(namespace,pod,container)(rate(container_cpu_cfs_throttled_periods_total{container!="",namespace=~"${namespace}",pod=~"${pod}"}[2m]))',"周期/秒")]},
  {title:"Kubernetes 工作负载",kind:"prometheus",hint:"Prometheus + kube-state-metrics · 可按集群、命名空间和 Pod 筛选",
    panels:[metrics("运行中的 Pod",'sum by(namespace) (kube_pod_status_phase{phase="Running",cluster=~"${cluster}",namespace=~"${namespace}",pod=~"${pod}"})',"Pod","stat"),
      metrics("容器重启速率",'sum by(namespace,pod) (rate(kube_pod_container_status_restarts_total{cluster=~"${cluster}",namespace=~"${namespace}",pod=~"${pod}"}[5m]))',"次/秒"),
      metrics("部署可用副本",'sum by(namespace,deployment) (kube_deployment_status_replicas_available{cluster=~"${cluster}",namespace=~"${namespace}"})',"副本")]},
  {title:"PostgreSQL 数据库",kind:"prometheus",hint:"Prometheus + postgres_exporter",
    panels:[metrics("Exporter 连通","pg_up","","stat"),metrics("活动连接","sum by(instance)(pg_stat_database_numbackends)","连接"),
      metrics("事务提交速率","sum by(instance)(rate(pg_stat_database_xact_commit[5m]))","次/秒"),
      metrics("回滚速率","sum by(instance)(rate(pg_stat_database_xact_rollback[5m]))","次/秒")]},
  {title:"Redis 缓存",kind:"prometheus",hint:"Prometheus + redis_exporter",
    panels:[metrics("Redis 连通","redis_up","","stat"),metrics("使用内存","redis_memory_used_bytes/1048576","MiB"),
      metrics("连接客户端","redis_connected_clients","连接"),metrics("命中速率","rate(redis_keyspace_hits_total[5m])","次/秒")]},
  {title:"Envoy 网关",kind:"prometheus",hint:"Prometheus 抓取 Envoy 的 /stats/prometheus · 按 job 和实例筛选",
    panels:[metrics("抓取状态",'up{job=~"${job}",instance=~"${instance}"}',"","stat"),
      metrics("活动连接",'sum by(instance)(envoy_http_downstream_cx_active{job=~"${job}",instance=~"${instance}"})',"连接"),
      metrics("入口请求速率",'sum by(instance)(rate(envoy_http_downstream_rq_total{job=~"${job}",instance=~"${instance}"}[5m]))',"次/秒","area"),
      metrics("新上游连接 P99",'histogram_quantile(0.99,sum by(instance,le)(rate(envoy_cluster_upstream_cx_connect_ms_bucket{job=~"${job}",instance=~"${instance}"}[5m])))',"ms")]},
  {title:"微服务请求 · OTel RED",kind:"prometheus",hint:"要求 OTel spanmetrics 或兼容服务指标，按 service_name 维度汇总",
    panels:[metrics("服务调用速率",'sum by(service_name)(rate(traces_span_metrics_calls_total{span_kind="SPAN_KIND_SERVER",service_name=~"${service}"}[5m]))',"次/秒","area"),
      metrics("服务错误速率",'sum by(service_name)(rate(traces_span_metrics_calls_total{span_kind="SPAN_KIND_SERVER",status_code="STATUS_CODE_ERROR",service_name=~"${service}"}[5m]))',"次/秒"),
      metrics("服务 P95",'histogram_quantile(0.95,sum by(service_name,le)(rate(traces_span_metrics_duration_milliseconds_bucket{span_kind="SPAN_KIND_SERVER",service_name=~"${service}"}[5m])))',"ms")]},
];

const grafanaTypes={timeseries:"line",graph:"line",stat:"stat",singlestat:"stat",gauge:"gauge",bargauge:"gauge",table:"table"};
const units={percent:"%",ms:"ms",s:"s",bytes:"B",Bps:"B/秒",reqps:"次/秒",short:"",none:""};
const transferable=new Set(["bc","app","cluster","namespace","service","job","pod","instance","node"]);

function grafanaExpression(value) {
  const expression=value.replace(/(=~|!~)\s*"\$(?:\{([a-z_][\w]*)(?::regex)?\}|([a-z_][\w]*))"/gi,
    (raw,operator,braced,plain)=>{
      const name=(braced||plain).toLowerCase();
      return transferable.has(name)?operator+'"${'+name+'}"':raw;
    });
  if(/[$]|\[\[[^\]]+\]\]/.test(expression.replace(/\$\{([a-z_]+)\}/g,(raw,key)=>transferable.has(key)?"":raw)))
    return null;
  return expression;
}

export function importDashboard(value, source) {
  if (value?.schema==="nebula-observer/1" && Array.isArray(value.panels)) {
    if(value.panels.length>20) throw new Error("导入文件超过 20 个面板");
    const ids=[...new Set(value.panels.map(panel=>panel?.source))];
    if(ids.some(ident=>typeof ident!=="string" || !ident || ident.length>24))throw new Error("Observer 本地模板缺少有效的数据源引用");
    const defined=new Map((Array.isArray(value.sources)?value.sources:[])
      .filter(ref=>ref && typeof ref.id==="string").map(ref=>[ref.id,ref]));
    return {title:value.title,panels:value.panels,references:ids.map(ident=>defined.get(ident)||{id:ident}),skipped:0,skippedQueries:0};
  }
  const board=value?.dashboard || value;
  if(!board || !Array.isArray(board.panels)) throw new Error("需要 Observer 或 Grafana 仪表盘 JSON");
  if(!source) throw new Error("先选择要绑定的 Prometheus 数据源");
  const panels=[];
  let skipped=0,skippedQueries=0;
  for(const p of board.panels.flatMap(p=>p?.type==="row" && Array.isArray(p.panels) ? p.panels : [p])) {
    const view=grafanaTypes[p?.type];
    if(!view || !Array.isArray(p.targets) || panels.length>=20) {skipped++;continue;}
    const queries=[];
    for(const t of p.targets) {
      if(!t || t.hide)continue;
      const expression=typeof t.expr==="string" && t.expr.trim() && t.expr.length<=1024 &&
        ![t.datasource?.type,p.datasource?.type].some(kind=>kind && kind!=="prometheus") ? grafanaExpression(t.expr) : null;
      if(!expression || queries.length>=4) {skippedQueries++;continue;}
      queries.push({expr:expression,alias:String(t.legendFormat||"").slice(0,60)});
    }
    if(!queries.length) {skipped++;continue;}
    panels.push({...metrics(String(p.title || "指标").slice(0,100),queries[0].expr,units[p.fieldConfig?.defaults?.unit] || "",view),
      source,queries,width:(p.gridPos?.w || 0)>=18 ? 2 : 1, max:p.fieldConfig?.defaults?.max>0 ? p.fieldConfig.defaults.max : 100});
  }
  if(!panels.length) throw new Error("没有可导入的 Prometheus 面板。含变量、插件或其他数据源的面板需手工适配。");
  return {title:String(board.title||"导入的仪表盘").slice(0,80),panels,references:[],skipped,skippedQueries};
}
