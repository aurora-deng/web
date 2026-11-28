"""Compatibility entry only; implementation: tools.observability.gateways.gateway."""
import sys as _sys
from importlib import import_module as _import_module
_implementation = _import_module('tools.observability.gateways.gateway')
_sys.modules[__name__] = _implementation
