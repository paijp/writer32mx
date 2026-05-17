/*
 * writer32mx - PIC32MX270F256B ICSP flash programmer
 *
 * Developed by paijp in collaboration with Anthropic's Claude Sonnet 4.6.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <xc.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef signed char B;
typedef unsigned char UB;
typedef short H;
typedef unsigned short UH;
typedef int W;
typedef unsigned int UW;

typedef volatile signed char _B;
typedef volatile unsigned char _UB;
typedef volatile short _H;
typedef volatile unsigned short _UH;
typedef volatile int _W;
typedef volatile unsigned int _UW;


#pragma config FNOSC    = PRIPLL
#pragma config POSCMOD  = XT
#pragma config FPLLIDIV = DIV_1
#pragma config FPLLMUL  = MUL_20
#pragma config FPLLODIV = DIV_2
#pragma config FPBDIV   = DIV_1
#pragma config UPLLEN   = ON
#pragma config UPLLIDIV = DIV_1
#pragma config FUSBIDIO = OFF
#pragma config FVBUSONIO= OFF
#pragma config FWDTEN   = OFF
#pragma config JTAGEN   = OFF
#pragma config FSOSCEN  = OFF
#pragma config IESO     = OFF
#pragma config FCKSM    = CSDCMD
#pragma config OSCIOFNC = OFF
#pragma config ICESEL   = ICS_PGx2
#pragma config PMDL1WAY = OFF
#pragma config IOL1WAY  = OFF
#pragma config DEBUG    = OFF
#pragma config PWP      = OFF
#pragma config BWP      = OFF
#pragma config CP       = OFF


/*
 * Pin assignments (writer -> target):
 *   Writer RB2  -> Target RB10 (PGED2)   : ICSP data / 2-wire bit0
 *   Writer RA0  -> Target RB11 (PGEC2)   : ICSP clock / 2-wire bit1
 *   Writer RB1  -> Target MCLR           : reset control
 */


/* ---- Pin definitions for target connection ---- */

/* ICSP pins (directly driving target PGEC2/PGED2) */
#define LAT_PGC0 LATAbits.LATA0
#define LAT_MCLR0 LATBbits.LATB1
#define PORT_PGD0 PORTBbits.RB2
#define LAT_PGD0 LATBbits.LATB2
#define TRIS_PGD0 TRISBbits.TRISB2


/* ============================================================ */
/* USB CDC driver (from cdc0008.c)                              */
/* Copyright (c) 2026 paijp, Apache License 2.0                */
/* ============================================================ */

typedef struct {
	volatile uint32_t stat;
	volatile uint32_t addr;
} bdt_t;

#define BDT_UOWN     0x0080
#define BDT_DATA01   0x0040
#define BDT_DTSEN    0x0008
#define BDT_BSTALL   0x0004
#define BDT_BC_SHIFT 16
#define BDT_BC_MASK  0x03FF0000

#define PID_MASK     0x003C
#define PID_SETUP    (0x0D << 2)

static bdt_t bdt[12] __attribute__((aligned(512)));

#define BD_EP0_OUT_E 0
#define BD_EP0_OUT_O 1
#define BD_EP0_IN_E  2
#define BD_EP0_IN_O  3
#define BD_EP2_OUT_E 8
#define BD_EP2_OUT_O 9
#define BD_EP2_IN_E  10
#define BD_EP2_IN_O  11

#define EP2_SIZE 64

static uint8_t ep0_out_buf[2][64];
static uint8_t ep0_in_buf[2][64];
static uint8_t ep2_out_buf[2][EP2_SIZE];
static uint8_t ep2_in_buf[2][EP2_SIZE];

static const uint8_t dev_desc[] = {
	18, 0x01, 0x10, 0x01, 0x02, 0x00, 0x00, 64,
	0xD8, 0x04, 0x0A, 0x00, 0x01, 0x00, 1, 2, 0, 1
};

#define CFG_TOTAL 67
static const uint8_t cfg_desc[CFG_TOTAL] = {
	9, 0x02, CFG_TOTAL, 0x00, 2, 1, 0, 0xA0, 50,
	9, 0x04, 0, 0, 1, 0x02, 0x02, 0x01, 0,
	5, 0x24, 0x00, 0x10, 0x01,
	5, 0x24, 0x01, 0x00, 0x01,
	4, 0x24, 0x02, 0x02,
	5, 0x24, 0x06, 0, 1,
	7, 0x05, 0x81, 0x03, 0x08, 0x00, 0x02,
	9, 0x04, 1, 0, 2, 0x0A, 0x00, 0x00, 0,
	7, 0x05, 0x02, 0x02, EP2_SIZE, 0x00, 0,
	7, 0x05, 0x82, 0x02, EP2_SIZE, 0x00, 0
};

