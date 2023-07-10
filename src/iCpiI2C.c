/********************************************************************
 *
 *	Copyright (C) 2023  John E. Wulff
 *
 *  You may distribute under the terms of either the GNU General Public
 *  License or the Artistic License, as specified in the README file.
 *
 *  For more information about this program, or for information on how
 *  to contact the author, see the README file
 *
 *	iCpiI2C
 *
 *	I/O driver for iCserver in the immediate C environment of a
 *	Raspberry Pi handling real 16 bit inputs and/or 16 bit outputs
 *	for each MCP23017 controller attached to a Raspberry Pi via 8
 *	multiplexed I2C busses of a PCA9548A I2C Switch, or from a number
 *	of direct GPIO pins on the Raspberry Pi or both.
 *
 *	To enable the PCA9548A I2C Mux/Switch in the Linux kernel, enable
 *	I2C in "3 Interface Options" in "raspi-config" and also add the
 *	following lines (without *s) to the /boot/config.txt file:
 *	    [all]
 *	    # Activate the I2C mux in the Linux kernel
 *	    dtoverlay=i2c-mux,pca9548,addr=0x70
 *	This will activate the mux at address 0x70 and create /dev/i2c-11
 *	to /dev/i2c-18 for SD0/SC0 to SD7/SC7 on the mux.
 *
 *	A maximum of 63 MCP23017 controllers can be handled altogether,
 *	8 MCP23017 controllers per multiplexed I2C channel /dev/i2c-11 to
 *	/dev/i2c-17 and 7 MCP23017 controllers for channel /dev/i2c-18.
 *	Each MCP23017 has 16 bit I/Os in two 8 bit registers A and B. Each
 *	bit I/O can be either an input or an output or not used at all.
 *
 *	To concentrate the interrupts from INTA and INTB of all 8 groups
 *	of MCP23017 controllers a special MCP23017 is used at address 0x27
 *	of /dev/i2c-18. The INTA and INTB output of this MCP are mirrored
 *	and are connected to GPIO 27 for interrupt handling.
 *
 *	All GPIO pins on a Raspberry Pi A, B, B+, 3B+ or 4 may be selected
 *	as either input bits or output bits independent of whether MCP23017s
 *	are present or not, except GPIO 2, 3 or 27 if MCP23017s are also
 *	processed. All MCP23017 and GPIO inputs are handled by interrupts.
 *
 *	If no MCP23017 controllers are found or if the program is called
 *	with the -G option, only GPIO pins will be handled.
 *
 *******************************************************************/

#include	<signal.h>
#include	<ctype.h>
#include	<assert.h>
#include	<errno.h>
#include	<sys/stat.h>
#include	<fcntl.h>
#include	<string.h>
#include	<limits.h>

#if !defined(RASPBERRYPI) || !defined(TCP) || defined(_WINDOWS)
#error - must be compiled with RASPBERRYPI and TCP defined and not _WINDOWS
#else	/* defined(RASPBERRYPI) && defined(TCP) && !defined(_WINDOWS) */

#include	"tcpc.h"
#include	"comp.h"		/* defines TSIZE 256 */
#include	"icc.h"			/* declares iC_emalloc() in misc.c */
#include	"rpi_rev.h"		/* Determine Raspberry Pi Board Rev */
#include	"rpi_gpio.h"
#include	"mcp23017.h"
#include	"bcm2835.h"

/********************************************************************
 *  Bit-field to select the particular IEC associated with an input
 *  IXcng or an output QXcng selected by qi, of either Register A or B
 *  selected by g, of the MCP23017 on I2C channel c and device address n.
 *******************************************************************/

typedef struct iec {
    unsigned int qi   : 1;		/* 0 = IX; 1 = QX */
    unsigned int g    : 1;		/* 0 = Register A; 1 = Register B */
    unsigned int n    : 3;		/* 0 = devAdr 0x20; 1 = 0x21; ... 7 = ox27 */
    unsigned int c    : 3;		/* 0 = /dev/int-11; 1 = /dev/int-12; ... 7 = /dev/int-18 */
} iec;

/********************************************************************
 *  There is one mcpDetails structure for each IEC name IXcng or QXcng
 *  associated with an active MCP23017 port (A or B). There are
 *  independent mcpDetails for input IXcng and output Qxcng where the
 *  port A/B selected by the address cng is the same as long as the
 *  bmask values are disjoint. mcpDetails can be expanded by further
 *  IXcng or QXcng arguments as long as the bmask bits in those
 *  arguments have not been set already in either IXcng or QXcng.
 *  Since the number cng in an MCP23017 IEC defines the hardware address
 *  of a particular register, there can be no conflict of IEC names.
 *******************************************************************/

typedef struct	mcpDetails {
    char *		name;		/* IEC name IXcng, IXcng-i, QXcng or QXcng-i */
    int			val;		/* previous input or output value */
    int			i2cFN;		/* file number for I2C channels 11 to 18 - repeated for output */
    int			mcpAdr;		/* MCP23017 address 0x20 to 0x27 - repeated for output */
    int			g;		/* MCP Port A or B - used by output */
    unsigned short	channel;	/* channel to send I or receive Q to/from iCserver -used by output */
    uint8_t		bmask;		/* selected bits for this input or output */
    uint8_t		inv;		/* selected bits in input or output are inverted */
} mcpDetails;

/********************************************************************
 *  Select either an mcpDetails instance for running MCP output or
 *  alternativeley select a gpioIO instance for RPi GPIO outputr.
 *  During command line argument analysis save iec details of a
 *  list argument.
 *******************************************************************/

typedef struct	iecS {
    int			use;
    union {
	mcpDetails *	mdp;		/* use = 0 for MCP output */
	gpioIO *	gep;		/* use = 1 for RPi GPIO */
	iec		cngqi;		/* use = 2 for save of argument lists during command line analysis */
    };
} iecS;

/********************************************************************
 *  The mcpIO structure fully supports all actions on an MCP23017.
 *  It supplies the file descriptor of the I2C channel and dev
 *  address of the MCP23017.
 *  The sub-matrix s[][] supplies mcpDetails for Register A and B
 *  in the MCP with independent input IX and output QX detail for both.
 *******************************************************************/

typedef struct	mcpIO {
    int			i2cFN;		/* file number for I2C channels 11 to 18 */
    int			mcpAdr;		/* MCP23017 address 0x20 to 0x27 */
    mcpDetails		s[2][2];	/* register [A and B][IXn and/or QXn details for that register] */
} mcpIO;

/********************************************************************
 *  mcpIO array mcpL[cn] is organised as follows:
 *    Each element of the array is a struct mcpIO which contains
 *      int i2cFN and int mcpAdr for fast access to the MCP23017
 *      mcpDetails s[g][qi] for Port A selected by g == 0; Port B by g = 1
 *      and output QX for the selected Port by qi = 0; input IX by qi = 1
 *******************************************************************/

static mcpIO *	mcpL[64];		/* array of pointers to MCP23017 info *mcpP */
					/* -1     MCP23017 not found */
					/* NULL   MCP23017 found but not configured */
					/* *mcpP  MCP23017 info configured */
static	int	i2cFdA[8] =		/* file descriptors for open I2C channels */
	    { -1, -1, -1, -1, -1, -1, -1, -1 };
static int	i2cFdCnt   = 0;		/* number of open I2C channels */
static int	gpio27FN = -1;		/* /sys/class/gpio/gpio27/value I2C file number */

static  char **		argp;
static  int		ofs     = 0;
static  int		invFlag = 0;
static fd_set		infds;			/* initialised file descriptor set for normal iC_wait_for_next_event() */
static fd_set		ixfds;			/* initialised extra descriptor set for normal iC_wait_for_next_event() */

static int scanIEC(const char * format, int * ieStartP, int * ieEndP, char * tail, unsigned short * channelP);

static const char *	usage =
"Usage:\n"
" iCpiI2C  [-GBIftmqzh][ -s <host>][ -p <port>][ -n <name>][ -i <inst>]\n"
"          [ -o <offs>][ -d <deb>]\n"
"          [ [~]IXcng[,IXcng,...][,<mask>][-<inst>] ...]\n"
"          [ [~]QXcng[,QXcng,...][,<mask>][-<inst>] ...]\n"
"          [ [~]IXcng-IXcng[,<mask>][-<inst>] ...]\n"
"          [ [~]QXcng-QXcng[,<mask>][-<inst>] ...]\n"
"          [ [~]IXn,<gpio>[,<gpio>,...][-<inst>] ...]\n"
"          [ [~]QXn,<gpio>[,<gpio>,...][-<inst>] ...]\n"
"          [ [~]IXn.<bit>,<gpio>[-<inst>] ...]\n"
"          [ [~]QXn.<bit>,<gpio>[-<inst>] ...]      # at least 1 IEC argument\n"
"          [ -R <aux_app>[ <aux_app_argument> ...]] # must be last arguments\n"
"    -s host IP address of server    (default '%s')\n"
"    -p port service port of server  (default '%s')\n"
"    -i inst instance of this client (default '') or 1 to %d digits\n"
"    -o offs offset for IEC GPIO selection in a second iCpiI2C in the same\n"
"            network (default 0; 2, 4, 6, 8 for extra iCpI2C's - see below)\n"
"            All IXcng and QXcng declarations must use g = offs or offs+1\n"
"    -G      service GPIO I/O only - block MCP23017s and MCP23017 arguments\n"
"    -B      start iCbox -d to monitor active MCP23017 and/or GPIO I/O\n"
"    -I      invert all MCP23017 and GPIO inputs and outputs\n"
"            MCP23017 and GPIO input circuits are normally hi - 1 and\n"
"            when a switch on the input is pressed it goes lo - 0. Therefore\n"
"            it is appropriate to invert inputs and outputs. When inverted\n"
"            a switch pressed on an input generates a 1 for the IEC inputs and\n"
"            a 1 on an IEC output turns a LED and relay on, which is natural.\n"
"            An exception is a relay with an inverting driver, in which case\n"
"            that output must be non-inverting.\n"
"    -W GPIO number used by the w1-gpio kernel module (default 4, maximum 31).\n"
"            When the GPIO with this number is used in this app, iCtherm is\n"
"            permanently blocked to avoid Oops errors in module w1-gpio.\n"
"    -f      force use of all interrupting gpio inputs - in particular\n"
"            force use of gpio 27 - some programs do not unexport correctly.\n"
"\n"
"                      MCP23017 arguments\n"
"          For each MCP23017 connected to a Raspberry Pi two 8 bit registers\n"
"          GPIOA and GPIOB must be configured as IEC inputs (IXcng) or\n"
"          outputs (IXcng) or both, where cng is a three digit number.\n"
"          For MCP23017 IEC base identifiers the number cng defines the\n"
"          complete address of a particular MCP23017 GPIO register:\n"
"           c  is the channel 1 to 8 for /dev/i2c-11 to /dev/i2c-18\n"
"           n  is the address 0 to 7 of a particular MCP23017 for the\n"
"              address range 0x20 to 0x27 selected with A0 to A2\n"
"           g  selects the GPIO register\n"
"              0 2 4 6 8 is GPIOA\n"
"              1 3 5 7 9 is GPIOB\n"
"              normally only g = 0 and 1 is used for GPIOA and GPIOB,\n"
"              but if a second instance of iCpiI2C is used on another\n"
"              Raspberry Pi, but connected to the one iCsever the\n"
"              alternate numbers differentiate between IEC variables\n"
"              for the two RPi's.  Second or more instances of iCpiI2C\n"
"              must be called with the option -o 2, -o 4 etc for\n"
"              using 2 and 3 or 4 and 5 etc to select GPIOA and GPIOB.\n"
"          Both registers GPIOA and GPIOB can have input bits and\n"
"          output bits, but any input bit cannot also be an output bit\n"
"          or vice versa.  Each input or output IEC variable (or list\n"
"          of variables) may be followed by a comma separated number,\n"
"          interpreted as a mask, declaring which bits in the base IEC\n"
"          variable byte are inputs for an input variable or outputs\n"
"          for an output variable. The mask is best written as a binary\n"
"          number eg 0b10001111 for bits 0, 1, 2, 3 and 7.  If no mask\n"
"          is supplied the default mask is 0b11111111 - all 8 bits set.\n"
"    IXcng[,IXcng,...][,<mask>]   list of input variables with the same mask\n"
"    QXcng[,QXcng,...][,<mask>]   list of output variables with the same mask\n"
"    IXcng-IXcng[,<mask>]         range of input variables with the same mask\n"
"    QXcng-QXcng[,<mask>]         range of output variables with the same mask\n"
"         IEC lists and ranges must contain either all inputs or all outputs.\n"
"         The only sensible range is in the digit n for the MCP23017 address.\n"
"\n"
"                      GPIO arguments\n"
"	  Any IEC I/Os IXn.y and QXn.y which are to be linked to GPIO\n"
"	  inputs or outputs must be named in the argument list. The\n"
"	  number n in the IEC must be one or at most two digits to\n"
"	  differentiate GPIO IEC's from three digit MCP23017 IEC's.\n"
"    IXn,<gpio>[,<gpio>,...]\n"
"    QXn,<gpio>[,<gpio>,...]\n"
"          Associate the bits of a particular input or output IEC\n"
"          with a list of gpio numbers.  Up to 8 gpio numbers can\n"
"          be given in a comma separated list. The first gpio number\n"
"          will be associated with bit 0, the second with bit 1 etc\n"
"          and the eighth with bit 7. If the list is shorter than\n"
"          8 the trailing bits are not used. The letter 'd' can be\n"
"          used in the list instead of a gpio number to mark that bit\n"
"          as unused.  eg. IX3,17,18,19,20,d,d,21 associates IX3.0 to\n"
"          IX3.3 with GPIO 17 to GPIO 20 and IX3.6 with GPIO 21.\n"
"          Alternatively:\n"
"    IXn.<bit>,<gpio>\n"
"    QXn.<bit>,<gpio>\n"
"          Each input or output <bit> 0 to 7 is associated with one\n"
"          gpio nunber eg. IX3.0,17 QX3.7,18\n"
"\n"
"                      COMMON extensions\n"
"    ~IEC  Each leading IEC of a list or range may be preceded by a\n"
"          tilde '~', which inverts all IEC inputs or outputs of a\n"
"          list or range.  If all IEC inputs and outputs are already\n"
"          inverted with -I, then ~ inverts them again, which makes them\n"
"          non-inverted. Individual bits for one IEC can be inverted\n"
"          or left normal for GPIO IEC's by declaring the IEC more than\n"
"          once with non-overlapping gpio bit lists or single bits. Also\n"
"          possible for MCP23017 I/O with two non overlapping masks -\n"
"          one inverting, the other non-inverting.\n"
"    IEC-inst Each individual IEC or range can be followed by -<inst>,\n"
"          where <inst> consists of 1 to %d numeric digits.\n"
"          Such an appended instance code takes precedence over the\n"
"          general instance specified with the -i <inst> option above.\n"
"    For a full Description and Technical Background of the MCP23017 I2C\n"
"    and GPIO driver software see the iCpiI2C man page\n"
"\n"
"                      CAVEAT\n"
"    There must be at least 1 MCP23017 or GPIO IEC argument. No IEC\n"
"    arguments are generated automatically for iC[iI2C.\n"
"    Only one instance of iCpiI2C or an app with real IEC parameters\n"
"    like iCpiFace may be run on one RPi and all GPIOs and MCP23017s\n"
"    must be controlled by this one instance. If two instances were\n"
"    running, the common interrupts would clash. Also no other program\n"
"    controlling GPIOs and MCP23017s like 'MCP23017 Digital Emulator'\n"
"    may be run at the same time as this application.  An exception\n"
"    is iCpiPWM which controls GPIOs by DMA and not by interrupts.\n"
"    Another exception is iCtherm which controls GPIO 4 by the 1Wire\n"
"    interface.  Care is taken that any GPIOs or MCP23017s used in one\n"
"    app, iCpiI2C, iCpiPWM or even iCtherm do not clash with another\n"
"    app (using ~/.iC/gpios.used).\n"
"\n"
"                      DEBUG options\n"
#if YYDEBUG && !defined(_WINDOWS)
"    -t      trace arguments and activity (equivalent to -d100)\n"
"         t  at run time toggles gate activity trace\n"
"    -m      display microsecond timing info\n"
"         m  at run time toggles microsecond timing trace\n"
#endif	/* YYDEBUG && !defined(_WINDOWS) */
"    -d deb  +1   trace TCP/IP send actions\n"
"            +2   trace TCP/IP rcv  actions\n"
#ifdef	TRACE
"            +4   trace MCP calls\n"
#endif	/* TRACE */
"            +10  trace I2C  input  actions\n"
"            +20  trace I2C  output actions\n"
#ifdef	TRACE
"            +40  debug MCP calls\n"
#endif	/* TRACE */
"            +100 show arguments\n"
"            +200 show more debugging details\n"
"            +400 exit after initialisation\n"
"    -q      quiet - do not report clients connecting and disconnecting\n"
"    -z      block keyboard input on this app - used by -R\n"
"    -h      this help text\n"
"         T  at run time displays registrations and equivalences in iCserver\n"
"         q  or ctrl+D  at run time stops iCpiI2C\n"
"\n"
"                      AUXILIARY app\n"
"    -R <app ...> run auxiliary app followed by -z and its arguments\n"
"                 as a separate process; -R ... must be last arguments.\n"
"\n"
"Copyright (C) 2023 John E. Wulff     <immediateC@gmail.com>\n"
"Version	$Id: iCpiI2C.c 1.2 $\n"
;

