"""Migration-test evidence only: allow Git's uniform LF/CRLF conversion.

Do not use this helper for model, dependency, runtime or security checksums.
Historical digests remain unchanged; all non-newline bytes must still match.
"""
import hashlib
import re


def text_variants(data):
    yield data
    if b'\0' in data:
        return
    try:
        data.decode('utf-8')
    except UnicodeDecodeError:
        return
    lf = data.replace(b'\r\n', b'\n')
    if b'\r' in lf or (b'\r\n' in data and b'\n' in data.replace(b'\r\n', b'')):
        return
    if lf != data:
        yield lf
    crlf = lf.replace(b'\n', b'\r\n')
    if crlf != data:
        yield crlf


def matches_text_digest(data, expected, digest=None):
    if not isinstance(expected, str) or not re.fullmatch(r'[0-9a-f]{64}', expected):
        return False
    digest = digest or (lambda value: hashlib.sha256(value).hexdigest())
    return any(digest(candidate) == expected for candidate in text_variants(data))