static const uint8_t str0[] = { 4, 0x03, 0x09, 0x04 };
static const uint8_t str1[] = { 2 + 2 * 2, 0x03, 'k', 0, 'b', 0 };
static const uint8_t str2[] = {
	2 + 7 * 2, 0x03,
	'c', 0, 'd', 0, 'c', 0, '0', 0, '0', 0, '0', 0, '8', 0
};

static uint8_t line_coding[7] = {
	0x80, 0x25, 0x00, 0x00, 0x00, 0x00, 0x08
};

typedef enum {
	CTRL_IDLE,
	CTRL_DATA_IN,
	CTRL_DATA_OUT
} ctrl_state_t;

static ctrl_state_t ctrl_state;
static const uint8_t *ctrl_src;
static uint8_t *ctrl_dst;
static uint16_t ctrl_left;
static uint8_t ctrl_in_d1;
static uint8_t ep0_in_ppbi;
static uint8_t usb_addr_pending;
static uint8_t usb_config;

static volatile uint8_t ep2_out_ready[2];
static volatile uint16_t ep2_out_len[2];
static volatile uint8_t ep2_out_next_ppbi;
static volatile uint8_t ep2_in_ppbi;

#define VA2PA(x)  ((uint32_t)(x) & 0x1FFFFFFF)

static void ep0_out_arm(uint8_t ppbi)
{
	uint8_t idx = (ppbi == 0) ? BD_EP0_OUT_E : BD_EP0_OUT_O;

	bdt[idx].addr = VA2PA(ep0_out_buf[ppbi]);
	bdt[idx].stat = ((uint32_t)64 << BDT_BC_SHIFT) | BDT_UOWN;
}

static void ep0_in_send(const void *data, uint16_t len)
{
	uint8_t idx = (ep0_in_ppbi == 0) ? BD_EP0_IN_E : BD_EP0_IN_O;
	uint8_t *buf = ep0_in_buf[ep0_in_ppbi];

	if (len > 64)
		len = 64;
	if (len)
		memcpy(buf, data, len);
	bdt[idx].addr = VA2PA(buf);
	bdt[idx].stat = ((uint32_t)len << BDT_BC_SHIFT) |
	                BDT_UOWN | BDT_DTSEN |
	                (ctrl_in_d1 ? BDT_DATA01 : 0);
	ctrl_in_d1 ^= 1;
	ep0_in_ppbi ^= 1;
}

static void ep0_in_zlp(void)
{
	uint8_t idx = (ep0_in_ppbi == 0) ? BD_EP0_IN_E : BD_EP0_IN_O;

	bdt[idx].addr = VA2PA(ep0_in_buf[ep0_in_ppbi]);
	bdt[idx].stat = (0u << BDT_BC_SHIFT) |
	                BDT_UOWN | BDT_DTSEN | BDT_DATA01;
	ep0_in_ppbi ^= 1;
}

static void ep0_in_continue(void)
{
	uint16_t chunk = ctrl_left;

	if (chunk > 64)
		chunk = 64;
	ep0_in_send(ctrl_src, chunk);
	ctrl_src += chunk;
	ctrl_left -= chunk;
	if (chunk == 0)
		ctrl_state = CTRL_IDLE;
}

static void ep2_out_arm(uint8_t ppbi)
{
	uint8_t idx = (ppbi == 0) ? BD_EP2_OUT_E : BD_EP2_OUT_O;
	uint32_t s = ((uint32_t)EP2_SIZE << BDT_BC_SHIFT) | BDT_UOWN | BDT_DTSEN;

	if (ppbi)
		s |= BDT_DATA01;
	bdt[idx].addr = VA2PA(ep2_out_buf[ppbi]);
	bdt[idx].stat = s;
}

