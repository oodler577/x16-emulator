Serial/network preview 0.2.0 for **Windows x86-64 and Linux x86-64**.

Includes the **X16NET Gopher demo** (`x16net-gopher.prg`) and its matching
**x16net.bin**. Add your compatible `rom.bin`, then run `start-gopher.bat` on
Windows or `sh start-gopher.sh` on Linux. The ROM is not bundled.

This update adds Telnet negotiation, verified TLS and SSH connections, AT&G
HTTP/HTTPS/FTP/Gopher downloads, guarded +++ escape and connection status,
incoming TCP calls, framed UDP as a documented emulator extension, and second
UART attachment to host COM/tty devices or a TCP serial bridge.

**Wi-Fi configuration is explicitly a MOCK. All networking uses the host bridge.**
No radio configuration or Wi-Fi credentials are required or saved.

Extract the entire Windows ZIP: it includes recursive runtime DLL dependencies,
a TLS CA bundle and third-party license files. Linux targets Ubuntu 24.04 with
SDL2, libcurl and libssh2. See README-FIRST.txt and SERIAL-NETWORK.md.

Native Windows/Linux protocol tests, full CPU-to-TCP tests and the bundled
Prog8 Gopher protocol test must pass before publication. Linux also tests a
serial PTY and undefined-behavior sanitization. Actual Windows COM hardware and
the physical X16 card still need manual verification. This host implementation
is not an ESP32 firmware image or a claim of complete firmware parity; documented
limits include one active connection, bounded downloads and volatile settings.

[Setup, commands and examples](https://github.com/oodler577/x16-emulator/wiki)

Credit: Bo Zimmerman's Zimodem; David Murray's ROMTERM; and
[TexElec's Commander X16 Serial & ESP32 Network Card](https://texelec.com/product/commander-x16-serial-network-card/).
