#!/usr/bin/env python3
"""Wings build identity: every package says which build it is, and a test build can be turned into a
release without compiling again.

  wings-stamp.py write FILE            write the stamp for the build in progress (tools/build.sh calls this)
  wings-stamp.py release DIST          turn the test package in DIST into a release package
  wings-stamp.py show DIST             print the stamp of the package in DIST

The stamp is assets/build-info.json inside the package:  {"build":"20261012-203105","channel":"test   "}
  * build   = UTC time of the build (YYYYMMDD-HHMMSS), so every build is different and they sort by age.
              WINGS_BUILD_ID overrides it (the build scripts set it once so a whole run agrees).
  * channel = "test   " or "release", padded to the same length, so the flip changes bytes in place and
              nothing in the file system image moves. WINGS_CHANNEL chooses it (default: test).
The app version users see is still param.json's contentVersion; the stamp only says which build of it this is.

`release` patches the app folder, rebuilds the zip, patches the .ffpkg in place (the old stamp text must be in
the image exactly once) and rewrites SHA256SUMS.txt. tools/verify-package.sh then extracts the image and checks
every file against the folder, so a bad patch cannot pass. Run it through tools/mark-release-wsl.sh.
SPDX-License-Identifier: GPL-3.0-or-later
"""
import datetime
import hashlib
import os
import re
import sys
import zipfile
from pathlib import Path

BUILD_RE = re.compile(r"\d{8}-\d{6}")
CHANNELS = {"test": "test   ", "release": "release"}
STAMP_RE = re.compile(r'\{"build":"(\d{8}-\d{6})","channel":"(test   |release)"\}\n')
STAMP_NAME = "assets/build-info.json"


def stamp_text(build, channel):
    if not BUILD_RE.fullmatch(build):
        raise SystemExit("build id must look like 20261012-203105, got %r" % build)
    if channel not in CHANNELS:
        raise SystemExit("channel must be test or release, got %r" % channel)
    return '{"build":"%s","channel":"%s"}\n' % (build, CHANNELS[channel])


def new_build_id():
    return os.environ.get("WINGS_BUILD_ID") or datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M%S")


def parse(text):
    m = STAMP_RE.fullmatch(text)
    if not m:
        raise SystemExit("not a Wings build stamp: %r" % text[:80])
    return m.group(1), m.group(2).strip()


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def find_package(dist):
    """(title id, folder, zip, ffpkg) of the one package in DIST."""
    folders = sorted(p for p in dist.iterdir() if p.is_dir() and re.fullmatch(r"PPSA\d{5}", p.name))
    if len(folders) != 1:
        raise SystemExit("%s must hold exactly one PPSAxxxxx folder, found %d" % (dist, len(folders)))
    title = folders[0].name
    for name in (title + ".zip", title + ".ffpkg"):
        if not (dist / name).is_file():
            raise SystemExit("missing %s in %s" % (name, dist))
    return title, folders[0], dist / (title + ".zip"), dist / (title + ".ffpkg")


def cmd_write(target):
    text = stamp_text(new_build_id(), os.environ.get("WINGS_CHANNEL", "test"))
    path = Path(target)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="ascii", newline="\n")
    print("build stamp: %s" % text.strip())


def patch_image(image, old, new):
    """Replace OLD by NEW (same length) in the file IMAGE; OLD must occur exactly once."""
    if len(old) != len(new):
        raise SystemExit("the stamp length changed; refusing to patch")
    data = image.read_bytes()
    count = data.count(old)
    if count != 1:
        raise SystemExit("the old stamp occurs %d times in %s (expected 1); nothing was changed" % (count, image.name))
    at = data.index(old)
    temporary = image.with_name(image.name + ".patching")
    temporary.write_bytes(data[:at] + new + data[at + len(old):])
    os.replace(temporary, image)


