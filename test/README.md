# test/ - target programs used to check the writers

Both print a counter at 115200 8N1 and echo what they receive, so a
write can be checked end to end through the writer's serial passthrough
(e.g. with the browser page in web/).

| File | Target | Serial pins | Output |
|---|---|---|---|
| `pic32-hello.c` | PIC32MX270F256B, FRC+PLL 40 MHz | U2TX=RB0, U2RX=RB1 | `count N` every 1 s, `echo: x` |
| `pic12f1822-hello.c` | PIC12F1822, INTOSC+PLL 32 MHz, LVP on | TX=RA0 (ICSPDAT), RX=RA1 (ICSPCLK) | `v3 0xNNNN` every 0.25 s, `echo v3: x` (lower-cased) |

Build:

```sh
# PIC32 (ghcr.io/paijp/xc32:v1.42)
xc32-gcc -mprocessor=32MX270F256B -O1 -o pic32-hello.elf pic32-hello.c && xc32-bin2hex pic32-hello.elf

# PIC12 (XC8 v1.38, see Containerfile.xc8)
mkdir -p out && xc8 --chip=12F1822 --outdir=out -Opic12f1822-hello pic12f1822-hello.c
```

With the PIC12F1822 wired as in the top-level README, its EUSART reaches
the host through the writer after programming.
