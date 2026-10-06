#!/usr/bin/env bash

# 安装 Phase 11 的 CPU Adapter 学习工具链。
# 该脚本会访问 PyPI 和 PyTorch 官方 CPU wheel 索引；必须显式传入 --install，
# 防止只想查看脚本时意外下载依赖。模型和训练数据不属于该脚本的职责。
set -euo pipefail

if [[ "${1:-}" != "--install" ]]; then
    echo "Usage: $0 --install" >&2
    echo "This command downloads the approved CPU-only Adapter toolchain." >&2
    exit 2
fi

phase11_home="${PHASE11_HOME:-/home/pikachu}"
python_root="${PHASE11_PYTHON_ROOT:-${phase11_home}/phase11-deps/python/3.12.15}"
venv_root="${PHASE11_LORA_VENV:-${phase11_home}/phase11-deps/python-envs/lora}"
report_root="${PHASE11_LORA_REPORTS:-${phase11_home}/phase11-deps/reports/lora}"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
constraints="${script_dir}/lora-constraints.txt"
python_bin="${python_root}/bin/python3.12"
cpu_index="https://download.pytorch.org/whl/cpu"

if [[ ! -x "${python_bin}" ]]; then
    echo "Approved CPython runtime is missing: ${python_bin}" >&2
    exit 1
fi
if [[ ! -f "${constraints}" ]]; then
    echo "Version constraints are missing: ${constraints}" >&2
    exit 1
fi

mkdir -p "$(dirname "${venv_root}")" "${report_root}"
if [[ ! -x "${venv_root}/bin/python" ]]; then
    "${python_bin}" -m venv "${venv_root}"
fi

venv_python="${venv_root}/bin/python"

# 先固定 CPU Torch。普通 PyPI 上的 Linux x86-64 Torch wheel 会声明大量 CUDA
# 依赖；官方 CPU 索引可以从源头避免把它们拉入虚拟机。
"${venv_python}" -m pip install \
    --only-binary=:all: \
    --index-url "${cpu_index}" \
    --constraint "${constraints}" \
    --report "${report_root}/torch-install.json" \
    "torch==2.10.0+cpu"

# lora extra 提供 Adapter 转换所需的 PEFT/Accelerate/SciPy；cpu extra 提供
# CPU ONNX Runtime。这里不用 finetune extra，避免引入 GPU 训练依赖。
"${venv_python}" -m pip install \
    --only-binary=:all: \
    --extra-index-url "${cpu_index}" \
    --constraint "${constraints}" \
    --report "${report_root}/adapter-install.json" \
    "olive-ai[lora,cpu]==0.13.0" \
    "transformers==5.3.0" \
    "peft==0.21.2" \
    "accelerate==1.15.0" \
    "scipy==1.18.1" \
    "onnxruntime-genai==0.17.0" \
    "requests==2.34.2"

"${venv_python}" -m pip check
"${venv_python}" -m pip freeze --all > "${report_root}/requirements.lock.txt"

# 最后再审计一次环境，防止依赖解析结果偏离“CPU-only”的审批范围。
"${venv_python}" - <<'PY'
import importlib.metadata as metadata
import sys

names = {dist.metadata["Name"].lower() for dist in metadata.distributions()}
forbidden = sorted(
    name for name in names
    if name == "triton" or name == "bitsandbytes" or name.startswith("nvidia-")
)
if forbidden:
    print("Unexpected GPU packages: " + ", ".join(forbidden), file=sys.stderr)
    raise SystemExit(1)
PY

"${venv_python}" - <<'PY'
import importlib.metadata as metadata
import accelerate
import olive
import onnxruntime_genai
import peft
import requests
import scipy
import torch
import transformers

assert torch.__version__ == "2.10.0+cpu", torch.__version__
assert torch.version.cuda is None, torch.version.cuda
assert not torch.cuda.is_available()

for package in (
    "olive-ai",
    "torch",
    "transformers",
    "peft",
    "accelerate",
    "scipy",
    "onnxruntime-genai",
    "requests",
):
    print(f"{package}={metadata.version(package)}")
print("cpu_only=true")
PY

"${venv_root}/bin/olive" --help >/dev/null
"${venv_root}/bin/olive" generate-adapter --help >/dev/null
"${venv_root}/bin/olive" convert-adapters --help >/dev/null

echo "Phase 11 CPU Adapter toolchain is ready."
echo "venv=${venv_root}"
echo "lock=${report_root}/requirements.lock.txt"
