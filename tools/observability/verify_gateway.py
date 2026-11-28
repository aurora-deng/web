"""Compatibility entry only; implementation: tools.observability.verification.verify_gateway."""
import sys as _sys
from importlib import import_module as _import_module
_implementation = _import_module('tools.observability.verification.verify_gateway')
if __name__ == "__main__":
    _implementation.main()
else:
    _sys.modules[__name__] = _implementation
