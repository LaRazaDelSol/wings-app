#!/usr/bin/env bash
# Wings TV app - Verify the packaged bytes, not just the staging folder.
# Run in the app build image after `make ffpkg`. Adapted from Orbit Store's
# tools/verify-package.sh (GPL-3.0-or-later): the Orbit payload checks are gone;
# instead it checks the image against the staged folder and fails if anything
# Orbit-specific (payload ELF, loader port, backend port) slipped into the package.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
title_id=$(python3 -c 'import json;print(json.load(open("sce_sys/param.json"))["titleId"])')
# WINGS_DIST: the folder holding the package (default dist; tools/mark-release-wsl.sh points it at build-out).
dist=${WINGS_DIST:-dist}
image=$dist/$title_id.ffpkg
runner=$(bash tools/setup-packaging-dependencies.sh ffpkg)
mkdir -p build
temporary=$(mktemp -d "$root/build/verify-package.XXXXXX")
trap 'rm -rf -- "$temporary" "$temporary.log"' EXIT
"$runner" extract "$image" "$temporary" >"$temporary.log"
python3 - "$temporary" "$title_id" "$dist" <<'PY'
import hashlib, json, struct, sys
from pathlib import Path

extracted = Path(sys.argv[1])
title_id = sys.argv[2]
dist = Path(sys.argv[3])
staged = dist / title_id


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def tree(base):
    return {str(p.relative_to(base)): digest(p) for p in sorted(base.rglob('*')) if p.is_file()}


# The image carries exactly what was staged.
packed, folder = tree(extracted), tree(staged)
assert packed.keys() == folder.keys(), f'file lists differ: {sorted(set(packed) ^ set(folder))}'
for name in folder:
    assert packed[name] == folder[name], f'packaged file differs: {name}'

# Metadata and the executable.
param = json.loads((extracted / 'sce_sys/param.json').read_text())
assert param == json.loads(Path('sce_sys/param.json').read_text()), 'package metadata differs'
assert param['titleId'] == title_id and title_id in param['contentId']
assert param['localizedParameters']['en-US']['titleName'] == 'Wings'
eboot = (extracted / 'eboot.bin').read_bytes()
assert struct.unpack('<I', eboot[:4])[0] == 0x1D3D154F, 'eboot.bin is not a signed FSELF'
icon = (extracted / 'sce_sys/icon0.png').read_bytes()
assert icon[:8] == b'\x89PNG\r\n\x1a\n' and struct.unpack('>II', icon[16:24]) == (512, 512), 'icon0.png is not 512x512'
for name in ('inter-display-light', 'inter-regular', 'inter-medium', 'inter-semibold'):
    assert (extracted / 'assets/fonts' / f'{name}.huifont').is_file(), f'missing font {name}'
assert (extracted / 'assets/fonts/Inter-LICENSE.txt').is_file(), 'Inter licence must ship with the fonts'
assert (extracted / 'sce_module/libc.prx').is_file(), 'missing runtime libc.prx'
# The build stamp (tools/wings-stamp.py): which build this is, and whether it is a test or a release package.
import re as _stamp_re
_stamp = _stamp_re.fullmatch(r'\{"build":"(\d{8}-\d{6})","channel":"(test   |release)"\}\n',
                             (extracted / 'assets/build-info.json').read_text())
assert _stamp, 'assets/build-info.json is missing or is not a valid build stamp'

# Nothing of Orbit's backend may ride along.
assert not (extracted / 'orbit').exists(), 'an orbit/ payload directory is in the package'
assert not list(extracted.rglob('orbit_store.elf')), 'an Orbit payload ELF is in the package'
for needle in (b'34177', b'9021', b'orbit_store', b'Orbit-Store', b'OrbitStoreTV'):
    assert needle not in eboot, f'eboot.bin still mentions {needle!r}'
