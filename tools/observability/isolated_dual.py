"""Compatibility entry only; implementation: tools.observability.laboratory.isolated_dual."""
import sys as _sys
from importlib import import_module as _import_module
_implementation = _import_module('tools.observability.laboratory.isolated_dual')
if __name__ == "__main__":
    _sys.exit(_implementation.main())
else:
    _sys.modules[__name__] = _implementation
