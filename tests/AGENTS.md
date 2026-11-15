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
--x16net for local validation against the existing private library. No private
binary or credentials belong in this public repository.

fetch_test_rom.py downloads a hash-pinned official r49 archive for tests only.
The ROM must not be bundled in the fork's binary artifacts. The tests do not
validate real ESP32 firmware, card wiring or electrical behavior.
