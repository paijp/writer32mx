# writer32mx

A PIC32MX270F256B-to-PIC32MX270F256B flash programmer via 2-wire Enhanced ICSP.
Both the writer and the target use PIC32MX270F256B.

## Files

| File | Description |
|---|---|
| `writer32mxuart.c` | Receives Intel HEX via UART2 and programs the target |
| `writer32mxcdc.c` | Receives Intel HEX via USB CDC and programs the target |
| `writer32mx-wroom-c20p1305-barcodeuart.c` | Receives Intel HEX over Wi-Fi (ESP-WROOM-02 + ChaCha20-Poly1305); pairing barcodes from a 9600 bps serial reader |
| `writer32mx-wroom-c20p1305-barcodehid.c` | Same, pairing barcodes from a USB-HID reader in keyboard mode |
| `c20p1305.h` | ChaCha20-Poly1305 single-file implementation, copied from [paijp/single-file-chacha20poly1305](https://github.com/paijp/single-file-chacha20poly1305) (MIT / public domain) |

## Hardware

### Requirements

- **writer**: PIC32MX270F256B
- **target**: PIC32MX270F256B
- **Crystal**: 4 MHz (required for USB; CDC version only)

### Pin connections

| Writer pin | Target pin | ICSP | Debug serial 115200 8N1 |
|---|---|---|---|
| RB2 | RB10 (PGED2 / UTX2) | ICSP data | target TX -> writer |
| RA0 | RB11 (PGEC2 / URX2) | ICSP clock | writer -> target RX |
| RA1 | MCLR | Reset control | |

## Build

Build with MPLAB X IDE and the XC32 compiler.
Each version is a single self-contained source file
(`writer32mxuart.c` or `writer32mxcdc.c`).

The Dockerfile at https://github.com/paijp/mplabx can be used to build
in a containerized environment.

## Operation

Data sent to the writer (via CDC or UART) is forwarded to the target at
115200 bps 8N1 on the PGC pin.  Data received from the target on the PGD
pin is returned to the host via CDC or UART.  This allows the host to
interact with a bootloader or debug output running on the target using a
plain serial terminal.

If the incoming data contains a `:` character (Intel HEX record start
code), the writer switches to programming mode and writes the HEX data to
the target flash via ICSP.

### Note on DEVCFG0

The DEBUG field in DEVCFG0 must be set to `11b` (debugger disabled,
ICSP accessible) for the target to run correctly after programming.
Some programmers silently modify this field; see
http://www.ze.em-net.ne.jp/~kenken/bbs/817.html for details.
The writer sets bit 1:0 of the DEVCFG0 block to `11b` before writing
(`p->d[0x3fc] |= 3`).

## Usage

### UART version (writer32mxuart.c)

```sh
# Monitor target output
stty -F /dev/ttyUSB0 115200 raw -echo
cat /dev/ttyUSB0

# In another terminal, send HEX file to program the target
cp test.hex /dev/ttyUSB0
```

### CDC version (writer32mxcdc.c)

Connect the writer to a PC via USB. It appears as `/dev/ttyACM0`.

```sh
# Terminal 1: monitor target output
stty -F /dev/ttyACM0 raw -echo
cat /dev/ttyACM0

# Terminal 2: send HEX file to program the target
cp test.hex /dev/ttyACM0
```

When programming is complete the target resets and starts the new program.

### Wi-Fi versions (writer32mx-wroom-c20p1305-*.c)

The host link moves to Wi-Fi: an ESP-WROOM-02 (AT firmware, 115200 bps)
on UART1 (UTX1=RPB15, U1RX=RPB13) talks to the PHP receiver from
[paijp/single-file-chacha20poly1305](https://github.com/paijp/single-file-chacha20poly1305)
`sample-host/`, authenticated and encrypted with ChaCha20-Poly1305.
The target side uses the same PGE\*2 pins as the CDC/UART versions, so
unmodified targets (e.g. ones printing debug on UTX2/RPB10) work as-is;
the writer side moves to UART2-capable pins:

| Writer pin | Target pin | ICSP | Debug serial 115200 8N1 |
|---|---|---|---|
| RB8 (P1) | RB10 (PGED2 / UTX2) | ICSP data | target TX -> writer |
| RB9 (P10) | RB11 (PGEC2 / URX2) | ICSP clock | writer -> target RX |
| RA1 (P3) | MCLR | Reset control | |

UART1 carries the WROOM link full-time; UART2 carries the target link
(U2RX=RPB8, UTX2=RPB9), so the target's debug output is captured even
while a server exchange is in flight.

Pairing follows the wroomc20p1305 samples: for 10 s after boot two
barcodes may be scanned and are persisted to flash (CP=ON):

```
WIFI:T:WPA;S:<ssid>;P:<password>;;
C20P:K:<64 hex key>;U:<URL up to "key0c20=">;;
```

- `-barcodeuart`: a 9600 bps serial reader wired through a weak resistor
  onto the WROOM->PIC32 line (the WROOM's TX pin is parked as GPIO input
  during the window).  Local debug log on UTX2/RPB0 (P4).
- `-barcodehid`: a USB-HID reader in keyboard mode on RB10/RB11 (US and
  JIS layouts auto-detected).  Local debug log on UTX2/RPB0 (P4).

The local debug log goes quiet once the writer loop starts (UART2 then
belongs to the target).  After the window closes and the Wi-Fi
association completes, the writer loop runs forever:

- The target's debug serial is buffered continuously - including during
  Wi-Fi accesses - and sent to the server as the encrypted request
  payload.  It appears on the server's `keys/from_<id>` FIFO.
- The decrypted reply (drained from `keys/to_<id>`, up to ~2 KB per
  exchange) is fed to the same Intel HEX parser as the CDC/UART
  versions: non-HEX bytes are echoed to the target UART, HEX records
  program the target via ICSP with mid-stream flushing.
- If both the send buffer and the previous exchange were empty, the
  next server access waits 2 s (aborted early as soon as target data
  arrives); otherwise it happens immediately, so queued HEX data drains
  at full rate.

To program a target, write a HEX file into the receiver's FIFO:

```sh
cat test.hex > keys/to_<id>
```

The `mclr`/`run`/`writing`/`IDCODE:xxxxxxxx` status messages that the
CDC/UART versions print to the host appear in `keys/from_<id>`,
interleaved with the target's debug output.

Note: re-programming the *writer* itself erases its nonce-counter flash
pages, so its nonces restart near zero while the server's
`keys/<id>.state` still holds the previous session's (higher) nonce.
The receiver then rejects every request (HTTP 200 with an empty body)
as a replay.  After re-flashing a writer that keeps its key, delete the
server-side `keys/<id>.state` file - or pair a fresh key by barcode.

## ICSP protocol

Uses the 2-wire Enhanced ICSP defined in DS60001145
(PIC32 Flash Programming Specification), 4-phase clock mode,
serial execution without a Programming Executive (PE).

## Authors

Developed by paijp in collaboration with Anthropic's Claude (Sonnet 4.6,
Opus 4.8, and Fable 5).
The requirements, hardware bring-up, and final design decisions were made
by paijp; Claude drafted and iterated on the source code based on that
feedback. All testing was performed on real hardware.

## Related projects

- [sergev/pic32prog](https://github.com/sergev/pic32prog) (GPL v2) --
  A full-featured PIC32 programmer. Referenced for ICSP protocol details
  (MTAP command register width, serial execution sequence).
- [paijp/pic32mx-usb-minimal](https://github.com/paijp/pic32mx-usb-minimal)
  (Apache 2.0) -- Minimal USB CDC driver for PIC32MX270F256B. The USB CDC
  implementation in `writer32mxcdc.c` and the USB host in
  `writer32mx-wroom-c20p1305-barcodehid.c` are taken from this repository.
- [paijp/single-file-chacha20poly1305](https://github.com/paijp/single-file-chacha20poly1305)
  (MIT / public domain) -- `c20p1305.h` and the WROOM-02 pairing /
  transport scheme used by the Wi-Fi versions come from this repository.

## License

Apache License 2.0
