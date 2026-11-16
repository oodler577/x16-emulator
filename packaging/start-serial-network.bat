@echo off
setlocal
cd /d "%~dp0"
if not exist "%~dp0rom.bin" (
  echo Put your compatible Commander X16 rom.bin beside x16emu.exe first.
  pause
  exit /b 1
)
"%~dp0x16emu.exe" -rom "%~dp0rom.bin" -serialcard %*
