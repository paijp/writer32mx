/*
 * writer32mx-wroom-c20p1305-barcodehid - Wi-Fi ICSP flash programmer
 * https://github.com/paijp/writer32mx
 *
 * Same as writer32mx-wroom-c20p1305-barcodeuart.c, but the pairing
 * barcodes are read from a USB-HID barcode reader in keyboard mode
 * during the 10 s window after boot.  main() is the single-file USB
 * host from pic32mx-usb-minimal (dev branch, host/usbhost0015.c,
 * Apache-2.0), which enumerates a HID boot keyboard; keystrokes are
 * decoded through both US and JIS layouts in parallel and dispatched
 * per line to the WIFI: / C20P: parsers.  Once the window closes the
 * Wi-Fi connect and the writer loop run inside the usb_polltask hook
 * and never return - USB is no longer serviced, which is fine because
 * barcode scanning is over.
 *
 * See writer32mx-wroom-c20p1305-barcodeuart.c for the writer-loop /
 * protocol description.
 *
 * USB host portion:
 * Copyright (c) 2026 paijp - Apache License 2.0
 *   https://github.com/paijp/pic32mx-usb-minimal
 *
 * Developed by paijp in collaboration with Anthropic's Claude.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <xc.h>
#include <sys/attribs.h>
#include <stdint.h>

#include "c20p1305.h"

typedef volatile signed char _B;
typedef volatile unsigned char _UB;
typedef volatile short _H;
typedef volatile unsigned short _UH;
typedef volatile int _W;
typedef volatile unsigned int _UW;


/* Configuration bits (usbhost0015.c set; USB needs the 48 MHz UPLL) */
#pragma config FPLLIDIV = DIV_1
#pragma config FPLLMUL  = MUL_20
#pragma config FPLLODIV = DIV_2
#pragma config FNOSC    = PRIPLL
#pragma config POSCMOD  = XT
#pragma config FSOSCEN  = OFF
#pragma config UPLLIDIV = DIV_1
#pragma config UPLLEN   = ON
#pragma config FPBDIV   = DIV_1
#pragma config FWDTEN   = OFF
#pragma config JTAGEN   = OFF
/* CP=ON: the flash pages hold Wi-Fi credentials and the ChaCha20 key. */
#pragma config CP       = ON


/*jp.pa-i.cir/map32mx2-28
 * Pin assignments:
 *   USB host:  D+/D- on RB10/RB11
 *   WROOM-02:  UTX1=RPB15 (P7), U1RX=RPB13 (P8)
 *   Target (same harness as writer32mxcdc/uart):
 *     Writer RB2 (P6) -> Target RB10 (PGED2) : ICSP data / target debug TX
 *     Writer RA0 (P2) -> Target RB11 (PGEC2) : ICSP clock / writer -> target
 *     Writer RA1 (P3) -> Target MCLR         : reset control
 *   Local debug log: UTX2 on RPB9 (P10) - RB10/RB11 belong to USB here.
 *
 * UART1 sharing: the WROOM pins and the target's debug line are all
 * UART1-capable pins (RB2 cannot map to U2RX), so U1RX listens to the
 * target's debug TX between server exchanges and to the WROOM during
 * them - target bytes emitted while an exchange is in flight are lost.
 * U1TX is remapped from RPB15 to RPA0 only for the moments echo bytes
 * are sent to the target.
 *
 * The target runs its debug serial as UTX2 on RPB10 (PGED2) and may
 * receive on URX2/RPB11 (PGEC2) - the writer32mxcdc/uart conventions.
 */


/* ---- Pin definitions for target connection ---- */

/* ICSP pins (directly driving target PGEC2/PGED2) */
#define LAT_PGC0 LATAbits.LATA0
#define LAT_MCLR0 LATAbits.LATA1
#define PORT_PGD0 PORTBbits.RB2
#define LAT_PGD0 LATBbits.LATB2
#define TRIS_PGD0 TRISBbits.TRISB2


/* ---- Runtime configuration (barcode -> flash -> RAM) ---- */

#define	SSID_MAX	32
#define	PASS_MAX	64
static	UB	stored_ssid[SSID_MAX + 1] = {0};
static	UB	stored_pass[PASS_MAX + 1] = {0};

#define	URL_MAX		224
static	UB	stored_key[32] = {0};
static	UB	stored_url[URL_MAX + 1] = {0};
static	UB	stored_host[64] = {0};
static	const	UB	*stored_path = (const UB*)"";

/* Built once after Wi-Fi associates, then sent each request iteration. */
static	UB	req[8 + URL_MAX] = {0};
static	UB	req2[64 + 64] = {0};
static	UB	cipstart[24 + 64] = {0};

static	UB	c20p1305nonce[12] = {0};
static	UW	c20p1305nvcounter = 0;
static	UW	c20p1305vcounter = 0;


/* ---- Buffer definitions (writer32mx core) ---- */

#define BUFFERSIZE 256
static UH p2cbuf[BUFFERSIZE];		/* server -> target echo */
static W p2cwpos = 0;
static W p2crpos = 0;

#define ULOGSIZE 4096			/* target/writer -> server payload */
static UB p2ubuf[ULOGSIZE];
static W p2uwpos = 0;
static W p2urpos = 0;
static W p2ucount = 0;

/* Decrypted server reply, consumed byte-wise by rsproc(). */
#define RECVBUF_SIZE 2048
static UB hostbuf[RECVBUF_SIZE];
static W hostlen = 0;
static W hostpos = 0;

/* Max log bytes per request: keeps the whole AT+CIPSEND under the
   ESP8266's 2048-byte single-send limit even with a long URL. */
#define LOGCHUNK 512

static W recverror = 0x8000;

#define BLOCKSIZE 0x400
#define ADDRHMASK 0xfffffc00
#define WRITEBUFSIZE 32
static struct writebuf_struct {
	UB d[BLOCKSIZE];
	UW addr;
	W size;
} writebuf[WRITEBUFSIZE];
static W writebufwsize = 0;

static W writing = 0;

static struct writebuf_struct *get_wp(UW addr);
static void writeflash(W isfinal);


/* ============================================================ */
/* Delay routines                                               */
/* ============================================================ */

static void pump(void);

static void wait1us(void)
{
	long l;
	for (l = 15; l > 0; l--)
		asm("nop");
}

static void wait100us(void)
{
	long l;

	pump();
	for (l = 1500; l > 0; l--)
		asm("nop");
}

static void dly_tsk(W ms)
{
	ms *= 10;
	while (ms-- > 0)
		wait100us();
}

static void wait1ms(void)
{
	dly_tsk(1);
}

static void wait200ms(void)
{
	dly_tsk(200);
}


/* ============================================================ */
/* Local debug log (UTX2 on RPB9)                               */
/* ============================================================ */

static	void	lcdtp_sendlogc(W c)
{
	while ((U2STAbits.UTXBF))
		pump();
	U2TXREG = c;
}


static	void	lcdtp_sendlogs(const char *s)
{
	W	c;

	while ((c = *(s++)))
		lcdtp_sendlogc(c);
}


static	void	lcdtp_sendlogun(UW v)
{
	static	const	char *bin2hex = "0123456789abcdef";

	lcdtp_sendlogc(bin2hex[v & 0xf]);
}


static	void	lcdtp_sendlogub(UW v)
{
	lcdtp_sendlogun(v >> 4);
	lcdtp_sendlogun(v);
}


static	void	lcdtp_sendloguw(UW v)
{
	lcdtp_sendlogun(v >> 28);
	lcdtp_sendlogun(v >> 24);
	lcdtp_sendlogun(v >> 20);
	lcdtp_sendlogun(v >> 16);
	lcdtp_sendlogun(v >> 12);
	lcdtp_sendlogun(v >> 8);
	lcdtp_sendlogun(v >> 4);
	lcdtp_sendlogun(v);
}


/* ============================================================ */
/* Serial buffer routines                                       */
/* ============================================================ */

static void p2cdata(UB c)
{
	p2cbuf[p2cwpos++] = c;
	if (p2cwpos >= BUFFERSIZE)
		p2cwpos = 0;
}

/* Queue a byte for the next server payload; full buffer drops the
   byte (the writer keeps running, log data is best effort). */
static void p2udata(UB c)
{
	if (p2ucount >= ULOGSIZE) {
		recverror |= 8;
		return;
	}
	p2ubuf[p2uwpos++] = c;
	if (p2uwpos >= ULOGSIZE)
		p2uwpos = 0;
	p2ucount++;
}

static void p2ustr(const UB *s)
{
	UB c;
	while ((c = *(s++)))
		p2udata(c);
}

static void p2uuw(UW v)
{
	static const UB hex[] = "0123456789abcdef";
	int i;
	for (i = 28; i >= 0; i -= 4)
		p2udata(hex[(v >> i) & 0xf]);
}


/* ============================================================ */
/* UART1 source switching + pump                                */
/*                                                              */
/* The WROOM (RPB15/RPB13) and the target's debug line (RB2,    */
/* PGED2) all sit on UART1-capable pins, so UART1 is shared:    */
/* between server exchanges U1RX listens to the target, during  */
/* an exchange it listens to the WROOM (target bytes emitted    */
/* meanwhile are lost).  Echo bytes for the target briefly      */
/* steal U1TX onto RPA0 (PGEC2); the WROOM link is idle then.   */
/* ============================================================ */

static	W	u1target = 0;	/* 1: U1RX is on RB2 capturing target debug */

static void target_uart_on(void)
{
	W c;

	if ((u1target) || (writing))
		return;
	U1RXR = 4;	/* RB2 <- target UTX2 (PGED2) */
	U1STA = 0x1400;
	while ((U1STAbits.URXDA))
		c = U1RXREG;
	(void)c;
	u1target = 1;
}

static void target_uart_off(void)
{
	if (!u1target)
		return;
	if ((U1STAbits.OERR))
		U1STA = 0x1400;
	while ((U1STAbits.URXDA))
		p2udata(U1RXREG);
	U1RXR = 3;	/* RPB13 <- WROOM */
	U1STA = 0x1400;
	u1target = 0;
}

static void pump(void)
{
	/* Echo bytes for the target: steal U1TX (RPA0/PGEC2) briefly.
	   Nothing is in flight on the WROOM link between exchanges. */
	if (!writing && u1target && (p2cwpos != p2crpos)) {
		RPB15R = 0;	/* WROOM TX pin to GPIO (idle high) */
		RPA0R = 1;	/* UTX1 -> target URX2 (PGEC2) */
		while (p2cwpos != p2crpos) {
			while ((U1STAbits.UTXBF))
				;
			U1TXREG = p2cbuf[p2crpos++];
			if (p2crpos >= BUFFERSIZE)
				p2crpos = 0;
		}
		while (U1STAbits.TRMT == 0)
			;
		RPA0R = 0;	/* PGC pin back to GPIO (idle high) */
		RPB15R = 1;	/* UTX1 back to the WROOM */
	}

	if ((writing) || !u1target)
		return;

	if ((U1STAbits.OERR))
		U1STA = 0x1400;
	while ((U1STAbits.URXDA)) {
		W c;
		c = U1RXREG;
		p2udata(c);
	}
}


/* Print "label=0xVALUE\r\n" */
static void dbg_reg(const UB *label, UW val)
{
#if 0
	p2ustr(label);
	p2udata('=');
	p2uuw(val);
	p2ustr("\r\n");
	pump();
#endif
}


/* ============================================================ */
/* WROOM-02 (UART1) helpers                                     */
/* ============================================================ */

static	W	c4wroom(W tmout)
{
	W	c;

	if (tmout > 0) {
		TMR2 = 0;
		IFS0bits.T2IF = 0;
	}
	for (;;) {
		pump();
		if ((U1STAbits.OERR))
			U1STA = 0x1400;
		if ((U1STAbits.URXDA)) {
			c = U1RXREG;
			return c;
		}
		if (tmout == 0)
			return -1;
		if ((tmout > 0)&&(IFS0bits.T2IF)) {
			IFS0bits.T2IF = 0;
			tmout--;
		}
	}
}


static	void	wroom4c(UB c)
{
	do {
		pump();
		if ((U1STAbits.OERR))
			U1STA = 0x1400;
	} while ((U1STAbits.UTXBF));
	U1TXREG = c;
}


static	W	wroom4cmd(const UB *send, const UB *recv, W tmout)
{
	W	c;
	const	UB	*p;

	lcdtp_sendlogs("\nwroom<");
	lcdtp_sendlogs(send);

	while ((U1STAbits.URXDA)) {
		if ((U1STAbits.OERR))
			U1STA = 0x1400;
		c = U1RXREG;
	}

	p = send;
	while ((c = *(p++)))
		wroom4c(c);

	if (recv == NULL)
		return 0;
	lcdtp_sendlogs("wroom>");
	p = recv;
	TMR2 = 0;
	IFS0bits.T2IF = 0;
	for (;;) {
		if ((IFS0bits.T2IF)) {
			IFS0bits.T2IF = 0;
			if (tmout > 0)
				tmout--;
		}
		if ((c = c4wroom(0)) < 0) {
			if (tmout == 0)
				return -1;
			continue;
		}
		lcdtp_sendlogc(c);
		if (c != *p) {
			p = recv;
			continue;
		}
		if (*(++p) == 0)
			break;
	}
	lcdtp_sendlogs("\n");
	return 0;
}


