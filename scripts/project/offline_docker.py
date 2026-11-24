"""Build, smoke-test and export the two application images without pulling a base image."""
import argparse
import gzip
import json
import os
from pathlib import Path, PurePosixPath
import re
import secrets
import shlex
import shutil
import subprocess
import tempfile
import time
from contextlib import contextmanager

from package_release import FLEET_EVIDENCE, FLEET_PROGRAMS, ROOT, WORKFLOW_PROGRAM, WORKFLOW_SCHEMA, extract_payload
from repro_runtime import browser_fixture, business_smoke, disposable_database, get_json, http_smoke, require
from runtime_libs import LOADER, checksum, prepare


LOADER_COMMAND = (f"/runtime/{LOADER}", "--library-path", "/runtime")
ENGINE = "docker"


def command(*args, env=None, timeout=90):
    result = subprocess.run([ENGINE, *args], env=env, capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"Docker {args[0]} exited {result.returncode}: {result.stderr[-1600:]}")
    return result.stdout.strip()


def wait(probe, description, timeout=20):
    end = time.monotonic() + timeout
    last = None
    while time.monotonic() < end:
        try:
            matched = probe()
            if matched:
                return matched
        except (OSError, RuntimeError, ValueError) as error:
            last = error
        time.sleep(0.1)
    raise RuntimeError(f"timeout waiting for {description}: {last}")


def stage_context(release, context):
    context.mkdir()
    app = context / "app"
    app.mkdir()
    for name in ("webserver", "LICENSE"):
        shutil.copy2(release / name, app / name)
    for folder in ("frontend/phase11", "static"):
        shutil.copytree(release / folder, app / folder)
    fleet = context / "fleet"
    fleet.mkdir()
    shutil.copy2(release / "LICENSE", fleet / "LICENSE")
    for name in FLEET_PROGRAMS:
        shutil.copy2(release / name, fleet / PurePosixPath(name).name)
    for name in (WORKFLOW_PROGRAM, WORKFLOW_SCHEMA):
        if (release / name).is_file():
            shutil.copy2(release / name, fleet / PurePosixPath(name).name)
    shutil.copy2(ROOT / "Dockerfile", context / "Dockerfile")
    # The staging directory is private on the host; only paths copied into the
    # images must allow the numeric non-root container user to traverse them.
    for folder in (app, fleet):
        folder.chmod(0o755)
        for path in folder.rglob("*"):
            if path.is_dir():
                path.chmod(0o755)


@contextmanager
def container(image, libraries, args, *, command_args=(), env=None):
    name = "nebula-release-" + secrets.token_hex(6)
    command("run", "--pull=never", "--detach", "--name", name,
            "--read-only", "--cap-drop=ALL", "--security-opt=no-new-privileges",
            "--memory=384m", "--pids-limit=128", "--volume", f"{libraries}:/runtime:ro,Z",
            *args, image, *command_args, env=env)
    try:
        yield name
    finally:
        command("rm", "--force", name, timeout=12)


def verify_web(image, release, libraries, postgres, work, manifest):
    key = work / "token.key"
    key.touch(mode=0o600, exist_ok=False)
    key.write_text(secrets.token_hex(32), encoding="ascii")
    identity = f"{os.getuid()}:{os.getgid()}"
    scratch = f"/state:mode=0700,uid={os.getuid()},gid={os.getgid()},size=32m"
    with disposable_database(postgres, release, work) as (connection, migration):
        parameters = dict(item.split("=", 1) for item in shlex.split(connection))
        socket = parameters["host"]
        parameters["host"] = "/pg"
        env = dict(os.environ, WEB_PHASE11_REQUIRED="1", WEB_FUNCTION3_ENABLE="1",
                   WEB_PHASE11_DATABASE=" ".join(f"{name}={value}" for name, value in parameters.items()),
                   WEB_PHASE11_TOKEN_KEY_FILE="/token.key", WEB_PHASE11_FILE_ROOT="/state/files",
                   WEB_FUNCTION3_MEDIA_ROOT="/state/media")
        names = tuple(name for name in env if name.startswith("WEB_") and name in {
            "WEB_PHASE11_REQUIRED", "WEB_FUNCTION3_ENABLE", "WEB_PHASE11_DATABASE", "WEB_PHASE11_TOKEN_KEY_FILE",
            "WEB_PHASE11_FILE_ROOT", "WEB_FUNCTION3_MEDIA_ROOT"})
        args = ["--user", identity, "--publish", "127.0.0.1::8080", "--tmpfs", scratch,
                "--volume", f"{socket}:/pg:ro,Z", "--volume", f"{key}:/token.key:ro,Z"]
        for name in names:
            args.extend(("--env", name))
        with container(image, libraries, args, env=env) as name:
            port = int(command("port", name, "8080/tcp").rsplit(":", 1)[1])
            try:
                def liveness():
                    require(command("inspect", "--format", "{{.State.Running}}", name) == "true",
                            "Web process stopped before listening")
                    return get_json(port, "/health/live") == {"status": "alive"}

                wait(liveness, "Web liveness", timeout=12)
                web = http_smoke(release, port, True)
                ready = get_json(port, "/health/ready")
                business_smoke(port, browser_fixture(ROOT))
            except Exception as error:
                log = subprocess.run([ENGINE, "logs", "--tail", "18", name], stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, timeout=5).stdout
                raise RuntimeError(f"Web smoke test failed: {error}\n{log}") from error
    require(web["revision"] == manifest["revision"][:12] and
            web["sourceFingerprint"] == manifest["sourceFingerprint"] and
            web["requiredMigration"] == migration == manifest["migration"], "Web image differs from verified ZIP")
    return {**web, "readyResponse": ready.get("status"), "registration": True,
            "webSocket": True, "sse": True, "idempotentStory": True}


