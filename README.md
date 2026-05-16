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

| Writer pin | Target pin | Signal |
|---|---|---|
| RB2 | RB10 (PGED2) | ICSP data |
| RA0 | RB11 (PGEC2) | ICSP clock |
| RB1 | MCLR | Reset control |

## Build

Build with MPLAB X IDE and the XC32 compiler.
Each version is a single self-contained source file
(`writer32mxuart.c` or `writer32mxcdc.c`).

## Usage

### UART version (writer32mxuart.c)

Send the HEX file to UART2 (RB10/RB11) at 115200 bps.

```sh
stty -F /dev/ttyUSB0 115200 raw -echo
cp test.hex /dev/ttyUSB0
```

### CDC version (writer32mxcdc.c)

Connect the writer to a PC via USB. It appears as `/dev/ttyACM0`.

```sh
# Terminal 1: monitor output
stty -F /dev/ttyACM0 raw -echo
cat /dev/ttyACM0

# Terminal 2: send HEX file
cp test.hex /dev/ttyACM0
```

When programming is complete the target resets and starts the new program.

## ICSP protocol

Uses the 2-wire Enhanced ICSP defined in DS60001145
(PIC32 Flash Programming Specification), 4-phase clock mode,
serial execution without a Programming Executive (PE).

## License

Apache License 2.0