static	W	wroom4ub(W c)
{
	static	const	UB	*bin2hex = "0123456789abcdef";

	static	UB	s[3] = {'0', '0', 0};

	s[0] = bin2hex[(c >> 4) & 0xf];
	s[1] = bin2hex[c & 0xf];
	wroom4cmd(s, NULL, -1);
	return 1;
}


/*
	Byte-stream layer over the WROOM's active-mode TCP output: strips the
	"+IPD,<len>:" framing so a response spanning several TCP segments reads
	as one continuous stream. Returns the next payload byte, or -1 once the
	connection reports CLOSED before another data frame. Call ipd_reset()
	before each response.
*/
static	W	ipd_remain = 0;

static	void	ipd_reset(void)
{
	ipd_remain = 0;
}

static	W	ipd_getc(void)
{
	static	const	UB	hdr[] = "+IPD,";
	static	const	UB	closed[] = "CLOSED";
	const	UB	*p = hdr;
	const	UB	*q = closed;
	W	c, len;

	if (ipd_remain <= 0) {
		for (;;) {
			c = c4wroom(-1);
			lcdtp_sendlogc(c);
			if (c == *p) {
				if (*(++p) == 0)
					break;
			} else
				p = (c == hdr[0]) ? hdr + 1 : hdr;
			if (c == *q) {
				if (*(++q) == 0)
					return -1;
			} else
				q = (c == closed[0]) ? closed + 1 : closed;
		}
		len = 0;
		for (;;) {
			c = c4wroom(-1);
			lcdtp_sendlogc(c);
			if (c == ':')
				break;
			if (c >= '0' && c <= '9')
				len = len * 10 + (c - '0');
		}
		if (len <= 0)
			return -1;
		ipd_remain = len;
	}
	ipd_remain--;
	c = c4wroom(-1);
	lcdtp_sendlogc(c);
	return c;
}


/* ============================================================ */
/* Config flash storage (from wroomc20p1305 samples)            */
/* ============================================================ */

#define	FLASHPAGEWORDS	(1024 / sizeof(UW))

/*
	Two pages so a power loss mid-update still leaves one valid copy:
	always write the unselected page first, then the other. Each page
	stores the counter in its first half with a bit-inverted copy in the
	second half (see checkflashpage / writeflashpage).
*/
static	const	UW	flashpage0[FLASHPAGEWORDS] __attribute__((aligned(1024))) = {0};
static	const	UW	flashpage1[FLASHPAGEWORDS] __attribute__((aligned(1024))) = {0};

/* One page each for Wi-Fi and ChaCha20+URL: a barcode can be rescanned if a
   write is interrupted, so no need for a redundant backup page. */
static	const	UW	cfgpage_wifi[FLASHPAGEWORDS] __attribute__((aligned(1024))) = {0};
static	const	UW	cfgpage_app[FLASHPAGEWORDS]  __attribute__((aligned(1024))) = {0};


static	void	nvmunlock(UW cmd)
{
	NVMCON = cmd;
	while ((NVMCONbits.LVDSTAT))
		;
	NVMKEY = 0xaa996655;
	NVMKEY = 0x556699aa;
	NVMCONSET = 0x8000;
	while ((NVMCON & 0x8000))
		;
	NVMCONCLR = 0x4000;
	dly_tsk(1);
}


static	void	writeflashpage_data(const volatile UW *mem, const UW *src)
{
	W	i;

	nvmunlock(0x4000);
	dly_tsk(200);
	nvmunlock(0x4000);

	NVMADDR = ((UW)mem) & 0x1fffffff;
	nvmunlock(0x4004);
	for (i=0; i<FLASHPAGEWORDS / 2; i++) {
		NVMADDR = ((UW)(mem + i)) & 0x1fffffff;
		NVMDATA = src[i];
		nvmunlock(0x4001);
	}
	for (i=0; i<FLASHPAGEWORDS / 2; i++) {
		NVMADDR = ((UW)(mem + FLASHPAGEWORDS / 2 + i)) & 0x1fffffff;
		NVMDATA = src[i] ^ 0xffffffff;
		nvmunlock(0x4001);
	}
}


static	void	writeflashpage(const volatile UW *mem, UW v)
{
	static	UW	buf[FLASHPAGEWORDS / 2];
	W	i;

	buf[0] = v;
	for (i=1; i<FLASHPAGEWORDS / 2; i++)
		buf[i] = 0;
	writeflashpage_data(mem, buf);
}


/* Returns 0 if the page passes the bit-inversion redundancy check, -1 if
   not. Callers that want the first word read it from mem[0] themselves —
   returning it here would conflate "first word looks negative" with "page
   invalid" once a key with the high bit set lives there. */
static	W	checkflashpage(const volatile UW *mem)
{
	W	i;

	for (i=0; i<FLASHPAGEWORDS / 2; i++)
		if ((mem[i] ^ mem[i + FLASHPAGEWORDS / 2]) != 0xffffffff)
			return -1;
	return 0;
}


static	void	load_wifi_from_flash(void)
{
	const	volatile	UW	*mem = cfgpage_wifi;
	W	i;

	if (checkflashpage(mem) < 0) {
		stored_ssid[0] = 0;
		stored_pass[0] = 0;
		return;
	}
	for (i=0; i<SSID_MAX / 4; i++)
		((UW*)stored_ssid)[i] = mem[i];
	stored_ssid[SSID_MAX] = 0;
	for (i=0; i<PASS_MAX / 4; i++)
		((UW*)stored_pass)[i] = mem[SSID_MAX / 4 + i];
	stored_pass[PASS_MAX] = 0;
}


static	void	save_wifi_to_flash(void)
{
	static	UW	buf[FLASHPAGEWORDS / 2];
	UB	*bbuf = (UB*)buf;
	W	i;

	for (i=0; i<sizeof(buf); i++)
		bbuf[i] = 0;
	for (i=0; i<SSID_MAX && stored_ssid[i]; i++)
		bbuf[i] = stored_ssid[i];
	for (i=0; i<PASS_MAX && stored_pass[i]; i++)
		bbuf[SSID_MAX + i] = stored_pass[i];
	writeflashpage_data(cfgpage_wifi, buf);
}


/* Split stored_url (e.g. "http://host/path?...key0c20=") into stored_host
   and stored_path. Tolerates missing scheme and missing path. */
static	void	parse_stored_url(void)
{
	const	UB	*p = stored_url;
	W	i;

	stored_host[0] = 0;
	stored_path = (const UB*)"";
	if (p[0] == 'h' && p[1] == 't' && p[2] == 't' && p[3] == 'p') {
		p += 4;
		if (*p == 's') p++;
		if (p[0] == ':' && p[1] == '/' && p[2] == '/') p += 3;
	}
	i = 0;
	while (*p && *p != '/' && *p != ':' && i < (W)sizeof(stored_host) - 1)
		stored_host[i++] = *p++;
	stored_host[i] = 0;
	while (*p && *p != '/')
		p++;	/* skip optional :port */
	stored_path = p;
}


static	void	load_app_from_flash(void)
{
	const	volatile	UW	*mem = cfgpage_app;
	W	i;

	if (checkflashpage(mem) < 0) {
		stored_key[0] = 0;
		stored_url[0] = 0;
		parse_stored_url();
		return;
	}
	for (i=0; i<8; i++)
		((UW*)stored_key)[i] = mem[i];
	for (i=0; i<URL_MAX / 4; i++)
		((UW*)stored_url)[i] = mem[8 + i];
	stored_url[URL_MAX] = 0;
	parse_stored_url();
}


static	void	save_app_to_flash(void)
{
	static	UW	buf[FLASHPAGEWORDS / 2];
	UB	*bbuf = (UB*)buf;
	W	i;

	for (i=0; i<sizeof(buf); i++)
		bbuf[i] = 0;
	for (i=0; i<32; i++)
		bbuf[i] = stored_key[i];
	for (i=0; i<URL_MAX && stored_url[i]; i++)
		bbuf[32 + i] = stored_url[i];
	writeflashpage_data(cfgpage_app, buf);
}


/* ============================================================ */
/* Barcode parsing (from wroomc20p1305 samples)                 */
/* ============================================================ */

/* Hex digit -> nibble (0..15), or -1 on garbage. */
static	W	hex2nib(W c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 0xa;
	if (c >= 'A' && c <= 'F') return c - 'A' + 0xa;
	return -1;
}


/* Generic tag-prefixed field copier: advances *pp past the value (and its
   trailing ';'). out may be NULL to skip. '\\' escapes the next char. */
static	void	copy_field(const UB **pp, UB *out, W max)
{
	const	UB	*p = *pp;
	W	i = 0;

	while (*p && *p != ';') {
		if (*p == '\\' && p[1])
			p++;
		if (out && i < max)
			out[i++] = *p;
		p++;
	}
	if (out)
		out[i] = 0;
	if (*p == ';')
		p++;
	*pp = p;
}


/* Parse "WIFI:T:<auth>;S:<ssid>;P:<password>;[H:<true|false>];" into
   stored_ssid / stored_pass. Unknown tokens skipped. */
static	void	parse_wifi_barcode(const UB *s)
{
	const	UB	*p = s;
	UB	tag;

	if (p[0] != 'W' || p[1] != 'I' || p[2] != 'F' || p[3] != 'I' || p[4] != ':')
		return;
	p += 5;
	stored_ssid[0] = 0;
	stored_pass[0] = 0;
	while (*p && *p != ';') {
		tag = p[0];
		if (p[1] != ':') {
			copy_field(&p, NULL, 0);
			continue;
		}
		p += 2;
		switch (tag) {
		case 'S':	copy_field(&p, stored_ssid, SSID_MAX);	break;
		case 'P':	copy_field(&p, stored_pass, PASS_MAX);	break;
		default:	copy_field(&p, NULL, 0);		break;
		}
	}
}


/* Parse "C20P:K:<64-hex chars>;U:<url>;;" into stored_key / stored_url.
   K: must be exactly 64 hex chars (32 bytes); malformed → key cleared. */
static	void	parse_c20p_barcode(const UB *s)
{
	const	UB	*p = s;
	UB	tag;
	W	i, hi, lo;

	if (p[0] != 'C' || p[1] != '2' || p[2] != '0' || p[3] != 'P' || p[4] != ':')
		return;
	p += 5;
	for (i=0; i<32; i++) stored_key[i] = 0;
	stored_url[0] = 0;
	while (*p && *p != ';') {
		tag = p[0];
		if (p[1] != ':') {
			copy_field(&p, NULL, 0);
			continue;
		}
		p += 2;
		switch (tag) {
		case 'K':
			for (i=0; i<32; i++) {
				if ((hi = hex2nib(*p)) < 0) break;
				p++;
				if ((lo = hex2nib(*p)) < 0) break;
				p++;
				stored_key[i] = (hi << 4) | lo;
			}
			copy_field(&p, NULL, 0);	/* skip rest until ';' */
			break;
		case 'U':
			copy_field(&p, stored_url, URL_MAX);
			break;
		default:
			copy_field(&p, NULL, 0);
			break;
		}
	}
	parse_stored_url();
}


#define	BARCODE_WINDOW_MS	10000

static	W	memsame(const UB *a, const UB *b, W len)
{
	while (len-- > 0)
		if (*(a++) != *(b++))
			return 0;
	return 1;
}


/* A reader may deliver the same scan several times in one window; comparing
   the parsed values against what's already stored avoids rewriting (and
   wearing) the flash page for every repeat. */
static	void	barcode_line(const UB *line)
{
	static	UB	prev_ssid[SSID_MAX + 1], prev_pass[PASS_MAX + 1];
	static	UB	prev_key[32], prev_url[URL_MAX + 1];
	W	i;

	if (line[0] == 0)
		return;
	lcdtp_sendlogs("barcode: ");
	lcdtp_sendlogs(line);
	lcdtp_sendlogs("\n");
	if (line[0] == 'W' && line[1] == 'I' && line[2] == 'F' && line[3] == 'I'
	    && line[4] == ':') {
		for (i=0; i<(W)sizeof(prev_ssid); i++) prev_ssid[i] = stored_ssid[i];
		for (i=0; i<(W)sizeof(prev_pass); i++) prev_pass[i] = stored_pass[i];
		parse_wifi_barcode(line);
		if (memsame(prev_ssid, stored_ssid, sizeof(prev_ssid))
		    && memsame(prev_pass, stored_pass, sizeof(prev_pass))) {
			lcdtp_sendlogs("wifi unchanged.\n");
			return;
		}
		lcdtp_sendlogs("ssid=");
		lcdtp_sendlogs(stored_ssid);
		lcdtp_sendlogs("\n");
		save_wifi_to_flash();
		lcdtp_sendlogs("wifi saved.\n");
	} else if (line[0] == 'C' && line[1] == '2' && line[2] == '0'
	    && line[3] == 'P' && line[4] == ':') {
		for (i=0; i<(W)sizeof(prev_key); i++) prev_key[i] = stored_key[i];
		for (i=0; i<(W)sizeof(prev_url); i++) prev_url[i] = stored_url[i];
		parse_c20p_barcode(line);
		if (memsame(prev_key, stored_key, sizeof(prev_key))
		    && memsame(prev_url, stored_url, sizeof(prev_url))) {
			lcdtp_sendlogs("app unchanged.\n");
			return;
		}
		lcdtp_sendlogs("url=");
		lcdtp_sendlogs(stored_url);
		lcdtp_sendlogs("\n");
		save_app_to_flash();
		lcdtp_sendlogs("app saved.\n");
	} else
		lcdtp_sendlogs("(unknown prefix, ignored)\n");
}

