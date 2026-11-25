#!/usr/bin/env python3
"""Fail closed: the release must contain newly built files for every client."""
from pathlib import Path
import hashlib
import sys
import zipfile

folder = Path(sys.argv[1] if len(sys.argv) > 1 else 'dist')
needed = {
    'capture2cloud-x86_64.AppImage': None,
    'capture2cloud-linux-x86_64.zip': ['capture2cloud'],
    'capture2cloud-switch.zip': ['capture2switch.nro'],
    'capture2cloud-wiiu-console.zip': ['capture2wiiu.wuhb', 'capture2wiiu.rpx'],
    'capture2cloud-old3ds.zip': ['capture2cloud-old3ds.cia', 'capture2cloud-old3ds.3dsx'],
    'capture2cloud-wiiu-gamepad.zip': ['wiiu_pad', 'vendor/libdrc/libdrc.so', 'vendor/x264/lib/libx264.so.140'],
}
android = list(folder.glob('capture2cloud-*.apk'))
if len(android) != 1:
    raise SystemExit(f'Expected exactly one Android APK, got: {android}')
if android[0].name != 'capture2cloud-android-debug.apk' and android[0].name.count('.') < 4:
    raise SystemExit(f'Unexpected Android release APK name: {android[0].name}')
for name, members in needed.items():
    file = folder / name
    if not file.is_file() or file.stat().st_size == 0:
        raise SystemExit(f'Missing/empty release asset: {name}')
    if members:
        with zipfile.ZipFile(file) as zipf:
            names = {n.removeprefix('./') for n in zipf.namelist()}
            for expected in members:
                if expected not in names:
                    raise SystemExit(f'{name} missing {expected}')
for file in sorted(list(folder.glob('*.zip')) + list(folder.glob('*.apk')) + list(folder.glob('*.AppImage'))):
    digest = hashlib.sha256(file.read_bytes()).hexdigest()
    print(f'{digest}  {file.name}')
