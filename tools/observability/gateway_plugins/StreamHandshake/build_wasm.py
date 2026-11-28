"""Build the Envoy telemetry plugin using explicitly provisioned Emscripten and SDK."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--emxx", type=Path, required=True)
    cli.add_argument("--sdk", type=Path, required=True)
    cli.add_argument("--output", type=Path, required=True)
    args = cli.parse_args()
    source = Path(__file__).resolve().parent
    sdk = args.sdk.resolve()
    targets = [sdk / name for name in ("proxy_wasm_intrinsics.h", "proxy_wasm_intrinsics.js", "proxy_wasm_intrinsics.cc")]
    if not args.emxx.is_file() or not all(path.is_file() for path in targets):
        cli.error("Provide an installed em++ compiler and the Proxy-Wasm C++ SDK checkout")
    output = args.output.resolve()
    if output.suffix != ".wasm" or output.exists():
        cli.error("Choose a new .wasm artifact path")
    output.parent.mkdir(parents=True, exist_ok=True)
    command = [str(args.emxx.resolve()), "--std=c++17", "-O2", "-fno-exceptions", "-fno-rtti", "--no-entry",
               "--js-library", str(targets[1]), "-sSTANDALONE_WASM", "-sEXPORTED_FUNCTIONS=_malloc",
               "-sALLOW_MEMORY_GROWTH=1", "-sMAXIMUM_MEMORY=16777216", "-sINITIAL_HEAP=64KB",
               "-I" + str(sdk), str(targets[2]), str(source / "StreamHandshake.cpp"),
               str(source / "StreamHandshakeWasm.cpp"), "-o", str(output)]
    subprocess.run(command, check=True)
    from tools.observability.gateways.gateway import verified_wasm
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    data = verified_wasm(output, digest)
    print(json.dumps({"wasm": str(output), "sha256": digest, "bytes": len(data)}))


if __name__ == "__main__":
    main()