char *		iC_progname;		/* name of this executable */
static char *	iC_path;
short		iC_debug = 0;
#if YYDEBUG && !defined(_WINDOWS)
int		iC_micro = 0;
#endif	/* YYDEBUG && !defined(_WINDOWS) */
unsigned short	iC_osc_lim = 1;

static unsigned short	topChannel = 0;
static char	buffer[BS];
static char	RS[] = "RS";		/* generates RQ for qi 0, SI for qi 1 */
static char	QI[] = "QI";
static char	AB[] = "AB";
static uint8_t	INTF[2] = { INTFA, INTFB };	/* MCP23017 register selection with g = 0 A; g = 1 B */
static uint8_t	GPIO[2] = { GPIOA, GPIOB };
static uint8_t	OLAT[2] = { OLATA, OLATB };
static short	errorFlag = 0;
static iecS *	Channels = NULL;	/* dynamic array to store IEC info mcpDetails* or gpioIO* indexed by channel */
static int	ioChannels = 0;		/* dynamically allocated size of Channels[] */
static long long ownUsed = 0LL;
char		iC_stdinBuf[REPLY];	/* store a line of STDIN - used by MCP23017CAD input */
static int	mcpCnt;
static int	concFd;
static long	convert_nr(const char * str, char ** endptr);
static int	termQuit(int sig);		/* clear and unexport RASPBERRYPI stuff */
int		(*iC_term)(int) = &termQuit;	/* function pointer to clear and unexport RASPBERRYPI stuff */

static void	storeIEC(unsigned short channel, iecS s);
static void	writeGPIO(gpioIO * gep, unsigned short channel, int val);

FILE *		iC_outFP;		/* listing file pointer */
FILE *		iC_errFP;		/* error file pointer */

#define		MC	1
#define		GP	2
#define		DR	4
#define		CONCDEV	0x27

/********************************************************************
 *
 *	Main program
 *
 *******************************************************************/

