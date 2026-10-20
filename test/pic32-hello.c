/* writer32mx web test: print a counter on UTX2 (RB0) at 115200, echo U2RX (RB1). FRC+PLL 40 MHz. */
#include <xc.h>
#pragma config FNOSC = FRCPLL, FPLLIDIV = DIV_2, FPLLMUL = MUL_20, FPLLODIV = DIV_2, FPBDIV = DIV_1
#pragma config POSCMOD = OFF, FSOSCEN = OFF, FWDTEN = OFF, JTAGEN = OFF, ICESEL = ICS_PGx2
#pragma config FUSBIDIO = OFF, FVBUSONIO = OFF, OSCIOFNC = OFF, IESO = OFF, FCKSM = CSDCMD
#pragma config PMDL1WAY = OFF, IOL1WAY = OFF, DEBUG = OFF, PWP = OFF, BWP = OFF, CP = OFF

static void putc2(char c) { while (U2STAbits.UTXBF) ; U2TXREG = c; }
static void puts2(const char *s) { while (*s) putc2(*s++); }
static void putu(unsigned v) { char b[11]; int i = 10; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); puts2(b + i); }

int main(void)
{
	unsigned n = 0, t;
	ANSELA = 0; ANSELB = 0;
	RPB0R = 2;		/* U2TX on RB0 */
	U2RXR = 2;		/* U2RX on RB1 */
	U2BRG = 86;		/* 40 MHz / 4 / 87 = 114943 */
	U2MODE = 0x8008;	/* on, BRGH */
	U2STA = 0x1400;		/* rx, tx enable */
	puts2("\r\nhello from writer32mx web test\r\n");
	for (;;) {
		puts2("count "); putu(n++); puts2("\r\n");
		t = _CP0_GET_COUNT();
		while (_CP0_GET_COUNT() - t < 20000000) {	/* 1 s (core timer = 20 MHz) */
			if (U2STAbits.OERR) U2STAbits.OERR = 0;
			if (U2STAbits.URXDA) { char c = U2RXREG; puts2("echo: "); putc2(c); puts2("\r\n"); }
		}
	}
}