/* ============================================================ */
/* USB-HID barcode capture (scan window)                        */
/* ============================================================ */

static	W	window_ms_left = BARCODE_WINDOW_MS;
static	W	app_done = 0;
static	W	in_polltask = 0;


static	W	barcode_prefix_ok(const UB *s)
{
	if (s[0] == 'W' && s[1] == 'I' && s[2] == 'F' && s[3] == 'I' && s[4] == ':')
		return 1;
	if (s[0] == 'C' && s[1] == '2' && s[2] == '0' && s[3] == 'P' && s[4] == ':')
		return 1;
	return 0;
}


/* Collect HID keystrokes into one line, decoded through BOTH keyboard
   layouts in parallel.  The completed line is a barcode: the
   compile-time default layout's decode is tried first and the
   alternate layout's used as fallback (its prefix check makes the
   choice unambiguous - auto-detection without guessing single
   characters). */
static	void	barcode_char(UB c_def, UB c_alt)
{
	static	UB	linebuf[256];		/* default layout */
	static	UB	linealt[256];		/* alternate layout */
	static	W	pos = 0;
	static	W	posa = 0;

	if (c_def == '\r' || c_def == '\n' || c_alt == '\r' || c_alt == '\n') {
		linebuf[pos] = 0;
		linealt[posa] = 0;
		if (barcode_prefix_ok(linebuf) || !barcode_prefix_ok(linealt))
			barcode_line(linebuf);
		else {
			lcdtp_sendlogs("(alternate keyboard layout)\n");
			barcode_line(linealt);
		}
		pos = 0;
		posa = 0;
		return;
	}
	if (c_def && pos < (W)sizeof(linebuf) - 1)
		linebuf[pos++] = c_def;
	if (c_alt && posa < (W)sizeof(linealt) - 1)
		linealt[posa++] = c_alt;
}


/* ============================================================ */
/* Encrypted server exchange (from wroomc20p1305-barcodehid)    */
/* ============================================================ */

/*
	One encrypted exchange with the server.

	The plaintext is "s<hex4>" (RECVBUF_SIZE, the decoded reply capacity
	advertised to the server) followed by a bare '=' that ends the
	transport parameters, then the raw payload bytes. The decrypted
	reply body is copied into rbuf; returns its length (0 = empty/none,
	-1 = nonce/MAC mismatch).
*/
static	W	send_request_raw(const UB *payload, W len, UB *rbuf, W rbufmax)
{
	static	const	UB	*bin2hex = "0123456789abcdef";
	static	UB	buf[] = "AT+CIPSEND=0000\r\n";
	UB	sparam[6];
	UB	*p;
	W	l, l2, sl;

	sparam[0] = 's';
	sparam[1] = bin2hex[(RECVBUF_SIZE >> 12) & 0xf];
	sparam[2] = bin2hex[(RECVBUF_SIZE >> 8) & 0xf];
	sparam[3] = bin2hex[(RECVBUF_SIZE >> 4) & 0xf];
	sparam[4] = bin2hex[RECVBUF_SIZE & 0xf];
	sparam[5] = '=';
	sl = 6;

	c20p1305vcounter += 2;
	c20p1305nonce[4] = c20p1305nvcounter >> 24;
	c20p1305nonce[5] = c20p1305nvcounter >> 16;
	c20p1305nonce[6] = c20p1305nvcounter >> 8;
	c20p1305nonce[7] = c20p1305nvcounter;
	c20p1305nonce[8] = c20p1305vcounter >> 24;
	c20p1305nonce[9] = c20p1305vcounter >> 16;
	c20p1305nonce[10] = c20p1305vcounter >> 8;
	c20p1305nonce[11] = c20p1305vcounter;

	wroom4cmd(cipstart, "OK", -1);
	dly_tsk(50);

	l = 0;
	while ((req[l]))
		l++;
	l2 = 0;
	while ((req2[l2]))
		l2++;
	l = l + l2 + 24 + (sl + len) * 2 + 32;
	p = buf;
	while (*(p++) != '=')
		;
	if (l >= 1000)
		*(p++) = bin2hex[((l / 1000) % 10) & 0xf];
	if (l >= 100)
		*(p++) = bin2hex[((l / 100) % 10) & 0xf];
	if (l >= 10)
		*(p++) = bin2hex[((l / 10) % 10) & 0xf];
	*(p++) = bin2hex[(l % 10) & 0xf];
	*(p++) = 0xd;
	*(p++) = 0xa;
	*(p++) = 0;
	wroom4cmd(buf, "OK", -1);

	wroom4cmd(req, NULL, -1);
	c20p1305_send(NULL, -1, stored_key, c20p1305nonce, wroom4ub);
	c20p1305_send(sparam, sl, stored_key, c20p1305nonce, wroom4ub);
	if (len > 0)	/* size==0 means "flush" to c20p1305_send */
		c20p1305_send((UB*)payload, len, stored_key, c20p1305nonce, wroom4ub);
	c20p1305_send(NULL, 0, stored_key, c20p1305nonce, wroom4ub);
	wroom4cmd(req2, NULL, -1);
	{
		static	UB	recvbuf[RECVBUF_SIZE];
		static	UB	mac[16];
		static	const	UB	marker[] = "key0c20=";
		const	UB	*mp;
		W	pos;
		W	c, upper;
		W	i;

		c20p1305nonce[11] |= 1;

		/* Hunt for "key0c20=" inside the de-framed +IPD stream, then read
		   hex until a non-hex byte or connection close. The response may
		   span several TCP segments; ipd_getc() hides the "+IPD,<n>:"
		   headers that would otherwise cut the hex short. */
		ipd_reset();
		mp = marker;
		for (;;) {
			if ((c = ipd_getc()) < 0)
				return 0;
			if (c == *mp) {
				if (*(++mp) == 0)
					break;
			} else
				mp = (c == marker[0]) ? marker + 1 : marker;
		}
		pos = 0;
		upper = -1;
		while (pos < (W)sizeof(recvbuf)) {
			if ((c = ipd_getc()) < 0)
				break;
			if ((c >= '0')&&(c <= '9'))
				c = c - '0';
			else if ((c >= 'A')&&(c <= 'F'))
				c = c - 'A' + 0xa;
			else if ((c >= 'a')&&(c <= 'f'))
				c = c - 'a' + 0xa;
			else
				break;
			if (upper < 0) {
				upper = c << 4;
				continue;
			}
			c |= upper;
			upper = -1;
			recvbuf[pos++] = c;
		}
		/* Drain the rest of the stream (trailing frame bytes and the
		   CLOSED notification) via ipd_getc, which returns -1 at CLOSED.
		   A separate blocking wait on the "CLOSED" marker would hang: by
		   now ipd_getc has already consumed it while framing. */
		while (c >= 0)
			c = ipd_getc();
		if (pos < 12 + 16)
			return 0;
		for (i=0; i<12; i++)
			if (recvbuf[i] != c20p1305nonce[i]) {
				p2ustr("nonce not match.\r\n");
				return -1;
			}
		c20p1305_mac(mac, NULL, 0, recvbuf + 12, pos - 12 - 16, stored_key, c20p1305nonce);
		for (i=0; i<(W)sizeof(mac); i++)
			if (recvbuf[pos - sizeof(mac) + i] != mac[i]) {
				p2ustr("mac not match.\r\n");
				return -1;
			}
		c20p1305_xor(recvbuf + 12, pos - 12 - 16, stored_key, c20p1305nonce);
		l = pos - 12 - 16;
		for (i=0; i<l && i<rbufmax; i++)
			rbuf[i] = recvbuf[12 + i];
		return (l < rbufmax) ? l : rbufmax;
	}
}


/* U1RX belongs to the WROOM for the duration of the exchange; the
   target's debug line is captured again as soon as it returns. */
static	W	send_request(const UB *payload, W len, UB *rbuf, W rbufmax)
{
	W	n;

	target_uart_off();
	n = send_request_raw(payload, len, rbuf, rbufmax);
	target_uart_on();
	return n;
}


/* ============================================================ */
/* 2-Wire Enhanced ICSP Implementation                          */
/* DS60001145 - PIC32 Flash Programming Specification           */
/*                                                              */
/* 2-wire ICSP uses PGECx (clock) and PGEDx (data).            */
/* In 4-phase mode: each JTAG bit takes 4 PGECx clocks         */
/*   phase 0: previous TDO                                     */
/*   phase 1: TDI                                               */
/*   phase 2: TMS                                               */
/*   phase 3: (next TDO captured internally)                    */
/* LSb first for data, MSb first for TMS in SetMode.            */
/* ============================================================ */

static inline void icsp_delay(void)
{
	asm("nop");
	asm("nop");
	asm("nop");
	asm("nop");
}

/*
 * icsp_clock_4phase: clock one JTAG bit using the 2-wire 4-phase protocol.
 *
 * This is the original working implementation that correctly reads IDCODE.
 * Do NOT change the phase order - it works as-is.
 *
 * Phase 1: TDI  - host drives PGEDx = tdi_val, clock HIGH/LOW
 * Phase 2: TMS  - host drives PGEDx = tms_val, clock HIGH/LOW
 * Phase 3: internal - host releases PGEDx (input), clock HIGH/LOW
 * Phase 4: TDO  - target drives PGEDx, host samples, clock HIGH/LOW
 *
 * Returns: TDO value captured in the PREVIOUS call's Phase 4 (pipeline delay).
 */
static W icsp_clock_4phase(W tdi_val, W tms_val)
{
	static W current_tdo = 0;
	W before_tdo;

	before_tdo = current_tdo;

	/* Phase 1: TDI */
	TRIS_PGD0 = 0;
	LAT_PGD0 = tdi_val ? 1 : 0;
	icsp_delay();
	LAT_PGC0 = 1;
	icsp_delay();
	LAT_PGC0 = 0;
	icsp_delay();

	/* Phase 2: TMS */
	LAT_PGD0 = tms_val ? 1 : 0;
	icsp_delay();
	LAT_PGC0 = 1;
	icsp_delay();
	LAT_PGC0 = 0;
	icsp_delay();

	/* Phase 3: release PGEDx for TDO */
	TRIS_PGD0 = 1;
	icsp_delay();
	LAT_PGC0 = 1;
	icsp_delay();
	LAT_PGC0 = 0;
	icsp_delay();

	/* Phase 4: sample TDO */
	icsp_delay();
	current_tdo = PORT_PGD0;
	LAT_PGC0 = 1;
	icsp_delay();
	LAT_PGC0 = 0;
	icsp_delay();

	return before_tdo;
}


/*
 * SetMode: clock 'nbits' of mode value into TMS (TDI=0, ignore TDO)
 * LSb of mode is sent first.
 */
static void icsp_SetMode(UW mode, W nbits)
{
	W i;
	for (i = 0; i < nbits; i++) {
		icsp_clock_4phase(0, (mode >> i) & 1);
	}
}


/*
 * SendCommand: send 5-bit TAP instruction
 * Assumes TAP is in Run-Test/Idle state.
 * Sequence: TMS 1,1,0,0 (RTI->Select-DR->Select-IR->Capture-IR->Shift-IR)
 * then 5 data bits (last with TMS=1), then TMS 1,0 (Exit1-IR->Update-IR->RTI)
 */
static void icsp_SendCommand(UB cmd)
{
	W i;

	/* TMS header: 1,1,0,0 (RTI -> Select-DR -> Select-IR -> Capture-IR -> Shift-IR) */
	icsp_clock_4phase(0, 1);
	icsp_clock_4phase(0, 1);
	icsp_clock_4phase(0, 0);
	icsp_clock_4phase(0, 0);

	/* Command bits 0..3 with TMS=0 (stay in Shift-IR) */
	for (i = 0; i < 4; i++) {
		icsp_clock_4phase((cmd >> i) & 1, 0);
	}
	/* Command bit 4 (MSb) with TMS=1 (Shift-IR -> Exit1-IR) */
	icsp_clock_4phase((cmd >> 4) & 1, 1);

	/* TMS footer: 1,0 (Exit1-IR -> Update-IR -> RTI) */
	icsp_clock_4phase(0, 1);
	icsp_clock_4phase(0, 0);
}


/*
 * icsp_XferData8: transfer 8-bit data in/out of MTAP command register.
 *
 * MTAP_COMMAND DR is 8 bits wide (not 32).
 * pic32prog uses bitbang_send(a, 0, 0, 8, cmd, read) for these transfers.
 *
 * Pipeline: icsp_clock_4phase returns the TDO captured in the PREVIOUS call.
 * The 3 header clocks (Select-DR, Capture-DR, Shift-DR) drain the pipeline,
 * so the call for bit[i] returns TDO[i] exactly.
 * The last data bit (bit7) uses TMS=1 to exit; its return value is TDO[6].
 * TDO[7] is captured on the first footer clock (Update-DR).
 */
