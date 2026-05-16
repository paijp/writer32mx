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

	/* Flush p2ubuf via CDC: copy up to EP2_SIZE bytes into a static
	 * buffer, then pass that to cdc_send(). */
	if (p2uwpos != p2urpos) {
		static uint8_t txchunk[EP2_SIZE];
		W avail;
		uint16_t sent;
		W i;

		if (p2uwpos > p2urpos)
			avail = p2uwpos - p2urpos;
		else
			avail = BUFFERSIZE - p2urpos;
		if (avail > EP2_SIZE)
			avail = EP2_SIZE;

		for (i = 0; i < avail; i++) {
			txchunk[i] = p2ubuf[p2urpos++];
			if (p2urpos >= BUFFERSIZE)
				p2urpos = 0;
		}
		sent = cdc_send(txchunk, (uint16_t)avail);
		if (sent < (uint16_t)avail) {
			/*
			 * cdc_send could not accept all bytes (IN buffer busy).
			 * Roll back p2urpos so the unsent bytes are retried.
			 */
			W unsent = (W)(avail - sent);
			p2urpos -= unsent;
			if (p2urpos < 0)
				p2urpos += BUFFERSIZE;
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

	/* UART1 RX (unused in CDC mode) */
	if ((U1STAbits.OERR))
		U1STA = 0x1400;
	while ((U1STAbits.URXDA)) {
		W c;
		c = U1RXREG;
		(void)c;
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