int
main(
    int		argc,
    char **	argv)
{
    int			fd;
    int			len;
    int			value;
    int			sig;
    int			n;
    int			c;
    int			m;
    mcpIO *		mcp;
    int			m1;
    iec			cngStart;
    iec			cngEnd;
    int			val;
    unsigned short	iid;
    char		iids[6];
    int			regBufLen = 0;
    unsigned short	iidN = USHRT_MAX;	/* internal instance "" initially */
    int			forceFlag = 0;
    char *		iC_fullProgname;
    char *		cp;
    char *		np;
    char *		op;
    int			ol;
    unsigned short	channel = 0;
    int			retval;
    char		tail[128];		/* must match sscanf formats %127s for tail */
    char *		mqz = "-qz";
    char *		mz  = "-z";
    char		dum[2] = { '\0', '\0' };
    int			iqStart;
    int			ieStart = 0;
    int			ieEnd   = 0;
    unsigned short	bit, bitStart, bitEnd;
    unsigned short	gpio;
    unsigned short	directFlag;		/* or MC for MCP23017, GP for GPIO, DR direct for both */
    gpioIO *		gep;
    gpioIO **		gpioLast;
    unsigned short	gpioSav[8];
    unsigned short	u;
    int			mask;
    int			b;
    int			ch;
    int			ns;
    int			gs;
    int			qi;
    int			cn;
    int			devId;
    int			nse;
    int			gpioCnt = 0;
    int			invMask;
    int			gpioTherm;
    char **		argip;
    int			argic;
    long long		gpioMask;
    ProcValidUsed *	gpiosp;

    iC_outFP = stdout;				/* listing file pointer */
    iC_errFP = stderr;				/* error file pointer */

#ifdef	EFENCE
    regBuf = iC_emalloc(REQUEST);
    rpyBuf = iC_emalloc(REPLY);
#endif	/* EFENCE */
    signal(SIGSEGV, iC_quit);			/* catch memory access signal */

    /********************************************************************
     *  By default do not invert MCP23017 and GPIO inputs and outputs.
     *  Both are inverted with -I for all inputs and outputs and '~' for
     *  individual inputs or outputs.
     *******************************************************************/

    invMask = 0x00;
    gpioTherm = 4;				/* default GPIO number used by kernel module w1-gpio */
    argip = iC_emalloc(argc * sizeof(char *));	/* temporary array to hold IEC arguments */
    argic = 0;

    /********************************************************************
     *
     *  Determine program name
     *
     *******************************************************************/

    iC_path = argv[0];			/* in case there is a leading path/ */
    len = strlen(iC_path);
    iC_fullProgname = iC_emalloc(len+1);
    strcpy(iC_fullProgname, iC_path);
    if ((iC_progname = strrchr(iC_path, '/')) == NULL &&
        (iC_progname = strrchr(iC_path, '\\')) == NULL
    ) {
	iC_progname = iC_path;		/* no leading path */
	iC_path     = "";		/* default path */
    } else {
	*iC_progname++ = '\0';		/* path has been stripped and isolated */
    }
    iC_iccNM = iC_progname;		/* generate our own name - not 'stdin' from tcpc.c */
    if (iC_debug & 0200)  fprintf(iC_outFP, "fullPath = '%s' path = '%s' progname = '%s'\n", iC_fullProgname, iC_path, iC_progname);

    /********************************************************************
     *
     *  Process command line arguments
     *
     *******************************************************************/

    while (--argc > 0) {
	if (**++argv == '-') {
	    /********************************************************************
	     *  Analyze -x switch options
	     *******************************************************************/
	    ++*argv;
	    do {
		switch (**argv) {
		    int		df;
		    int		slen;
		case 's':
		    if (! *++*argv) { --argc; if(! *++argv) goto missing; }
		    if (strlen(*argv)) iC_hostNM = *argv;
		    goto break2;
		case 'p':
		    if (! *++*argv) { --argc; if(! *++argv) goto missing; }
		    if (strlen(*argv)) iC_portNM = *argv;
		    goto break2;
		case 'i':
		    if (! *++*argv) { --argc; if(! *++argv) goto missing; }
		    if ((slen = strlen(*argv)) > INSTSIZE ||
			slen != strspn(*argv, "0123456789")) {
			fprintf(iC_errFP, "ERROR: %s: '-i %s' is non numeric or longer than %d digits\n",
			    iC_progname, *argv, INSTSIZE);
			errorFlag++;
		    } else {
			iC_iidNM = *argv;
			iidN = atoi(iC_iidNM);	/* internal instance from command line -i<instance> */
		    }
		    goto break2;
		case 'o':
		    if (! *++*argv) { --argc; if(! *++argv) goto missing; }
		    if (strspn(*argv, "02468") != 1) {
			fprintf(iC_errFP, "ERROR: %s: '-o %s' is not a proper offset (0 2 4 6 8)\n",
			    iC_progname, *argv);
			errorFlag++;
		    } else {
			ofs = **argv - '0';	/* offset for MCP23017 GPIOA/GPIOB selection */
		    }
		    goto break2;
		case 'G':
		    iC_opt_G = 1;	/* GPIO direct action only - block MCP23017 IO, even if MCP23017s present */
		    break;
		case 'B':
		    iC_opt_B = 1;	/* start iCbox -d to monitor active MCP23017 and/or GPIO I/O */
		    break;
		case 'I':
		    invMask = 0xff;	/* invert MCP23017 and GPIO inputs and outputs with -I */
		    iC_opt_P = 1;
		    break;
		case 'W':
		    if (! *++*argv) { --argc; if(! *++argv) goto missing; }
		    gpioTherm = atoi(*argv);
		    if (gpioTherm > 31) {
			fprintf(iC_errFP, "ERROR: %s: -W %hu value > 31\n", iC_progname, gpioTherm);
			goto error;
		    }
		    goto break2;
		case 'f':
		    forceFlag = 1;	/* force use of GPIO interrupts - in particular GPIO 27 */
		    break;
#if YYDEBUG && !defined(_WINDOWS)
		case 't':
		    iC_debug |= 0100;	/* trace arguments and activity */
		    break;
		case 'm':
		    iC_micro++;		/* microsecond info */
		    break;
#endif	/* YYDEBUG && !defined(_WINDOWS) */
		case 'd':
		    if (! *++*argv) { --argc; if(! *++argv) goto missing; }
		    if ((slen = strlen(*argv)) != strspn(*argv, "01234567") ||	/* octal digits only */
			sscanf(*argv, "%o", &df) != 1 ||
			df & ~0777) {
			fprintf(iC_errFP, "ERROR: %s: '-d %s' is not a well formed octal string or exceeds range 777\n",
			    iC_progname, *argv);
			errorFlag++;
		    }
		    iC_debug |= df;	/* short */
		    goto break2;
		case 'q':
		    iC_debug |= DQ;	/* -q    quiet operation of all apps and iCserver */
		    break;
		case 'z':
		    iC_debug |= DZ;	/* -z    block all STDIN interrupts for this app */
		    break;
		case 'R':
		    /********************************************************************
		     *  Run auxiliary app with rest of command line
		     *  splice in the "-z" option to block STDIN interrupts in chained apps
		     *  alternatively "-qz" option for quiet operation and to block STDIN
		     *******************************************************************/
		    if (! *++*argv) { --argc; if(! *++argv) goto missing; }
		    *(argv-1) = *argv;	/* move app string to previous argv array member */
		    *argv = iC_debug & DQ ?  mqz : mz; /* point to "-qz"  or "-z" in current argv */
		    argv--;			/* start call with app string */
		    goto break3;
		missing:
		    fprintf(iC_errFP, "ERROR: %s: missing value after '%s'\n", iC_progname, ((*--argv)-=2, *argv));
		case 'h':
		case '?':
	      error:
		    fprintf(iC_errFP, usage, iC_hostNM, iC_portNM, INSTSIZE, INSTSIZE);
		    exit(-1);
		default:
		    fprintf(iC_errFP, "WARNING: %s: unknown option -%c\n", iC_progname, **argv);
		    break;
		}
	    } while (*++*argv);
	  break2: ;
	} else {
	    /********************************************************************
	     *  Save IEC arguments for later analysis after all options have
	     *  been determined, in particlar -G, and after MCP23017s have
	     *  been counted. IEC arguments and options may be mixed (except -R).
	     *  IEC arguments must start with I ~I Q or ~Q.
	     *  A special case is a lone argument ~, which is converted to the HOME
	     *  environment variable by the Linux shell. It is converted back to ~ here.
	     *******************************************************************/
	    iC_opt_P = 1;	/* using an IEC argument is equivalent to -P option */
	    if (strspn(*argv, "~QI") == 0) {
		if (strcmp(*argv, getenv("HOME")) == 0) {
		    argip[argic++] = "~";	/* transmogrify shell translation of HOME to "~" */
		    continue;			/* get next command line argument */
		}
		fprintf(stderr, "ERROR: %s: invalid argument '%s' (IEC variables start with I ~I Q ~Q)\n", iC_progname, *argv);
		errorFlag++;
	    }
	    argip[argic++] = *argv;
	}
    }
  break3:
    /********************************************************************
     *  if argc != 0 then -R and argv points to auxialliary app + arguments
     *               do not fork and execute aux app until this app has
     *               connected to iCserver and registered all its I/Os
     *******************************************************************/
    if (! errorFlag && iC_opt_P) {
	/********************************************************************
	 *  Open or create and lock the auxiliary file ~/.iC/gpios.used,
	 *  which contains one binary struct ProcValidUsed gpios.
	 *  If rpi.rev exists, gpios.proc and gpios.valid is checked
	 *  against the values returned by boardrev(). (Must be the same)
	 *  gpios.proc is the int returned by boardrev() // Raspberry Pi Board revision
	 *
	 *  gpios.valid has 64 bits - a bit is set for each valid GPIO
	 *
	 *        .... 3333 3322 2222 2222 1111 1111 11
	 *  GPIO  .... 5432 1098 7654 3210 9876 5432 1098 7654 3210
	 *  valid .... ---- ---- 1111 1111 1111 1111 1111 1111 11--
	 *        ....    0    0    f    f    f    f    f    f    c
	 *
	 *  gpios.used has 64 bits - a bit is set for each GPIO used in other apps
	 *  If -f (forceFlag) is set the used word is cleared unconditionally
	 *  Else only GPIO's not used in other apps can be used in this app.
	 *
	 *  For an RPi B+ processor it contains:
	 *  	gpios.proc	1
	 *  	gpios.valid	0x00000000fbc6cf9c
	 *  	gpios.used	0x0000000000000000
	 *
	 *******************************************************************/
	if ((gpiosp = openLockGpios(forceFlag)) == NULL) {
	    fprintf(iC_errFP, "ERROR: %s: in openLockGpios()\n",
		iC_progname);
	    goto FreeNow;
	}
	if (iC_debug & 0200) fprintf(iC_outFP,
	    "Raspberry Pi Board revision = %d (0 = A,A+; 1 = B; 2 = B+,2B)\n"
	    "valid    = 0x%016llx\n",
	    gpiosp->proc, gpiosp->valid);
	/********************************************************************
	 *
	 *  Identify and count MCP23017s available.
	 *
	 *  Do this before checking whether GPIO 27 is not used by other apps.
	 *  GPIO 27 is used during I2C and MCB23017  setup, but is left unchanged
	 * if no MCP23017s is found.
	 *
	 *  If no MCP23017s are found proceed with opt_G set. An error will then occur
	 *  only if an argument requesting MCP23017 IO (Innn or Qnnn) is found.
	 *
	 *  Initialisation of /dev/i2cdev0.0 and/or /dev/i2cdev0.1 for I2C
	 *  modes and read/writeBytes
	 *
	 *  Test for the presence of a MCP23017 by writing to the IOCON register and
	 *  checking if the value returned is the same. If not, there is no MCP23017
	 *  at that address. Do this for all 8 mux channels and 8 MCP's each channel.
	 *
	 *  Initialise all MCP23017s found by writing to all relevant MCB23017
	 *  registers because there may be input interrupts.
	 *
	 *******************************************************************/
	value = mcpCnt = 0;
	for (ch = 0; ch < 8; ch++) {		/* scan /dev/ic2-11 to /dev/i2c-18 */
	    if ((fd = setupI2C(ch+1)) < 0) {
		fprintf(stdout, "ERROR: %s: failed to init I2C communication on channel %d: %s\n",
		    iC_progname, 11+ch, strerror(errno));
		exit(1);
	    }
	    i2cFdCnt++;				/* flags that at least one I2C channel is valid */
	    m = 0;
	    for (ns = 0; ns < 8; ns++) {
		/********************************************************************
		 *  Detect data MCP23017's
		 *******************************************************************/
		cn = ch << 3 | ns;
		if (cn != 077) {
		    if (setupMCP23017(fd, detect, 0x20+ns, IOCON_ODR, 0x00, 0x00) < 0) {
			mcpL[cn] = (void *) -1;
			continue;			/* no MCP23017 at this address */
		    }
		    mcpL[cn] = NULL;
		    m++;				/* count active MCP23017s in this I2C channel */
		    mcpCnt++;			/* count all active MCP23017s */
		} else {
		    /********************************************************************
		     *  Configure last MCP23017 for input interrupt concentration
		     *  unless -G option for RPi GPIO only
		     *******************************************************************/
		    uint8_t in = iC_opt_G ? 0x00 : 0xff;	/* block concentrator interrupts for opt -G */
		    if (setupMCP23017(fd, concentrate, CONCDEV, IOCON_MIRROR, in, in) < 0) {
			fprintf(iC_errFP, "ERROR: %s: no concentrator MCP23017 found at ch 18 dev 0x27\n",
			    iC_progname);
			errorFlag++;
		    }
		    concFd = fd;
		    m++;				/* keep concFd open */
		    if (iC_debug & 0200) fprintf(iC_outFP, "concentrator MCP23017 at ch 18 dev 0x27 configured\n");
		}
	    }
	    if (m == 0) {
		close(fd);				/* no active MCP23017s in this I2C channel */
		fd = -1;
	    }
	    i2cFdA[ch] = fd;			/* save the file descriptor for this I2C channel */
	}
	if (mcpCnt && !iC_opt_G) {
	    /********************************************************************
	     *  MCP23017s found
	     *  Check if GPIO 27 required by MCP23017 has been used in another app
	     *******************************************************************/
	    gpioMask = 0x0800000cLL;		/* gpio 2,3,27 */
	    assert((gpiosp->valid & gpioMask) == gpioMask);	/* assume these are valid for all RPi's */
	    if (iC_debug & 0200) fprintf(iC_outFP,
		"gpio     = 27\n"
		"used     = 0x%016llx\n"
		"gpioMask = 0x%016llx\n",
		gpiosp->u.used, gpioMask);
	    if ((gpioMask & gpiosp->u.used)) {
		gpioMask &= gpiosp->u.used;
		fprintf(iC_errFP, "ERROR: %s: The following GPIO's required by iCpiI2C have been used in another app\n",
		    iC_progname);
		for (b = 2; b <= 27; b++) {
		    if ((gpioMask & (1LL << b))) {
			fprintf(iC_errFP, "		GPIO %d\n", b);
		    }
		}
		fprintf(iC_errFP,
		    "	Cannot run MCP23017s correctly - also stop apps using these GPIOs,\n"
		    "	because they will be interfered with by MCP23017 hardware\n");
		errorFlag++;
		goto UnlockGpios;
	    }
	    /********************************************************************
	     *  Free to use MCP23017s
	     *  Block GPIOs 2,3,27 required by MCP23017 hardware
	     *******************************************************************/
	    gpiosp->u.used |= gpioMask;		/* mark gpio used for ~/.iC/gpios.used */
	    ownUsed        |= gpioMask;		/* mark gpio in ownUsed for termination */
	    /********************************************************************
	     *  Initialisation using the /sys/class/gpio interface to the GPIO
	     *  systems - slightly slower, but always usable as a non-root user
	     *  export GPIO 27, which is mirrored INTA of the MCB23017 concentrator.
	     *
	     *  Wait for Interrupts on GPIO 27 which has been exported and then
	     *  opening /sys/class/gpio/gpio27/value
	     *
	     *  This is actually done via the /sys/class/gpio interface regardless of
	     *  the wiringPi access mode in-use. Maybe sometime it might get a better
	     *  way for a bit more efficiency.	(comment by Gordon Henderson)
	     *
	     *  Set the gpio27/INTA for falling edge interrupts
	     *******************************************************************/
	    if (iC_debug & 0200) fprintf(iC_outFP, "### test gpio_export 27\n");
	    if ((sig = gpio_export(27, "in", "falling", forceFlag, iC_fullProgname)) != 0) {
		iC_quit(sig);			/* another program is using gpio 27 */
	    }
	    /********************************************************************
	     *  open /sys/class/gpio/gpio27/value permanently for obtaining
	     *  out-of-band interrupts
	     *******************************************************************/
	    if ((gpio27FN = gpio_fd_open(27, O_RDONLY)) < 0) {
		fprintf(iC_errFP, "ERROR: %s: MCP23017 open gpio 27: %s\n", iC_progname, strerror(errno));
	    }
	    if (gpio27FN > iC_maxFN) {
		iC_maxFN = gpio27FN;
	    }
	    /********************************************************************
	     *  read the initial /sys/class/gpio/gpio27/value
	     *******************************************************************/
	    if ((value = gpio_read(gpio27FN)) == -1) {
		fprintf(iC_errFP, "ERROR: %s: MCP23017 read gpio 27: %s\n", iC_progname, strerror(errno));
	    }
	    if (iC_debug & 0200) fprintf(iC_outFP, "Initial read 27 = %d\n", value);
	    /********************************************************************
	     *  The INTA output of the interrupt concntrator MCP23017 is configured
	     *  as active high output and does not need a pullup resistor at gpio 27.
	     *******************************************************************/
	} else {
	    iC_opt_G = 1;		/* no MCP23017s - block all MCP23017 command line arguments */
	}
	/********************************************************************
	 *  Match MCP23017s and GPIOs to IEC arguments in the command line.
	 *  invMask = 0x00 initially; invMask = 0xff with option -I
	 *  invFlag = invMask for every new IEC argument, but it is inverted
	 *            locally for every '~' preceding a new IEC argument.
	 *******************************************************************/
	invFlag = invMask;				/* start every IEC argument with correct inversion */
	directFlag = 0;
	for (argp = argip; argp < &argip[argic]; argp++) {
	    directFlag &= DR;				/* clear all bits except DR for each new argument */
	    if (strlen(*argp) >= 128) {
		fprintf(iC_errFP, "ERROR: %s: command line argument too long: '%s'\n", iC_progname, *argp);
		exit(1);
	    }
	    iid = iidN;					/* global instance either 0xffff for "" or 0 - 999 */
	    if (iid == USHRT_MAX) {
		*iids = '\0';	/* track argument instance for error messages */
	    } else {
		snprintf(iids, 6, "-%hu", iid);		/* global instance "-0" to "-999" */
	    }
	    strncpy(tail, *argp, 128);			/* start scans with current argument */
	    /********************************************************************
	     *  Scan for an optional tilde ~ before any IEC argument, which inverts
	     *  the input or output sense of the following IEC argument.
	     *  ~ either buts up to the IEC argument or can be separated by a space,
	     *  although this may cause problems, since the Linux shell converts a
	     *  lone ~ to the home directory string, which is not what we want.
	     *  To act as expected, the HOME directory string is converted back
	     *  to ~ in the first argument scan above.
	     *  More than 1 tilde is a bit silly, but each one inverts the
	     *  output sense - thus keeping the logic correct.
	     *******************************************************************/
	    while ((n = sscanf(tail, "%1[~]%127s", dum, tail)) > 0) {
		invFlag ^= 0xff;			/* invert following argument logic sense */
		if (n == 1) {
		    goto skipInvFlag;			/* lone '~' - apply invFlag to next argument */
		}
	    }
	    mask = 0xff;				/* default is all bits of input or output active */
	    /********************************************************************
	     *  Scan for a mandatory first IXn or QXn starting IEC argument
	     *******************************************************************/
	    switch (scanIEC("%1[QI]X%5[0-9]%127s", &ieStart, &ieEnd, tail, &channel)) {
		case 1:
		    /********************************************************************
		     *  check for ,<mask>
		     *******************************************************************/
		// checkMask:
		    if ((n = sscanf(tail, ",%16[0-9xa-fXA-F]%126s", buffer, tail)) > 0) {
			char * endptr;
			mask = convert_nr(buffer, &endptr);
			if (mask <= 0 || mask > 0xff || *endptr != '\0') {
			    fprintf(iC_errFP, "ERROR: %s: '%s' mask 0x%02x is too small or too large\n", iC_progname, *argp, mask);
			    errorFlag++;
			    goto endOneArg;
			}
			if (n == 1) tail[0] = '\0';
			if (tail[0] == '-') goto checkInst;
		    } else {
			memmove(tail, tail+1, 126);	/* mask == 0b11111111 by default TODO check */
		    }
		    /* fall through */
		case 2:
		    /********************************************************************
		     *  check option -G is not used when MCP23017 IECs have been seen
		     *******************************************************************/
		  checkOptionG:
		    if (ieStart && iC_opt_G) {
			fprintf(iC_errFP, "WARNING: %s: '%s' no MCP23017 IEC arguments allowed with option -G\n", iC_progname, *argp);
			goto endOneArg;
		    }
		    if (iC_debug & 0200) fprintf(iC_outFP, "channel = %d\n", channel);
		    cngStart = Channels[0].cngqi;
		    cngEnd   = Channels[channel == USHRT_MAX ? 1 : channel].cngqi;
		    if (iC_debug & 0200) fprintf(iC_outFP, "%s%cX%u%u%u-%cX%u%u%u,0x%02x%s\n",
			invFlag ? "~" : "",
			QI[cngStart.qi], cngStart.c+1, cngStart.n, cngStart.g+ofs,
			QI[cngEnd.qi], cngEnd.c+1, cngEnd.n, cngEnd.g+ofs,
			mask, iids);
		    directFlag |= MC | DR;
		    break;
		case 3:
		    /********************************************************************
		     *  GPIO IEC arguments with ,gpio,gpio,... or .<bit>,gpio
		     *******************************************************************/
		// processGPIO:
		    assert(channel == 0);
		    iqStart = Channels[0].cngqi.qi;	/* 1 for 'IX', 0 for 'QX' */
		    switch (tail[0]) {			/* test for mask or instance */
			case '.':
			    if (sscanf(tail, ".%hu%127s", &bit, tail) != 2 || bit >= 8 || tail[0] == '-') {
				goto illFormed;	/* must have tail for gpio number */
			    }
			    bitStart = bitEnd = bit;	/* only 1 bit processed */
			    break;
			case ',':
			    bitStart = 0;
			    bitEnd   = 7;		/* process all 8 bits */
			    break;
			default:
			    fprintf(iC_errFP, "ERROR: %s: '%s' GPIO IEC variable without gpio number\n", iC_progname, *argp);
			    errorFlag++;
			    goto endOneArg;
		    }
		    for (bit = 0; bit <= 7; bit++) {
			gpioSav[bit] = 0xffff;	/* initialise empty Save list */
		    }
		    for (bit = bitStart, n = 2; bit <= bitEnd && n == 2; bit++) {
			if (tail[0] == '-') goto checkInst;
			*dum = '\0';
			gpio = 0;
			if (((n = sscanf(tail, ",%hu%127s", &gpio, tail)) < 1 &&
			    (n = sscanf(tail, ",%1[d]%127s", dum, tail)) < 1) ||
			    gpio > 55) {
			    goto illFormed;
			}
			if (n == 1) tail[0] = '\0';
			directFlag |= GP | DR;
			if (*dum == '\0') {
			    /********************************************************************
			     *  TODO Check later that a GPIO is not used twice for different IEC's.
			     *  Also check later that a bit in an IEC of a particular instance
			     *  is not linked with a GPIO more than once.
			     *******************************************************************/
			    gpioSav[bit] = gpio;	/*  Save list of GPIO numbers for later building */
			    if (iC_debug & 0200) fprintf(iC_outFP, "%s%cX%d.%hu	gpio = %hu\n",
				invFlag ? "~" : "", QI[iqStart], ieEnd, bit, gpio);
			}
		    }
		    if (n == 2 && tail[0] == ',') {
			fprintf(iC_errFP, "ERROR: %s: '%s' gpio list limited to 8 comma separated numbers or 1 if .bit\n", iC_progname, *argp);
			errorFlag++;
			goto endOneArg;
		    }
		    /* fall through */
		case 4:
		    /********************************************************************
		     *  check instance -0 to -999
		     *******************************************************************/
		  checkInst:
		    if (tail[0] == '-') {
			if (n != 1 && (n = sscanf(tail, "-%3hu%127s", &iid, tail)) == 1) {	/* individual instance has precedence */
			    snprintf(iids, 6, "-%hu", iid);	/* instance "-0" to "-999" */
			} else {
			    goto illFormed;
			}
		    } else if (tail[0] != '\0') {
			goto illFormed;			/* a tail other than -instance eg -999 */
		    }
		    if (iC_debug & 0200) fprintf(iC_outFP, "### iids = '%s'\n", iids);
		    if (ieEnd > 99) {
			goto checkOptionG;		/* MCP23017 IEC variable */
		    }
		    break;
		case 5:
		    /********************************************************************
		     *  ill formed argument
		     *******************************************************************/
		  illFormed:
		    fprintf(iC_errFP, "ERROR: %s: '%s' is not a well formed IEC, IEC list or IEC range\n", iC_progname, tail);
		    errorFlag++;
		    /* fall through */
		case 6: goto endOneArg;
		default: assert(0);
	    }
	    /********************************************************************
	     *  Use the IEC argument to extend MCP23017 or GPIO control structures
	     *  Check IEC argument was not used before
	     *******************************************************************/
	    if (directFlag & MC) {
		assert((directFlag & GP) == 0);		/* should not have both */
		if (iC_debug & 0200) fprintf(iC_outFP, "MCP23017 IEC found; channel = %hu\n", channel);
		/********************************************************************
		 *  Process MCP23017 IEC arguments
		 *******************************************************************/
		if (channel == USHRT_MAX) {
		    /********************************************************************
		     *  Turn a two IEC unit range into a list
		     *******************************************************************/
		    iecS	sel;
		    channel = 0;
		    iec cngqi;
		    assert(Channels[0].use == 2 || Channels[1].use == 2);
		    cngqi = Channels[1].cngqi;
		    nse = cngqi.n;			/* end of range */
		    for (ns = Channels[0].cngqi.n + 1; ns <= nse ; ns++) {
			cngqi.n = ns;			/* modify the n in the IEC name */
			sel.use = 2;
			sel.cngqi = cngqi;
			storeIEC(++channel, sel);	/* extend range into a list */
		    }
		}
		/********************************************************************
		 *  Further MCP processing only deals with lists of IEC arguments
		 *******************************************************************/
		if (iC_debug & 0200) {
		    fprintf(iC_outFP, "%s", invFlag ? "~" : "");
		    for (ch = 0; ch <= channel; ch++) {
			fprintf(iC_outFP, "%cX%u%u%u,",
			    QI[Channels[ch].cngqi.qi], Channels[ch].cngqi.c+1,
			    Channels[ch].cngqi.n, Channels[ch].cngqi.g+ofs);
		    }
		    fprintf(iC_outFP, "0x%02x%s\n", mask, iids);
		}
		/********************************************************************
		 *  Initial processing of the the IEC's in one command line argument
		 *******************************************************************/
		for (ch = 0; ch <= channel; ch++) {
		    iec			cngqi;
		    mcpDetails *	mdp;
		    mcpDetails *	mip;
		    char		temp[TSIZE];
		    unsigned int	c, n, g, qi, nqi, cn;
		    cngqi = Channels[ch].cngqi;
		    c = cngqi.c;
		    n = cngqi.n;
		    g = cngqi.g;
		    qi = cngqi.qi;
		    nqi = qi ? 0 : 1;			/* ~qi */
		    cn = c << 3 | n;			/* index into mcpL[] array */
		    mcp = mcpL[cn];
		    snprintf(temp, TSIZE, "%cX%u%u%u%s", QI[qi], c+1, n, g+ofs, iids);
		    if (mcp == (void *) -1) {
			fprintf(iC_errFP, "ERROR: %s: '%s' MCP23017 %s does not exist\n", iC_progname, *argp, temp);
			errorFlag++;
			continue;
		    }
		    if (mcp == NULL || mcp->s[g][qi].name == NULL) {
			if (mcp == NULL) {
			    if (iC_debug & 0200) fprintf(iC_outFP, "configure new MCP %s\n", temp);
			    mcpL[cn] = mcp = iC_emalloc(sizeof(mcpIO));	/* new mcpIO element */
			    mcp->i2cFN = i2cFdA[c];
			    mcp->mcpAdr = 0x20 + n;
			}
			if (iC_debug & 0200) fprintf(iC_outFP, "configure register %c\t", AB[g]);
			mdp = &mcp->s[g][qi];
			mip = &mcp->s[g][nqi];
			mdp->name = iC_emalloc(strlen(temp)+1);		/* +1 for '\0' */
			strcpy(mdp->name, temp);
			mdp->val = 0;			/* iC channel assigned after registration with iCserver */
			if (mip->bmask & mask) {	/* test clash with bmask in alternate IEC for this register */
			    fprintf(iC_errFP, "ERROR: %s: '%s' bit mask clash for %s,0x%02x - previous mask %s,0x%02x\n",
				iC_progname, *argp, temp, mask, mip->name, mip->bmask);
			    errorFlag++;
			    continue;
			}
			mdp->bmask = mask;		/* defines output or input bits in this IEC declaration - may be extended */
			mdp->inv = invFlag ? mask : 0;	/* the same IEC can have non-inverting and inverting bits */
			if (iC_debug & 0200) fprintf(iC_outFP, "%s%s, bmask = 0x%02x inv = 0x%02x\n", invFlag ? "~" : "", temp, mdp->bmask, mdp->inv);
		    } else {
			if (iC_debug & 0200) fprintf(iC_outFP, "extend register %c\t", AB[g]);
			mdp = &mcp->s[g][qi];		/* for arguments in a list mcp or g are changed */
			mip = &mcp->s[g][nqi];
			if (strcmp(mdp->name, temp)) {	/* test for change of instance in name */
			    fprintf(iC_errFP, "ERROR: %s: '%s' %s differs to previous name %s because of instance change\n",
				iC_progname, *argp, temp, mdp->name);
			    errorFlag++;
			    continue;
			}
			if ((mdp->bmask | mip->bmask) & mask) {	/* test clash with bmask in both IX and QX */
			    fprintf(iC_errFP, "ERROR: %s: '%s' bit mask clash for %s,0x%02x - previous mask %s,0x%02x | %s,0x%02x\n",
				iC_progname, *argp, temp, mask, mdp->name, mdp->bmask, mip->name, mip->bmask);
			    errorFlag++;
			    continue;
			}
			mdp->bmask |= mask;		/* extend output or input bit definitions in this IEC declaration */
			mdp->inv |= invFlag ? mask : 0;	/* the same IEC can have non-inverting and inverting bits */
			if (iC_debug & 0200) fprintf(iC_outFP, "%s%s, bmask = 0x%02x inv = 0x%02x\n", invFlag ? "~" : "", temp, mdp->bmask, mdp->inv);
		    }
		}
	    } else if (directFlag & GP) {
		/********************************************************************
		 *  Build or extend a new gpioIO structure and store in iC_gpL[iqStart]
		 *      iqStart 0 = 'Q', 1 = 'I'
		 *  temporarily store ieEnd (IEC number) in val of iqDetails
		 *  temporarily store iid (instance) in channel of iqDetails
		 *  Check that the GPIO for the IEC argument is valid for this RPi
		 *  and was not used before in this or another app.
		 *******************************************************************/
		gpioLast = &iC_gpL[iqStart];	/* scan previous gpio list */
		for (gep = iC_gpL[iqStart]; gep; gep = gep->nextIO) {
		    if (gep->Gval == ieEnd &&
			gep->Gchannel == iid) {
			goto previousElement;	/* already partially filled */
		    }
		    gpioLast = &gep->nextIO;
		}
		*gpioLast = gep = iC_emalloc(sizeof(gpioIO));	/* new gpioIO element */
		gep->Gname = NULL;		/* GPIO IEC in or output not active */
		gep->Gval = ieEnd;
		gep->Gchannel = iid;		/* temporary IEC details in new element */
		for (bit = 0; bit <= 7; bit++) {
		    gep->gpioNr[bit] = 0xffff;	/* mark gpio number unused */
		    gep->gpioFN[bit] = -1;	/* mark file number unused */
		}
		gpioCnt++;
	      previousElement:
		for (bit = 0; bit <= 7; bit++) {
		    if((gpio = gpioSav[bit]) != 0xffff) {
			mask = iC_bitMask[bit];
			assert(gpio <= 55);
			gpioMask = 1LL << gpio;		/* gpio is 0 - 55 max */
			if (iC_debug & 0200) fprintf(iC_outFP,
			    "gpio     = %hu\n"
			    "used     = 0x%016llx\n"
			    "gpioMask = 0x%016llx\n",
			    gpio, gpiosp->u.used, gpioMask);
			if ((u = gep->gpioNr[bit]) != 0xffff &&
			    ((mask & gep->Ginv) != (mask & invFlag) ||
			    (u != gpio))) {		/* ignore 2nd identical definition */
			    fprintf(iC_errFP, "ERROR: %s: %c%cX%d.%hu,%hu%s cannot override %c%cX%d.%hu,%hu%s\n",
				iC_progname,
				invFlag & mask ? '~' : ' ', QI[iqStart], ieEnd, bit, gpio, iids,
				gep->Ginv & mask ? '~' : ' ', QI[iqStart], ieEnd, bit, gep->gpioNr[bit], iids);
			    errorFlag++;
			} else if (!(gpiosp->valid & gpioMask)) {
			    fprintf(iC_errFP, "ERROR: %s: trying to use invalid gpio %hu on %cX%d.%hu%s on RPi board rev %d\n",
				iC_progname, gpio, QI[iqStart], ieEnd, bit, iids, gpiosp->proc);
			    errorFlag++;
			} else if (gpiosp->u.used & gpioMask) {
			    fprintf(iC_errFP, "ERROR: %s: trying to use gpio %hu a 2nd time on %c%cX%d.%hu%s\n",
				iC_progname, gpio, invFlag & mask ? '~' : ' ', QI[iqStart], ieEnd, bit, iids);
			    errorFlag++;
			} else {
			    gep->Ginv |= mask & invFlag;/* by default do not invert GPIO in or outputs - inverted with -I or '~' */
			    gpiosp->u.used |= gpioMask;	/* mark gpio used for ~/.iC/gpios.used */
			    ownUsed        |= gpioMask;	/* mark gpio in ownUsed for termination */
			    gep->gpioNr[bit] = gpio;	/* save gpio number for this bit */
			    gep->Gbmask |= iC_bitMask[bit];	/* OR hex 01 02 04 08 10 20 40 80 */
			    /********************************************************************
			     *  Check if GPIO number is used by w1-gpio kernel module to drive iCtherm
			     *  If yes and bit 0 (OopsMask) has not been set in gpios->u.oops before
			     *  remove w1-therm and w1-gpio and set bit 0 to block iCtherm
			     *******************************************************************/
			    if (gpio == gpioTherm) {
				strncpy(buffer, "sudo modprobe -r w1-therm w1-gpio", BS);	/* remove kernel modules */
				if (iC_debug & 0200) fprintf(iC_outFP, "%s\n", buffer);
				if ((b = system(buffer)) != 0) {
				    perror("sudo modprobe");
				    fprintf(iC_errFP, "WARNING: %s: system(\"%s\") could not be executed $? = %d - ignore\n",
					iC_progname, buffer, b);
				}
				gpiosp->u.oops |= OopsMask;	/* block iCtherm permanently */
			    }
			}
		    }
		}
	    }
	  endOneArg:
	    invFlag = invMask;				/* start new tilde '~' analysis for next argument */
	  skipInvFlag:;
	}
      UnlockGpios:
	if (iC_debug & 0200) fprintf(iC_outFP, "used     = 0x%016llx\n"
					       "oops     = 0x%016llx\n", gpiosp->u.used, gpiosp->u.oops);
	if (writeUnlockCloseGpios() < 0) {
	    fprintf(iC_errFP, "ERROR: %s: in writeUnlockCloseGpios()\n",
		iC_progname);
	    errorFlag++;
	}
    }
  FreeNow:
    free(argip);
    if (errorFlag) {
	iC_quit(-3);					/* after all the command line has been analyzed */
    }
    if (directFlag == 0) {
	fprintf(iC_errFP, "ERROR: %s: no IEC arguments? there must be at least 1 MCP23017 or GPIO argument\n", iC_progname);
	iC_quit(-4);					/* call termQuit() to terminate I/O */
    }
    if (iC_opt_G) mcpCnt = 0;				/* ignore all MCP23017s and their IECs */
    /********************************************************************
     *  Generate IEC names for all GPIO output and input arguments
     *  Do the same for iC_gpL[0] and iC_gpL[1]
     *******************************************************************/
    for (qi = 0; qi < 2; qi++) {
	for (gep = iC_gpL[qi]; gep; gep = gep->nextIO) {
	    len = snprintf(buffer, BS, "%cX%d", QI[qi], gep->Gval);	/* IEC name without instance */
	    if ((iid = gep->Gchannel) != 0xffff) {			/* append possible instance */
		len += snprintf(buffer + len, BS, "-%hu", iid);
	    }
	    gep->Gname = iC_emalloc(++len);
	    strcpy(gep->Gname, buffer);		/* store name */
	}
    }
    /********************************************************************
     *  Do the same for iC_gpL[0] and iC_gpL[1]
     *******************************************************************/
    for (qi = 0; qi < 2; qi++) {
	for (gep = iC_gpL[qi]; gep; gep = gep->nextIO) {
	    assert(gep->Gname);		/* later scans rely on a name */
	    gep->Gval = 0;		/* restore values */
	    gep->Gchannel = 0;
	}
    }
    /********************************************************************
     *  End of MCP23017 detection
     *******************************************************************/
    for (ch = 0; ch < 8; ch++) {		/* scan /dev/ic2-11 to /dev/i2c-18 */
	if (i2cFdA[ch] == -1) {
	    continue;				/* no MCP23017s on this I2C channel */
	}
	for (ns = 0; ns < 8; ns++) {
	    /********************************************************************
	     *  Setup MCP23017's
	     *******************************************************************/
	    cn = ch << 3 | ns;
	    mcp = mcpL[cn];
	    if (mcp == (void *) -1 || mcp == NULL) {
		continue;			/* no MCP23017 at this address or not matched by an IEC */
	    }					/* this includes the concentrator MCP */
	    fd = mcp->i2cFN;
	    assert(fd > 0);
	    devId = mcp->mcpAdr;
	    assert(devId == 0x20 + ns);
	    if (iC_debug & 0200) fprintf(iC_outFP, "=== ch = %d ns = %d cn = %02o fd = %d devId = 0x%2x\n", ch, ns, cn, fd, devId);
	    /********************************************************************
	     *  Any MCP23017 that has been configured has 4 possible IECs
	     *    s[0][0] is mcpDetails for a possible output QXcng for Port A
	     *    s[0][1] is mcpDetails for a possible input  IXcng for Port A
	     *    s[1][0] is mcpDetails for a possible output QXcng for Port B
	     *    s[1][1] is mcpDetails for a possible input  IXcng for Port B
	     *  Any of the 4 IECs that have not been used in the command line
	     *  have mcpDetails wich are all 0's. This means that any MCP registers
	     *  which have no input bits have bmask = 0x00 and inv = 0x00.
	     *  These values are the default values for MCP23017 setup of the
	     *  IODIR and IPOL registers (output and no logic inversion) and can
	     *  be written without testing if the mcpDetails have a name pointer.
	     *******************************************************************/
	    if (setupMCP23017(fd, data, devId, IOCON_ODR, mcp->s[0][1].bmask, mcp->s[1][1].bmask) < 0) {
		assert(0);			/* no MCP23017 at this address ?? was previously detected */
	    }
	    writeByte(fd, devId, IPOLA, mcp->s[0][1].inv);	/* Port A input bit polarity - TODO merge to setupMCP23017 */
	    writeByte(fd, devId, IPOLB, mcp->s[1][1].inv);	/* Port B input bit polarity */
	}
    }
    /********************************************************************
     *  Export and open all gpio files for all GPIO arguments
     *  open /sys/class/gpio/gpioN/value permanently for obtaining
     *  out-of-band interrupts and to allow read and write operations
     *******************************************************************/
    for (qi = 0; qi < 2; qi++) {
	for (gep = iC_gpL[qi]; gep; gep = gep->nextIO) {
	    for (bit = 0; bit <= 7; bit++) {
		if ((gpio = gep->gpioNr[bit]) != 0xffff) {
		    if (qi == 0) {
			/********************************************************************
			 *  QXn gpio N export with direction out and no interrupts
			 *******************************************************************/
			if ((sig = gpio_export(gpio, "out", "none", forceFlag, iC_fullProgname)) != 0) {
			    iC_quit(sig);		/* another program is using gpio N */
			}
			if ((gep->gpioFN[bit] = gpio_fd_open(gpio, O_RDWR)) < 0) {
			    fprintf(iC_errFP, "ERROR: %s: Cannot open GPIO %d for output: %s\n", iC_progname, gpio, strerror(errno));
			} else {
			    gpio_write(gep->gpioFN[bit], 0);	/* write 0 to GPIO to initialise it */
			}
		    } else {
			/********************************************************************
			 *  IXn gpio N export with direction in and interrupts on both edges
			 *******************************************************************/
			if ((sig = gpio_export(gpio, "in", "both", forceFlag, iC_fullProgname)) != 0) {
			    iC_quit(sig);		/* another program is using gpio N */
			}
			if ((gep->gpioFN[bit] = gpio_fd_open(gpio, O_RDONLY)) < 0) {
			    fprintf(iC_errFP, "ERROR: %s: Cannot open GPIO %d for input: %s\n", iC_progname, gpio, strerror(errno));
			}
			/********************************************************************
			 *  Execute the SUID root progran iCgpioPUD(gpio, pud) to set pull-up
			 *  to 3.3 volt. It is not recommended to connect a GPIO input to 5 volt,
			 *  although I have never had a problem doing so. JW 2023-07-04
			 *  Previously (before version 1.2) the following was done:
			 *    for normal   input (logic 0 = low)  set pull down pud = 1
			 *    for inverted input (logic 0 = high) set pull up   pud = 2
			 *  That required pulling the input high for normal input, which is
			 *  awkward, especially if you should only use 3.3 volt, not 5 volt.
			 *  Now it is always pull up  pud = 2. GPIO inputs are activated by
			 *  pulling them down to 0 volts, which is the same as MCP23017 inputs.
			 *******************************************************************/
			iC_gpio_pud(gpio, BCM2835_GPIO_PUD_UP);
		    }
		    if (gep->gpioFN[bit] > iC_maxFN) {
			iC_maxFN = gep->gpioFN[bit];
		    }
		} else if (qi == 1) {
		    gep->Ginv &= ~iC_bitMask[bit];	/* blank out missing GPIO input bits IXx.y */
		}
	    }
	}
    }
    /********************************************************************
     *  Generate a meaningful name for network registration
     *******************************************************************/
    len = snprintf(buffer, BS, "%s", iC_iccNM);
    if (mcpCnt == 0) {
	len += snprintf(buffer+len, BS-len, "_G");		/* name GPIO marker */
    }
    if (*iC_iidNM) {
	len += snprintf(buffer+len, BS-len, "-%s", iC_iidNM);	/* name-instance header */
    }
    iC_iccNM = iC_emalloc(len + 1);
    strcpy(iC_iccNM, buffer);
#if YYDEBUG && !defined(_WINDOWS)
    if (iC_micro) iC_microReset(0);		/* start timing */
#endif	/* YYDEBUG && !defined(_WINDOWS) */
    if (iC_debug & 0200) fprintf(iC_outFP, "host = %s, port = %s, name = %s\n", iC_hostNM, iC_portNM, iC_iccNM);
    if (iC_debug & 0400) iC_quit(0);
    signal(SIGINT, iC_quit);			/* catch ctrlC and Break */
#ifdef	SIGTTIN
    /********************************************************************
     *  The following behaviour was observed on Linux kernel 2.2
     *  When this process is running in the background, and a command
     *  (even a command which does not exist) is run from the bash from
     *  the same terminal on which this process was started, then this
     *  process is sent signal SIGTTIN. If the signal SIGTTIN is ignored,
     *  then stdin receives EOF (a few thousand times) every time a
     *  character is entered at the terminal.
     *
     *  To prevent this process from hanging when run in the background,
     *  SIGTTIN is ignored, and when the first EOF is received on stdin,
     *  FD_CLR(0, &infds) is executed to stop selects on stdin from then on.
     *  This means, that stdin can still be used normally for the q, t and
     *  m command when the process is run in the foreground, and behaves
     *  correctly (does not STOP mysteriously) when run in the background.
     *
     *  This means that such a process cannot be stopped with q, only with
     *  ctrl-D, when it has been brought to the foreground with fg.
     *******************************************************************/
    signal(SIGTTIN, SIG_IGN);			/* ignore tty input signal in bg */
#endif	/* SIGTTIN */

    /********************************************************************
     *
     *  All arguments have been taken care of
     *
     *  Start TCP/IP communication, build registration string and register
     *
     *******************************************************************/

    iC_sockFN = iC_connect_to_server(iC_hostNM, iC_portNM);
    if (iC_sockFN > iC_maxFN) {
	iC_maxFN = iC_sockFN;
    }

    regBufLen = REQUEST;			/* really ready to copy iecId's into regBuf  Nname...,Z\n */
    len = snprintf(regBuf, regBufLen, "N%s", iC_iccNM);	/* name header */
    cp = regBuf + len;
    regBufLen -= len;
    if (mcpCnt) {
	/********************************************************************
	 *  Generate registration string made up of all active MCP23017 I/O names
	 *  There are either 2 input or 2 output names or both per active MCP23017
	 *******************************************************************/
	for (ch = 0; ch < 8; ch++) {		/* scan /dev/ic2-11 to /dev/i2c-18 */
	    if (i2cFdA[ch] == -1) {
		continue;			/* no MCP23017s on this I2C channel */
	    }
	    for (ns = 0; ns < 8; ns++) {
		/********************************************************************
		 *  Scan MCP23017's mcpDetails for names
		 *******************************************************************/
		cn = ch << 3 | ns;
		mcp = mcpL[cn];
		if (mcp == (void *) -1 || mcp == NULL) {
		    continue;			/* no MCP23017 at this address or not matched by an IEC */
		}
		assert(regBufLen > ENTRYSZ);	/* accomodates ",SIX123456,RQX123456,Z" */
		for (gs = 0; gs < 2; gs++) {
		    for (qi = 0; qi < 2; qi++) {
			if ((np = mcp->s[gs][qi].name) != NULL) {
			    assert(*np == QI[qi]);
			    len = snprintf(cp, regBufLen, ",%c%s", RS[qi], np);
			    cp += len;		/* input send name or output receive name */
			    regBufLen -= len;
			}
		    }
		}
	    }
	}
    }
    /********************************************************************
     *  Extend registration string with all active GPIO I/O names
     *******************************************************************/
    for (qi = 0; qi < 2; qi++) {
	for (gep = iC_gpL[qi]; gep; gep = gep->nextIO) {
	    np = gep->Gname;
	    assert(regBufLen > ENTRYSZ);	/* accomodates ",SIX123456,RQX123456,Z" */
	    len = snprintf(cp, regBufLen, ",%c%s", RS[qi], np);
	    cp += len;				/* input send name or output receive name */
	    regBufLen -= len;
	}
    }
    /********************************************************************
     *  Send registration string made up of all active I/O names - TODO support registration strings > 1400 bytes
     *******************************************************************/
    if (iC_debug & 0200)  fprintf(iC_outFP, "regBufLen = %d\n", regBufLen);
    snprintf(cp, regBufLen, ",Z");		/* Z terminator */
    if (iC_debug & 0200)  fprintf(iC_outFP, "register:%s\n", regBuf);
#if YYDEBUG && !defined(_WINDOWS)
    if (iC_micro) iC_microPrint("send registration", 0);
#endif	/* YYDEBUG && !defined(_WINDOWS) */
    iC_send_msg_to_server(iC_sockFN, regBuf);		/* register IOs with iCserver */
    /********************************************************************
     *  Receive a channel number for each I/O name sent in registration
     *  Distribute read and/or write channels returned in acknowledgment
     *******************************************************************/
    if (iC_rcvd_msg_from_server(iC_sockFN, rpyBuf, REPLY) != 0) {	/* busy wait for acknowledgment reply */
	iecS	sel;				/* can store either NULL or mcpDetails* mdp or gpioIO* gep */
#if YYDEBUG && !defined(_WINDOWS)
	if (iC_micro) iC_microPrint("ack received", 0);
#endif	/* YYDEBUG && !defined(_WINDOWS) */
	if (iC_debug & 0200) fprintf(iC_outFP, "reply:%s\n", rpyBuf);
	if (iC_opt_B) {				/* prepare to execute iCbox -d */
	    len = snprintf(buffer, BS, "iCbox -dz%s -n %s-DI", (iC_debug & DQ) ? "q" : "", iC_iccNM);
	    b = 4;				/* initial number of tokens in buffer */
	    op = buffer + len;			/* optional host and port appended in iC_fork_and_exec() */
	    ol = BS - len;
	}
	cp = rpyBuf - 1;	/* increment to first character in rpyBuf in first use of cp */
	/********************************************************************
	 *  iC channels for MCP23017 acknowledgments
	 *******************************************************************/
	for (ch = 0; ch < 8; ch++) {		/* scan /dev/ic2-11 to /dev/i2c-18 */
	    if (i2cFdA[ch] == -1) {
		continue;			/* no MCP23017s on this I2C channel */
	    }
	    for (ns = 0; ns < 8; ns++) {
		/********************************************************************
		 *  Scan MCP23017's mcpDetails for names
		 *******************************************************************/
		cn = ch << 3 | ns;
		mcp = mcpL[cn];
		if (mcp == (void *) -1 || mcp == NULL) {
		    continue;			/* no MCP23017 at this address or not matched by an IEC */
		}
		assert(regBufLen > ENTRYSZ);	/* accomodates ",SIX123456,RQX123456,Z" */
		for (gs = 0; gs < 2; gs++) {
		    for (qi = 0; qi < 2; qi++) {
			mcpDetails *	mdp;
			mdp = &mcp->s[gs][qi];
			if ((np = mdp->name) != NULL) {
			    assert(cp);			/* not enough channels in ACK string */
			    channel = atoi(++cp);	/* read channel from ACK string */
			    assert(channel > 0);
			    if (channel > topChannel) {
				topChannel = channel;
			    }
			    if (iC_debug & 0200) fprintf(iC_outFP, "MCP23017 %s on channel %hu\n", np, channel);
			    mdp->channel = channel;	/* link send channel to MCP23017 (ignore receive channel) */
			    mdp->i2cFN   = mcp->i2cFN;	/* repeat mcp members for fast access in output */
			    mdp->mcpAdr  = mcp->mcpAdr;
			    mdp->g       = gs;
			    sel.use  = 0;		/* select mcpDetails* */
			    sel.mdp = mdp;
			    storeIEC(channel, sel);	/* link mcpDetails to channel */
			    if (iC_opt_B) {
				assert(ol > 10);
				len = snprintf(op, ol, " %s", np);	/* add I/O name to execute iCbox -d */
				b++;			/* count tokens in buffer */
				op += len;
				ol -= len;
				if ((mask = mdp->bmask) != 0xff) {
				    len = snprintf(op, ol, ",0x%02x", mask);	/* mask out any MCP23017 bits not used */
				    op += len;
				    ol -= len;
				}
			    }
			    cp = strchr(cp, ',');
			}
		    }
		}
	    }
	}
	/********************************************************************
	 *  iC channels for GPIO acknowledgments
	 *******************************************************************/
	for (qi = 0; qi < 2; qi++) {
	    for (gep = iC_gpL[qi]; gep; gep = gep->nextIO) {
		np = gep->Gname;		/* not NULL assert previously */
		assert(cp);			/* not enough channels in ACK string */
		channel = atoi(++cp);		/* read channel from ACK string */
		assert(channel > 0);
		if (channel > topChannel) {
		    topChannel = channel;
		}
		if (iC_debug & 0200) {
		    fprintf(iC_outFP, "GPIO");
		    for (bit = 0; bit <= 7; bit++) {
			if ((gpio = gep->gpioNr[bit]) != 0xffff) {
			    fprintf(iC_outFP, ",%hu", gpio);
			} else {
			    fprintf(iC_outFP, ",d");
			}
		    }
		    fprintf(iC_outFP, "	%s  on channel %hu\n", np, channel);
		}
		gep->Gchannel = channel;	/* link send channel to GPIO (ignore receive channel) */
		sel.use = 1;
		sel.gep = gep;			/* identifies GPIO */
		storeIEC(channel, sel);		/* link GPIO element pointer to send channel */
		if (iC_opt_B) {
		    assert(ol > 14);
		    len = snprintf(op, ol, " %s", np);	/* add I/O name to execute iCbox -d */
		    b++;				/* count tokens in buffer */
		    op += len;
		    ol -= len;
		    if ((mask = gep->Gbmask) != 0xff) {
			len = snprintf(op, ol, ",%d", mask);	/* mask out any GPIO bits not used */
			op += len;
			ol -= len;
		    }
		}
		cp = strchr(cp, ',');
	    }
	}
	assert(cp == NULL);			/* Ack string matches Registration */
	/********************************************************************
	 *  ACK string complete
	 *******************************************************************/
	if (iC_opt_B && b > 4) {
	    iC_fork_and_exec(iC_string2argv(buffer, b));	/* fork iCbox -d */
	}
	if (iC_debug & 0200) fprintf(iC_outFP, "reply: top channel = %hu\n", topChannel);
    } else {
	iC_quit(QUIT_SERVER);			/* quit normally with 0 length message */
    }
    /********************************************************************
     *  Report results of argument analysis
     *  Port A outputs are inverted by hardware
     *  Port B inputs and outputs are direct
     *******************************************************************/
    if (iC_debug & 0300) {
	fprintf(iC_outFP, "\n");
	if (mcpCnt) {
	    fprintf(iC_outFP, "Allocation for %d MCP23017%s, global instance = \"%s\"\n",
		mcpCnt, mcpCnt == 1 ? "" : "s", iC_iidNM);
	    fprintf(iC_outFP, "	 IEC inst	    bits	      channel\n");
	    for (ch = 0; ch < 8; ch++) {	/* scan /dev/ic2-11 to /dev/i2c-18 */
		if (i2cFdA[ch] == -1) {
		    continue;			/* no MCP23017s on this I2C channel */
		}
		for (ns = 0; ns < 8; ns++) {
		    cn = ch << 3 | ns;
		    mcp = mcpL[cn];
		    if (mcp == (void *) -1 || mcp == NULL) {
			continue;		/* no MCP23017 at this address or not matched by an IEC */
		    }
		    fprintf(iC_outFP, "M%2d:%2x\n", ch+11, ns+0x20);
		    for (gs = 0; gs < 2; gs++) {
			for (qi = 0; qi < 2; qi++) {
			    mcpDetails *	mdp;
			    mdp = &mcp->s[gs][qi];
			    if (mdp->name) {
				fprintf(iC_outFP, "     %c  %s	", AB[gs], mdp->name);
				for (n = 0, m = 0x01; n < 8; n++, m <<= 1) {
				    if (mdp->bmask & m) {
					fprintf(iC_outFP, " %c%d", mdp->inv & m ? '~' : ' ', n);
				    } else {
					fprintf(iC_outFP, "   ");
				    }
				}
				fprintf(iC_outFP, "	%d\n", mdp->channel);
			    }
			}
		    }
		}
	    }
	    fprintf(iC_outFP, "\n");
	}
	if (gpioCnt) {
	    char *	iidPtr;
	    char *	iidSep;
	    fprintf(iC_outFP, "Allocation for %d GPIO element%s, global instance = \"%s\"\n",
		gpioCnt, gpioCnt == 1 ? "" : "s", iC_iidNM);
	    fprintf(iC_outFP, "	IEC bit	inst	gpio	channel\n\n");
	    for (qi = 0; qi < 2; qi++) {
		for (gep = iC_gpL[qi]; gep; gep = gep->nextIO) {
		    strcpy(buffer, gep->Gname);		/* retrieve name[-instance] */
		    if ((iidPtr = strchr(buffer, '-')) != NULL) {
			iidSep = "-";
			*iidPtr++ = '\0';		/* separate name and instance */
		    } else {
			iidSep = iidPtr = "";		/* null or global instance */
		    }
		    for (bit = 0; bit <= 7; bit++) {
			if ((gpio = gep->gpioNr[bit]) != 0xffff) {	/* saved gpio number for this bit */
			    fprintf(iC_outFP, "	%c%s.%hu	%s%s	%3hu	%3hu\n",
				gep->Ginv & iC_bitMask[bit] ? '~' : ' ',
				buffer, bit, iidSep, iidPtr, gpio, gep->Gchannel);
			}
		    }
		}
		fprintf(iC_outFP, "\n");
	    }
	}
    }
    cp = regBuf;
    regBufLen = REPLY;
    if (iC_debug & 0100) {
	op = buffer;
	ol = BS;
    }
    /********************************************************************
     *  Generate MCP23017 initialisation inputs and outputs for active MCP23017s
     *  Send possible TCP/IP after GPIO done
     *******************************************************************/
    if (!iC_opt_G) {
	assert(gpio27FN > 0);
	if (iC_debug & 0200) fprintf(iC_outFP, "### Initialise %d unit(s)\n", mcpCnt);
	gpio_read(gpio27FN);			/* dummy read to clear interrupt on /dev/class/gpio27/value */
#if YYDEBUG && !defined(_WINDOWS)
	if (iC_micro) iC_microPrint("I2C initialise", 0);
#endif	/* YYDEBUG && !defined(_WINDOWS) */
	for (ch = 0; ch < 8; ch++) {		/* scan /dev/ic2-11 to /dev/i2c-18 */
	    if (i2cFdA[ch] == -1) {
		continue;			/* no MCP23017s on this I2C channel */
	    }
	    for (ns = 0; ns < 8; ns++) {
		cn = ch << 3 | ns;
		mcp = mcpL[cn];
		if (mcp == (void *) -1 || mcp == NULL) {
		    continue;			/* no MCP23017 at this address or not matched by an IEC */
		}
		assert(regBufLen > ENTRYSZ);	/* accomodates ",SIX123456,RQX123456,Z" */
		for (gs = 0; gs < 2; gs++) {
		    for (qi = 0; qi < 2; qi++) {
			mcpDetails *	mdp;
			mdp = &mcp->s[gs][qi];
			if (mdp->name) {
			    if (qi == 0) {
				writeByte(mcp->i2cFN, mcp->mcpAdr, OLAT[gs], mdp->inv);
				mdp->val = mdp->inv;	/* TODO check if necssary */
			    } else {
				assert(regBufLen > 11);		/* fits largest data telegram */
				val = readByte(mcp->i2cFN, mcp->mcpAdr, GPIO[gs]) & mdp->bmask;	/* read registered MCP23017 A/B */
				if (val != mdp->val) {
				    len = snprintf(cp, regBufLen, ",%hu:%d",	/* data telegram */
						    mdp->channel,
						    val
						  );
				    cp += len;
				    regBufLen -= len;
				    if (iC_debug & 0100) {
					len = snprintf(op, ol, " M %s(INI)", mdp->name);	/* source name */
					op += len;
					ol -= len;
				    }
				    mdp->val = val;		/* initialise input for comparison */
				}
			    }
			}
		    }
		    readByte(concFd, CONCDEV, GPIO[gs]);	/* clears interrupt if it occured in this port */
		}
	    }
	}
    }
    /********************************************************************
     *  Extend with GPIO IXn initialisation inputs
     *  There may be no GPIOs or only GPIO outputs - nothing to initialise
     *******************************************************************/
    for (gep = iC_gpL[1]; gep; gep = gep->nextIO) {	/* IXn inputs only */
	val = 0;
	for (bit = 0; bit <= 7; bit++) {
	    if ((gpio = gep->gpioNr[bit]) != 0xffff) {
		if ((n = gpio_read(gep->gpioFN[bit])) != -1) {
		    if (n) val |= iC_bitMask[bit];
		} else {
		    fprintf(iC_errFP, "WARNING: %s: GPIO %hu returns invalid value -1 (not 0 or 1 !!)\n",
			iC_progname, gpio);		/* should not happen */
		}
	    }
	}
	assert(gep->Gname);
	assert(regBufLen > 11);			/* fits largest data telegram */
	/* by default do not invert GPIO inputs - they are inverted with -I */
	len = snprintf(cp, regBufLen, ",%hu:%d", gep->Gchannel, val ^ gep->Ginv);	/* data telegram */
	cp += len;
	regBufLen -= len;
	if (iC_debug & 0100) {
	    len = snprintf(op, ol, " G %s(INI)", gep->Gname);	/* source name */
	    op += len;
	    ol -= len;
	}
	gep->Gval = val;			/* initialise input for comparison */
    }
    /********************************************************************
     *  Send IXn inputs if any - to iCsever to initialise receivers
     *******************************************************************/
    if (cp > regBuf) {
	iC_send_msg_to_server(iC_sockFN, regBuf+1);	/* send data telegram(s) to iCserver (skip initial ',') */
	if (iC_debug & 0100) fprintf(iC_outFP, "P: %s:	%s	<%s\n", iC_iccNM, regBuf+1, buffer);
    }
    /********************************************************************
     *  Write GPIO QXn initialisation outputs
     *******************************************************************/
    for (gep = iC_gpL[0]; gep; gep = gep->nextIO) {	/* QXn outputs only */
	val = 0;				/* initialise with logical 0 */
	gep->Gval = gep->Ginv ^ 0xff;		/* force all output bits */
	writeGPIO(gep, gep->Gchannel, val);
    }
    if (argc != 0) {
	/********************************************************************
	 *  -R and argv points to auxialliary app + arguments
	 *  This app has now connected to iCserver and registered all its I/Os
	 *******************************************************************/
	assert(argv && *argv);
	iC_fork_and_exec(argv);			/* run auxiliary app */
    }
    /********************************************************************
     *  Clear and then set all bits to wait for interrupts
     *  do after iC_connect_to_server() 
     *******************************************************************/
    FD_ZERO(&infds);				/* should be done centrally if more than 1 connect */
    FD_ZERO(&ixfds);				/* should be done centrally if more than 1 connect */
    FD_SET(iC_sockFN, &infds);			/* watch TCP/IP for inputs */
    if (iC_debug & 0200) fprintf(iC_outFP, "FD_SET TCP/IP socket  	FN %d\n", iC_sockFN);
    if (mcpCnt) {
	FD_SET(gpio27FN, &ixfds);		/* watch GPIO 27 for out-of-band input */
	if (iC_debug & 0200) fprintf(iC_outFP, "FD_SET GPIO 27 MCP INT	FN %d === mcpCnt = %d\n", gpio27FN, mcpCnt);
    }
    /********************************************************************
     *  Set all GPIO IXn input bits for interrupts
     *******************************************************************/
    for (gep = iC_gpL[1]; gep; gep = gep->nextIO) {	/* IXn inputs only */
	for (bit = 0; bit <= 7; bit++) {
	    if ((gpio = gep->gpioNr[bit]) != 0xffff) {
		assert(gep->gpioFN[bit] > 0);		/* make sure it has been opened */
		FD_SET(gep->gpioFN[bit], &ixfds);	/* watch GPIO N for out-of-band input */
		if (iC_debug & 0200) fprintf(iC_outFP, "FD_SET GPIO %-2d %s.%d	FN %d\n", gpio, gep->Gname, bit, gep->gpioFN[bit]);
	    }
	}
    }
    if ((iC_debug & DZ) == 0) {
	FD_SET(0, &infds);			/* watch stdin for inputs unless - FD_CLR on EOF */
	if (iC_debug & 0200) fprintf(iC_outFP, "FD_SET STDIN INT    	FN 0\n");
    }
    /********************************************************************
     *  External input (TCP/IP via socket, I2C from MCP23017, GPIO and STDIN)
     *  Wait for input in a select statement most of the time
     *******************************************************************/
    for (;;) {
	if ((retval = iC_wait_for_next_event(&infds, &ixfds, NULL)) > 0) {
	    if (FD_ISSET(iC_sockFN, &iC_rdfds)) {
		/********************************************************************
		 *  TCP/IP input from iCserver
		 *******************************************************************/
		if (iC_rcvd_msg_from_server(iC_sockFN, rpyBuf, REPLY) != 0) {
#if YYDEBUG && !defined(_WINDOWS)
		    if (iC_micro) iC_microPrint("TCP input received", 0);
#endif	/* YYDEBUG && !defined(_WINDOWS) */
		    assert(Channels);
		    cp = rpyBuf - 1;		/* increment to first character in rpyBuf in first use of cp */
		    if (isdigit(rpyBuf[0])) {
			char *	cpe;
			char *	cps;
			iecS	sel;		/* union can store either mcpDetails* mdp or gpioIO* gep */

			do {
			    if ((cpe = strchr(++cp, ',')) != NULL) { /* find next comma in input */
				*cpe = '\0';	/* split off leading comma separated token */
			    }
			    if (
				(cps = strchr(cp, ':')) != NULL &&	/* strip only first ':' separating channel and data */
				(channel = (unsigned short)atoi(cp)) > 0 &&
				channel <= topChannel
			    ) {
				val = atoi(++cps);
				sel = Channels[channel];
				if (sel.use == 0) {
				    /********************************************************************
				     *  TCP/IP output for a MCP23017
				     *******************************************************************/
				    mcpDetails *	mdp;
				    mdp = sel.mdp;			/* MCP details */
				    if (mdp->name) {			/* is output registered? */
					/********************************************************************
					 *  Direct output to an MCP23017 Port
					 *******************************************************************/
					writeByte(mdp->i2cFN, mdp->mcpAdr, OLAT[mdp->g], (val & mdp->bmask) ^ mdp->inv);
					if (iC_debug & 0100) fprintf(iC_outFP, "M: %s:	%hu:%d	> MCP %d:%x%c - %s\n",
						iC_iccNM, channel, val, mdp->name[2]-'0'+11, mdp->mcpAdr, AB[mdp->g], mdp->name);
				    } else {
					fprintf(iC_errFP, "WARNING: %s: %hu:%d no output registered for MCP???\n",
					    iC_iccNM, channel, val);	/* should not happen */
				    }
				} else if (sel.use == 1) {
				    /********************************************************************
				     *  TCP/IP output for a GPIO
				     *******************************************************************/
				    writeGPIO(sel.gep, channel, val);
				} else {
				    assert(0);				/* error in storeIEC() */
				}
			    } else {
				fprintf(iC_errFP, "channel = %hu, topChannel = %hu\n", channel, topChannel);
				goto RcvWarning;
			    }
			} while ((cp = cpe) != NULL);		/* next token if any */
		    }
		    else {
		      RcvWarning:
			fprintf(iC_errFP, "WARNING: %s: received '%s' from iCserver ???\n", iC_iccNM, rpyBuf);
		    }
		} else {
		    iC_quit(QUIT_SERVER);	/* quit normally with 0 length message from iCserver */
		}
	    }	/* end of TCP/IP input */
	    if (gpio27FN > 0 && FD_ISSET(gpio27FN, &iC_exfds)) {	/* watch for out-of-band GPIO 27 input */
		/********************************************************************
		 *  GPIO 27 interrupt means I2C input from a MCP23017
		 *******************************************************************/
		m1 = m = 0;
#if YYDEBUG && !defined(_WINDOWS)
		if (iC_micro) iC_microPrint("I2C input received", 0);
#endif	/* YYDEBUG && !defined(_WINDOWS) */
		cp = regBuf;
		regBufLen = REPLY;
		if (iC_debug & 0300) {
		    op = buffer;
		    ol = BS;
		}
		do {
		    /********************************************************************
		     *  Scan MCP23017s for input interrupts (even those not used)
		     *  More interrupts can arrive for MCP23017's already scanned, especially
		     *  with bouncing mechanical contacts - repeat the scan until GPIO 27 == 1
		     *******************************************************************/
		    unsigned int	n, g, cn, ma, diff;
		    mcpDetails *	mdp;
		    for (g = 0; g < 2; g++) {
			diff = ~readByte(concFd, CONCDEV, GPIO[g]) & 0xff;	/* concentrator MCP A/B points to I2C channel */
			while (diff) {
			    bit = iC_bitIndex[diff];	/* returns rightmost bit 0 - 7 for values 1 - 255 (avoid 0) */
			    mask  = iC_bitMask[bit];	/* returns hex 01 02 04 08 10 20 40 80 */
			    cn = bit << 3;		/* start if I2C channel selected by bit from concentrator MCP */
			    for (n = 0; n < 8; n++) {
				mcp = mcpL[cn+n];	/* test every MCP on this I2C channel */
				if (mcp != (void*)-1 && mcp != NULL) {
				    fd  = mcp->i2cFN;	/* configured MCP found in I2C channel */
				    ma  = mcp->mcpAdr;
				    if (readByte(fd, ma, INTF[g])) {	/* test interrupt flag of this MCP */
					mdp = &mcp->s[g][1];		/* interrupt - get mcpDetails for Port A/B and input */
					assert(regBufLen > 11);			/* fits largest data telegram */
					val = readByte(fd, ma, GPIO[g]);	/* read data MCP23017 A/B at interrupt */
					readByte(concFd, CONCDEV, GPIO[g]);	/* concentrator again to clear interrupt */
					if (val != mdp->val) {
					    if (mdp->name) {
						len = snprintf(cp, regBufLen, ",%hu:%d",	/* data telegram */
								mdp->channel,
								val & mdp->bmask
							      );
						cp += len;
						regBufLen -= len;
						if (iC_debug & 0300) {
						    len = snprintf(op, ol, " MCP %d:%x%c - %s", bit+11, ma, AB[g], mdp->name); /* source name */
						    op += len;
						    ol -= len;
						}
					    } else if (iC_debug & 0100) {
						fprintf(iC_outFP, "M: %s: %d input on unregistered MCP23017 %d:%x%c\n", iC_iccNM, val, bit+11, ma, AB[g]);
					    }
					    mdp->val = val;		/* store change for comparison */
					}
					m++;				/* count INTF interrupts found */
				    }
				}
			    }
			    diff &= ~mask;		/* clear the bit just processed */
			}
		    }
		    m1++;
		    if ((val = gpio_read(gpio27FN)) == -1) {
			perror("GPIO 27 read");
			fprintf(iC_errFP, "ERROR: %s: GPIO 27 read failed\n", iC_progname);
			break;
		    }
		} while (val != 1);		/* catch interrupts which came in during for loop */
		if (cp > regBuf) {
		    iC_send_msg_to_server(iC_sockFN, regBuf+1);			/* send data telegram(s) to iCserver */
		    if (iC_debug & 0100) fprintf(iC_outFP, "P: %s:	%s	<%s\n", iC_iccNM, regBuf+1, buffer);
		}
		if ((iC_debug & (DQ | 0200)) == 0200) {
		    if (m1 > 5){
			fprintf(iC_errFP, "WARNING: %s: GPIO 27 interrupt %d loops %d changes \"%s\"\n", iC_progname, m1, m, regBuf+1);
		    } else if (m == 0) {	/* for some reason this happens occasionaly - no inputs are missed though */
			fprintf(iC_errFP, "WARNING: %s: GPIO 27 interrupt and no INTF set on MCP23017s\n", iC_progname);
		    }
		    *(regBuf+1) = '\0';			/* clean debug output next time */
		}
	    }	/*  end of GPIO 27 interrupt */
	    /********************************************************************
	     *  GPIO N interrupt means GPIO n input
	     *******************************************************************/
	    m1 = 0;
	    cp = regBuf;
	    regBufLen = REPLY;
	    if (iC_debug & 0100) {
		op = buffer;
		ol = BS;
	    }
	    for (gep = iC_gpL[1]; gep; gep = gep->nextIO) {	/* IXn inputs only */
		assert(regBufLen > 11);		/* fits largest data telegram */
		m = 0;
		val = gep->Gval;
		for (bit = 0; bit <= 7; bit++) {
		    if ((gpio = gep->gpioNr[bit]) != 0xffff) {
			assert(gep->gpioFN[bit] > 0);
			if (FD_ISSET(gep->gpioFN[bit], &iC_exfds)) {	/* any out-of-band GPIO N input */
			    if ((n = gpio_read(gep->gpioFN[bit])) == 0) {
				val &= ~(1 << bit);
			    } else if (n == 1) {
				val |= 1 << bit;
			    } else {
				fprintf(iC_errFP, "WARNING: %s: GPIO %hu returns invalid value %d (not 0 or 1 !!)\n",
				    iC_progname, gpio, n);		/* should not happen */
			    }
			    m++;
#if YYDEBUG && !defined(_WINDOWS)
			    if (m1++ == 0 && iC_micro) iC_microPrint("GPIO input received", 0);
#endif	/* YYDEBUG && !defined(_WINDOWS) */
			}
		    }
		}
		if (m && val != gep->Gval) {
		    /* by default do not invert GPIO inputs - they are inverted with -I */
		    len = snprintf(cp, regBufLen, ",%hu:%d", gep->Gchannel, val ^ gep->Ginv); /* data telegram */
		    cp += len;
		    regBufLen -= len;
		    if (iC_debug & 0100) {
			len = snprintf(op, ol, " G %s", gep->Gname); /* source name */
			op += len;
			ol -= len;
		    }
		    gep->Gval = val;		/* store change for comparison */
		}
	    }	/*  end of GPIO N interrupt */
	    /********************************************************************
	     *  Send data telegrams collected from MCP input and GPIO N input
	     *******************************************************************/
	    if (cp > regBuf) {
		iC_send_msg_to_server(iC_sockFN, regBuf+1);	/* send data telegram(s) to iCserver */
		if (iC_debug & 0100) fprintf(iC_outFP, "P: %s:	%s	<%s\n", iC_iccNM, regBuf+1, buffer);
	    }
	    if (FD_ISSET(0, &iC_rdfds)) {
		/********************************************************************
		 *  STDIN interrupt
		 *******************************************************************/
		if (fgets(iC_stdinBuf, REPLY, stdin) == NULL) {	/* get input before TX0.1 notification */
		    FD_CLR(0, &infds);			/* ignore EOF - happens in bg or file - turn off interrupts */
		    iC_stdinBuf[0] = '\0';		/* notify EOF to iC application by zero length iC_stdinBuf */
		}
		if ((c = iC_stdinBuf[0]) == 'q' || c == '\0') {
		    iC_quit(QUIT_TERMINAL);		/* quit normally with 'q' or ctrl+D */
#if YYDEBUG && !defined(_WINDOWS)
		} else if (c == 't') {
		    iC_debug ^= 0100;			/* toggle -t flag */
		} else if (c == 'm') {
		    iC_micro ^= 1;			/* toggle micro */
#endif	/* YYDEBUG && !defined(_WINDOWS) */
		} else if (c == 'T') {
		    iC_send_msg_to_server(iC_sockFN, "T");	/* print iCserver tables */
		} else if (c == 'C') {
		    fprintf(iC_outFP, "INTFA = 0x%02x GPIOA = 0x%02x INTFB = 0x%02x GPIOB = 0x%02x IX100 = 0x%02x\n",
			readByte(concFd, CONCDEV, INTFA),
			readByte(concFd, CONCDEV, GPIOA),	/* clears interrupt if it occured in this port */
			readByte(concFd, CONCDEV, INTFB),
			readByte(concFd, CONCDEV, GPIOB),	/* clears interrupt if it occured in this port */
			readByte(i2cFdA[0], 0x20, GPIOA));	/* clears interrupt on data MCP10 */
		} else if (c != '\n') {
		    fprintf(iC_errFP, "no action coded for '%c' - try t, m, T, i, I, or q followed by ENTER\n", c);
		}
	    }	/*  end of STDIN interrupt */
	} else {
	    perror("ERROR: select failed");				/* retval -1 */
	    iC_quit(SIGUSR1);		/* error quit */
	}
    }
		    iC_quit(QUIT_TERMINAL);		/* TODO remove */
} /* main */