static UW icsp_XferData8(UW idata)
{
	UW odata = 0;
	W i;

	/* RTI -> Select-DR-Scan -> Capture-DR -> Shift-DR */
	icsp_clock_4phase(0, 1);
	icsp_clock_4phase(0, 0);
	icsp_clock_4phase(0, 0);

	/* Bits 0..7: each call returns TDO[i] */
	for (i = 0; i < 8; i++) {
		W tdo = icsp_clock_4phase((idata >> i) & 1, (i == 7) ? 1 : 0);
		if (tdo)
			odata |= (1UL << i);
	}
	icsp_clock_4phase(0, 1);
	icsp_clock_4phase(0, 0);

	return odata;
}

/*
 * icsp_XferData: transfer 32-bit data in/out of the current DR.
 *
 * JTAG path (from RTI):
 *   TMS 1,0,0  -> Select-DR-Scan, Capture-DR, Shift-DR
 *   32 data bits LSb first (bit 31 with TMS=1) -> Exit1-DR
 *   TMS 1,0    -> Update-DR, RTI
 *
 * Same pipeline logic as XferData8: call for bit[i] returns TDO[i].
 * Bit 31 clock (TMS=1) returns TDO[30] — already stored.
 * TDO[31] comes on the footer Update-DR clock.
 */
static UW icsp_XferData(UW idata)
{
	UW odata = 0;
	W i;

	/* RTI -> Select-DR-Scan -> Capture-DR -> Shift-DR */
	icsp_clock_4phase(0, 1);
	icsp_clock_4phase(0, 0);
	icsp_clock_4phase(0, 0);

	/* Bits 0..31 with TMS=0: call for bit[i] returns TDO[i] */
	for (i = 0; i < 32; i++) {
		W tdo = icsp_clock_4phase((idata >> i) & 1, (i == 31) ? 1 : 0);
		if (tdo)
			odata |= (1UL << i);
	}
	icsp_clock_4phase(0, 1);
	icsp_clock_4phase(0, 0);

	return odata;
}


/*
 * XferFastData: fast 33-bit transfer (PrAcc + 32-bit data)
 * Used after SendCommand(ETAP_FASTDATA)
 */
static UW icsp_XferFastData(UW idata)
{
	UW odata = 0;
	W pracc;
	W i;

	do {
		/* TMS header: 1,0,0 */
		icsp_clock_4phase(0, 1);
		icsp_clock_4phase(0, 0);
		icsp_clock_4phase(0, 0);

		/* Read 33 bits (PrAcc + 32-bit data), LSb first */
		pracc = icsp_clock_4phase(0, 0);
		odata = 0;
		for (i = 0; i < 32; i++) {
			W tdo = icsp_clock_4phase((idata >> i) & 1, (i == 31) ? 1 : 0);
			if (tdo)
				odata |= (1UL << i);
		}

		/* TMS footer: 1,0 */
		icsp_clock_4phase(0, 1);
		icsp_clock_4phase(0, 0);
	} while (pracc == 0);

	return odata;
}


/*
 * XferInstruction: execute a MIPS instruction on target CPU
 */

/* TAP instruction codes */
#define MTAP_SW_MTAP 0x04
#define MTAP_SW_ETAP 0x05
#define MTAP_COMMAND 0x07
#define ETAP_ADDRESS 0x08
#define ETAP_DATA 0x09
#define ETAP_CONTROL 0x0A
#define ETAP_EJTAGBOOT 0x0C
#define ETAP_FASTDATA 0x0E

/* MTAP_COMMAND DR commands */
#define MCHP_STATUS 0x00
#define MCHP_ASSERT_RST 0xD1
#define MCHP_DE_ASSERT_RST 0xD0
#define MCHP_ERASE 0xFC
#define MCHP_FLASH_EN 0xFE
#define MCHP_FLASH_DIS 0xFD

/* Status bits */
#define STAT_CPS 0x80 /* NOT code-protected */
#define STAT_NVMERR 0x20
#define STAT_CFGRDY 0x08
#define STAT_FCBUSY 0x04
#define STAT_FAEN 0x02 /* Flash access enabled */
#define STAT_DEVRST 0x01 /* Device reset active */


static W icsp_XferInstruction(UW instruction)
{
	UW controlVal;
	W timeout = 10000;

	/* Select Control Register */
	icsp_SendCommand(ETAP_CONTROL);

	/*
	 * Poll with 0x0004C000 (PrAcc + ProbEn + ProbTrap)
	 * matching pic32prog implementation.
	 */
	do {
		controlVal = icsp_XferData(0x0004C000);
		if (--timeout <= 0) {
			p2ustr("XI:timeout ");
			p2uuw(instruction);
			p2ustr(" ctl=");
			p2uuw(controlVal);
			p2ustr("\r\n");
			return -1;
		}
	} while (!(controlVal & (1UL << 18)));

	/* Select Data Register and send instruction */
	icsp_SendCommand(ETAP_DATA);
	icsp_XferData(instruction);

	/* Execute: clear PrAcc */
	icsp_SendCommand(ETAP_CONTROL);
	icsp_XferData(0x0000C000);
	return 0;
}


/*
 * icsp_read_word: read a 32-bit word from target memory.
 * Matches pic32prog bitbang_read_word method:
 *   1. Set s3 = 0xFF200000 (DMSEG FastData address)
 *   2. Load address, LW, SW to FastData via s3
 *   3. Select ETAP_FASTDATA, read 33 bits, shift right 1 (discard PrAcc)
 */
static UW icsp_read_word(UW virt_addr)
{
	UW addr_hi = (virt_addr >> 16) & 0xffff;
	UW addr_lo = virt_addr & 0xffff;

	icsp_XferInstruction(0x3c13ff20); /* lui s3, 0xFF20 */

	/* Load address and read memory */
	icsp_XferInstruction(0x3c080000 | addr_hi); /* lui t0, addr_hi */
	icsp_XferInstruction(0x35080000 | addr_lo); /* ori t0, t0, addr_lo */
	icsp_XferInstruction(0x8d090000); /* lw t1, 0(t0) */
	icsp_XferInstruction(0xae690000); /* sw t1, 0(s3) */
	icsp_XferInstruction(0); /* nop */

	/* Select FASTDATA register and read 33 bits */
	icsp_SendCommand(ETAP_FASTDATA);

	return icsp_XferFastData(0);
}


static void icsp_write_word(UW virt_addr, UW data)
{
	UW addr_hi = (virt_addr >> 16) & 0xffff;
	UW addr_lo = virt_addr & 0xffff;
	UW data_hi = (data >> 16) & 0xffff;
	UW data_lo = data & 0xffff;

	icsp_XferInstruction(0x3c13ff20); /* lui s3, 0xFF20 */

	/* Load address and read memory */
	icsp_XferInstruction(0x3c080000 | addr_hi); /* lui t0, addr_hi */
	icsp_XferInstruction(0x35080000 | addr_lo); /* ori t0, t0, addr_lo */
	icsp_XferInstruction(0x3c090000 | data_hi); /* lui t1, addr_hi */
	icsp_XferInstruction(0x35290000 | data_lo); /* ori t1, t1, addr_lo */
	icsp_XferInstruction(0xad090000); /* sw t1, 0(t0) */
	icsp_XferInstruction(0); /* nop */
}


/* ============================================================ */
/* ICSP high-level operations                                   */
/* ============================================================ */

/*
 * Enter 2-wire Enhanced ICSP mode
 * Key sequence: 0x4D434850 ("MCHP" in ASCII), MSb first
 */
static void icsp_enter(void)
{
	UW key = 0x4D434850;
	W i;

	/* Set pins as outputs, clock and data low */
	LAT_PGC0 = 0;
	LAT_PGD0 = 0;
	TRIS_PGD0 = 0; /* output */

	/* Ensure MCLR low for full POR reset */
	LAT_MCLR0 = 0;
	wait200ms();

	/*
	 * MCLR high to let CPU reset properly.
	 * DS60001145: need P13 (>1ms) for MCLR to VDD.
	 * Use 10ms to ensure CPU fully resets and clears debug state.
	 * PGECx/PGEDx are held Low by writer to prevent bootloader
	 * from doing anything harmful during this time.
	 */
	LAT_MCLR0 = 1;
	wait1ms();
	wait1ms();
	LAT_MCLR0 = 0;

	/* Delay P6 (100ns min) */
	wait1ms();
	icsp_delay();

	/* Clock in 32-bit key, MSb first, on PGEDx, rising edge of PGECx */
	for (i = 31; i >= 0; i--) {
		LAT_PGD0 = (key >> i) & 1;
		icsp_delay();
		LAT_PGC0 = 1;
		icsp_delay();
		LAT_PGC0 = 0;
		icsp_delay();
	}

	/* MCLR high - enter programming mode */
	LAT_MCLR0 = 1;

	/* Delay P12 (500ns min) + P20 (varies, use 500us) */
	wait1ms();

	/*
	 * TAP starts in Test-Logic-Reset state after ICSP entry.
	 * Transition to Run-Test/Idle with TMS=0.
	 */
	icsp_SetMode(0x1f, 5); /* ensure TLR */
	icsp_SetMode(0, 1); /* TLR -> Run-Test/Idle */
}


/*
 * Exit ICSP mode.
 * Following pic32prog: SW_ETAP, TLR, MCLR low, release pins.
 * MCLR is left LOW: the caller restores the UART pin mapping first and
 * then releases reset itself, so the first bytes of the target's boot
 * log are captured.
 */
static void icsp_exit(void)
{
	/* Clear EJTAGBOOT mode (pic32prog pattern) */
	icsp_SendCommand(MTAP_SW_ETAP);
	icsp_SetMode(0x1f, 5); /* TMS 1-1-1-1-1 -> TLR */
	icsp_SetMode(0, 1); /* TLR -> RTI */

	/* MCLR low */
	LAT_MCLR0 = 0;

	/* Drive PGC/PGD low before releasing */
	LAT_PGC0 = 0;
	LAT_PGD0 = 0;
	wait1ms();

	/* Release pins */
	TRIS_PGD0 = 1;
}


/*
 * Enter Serial Execution mode.
 * Follows pic32prog / DS60001145 sequence exactly.
 */
static W icsp_enter_serial_exec(void)
{
	UW status;
	UW ctl;

	/* Step 1: MTAP_SW_MTAP */
	icsp_SendCommand(MTAP_SW_MTAP);

	/* Step 2: MTAP_COMMAND */
	icsp_SendCommand(MTAP_COMMAND);

	/*
	 * Step 3: read MCHP_STATUS (twice to stabilise, like pic32prog).
	 */
	icsp_XferData8(MCHP_STATUS);
	status = icsp_XferData8(MCHP_STATUS);
	dbg_reg("SE:stat", status);

	if (!(status & STAT_CPS)) {
		p2ustr("SE:WARN code-protected!\r\n");
		return -1;
	}

	/* Step 5: MCHP_ASSERT_RST */
	icsp_XferData8(MCHP_ASSERT_RST);

	/* Step 6: TAP_SW_ETAP */
	icsp_SendCommand(MTAP_SW_ETAP);

	/* Step 7: ETAP_EJTAGBOOT */
	icsp_SendCommand(ETAP_EJTAGBOOT);

	/* Step 8: TAP_SW_MTAP */
	icsp_SendCommand(MTAP_SW_MTAP);

	/* Step 9: MTAP_COMMAND */
	icsp_SendCommand(MTAP_COMMAND);

	/* Step 10: MCHP_DEASSERT_RST */
	icsp_XferData8(MCHP_DE_ASSERT_RST);

	/*
	 * Wait for CPU to come out of reset and trap into DMSEG.
	 * DS60001145 does not specify an exact delay here, but the CPU
	 * needs time to run its startup sequence and reach the debug
	 * exception vector (0xFF200200) set by EJTAGBOOT.
	 * pic32prog runs at ~115kbps serial which is slow enough;
	 * we run at 40MHz so we need an explicit delay.
	 */
	wait1ms();
	wait1ms();
	wait1ms();

	/* Step 11: MCHP_FLASH_ENABLE (MX only) */
	icsp_XferData8(MCHP_FLASH_EN);

	/* Step 12: TAP_SW_ETAP */
	icsp_SendCommand(MTAP_SW_ETAP);

	/* Verify PrAcc=1 (CPU halted in DMSEG, waiting for instruction) */
	icsp_SendCommand(ETAP_CONTROL);
	ctl = icsp_XferData(0x0004C000);
	dbg_reg("SE:ctl", ctl);
	if (!(ctl & (1UL << 18))) {
		p2ustr("SE:WARN PrAcc=0 after entry\r\n");
		return -1;
	}

	/* Test NOP */
	if (icsp_XferInstruction(0x00000000) < 0) {
		p2ustr("SE:NOP fail\r\n");
		return -1;
	}
	dbg_reg("SE:NOP ok", 0);
	dbg_reg("SE:ready", 0);
	return 0;
}


/* ============================================================ */
/* Target UART passthrough pin control                          */
/* ============================================================ */

/*
 * Release the PGD line, point U1RX at the target's debug TX and let
 * the target out of reset.
 */
static void passthrough_start(void)
{
	p2ustr("mclr\r\n");

	RPA0R = 0;	/* PGC pin: GPIO idle high; U1TX is stolen on demand */
	TRIS_PGD0 = 1;	/* in */
	target_uart_on();

	LAT_MCLR0 = 0;
	wait1ms();
	LAT_MCLR0 = 1;

	writing = 0;

	p2ustr("run\r\n");
}


