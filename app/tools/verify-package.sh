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
image=dist/$title_id.ffpkg
runner=$(bash tools/setup-packaging-dependencies.sh ffpkg)
mkdir -p build
temporary=$(mktemp -d "$root/build/verify-package.XXXXXX")
trap 'rm -rf -- "$temporary" "$temporary.log"' EXIT
"$runner" extract "$image" "$temporary" >"$temporary.log"
python3 - "$temporary" "$title_id" <<'PY'
import hashlib, json, struct, sys
from pathlib import Path

extracted = Path(sys.argv[1])
title_id = sys.argv[2]
staged = Path('dist') / title_id


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
          'imageSha256': digest(Path('dist') / f'{title_id}.ffpkg'),
          'appSha256': digest(extracted / 'eboot.bin'), 'files': len(folder),
          'caBundleCertificates': certificates}
Path('build/package-verification.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(record, indent=2))
PY