/********************************************************************
 *
 *	Scan and process either one MCP23017 IEC argument or a list
 *	or a range of them. MCP23017 IEC lists or ranges are either
 *	all input or output IECs.
 *	  single:  IX100		or   QX101
 *	  list:    IX200,IX210,IX250	or   QX201,QX211,QX251
 *	  range:   IX300-IX370		or   QX301-QX371
 *	Each of the three argument groups can be preceded by '~' for
 *	inverting each IEC in the group or followed by ,<mask> or
 *	-<instance> or ,<mask>-<instance>
 *
 *	Initial format "%1[QI]X%5[0-9]%127s"
 *	List format   ",%1[~IQ0-9]X%5[0-9]%127s"
 *	Range format  "-%1[~IQ0-9]X%5[0-9]%127s"
 *
 *******************************************************************/

static int
scanIEC(const char * format, int * ieStartP, int * ieEndP, char * tail, unsigned short * channelP)
{
    int			n;
    int			r;
    iecS		sel;
    int			ch;
    int			ns;
    int			gs;
    size_t		sl;
    char		iqe[2] = { '\0', '\0' };
    char		cng[6] = { '\0', '\0', '\0', '\0', '\0', '\0' };

    static char		iqs[2] = { '\0', '\0' };
    static int		chs;
    static int		nss;
    static int		gss;

    if ((n = sscanf(tail, format, iqe, cng, tail)) >= 1) {
	sel.use = 2;
	/* the first test cannot occur in the initial scan because format lacks [~] */
	if (*iqe == '~') {
	    fprintf(iC_errFP, "ERROR: %s: '%s' no inversion operator '~' allowed in an MCP list or range\n", iC_progname, *argp);
	    errorFlag++;
	    return 6;				/* goto endOneArg */
	}
	if (*format == '%') {
	    *channelP = 0;			/* start temporary storage wih first IEC */
	} else {
	    if (n == 1 && isdigit(*iqe)) {
		switch (tail[0]) {		/* test for mask or instance */
		    case ',': return 1;	/* goto checkMask */
		    case '-': return 4;	/* goto checkInst */
		    default:  return 5;	/* goto illFormed */
		}
	    }
	    ++*channelP;			/* next location in temporary store for further IEC */
	}
	if (n <= 1) {
	    return 5;				/* goto illFormed */
	}
	sel.mdp = NULL;				/* clear all bits in the bit field */
	sel.cngqi.c = ch = cng[0] - '1';
	sel.cngqi.n = ns = cng[1] - '0';
	sel.cngqi.g = gs = cng[2] - '0' - ofs;
	assert(*iqe == 'I' || *iqe == 'Q');
	sel.cngqi.qi = *iqe == 'I' ? 1 : 0;	/* that completes IEC selection */ 
	*ieEndP = atoi(cng);			/* assume a single IXn or QXn */
	sl = strlen(&cng[0]);
	if (sl < 3) {
	    // IEC is GPIO variable
	    if (*format == '%') {
		sel.use = 3;
		storeIEC(*channelP, sel);	/* save IEC details in temporary storage - qi used by GPIO */
		return 3;			/* goto processGPIO */
	    } else {
		fprintf(iC_errFP, "ERROR: %s: '%s' GPIO IEC variable in an MCP23017 list or range\n", iC_progname, *argp);
		errorFlag++;
		return 6;			/* goto endOneArg */
	    }
	}
	if (*format != '%' && *iqs != *iqe) {
	    fprintf(iC_errFP, "ERROR: %s: '%s' mixed input and output in an MCP23017 list or range\n", iC_progname, *argp);
	    errorFlag++;
	    return 6;				/* goto endOneArg */
	}
	if (sl > 3 ||
	    ch < 0 || ch > 7 ||
	    gs < 0 || ns > 7 ||
	    gs < 0 || gs > 1 ||
	    (ch == 7 && ns == 7)) {
	    fprintf(iC_errFP, "ERROR: %s: '%s' with offset %d does not address a data MCP23017 register\n", iC_progname, *argp, ofs);
	    errorFlag++;
	    return 6;				/* goto endOneArg */
	}
	storeIEC(*channelP, sel);		/* save IEC details in temporary storage later used for channels */
	if (*format == '-') {
	    assert(*channelP == 1);
	    *channelP = USHRT_MAX;		/* 65535 identifies range */
	    if (ch != chs || gs != gss) {
		fprintf(iC_errFP, "ERROR: %s: '%s' only allowed MCP23017 range is in dev address (middle digit)\n", iC_progname, *argp);
		errorFlag++;
		return 6;			/* goto endOneArg */
	    }
	    if (nss >= ns) {
		fprintf(iC_errFP, "ERROR: %s: '%s' MCP23017 range in dev address (middle digit) is not positive\n", iC_progname, *argp);
		errorFlag++;
		return 6;			/* goto endOneArg */
	    }
	}
	if (iC_debug & 0200) fprintf(iC_outFP, "	MCP23017 %s%cX%d\n", invFlag ? "~" : "", *iqe, *ieEndP);
	if (n == 2) {
	    tail[0] = '\0';			/* single IEC argument without ,<mask> or -<inst> */
	    return 2;				/* goto checkOptionG */
	}
	if (*format == '%') {
	    /********************************************************************
	     *  Scan more IEC arguments in a list or range
	     *******************************************************************/
	    *iqs = *iqe;			/* enter here only in initial scan */
	    *ieStartP = *ieEndP;
	    chs = ch;
	    nss = ns;
	    gss = gs;
	    while (tail[0] == ',') {
		/********************************************************************
		 *  Scan next IEC in an MCP23017 IEC argument list
		 *******************************************************************/
		if ((r = scanIEC(",%1[~IQ0-9]X%5[0-9]%127s", ieStartP, ieEndP, tail, channelP)) != 0) {
		    return r;			/* a goto condition has been found */
		}
	    }
	    if (tail[0] == '-') {
		/********************************************************************
		 *  Scan last IEC in an MCP23017 IEC argument range
		 *******************************************************************/
		if ((r = scanIEC("-%1[~IQ0-9]X%5[0-9]%127s", ieStartP, ieEndP, tail, channelP)) != 0) {
		    return r;			/* a goto condition has been found */
		}
		if (!isdigit(tail[1])) {
		    return 5;			/* goto illFormed */
		}
		switch (tail[0]) {		/* test for mask or instance */
		    case ',': return 1;		/* goto checkMask */
		    case '-': return 4;		/* goto checkInst */
		    default:  return 5;		/* goto illFormed */
		}
	    }
	}
    }
    return 0;
} /* scanIEC */

