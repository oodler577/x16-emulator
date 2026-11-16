Experimental serial/network emulator preview for **Windows x86-64** and
**Linux x86-64**.

This release adds the Commander X16 dual-UART serial-card model and a host TCP
modem to the `serial-network` branch. `master` remains an upstream mirror.

Download the Windows ZIP or Linux tar.gz, extract the complete package, and put
your compatible `rom.bin` beside the executable. An X16 ROM is not bundled.
Start with the included launcher, or use:

```text
Windows: x16emu.exe -rom rom.bin -serialcard
Linux:   ./x16emu -rom rom.bin -serialcard
```

Windows includes the required runtime DLLs. Linux targets Ubuntu 24.04-compatible
x86-64 systems with SDL2 installed. Verify downloads using `SHA256SUMS`.

[Setup, ROMTERM manual tests, and Prog8/BASIC/ASM examples](https://github.com/oodler577/x16-emulator/wiki)

Both native builds must pass UART/socket tests and a full emulated-CPU TCP echo
test before the release is published. Linux also runs sanitizer checks.

**Scope:** raw TCP, UART/FIFO/handshake/IRQ modeling, quiet mode and DTR hangup.
This is a Zimodem-compatible subset, not ESP32 firmware. Wi-Fi configuration,
AT&G, Telnet negotiation, TLS/HTTPS, SSH, UDP and incoming listeners are not
implemented. The second UART is unattached except for local loopback.

The physical card and David Murray's ROMTERM are described on
[TexElec's Commander X16 Serial & ESP32 Network Card page](https://texelec.com/product/commander-x16-serial-network-card/).
