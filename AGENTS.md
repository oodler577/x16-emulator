# Serial/network fork maintenance

master mirrors X16Community/x16-emulator master and must carry no local features.
serial-network is upstream master plus the optional serial-card patch.
Do not submit an upstream PR without a user request.

Update master by fetching upstream/master, fast-forwarding local master, then
pushing master to origin. Merge the updated master into serial-network, resolve
conflicts, and rerun the build/tests before pushing that branch. Never force
push upstream or mix feature commits into master.

The development checkout is /opt/x16-emulator-net on the Linux Prog8 VM.
Windows is a required target: use Winsock and the Windows CI job, not POSIX-only
network APIs. Host socket work belongs off the emulated CPU/UI thread.
Keep the device disabled by default and avoid incoming network listeners.

src/netcard.c owns UART/modem state, src/net_socket.c owns threaded host TCP.
The modem is a documented subset, not actual ESP32 firmware or full Zimodem.
Keep register debug reads side-effect free, preserve byte data and backpressure,
and test immediate close, cancellation/generation changes, and reset.

See SERIAL-NETWORK.md for build commands and tests/AGENTS.md for test boundaries.
Do not modify the existing system x16emu installation or the private x16net repo
as part of emulator work. Use /opt/x16net/bin/x16net.bin read-only for local
optional integration tests; do not copy it into this public fork.
