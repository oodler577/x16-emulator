# Serial/network development branch

This fork keeps **master identical to upstream master**. **serial-network**
contains that base plus the optional serial/network-card implementation.
There is no upstream PR yet.

## Windows

Download the Windows ZIP from [Releases](https://github.com/oodler577/x16-emulator/releases).
Development builds are also available as the **serial-network-windows-x86_64**
artifact in this branch's **Serial Network** workflow. Extract it into a separate folder;
it includes x16emu.exe and runtime DLLs. Supply your compatible rom.bin, then run:

    x16emu.exe -rom rom.bin -serialcard

For Linux, use the serial-network-linux artifact or build from source:

    cmake -S . -B build -DENABLE_FLUIDSYNTH=OFF -DENABLE_TRACE=OFF
    cmake --build build --parallel 4
    ./build/x16emu -rom /path/to/rom.bin -serialcard

The native builds need SDL2 and zlib, just like the base emulator. Windows uses
Winsock; Linux/macOS use POSIX sockets. No Python service, VM bridge adapter or
Wi-Fi configuration is needed for normal use. Python is only a test dependency.

The option is disabled by default. Enabling it allows guest programs to initiate
outbound TCP connections using the host's ordinary network access. There are no
incoming listeners, shell commands, credential stores or firmware updates.
It supports IPv4/IPv6/DNS through the host resolver, not a virtual Ethernet NIC.

Optional base address:

    x16emu.exe -serialcard 9fe0

Use a 16-byte-aligned I/O slot $9f60..$9ff0 except $9fb0 (emulator registers).
A MIDI card cannot occupy the same slot. Port A at base is the host modem;
port B at base+8 models an unattached physical UART, with local loopback support.
Port B does not open a host COM/tty port.

## Guest use

The default hardware layout is $9fe0-$9fe7 for the ESP/network UART and
$9fe8-$9fef for the second UART. Guest programs access ordinary card registers;
no emulator-specific networking API is required. Existing ROMTERM UART probing,
ATI and quoted raw TCP dialing can exercise the model. ROMTERM itself has not
been manually tested here.

The existing x16net binary can initialize $9fe0, prepare, connect and transfer
raw bytes without changes. Prog8 uses its normal binary bindings. Do not load
the binary on top of a guest program's memory.

Examples of supported commands:

    ATI
    ATE0V1Q0X0F0R0&P0S62=0S63=2S46=0S48=0S50=0S54=0N0H
    ATD"example.com:80"
    ATQ1D"example.com:80"

Q1 suppresses modem result text; DCD reports the connection. DTR deassertion
hangs up when S63=2. Raw stream bytes are not translated: NUL, XON/XOFF, +++,
ESC and $ff remain data. There is no +++ escape recognizer; use DTR.
While quiet, closed/refused connections produce no text. Code using Q1 must
handle carrier/timeout and its own application deadlines.

The parser also supports AT, H/H0, Z/Z0, E0/1, V0/1, Q0/1, X0/1, F0/4, R0/1,
Bn, N0, &W, &P0, &K0/3, S63=0/2/3, and the zero-valued profile registers shown
above. S45=0..3 is accepted for initialization compatibility; AT&G is unsupported.
&W is a volatile no-op. ATI4 identifies this bridge, not a Zimodem version.
Unknown commands return ERROR (unless quiet).

## Implementation boundaries

* Two independent UART register sets, DLAB/divisors, scratch registers, 1/16-byte
  FIFOs, trigger levels, THRE/TEMT, local loopback, overrun/break flags, receive
  timeout, interrupt priorities and modem-status deltas are modeled.
* The emulator's existing IRQ path carries card interrupts. No NMI jumper mode.
* Character scheduling follows emulated CPU clock and UART divisor/framing.
  It is not an electrical/cycle-exact TL16C2550 validation. Mismatched modem
  baud, parity corruption, and analog serial lines are not simulated.
* Host sockets and DNS run on a worker thread. Reset/cancellation generations
  prevent stale results from reconnecting a reset guest. Native OS DNS cannot
  always be interrupted; closing the emulator can wait for an in-progress host
  resolver call. TCP connect waits are bounded and cancellable.
* Buffers are finite. RTS/CTS holds received data at the modem/socket side;
  disabling flow control can overrun the emulated receive FIFO.
* Queued incoming bytes drain before carrier drops on remote close. Even an
  immediate empty connection asserts carrier for four emulated character times
  so polling clients can observe it. This is a deliberate model behavior, not
  evidence about physical ESP32 firmware queue timing.
* This is a Zimodem-compatible command subset. It does not execute ESP32 firmware,
  provision Wi-Fi, negotiate Telnet, offer AT&G file downloads, TLS/HTTPS, UDP,
  SSH, inbound servers, saved settings or multiple network connections.
* Browser/WebAssembly builds retain their existing behavior; host networking is
  unavailable and -serialcard is rejected.

## Tests

    cmake -S . -B build -DENABLE_FLUIDSYNTH=OFF -DENABLE_TRACE=OFF -DENABLE_LTO=OFF -DBUILD_SERIALCARD_TESTS=ON
    cmake --build build --parallel 4
    python3 tests/test_serialcard.py build/libserialcard_test.so
    python3 tests/fetch_test_rom.py
    python3 tests/test_emulator_serialcard.py --emulator build/x16emu --rom build/test-rom.bin

On Windows the test library is build/libserialcard_test.dll.
CI runs the real C register/socket tests and a full guest-to-local-TCP echo
test on Linux and Windows. Linux also verifies assembly fixture reproduction and
runs undefined-behavior sanitization. All servers are loopback-only; CI does not
depend on public Internet services. A pinned official ROM is fetched for testing
but is not redistributed in the fork artifacts.

Optional local x16net integration (supply your own binary):

    64tass -q -a -B --nostart -D USING_LIBRARY=1 -o build/x16net_guest.bin tests/serialcard_guest.asm
    python3 tests/test_emulator_serialcard.py --emulator build/x16emu --rom build/test-rom.bin --guest build/x16net_guest.bin --x16net /path/to/x16net.bin

## Tracking upstream

    git fetch upstream
    git switch master
    git merge --ff-only upstream/master
    git push origin master
    git switch serial-network
    git merge master

Run both native CI jobs after integrating changes. Keep feature commits out of
master, and never push this branch to upstream without agreeing on a PR.

## Publishing a preview

Push a serial-network-v* tag from this branch. The workflow rebuilds and tests
Windows and Linux, then publishes both packages and SHA256SUMS as a prerelease.
The publisher runs only after both native jobs succeed.
