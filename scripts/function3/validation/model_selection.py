"""Offline local model selection shared by isolated test runners. No downloads or DB access."""
from dataclasses import dataclass
from pathlib import Path
import re

DEFAULT_MODEL_ROOT = Path("/home/pikachu/phase11-models")
DEFAULT_MODEL = "phi3-mini-4k-instruct-cpu-int4"


def add_model_arguments(parser):
    parser.add_argument("--model-root", type=Path, default=DEFAULT_MODEL_ROOT)
    parser.add_argument("--model-dir", type=Path, help="Installed ONNX directory inside --model-root; defaults to phi3")
    parser.add_argument("--adapter", default="", help="Existing adapter artifact relative to the selected model, never an external path")
    parser.add_argument("--label", help="Short report label; not a claim about quality or model identity")


@dataclass(frozen=True)
class ModelSelection:
    root: Path
    directory: Path
    artifact: str
    adapter: str
    label: str

    def arguments(self):
        args = ["--model-root", str(self.root), "--model-dir", str(self.directory), "--label", self.label]
        if self.adapter: args += ["--adapter", self.adapter]
        return args

    def report(self):
        return {"modelArtifact": self.artifact, "adapterArtifact": self.adapter, "label": self.label}


def resolve_selection(args):
    root = args.model_root.resolve(strict=True)
    if not root.is_dir(): raise ValueError("model root is not a directory")
    selected = args.model_dir if args.model_dir is not None else root / DEFAULT_MODEL
    if not selected.is_absolute(): selected = root / selected
    directory = selected.resolve(strict=True)
    if (directory == root or not directory.is_relative_to(root) or not directory.is_dir()
            or not (directory / "genai_config.json").is_file()):
        raise ValueError("model must be an installed ONNX directory inside model root")
    if not (directory / "genai_config.json").resolve().is_relative_to(directory):
        raise ValueError("model configuration escapes its directory")
    artifact = directory.relative_to(root).as_posix()
    if any(re.fullmatch(r"[A-Za-z0-9_.-]+", part) is None for part in Path(artifact).parts):
        raise ValueError("unsupported model artifact name")
    adapter = args.adapter
    if adapter:
        relative = Path(adapter)
        if relative.is_absolute() or relative.anchor or ".." in relative.parts:
            raise ValueError("adapter must be relative to the selected model")
        path = (directory / relative).resolve(strict=True)
        if not path.is_relative_to(directory) or not path.is_file() or path.suffix != ".onnx_adapter":
            raise ValueError("adapter must be an existing .onnx_adapter inside the model directory")
        adapter = path.relative_to(directory).as_posix()
    label = args.label if args.label is not None else directory.name
    if re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}", label) is None:
        raise ValueError("label must be 1-64 safe filename characters")
    return ModelSelection(root, directory, artifact, adapter, label)