/********************************************************************
 *
 *	Initalise and expand dynamic array Channels[] as necessary
 *	Store MCP23017 address cngqi in Channels[] indexed by channel
 *	Alternatively store gpioIO * to current GPIO element
 *
 *******************************************************************/

static void
storeIEC(unsigned short channel, iecS sel)
{
    while (channel >= ioChannels) {
	Channels = (iecS *)realloc(Channels,		/* initially NULL */
	    (ioChannels + IOUNITS) * sizeof(iecS));
	assert(Channels);
	memset(&Channels[ioChannels], '\0', IOUNITS * sizeof(iecS));
	ioChannels += IOUNITS;			/* increase the size of the array */
	if (iC_debug & 0200) fprintf(iC_outFP, "storeIEC: Channels[%d] increase\n", ioChannels);
    }
    if (iC_debug & 0200) {
	switch(sel.use) {
	    case 0: fprintf(iC_outFP, "storeIEC: Channels[%d] <== %p\n", channel, sel.mdp);
		break;
	    case 1: fprintf(iC_outFP, "storeIEC: Channels[%d] <== %p\n", channel, sel.gep);
		break;
	    case 2: fprintf(iC_outFP, "storeIEC: Channels[%d] <== %cX%d%d%d\n",
			channel, QI[sel.cngqi.qi], sel.cngqi.c+1, sel.cngqi.n, sel.cngqi.g+ofs);
		break;
	    case 3: fprintf(iC_outFP, "storeIEC: Channels[%d] <== GPIO %cX\n",
			channel, QI[sel.cngqi.qi]);
		break;
	}
    }
    Channels[channel] = sel;			/* store MCP23017 address cngqi or gpioIO* gep */
} /* storeIEC */

