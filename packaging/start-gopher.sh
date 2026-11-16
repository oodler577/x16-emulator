#!/bin/sh
set -eu
cd "$(dirname "$0")"
exec ./x16emu -rom rom.bin -serialcard -fsroot . -prg x16net-gopher.prg -run "$@"
