"""Check the Observer v1 state vocabulary against the C++ and browser runtimes."""
import ast
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]


def contents(path):
    return (ROOT / path).read_text(encoding="utf-8")


def main():
    spec = json.loads(contents("docs/contracts/dag-states.json"))
    graph = contents("backend/workflow/WorkflowGraph/WorkflowGraph.h")
    names = re.search(r"enum class State\s*\{([^}]+)\}", graph)[1]
    cpp = [value.strip() for value in names.split(",")]
    wire = re.search(r"string_view names\[\]\{([^}]+)\}", contents("backend/workflow/DagExecutor/DagExecutor.cpp"))[1]
    assert cpp == re.findall(r'"([a-z_]+)"', wire)
    assert set(cpp) == set(spec["run"]) | set(spec["node"]) | set(spec["internal"])
    browser = contents("frontend/phase11/shared/AgentRuntime/DagExecutor.js")
    observer = ast.parse(contents("tools/observability/observer/events.py"))
    words = next(ast.literal_eval(node.value) for node in observer.body
                 if isinstance(node, ast.Assign) and any(isinstance(x, ast.Name) and x.id == "WORDS" for x in node.targets))
    for kind in ("run", "node"):
        declaration = re.search(rf"\b{kind}: Object\.freeze\(\[([^\]]+)\]\)", browser)[1]
        assert re.findall(r"'([a-z_]+)'", declaration) == spec[kind]
        assert words["dag." + kind]["state"].split() == spec[kind]
    print("PASS C++ / browser / Observer DAG state contract v1")


if __name__ == "__main__":
    main()
