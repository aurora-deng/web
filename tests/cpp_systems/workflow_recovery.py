"""Crash between the durable intent and durable result, then reopen the database."""

from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="nebula-dag-", dir=Path.cwd()) as temp:
        root = Path(temp)

        def call(effect, phase, expected=0):
            # Keep native argv ASCII on Windows. Python sets the Unicode working
            # directory; SQLite and the effect file still live in that same directory.
            result = subprocess.run([executable, f"{effect}.sqlite", effect, phase], cwd=root,
                                    capture_output=True, text=True, timeout=8, check=False)
            assert result.returncode == expected, (effect, phase, result.returncode, result.stderr)
            return result.stdout.strip()

        call("external", "crash", 86)
        trace = call("external", "blocked")
        assert len(trace) == 32 and call("external", "reconcile") == call("external", "resume") == trace
        call("external", "conflict")
        call("idempotent", "crash", 86)
        assert call("idempotent", "resume") == call("idempotent", "resume")
        for effect in ("external", "idempotent"):
            assert (root / f"{effect}.sqlite.effect").read_text(encoding="ascii").splitlines() == ["proof:action"]
    print("Workflow recovery passed: external uncertainty, verified settlement, stable retry key, one effect each")


if __name__ == "__main__":
    main()