static void handle_setup(const uint8_t *s)
{
	uint8_t bmRequestType = s[0];
	uint8_t bRequest = s[1];
	uint16_t wValue = (uint16_t)s[2] | ((uint16_t)s[3] << 8);
	uint16_t wIndex = (uint16_t)s[4] | ((uint16_t)s[5] << 8);
	uint16_t wLength = (uint16_t)s[6] | ((uint16_t)s[7] << 8);

	(void)wIndex;

	ctrl_in_d1 = 1;
	ctrl_state = CTRL_IDLE;

	if ((bmRequestType & 0x60) == 0) {
		switch (bRequest) {
		case 0x05:
			usb_addr_pending = (uint8_t)wValue;
			ep0_in_zlp();
			goto out;

		case 0x06: {
			uint8_t type = (uint8_t)(wValue >> 8);
			uint8_t index = (uint8_t)(wValue & 0xFF);
			const uint8_t *p = 0;
			uint16_t n = 0;

			if (type == 0x01) {
				p = dev_desc; n = sizeof dev_desc;
			} else if (type == 0x02) {
				p = cfg_desc; n = sizeof cfg_desc;
			} else if (type == 0x03) {
				if (index == 0) {
					p = str0; n = sizeof str0;
				} else if (index == 1) {
					p = str1; n = sizeof str1;
				} else if (index == 2) {
					p = str2; n = sizeof str2;
				}
			}
			if (p) {
				if (n > wLength)
					n = wLength;
				ctrl_src = p;
				ctrl_left = n;
				ctrl_state = CTRL_DATA_IN;
				ep0_in_continue();
				goto out;
			}
			break;
		}

		case 0x09:
			usb_config = (uint8_t)wValue;
			U1EP1 = 0x15;
			U1EP2 = 0x1D;
			ep2_in_ppbi = 0;
			ep2_out_next_ppbi = 0;
			ep2_out_ready[0] = 0;
			ep2_out_ready[1] = 0;
			ep2_out_arm(0);
			ep2_out_arm(1);
			ep0_in_zlp();
			goto out;

		case 0x00:
			ep0_in_buf[ep0_in_ppbi][0] = 0;
			ep0_in_buf[ep0_in_ppbi][1] = 0;
			ctrl_src = ep0_in_buf[ep0_in_ppbi];
			ctrl_left = 2;
			ctrl_state = CTRL_DATA_IN;
			ep0_in_continue();
			goto out;

		case 0x08:
			ep0_in_buf[ep0_in_ppbi][0] = usb_config;
			ctrl_src = ep0_in_buf[ep0_in_ppbi];
			ctrl_left = 1;
			ctrl_state = CTRL_DATA_IN;
			ep0_in_continue();
			goto out;

		case 0x0A:
			ep0_in_zlp();
			goto out;
		}
	} else if ((bmRequestType & 0x60) == 0x20) {
		switch (bRequest) {
		case 0x20:
			ctrl_dst = line_coding;
			ctrl_left = (wLength > 7) ? 7 : wLength;
			ctrl_state = CTRL_DATA_OUT;
			goto out;
		case 0x21:
			ctrl_src = line_coding;
			ctrl_left = 7;
			ctrl_state = CTRL_DATA_IN;
			ep0_in_continue();
			goto out;
		case 0x22:
		case 0x23:
			ep0_in_zlp();
			goto out;
		}
	}

	{
		uint8_t in_idx = (ep0_in_ppbi == 0) ? BD_EP0_IN_E : BD_EP0_IN_O;
		bdt[in_idx].stat = BDT_UOWN | BDT_BSTALL;
	}
out:
	U1CONbits.PKTDIS = 0;
}

static void handle_trn(void)
{
	uint8_t stat = U1STAT;
	uint8_t ep = (stat >> 4) & 0x0F;
	uint8_t dir = (stat >> 3) & 0x01;
	uint8_t ppbi = (stat >> 2) & 0x01;
	uint8_t idx = (ep << 2) | (dir << 1) | ppbi;
	bdt_t *b = &bdt[idx];
	uint8_t pid = (uint8_t)(b->stat & PID_MASK);

	U1IR = 0x08;

	if (ep == 0) {
		if (pid == PID_SETUP) {
			handle_setup(ep0_out_buf[ppbi]);
			ep0_out_arm(ppbi);
		} else if (dir == 1) {
			if (usb_addr_pending) {
				U1ADDR = usb_addr_pending;
				usb_addr_pending = 0;
			}
			if (ctrl_state == CTRL_DATA_IN && ctrl_left > 0)
				ep0_in_continue();
		} else {
			if (ctrl_state == CTRL_DATA_OUT && ctrl_left > 0) {
				uint16_t n = (b->stat & BDT_BC_MASK) >> BDT_BC_SHIFT;

				if (n > ctrl_left)
					n = ctrl_left;
				memcpy(ctrl_dst, ep0_out_buf[ppbi], n);
				ctrl_dst += n;
				ctrl_left -= n;
				if (ctrl_left == 0) {
					ctrl_state = CTRL_IDLE;
					ep0_in_zlp();
				}
			}
			ep0_out_arm(ppbi);
		}
	} else if (ep == 2) {
		if (dir == 0) {
			uint16_t n = (b->stat & BDT_BC_MASK) >> BDT_BC_SHIFT;
			ep2_out_len[ppbi] = n;
			ep2_out_ready[ppbi] = 1;
		}
	}
}