/********************************************************************
 *
 *	Write changed GPIO output bits
 *
 *******************************************************************/

static void
writeGPIO(gpioIO * gep, unsigned short channel, int val)
{
    int			diff;
    int			fd;
    int			mask;
    unsigned short	bit;

    assert(gep && gep->Gname && *gep->Gname == 'Q');	/* make sure this is really a GPIO output */
    if (iC_debug & 0100) fprintf(iC_outFP, "P: %s:	%hu:%d G	> %s\n",
	iC_iccNM, channel, (int)val, gep->Gname);
    val ^= gep->Ginv;			/* normally write non-inverted data to GPIO output */
    diff = val ^ gep->Gval;		/* bits which are going to change */
    assert ((diff & ~0xff) == 0);	/* may receive a message which involves no change */
    while (diff) {
	bit = iC_bitIndex[diff];	/* returns 0 - 7 for values 1 - 255 (avoid 0) */
	mask  = iC_bitMask[bit];	/* returns hex 01 02 04 08 10 20 40 80 */
	if ((fd = gep->gpioFN[bit]) != -1) {	/* is GPIO pin open ? */
	    gpio_write(fd, val & mask);	/* yes - write to GPIO (does not need to be a bit in pos 0) */
	} else if ((iC_debug & 0300) == 0300) {
	    fprintf(iC_errFP, "WARNING: %s: no GPIO associated with %s.%hu\n",
		iC_progname, gep->Gname, bit);
	}
	diff &= ~mask;			/* clear the bit just processed */
    }
    gep->Gval = val;			/* ready for next output */
} /* writeGPIO */