# The only /api/v1/ routes allowed are ShadowMountPlus' local API (scan after a download):
# an Orbit backend route would be anything else.
import re as _re
_routes = set(_re.findall(rb'/api/v1/[A-Za-z0-9_/-]*', eboot))
_allowed = {b'/api/v1/version', b'/api/v1/scan', b'/api/v1/games'}
assert _routes <= _allowed, f'eboot.bin mentions unexpected API routes {sorted(_routes - _allowed)!r}'
# The Wings update helper (app/helper, built by `make helper`): a separate payload ELF the app sends
# to the payload loader. It ports parts of Orbit's backend (credited in THIRD-PARTY-NOTICES.md), so
# it gets the same Orbit checks as eboot.bin, and it may only serve its own /wings-helper/v1/ routes.
helper_path = extracted / 'assets/helper/wings-helper.elf'
assert helper_path.is_file(), 'assets/helper/wings-helper.elf is missing: the in-app updater has no helper'
helper = helper_path.read_bytes()
assert helper[:4] == b'\x7fELF', 'wings-helper.elf is not an ELF file'
assert helper[4] == 2 and struct.unpack('<H', helper[18:20])[0] == 0x3E, 'wings-helper.elf is not an x86-64 ELF64'
assert len(helper) >= 64 * 1024, f'wings-helper.elf is only {len(helper)} bytes'
for needle in (b'34177', b'orbit_store', b'Orbit-Store', b'OrbitStoreTV'):
    assert needle not in helper, f'wings-helper.elf still mentions {needle!r}'
assert b'/api/v1/' not in helper, 'wings-helper.elf serves /api/v1/ routes (Orbit backend or ShadowMountPlus)'
# The helper keeps the prefix and the route names apart (prefix + "status"), so the bare prefix is allowed too.
_helper_allowed = {b'/wings-helper/v1/' + r for r in (b'', b'status', b'install', b'cancel', b'stop', b'tidy')}
_helper_routes = set(_re.findall(rb'/wings-helper/v1/[A-Za-z0-9_/-]*', helper))
assert _helper_routes and (b'/wings-helper/v1/status' in _helper_routes or b'status\x00' in helper), \
    'wings-helper.elf does not serve /wings-helper/v1/status'
assert _helper_routes <= _helper_allowed, \
    f'wings-helper.elf mentions unexpected routes {sorted(_helper_routes - _helper_allowed)!r}'
assert b'Wings-Helper/' in helper, 'wings-helper.elf does not identify itself as Wings-Helper/<version>'
_eboot_helper_routes = set(_re.findall(rb'/wings-helper/v1/[A-Za-z0-9_/-]*', eboot))
assert _eboot_helper_routes <= _helper_allowed, \
    f'eboot.bin mentions unexpected helper routes {sorted(_eboot_helper_routes - _helper_allowed)!r}'
assert b'wings-helper.elf' in eboot, 'eboot.bin does not reference the update helper'
# Cover art (wings/art): the shipped CA bundle, and the same imports as before.
# DT_NEEDED must not gain libScePosixForWebKit (getaddrinfo is shimmed away, a
# native title does not load that module) nor libSceNet (system DNS is an
# unverified experiment, built only on purpose with SCENET).
bundle = extracted / 'assets/ca-bundle.crt'
assert bundle.is_file(), 'assets/ca-bundle.crt is missing: HTTPS cover art would refuse every connection'
certificates = bundle.read_bytes().count(b'-----BEGIN CERTIFICATE-----')
assert certificates >= 100, f'CA bundle holds only {certificates} certificates'
for needle in (b'libScePosixForWebKit', b'libSceNet.'):
    assert needle not in eboot, f'eboot.bin imports {needle!r}'
assert b'[WINGS-ART]' in eboot, 'the cover art module is not linked into eboot.bin'
# The Pegasus endpoint the shell waits for is present.
assert b'127.0.0.1:6970' in eboot or b'6970' in eboot, 'eboot.bin does not mention the Pegasus port'

record = {'titleId': title_id, 'contentId': param['contentId'],
          'contentVersion': param['contentVersion'],
          'build': _stamp.group(1), 'channel': _stamp.group(2).strip(),
          'imageSha256': digest(dist / f'{title_id}.ffpkg'),
          'appSha256': digest(extracted / 'eboot.bin'), 'helperSha256': digest(helper_path),
          'files': len(folder),
          'caBundleCertificates': certificates}
Path('build/package-verification.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(record, indent=2))
PY