static void usb_bus_reset(void)
{
	U1ADDR = 0;
	usb_addr_pending = 0;
	usb_config = 0;
	ctrl_state = CTRL_IDLE;
	ctrl_in_d1 = 1;
	ep0_in_ppbi = 0;
	ep2_in_ppbi = 0;
	ep2_out_next_ppbi = 0;
	ep2_out_ready[0] = 0;
	ep2_out_ready[1] = 0;
	U1IR = 0xFF;
	U1EIR = 0xFF;
	U1CONbits.PPBRST = 1;
	U1CONbits.PPBRST = 0;
	U1EP0 = 0x0D;
	U1EP1 = 0;
	U1EP2 = 0;
	memset(bdt, 0, sizeof bdt);
	ep0_out_arm(0);
	ep0_out_arm(1);
}

static void usb_init_hw(void)
{
	uint32_t pa;

	U1CNFG1 = 0x02;
	U1CONbits.PPBRST = 1;
	U1CONbits.PPBRST = 0;
	U1IE = 0;
	U1EIE = 0;
	U1CONbits.USBEN = 1;
	U1IR = 0x08; U1IR = 0x08; U1IR = 0x08; U1IR = 0x08;
	U1IR = 0xFF;
	U1EIR = 0xFF;
	pa = VA2PA(bdt);
	U1BDTP1 = (uint8_t)((pa >> 8) & 0xFE);
	U1BDTP2 = (uint8_t)(pa >> 16);
	U1BDTP3 = (uint8_t)(pa >> 24);
	U1EP0 = 0; U1EP1 = 0; U1EP2 = 0; U1EP3 = 0;
	U1EP4 = 0; U1EP5 = 0; U1EP6 = 0; U1EP7 = 0;
	U1EP8 = 0; U1EP9 = 0; U1EP10 = 0; U1EP11 = 0;
	U1EP12 = 0; U1EP13 = 0; U1EP14 = 0; U1EP15 = 0;
	U1EP0 = 0x0D;
	U1ADDR = 0;
	usb_addr_pending = 0;
	usb_config = 0;
	ctrl_state = CTRL_IDLE;
	ctrl_in_d1 = 1;
	ep0_in_ppbi = 0;
	ep2_in_ppbi = 0;
	ep2_out_next_ppbi = 0;
	ep2_out_ready[0] = 0;
	ep2_out_ready[1] = 0;
	memset(bdt, 0, sizeof bdt);
	ep0_out_arm(0);
	ep0_out_arm(1);
	U1PWRCbits.USBPWR = 1;
}

static void cdc_init(void)
{
	usb_init_hw();
}

static void cdc_poll(void)
{
	if (U1IR & 0x01) {
		usb_bus_reset();
		U1IR = 0x01;
	}
	if (U1IR & 0x08)
		handle_trn();
	if (U1IR & 0x10)
		U1IR = 0x10;
	if (U1IR & 0x80)
		U1IR = 0x80;
	if (U1IR & 0x02) {
		U1EIR = 0xFF;
		U1IR = 0x02;
	}
}

static uint16_t cdc_recv(uint8_t *buf, uint16_t max)
{
	uint8_t ppbi;
	uint16_t n;

	if (!usb_config)
		return 0;
	ppbi = ep2_out_next_ppbi;
	if (!ep2_out_ready[ppbi]) {
		ppbi ^= 1;
		if (!ep2_out_ready[ppbi])
			return 0;
	}
	n = ep2_out_len[ppbi];
	if (n == 0) {
		ep2_out_ready[ppbi] = 0;
		ep2_out_arm(ppbi);
		ep2_out_next_ppbi = ppbi ^ 1;
		return 0;
	}
	if (max < n)
		return n;
	if (buf != NULL)
		memcpy(buf, ep2_out_buf[ppbi], n);
	ep2_out_ready[ppbi] = 0;
	ep2_out_arm(ppbi);
	ep2_out_next_ppbi = ppbi ^ 1;
	return n;
}

static uint16_t cdc_send(const uint8_t *buf, uint16_t len)
{
	uint8_t idx;
	uint8_t *dst;
	uint32_t s;

	if (!usb_config)
		return 0;
	if (len == 0) {
		idx = (ep2_in_ppbi == 0) ? BD_EP2_IN_E : BD_EP2_IN_O;
		return (bdt[idx].stat & BDT_UOWN) ? 0 : EP2_SIZE;
	}
	if (len > EP2_SIZE)
		len = EP2_SIZE;
	idx = (ep2_in_ppbi == 0) ? BD_EP2_IN_E : BD_EP2_IN_O;
	if (bdt[idx].stat & BDT_UOWN)
		return 0;
	dst = ep2_in_buf[ep2_in_ppbi];
	memcpy(dst, buf, len);
	bdt[idx].addr = VA2PA(dst);
	s = ((uint32_t)len << BDT_BC_SHIFT) | BDT_UOWN | BDT_DTSEN;
	if (ep2_in_ppbi)
		s |= BDT_DATA01;
	bdt[idx].stat = s;
	ep2_in_ppbi ^= 1;
	return len;
}


/* ---- Buffer definitions ---- */

