# Serial card tests

test_serialcard.py loads the actual C implementation via ctypes and uses only
loopback TCP fixture servers. It tests both UARTs, masking, FIFOs, interrupt
acknowledgment, debugger reads, timing, raw byte values, backpressure, and reset.
All waits have wall-clock limits. Never replace failures with arbitrary sleeps.

test_emulator_serialcard.py runs a guest on the full testbench-enabled emulator.
serialcard_guest.asm is its source; serialcard_guest.bin is the reproducible raw
binary used on Windows without requiring an assembler. Linux CI verifies it.
The test transfers exactly 4096 known bytes and verifies them at both ends.

Build the optional x16net client with -D USING_LIBRARY=1 and pass --guest plus
--x16net for local validation against the existing private library. The user has since authorized bundling the x16net binary and Gopher PRG in
releases; see demos/x16net/PROVENANCE.json. No credentials belong in the repository.

fetch_test_rom.py downloads a hash-pinned official r49 archive for tests only.
The ROM must not be bundled in the fork's binary artifacts. The tests do not
validate real ESP32 firmware, card wiring or electrical behavior.

Modem protocol tests execute the real C code with local TLS, SSH, HTTP, UDP,
Telnet and listener fixtures. Never depend on public servers. SSH fixtures use
temporary keys and verified known_hosts; TLS fixtures use a temporary CA.
Linux tests native serial with a PTY; Windows checks invalid COM failure and
requires later physical COM testing. Keep those evidence boundaries explicit.
The bundled Gopher test runs the x16net protocol-test PRG against a local server,
verifies files byte-for-byte, and never overwrites an existing user file.
