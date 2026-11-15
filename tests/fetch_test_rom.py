#!/usr/bin/env python3
"""Fetch only the ROM from a hash-pinned official emulator test package."""
import hashlib, io, urllib.request, zipfile
from pathlib import Path
url="https://github.com/X16Community/x16-emulator/releases/download/r49/x16emu_linux-x86_64-r49.zip"
expected="efb2d4ad36ae4244370004891aa67bdf85d2ec01b42b7f12f97b48b62aff009d"
data=urllib.request.urlopen(url,timeout=90).read()
if hashlib.sha256(data).hexdigest()!=expected:
    raise SystemExit("official r49 test package checksum mismatch")
with zipfile.ZipFile(io.BytesIO(data)) as z:
    rom=z.read("rom.bin")
path=Path("build/test-rom.bin")
path.parent.mkdir(exist_ok=True)
path.write_bytes(rom)
print("Fetched matching r49 ROM for tests; not included in fork artifacts.")