#define BUFFERSIZE 256
static UH p2cbuf[BUFFERSIZE];
static W p2cwpos = 0;
static W p2crpos = 0;

static UB p2ubuf[BUFFERSIZE];
static W p2uwpos = 0;
static W p2urpos = 0;

static W recverror = 0x8000;

#define BLOCKSIZE 0x400
#define ADDRHMASK 0xfffffc00
#define WRITEBUFSIZE 32
static struct writebuf_struct {
	UB d[BLOCKSIZE];
	UW addr;
	W size;
} writebuf[WRITEBUFSIZE];
static W writebufrsize = 0;
static W writebufwsize = 0;
static struct writebuf_struct *wp = NULL;

static W writing = 0;


/* ============================================================ */
/* Delay routines                                               */
/* ============================================================ */

static void wait1us(void)
{
	long l;
	for (l = 15; l > 0; l--)
		asm("nop");
}

static void wait10us(void)
{
	long l;
	for (l = 150; l > 0; l--)
		asm("nop");
}

static void wait100us(void)
{
	long l;
	for (l = 1500; l > 0; l--)
		asm("nop");
}

/* ============================================================ */
/* UART / serial buffer routines                                */
/* ============================================================ */

static void p2cdata(UB c)
{
	p2cbuf[p2cwpos++] = c;
	if (p2cwpos >= BUFFERSIZE)
		p2cwpos = 0;
}

