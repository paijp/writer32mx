# writer32mx

A PIC32MX270F256B-to-PIC32MX270F256B flash programmer via 2-wire Enhanced ICSP.
Both the writer and the target use PIC32MX270F256B.

## Files

| File | Description |
|---|---|
| `writer32mxuart.c` | Receives Intel HEX via UART2 and programs the target |
| `writer32mxcdc.c` | Receives Intel HEX via USB CDC and programs the target |

## Hardware

### Requirements

- **writer**: PIC32MX270F256B
- **target**: PIC32MX270F256B
- **Crystal**: 4 MHz (required for USB; CDC version only)

### Pin connections

| Writer pin | Target pin | ICSP | Debug serial 115200 8N1 |
|---|---|---|---|
| RB2 | RB10 (PGED2 / UTX2) | ICSP data | <- target TX |
| RA0 | RB11 (PGEC2 / URX2) | ICSP clock | -> target RX |
| RB1 | MCLR | Reset control | |

## Build

Build with MPLAB X IDE and the XC32 compiler.
Each version is a single self-contained source file
(`writer32mxuart.c` or `writer32mxcdc.c`).

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

## ICSP protocol

Uses the 2-wire Enhanced ICSP defined in DS60001145
(PIC32 Flash Programming Specification), 4-phase clock mode,
serial execution without a Programming Executive (PE).

## License

Apache License 2.0