/* ============================================================ */
/* Intel HEX parser                                             */
/* ============================================================ */

static void rsproc(void)
{
	static W linewpos = -1;
	static W upper = -1;
	static UB linebuf[4];
	static UB linesum = 0;
	static UW addrh = 0;
	static UW addr = 0;
	UW c;
	struct writebuf_struct *wp;
	W off;

	if (p2cwpos != p2crpos)
		return;
	if (hostpos >= hostlen)
		return;
	c = hostbuf[hostpos++];

	if (c == ':') {
		linewpos = 0;
		upper = -1;
		linesum = 0;
		return;
	}
	if (linewpos < 0) {
		if ((((-linewpos) & 2) == 0) && (c == 0xd)) {
			linewpos -= 2;
			return;
		}
		if ((((-linewpos) & 4) == 0) && (c == 0xa)) {
			linewpos -= 4;
			return;
		}
		linewpos = -7;
		if (!writing)
			p2cdata(c);
		return;
	}
	if ((c >= '0') && (c <= '9'))
		c -= '0';
	else if ((c >= 'A') && (c <= 'F'))
		c = c - 'A' + 0xa;
	else if ((c >= 'a') && (c <= 'f'))
		c = c - 'a' + 0xa;
	else {
		linewpos = -7;
		if (!writing)
			p2cdata(c);
		return;
	}
	if (upper < 0) {
		upper = c << 4;
		return;
	}
	c |= upper;
	upper = -1;
	linesum += c;
	if ((linewpos == 0) && (c == 0xff)) {
		/*
		 * Custom ":FF" reset trigger.
		 * Only honored when not in the middle of a programming
		 * session - aborting an ICSP session by toggling MCLR would
		 * leave the chip in an undefined state.
		 */
		if (!writing) {
			p2ustr("mclr\r\n");
			LAT_MCLR0 = 0;
			wait1ms();
			LAT_MCLR0 = 1;
			p2ustr("run\r\n");
		}
		linewpos = -1;
		return;
	}
	if (linewpos < 4) {
		linebuf[linewpos++] = c;
		return;
	}
	switch (linebuf[3]) {
	default:
		linewpos = -1;
		return;
	case 0:
		break;
	case 1:
		writeflash(1);
		linewpos = -1;
		return;
	case 4: /* address-high */
		if (linewpos == 4) {
			addrh = c << 24;
			linewpos++;
		} else if (linewpos == 5) {
			addrh |= (c << 16);
			linewpos++;
		} else
			linewpos = -1;
		return;
	}
	if (linewpos == 4)
		addr = addrh | (((UW)linebuf[1]) << 8) | linebuf[2];
	if (linewpos >= linebuf[0] + 4) {
		if ((linesum))
			recverror |= 4;
		linewpos = -1;
		return;
	}

	wp = get_wp(addr);
	off = addr - wp->addr;
	while (wp->size < off)
		wp->d[wp->size++] = 0xff;
	wp->d[wp->size++] = c;
	linewpos++;
	addr++;
}


/* ============================================================ */
/* Flash write                                                  */
/* ============================================================ */

/*
 * Return a writebuf slot for the block containing addr.  If no slot
 * holds that block and writebuf is full, perform a flush
 * (writeflash(0)) to evict everything except the boot-flash slot,
 * then allocate a fresh slot.  The flush also performs the one-time
 * ICSP enter / erase / PE upload on its first invocation.
 */
static struct writebuf_struct *get_wp(UW addr)
{
	UW l = addr & ADDRHMASK;
	struct writebuf_struct *p;
	W i;

	for (i = 0; i < writebufwsize; i++)
		if (writebuf[i].addr == l)
			return writebuf + i;

	if (writebufwsize >= WRITEBUFSIZE)
		writeflash(0);

	p = writebuf + writebufwsize++;
	p->addr = l;
	p->size = 0;
	return p;
}

/*
 * Flush writebuf to flash.
 *
 *   isfinal == 0: mid-stream flush.  Enter ICSP+erase+PE on first call,
 *                 write every non-bootflash slot, drop them from the
 *                 array, keep the ICSP session open for more data.
 *   isfinal == 1: end-of-file flush.  Same setup if needed, write
 *                 non-bootflash, then bootflash (with debugger-enable
 *                 set), send the FASTDATA trailer, exit ICSP, restore
 *                 UART pins for passthrough.
 *
 * Re-writing a page that was already written by an earlier flush is
 * safe because the flash bit cells can only transition 1->0 without
 * an erase: bytes filled with 0xff leave previously-programmed bits
 * untouched.  A new slot post-flush starts with size=0, so the
 * rsproc pad-loop fills any leading gap with 0xff and the per-block
 * tail pad below extends it to BLOCKSIZE.
 */
static void writeflash(W isfinal)
{
	static const UW pe[] = {
	    /* a0000800: init */
	    0x0c00021d, /* jal nvmunlock */
	    0x24044000, /* li a0, 0x4000 nop */
	    0x0c00021d, /* jal nvmunlock */
	    0x24044000, /* li a0, 0x4000 nop */

	    /* a0000810: main */
	    0x3c12a000, /* lui s2, 0xa000 */
	    0x3c0aa000, /* lui t2, 0xa000 */
	    0x354a0078, /* ori t2, t2, 0x0078 */
	    0x8e710000, /* lw s1, 0(s3) */

	    /* a0000820 */
	    0x8e690000, /* lw t1, 0(s3) */
	    0xae490000, /* sw t1, 0(s2) */
	    0x26520004, /* addiu s2, s2, 4 */
	    0x164afffc, /* bne s2, t2, -4 */
	    0, /* nop */

	    0x0c00022c, /* jal waitwr */

	    /* a0000838 */
	    0x8e690000, /* lw t1, 0(s3) */
	    0xae490000, /* sw t1, 0(s2) */
	    0x26520004, /* addiu s2, s2, 4 */

	    0x8e690000, /* lw t1, 0(s3) */
	    0xae490000, /* sw t1, 0(s2) */
	    0x26520004, /* addiu s2, s2, 4 */

	    /* a0000850 */
	    0x3c18ffff, /* lui t8, 0xffff */
	    0x3718ff80, /* ori t8, t8, 0xff80 */
	    0x02388824, /* and s1, s1, t8 */
	    0xae11f420, /* sw s1, 0xfffff420(s0) NVMADDR */
	    0xae00f440, /* sw zero, 0xfffff440(s0) */
	    0x0c00021d, /* jal nvmunlock */
	    0x24044003, /* li a0, 0x4003 write row */

	    /* a000086c */
	    0x1000ffe8, /* b -24 */
	    0, /* nop */

	    /* a0000874: nvmunlock */
	    0xae04f400, /* sw a0, 0xfffff400(s0) */

	    0x8e18f400, /* lw t8, 0xfffff400(s0) */
	    0x33180800, /* andi t8, t8, 0x800 */
	    0x1700fffd, /* bnez t8, -3 */
	    0, /* nop */

	    /* a0000888 */
	    0x3c18aa99, /* lui t8, 0xaa99 */
	    0x37186655, /* ori t8, t8, 0x6655*/
	    0xae18f410, /* sw t8, 0xfffff410(s0)*/

	    0x3c185566, /* lui t8, 0x5566 */
	    0x371899aa, /* ori t8, t8, 0x99aa */
	    0xae18f410, /* sw t8, 0xfffff410(s0) */

	    /* a00008a0 */
	    0x34188000, /* li t8, 0x8000 */
	    0xae18f408, /* sw t8, 0xfffff408(s0)*/

	    0x03e00008, /* jr ra */
	    0, /* nop */

	    /* a00008b0:waitwr */
	    0x8e18f400, /* lw t8, 0xfffff400(s0) */
	    0x33188000, /* andi t8, t8, 0x8000 */
	    0x1700fffd, /* bnez t8, -3 */
	    0, /* nop */

	    0x24184000, /* li t8, 0x4000 */
	    0xae18f404, /* sw t8, 0xfffff404(s0) */

	    0x03e00008, /* jr ra */
	    0 /* nop */
	};
	W i, j, k, new_size;
	UW v, status, idcode;
	W timeout;
	struct writebuf_struct *p;

	if (!writing) {
		p2ustr(isfinal ? "writing\r\n" : "writing otf\r\n");
		/* U1RX back to the WROOM so mid-stream exchanges keep
		   working while the ICSP session is open. */
		target_uart_off();
		RPA0R = 0; /* PGC: i/o */
		writing = 1;

		/* ---- Enter ICSP ---- */
		dbg_reg("ICSP: enter", 0);
		icsp_enter();

		/* ---- Read IDCODE ---- */
		icsp_SetMode(0x1f, 5);
		icsp_SetMode(0, 1);
		idcode = icsp_XferData(0x00000000);
		p2ustr("IDCODE:");
		p2uuw(idcode);
		p2ustr("\r\n");

		if (idcode == 0x00000000 || idcode == 0xffffffff) {
			p2ustr("IDCODE invalid\r\n");
			goto teardown;
		}

		/* ---- STATUS check ---- */
		icsp_SendCommand(MTAP_SW_MTAP);
		icsp_SendCommand(MTAP_COMMAND);
		icsp_XferData8(MCHP_STATUS);
		icsp_XferData8(MCHP_STATUS);
		status = icsp_XferData8(MCHP_STATUS);
		dbg_reg("STATUS", status);

		timeout = 5000;
		while (timeout-- > 0) {
			status = icsp_XferData8(MCHP_STATUS);
			if ((status & STAT_CFGRDY) && !(status & STAT_FCBUSY))
				break;
		}
		dbg_reg("STATwait", status);

		/* ---- Chip erase ---- */
		dbg_reg("=== CHIP ERASE ===", 0);
		icsp_SendCommand(MTAP_SW_MTAP);
		icsp_SendCommand(MTAP_COMMAND);
		icsp_XferData8(MCHP_ERASE);
		dbg_reg("erase sent", 0);
		wait200ms();
		wait200ms();

		timeout = 10000;
		do {
			status = icsp_XferData8(MCHP_STATUS);
			if (!(status & STAT_FCBUSY))
				break;
		} while (timeout-- > 0);
		dbg_reg("post-erase stat", status);

		if (timeout <= 0) {
			p2ustr("FCBUSY stuck after erase\r\n");
			goto teardown;
		}
		dbg_reg("erase done", 0);

		/* ---- Enter serial execution ---- */
		dbg_reg("serial exec...", 0);
		if (icsp_enter_serial_exec() < 0) {
			goto teardown;
		}

		/* ---- PE upload ---- */
		for (i = 0; i < (W)(sizeof(pe) / sizeof(pe[0])); i++)
			icsp_write_word(0xa0000800 + sizeof(pe[0]) * i, pe[i]);

		icsp_XferInstruction(0x3c04bf88); /* setup BMXCON */
		icsp_XferInstruction(0x34842000);
		icsp_XferInstruction(0x3c05001f);
		icsp_XferInstruction(0x34a50040);
		icsp_XferInstruction(0xac850000);
		icsp_XferInstruction(0x34050800);
		icsp_XferInstruction(0xac850010);
		icsp_XferInstruction(0x8c850040);
		icsp_XferInstruction(0xac850020);
		icsp_XferInstruction(0xac850030);

		icsp_XferInstruction(0x3c10bf81); /* lui  s0, 0xbf81 */
		icsp_XferInstruction(0x3c13ff20); /* lui  s3, 0xff20 */

		icsp_XferInstruction(0x3c1da000); /* setup stack */
		icsp_XferInstruction(0x37bd2000);
		icsp_XferInstruction(0x3c1aa000); /* jump */
		icsp_XferInstruction(0x375a0800);
		icsp_XferInstruction(0x03400008);
		icsp_XferInstruction(0);

		icsp_SendCommand(ETAP_FASTDATA);
	}

	/* ---- Write non-bootflash slots ---- */
	for (i = 0; i < writebufwsize; i++) {
		p = writebuf + i;
		if (p->addr == 0x1fc00800)
			continue;
		while (p->size < BLOCKSIZE)
			p->d[p->size++] = 0xff;
		j = 0;
		while (j < BLOCKSIZE) {
			icsp_XferFastData(p->addr + j);
			for (k = 0; k < 32; k++) {
				v = p->d[j++];
				v |= ((UW)p->d[j++]) << 8;
				v |= ((UW)p->d[j++]) << 16;
				v |= ((UW)p->d[j++]) << 24;
				icsp_XferFastData(v);
			}
		}
	}

	if (!isfinal) {
		p2ustr("flush\r\n");
		/* Compact: keep boot-flash slot(s) only. */
		new_size = 0;
		for (i = 0; i < writebufwsize; i++) {
			if (writebuf[i].addr == 0x1fc00800) {
				if (new_size != i)
					writebuf[new_size] = writebuf[i];
				new_size++;
			}
		}
		writebufwsize = new_size;
		return;
	}

	/* ---- Write boot-flash slot(s) ---- */
	for (i = 0; i < writebufwsize; i++) {
		p = writebuf + i;
		if (p->addr != 0x1fc00800)
			continue;
		p->d[0x3fc] |= 3; /* debugger enable */
		while (p->size < BLOCKSIZE)
			p->d[p->size++] = 0xff;
		j = 0;
		while (j < BLOCKSIZE) {
			icsp_XferFastData(p->addr + j);
			for (k = 0; k < 32; k++) {
				v = p->d[j++];
				v |= ((UW)p->d[j++]) << 8;
				v |= ((UW)p->d[j++]) << 16;
				v |= ((UW)p->d[j++]) << 24;
				icsp_XferFastData(v);
			}
		}
	}
	/* PE flush trailer (kept identical to the pre-refactor sequence). */
	for (i = 0; i < 32; i++)
		icsp_XferFastData(0xffffffff);

teardown:
	dbg_reg("ICSP: exit", 0);
	icsp_exit();
	writing = 0;
	writebufwsize = 0;
	/* Restore the UART before releasing reset so the first bytes of
	   the target's boot log are captured. */
	target_uart_on();
	LAT_MCLR0 = 1;
	p2ustr("run\r\n");
}