/********************************************************************
 *
 *	Convert a string to integer using the first characters to determine the base
 *	    0		return 0
 *	    0b[01]*	convert binary string
 *	    0x[0-9a-f]*	convert hexadeximal string
 *	    0X[0-9A-F]*	convert hexadeximal string
 *	    0[0-7]*	convert octal string
 *	    [1-9][0-9]*	convert decimal string
 *	    error	return 0x7fffffff	// LONG_MAX
 *
 *******************************************************************/

static long
convert_nr(const char * str, char ** endptr)
{
    long	val;

    errno = 0;				/* to distinguish success/failure after call */
    if (*str == '0' && (*(str + 1) == 'b' || *(str + 1) == 'B')) {
	val = strtol(str+2, endptr, 2);	/* convert base 2 binary */
    } else {
	val = strtol(str, endptr, 0);	/* convert hex, octal or decimal */
    }
    if ((errno == ERANGE && (val == LONG_MAX || val == LONG_MIN))
	|| (errno != 0 && val == 0)) {	/* Check for various possible errors */
	return LONG_MAX;
    }
    return val;
} /* convert_nr */

/********************************************************************
 *
 *	Unexport GPIOs and close MCP23017s
 *	Clear gpiosp->u.used bits for GPIOs and A/D channels used in this app
 *
 *******************************************************************/

static int
termQuit(int sig)
{
    if (iC_opt_P) {
	if (iC_debug & 0200) fprintf(iC_outFP, "=== Unexport GPIOs and close MCP23017s =======\n");
	mcpIO *		mcp;
	int		ch;
	int		ns;
	int		cn;
	int		qi;
	gpioIO *	gep;
	unsigned short	bit;
	unsigned short	gpio;
	int		fd;
	ProcValidUsed *	gpiosp;

	/********************************************************************
	 *  Unexport and close all gpio files for all GPIO arguments
	 *******************************************************************/
	for (qi = 0; qi < 2; qi++) {
	    for (gep = iC_gpL[qi]; gep; gep = gep->nextIO) {
		for (bit = 0; bit <= 7; bit++) {
		    if ((gpio = gep->gpioNr[bit]) != 0xffff) {
			/********************************************************************
			 *  Execute the SUID root progran iCgpioPUD(gpio, pud) to turn off pull-up/down
			 *******************************************************************/
			if (qi == 1) iC_gpio_pud(gpio, BCM2835_GPIO_PUD_OFF);	/* inputs only */
			/********************************************************************
			 *  Close GPIO N value
			 *******************************************************************/
			if ((fd = gep->gpioFN[bit])> 0) {
			    close(fd);			/* close connection to /sys/class/gpio/gpio_N/value */
			}
			/********************************************************************
			 *  Force all outputs and inputs to direction "in" and "none" interrupts
			 *******************************************************************/
			if (sig != SIGUSR2 && gpio_export(gpio, "in", "none", 1, iC_progname) != 0) {
			    sig = SIGUSR1;		/* unable to export gpio_N */
			}
			/********************************************************************
			 *  free up the sysfs for gpio N unless used by another program (SIGUSR2)
			 *******************************************************************/
			if (iC_debug & 0200) fprintf(iC_outFP, "### Unexport GPIO %hu\n", gpio);
			if (sig != SIGUSR2 && gpio_unexport(gpio) != 0) {
			    sig = SIGUSR1;		/* unable to unexport gpio_N */
			}
		    }
		}
	    }
	}
	/********************************************************************
	 *  MCP23017s
	 *******************************************************************/
	if (mcpCnt) {
	    if ((iC_debug & 0200) != 0) fprintf(iC_outFP, "### Shutdown active MCP23017s\n");
	    /********************************************************************
	     *  Shutdown all active MCP23017s leaving interrupts off and open drain
	     *  Clear active MCP23017 outputs
	     *******************************************************************/
	    for (ch = 0; ch < 8; ch++) {	/* scan /dev/ic2-11 to /dev/i2c-18 */
		if (i2cFdA[ch] == -1) {
		    continue;			/* no MCP23017s on this I2C channel */
		}
		for (ns = 0; ns < 8; ns++) {
		    cn = ch << 3 | ns;
		    mcp = mcpL[cn];
		    if (mcp == (void *) -1 || mcp == NULL) {
			continue;		/* no MCP23017 at this address */
		    }
		    setupMCP23017(mcp->i2cFN, data, mcp->mcpAdr, IOCON_ODR, 0x00, 0x00);
		}
	    }
	    if (concFd > 0) {
		setupMCP23017(concFd, data, CONCDEV, IOCON_ODR, 0x00, 0x00);	/* concentrator MCP */
	    }
	    /********************************************************************
	     *  Close selected i2cdev devices
	     *******************************************************************/
	    for (ch = 0; ch < 8; ch++) {	/* scan /dev/ic2-11 to /dev/i2c-18 */
		if (i2cFdA[ch] > 0) {
		    close(i2cFdA[ch]);		/* close connection to I2C channels*/
		}
	    }
	    /********************************************************************
	     *  Close GPIO 27 INTA value
	     *  free up the sysfs for gpio 27 unless used by another program (SIGUSR2)
	     *******************************************************************/
	    if (gpio27FN > 0) {
		close(gpio27FN);			/* close connection to /sys/class/gpio/gpio27/value */
		if (iC_debug & 0200) fprintf(iC_outFP, "### Unexport GPIO 27\n");
		if (sig != SIGUSR2 && gpio_unexport(27) != 0) {
		    sig = SIGUSR1;			/* unable to unexport gpio 27 */
		}
	    }
	}
	/********************************************************************
	 *  Open and lock the auxiliary file ~/.iC/gpios.used again
	 *  Other apps may have set used bits since this app was started
	 *******************************************************************/
	if ((gpiosp = openLockGpios(0)) == NULL) {
	    fprintf(iC_errFP, "ERROR: %s: in openLockGpios()\n",
		iC_progname);
	    return (SIGUSR1);		/* error quit */
	}
	gpiosp->u.used &= ~ownUsed;		/* clear all bits for GPIOs and A/D channels used in this app */
	if (writeUnlockCloseGpios() < 0) {	/* unlink (remove) ~/.iC/gpios.used if gpios->u.used and oops is 0 */
	    fprintf(iC_errFP, "ERROR: %s: in writeUnlockCloseGpios()\n",
		iC_progname);
	    return (SIGUSR1);		/* error quit */
	}
	if (iC_debug & 0200) fprintf(iC_outFP, "=== End Unexport GPIOs and close MCP23017s ===\n");
    } /* iC_opt_P */
    return (sig);			/* finally quit */
} /* termQuit */
#endif	/* defined(RASPBERRYPI) && defined(TCP) && !defined(_WINDOWS) */

