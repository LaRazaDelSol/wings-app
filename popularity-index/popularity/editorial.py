"""Editorial side files published next to popularity.v1.json: notes.v1.json and curated.v1.json.

Pure functions, stdlib only. Error messages describe the problem (field, index, limit) and never contain the input
text, because CI logs are public."""
import base64
import binascii
import json
import os
import re
import zlib

ID_RE = re.compile(r"^(?:PPSA|CUSA)\d{5}$")
URL_CREDENTIALS_RE = re.compile(r"[A-Za-z][A-Za-z0-9+.\-]*://[^/\s?#]*@")
MAX_NOTES = 600
MAX_CURATED = 24
MAX_FILE_BYTES = 64000
MAX_DECODED_BYTES = 1000000


class EditorialError(ValueError):
    pass


def decode(raw):
    """Secret text -> parsed JSON. Accepts plain JSON text or base64(gzip(JSON)). Returns None when unset/blank."""
    if raw is None or not raw.strip():
        return None
    text = raw.strip()
    if text[0] in "{[":
        data = text.encode("utf-8")
    else:
        try:
            packed = base64.b64decode("".join(text.split()), validate=True)
            data = zlib.decompressobj(16 + zlib.MAX_WBITS).decompress(packed, MAX_DECODED_BYTES + 1)
        except (binascii.Error, zlib.error, ValueError, EOFError):
            raise EditorialError("value is neither JSON text nor base64+gzip") from None
        if len(data) > MAX_DECODED_BYTES:
            raise EditorialError("unpacked value is too large")
    try:
        return json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        raise EditorialError("value is not valid UTF-8 JSON") from None


def _has_control(s):
    return any(ord(c) < 32 or 127 <= ord(c) < 160 for c in s)


def _check_id(key, where):
    if not isinstance(key, str) or not ID_RE.match(key):
        raise EditorialError("%s: id does not match PPSAnnnnn or CUSAnnnnn" % where)


def _serialise(doc, name):
    text = json.dumps(doc, ensure_ascii=False, separators=(",", ":"))
    if len(text.encode("utf-8")) > MAX_FILE_BYTES:
        raise EditorialError("%s: output exceeds %d bytes" % (name, MAX_FILE_BYTES))
    return text + "\n"


def validate_notes(data):
    """Returns the serialised notes.v1.json text. Accepts {"notes":{...}} (optional schemaVersion 1) or a bare map."""
    if not isinstance(data, dict):
        raise EditorialError("notes: top level must be an object")
    if "notes" in data:
        if data.get("schemaVersion", 1) != 1:
            raise EditorialError("notes: unsupported schemaVersion")
        if set(data) - {"schemaVersion", "notes"}:
            raise EditorialError("notes: unexpected top-level field")
        notes = data["notes"]
    else:
        notes = data
    if not isinstance(notes, dict):
        raise EditorialError("notes: 'notes' must be an object")
    if len(notes) > MAX_NOTES:
        raise EditorialError("notes: %d entries, limit is %d" % (len(notes), MAX_NOTES))
    for n, (key, val) in enumerate(notes.items(), 1):
        where = "notes entry #%d" % n
        _check_id(key, where)
        if not isinstance(val, str):
            raise EditorialError("%s: text must be a string" % where)
        if len(val) < 1:
            raise EditorialError("%s: text must not be empty" % where)
        if _has_control(val):
            raise EditorialError("%s: text contains control characters" % where)
        if URL_CREDENTIALS_RE.search(val):
            raise EditorialError("%s: text contains a URL with credentials" % where)
    return _serialise({"schemaVersion": 1, "notes": notes}, "notes")


def validate_curated(data):
    """Returns the serialised curated.v1.json text. Accepts {"items":[...]} (optional schemaVersion 1) or a bare list."""
    if isinstance(data, dict):
        if data.get("schemaVersion", 1) != 1:
            raise EditorialError("curated: unsupported schemaVersion")
        if set(data) - {"schemaVersion", "items"}:
            raise EditorialError("curated: unexpected top-level field")
        items = data.get("items")
    else:
        items = data
    if not isinstance(items, list):
        raise EditorialError("curated: 'items' must be an array")
    if len(items) > MAX_CURATED:
        raise EditorialError("curated: %d ids, limit is %d" % (len(items), MAX_CURATED))
    seen = set()
    for n, item in enumerate(items, 1):
        _check_id(item, "curated item #%d" % n)
        if item in seen:
            raise EditorialError("curated item #%d: duplicate id" % n)
        seen.add(item)
    return _serialise({"schemaVersion": 1, "items": items}, "curated")


def build(notes_raw=None, curated_raw=None):
    """-> {filename: text} for each non-empty input. Raises EditorialError (never echoing content) on invalid input.
    Nothing is written here, so one invalid input means no file at all."""
    out = {}
    for label, raw, check, fname in (("notes", notes_raw, validate_notes, "notes.v1.json"),
                                     ("curated", curated_raw, validate_curated, "curated.v1.json")):
        try:
            data = decode(raw)
            if data is None:
                continue
            out[fname] = check(data)
        except EditorialError as e:
            msg = str(e)
            raise EditorialError(msg if msg.startswith(label) else "%s: %s" % (label, msg)) from None
    return out


def write(files, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    for name, text in files.items():
        with open(os.path.join(out_dir, name), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    return sorted(files)
