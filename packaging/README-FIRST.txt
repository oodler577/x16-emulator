X16 SERIAL/NETWORK EMULATOR - WINDOWS / LINUX

Extract this whole package into a fresh folder and add your compatible rom.bin.
The X16 ROM is not included. Keep all Windows DLLs and curl-ca-bundle.crt here.

To browse Gopher:
  Windows: double-click start-gopher.bat
  Linux:   sh start-gopher.sh

The Gopher application and matching x16net.bin are included. Read GOPHER.md.
The filename is x16net.bin (not x16lib.bin).

To use ROMTERM or another program:
  Windows: x16emu.exe -rom rom.bin -serialcard -prg ROMTERM.PRG -run
  Linux:   ./x16emu -rom rom.bin -serialcard -prg ROMTERM.PRG -run

Supply ROMTERM yourself. Choose UART $9FE0, 115200, RTS/CTS and ASCII.
Wi-Fi configuration is a MOCK; connections use your host's network and DNS.
Try AT and ATI. See SERIAL-NETWORK.md for Telnet, TLS, SSH, incoming connections,
AT&G downloads, UDP framing and second-UART attachment.

Linux needs Ubuntu 24.04-compatible libraries:
  sudo apt install libsdl2-2.0-0 libcurl4t64 libssh2-1t64

SHA256SUMS inside this package verifies the bundled x16net library and Gopher
application. The release page's SHA256SUMS verifies the downloadable archives.
PROVENANCE.json records the x16net source revision for these demo binaries.

Setup and examples: https://github.com/oodler577/x16-emulator/wiki