/* ============================================================ */
/* Wi-Fi association + request template setup                   */
/* ============================================================ */

static	void	wifi_run(void)
{
	for (;;) {
		dly_tsk(50);
		if (wroom4cmd("AT+CWDHCP_CUR=1,1\r\n", "OK", 1000) < 0)
			continue;
		dly_tsk(50);
		{
			static	UB	cmdbuf[16 + SSID_MAX + 4 + PASS_MAX + 4];
			const	UB	*s;
			W	p = 0;

			for (s = (const UB*)"AT+CWJAP_CUR=\""; *s; s++) cmdbuf[p++] = *s;
			for (s = stored_ssid; *s; s++)                 cmdbuf[p++] = *s;
			for (s = (const UB*)"\",\""; *s; s++)          cmdbuf[p++] = *s;
			for (s = stored_pass; *s; s++)                 cmdbuf[p++] = *s;
			for (s = (const UB*)"\"\r\n"; *s; s++)         cmdbuf[p++] = *s;
			cmdbuf[p] = 0;
			if (wroom4cmd(cmdbuf, "OK", 10000) < 0)
				continue;
		}
		dly_tsk(50);
		break;
	}
	{
		W	i;
		static	const	UB	pre[]  = " HTTP/1.0\r\nHost:";
		static	const	UB	suf[]  = "\r\nConnection:close\r\n\r\n";
		static	const	UB	cpre[] = "AT+CIPSTART=\"TCP\",\"";
		static	const	UB	csuf[] = "\",80\r\n";
		const	UB	*s;

		/* "GET <path>" — hex payload appended at send time, then req2. */
		i = 0;
		req[i++] = 'G'; req[i++] = 'E'; req[i++] = 'T'; req[i++] = ' ';
		for (s=stored_path; *s && i<(W)sizeof(req)-1; s++) req[i++] = *s;
		req[i] = 0;

		/* " HTTP/1.0\r\nHost:<host>\r\nConnection:close\r\n\r\n" */
		i = 0;
		for (s=pre; *s; s++) req2[i++] = *s;
		for (s=stored_host; *s; s++) req2[i++] = *s;
		for (s=suf; *s; s++) req2[i++] = *s;
		req2[i] = 0;

		/* AT+CIPSTART="TCP","<host>",80 */
		i = 0;
		for (s=cpre; *s; s++) cipstart[i++] = *s;
		for (s=stored_host; *s; s++) cipstart[i++] = *s;
		for (s=csuf; *s; s++) cipstart[i++] = *s;
		cipstart[i] = 0;
	}
	lcdtp_sendlogs("wifi ready.\n");
}


/* ============================================================ */
/* Writer loop                                                  */
/* ============================================================ */

static	void	writer_loop(void)
{
	static	UB	payload[LOGCHUNK];
	W	last_active;
	W	plen, n, t;

	passthrough_start();
	last_active = 1;	/* announce "mclr/run" right away */

	for (;;) {
		pump();
		rsproc();
		if (hostpos < hostlen)
			continue;	/* digest the current reply first */
		if (p2cwpos != p2crpos)
			continue;	/* drain the target echo backlog */

		/*
		 * Pacing: with an empty send buffer and an empty previous
		 * exchange, wait 2 s before the next server access - but
		 * abort the wait as soon as target data arrives.
		 */
		if ((p2ucount == 0) && !last_active) {
			for (t = 0; t < 20000 && p2ucount == 0; t++)
				wait100us();
		}

		plen = 0;
		while (plen < LOGCHUNK && p2ucount > 0) {
			payload[plen++] = p2ubuf[p2urpos++];
			if (p2urpos >= ULOGSIZE)
				p2urpos = 0;
			p2ucount--;
		}

		n = send_request(payload, plen, hostbuf, sizeof(hostbuf));
		hostlen = (n > 0) ? n : 0;
		hostpos = 0;
		last_active = (plen > 0) || (n > 0);
	}
}


/* ============================================================ */
/* Scan window + application hook                               */
/* ============================================================ */

/* Called from every USB busy-wait via usb_polltask.  Counts down the
   scan window on TMR2 (1 kHz), then runs the Wi-Fi connect and the
   writer loop - which never returns; USB is no longer serviced. */
static	void	app_polltask(void)
{
	if (in_polltask || app_done)
		return;
	in_polltask = 1;
#ifdef DEBUG_UART_SCAN
	/* Test hook (no barcode reader): during the scan window accept
	   barcode text on U2RX/RB1 too (a real HID sends keycodes, but
	   debug bytes are already ASCII - feed them to both layout slots
	   as-is). */
	if ((U2STAbits.OERR))
		U2STA = 0x1400;
	if ((U2STAbits.URXDA)) {
		UB ch = U2RXREG;
		barcode_char(ch, ch);
	}
#endif
	if ((IFS0bits.T2IF)) {
		IFS0bits.T2IF = 0;
		if (window_ms_left > 0)
			window_ms_left--;
	}
	if (window_ms_left == 0) {
		app_done = 1;
		lcdtp_sendlogs("scan window closed\n");
		if (stored_ssid[0] == 0 || stored_url[0] == 0) {
			lcdtp_sendlogs("config incomplete (need WIFI: and C20P: scans); halted.\n");
			for (;;)
				;
		}
		wifi_run();
		writer_loop();	/* never returns */
	}
	in_polltask = 0;
}


/* One-time application init, before the USB host loop starts. */
static	void	app_init(void)
{
	CNPUA = 0xffff;
	CNPUB = 0xffff;	/* usb_init() clears CNPUB again for D+/D- */

	CNPDA = 0;
	CNPDB = 0;

	ANSELA = 0;
	ANSELB = 0;

	/* Target-facing pins: MCLR (RA1) held low until "run"; PGC (RA0)
	   idle high; PGD (RB2) input.  Other pins keep their POR state. */
	LATAbits.LATA1 = 0;
	TRISAbits.TRISA1 = 0;
	LATAbits.LATA0 = 1;
	TRISAbits.TRISA0 = 0;
	TRISBbits.TRISB2 = 1;

	/* WROOM on U1: TX=RPB15, RX=RPB13. RB10/RB11 belong to USB. */
	RPB15R = 1;		/* UTX1 */
	U1RXR = 3;		/* RPB13 */
	TRISBbits.TRISB13 = 1;
	LATBbits.LATB15 = 1;	/* idle high while U1TX is stolen for the target */
	TRISBbits.TRISB15 = 0;
	U1MODE = 0;
	U1BRG = (10000000 / 115200) - 1;
	U1MODE = 0x8008;	/* enable N81 4(U1BRG + 1) */
	U1STA = 0x1400;

	/* UART2: local debug log on RPB9 (P10). */
	RPB9R = 2;		/* UTX2 */
#ifdef DEBUG_UART_SCAN
	U2RXR = 2;		/* RPB1: barcode text injection during the window */
#endif
	U2MODE = 0;
	U2BRG = 86;		/* 115.4kbps */
	U2MODE = 0x8008;	/* enable N81 4(U2BRG + 1) */
	U2STA = 0x1400;

	T2CON = 0x0070;		/* 1/256 */
	TMR2 = 0;
	PR2 = 156;		/* 40M / 256 / 1000Hz */
	T2CON = 0x8070;

	SYSKEY = 0;
	SYSKEY = 0xaa996655;
	SYSKEY = 0x556699aa;
	SYSKEY = 0;

	{
		if (checkflashpage(flashpage0) >= 0)
			c20p1305nvcounter = flashpage0[0];
		lcdtp_sendloguw(c20p1305nvcounter);
		lcdtp_sendlogs(":page0\n");
		if (checkflashpage(flashpage1) >= 0)
			c20p1305nvcounter = flashpage1[0];
		lcdtp_sendloguw(c20p1305nvcounter);
		lcdtp_sendlogs(":page1\n");

		writeflashpage(flashpage0, c20p1305nvcounter + 1);
		writeflashpage(flashpage1, c20p1305nvcounter + 1);
		lcdtp_sendloguw(c20p1305nvcounter);
		lcdtp_sendlogs(":nvcounter\n");
	}
	load_wifi_from_flash();
	load_app_from_flash();
	lcdtp_sendlogs("ssid=");
	lcdtp_sendlogs((stored_ssid[0]) ? (char*)stored_ssid : "(unset)");
	lcdtp_sendlogs("\nurl=");
	lcdtp_sendlogs((stored_url[0])  ? (char*)stored_url  : "(unset)");
	lcdtp_sendlogs("\n");

	/* Bring the WROOM up to a known state before the scan window. */
	for (;;) {
		dly_tsk(200);
		while (wroom4cmd("AT+RST\r\n", "OK", 2000) < 0)
			;
		dly_tsk(1000);
		if (wroom4cmd("ATE0\r\n", "OK", 1000) < 0)
			continue;
		dly_tsk(50);
		if (wroom4cmd("AT+CWMODE_CUR=1\r\n", "OK", 1000) < 0)
			continue;
		dly_tsk(50);
		break;
	}
	TMR2 = 0;
	IFS0bits.T2IF = 0;
	lcdtp_sendlogs("scan window open (usb hid)\n");
}


/* ============================================================
 * USB host implementation (pic32mx-usb-minimal usbhost0015.c)
 * ============================================================ */
#define SYS_CLK_HZ       40000000UL
#define CORE_TICK_PER_MS  (SYS_CLK_HZ / 2 / 1000)

/* ============================================================
 * Constants
 * ============================================================ */
#define USB_PID_SETUP  0xD
#define USB_PID_IN     0x9
#define USB_PID_OUT    0x1

#define KVA_TO_PA(v)  ((uint32_t)(v) & 0x1FFFFFFF)

/* ============================================================
 * BDT
 * MLA style: 4 entries (IN Even, IN Odd, OUT Even, OUT Odd).
 * The endpoint number is selected via U1TOK, not the BDT index.
 * ============================================================ */
typedef struct {
	uint32_t stat;
	uint32_t adr;
} BdtEntry;

#define BDT_UOWN   (1u << 7)
#define BDT_DATA1  (1u << 6)
#define BDT_DTS    (1u << 3)
#define BDT_BC(n)  (((uint32_t)(n) & 0x3FF) << 16)

#define BDT_IN_EVEN    0
#define BDT_IN_ODD     1
#define BDT_OUT_EVEN   2
#define BDT_OUT_ODD    3
#define BDT_SIZE       4

static BdtEntry __attribute__((aligned(512))) g_bdt[BDT_SIZE];

/* ============================================================
 * Buffers
 * ============================================================ */
static uint8_t g_ep0_rx_buf[64];
static uint8_t g_ep0_tx_buf[64];
static uint8_t g_bulk_buf[64];
static uint8_t g_hid_buf[8];

/* ============================================================
 * State
 * ============================================================ */
typedef enum {
	USB_DEV_UNKNOWN,
	USB_DEV_PRINTER,
	USB_DEV_KEYBOARD
} UsbDevType;

typedef enum {
	USB_OK = 0,
	USB_ERR_NAK_TIMEOUT,
	USB_ERR_STALL,
	USB_ERR_TIMEOUT
} UsbResult;

#define BULK_RETRY_MAX  100

static UsbDevType g_dev_type     = USB_DEV_UNKNOWN;
static uint8_t    g_dev_addr     = 0;
static uint8_t    g_bulk_ep      = 0;
static uint8_t    g_hid_ep       = 0;
static uint8_t    g_bulk_toggle  = 0;
static uint8_t    g_hid_toggle   = 0;
static uint8_t    g_ep0_toggle   = 0;
static uint8_t    g_is_low_speed = 0;
static uint8_t    g_ep0_max_pkt  = 8;

/* Ping-pong tracking (MLA bfPingPongIn/bfPingPongOut equivalent) */
static uint8_t g_pp_in  = 0;  /* 0 = EVEN next, 1 = ODD next */
static uint8_t g_pp_out = 0;

/* ============================================================
 * Background task hook
 *
 * If non-NULL this function is called repeatedly inside every
 * busy-wait loop (delay, attach wait, token completion wait,
 * detach wait).  The callee must return promptly.
 * ============================================================ */
void (*usb_polltask)(void);

/* ============================================================
 * Utility
 * ============================================================ */
static void poll_call(void)
{
	if (usb_polltask) {
		usb_polltask();
	}
}

int usb_is_detached(void)
{
	return U1IRbits.DETACHIF != 0;
}