static void p2udata(UB c)
{
	p2ubuf[p2uwpos++] = c;
	if (p2uwpos >= BUFFERSIZE)
		p2uwpos = 0;
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

static void p2uub(UB v)
{
	static const UB hex[] = "0123456789abcdef";
	p2udata(hex[(v >> 4) & 0xf]);
	p2udata(hex[v & 0xf]);
}


/* ============================================================ */
/* Intel HEX parser                                             */
/* ============================================================ */

static void recvrs(UW c)
{
	static W linewpos = -1;
	static W upper = -1;
	static UB linebuf[4];
	static UB linesum = 0;
	static UW addrh = 0;
	static UW addr = 0;
	UW l;
	W i;

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
		linewpos = -1;
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
		for (i = 0; i < writebufwsize; i++) {
			wp = writebuf + i;
			while (wp->size < BLOCKSIZE)
				wp->d[wp->size++] = 0xff;
		}
		writebufrsize = writebufwsize;
		writebufwsize = 0;
		wp = NULL;
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
		writebufrsize = 0;
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

	l = addr & ADDRHMASK;
	if ((wp == NULL) || (wp->addr != l)) {
		wp = NULL;
		for (i = 0; i < writebufwsize; i++)
			if (writebuf[i].addr == l) {
				wp = writebuf + i;
				break;
			}
		if (wp == NULL) {
			if (writebufwsize >= WRITEBUFSIZE - 1) {
				recverror |= 0x100;
				linewpos = -1;
				return;
			}
			wp = writebuf + writebufwsize++;
			wp->addr = l;
			wp->size = 0;
		}
	}

	i = addr - wp->addr;
	while (wp->size < i)
		wp->d[wp->size++] = 0xff;
	wp->d[wp->size++] = c;
	linewpos++;
	addr++;
}


/* ============================================================ */
/* Idle task - flush UART1 buffer, USB CDC send/recv HEX data  */
/* ============================================================ */

static void idletask(void)
{
	/* Flush UART1 (debug output) */
	while ((p2cwpos != p2crpos) && (U1STAbits.UTXBF == 0)) {
		U1TXREG = p2cbuf[p2crpos++];
		if (p2crpos >= BUFFERSIZE)
			p2crpos = 0;
	}

	/* USB CDC polling */
	cdc_poll();

	/* Flush p2ubuf via CDC.
	 * Query available IN buffer space first (cdc_send with len=0),
	 * then copy exactly that many bytes from the ring buffer and send. */
	if (p2uwpos != p2urpos) {
		static uint8_t txchunk[EP2_SIZE];
		uint16_t space = cdc_send(txchunk, 0);	/* query available space */
		if (space > 0) {
			W avail;
			W i;

			if (p2uwpos > p2urpos)
				avail = p2uwpos - p2urpos;
			else
				avail = BUFFERSIZE - p2urpos;
			if (avail > (W)space)
				avail = (W)space;

			for (i = 0; i < avail; i++) {
				txchunk[i] = p2ubuf[p2urpos++];
				if (p2urpos >= BUFFERSIZE)
					p2urpos = 0;
			}
			cdc_send(txchunk, (uint16_t)avail);
		}
	}

	/* Receive HEX data from CDC */
	{
		uint8_t rbuf[EP2_SIZE];
		uint16_t n = cdc_recv(rbuf, sizeof rbuf);
		uint16_t k;

		for (k = 0; k < n; k++)
			recvrs(rbuf[k]);
	}

	if ((writing))
		return;

	/* UART1 RX */
	if ((U1STAbits.OERR))
		U1STA = 0x1400;
	while ((U1STAbits.URXDA)) {
		W c;
		c = U1RXREG;
		p2udata(c);
	}
}


static void wait1ms(void)
{
	W i;

	for (i = 0; i < 10; i++) {
		idletask();
		wait100us();
	}
}


static void wait200ms(void)
{
	W i;

	for (i = 0; i < 2000; i++) {
		idletask();
		wait100us();
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
	idletask();
#endif
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

/*
 * ICSP low-level: clock one 4-phase cycle
 *   tdi_val: value to present on PGEDx during TDI phase
 *   tms_val: value to present on PGEDx during TMS phase
 *   returns: TDO value captured in phase 0
 *
 * Timing: PGECx minimum period ~200ns (P1), setup ~15ns (P2)
 * At 40MHz PBCLK, one instruction = 25ns, so small delays suffice.
 */

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
	W i;

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
	W i;

	icsp_XferInstruction(0x3c13ff20); /* lui s3, 0xFF20 */

	/* Load address and read memory */
	icsp_XferInstruction(0x3c080000 | addr_hi); /* lui t0, addr_hi */
	icsp_XferInstruction(0x35080000 | addr_lo); /* ori t0, t0, addr_lo */
	icsp_XferInstruction(0x3c090000 | data_hi); /* lui t1, addr_hi */
	icsp_XferInstruction(0x35290000 | data_lo); /* ori t1, t1, addr_lo */
	icsp_XferInstruction(0xad090000); /* sw t1, 0(t0) */
	icsp_XferInstruction(0); /* nop */
}


/*
 * Dump first row (32 words = 128 bytes) at given virtual address
 */
static void icsp_dump_row(const UB *label, UW virt_addr)
{
	W i;

	p2ustr(label);
	p2ustr(" @");
	p2uuw(virt_addr);
	p2ustr(":\r\n");

	for (i = 0; i < 32; i++) {
		UW val = icsp_read_word(virt_addr + i * 4);
		if ((i % 4) == 0) {
			p2ustr("  ");
			p2uuw(virt_addr + i * 4);
			p2ustr(": ");
		}
		p2uuw(val);
		p2udata(' ');
		if ((i % 4) == 3) {
			p2ustr("\r\n");
			while (p2urpos != p2uwpos)
				idletask();
		}
	}
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

	/* MCLR high - device resets fresh */
	LAT_MCLR0 = 1;
	wait200ms();
}


/*
 * Enter Serial Execution mode (required before XferInstruction)
 */
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
/* Main                                                         */
/* ============================================================ */

void main(void)
{
	CNPUA = 0xffff;
	CNPUB = 0xffff;

	CNPDA = 0;
	CNPDB = 0;

	TRISA = 0x0010; /* -------- ---I--OO */
	TRISB = 0x2b84; /* OOI-IOII I-OOOIOO */

	ANSELA = 0;
	ANSELB = 0;

	PORTA = 0xffff;
	PORTB = 0xffff;

	SYSKEY = 0;
	SYSKEY = 0xaa996655;
	SYSKEY = 0x556699aa;
	SYSKEY = 0;

	SYSKEY = 0;
	SYSKEY = 0xaa996655;
	SYSKEY = 0x556699aa;
	SYSKEY = 0;

	U1MODE = 0;
	U1BRG = 86; /* 115.4kbps */
	U1MODE = 0x8008; /* enable N81 4(u2brg + 1) */
	U1STA = 0x1400; /* rx-enable */

	/* USB CDC initialization (replaces UART2) */
	cdc_init();

	for (;;) {
		UW idcode;
		UW status;
		UW addr;
		W timeout;
		W i;

		writebufrsize = 0;
		p2ustr("mclr\r\n");

		RPA0R = 1; /* UTX1 */
		U1RXR = 4; /* RB2 */

		TRIS_PGD0 = 1; /* in */

		LAT_MCLR0 = 0;

		for (i = 0; i < 15000; i++)
			idletask();

		LAT_MCLR0 = 1;

		writing = 0;

		p2ustr("run\r\n");

#if 0
		writebufrsize = 0;
		writebuf[writebufrsize].d[0] = 0x01;
		writebuf[writebufrsize].d[1] = 0x02;
		writebuf[writebufrsize].d[2] = 0x03;
		writebuf[writebufrsize].d[3] = 0x04;
		writebuf[writebufrsize].d[4] = 0x05;
		writebuf[writebufrsize].d[5] = 0x06;
		writebuf[writebufrsize].d[6] = 0x07;
		writebuf[writebufrsize].d[0x80] = 0x08;
		writebuf[writebufrsize].d[0x81] = 0x09;
		writebuf[writebufrsize].addr = 0x1fc00000;
		writebuf[writebufrsize].size = BLOCKSIZE;
		writebufrsize++;
		writebuf[writebufrsize].d[0] = 0xde;
		writebuf[writebufrsize].d[1] = 0xdd;
		writebuf[writebufrsize].addr = 0x1fc00400;
		writebuf[writebufrsize].size = BLOCKSIZE;
		writebufrsize++;
#else
		while (writebufrsize <= 0)
			idletask();
#endif

		p2ustr("writing\r\n");

		RPA0R = 0; /* i/o */
		U1RXR = 0; /* dummy:RA2 */
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
			icsp_exit();
			continue;
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

		/* ---- Enter serial execution ---- */
		dbg_reg("serial exec...", 0);
		if (icsp_enter_serial_exec() < 0) {
			icsp_exit();
			continue;
		}
#if 0
		{
			UW	addr, v0, v1, v2, v3;
			
			for (addr=0xbd000000; addr<0xbd040000; addr+=16) {
				if (addr == 0xbd010000)
					addr = 0xbd03f000;
				v0 = icsp_read_word(addr);
				v1 = icsp_read_word(addr + 4);
				v2 = icsp_read_word(addr + 8);
				v3 = icsp_read_word(addr + 0xc);
				
				if ((addr & 0xfff) == 0)
					;
				else if ((v0 & v1 & v2 & v3) == 0xffffffff)
					continue;
				p2uuw(addr);
				p2ustr(": ");
				p2uuw(v0);
				p2ustr(" ");
				p2uuw(v1);
				p2ustr(" ");
				p2uuw(v2);
				p2ustr(" ");
				p2uuw(v3);
				p2ustr("\r\n");
				while (p2urpos != p2uwpos)
					idletask();
			}
			for (addr=0xbfc00000; addr<0xbfc00c00; addr+=16) {
				v0 = icsp_read_word(addr);
				v1 = icsp_read_word(addr + 4);
				v2 = icsp_read_word(addr + 8);
				v3 = icsp_read_word(addr + 0xc);
				
				if ((v0 & v1 & v2 & v3) == 0xffffffff)
					continue;
				p2uuw(addr);
				p2ustr(": ");
				p2uuw(v0);
				p2ustr(" ");
				p2uuw(v1);
				p2ustr(" ");
				p2uuw(v2);
				p2ustr(" ");
				p2uuw(v3);
				p2ustr("\r\n");
				while (p2urpos != p2uwpos)
					idletask();
			}
		}
#endif
#if 0
		/* ---- Dump #1: before erase ---- */
		p2ustr("=== DUMP 1 (before erase) ===\r\n");
		icsp_dump_row("BFM", 0xBFC00000);
		icsp_dump_row("BFM", 0xBFC00040);
		icsp_dump_row("BFM", 0xBFC00400);
		icsp_dump_row("BFM", 0xBFC00b80);
#endif
#if 0
		{
			static const UW test_vals[] = { 1, 3, 5, 7, 2 };
			UW acc = 0;
			W i;
			
			p2ustr("=== FASTDATA TEST ===\r\n");
			
			icsp_write_word(0xa0000800, 0x8e690000);	/* lw t1, 0(s3) */
			icsp_write_word(0xa0000804, 0);		/* nop */
			icsp_write_word(0xa0000808, 0x01094021);	/* addu t0, t0, t1 */
			icsp_write_word(0xa000080c, 0xae680000);	/* sw t0, 0(s3) */
			icsp_write_word(0xa0000810, 0);		/* nop */
			icsp_write_word(0xa0000814, 0x1000fffa);	/* b -5*/
			icsp_write_word(0xa0000818, 0);		/* nop */
			
			icsp_XferInstruction(0x3c04bf88);	/* setup BMXCON */
			icsp_XferInstruction(0x34842000);
			icsp_XferInstruction(0x3c05001f);
			icsp_XferInstruction(0x34a50040);
			icsp_XferInstruction(0xac850000);
			icsp_XferInstruction(0x34050800);
			icsp_XferInstruction(0xac850010);
			icsp_XferInstruction(0x8c850040);
			icsp_XferInstruction(0xac850020);
			icsp_XferInstruction(0xac850030);
			
			/* Ensure s3 = 0xFF200000 */
			icsp_XferInstruction(0x3c13ff20);	/* lui  s3, 0xff20 */
			icsp_XferInstruction(0x36730000);	/* ori  s3, s3, 0x0000 */
			
			/* t0 = 0 (accumulator) */
			icsp_XferInstruction(0x00004021);	/* addu t0, zero, zero */
			
			icsp_XferInstruction(0x3c1da000);	/* setup stack */
			icsp_XferInstruction(0x37bd2000);
			icsp_XferInstruction(0x3c1aa000);	/* jump */
			icsp_XferInstruction(0x375a0800);
			icsp_XferInstruction(0x03400008);
			icsp_XferInstruction(0);
			
			/*
			 * HOST <-> CPU exchange loop.
			 * Each iteration: HOST writes val, CPU adds to acc, CPU writes acc back.
			 */
			icsp_SendCommand(ETAP_FASTDATA);
			while (p2uwpos != p2urpos)
				idletask();
			for (i = 0; i < (W)(sizeof(test_vals) / sizeof(test_vals[0])); i++) {
				UW write_val = test_vals[i];
				UW read_val;
				
				acc += write_val;
				
				/* HOST -> CPU: write value */
				icsp_XferFastData(write_val);
				
				/* CPU -> HOST: read accumulator */
				read_val = icsp_XferFastData(0);
				
				/* Print result */
				p2ustr("  write=");
				p2uuw(write_val);
				p2ustr(" read=");
				p2uuw(read_val);
				p2ustr(" expect=");
				p2uuw(acc);
				p2ustr(read_val == acc ? " OK\r\n" : " FAIL\r\n");
				while (p2uwpos != p2urpos)
					idletask();
			}
			
			p2ustr("=== FASTDATA TEST DONE ===\r\n");
			for (;;)
				idletask();
		}
#endif

		/* ---- Chip erase ---- */
		/*
		 * Chip erase via MTAP MCHP_ERASE command.
		 * DS60001145 erase sequence:
		 *   1. SW_MTAP
		 *   2. MTAP_COMMAND
		 *   3. XferData8(MCHP_ERASE)   -- no ASSERT_RST needed
		 *   4. Wait ~20ms (MX1: typ 20ms, use 400ms for margin)
		 *   5. Poll MCHP_STATUS until FCBUSY=0
		 *
		 * Note: icsp_enter_serial_exec() leaves TAP in ETAP.
		 * We must switch back to MTAP here.
		 * Do NOT send ASSERT_RST -- it prevents erase from running.
		 */
		dbg_reg("=== CHIP ERASE ===", 0);

		/* Switch to MTAP */
		icsp_SendCommand(MTAP_SW_MTAP);
		icsp_SendCommand(MTAP_COMMAND);

		/* Send erase command (same as icsp_check_and_erase) */
		icsp_XferData8(MCHP_ERASE);
		dbg_reg("erase sent", 0);

		/* Wait for erase to complete */
		wait200ms();
		wait200ms();

		/* Poll FCBUSY until clear */
		timeout = 10000;
		do {
			status = icsp_XferData8(MCHP_STATUS);
			if (!(status & STAT_FCBUSY))
				break;
		} while (timeout-- > 0);
		dbg_reg("post-erase stat", status);

		if (timeout <= 0) {
			p2ustr("FCBUSY stuck after erase\r\n");
			continue;
		}

		dbg_reg("erase done", 0);

		/* ---- Re-enter serial execution for second dump ---- */
		dbg_reg("serial exec #2...", 0);
		if (icsp_enter_serial_exec() < 0) {
			icsp_exit();
			continue;
		}
#if 0
		/* ---- Dump #2: after erase ---- */
		p2ustr("=== DUMP 2 (after erase) ===\r\n");
		icsp_dump_row("BFM", 0xBFC00000);
		icsp_dump_row("BFM", 0xBFC00040);
		icsp_dump_row("BFM", 0xBFC00400);
		icsp_dump_row("BFM", 0xBFC00b80);
#endif
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

			for (i = 0; i < sizeof(pe) / sizeof(pe[0]); i++)
				icsp_write_word(0xa0000800 + sizeof(pe[0]) * i, pe[i]);
		}

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

		/*
		 * HOST <-> CPU exchange loop.
		 * Each iteration: HOST writes val, CPU adds to acc, CPU writes acc back.
		 */
		icsp_SendCommand(ETAP_FASTDATA);

		for (i = 0; i < writebufrsize; i++) {
			struct writebuf_struct *p;
			UW v;
			W j, k;

			p = writebuf + i;
			if (p->addr == 0x1fc00800)
				continue;
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

		for (i = 0; i < writebufrsize; i++) {
			struct writebuf_struct *p;
			UW v;
			W j, k;

			j = 0;
			p = writebuf + i;
			if (p->addr != 0x1fc00800)
				continue;
#if 1
			p->d[0x3fc] |= 3; /* debugger enable */
#endif

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
		icsp_XferFastData(0xffffffff);
		for (i = 0; i < 31; i++)
			icsp_XferFastData(0xffffffff);

		/* ---- Exit ICSP ---- */
		dbg_reg("ICSP: exit", 0);
		icsp_exit();
	}
}
