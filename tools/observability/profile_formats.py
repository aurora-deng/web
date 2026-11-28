"""Compatibility entry only; implementation: tools.observability.diagnostics.profile_formats."""
import sys as _sys
from importlib import import_module as _import_module
_implementation = _import_module('tools.observability.diagnostics.profile_formats')
_sys.modules[__name__] = _implementation
