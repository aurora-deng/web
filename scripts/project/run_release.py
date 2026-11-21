"""Private, process-scoped launcher for an extracted, preflight-verified release."""
import json
import os
from pathlib import Path
import sys
from release_preflight import check


def main():
    root=Path(__file__).resolve().parent
    report=check(root)
    if not report['passed']:
        print(json.dumps(report,indent=2),file=sys.stderr)
        return 1
    # Runtime capabilities are enabled only by explicit operator configuration.
    # No default model, cloud credential, database or global environment changes.
    os.chdir(root)
    os.execve(str(root/'webserver'),[str(root/'webserver'),*sys.argv[1:]],dict(os.environ))


if __name__=='__main__':raise SystemExit(main())
