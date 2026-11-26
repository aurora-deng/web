"""Fail-closed, bounded subset of JSON Schema, implemented with the stdlib.

Supported: object, array, string, integer, number, boolean; enum and const.
Unknown keywords are rejected instead of silently weakening a contract.
"""
from __future__ import annotations

import math
import re


class SchemaError(ValueError):
    pass


_COMMON = {"type", "enum", "const"}
_FIELDS = {
    "object": {"properties", "required", "additionalProperties"},
    "array": {"items", "minItems", "maxItems", "uniqueItems"},
    "string": {"minLength", "maxLength", "pattern"},
    "integer": {"minimum", "maximum"},
    "number": {"minimum", "maximum"},
    "boolean": set(),
}
_FORBIDDEN = {"begin", "commit", "rollback", "transaction", "sql", "shell", "url", "path"}


def check_schema(schema: dict, depth: int = 0) -> None:
    if depth > 8 or not isinstance(schema, dict):
        raise SchemaError("invalid_schema")
    kind = schema.get("type")
    if kind not in _FIELDS or set(schema) - _COMMON - _FIELDS[kind]:
        raise SchemaError("unsupported_schema")
    if "enum" in schema and (not isinstance(schema["enum"], list) or not schema["enum"]):
        raise SchemaError("invalid_enum")
    if kind == "object":
        props = schema.get("properties")
        required = schema.get("required")
        if (schema.get("additionalProperties") is not False or not isinstance(props, dict)
                or not isinstance(required, list) or len(required) != len(set(required))
                or not set(required) <= set(props) or len(props) > 32):
            raise SchemaError("object_must_be_strict")
        for key, child in props.items():
            if not isinstance(key, str) or key.lower() in _FORBIDDEN:
                raise SchemaError("internal_port_field")
            check_schema(child, depth + 1)
    elif kind == "array":
        maximum = schema.get("maxItems")
        minimum = schema.get("minItems", 0)
        if (type(maximum) is not int or not 0 <= maximum <= 256
                or type(minimum) is not int or not 0 <= minimum <= maximum):
            raise SchemaError("array_must_be_bounded")
        if "uniqueItems" in schema and type(schema["uniqueItems"]) is not bool:
            raise SchemaError("invalid_unique_items")
        check_schema(schema.get("items"), depth + 1)
    elif kind == "string":
        maximum = schema.get("maxLength")
        minimum = schema.get("minLength", 0)
        if (type(maximum) is not int or not 0 <= maximum <= 8192
                or type(minimum) is not int or not 0 <= minimum <= maximum):
            raise SchemaError("string_must_be_bounded")
        if "pattern" in schema:
            # Patterns are trusted application contracts, never model input.
            try:
                re.compile(schema["pattern"])
            except (re.error, TypeError) as error:
                raise SchemaError("invalid_pattern") from error
    elif kind in {"integer", "number"}:
        low, high = schema.get("minimum"), schema.get("maximum")
        if (type(low) not in {int, float} or type(high) not in {int, float}
                or not math.isfinite(low) or not math.isfinite(high) or low > high):
            raise SchemaError("number_must_be_bounded")
    for item in schema.get("enum", []):
        validate({k: v for k, v in schema.items() if k != "enum"}, item)
    if "const" in schema:
        validate({k: v for k, v in schema.items() if k != "const"}, schema["const"])


def validate(schema: dict, value, depth: int = 0) -> None:
    if depth > 8:
        raise SchemaError("too_deep")
    kind = schema["type"]
    types = {"object": dict, "array": list, "string": str, "integer": int, "boolean": bool}
    if kind == "number":
        valid = type(value) in {int, float} and math.isfinite(value)
    else:
        valid = type(value) is types[kind]
    if not valid:
        raise SchemaError("invalid_type")
    if "enum" in schema and value not in schema["enum"]:
        raise SchemaError("invalid_enum")
    if "const" in schema and value != schema["const"]:
        raise SchemaError("invalid_const")
    if kind == "object":
        props = schema["properties"]
        if not set(value) <= set(props) or not set(schema["required"]) <= set(value):
            raise SchemaError("unknown_or_missing_field")
        for key, child in value.items():
            validate(props[key], child, depth + 1)
    elif kind == "array":
        if not schema.get("minItems", 0) <= len(value) <= schema["maxItems"]:
            raise SchemaError("invalid_array_length")
        if schema.get("uniqueItems") and any(x in value[:i] for i, x in enumerate(value)):
            raise SchemaError("duplicate_items")
        for item in value:
            validate(schema["items"], item, depth + 1)
    elif kind == "string":
        if (not schema.get("minLength", 0) <= len(value) <= schema["maxLength"]
                or any(ord(c) < 32 for c in value)
                or ("pattern" in schema and re.fullmatch(schema["pattern"], value) is None)):
            raise SchemaError("invalid_string")
    elif kind in {"integer", "number"} and not schema["minimum"] <= value <= schema["maximum"]:
        raise SchemaError("out_of_range")