static void my_memset(uint8_t *dst, uint8_t val, uint16_t len)
{
	while (len--) {
		*dst++ = val;
	}
}

static void my_memcpy(uint8_t *dst, const uint8_t *src, uint16_t len)
{
	while (len--) {
		*dst++ = *src++;
	}
}

static void delay_init_ms(uint32_t ms)
{
	uint32_t start = _CP0_GET_COUNT();
	uint32_t ticks = ms * CORE_TICK_PER_MS;

	while ((_CP0_GET_COUNT() - start) < ticks) {
		poll_call();
	}
}

static void delay_usbms(uint32_t ms)
{
	while (ms--) {
		U1OTGIR = _U1OTGIR_T1MSECIF_MASK;
		while (!(U1OTGIR & _U1OTGIR_T1MSECIF_MASK)) {
			poll_call();
		}
	}
}

/* ============================================================
 * Ping-pong BDT selection
 * ============================================================ */
static uint8_t pick_bdt_in(void)
{
	uint8_t idx = g_pp_in ? BDT_IN_ODD : BDT_IN_EVEN;
	g_pp_in ^= 1;
	return idx;
}

static uint8_t pick_bdt_out(void)
{
	uint8_t idx = g_pp_out ? BDT_OUT_ODD : BDT_OUT_EVEN;
	g_pp_out ^= 1;
	return idx;
}

static void reset_ping_pong(void)
{
	g_pp_in  = 0;
	g_pp_out = 0;
}

/*
 * U1EP0 bit 6 is RETRYDIS (Retry Disable).
 * Define it manually in case the xc.h version lacks the macro.
 */
#ifndef _U1EP0_RETRYDIS_MASK
#define EP_RETRYDIS   0x40
#else
#define EP_RETRYDIS   _U1EP0_RETRYDIS_MASK
#endif

/* ============================================================
 * Transfer primitives
 * ============================================================ */

/*
 * token_send - MLA _USB_SendToken equivalent.
 *
 * Re-applies U1EP0 and U1ADDR before every token so that
 * low-speed / full-speed switching and any stale register state
 * are handled reliably.
 *
 * pid:        USB_PID_SETUP / IN / OUT
 * ep:         endpoint number (0..15)
 * is_control: 1 enables SETUP (clears EPCONDIS), 0 for bulk/int
 */
static void token_send(uint8_t pid, uint8_t ep, uint8_t is_control)
{
	uint8_t ep_val;
	uint8_t addr_val;

	/*
	 * Control: RETRYDIS=0 (HW auto-retry, NAK absorbed by SIE)
	 * Int/Bulk: RETRYDIS=1 (NAK surfaces immediately for SW retry)
	 */
	ep_val = _U1EP0_EPRXEN_MASK
	       | _U1EP0_EPTXEN_MASK
	       | _U1EP0_EPHSHK_MASK;
	if (!is_control) {
		ep_val |= _U1EP0_EPCONDIS_MASK;
		ep_val |= EP_RETRYDIS;
	}
	if (g_is_low_speed) {
		ep_val |= _U1EP0_LSPD_MASK;
	}
	U1EP0 = ep_val;

	/* U1ADDR: set LSEN for low-speed devices */
	addr_val = g_dev_addr;
	if (g_is_low_speed) {
		addr_val |= 0x80;
	}
	U1ADDR = addr_val;

	/* Issue the token */
	U1TOK = (pid << 4) | (ep & 0x0F);
}

static UsbResult wait_trn(void)
{
	uint32_t timeout = 500000;

	while (!U1IRbits.TRNIF) {
		if (--timeout == 0) {
			return USB_ERR_TIMEOUT;
		}
		if (U1IRbits.DETACHIF) {
			return USB_ERR_TIMEOUT;
		}
		poll_call();
	}
	U1IR = _U1IR_TRNIF_MASK;
	__asm__("nop");
	__asm__("nop");
	return USB_OK;
}

static UsbResult check_pid(uint8_t bdt_idx)
{
	uint8_t pid = (g_bdt[bdt_idx].stat >> 2) & 0x0F;

	if (pid == 0x0A) {
		return USB_ERR_NAK_TIMEOUT;
	}
	if (pid == 0x0E) {
		return USB_ERR_STALL;
	}
	return USB_OK;
}

/* ============================================================
 * USB initialisation
 * ============================================================ */
void usb_init(void)
{
	uint32_t pa;

	my_memset((uint8_t *)g_bdt, 0, sizeof(g_bdt));
	reset_ping_pong();

	/* Disable GPIO / analogue on D+/D- pins (RB10/RB11) */
	ANSELB &= ~((1 << 10) | (1 << 11));
	CNPUB   = 0;   /* clear any stale weak pull-ups */
	CNPDB   = 0;

	/* Disable all USB interrupts and clear flags */
	U1IE    = 0;
	U1IR    = 0xFF;
	U1OTGIE = 0;
	U1OTGIR = 0x7D;
	U1EIE   = 0;
	U1EIR   = 0xFF;

	/* BDT base address */
	pa = KVA_TO_PA((uint32_t)g_bdt);
	U1BDTP1 = (pa >> 8)  & 0xFF;
	U1BDTP2 = (pa >> 16) & 0xFF;
	U1BDTP3 = (pa >> 24) & 0xFF;

	/* HOSTEN + PPBRST sequence */
	U1CON = _U1CON_HOSTEN_MASK;
	U1CON = _U1CON_HOSTEN_MASK | _U1CON_PPBRST_MASK;
	U1CON = _U1CON_HOSTEN_MASK;

	/* D+/D- pull-downs + VBUS on (separate write) */
	U1OTGCON = _U1OTGCON_DPPULDWN_MASK | _U1OTGCON_DMPULDWN_MASK;
	U1OTGCON |= _U1OTGCON_VBUSON_MASK;

	/* Full ping-pong */
	U1CNFG1 = 0x02;

	U1ADDR = 0;
	U1EP0  = _U1EP0_EPCONDIS_MASK
	       | _U1EP0_EPRXEN_MASK
	       | _U1EP0_EPTXEN_MASK
	       | _U1EP0_EPHSHK_MASK;
	/* RETRYDIS is set per-transfer inside token_send() */
	U1SOF = 0x4A;

	/* Power on the USB module last */
	U1PWRCbits.USBPWR = 1;
	delay_init_ms(10);
}

/* ============================================================
 * Attach wait and bus reset
 * ============================================================ */
void usb_wait_attach_and_reset(void)
{
	while (!U1IRbits.ATTACHIF) {
		poll_call();
	}
	U1IR = _U1IR_ATTACHIF_MASK;

	delay_usbms(200);

	if (!U1CONbits.JSTATE) {
		g_is_low_speed = 1;
	} else {
		g_is_low_speed = 0;
	}

	/* Re-reset ping-pong (both HW and SW) */
	U1CONbits.PPBRST = 1;
	U1CONbits.PPBRST = 0;
	reset_ping_pong();

	/* Bus reset: assert for 50 ms */
	U1CONbits.USBRST = 1;
	delay_usbms(50);
	U1CONbits.USBRST = 0;

	/* MLA order: enable SOF immediately after reset release */
	U1CONbits.SOFEN = 1;

	/* Reset recovery: low-speed devices need ~100 ms */
	delay_usbms(100);

	g_dev_addr  = 0;
	g_ep0_toggle = 0;

	U1IR = _U1IR_DETACHIF_MASK;
}

/* ============================================================
 * Control transfer primitives (ping-pong aware)
 * ============================================================ */
static void ctrl_setup(uint8_t *pkt8)
{
	uint8_t idx = pick_bdt_out();

	my_memcpy(g_ep0_tx_buf, pkt8, 8);

	g_bdt[idx].adr  = KVA_TO_PA((uint32_t)g_ep0_tx_buf);
	g_bdt[idx].stat = BDT_UOWN | BDT_DTS | BDT_BC(8);

	token_send(USB_PID_SETUP, 0, 1);
	wait_trn();
	g_ep0_toggle = 1;
}

static uint16_t ctrl_in(uint8_t *data, uint16_t max_len)
{
	uint16_t total = 0;
	uint16_t chunk_len;
	uint16_t rx_len;
	uint8_t  idx;

	while (total < max_len) {
		chunk_len = max_len - total;
		if (chunk_len > g_ep0_max_pkt) {
			chunk_len = g_ep0_max_pkt;
		}

		idx = pick_bdt_in();
		g_bdt[idx].adr  = KVA_TO_PA((uint32_t)g_ep0_rx_buf);
		g_bdt[idx].stat = BDT_UOWN | BDT_DTS
		                | (g_ep0_toggle ? BDT_DATA1 : 0)
		                | BDT_BC(chunk_len);

		token_send(USB_PID_IN, 0, 1);
		if (wait_trn() != USB_OK) {
			break;
		}

		rx_len = (g_bdt[idx].stat >> 16) & 0x3FF;
		my_memcpy(data + total, g_ep0_rx_buf, rx_len);
		total += rx_len;
		g_ep0_toggle ^= 1;

		/* Short packet terminates the transfer */
		if (rx_len < chunk_len) {
			break;
		}
	}
	return total;
}

static void ctrl_out_zlp(void)
{
	uint8_t idx = pick_bdt_out();

	g_bdt[idx].adr  = KVA_TO_PA((uint32_t)g_ep0_tx_buf);
	g_bdt[idx].stat = BDT_UOWN | BDT_DTS | BDT_DATA1 | BDT_BC(0);

	token_send(USB_PID_OUT, 0, 1);
	wait_trn();
}

static void ctrl_in_zlp(void)
{
	uint8_t idx = pick_bdt_in();

	g_bdt[idx].adr  = KVA_TO_PA((uint32_t)g_ep0_rx_buf);
	g_bdt[idx].stat = BDT_UOWN | BDT_DTS | BDT_DATA1 | BDT_BC(0);

	token_send(USB_PID_IN, 0, 1);
	wait_trn();
}

/* ============================================================
 * Standard requests
 * ============================================================ */
static uint16_t usb_get_descriptor(uint8_t type, uint8_t idx,
                                   uint8_t *buf, uint16_t len)
{
	uint8_t  setup[8];
	uint16_t rx_len;

	setup[0] = 0x80; setup[1] = 0x06;
	setup[2] = idx;  setup[3] = type;
	setup[4] = 0x00; setup[5] = 0x00;
	setup[6] = (uint8_t)(len & 0xFF);
	setup[7] = (uint8_t)(len >> 8);

	ctrl_setup(setup);
	rx_len = ctrl_in(buf, len);
	ctrl_out_zlp();
	return rx_len;
}

static void usb_set_address(uint8_t addr)
{
	uint8_t setup[8];

	setup[0] = 0x00; setup[1] = 0x05;
	setup[2] = addr; setup[3] = 0x00;
	setup[4] = 0x00; setup[5] = 0x00;
	setup[6] = 0x00; setup[7] = 0x00;

	ctrl_setup(setup);
	ctrl_in_zlp();
	delay_usbms(2);

	g_dev_addr = addr;
}

static void usb_set_configuration(uint8_t cfg_val)
{
	uint8_t setup[8];

	setup[0] = 0x00; setup[1] = 0x09;
	setup[2] = cfg_val; setup[3] = 0x00;
	setup[4] = 0x00; setup[5] = 0x00;
	setup[6] = 0x00; setup[7] = 0x00;

	ctrl_setup(setup);
	ctrl_in_zlp();
}

static void usb_set_interface(uint8_t if_num, uint8_t alt_num)
{
	uint8_t setup[8];

	setup[0] = 0x01; setup[1] = 0x0B;
	setup[2] = alt_num; setup[3] = 0x00;
	setup[4] = if_num;  setup[5] = 0x00;
	setup[6] = 0x00; setup[7] = 0x00;

	ctrl_setup(setup);
	ctrl_in_zlp();
}

/* Printer class GET_PORT_STATUS: one byte, bit5 paper-out, bit4
   selected, bit3 no-error (centronics polarity). Returns 0..255 on
   success, -1 when the device does not answer with a full byte. */
static int usb_printer_get_port_status(void)
{
	uint8_t setup[8];
	uint8_t status;

	setup[0] = 0xA1; setup[1] = 0x01;
	setup[2] = 0x00; setup[3] = 0x00;
	setup[4] = 0x00; setup[5] = 0x00;
	setup[6] = 0x01; setup[7] = 0x00;

	ctrl_setup(setup);
	if (ctrl_in(&status, 1) != 1) {
		return -1;
	}
	ctrl_out_zlp();
	return status;
}

/* ============================================================
 * Configuration descriptor parser
 * ============================================================ */
