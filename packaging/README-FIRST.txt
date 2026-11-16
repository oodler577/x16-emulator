X16 Serial/Network Emulator Preview
==================================

This is the serial-network branch of oodler577/x16-emulator.
Networking is opt-in. The included launcher enables the emulated serial card.

1. Extract the whole archive into its own folder.
2. Copy your compatible Commander X16 rom.bin into that folder.
   The release does not include an X16 ROM.
3. Windows: double-click start-serial-network.bat.
   Linux: run ./start-serial-network.sh.
   Linux requires SDL2 and an Ubuntu 24.04-compatible x86-64 runtime.
4. The network UART is $9FE0. The second, unattached UART is $9FE8.
5. Run ROMTERM, choose $9FE0 / 115200 / RTS/CTS, then type ATI.
   Use ATD"host:port" for raw TCP. Your host supplies the Internet connection;
   there is no Wi-Fi setup in the emulator.

Direct launch:
  Windows: x16emu.exe -rom rom.bin -serialcard
  Linux:   ./x16emu -rom rom.bin -serialcard

Manual tests and Prog8/BASIC/assembly examples:
https://github.com/oodler577/x16-emulator/wiki

Read SERIAL-NETWORK.md for supported commands and limitations.
This is a UART and modem-command subset, not actual ESP32 firmware.
AT+CONFIG, AT&G, Telnet negotiation, TLS/HTTPS, SSH, UDP, and incoming
listeners are not implemented. Physical-card validation remains separate.

The master branch mirrors upstream. Local work stays on serial-network.
