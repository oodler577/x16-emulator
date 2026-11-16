@echo off
setlocal
cd /d "%~dp0"
if not exist rom.bin (
  echo Put your compatible Commander X16 rom.bin here first.
  pause
  exit /b 1
)
"%~dp0x16emu.exe" -rom rom.bin -serialcard -fsroot . -prg x16net-gopher.prg -run %*