/* ############ POD to generate iCpiI2C man page ############################

=encoding utf8

=head1 NAME

iCpiI2C - real I2C digital I/O on a Raspberry Pi for the iC environment

=head1 SYNOPSIS

 iCpiI2C  [-GBIftmqzh][ -s <host>][ -p <port>][ -n <name>][ -i <inst>]
          [ -o <offs>][ -d <deb>]
          [ [~]IXcng[,IXcng,...][,<mask>][-<inst>] ...]
          [ [~]QXcng[,QXcng,...][,<mask>][-<inst>] ...]
          [ [~]IXcng-IXcng[,<mask>][-<inst>] ...]
          [ [~]QXcng-QXcng[,<mask>][-<inst>] ...]
          [ [~]IXn,<gpio>[,<gpio>,...][-<inst>] ...]
          [ [~]QXn,<gpio>[,<gpio>,...][-<inst>] ...]
          [ [~]IXn.<bit>,<gpio>[-<inst>] ...]
          [ [~]QXn.<bit>,<gpio>[-<inst>] ...]      # at least 1 IEC argument
          [ -R <aux_app>[ <aux_app_argument> ...]] # must be last arguments
    -s host IP address of server    (default '127.0.0.1')
    -p port service port of server  (default '8778')
    -i inst instance of this client (default '') or 1 to 3 digits
    -o offs offset for MCP GPIOA/GPIOB selection in a second iCpiI2C in the
            same network (default 0; 2, 4, 6, 8 for extra iCpI2C's)
            All IXcng and QXcng declarations must use g = offs or offs+1
    -G      service GPIO I/O only - block MCP23017s and MCP23017 arguments
    -B      start iCbox -d to monitor active MCP23017 and/or GPIO I/O
    -I      invert all MCP23017 and GPIO inputs and outputs
            MCP23017 and GPIO input circuits are normally hi - 1 and
            when a switch on the input is pressed it goes lo - 0. Therefore
            it is appropriate to invert inputs and outputs. When inverted
            a switch pressed on an input generates a 1 for the IEC inputs and
            a 1 on an IEC output turns a LED and relay on, which is natural.
            An exception is a relay with an inverting driver, whose output
            must be non-inverting (~ operator inverts bit variables).
    -W GPIO number used by the w1-gpio kernel module (default 4, maximum 31).
            When the GPIO with this number is used in this app, iCtherm is
            permanently blocked to avoid Oops errors in module w1-gpio.
    -f      force use of all interrupting gpio inputs - in particular
            force use of gpio 27 - some programs do not unexport correctly.

                      MCP23017 arguments
          For each MCP23017 connected to a Raspberry Pi two 8 bit registers
          GPIOA and GPIOB must be configured as IEC inputs (IXcng) or
          outputs (QXcng) or both, where cng is a three digit number.
          For MCP23017 IEC base identifiers the number cng defines the
          complete address of a particular MCP23017 GPIO register:
           c  is the channel 1 to 8 for /dev/i2c-11 to /dev/i2c-18
           n  is the address 0 to 7 of a particular MCP23017 for the
              address range 0x20 to 0x27 selected with A0 to A2
           g  selects the GPIO register
              g = 0 2 4 6 or 8 selects GPIOA
              g = 1 3 5 7 or 9 selects GPIOB
              normally only g = 0 and 1 is used for GPIOA and GPIOB,
              but if a second instance of iCpiI2C is used on another
              Raspberry Pi, but connected to the one iCsever the
              alternate numbers differentiate between IEC variables
              for the two RPi's.  Second or more instances of iCpiI2C
              must be called with the option -o 2, -o 4 etc for
              using 2 and 3 or 4 and 5 etc to select GPIOA and GPIOB.
          Both registers GPIOA and GPIOB can have input bits and
          output bits, but any input bit cannot also be an output bit
          or vice versa.  Each input or output IEC variable (or list
          of variables) may be followed by a comma separated number,
          interpreted as a mask, declaring which bits in the base IEC
          variable byte are inputs for an input variable or outputs
          for an output variable. The mask is best written as a binary
          number eg 0b10001111 for bits 0, 1, 2, 3 and 7.  If no mask
          is supplied the default mask is 0b11111111 - all 8 bits set.
    IXcng[,IXcng,...][,<mask>]   list of input variables with the same mask
    QXcng[,QXcng,...][,<mask>]   list of output variables with the same mask
    IXcng-IXcng[,<mask>]         range of input variables with the same mask
    QXcng-QXcng[,<mask>]         range of output variables with the same mask
         IEC lists and ranges must contain either all inputs or all outputs.
         The only range supported is in the digit n for the MCP23017 address.

                      GPIO arguments
          Any IEC I/Os IXn.y and QXn.y which are to be linked to GPIO
          inputs or outputs must be named in the argument list. The
          number n in the IEC must be one or at most two digits to
          differentiate GPIO IEC's from three digit MCP23017 IEC's.
    IXn,<gpio>[,<gpio>,...]
    QXn,<gpio>[,<gpio>,...]
          Associate the bits of a particular input or output IEC
          with a list of gpio numbers.  Up to 8 gpio numbers can
          be given in a comma separated list. The first gpio number
          will be associated with bit 0, the second with bit 1 etc
          and the eighth with bit 7. If the list is shorter than
          8 the trailing bits are not used. The letter 'd' can be
          used in the list instead of a gpio number to mark that bit
          as unused.  eg. IX3,17,18,19,20,d,d,21 associates IX3.0 to
          IX3.3 with GPIO 17 to GPIO 20 and IX3.6 with GPIO 21.
          Alternatively:
    IXn.<bit>,<gpio>
    QXn.<bit>,<gpio>
          Each input or output <bit> 0 to 7 is associated with one
          gpio nunber eg. IX3.0,17 QX3.7,18

                      COMMON extensions
    ~IEC  Each leading IEC of a list or range may be preceded by a
          tilde '~', which inverts all IEC inputs or outputs of a
          list or range.  If all IEC inputs and outputs are already
          inverted with -I, then ~ inverts them again, which makes them
          non-inverted. Individual bits for one IEC can be inverted
          or left normal for GPIO IEC's by declaring the IEC more than
          once with non-overlapping gpio bit lists or single bits. Also
          possible for MCP23017 I/O with two non overlapping masks -
          one inverting, the other non-inverting.
    IEC-inst Each individual IEC or range can be followed by -<inst>,
          where <inst> consists of 1 to 3 numeric digits.
          Such an appended instance code takes precedence over the
          general instance specified with the -i <inst> option above.

                      CAVEAT
    There must be at least 1 MCP23017 or GPIO IEC argument. No IEC
    arguments are generated automatically for iCpiI2C.
    Only one instance of iCpiI2C or an app with real IEC parameters
    like iCpiFace may be run on one RPi and all GPIOs and MCP23017s
    must be controlled by this one instance. If two instances were
    running, the common interrupts would clash. Also no other program
    controlling GPIOs and MCP23017s like 'MCP23017 Digital Emulator'
    may be run at the same time as this application.  An exception
    is iCpiPWM which controls GPIOs by DMA and not by interrupts.
    Another exception is iCtherm which controls GPIO 4 by the 1Wire
    interface.  Care is taken that any GPIOs or MCP23017s used in one
    app, iCpiI2C, iCpiPWM or even iCtherm do not clash with another
    app (using ~/.iC/gpios.used).

                      DEBUG options
    -t      trace arguments and activity (equivalent to -d100)
         t  at run time toggles gate activity trace
    -m      display microsecond timing info
         m  at run time toggles microsecond timing trace
    -d deb  +1   trace TCP/IP send actions
            +2   trace TCP/IP rcv  actions
            +10  trace I2C  input  actions
            +20  trace I2C  output actions
            +100 show arguments
            +200 show more debugging details
            +400 exit after initialisation
    -q      quiet - do not report clients connecting and disconnecting
    -z      block keyboard input on this app - used by -R
    -h      this help text
         T  at run time displays registrations and equivalences in iCserver
         q  or ctrl+D  at run time stops iCpiI2C

                      AUXILIARY app
    -R <app ...> run auxiliary app followed by -z and its arguments
                 as a separate process; -R ... must be last arguments.

=head1 DESCRIPTION

B<iCpiI2C> is an I/O client for iCserver in the immediate C environment
of a Raspberry Pi handling real 16 bit inputs and/or 16 bit outputs for
each MCP23017 controller attached to a Raspberry Pi via 8 multiplexed
I2C busses, or from a number of direct GPIO pins on the Raspberry Pi
or both.

A maximum of 127 MCP23017 controllers can be handled altogether.
8 MCP23017 controllers per multiplexed I2C channel /dev/i2c-11 to
/dev/i2c-17 and 7 MCP23017 controllers for channel /dev/i2c-18.
Each MCP23017 can have 16 I/Os in two 8 bit registers A and B.
Each I/O can be either an input or an output or not used at all.

All GPIO pins on a Raspberry Pi A, B, B+, 3B+ or 4 may be selected
as either input bits or output bits independent of whether MCP23017s
are present or not, except GPIO 2, 3 and 27 if MCP23017s are also
processed.  All MCP23017 and GPIO inputs are handled by interrupts.

=head1 TECHNICAL BACKGROUND

The direct GPIO connections on the Raspberry Pi:

 0) The Raspberry Pi A or B brings out 17 GPIO signals on a 26 pin
    connecteor, 5 of which double up to do the I2C interface, 2 for
    a UART, leaving 10 for general purpose input/output (there are
    4 more on a 2nd connector, which is not normally fitted). The
    Raspberry Pi B+ brings out 9 more GPIO signals on its 40 pin
    connector making a total of 19 free GPIO pins.  The Linux "sysfs"
    can access the value of these GPIO pins from user space and more
    importantly can generate interrupts from them.

    All GPIO pin I/O is handled by the Linux "sysfs" and its interrupts.

    For details see:
    http://www.auctoris.co.uk/2012/07/19/gpio-with-sysfs-on-a-raspberry-pi/

    The "sysfs" export, unexport, direction, edge and value commands
    can be run by normal (non root) users if they belong to group gpio.
    To achieve this the true path that the link /sys/class/gpio/gpio*
    points to must be adjusted to root:gpio by a system script.
    The path in the kernel and the matching udev script has changed a
    number of times between 2012 and 2015 causing much confusion.

    The following change works for me as of December 2015
    https://github.com/raspberrypi/linux/issues/1117

    popcornmix commented on Aug 25 2015
    Looks like something may have changed in the later kernel where
    /sys/devices/soc/*.gpio/gpio is now /sys/devices/platform/soc/*.gpio/gpio
    Can you try editing:
    /lib/udev/rules.d/60-python-pifacecommon.rules and
    /lib/udev/rules.d/60-python3-pifacecommon.rules (if they both exist) to be:

 KERNEL=="i2cdev*", GROUP="i2c", MODE="0660"
 SUBSYSTEM=="gpio*", PROGRAM="/bin/sh -c 'chown -R root:gpio /sys/class/gpio &&\
 chmod -R 770 /sys/class/gpio; chown -R root:gpio /sys/devices/virtual/gpio &&\
 chmod -R 770 /sys/devices/virtual/gpio;\
 chown -R root:gpio /sys/devices/platform/soc/*.gpio/gpio &&\
 chmod -R 770 /sys/devices/platform/soc/*.gpio/gpio'"

    Another complication is, that after a "sysfs" export, the first access
    to the direction (or any other) register takes about 200 ms. I have
    taken care of this by retrying  every 50 ms for a max of 2.5 seconds
    before giving up. (Discussed in a number of forums).

The I/O connections on any MCP23017 board are controlled from the
Raspberry Pi via four different paths:

 1) the kernel I2C system can write and read to and from the MCB23017
    16-Bit I/O Expander chip on a Piface. For Details see:
    http://ww1.microchip.com/downloads/en/DeviceDoc/21952b.pdf
    There are two I2C channels on the Raspberry Pi:
        /dev/i2cdev0.0 - which enables CS CE0 when I2C writes or reads.
        /dev/i2cdev0.1 - which enables CS CE1 when I2C writes or reads.
    The MCB23017 is a slave I2C device. The slave address contains four
    fixed bits and three user-defined hardware address bits pins A0, A1
    and A2

 2) The MCB23017 has a number of 8 bit registers which can be written
    and read via the above I2C channels. The most important of these
    for the MCP23017 driver are:
        IOCON   - which sets the configuration of the MCB23017.
        IODIRA  - set to 0x00, which makes all 8 port A pins outputs.
        IODIRB  - set to 0xff, which makes all 8 port B pins inputs.
        OLATA   - which sets the 8 output pins when written to.
        GPIOB   - which returns the 8 input pins when read.
        GPINTENB- set to 0xff, a change on any port B pin activates INTB.
        INTFB   - set for any pin which changed and caused an interrupt.
        INTCAPB - which returns the 8 input pins at interrupt time.

    The INTB output, which signals a change on any of the input bits,
    is set to active-low in an open-drain configuration to allow for more
    than one MCP23017. INTB is connected to pin 13 on all MCP23017s going to
    GPIO 27 on the Raspberry Pi.

    Reading either GPIOB or INTCAPB resets the source of the input
    interrupt.  INTCAPB only supplies the state at interrupt time,
    which does not necessarily reflect the state of the input when
    executing the interrupt handler. Using INTCAPB sometimes led to
    lost input changes. Therefore I only read GPIOB.

    When INTFB from several MCP23017s are scanned in the interrupt
    handler, it sometimes happens that another change of input
    (usually due to fast bounces from a pushbutton contact) occurs
    before the last MCP23017 has been dealt with sending its INTFB low
    again. This means that INTB will remain low after the last MCP23017
    has been dealt with. Since the GPIO 27 interrupt only occurs on
    a falling edge, no interrupt will be seen for the latest change
    and the system hangs with INTB permanently low. That is why it is
    important to read the GPIO 27 value after all Pifaces have been
    scanned. If it is still low, the scan is repeated until it goes
    high, which makes GPIO 27 ready for the next falling edge interrupt.

 3) GPIO 27 (the interrupt from the MCP23017 inputs) is exported with
    "sysfs" in the MCP23017 driver and set to interrupt on its falling
    edge. The interrupt is out-of-band, so if select(2) is used,
    the third exception file descriptor set must be used. select is
    generally used in the iC system, rather than poll(2), because
    select handles timeouts correctly when interrupted.

    NOTE: only one instance of iCpiI2C may be run and all MCP23017s as
    well as extra GPIO's must be controlled by this one instance. If
    two instances were running, the common interrupts would clash.
    If GPIO 27 has already been exported by another program or by
    another instance of iCpiI2C the program exits with an error
    message, since GPIO 27 cannot be used by more than one program
    simultaneously.  Some programs (MCP23017 Digital Emulator) do
    not unexport GPIO 27 when they close. The -f command line option
    can be used to force use of GPIO 27 if you are sure there is no
    running program actually using GPIO 27.

=head1 AUTHOR

John E. Wulff

=head1 BUGS

Email bug reports to B<immediateC@gmail.com> with L<iC Project> in the
subject field.

=head1 SEE ALSO

L<iCpiPiFace(1)>, L<iCpiPWM(1)>, L<iCtherm(1)>, L<iCserver(1)>, L<iCbox(1)>,
L<makeAll(1)>, L<select(2)>

=head1 COPYRIGHT

COPYRIGHT (C) 2014-2023  John E. Wulff

You may distribute under the terms of either the GNU General Public
License or the Artistic License, as specified in the README file.

For more information about this program, or for information on how
to contact the author, see the README file.

=cut

   ############ end of POD to generate iCpiI2C man page ##################### */