def verify_fleet(image, libraries, extended=False):
    operator, robot = secrets.token_hex(24), secrets.token_hex(24)
    env = dict(os.environ, NEBULA_FLEET_OPERATOR_TOKEN=operator, DEMO_EDGE_KEY=robot, NEBULA_FLEET_ROBOT_TOKEN=robot)
    args = ["--network", "none", "--tmpfs", "/state:uid=10001,gid=10001,mode=0700,size=24m",
            "--env", "NEBULA_FLEET_OPERATOR_TOKEN", "--env", "DEMO_EDGE_KEY"]
    coordinator = ("/app/nebula_fleet_coordinator", "127.0.0.1:0", "fleet.sqlite", "alpha", "1", "DEMO_EDGE_KEY")
    with container(image, libraries, args, command_args=coordinator, env=env) as name:
        def listen():
            match = re.search(r"^READY (127\.0\.0\.1:[0-9]+)$", command("logs", name), re.MULTILINE)
            return match[1] if match else None

        address = wait(listen, "Fleet listener")
        command("exec", "--detach", "--env", "NEBULA_FLEET_ROBOT_TOKEN", name, *LOADER_COMMAND,
                "/app/nebula_fleet_edge", address, "alpha", "alpha.sqlite", "1", "5", env=env)

        def task(*options):
            return json.loads(command("exec", name, *LOADER_COMMAND, "/app/nebula_fleet_ctl", address, *options))

        submitted = task("submit", "release-smoke", "alpha")
        def completed():
            observed = task("status", "release-smoke")
            return observed if observed["phase"] == "succeeded" else None

        final = wait(completed, "Fleet gRPC delivery and receipt")
        repeated = task("submit", "release-smoke", "alpha")
        require(final["outcome"] == "arrived" and submitted["epoch"] == repeated["epoch"] == final["epoch"]
                and submitted["traceId"] == repeated["traceId"] == final["traceId"], "Fleet identity or receipt changed")
        queued = task("submit", "agent-smoke", "beta")
        agent = (*LOADER_COMMAND, "/app/nebula_fleet_agent")
        proposal = json.loads(command("exec", name, *agent, address, "propose", "agent-smoke"))
        require(queued["phase"] == "queued" and proposal["proposal"]["skillId"] == "fleet.propose_sim_reschedule",
                "Fleet Agent could not plan from queued evidence")
        evidence = {"success": True, "persistentAckAndDeduplication": True, "agentProposal": True, "network": "none"}
        if extended:
            tools = json.loads(command("exec", name, *agent, "tools"))
            require({row["id"] for row in tools} == {"fleet.get_task_status", "fleet.list_tasks", "fleet.query_evidence"}
                    and all(row["effect"] == "read" for row in tools), "Agent tool inventory differs")
            page = json.loads(command("exec", name, *agent, address, "list", "0", "1"))
            require(page["items"][0]["taskId"] == "agent-smoke" and page["nextBeforeEpoch"] == queued["epoch"],
                    "Fleet container task pagination failed")
            flags = ("--run-id", "docker-proof", "--session", "docker-session")
            first = json.loads(command("exec", name, *agent, *flags, address, "propose", "agent-smoke"))
            again = json.loads(command("exec", name, *agent, *flags, address, "propose", "agent-smoke"))
            require(first == again and first["state"] == "success", "Fleet container checkpoint replay differs")
            evidence.update(readOnlyTools=len(tools), taskPagination=True, checkpointReplay=True)
        return evidence


