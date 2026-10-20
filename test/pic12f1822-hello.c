/* PIC12F1822 test v3: 32 MHz INTOSC, EUSART TX=RA0 RX=RA1, 115200.
 * Waits for the PLL before sending; "v3 0xNNNN" every 0.25 s; echoes lower-cased. */
#include <xc.h>
#pragma config FOSC = INTOSC, WDTE = OFF, PWRTE = ON, MCLRE = ON, CP = OFF, CPD = OFF
#pragma config BOREN = ON, CLKOUTEN = OFF, IESO = OFF, FCMEN = OFF
#pragma config WRT = OFF, PLLEN = ON, STVREN = ON, BORV = LO, LVP = ON
#define _XTAL_FREQ 32000000

static char rxq[16];
static unsigned char rxw, rxr;

static void poll_rx(void)
{
	if (OERR) { CREN = 0; CREN = 1; }
	while (RCIF) {
		rxq[rxw] = RCREG;
		rxw = (rxw + 1) & 15;
	}
}
static void putch2(char c) { while (!TXIF) poll_rx(); TXREG = c; }
static void puts2(const char *s) { while (*s) putch2(*s++); }

static void puthex(unsigned v)
{
	signed char i;
	for (i = 12; i >= 0; i -= 4)
		putch2("0123456789abcdef"[(v >> i) & 15]);
}

void main(void)
{
	unsigned n = 0, t;
	char c;
	OSCCON = 0xF0;		/* 8 MHz x4 PLL */
	while (!PLLR || !HFIOFS) ;	/* PLL locked, HFINTOSC stable */
	ANSELA = 0;
	TRISA = 0x3E;
	BAUDCON = 0x08;
	SPBRGH = 0; SPBRGL = 68;
	TXSTA = 0x24;
	RCSTA = 0x90;
	__delay_ms(20);
	puts2("\r\npic12f1822 test v3\r\n");
	for (;;) {
		puts2("v3 0x"); puthex(n++); puts2("\r\n");
		for (t = 0; t < 20000; t++) {	/* ~0.25 s, RX polled every ~12 us */
			__delay_us(10);
			poll_rx();
			while (rxr != rxw) {
				c = rxq[rxr];
				rxr = (rxr + 1) & 15;
				if (c >= 'A' && c <= 'Z')
					c += 32;
				puts2("echo v3: "); putch2(c); puts2("\r\n");
			}
		}
	}
}
