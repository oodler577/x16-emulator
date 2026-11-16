#!/bin/sh
set -eu
serial_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ ! -f "$serial_dir/rom.bin" ]; then
    echo "Put your compatible Commander X16 rom.bin beside x16emu first." >&2
    exit 1
fi
exec "$serial_dir/x16emu" -rom "$serial_dir/rom.bin" -serialcard "$@"