static UsbDevType parse_config_desc(uint8_t *buf, uint16_t len)
{
	UsbDevType dev_type = USB_DEV_UNKNOWN;
	uint16_t i = 0;
	uint8_t  desc_len;
	uint8_t  desc_type;
	uint8_t  cls;
	uint8_t  subcls;
	uint8_t  proto;
	uint8_t  alt_num;
	uint8_t  ep_addr;
	uint8_t  attr;
	uint8_t  in_target = 0;

	g_bulk_ep = 0;
	g_hid_ep  = 0;

	while (i < len) {
		desc_len  = buf[i];
		desc_type = buf[i + 1];

		if (desc_type == 0x04) {  /* Interface descriptor */
			alt_num = buf[i + 3];
			cls     = buf[i + 5];
			subcls  = buf[i + 6];
			proto   = buf[i + 7];

			/*
			 * Accept only the first matching interface
			 * at alternate setting 0.
			 */
			if (alt_num == 0 && dev_type == USB_DEV_UNKNOWN) {
				if (cls == 0x07) {
					dev_type  = USB_DEV_PRINTER;
					in_target = 1;
				} else if (cls == 0x03
				           && subcls == 0x01
				           && proto == 0x01) {
					dev_type  = USB_DEV_KEYBOARD;
					in_target = 1;
				} else {
					in_target = 0;
				}
			} else {
				in_target = 0;
			}
		}

		if (desc_type == 0x05 && in_target) {  /* Endpoint */
			ep_addr = buf[i + 2];
			attr    = buf[i + 3];

			if (dev_type == USB_DEV_PRINTER) {
				if ((ep_addr & 0x80) == 0x00
				    && (attr & 0x03) == 0x02) {
					if (g_bulk_ep == 0) {
						g_bulk_ep = ep_addr & 0x0F;
					}
				}
			} else if (dev_type == USB_DEV_KEYBOARD) {
				if ((ep_addr & 0x80) == 0x80
				    && (attr & 0x03) == 0x03) {
					if (g_hid_ep == 0) {
						g_hid_ep = ep_addr & 0x0F;
					}
				}
			}
		}

		if (desc_len == 0) {
			break;
		}
		i += desc_len;
	}
	return dev_type;
}

static void hid_set_boot_protocol(void)
{
	uint8_t setup[8];

	setup[0] = 0x21; setup[1] = 0x0B;
	setup[2] = 0x00; setup[3] = 0x00;
	setup[4] = 0x00; setup[5] = 0x00;
	setup[6] = 0x00; setup[7] = 0x00;

	ctrl_setup(setup);
	ctrl_in_zlp();
}

/* ============================================================
 * Enumeration
 * ============================================================ */
void usb_enumerate(void)
{
	uint8_t  buf[256];
	uint16_t total_len;

	my_memset(buf, 0, sizeof(buf));
	g_ep0_max_pkt = 8;

	usb_get_descriptor(0x01, 0, buf, 8);

	g_ep0_max_pkt = buf[7];
	if (g_ep0_max_pkt == 0 || g_ep0_max_pkt > 64) {
		g_ep0_max_pkt = 8;
	}

	usb_set_address(0x01);

	usb_get_descriptor(0x01, 0, buf, 18);

	usb_get_descriptor(0x02, 0, buf, 9);

	total_len = buf[2] | ((uint16_t)buf[3] << 8);
	usb_get_descriptor(0x02, 0, buf, total_len);

	g_dev_type = parse_config_desc(buf, total_len);

	usb_set_configuration(buf[5]);

	if (g_dev_type == USB_DEV_PRINTER) {
		usb_set_interface(0, 0);

		/*
		 * Enable the EP1 register for bulk transfers.
		 * The endpoint number is still selected via U1TOK,
		 * but the EPn register must also be enabled in host
		 * mode.
		 */
		U1EP1 = _U1EP1_EPTXEN_MASK
		      | _U1EP1_EPRXEN_MASK
		      | _U1EP1_EPHSHK_MASK;
		g_bulk_toggle = 0;
	} else if (g_dev_type == USB_DEV_KEYBOARD) {
		hid_set_boot_protocol();
		U1EP1 = _U1EP1_EPRXEN_MASK | _U1EP1_EPHSHK_MASK;
		g_hid_toggle = 0;
	}
}

/* ============================================================
 * Post-detach full reset
 * ============================================================ */
static void usb_reset_state(void)
{
	g_dev_type     = USB_DEV_UNKNOWN;
	g_dev_addr     = 0;
	g_bulk_ep      = 0;
	g_hid_ep       = 0;
	g_bulk_toggle  = 0;
	g_hid_toggle   = 0;
	g_ep0_toggle   = 0;
	g_is_low_speed = 0;
	g_ep0_max_pkt  = 8;

	U1IE    = 0;
	U1OTGIE = 0;
	U1EIE   = 0;
	U1CON   = 0;
	U1PWRCbits.USBPWR = 0;
	delay_init_ms(2);

	my_memset((uint8_t *)g_bdt, 0, sizeof(g_bdt));

	usb_init();
}

/* ============================================================
 * Bulk OUT transfer (shares the BDT with EP0; endpoint number
 * is selected via U1TOK)
 * ============================================================ */
UsbResult usb_bulk_write(uint8_t *data, uint16_t len)
{
	uint16_t  offset = 0;
	uint16_t  chunk_len;
	uint8_t   retry_count;
	uint8_t   idx;
	UsbResult result;

	while (offset < len) {
		chunk_len = len - offset;
		if (chunk_len > 64) {
			chunk_len = 64;
		}
		retry_count = 0;

		my_memcpy(g_bulk_buf, data + offset, chunk_len);

		do {
			idx = pick_bdt_out();
			g_bdt[idx].adr  = KVA_TO_PA((uint32_t)g_bulk_buf);
			g_bdt[idx].stat = BDT_UOWN | BDT_DTS
			                | (g_bulk_toggle ? BDT_DATA1 : 0)
			                | BDT_BC(chunk_len);

			token_send(USB_PID_OUT, g_bulk_ep, 0);

			result = wait_trn();
			if (result == USB_ERR_TIMEOUT) {
				return USB_ERR_TIMEOUT;
			}

			result = check_pid(idx);

			if (result == USB_ERR_STALL) {
				return USB_ERR_STALL;
			}

			if (result == USB_ERR_NAK_TIMEOUT) {
				retry_count++;
				if (retry_count >= BULK_RETRY_MAX) {
					return USB_ERR_NAK_TIMEOUT;
				}
				delay_usbms(1);
				if (U1IRbits.DETACHIF) {
					return USB_ERR_TIMEOUT;
				}
			}
		} while (result == USB_ERR_NAK_TIMEOUT);

		g_bulk_toggle ^= 1;
		offset += chunk_len;
	}
	return USB_OK;
}

/* ============================================================
 * Interrupt IN transfer
 * ============================================================ */
typedef struct {
	uint8_t modifier;
	uint8_t reserved;
	uint8_t keycode[6];
} HidKbReport;

static int usb_interrupt_in(void)
{
	uint32_t timeout;
	uint8_t  pid;
	uint8_t  idx;

	idx = pick_bdt_in();
	g_bdt[idx].adr  = KVA_TO_PA((uint32_t)g_hid_buf);
	g_bdt[idx].stat = BDT_UOWN | BDT_DTS
	                | (g_hid_toggle ? BDT_DATA1 : 0)
	                | BDT_BC(8);

	token_send(USB_PID_IN, g_hid_ep, 0);

	/*
	 * RETRYDIS is set, so a NAK raises TRNIF immediately.
	 * A short timeout is sufficient.
	 */
	timeout = 10000;
	while (!U1IRbits.TRNIF) {
		if (--timeout == 0) {
			return -1;
		}
		if (U1IRbits.DETACHIF) {
			return -1;
		}
		poll_call();
	}
	U1IR = _U1IR_TRNIF_MASK;

	pid = (g_bdt[idx].stat >> 2) & 0x0F;
	if (pid == 0x0A) {
		return 0;  /* NAK - no data available */
	}

	g_hid_toggle ^= 1;
	return 1;
}

/* ============================================================
 * Keycode to ASCII (US layout)
 * ============================================================ */
static const char keycode_to_ascii[58] = {
	0,    0,    0,    0,   'a', 'b', 'c', 'd',
	'e',  'f',  'g',  'h', 'i', 'j', 'k', 'l',
	'm',  'n',  'o',  'p', 'q', 'r', 's', 't',
	'u',  'v',  'w',  'x', 'y', 'z', '1', '2',
	'3',  '4',  '5',  '6', '7', '8', '9', '0',
	'\n', 0,   '\b', '\t', ' ', '-', '=', '[',
	']',  '\\', 0,    ';', '\'', '`', ',', '.',
	'/',  0,
};

static const char keycode_to_ascii_shift[58] = {
	0,    0,    0,    0,   'A', 'B', 'C', 'D',
	'E',  'F',  'G',  'H', 'I', 'J', 'K', 'L',
	'M',  'N',  'O',  'P', 'Q', 'R', 'S', 'T',
	'U',  'V',  'W',  'X', 'Y', 'Z', '!', '@',
	'#',  '$',  '%',  '^', '&', '*', '(', ')',
	'\n', 0,   '\b', '\t', ' ', '_', '+', '{',
	'}',  '|',  0,    ':', '"', '~', '<', '>',
	'?',  0,
};

/* JIS (JP 106/109) layout tables. Letters and digits match US; the symbol
   block differs. Keycode 0x87 (International1, the "Ro" key) is JIS-only:
   '\\' unshifted, '_' shifted — handled separately below. */
static const char keycode_to_ascii_jis[58] = {
	0,    0,    0,    0,   'a', 'b', 'c', 'd',
	'e',  'f',  'g',  'h', 'i', 'j', 'k', 'l',
	'm',  'n',  'o',  'p', 'q', 'r', 's', 't',
	'u',  'v',  'w',  'x', 'y', 'z', '1', '2',
	'3',  '4',  '5',  '6', '7', '8', '9', '0',
	'\n', 0,   '\b', '\t', ' ', '-', '^', '@',
	'[',  ']',  ']',  ';', ':', 0,   ',', '.',
	'/',  0,
};

static const char keycode_to_ascii_jis_shift[58] = {
	0,    0,    0,    0,   'A', 'B', 'C', 'D',
	'E',  'F',  'G',  'H', 'I', 'J', 'K', 'L',
	'M',  'N',  'O',  'P', 'Q', 'R', 'S', 'T',
	'U',  'V',  'W',  'X', 'Y', 'Z', '!', '"',
	'#',  '$',  '%',  '&', '\'', '(', ')', 0,
	'\n', 0,   '\b', '\t', ' ', '=', '~', '`',
	'{',  '}',  '}',  '+', '*', 0,   '<', '>',
	'?',  0,
};

/*
	Keyboard layout. The compile-time default is JP (JIS); build with
	-DHID_LAYOUT_US for US. During the barcode scan window every keystroke
	is decoded through BOTH layouts in parallel and barcode_char() picks
	the line whose prefix validates — automatic and unambiguous. After the
	window (the "k=" phase) only the default layout is used.
*/
#ifdef HID_LAYOUT_US
#define HID_LAYOUT_DEFAULT_JIS	0
#else
#define HID_LAYOUT_DEFAULT_JIS	1
#endif

static char keycode_to_char(uint8_t keycode, uint8_t modifier, uint8_t jis)
{
	/* JIS-only keys: International1 "Ro" and International3 Yen. */
	if (keycode == 0x87)
		return jis ? ((modifier & 0x22) ? '_' : '\\') : 0;
	if (keycode == 0x89)
		return jis ? ((modifier & 0x22) ? '|' : '\\') : 0;
	if (keycode >= sizeof(keycode_to_ascii)) {
		return 0;
	}
	/* Barcode scans carry ':', ';', '&', '?', '_', uppercase, etc., so
	   shift must map the full table, not just letters. */
	if (jis)
		return (modifier & 0x22) ? keycode_to_ascii_jis_shift[keycode]
		                         : keycode_to_ascii_jis[keycode];
	return (modifier & 0x22) ? keycode_to_ascii_shift[keycode]
	                         : keycode_to_ascii[keycode];
}


/* ============================================================ */
/* Main - USB host loop (from usbhost0015.c) with barcode hooks */
/* ============================================================ */
int main(void)
{
	HidKbReport  prev;
	HidKbReport *rep;
	int          ret;
	int          i;
	int          j;
	int          already;
	uint8_t      kc;
	char         c;

	app_init();
	usb_polltask = app_polltask;

	usb_init();

	while (1) {
		usb_wait_attach_and_reset();

		usb_enumerate();

		if (g_dev_type == USB_DEV_KEYBOARD) {
			prev.modifier = 0;
			prev.reserved = 0;
			for (i = 0; i < 6; i++) {
				prev.keycode[i] = 0;
			}

			while (!usb_is_detached()) {
				ret = usb_interrupt_in();
				delay_usbms(10);

				if (ret <= 0) {
					continue;
				}

				rep = (HidKbReport *)g_hid_buf;

				for (i = 0; i < 6; i++) {
					kc = rep->keycode[i];
					if (kc == 0) {
						continue;
					}

					already = 0;
					for (j = 0; j < 6; j++) {
						if (prev.keycode[j] == kc) {
							already = 1;
							break;
						}
					}
					if (already) {
						continue;
					}

					c = keycode_to_char(kc, rep->modifier,
					                    HID_LAYOUT_DEFAULT_JIS);
					{
						char c2 = keycode_to_char(kc, rep->modifier,
						                          !HID_LAYOUT_DEFAULT_JIS);
						if (c || c2) {
							barcode_char((UB)c, (UB)c2);
						}
					}
				}
				prev = *rep;
			}
		} else {
			/* Printer / unknown class: idle until detach (the scan
			   window countdown keeps running via poll_call). */
			while (!usb_is_detached()) {
				poll_call();
			}
		}

		usb_reset_state();
		delay_init_ms(200);
	}

	return 0;
}
