static const char time_h[] =
"@(#)$Id: time.h,v 1.1 1996/07/30 16:18:20 john Exp $";
/********************************************************************
 *
 *  	Definitionen der Hardware
 *	(f�r COP identisch zum "normalen" PC)
 *
 *******************************************************************/
/* " time.h	0.00	94/10/08" */

#define CT_0		0x40		/* counter kanal 0 */
#define CT_CMD		0x43 		/* control-byte, write only */

/* control-byte */
struct cmdbyte {
    unsigned	select:		2;		/* select kanal */
    unsigned	read_load:	2;		/* read/load counter */
    unsigned	ct_mode:	3;		/* betriebsart der z�hler */
    unsigned	bcd:		1;		/* bcd- oder 16bit- z�hler */
};

/* control word format */
/* nur 16-bit-Z�hler realisiert (LSB first, MSB last), binary mode */
/* das eigentliche Steuerwort ergibt sich nach Addition der Werte f�r */
/* select und der ct_mode-Definition */
/* select: */
#define COUNTER0	0x30
#define COUNTER1	0x70
#define COUNTER2	0xB0
/* ct_mode (bin�r): */
#define TCOUNT		0x00	/* mode0: interrupt on terminal count */
#define ONESHOT		0x02	/* mode1: programmable one shot */
#define DIVBYN		0x04	/* mode2: div by n counter (rate generator) */
#define SQWAVE		0x06	/* mode3: square wave rate generator */
#define STSTROBE	0x08	/* mode4: software triggered strobe */
#define HTSTROBE	0x0a	/* mode5: hardware triggered strobe */

/* Z�hlwerte */
#define VALUE0_LO	0xa9	/* Z�hler 0: 1ms -> 1193dez bei 1,193 MHz */
#define VALUE0_HI	0x04    /*               -> 4a9 hex */
				/* Interrupt jede Millisekunde */

/*-----------------------------------------------------------------*/
/* Externe Funktionen 						   */
/*-----------------------------------------------------------------*/

extern void tinit(void);
extern void trestore(void);
extern void interrupt far isrtimer(void);	/* isr 8253-Interrupt */