def verify_workflow(image, release, libraries, postgres, work):
    with disposable_database(postgres, release, work) as (connection, _):
        subprocess.run([postgres / "bin/psql", connection, "-X", "-v", "ON_ERROR_STOP=1",
                        "-f", release / WORKFLOW_SCHEMA], check=True, stdout=subprocess.DEVNULL)
        parameters = dict(item.split("=", 1) for item in shlex.split(connection))
        socket = parameters.pop("host")
        dsn = "host=/pg " + " ".join(f"{name}={value}" for name, value in parameters.items())
        env = dict(os.environ, NEBULA_WORKFLOW_DSN=dsn)
        identity = f"{os.getuid()}:{os.getgid()}"
        args = ["--network", "none", "--user", identity, "--volume", f"{socket}:/pg:ro,Z",
                "--env", "NEBULA_WORKFLOW_DSN"]

        def run(*arguments):
            with container(image, libraries, args, env=env,
                           command_args=("/app/nebula_workflow_worker", *arguments)) as name:
                require(command("wait", name, timeout=20) == "0", "Workflow container exited with error")
                return command("logs", name)

        trace = run("submit", "docker-fork-join", "read")
        logs = run("work", "docker-fork-join", "read", "docker-worker", "1000")
        require(re.fullmatch(r"[a-f0-9]{32}", trace) and f"CLAIM D 1 {trace}" in logs
                and logs.endswith("RESULT success"), "Workflow container join failed")
        return {"passed": True, "forkJoin": True, "workers": 1, "socketMounted": True}


def export_images(destination, images):
    with destination.open("xb") as raw, gzip.GzipFile(filename="", mode="wb", mtime=0, fileobj=raw) as archive:
        with subprocess.Popen([ENGINE, "image", "save", *images], stdout=subprocess.PIPE) as process:
            shutil.copyfileobj(process.stdout, archive)
            if process.wait() != 0:
                raise RuntimeError("Docker image save failed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path, help="Verified Linux ZIP including the Fleet binaries")
    parser.add_argument("--postgres-prefix", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tag", default="function3.6")
    parser.add_argument("--discard-archive", action="store_true", help="Test export and reload in the temporary directory")
    args = parser.parse_args()
    require(re.fullmatch(r"[a-z0-9][a-z0-9._-]{0,48}", args.tag), "Choose a plain Docker tag")
    images = {name: f"nebula-{name}:{args.tag}" for name in ("web", "fleet")}
    require(os.getuid() != 0, "Use a normal Linux account with access to Docker and a disposable PostgreSQL")
    args.output.mkdir(parents=True, exist_ok=False)
    with tempfile.TemporaryDirectory(prefix="nebula-offline-") as directory:
        work = Path(directory)
        release = work / "release"
        context = work / "context"
        libraries = {name: work / f"runtime-{name}" for name in images}
        manifest = extract_payload(args.archive.resolve(), release)
        require(FLEET_PROGRAMS.keys() <= manifest["files"].keys(), "Linux release must include the tested Fleet programs")
        library_report = {name: prepare(release, path, name) for name, path in libraries.items()}
        stage_context(release, context)
        ids = {}
        for name, image in images.items():
            command("build", "--network=none", "--pull=false", "--target", name, "--tag", image,
                    "--build-arg", f"NEBULA_REVISION={manifest['revision']}", str(context), timeout=200)
            ids[name] = command("image", "inspect", "--format", "{{.Id}}", image)
        web = verify_web(images["web"], release, libraries["web"], args.postgres_prefix.resolve(), work, manifest)
        flags = json.loads((release / FLEET_EVIDENCE).read_text(encoding="utf-8"))
        fleet = verify_fleet(images["fleet"], libraries["fleet"], extended=flags.get("ollama", False))
        workflow = verify_workflow(images["fleet"], release, libraries["fleet"], args.postgres_prefix.resolve(), work) \
            if (release / WORKFLOW_PROGRAM).is_file() else None
        output = work if args.discard_archive else args.output
        artifact = output / f"nebula-{args.tag}-docker-images-linux-amd64.tar.gz"
        export_images(artifact, images.values())
        command("image", "load", "--input", str(artifact.resolve()), timeout=180)
        for name, image in images.items():
            require(command("image", "inspect", "--format", "{{.Id}}", image) == ids[name], "Image export changed")
        report = {"revision": manifest["revision"], "platform": "linux/amd64", "buildNetwork": "none",
                  "externalDependenciesDownloaded": False, "imageTags": images, "imageIds": ids,
                  "loadedArchive": True, "web": web, "fleet": fleet, "workflow": workflow, "runtimeMount": library_report,
                  "dockerArchiveSha256": checksum(artifact), "runtimeZipSha256": checksum(args.archive)}
        (args.output / "delivery-verification.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
        print(json.dumps({"images": images, "release": manifest["revision"], "web": True, "fleet": True}))


if __name__ == "__main__":
    main()
