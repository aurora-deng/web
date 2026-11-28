"""Load checked-in Agent contracts and recipes, never an external plugin path."""
import copy
from functools import lru_cache, partial
from importlib import import_module
import json
from pathlib import Path
import re

AGENT_ROOT = Path(__file__).resolve().parents[1]
CATALOG_PATH = AGENT_ROOT / "catalog.json"
_NAME = re.compile(r"[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)+")


def _entries(kind):
    if kind not in {"tools", "skills"}:
        raise ValueError("unknown_catalog_kind")
    manifest = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
    if (not isinstance(manifest, dict) or set(manifest) != {"version", "tools", "skills"}
            or type(manifest["version"]) is not int or manifest["version"] != 1):
        raise ValueError("invalid_catalog_manifest")
    names = manifest[kind]
    if (not isinstance(names, list) or not names or len(names) > 256
            or any(not isinstance(name, str) or _NAME.fullmatch(name) is None for name in names)
            or len(set(names)) != len(names)):
        raise ValueError("invalid_catalog_names")
    root = (AGENT_ROOT / kind).resolve(strict=True)
    if not root.is_relative_to(AGENT_ROOT.resolve(strict=True)):
        raise ValueError("catalog_path_escape")
    expected = set()
    entries = []
    for name in names:
        directory = root.joinpath(*name.split(".")).resolve(strict=True)
        if not directory.is_relative_to(root):
            raise ValueError("catalog_path_escape")
        required = ["spec.py", "impl.py", "README.md"]
        for filename in required:
            path = directory / filename
            if not path.is_file() or not path.resolve(strict=True).is_relative_to(directory):
                raise ValueError("missing_or_unsafe_catalog_file: " + name + "/" + filename)
        if not (directory / "README.md").read_text(encoding="utf-8").strip():
            raise ValueError("empty_catalog_documentation: " + name)
        expected.add(directory / "spec.py")
        entries.append((name, "backend.agent." + kind + "." + name))
    if {p.resolve() for p in root.rglob("spec.py")} != expected:
        raise ValueError("unlisted_or_duplicate_catalog_spec")
    return entries


@lru_cache(maxsize=2)
def _checked_specs(kind):
    from .registry import ToolSpec, SkillSpec
    expected_type = ToolSpec if kind == "tools" else SkillSpec
    result = []
    for name, module in _entries(kind):
        spec = getattr(import_module(module + ".spec"), "SPEC", None)
        if not isinstance(spec, expected_type) or spec.name != name:
            raise ValueError("catalog_identity_mismatch: " + name)
        result.append(spec)
    return tuple(result)


def load_specs(kind):
    # Contracts are verified once per process; callers never receive the cached mutable schemas.
    return copy.deepcopy(_checked_specs(kind))


@lru_cache(maxsize=1)
def _checked_recipes():
    result = {}
    for name, module in _entries("skills"):
        recipe = getattr(import_module(module + ".impl"), "run", None)
        if not callable(recipe):
            raise ValueError("missing_skill_recipe: " + name)
        result[name] = recipe
    return tuple(result.items())


def load_recipes(runtime):
    result = {name: partial(recipe, runtime) for name, recipe in _checked_recipes()}
    if set(result) != set(runtime.registry.skills):
        raise ValueError("every advertised skill must have exactly one executable recipe")
    return result


@lru_cache(maxsize=2)
def _checked_tool_handlers(adapter):
    if adapter not in {"workspace", "nebula"}:
        raise ValueError("unknown_tool_adapter")
    from .registry import Effect
    specs = {spec.name: spec for spec in _checked_specs("tools")}
    reads, writes = [], []
    for name, module in _entries("tools"):
        read = specs[name].effect == Effect.READ
        attribute = ("read_" if read else "write_") + adapter
        handler = getattr(import_module(module + ".impl"), attribute, None)
        if not callable(handler):
            raise ValueError("missing_tool_implementation: " + name + "/" + attribute)
        (reads if read else writes).append((name, handler))
    return tuple(reads), tuple(writes)


def load_tool_handlers(adapter):
    """Only checked-in catalog IDs select code; caller parameters never select modules."""
    reads, writes = _checked_tool_handlers(adapter)
    return dict(reads), dict(writes)
