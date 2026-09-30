# Ethernet-to-CAN recovery

The active bridge is UDP on socket 0. Network settings remain
`192.168.1.101/24`, gateway `192.168.1.1`, MAC `00:08:dc:ab:cd:ef`.
Commands arrive on port 56800; responses go to port 56801 on the last valid
command sender's IP. Before the first valid command, responses use
`192.168.1.255`. CAN remains 250 kbit/s with automatic retransmission disabled.

## Packet handling

Commands use `{007,232,001,ID,DLC,DATA}`. ID is hexadecimal, from 0 through
7FF; DLC is decimal, from 0 through 8; DATA is exactly two hexadecimal digits
per byte. DLC 0 uses an empty DATA field, for example `{007,232,001,200,0,}`.
Hexadecimal digits are case-insensitive; ID may have a `0x` prefix. Whitespace
around fields or the packet is accepted. Missing fields, extra fields,
invalid digits, incomplete payloads (except the legacy enable below), and packets
over 200 bytes are rejected.
An oversized datagram is drained in bounded chunks, without interpreting its
continuations as commands. Rejected commands cannot change the destination IP.

The current analyzer enables a device with `{007,232,001,ID,4,1445650a}`.
For compatibility with released analyzers, the bridge also accepts the exact
legacy form `{007,232,001,ID,5,1445650a}` (case-insensitive hex), preserving DLC 5
and supplying a zero fifth byte. The active load uses the first four bytes for
this command. Other incomplete payloads remain invalid; in particular, truncated
settings commands are never padded. Previously rejecting the legacy enable kept
the load off, allowing the analyzer's zero-current detection to report
`END_VOLTAGE_REACHED` even above the cutoff voltage.

Responses retain `{507,232,4,ID,8,B0,B1,B2,B3,B4,B5,B6,B7}` with uppercase
hexadecimal ID/bytes. ID has at least two digits. Short CAN frames are padded
with zero bytes. Extended-ID and remote-request CAN frames cannot be represented
by this protocol and are counted and ignored.

The CAN interrupt copies complete frames into a static 32-frame queue. A full
queue drops the incoming frame. Every normal task iteration receives one UDP
command/chunk and forwards one queued CAN frame, then yields for one RTOS tick.
This is a best-effort bridge, not a lossless transport at arbitrary bus load.
CAN mailbox exhaustion, CAN submission failure, and UDP send failure are counted;
failed commands/frames are not retried.

## Recovery and debugger counters

`bridge_diagnostics` is available in Debug and Release images. Useful fields:

| Fields | Meaning |
| --- | --- |
| `task_iterations`, `last_progress_ms` | Task entry count and latest HAL tick; progress continues during link-down and retry delays |
| `stack_free_bytes` | Minimum unused task stack, sampled every 1000 iterations; zero until first sample |
| `parser_rejections` | Rejected complete datagrams, counted once per oversized datagram |
| `can_queue_overflow`, `can_offline_drops` | Frames dropped because the queue is full or networking is recovering/offline |
| `can_tx_ok`, `can_tx_busy`, `can_tx_errors` | CAN mailbox submissions accepted, unavailable/bus-off, or failed; acceptance is not proof of on-wire delivery |
| `can_rx_errors`, `can_rx_ignored`, `can_rx_forwarded` | CAN receive failures, unsupported frames, and successful UDP forwards |
| `can_esr`, `can_error_transitions`, `can_bus_off_entries`, `can_bus_off_exits` | Task-sampled CAN status; brief transitions between samples may be missed |
| `spi_errors`, `ethernet_timeouts` | HAL SPI failures and expired software polling deadlines |
| `socket_errors`, `socket_reopens`, `udp_send_drops` | Socket faults, open attempts, and failed/dropped UDP sends |
| `ethernet_resets`, `ethernet_init_failures`, `link_down_events` | W5500 initialization/reset attempts (including boot), failures, and observed link loss |

SPI transfers have a 10 ms timeout. Command/status and stable-counter polling
have 100 ms deadlines; send completion has a 2 s deadline. All deadlines tolerate
tick wraparound. A latched SPI failure invalidates reads even if they return zero.
Polling yields to the RTOS; no SPI operation runs in the CAN interrupt.

Socket faults discard queued CAN frames and request reopening. A failed reopen
escalates to SPI reinitialization and W5500 hardware reset. Reset holds RST low
for at least 1 ms and allows at least 10 ms after release (including an extra
RTOS tick for timing margin). Initialization verifies the W5500 identity,
network settings, and socket buffer sizes. Failed initialization/reopening is
retried after one second. An unplugged Ethernet cable waits for link-up without
repeated chip resets. CAN automatic bus-off recovery is enabled; when bus-off is
observed, pending transmit mailboxes are aborted to avoid replaying old commands.

There is no watchdog or automatic MCU reset. The unused TCP application loop
was removed; the remaining ioLibrary TCP APIs are not part of this recovery path.

## Automated validation

With `arm-none-eabi-gcc` on PATH:

```sh
cmake --preset Debug -DCMAKE_C_FLAGS=-fstack-usage
cmake --build --preset Debug
cmake --preset Release -DCMAKE_C_FLAGS=-fstack-usage
cmake --build --preset Release
```

Host tests require a native GCC-compatible compiler and its AddressSanitizer and
UndefinedBehaviorSanitizer runtime libraries:

```sh
cmake -S tests -B build/host-tests -G Ninja
cmake --build build/host-tests
ctest --test-dir build/host-tests --output-on-failure
```

The tests run the production parser, bridge, and W5500/socket drivers against an
SPI register model. Coverage includes current/legacy analyzer enable commands,
deterministic zero padding, rejection of other truncated commands, malformed
packets and a deterministic
10,000-input sweep, FIFO ordering/overflow, traffic fairness, oversized and empty
datagrams, stuck command/status registers, missing send completion, hardware
timeouts, SPI failure during command acknowledgement, unstable size registers,
tick rollover, initialization retries, link recovery, mailbox exhaustion, and
sampled CAN bus-off handling. They do not simulate physical CAN recovery or PHY
timing. Compiler stack reports are useful estimates; the real stack margin must
be checked with `stack_free_bytes` on hardware.

## Board acceptance (not yet performed)

1. Record the firmware revision, board, power supply, CAN termination, peer,
   offered traffic rates, and starting counters. Confirm 250 kbit/s with a CAN
   analyzer and verify short-frame zero padding and DLC 0/8 commands.
2. Run simultaneous traffic in both directions at the intended application rate;
   verify payloads and continued UDP-to-CAN service under heavy CAN input. At
   overload, verify counted drops and continued task progress.
3. Inject malformed/oversized UDP packets between valid commands. Confirm no CAN
   transmission or destination change from invalid traffic, then verify the next
   valid command succeeds.
4. Disconnect/reconnect Ethernet repeatedly, stop/restart the peer, and make the
   learned destination unreachable. Confirm bounded errors, no reset storm during
   cable disconnection, and restored forwarding within five seconds of healthy
   link/peer availability.
5. On an isolated CAN test bench, induce bus-off with controlled error injection,
   restore a healthy bus, and verify forwarding resumes without power cycling.
   Record ESR, error counters, and analyzer evidence; software submission alone
   does not establish delivery.
6. Run a 24-hour bidirectional traffic soak. Record counters and stack high-water
   mark periodically. Require no stalls/power cycles, correct payloads, and at
   least 256 bytes of remaining task stack. Explain any drops against the offered
   traffic and injected faults. If a stall occurs, halt over SWD before resetting
   and capture the PC/backtrace, fault registers, CAN ESR, and bridge counters.
