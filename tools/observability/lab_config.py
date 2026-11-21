"""Build loopback-only Prometheus, trace and notification configurations."""

import json

from .observer.alert_policy import prometheus_alerts


def build(run, ports, targets, *, full=False):
    def address(name):
        return f"127.0.0.1:{ports[name]}"

    def static(endpoint, instance=None):
        labels = {"bc": "lab", "app": "nebula"}
        if instance:
            labels["instance"] = instance
        return {"targets": [endpoint], "labels": labels}

    jobs = {k: [static(address(k))] for k in ("observer", "prometheus")}
    jobs["node"] = [static(address("node"), targets[0][0] if len(targets) == 1 else "lab-host")]
    jobs["probe"] = [static(address(probe), instance) for instance, _, probe in targets]
    if full:
        jobs["collector"] = [static(address("collector_metrics"))]
        jobs["tempo"] = [static(address("tempo"))]
    if "alertmanager" in ports:
        jobs["alertmanager"] = [static(address("alertmanager"))]
    if "gateway_admin" in ports:
        jobs["gateway"] = [static(address("gateway_admin"))]
    scrape = [{"job_name": job, "static_configs": configs,
               **({"metrics_path": "/stats/prometheus"} if job == "gateway" else {})}
              for job, configs in jobs.items()]
    scrape.append({"job_name": "nebula", "file_sd_configs": [
        {"files": [str(run/"nebula-targets.json")], "refresh_interval": "5s"}],
        "authorization": {"type": "Bearer", "credentials_file": str(run/"scrape-token")}})
    prometheus = {"global": {"scrape_interval": "15s", "scrape_timeout": "6s", "evaluation_interval": "15s"},
                  "rule_files": [str(run/"alerts.yaml")], "scrape_configs": scrape}
    extra = tuple(name for name in ("collector", "tempo", "alertmanager", "gateway") if name in jobs)
    files = {"nebula-targets.json": [static(f"127.0.0.1:{port}", instance) for instance, port, _ in targets],
             "alerts.yaml": {"groups": [{"name": "nebula", "rules": prometheus_alerts(extra)}]}}
    if "alertmanager" in ports:
        prometheus["alerting"] = {"alertmanagers": [{"static_configs": [
            {"targets": [address("alertmanager")]}]}]}
        files["alertmanager.yaml"] = {
            "route": {"receiver": "loopback", "group_by": ["alertname", "bc", "app"],
                      "group_wait": "1s", "group_interval": "5s", "repeat_interval": "10m"},
            "receivers": [{"name": "loopback", "webhook_configs": [
                {"url": "http://"+address("alert_receiver")+"/webhook", "send_resolved": True, "max_alerts": 16}]}]}
    files["prometheus.yaml"] = prometheus
    if full:
        files.update(trace_configs(run, ports, address))
    return files


def trace_configs(run, ports, address):
    collector = {
        "receivers": {"otlp": {"protocols": {"http": {"endpoint": address("collector")}}}},
        "processors": {"memory_limiter": {"check_interval": "1s", "limit_mib": 128, "spike_limit_mib": 32},
                       "batch": {"timeout": "1s", "send_batch_size": 128, "send_batch_max_size": 256}},
        "exporters": {"otlp_http": {"endpoint": "http://"+address("tempo_ingest"), "sending_queue": {"queue_size": 32},
                                    "retry_on_failure": {"initial_interval": "500ms", "max_interval": "2s",
                                                         "max_elapsed_time": "5s"}}},
        "service": {"pipelines": {"traces": {"receivers": ["otlp"], "processors": ["memory_limiter", "batch"],
                                            "exporters": ["otlp_http"]}},
                    "telemetry": {"metrics": {"readers": [
                        {"pull": {"exporter": {"prometheus": {"host": "127.0.0.1", "port": ports["collector_metrics"]}}}}]}}},
    }
    loopback_ring = {"instance_addr": "127.0.0.1"}
    tempo = {
        "target": "all",
        "server": {"http_listen_address": "127.0.0.1", "http_listen_port": ports["tempo"],
                   "grpc_listen_address": "127.0.0.1", "grpc_listen_port": ports["tempo_grpc"]},
        "distributor": {"ring": loopback_ring,
                        "receivers": {"otlp": {"protocols": {"http": {"endpoint": address("tempo_ingest")}}}}},
        "memberlist": {"bind_addr": ["127.0.0.1"], "advertise_addr": "127.0.0.1"},
        "live_store": {"ring": loopback_ring, "wal": {"path": str(run/"tempo/live-store/wal")},
                       "shutdown_marker_dir": str(run/"tempo/live-store/shutdown-marker")},
        "metrics_generator": {"ring": loopback_ring},
        "storage": {"trace": {"backend": "local", "wal": {"path": str(run/"tempo/wal")},
                              "local": {"path": str(run/"tempo/blocks")}}},
        "backend_scheduler": {"local_work_path": str(run/"tempo/scheduler")},
        # Tempo 3.0.3 owns its retention policy in the backend worker.
        "backend_worker": {"ring": loopback_ring, "compaction": {"block_retention": "168h"}},
        "usage_report": {"reporting_enabled": False},
    }
    return {"collector.yaml": collector, "tempo.yaml": tempo}


def write(run, ports, targets, writer, *, full=False):
    for name, contents in build(run, ports, targets, full=full).items():
        writer(run/name, json.dumps(contents, ensure_ascii=False))