def cmd_release(dist):
    dist = Path(dist)
    title, folder, archive, image = find_package(dist)
    stamp = folder / STAMP_NAME
    old_text = stamp.read_text(encoding="ascii")
    build, channel = parse(old_text)
    if channel == "release":
        print("%s build %s is already a release package; nothing to do" % (title, build))
    else:
        new_text = stamp_text(build, "release")
        patch_image(image, old_text.encode("ascii"), new_text.encode("ascii"))
        stamp.write_text(new_text, encoding="ascii", newline="\n")
        archive.unlink()
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
            # the same layout `python3 -m zipfile -c` makes in tools/build.sh: the folder, then what is in it
            z.write(folder, title + "/")
            for path in sorted(folder.rglob("*")):
                name = "%s/%s" % (title, path.relative_to(folder).as_posix())
                z.write(path, name + ("/" if path.is_dir() else ""))
        print("%s build %s: test -> release" % (title, build))
    (dist / "SHA256SUMS.txt").write_text(
        "%s  %s\n%s  %s\n" % (digest(image), image.name, digest(archive), archive.name), encoding="ascii", newline="\n")


def cmd_show(dist):
    title, folder, _archive, _image = find_package(Path(dist))
    build, channel = parse((folder / STAMP_NAME).read_text(encoding="ascii"))
    print("%s build %s %s" % (title, build, channel))


def selftest():
    import tempfile
    t = stamp_text("20261012-203105", "test")
    assert t == '{"build":"20261012-203105","channel":"test   "}\n' and parse(t) == ("20261012-203105", "test")
    r = stamp_text("20261012-203105", "release")
    assert len(r) == len(t) and parse(r) == ("20261012-203105", "release")
    with tempfile.TemporaryDirectory() as tmp:
        image = Path(tmp) / "x.ffpkg"
        image.write_bytes(b"A" * 5000 + t.encode() + b"B" * 5000)
        patch_image(image, t.encode(), r.encode())
        assert image.read_bytes() == b"A" * 5000 + r.encode() + b"B" * 5000
        image.write_bytes(t.encode() + b"." + t.encode())  # twice: refuse
        try:
            patch_image(image, t.encode(), r.encode())
        except SystemExit:
            pass
        else:
            raise AssertionError("a doubled stamp must be refused")
        image.write_bytes(b"no stamp here")  # none: refuse
        try:
            patch_image(image, t.encode(), r.encode())
        except SystemExit:
            pass
        else:
            raise AssertionError("a missing stamp must be refused")
        # a whole fake package through `release`
        dist = Path(tmp) / "dist"
        (dist / "PPSA99178" / "assets").mkdir(parents=True)
        (dist / "PPSA99178" / "assets" / "build-info.json").write_text(t, newline="\n")
        (dist / "PPSA99178.ffpkg").write_bytes(b"\0" * 100 + t.encode() + b"\0" * 100)
        (dist / "PPSA99178.zip").write_bytes(b"old")
        cmd_release(dist)
        assert parse((dist / "PPSA99178" / STAMP_NAME).read_text()) == ("20261012-203105", "release")
        assert r.encode() in (dist / "PPSA99178.ffpkg").read_bytes()
        with zipfile.ZipFile(dist / "PPSA99178.zip") as z:
            assert z.read("PPSA99178/assets/build-info.json").decode() == r
        sums = (dist / "SHA256SUMS.txt").read_text().splitlines()
        assert len(sums) == 2 and sums[0].endswith("  PPSA99178.ffpkg") and sums[1].endswith("  PPSA99178.zip")
        cmd_release(dist)  # again: no change, still fine
    print("wings-stamp selftest: ok")


def main(argv):
    if len(argv) == 2 and argv[1] == "selftest":
        return selftest()
    if len(argv) != 3 or argv[1] not in ("write", "release", "show"):
        raise SystemExit(__doc__)
    {"write": cmd_write, "release": cmd_release, "show": cmd_show}[argv[1]](argv[2])


if __name__ == "__main__":
    main(sys.argv)
