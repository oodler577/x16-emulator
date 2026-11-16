# Serial/network branch

`master` mirrors X16Community's upstream master. `serial-network` carries the
optional serial/network card and host bridge. No upstream PR has been submitted.

## Start on Windows or Linux

Download the appropriate package from
[Releases](https://github.com/oodler577/x16-emulator/releases), extract everything
into a fresh folder, and add your compatible `rom.bin`. The ROM is not bundled.

```text
Windows: x16emu.exe -rom rom.bin -serialcard
Linux:   ./x16emu -rom rom.bin -serialcard
```

Windows includes recursive DLL dependencies, third-party license files and a CA
bundle for TLS. Linux targets Ubuntu 24.04 with SDL2, libcurl and libssh2 installed:
`sudo apt install libsdl2-2.0-0 libcurl4t64 libssh2-1t64`.

The packages also include **x16net.bin** and **x16net-gopher.prg**, a Prog8 Gopher
browser using the pure library bindings. Use `start-gopher.bat` or
`sh start-gopher.sh`; see **GOPHER.md** for navigation and limits.

The card is disabled unless `-serialcard` is present. Its default network UART is
**$9FE0**, and the second UART is **$9FE8**. An optional aligned base address
($9F60..$9FF0, excluding $9FB0) follows `-serialcard`. Do not overlap the MIDI card.

## Host bridge and mock Wi-Fi

**This is a host network bridge, not an ESP32 firmware image or a Wi-Fi radio.**
`ATI`, `ATI2`, `ATI3`, `ATW` and `AT+CONFIG` identify the Wi-Fi interface as a
**MOCK**. `ATW"label,password"` changes only a mock SSID label; no Wi-Fi connection
is made and no password is saved. The configuration menu offers bridge status
and a mock label. Your host's network routing and DNS provide connectivity.
No credentials or real Wi-Fi settings are needed for ordinary TCP connections.

## ROMTERM commands

Set ROMTERM to $9FE0, 115200, RTS/CTS and ASCII. For ANSI services, choose the
appropriate ROMTERM display mode as well. Basic command examples:

| Command | Behavior |
| --- | --- |
| `AT`, `ATI` | Test the command interpreter; identify the bridge |
| `AT+CONFIG`, `ATW`, `ATI3` | Clearly labeled mock Wi-Fi setup/status |
| `ATD"host:port"` | Raw TCP stream |
| `ATDT"host:port"` | Telnet stream with option negotiation |
| `ATS62=1` / `ATS62=0` | Enable / disable Telnet as the default for dialing |
| `ATDS"host:port"` | TLS stream with certificate/hostname verification |
| `ATDS"user:password@host:port"` | SSH shell (password authentication) |
| `ATC`, `ATC0` | Current connection and listener information |
| `ATO` or `ATD` | Resume the current stream after escaping |
| `ATH` | Disconnect; retain a configured listener |
| `ATA2323` | Start an incoming TCP listener on port 2323 |
| `ATA` | Answer the pending call |
| `ATS0=1S41=1A2323` | Listen and automatically enter a stream on the first ring |
| `ATN0`, `ATN1` | Stop the listener |
| `ATZ` | Disconnect, stop listeners and reset volatile settings |

Pause for at least one second, send `+++`, then pause for one second to return
to command mode while keeping carrier. `ATO` resumes. `S2` selects the escape
byte; values >=128 disable recognition. `S12` is the guard interval in 20 ms
units. Unguarded plus signs remain payload. DTR hangup remains available with
`S63=2` (or OPA with `S63=3`). `Q1` suppresses result messages for binary transfers;
DCD then reports carrier. Restart the emulator between unrelated guest programs.

The initialization string used by x16net remains supported, including raw mode,
quiet mode, flow control, ASCII and the supported profile pin-register settings.
`&W` remains a volatile compatibility acknowledgment; settings are not persisted.
Unknown commands return `ERROR`, rather than pretending to support ESP32 features.
`I1`/`I5` show implemented settings. `I6` labels its synthetic MAC as a mock.

### Telnet

IAC commands are consumed, literal $FF is escaped, and fragmented commands are
handled across UART reads. Negotiation supports binary mode, echo,
suppress-go-ahead, VT100 terminal type and 80x25 window size; unsupported options
are refused. Raw TCP mode continues to preserve every byte. Terminal rendering
belongs to ROMTERM or the guest application, not to the network bridge.

### TLS and SSH trust

TLS uses libcurl's certificate and hostname verification. On Windows, the bundled
`curl-ca-bundle.crt` is found beside the executable. For a private/test CA, set
`X16_SERIAL_CA_FILE` to a PEM trust file before launching the emulator.

SSH uses libssh2, requests a VT100 PTY, and verifies the host key against OpenSSH
`known_hosts` before transmitting a password. It reads `~/.ssh/known_hosts` (or
`USERPROFILE` when HOME is absent on Windows). Set `X16_SERIAL_KNOWN_HOSTS` to use
another file. Add the server's independently verified key to that file first.
An unknown/mismatched host key, failed authentication or invalid certificate
returns `NO ANSWER`; it does not fall back to an insecure connection.

### Downloads with AT&G

```text
AT&G"http://host:port/path"
AT&G"https://host/path"
AT&G"host:port/path"
```

HTTP, HTTPS, FTP, FTPS and Gopher URLs use libcurl, with at most five redirects.
Without a scheme, HTTP is assumed. The worker caches up to **16 MiB**, bounded by
a 60-second request timeout. Failed/truncated HTTP transfers produce `ERROR`.
This implementation buffers before delivering the file even with S45=3.

S45 chooses the Zimodem output format:

* `S45=0`: `[ 0 length sum8 ]` plus EOL, followed by exact binary data.
* `S45=1`: hexadecimal header and data, 39 bytes per output line.
* `S45=2`: decimal header fields and one decimal byte per line.
* `S45=3`: `[ 0 length ]` plus EOL, followed by exact binary data.

A final `OK` follows a completed transfer unless quiet mode is enabled. ASCII
Ctrl+C cancels. RTS/CTS preserves output backpressure. The hardware firmware's
flash, SD card, packet-number variants, XON/XOFF and programmable packet
transformations are not emulated.

### Incoming connections

Listeners bind **127.0.0.1 by default**. To accept connections from another machine,
set `X16_SERIAL_BIND` to a particular host interface address (or `0.0.0.0`) before
launch. The host firewall must allow that port. Merely enabling `-serialcard`
does not create a listener: a guest must issue `ATAport`.

The model supports one active network connection and one configured listener.
`RING`, RI, manual answer and S0 auto-answer are modeled. A listener rearms after
disconnect; it cannot accept another connection while the first is active.
S41 is accepted; incoming connections use stream mode in this bridge. There is
no multi-connection command-mode packet API.

### UDP extension

`ATDU"host:port"` is an **emulator extension**, not a claimed Zimodem command.
Each serial packet consists of a **two-byte big-endian payload length** followed
by one UDP datagram. Incoming datagrams use the same framing. Empty datagrams
are supported. The maximum payload is 8192 bytes. Oversized outgoing frames close
carrier; oversized incoming datagrams are discarded. Only the selected peer can
send into a connected UDP socket. UART flow control cannot make UDP reliable.

## Second UART

Port B can connect to either a host serial device or a TCP serial bridge:

```text
x16emu.exe -rom rom.bin -serialcard -serial2-device COM3
./x16emu -rom rom.bin -serialcard -serial2-device /dev/ttyUSB0
x16emu.exe -rom rom.bin -serialcard -serial2 127.0.0.1:2525
```

COM/tty operations run on the worker. Guest baud, framing, DTR/RTS and break are
applied where the host OS supports them; CTS/DSR/RI/DCD are read back. Common baud
rates through 921600 are supported on Linux when provided by termios. Unsupported
host settings leave the previous host configuration in place. Linux PTYs have
no physical modem pins and report a ready peer. The TCP bridge has ready modem
signals while connected. A disconnected second UART must be reattached by
restarting the emulator. With no attachment, it remains available for local
UART loopback. Physical electrical timing/error behavior is not simulated.

## Build and tests

```sh
sudo apt install cmake libsdl2-dev zlib1g-dev libcurl4-openssl-dev libssh2-1-dev \
  python3-paramiko openssl 64tass
cmake -S . -B build -DENABLE_FLUIDSYNTH=OFF -DENABLE_TRACE=OFF \
  -DENABLE_LTO=OFF -DBUILD_SERIALCARD_TESTS=ON
cmake --build build --parallel 4
python3 tests/test_serialcard.py build/libserialcard_test.so
python3 tests/test_modem_protocols.py build/libserialcard_test.so
python3 tests/fetch_test_rom.py
python3 tests/test_emulator_serialcard.py --emulator build/x16emu --rom build/test-rom.bin
X16EMU="$PWD/build/x16emu" X16ROM="$PWD/build/test-rom.bin" python3 tests/test_gopher_demo.py
```

Windows uses the equivalent MinGW packages and `libserialcard_test.dll`. CI runs
native Windows and Linux tests, including local TLS/SSH fixtures, malformed and
fragmented messages, TCP/UDP, downloads, incoming calls and the bundled Prog8
Gopher core. Linux exercises a PTY and undefined-behavior sanitization. Windows
checks invalid COM-port failure; actual COM hardware needs manual testing. The
extracted Windows ZIP is tested with MSYS2 removed from the emulator's PATH.

Both UARTs model registers, DLAB, FIFOs, trigger levels, transmit timing, loopback,
overrun/break status, interrupt priorities and modem-status deltas. Debug reads
are side-effect free. Receive queues drain before DCD falls. A successful empty
TCP connection holds carrier for four guest character periods so polling clients
can observe it. Native DNS can delay shutdown until the host resolver returns.
WebAssembly builds do not expose host networking. NMI jumper operation and exact
ESP32/TL16C2550 timing are not modeled. Real hardware validation remains separate.

## References and branch maintenance

The command/protocol reference is Bo Zimmerman's
[Zimodem](https://github.com/bozimmerman/Zimodem), inspected at
`a534e5129d88b8b09aa01fe17498d5cd5368569e`. Credit also goes to David Murray's
ROMTERM and [TexElec's Commander X16 Serial & ESP32 Network Card](https://texelec.com/product/commander-x16-serial-network-card/).
This independently implemented bridge does not bundle their firmware or ROMTERM.

To update the upstream base, fetch `upstream`, fast-forward `master` to
`upstream/master`, push `master`, then merge it into `serial-network` and rerun
both native CI jobs. Feature commits belong only on `serial-network`. Publish a
`serial-network-v*` tag to build, test and release Windows/Linux prerelease assets.
