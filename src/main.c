static const char main_c[] =
"@(#)$Id: main.c,v 1.8 2000/06/10 11:27:58 jw Exp $";
/*
 *	"main.c"
 *	compiler for pplc
 *
 *	"main.c	1.10	95/02/09"
 */

#include	<stdio.h>
#include	<stdlib.h>
#include	"pplc.h"
#include	"comp.h"
#ifdef TCP
#include	"tcpc.h"
#endif

extern const char	SC_ID[];

static const char *	usage = "\
USAGE for compile mode:\n\
  %s [-aAc] [-o<out>] [-l<list>] [-e<err>] [-d<debug>] <iC_program>\n\
       Options in compile mode only:\n\
        -o <outFN>      name of compiler output file - sets compile mode\n\
        -a              append linking info for 2nd and later files\n\
        -A              compile output ARITHMETIC ALIAS nodes for symbol debugging\n\
        -c              generate auxiliary file cexe.c to extend pplc compiler\n\
USAGE for run mode:\n\
  %s [-txh]"
#ifdef TCP
" [-s <server>] [-p <port>] [-u <unitID>]"
#endif
" [-l<list>] [-e<err>] [-d<debug>] [-n<count>] <iC_program>\n\
       Options in both modes:\n"
#ifdef TCP
"        -s host ID of server      (default '%s')\n\
        -p service port of server (default '%s')\n\
        -u unit ID of this client (default '%s')\n"
#endif
"        -l <listFN>     name of list file  (default is stdout)\n\
        -e <errFN>      name of error file (default is stderr)\n\
        -d <debug>2000  display scan_cnt and link_cnt\n\
                 +1000  I0 toggled every second\n\
                  +400  exit after initialisation\n\
                  +200  display loop info (+old style logic)\n\
                  +100  initialisation and run time info\n\
                   +40  net statistics\n\
                   +20  net topology\n\
                   +10  source listing\n\
                    +4  logic expansion\n"
#ifdef YYDEBUG
"                    +2  logic generation\n\
                    +1  yacc debug info\n"
#endif
"       Options in run mode only:\n\
        -t              trace debug (equivalent to -d 100)\n\
        -n <count>      maxinum loop count (default is %d, limit 15)\n\
        -x              arithmetic info in hexadecimal (default decimal)\n\
                        can be changed at run time with x or d\n\
        -h              this help text\n\
\n\
        <iC_program>    any iC language program file (extension .iC)\n\
                        - or default is stdin\n\
			typing q or ctrl-C quits run time mode\n\
Copyright (C) 1985-2001 John E. Wulff     <john.wulff@inka.de>\n\
%s\n";

short		debug = 0;
unsigned short	xflag;
unsigned short	osc_max = MARKMAX;
#ifdef YYDEBUG
extern	int	yydebug;
#endif

char *		szFile_g;		/* file name to process */
#define progFN	szNames[0]		/* for error messages */
#define inpFN	szNames[1]		/* list file name */
#define errFN	szNames[2]		/* error file name */
#define listFN	szNames[3]		/* list file name */
#define outFN	szNames[4]		/* compiler output file name */
#define exiFN	szNames[5]		/* cexe input file name */
#define exoFN	szNames[6]		/* cexe output file name */
char *		szNames[7] = {		/* matches return in compile */
    	0, 0, 0, 0, 0, 0, "cexe.tmp",
};

char * OutputMessage[] = {
    0,					/* [0] no error */
    "%s: syntax or generate errors\n",	/* [1] */
    "%s: block count error\n",		/* [2] */
    "%s: link count error\n",		/* [3] */
    "%s: cannot open file %s\n",	/* [4] */
};


/********************************************************************
 *
 *	main program
 *
 *******************************************************************/

int
main(
    int		argc,
    char * *	argv)
{
    int		r;		/* return value of compile */
    progFN = *argv;		/* Process the arguments */
    inFP = stdin;		/* input file pointer */
    outFP = stdout;		/* listing file pointer */
    errFP = stderr;		/* error file pointer */
    while (--argc > 0) {
	if (**++argv == '-') {
	    ++*argv;
	    do {
		switch (**argv) {
		    int	debi;

		case '\0':
		    szFile_g = 0;	/* - is standard input */
#ifdef TCP
		case 's':
		    if (! *++*argv) { --argc, ++argv; }
		    hostNM = *argv;
		    goto break2;
		case 'p':
		    if (! *++*argv) { --argc, ++argv; }
		    portNM = *argv;
		    goto break2;
		case 'u':
		    if (! *++*argv) { --argc, ++argv; }
		    pplcNM = *argv;
		    goto break2;
#endif
		case 'd':
		    if (! *++*argv) { --argc, ++argv; }
		    sscanf(*argv, "%o", &debi);
		    debug = debi;	/* short */
#ifdef YYDEBUG
		    yydebug = debug & 01;
#endif
		    goto break2;
		case 't':
		    debug = 0100;		/* trace only */
		    break;
		case 'n':
		    if (! *++*argv) { --argc, ++argv; }
		    osc_max = atoi(*argv);
		    if (osc_max > 15) goto error;
		    goto break2;
		case 'o':
		    if (! *++*argv) { --argc, ++argv; }
		    outFN = *argv;	/* compiler output file name */
		    exiFN = 0;
		    exoFN = "cexe.tmp";
		    goto break2;
		case 'l':
		    if (! *++*argv) { --argc, ++argv; }
		    listFN = *argv;	/* listing file name */
		    goto break2;
		case 'e':
		    if (! *++*argv) { --argc, ++argv; }
		    errFN = *argv;	/* error file name */
		    goto break2;
		case 'c':
		    if (outFN == 0) {
			exiFN = "cexe.h";
			exoFN = "cexe.c";
		    }
		case 'A':
		    Aflag = 1;		/* generate ARITH ALIAS in outFN */
		    break;
		case 'a':
		    aflag = 1;		/* append for compile */
		    break;
		case 'x':
		    xflag = 1;		/* start with hexadecimal display */
		    break;
		default:
		    fprintf(stderr,
			"%s: unknown flag '%c'\n", progFN, **argv);
		case 'h':
		case '?':
		error:
		    fprintf(stderr, usage, progFN, progFN,
#ifdef TCP
		    hostNM, portNM, pplcNM,
#endif
		    MARKMAX, SC_ID);
		    exit(1);
		}
	    } while (*++*argv);
	    break2: ;
	} else {
	    inpFN = szFile_g = *argv;
	}
    }
    if ((r = compile(listFN, errFN, outFN, exiFN, exoFN)) != 0) {
	fprintf(stderr, OutputMessage[4], progFN, szNames[r]);
    } else if ((r = output(outFN)) != 0) {
	fprintf(stderr, OutputMessage[r < 4 ? r : 4], progFN, szNames[r]);
	r += 10;
    }
    if (exoFP) fclose(exoFP);
    return (r);	/* 1 - 6 compile errors, 11 - 16 output errors */
} /* main */
