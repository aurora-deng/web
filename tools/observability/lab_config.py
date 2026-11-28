"""Compatibility entry only; implementation: tools.observability.laboratory.lab_config."""
import sys as _sys
from importlib import import_module as _import_module
_implementation = _import_module('tools.observability.laboratory.lab_config')
_sys.modules[__name__] = _implementation
