static const char cexe_h[] =
"@(#)$Id: cexe.h,v 1.14 2002/07/05 17:00:45 jw Exp $";
/********************************************************************
 *
 *	Copyright (C) 1985-2001  John E. Wulff
 *
 *  You may distribute under the terms of either the GNU General Public
 *  License or the Artistic License, as specified in the README file.
 *
 *  For more information about this program, or for information on how
 *  to contact the author, see the README file or <john@je-wulff.de>
 *
 *	cexe.h
 *	template to generate cexe.c
 *	for extensions to the compiler run time system
 *	to execute literal blocks and arithmetic expressions
 *
 *******************************************************************/

#include <stdio.h>
#include "icc.h"
#include "comp.h"

#define _(x) Lookup(#x)->u.gate->gt_old
#define _A(x,v) assign(Lookup(#x)->u.gate, v)

static Symbol *
Lookup(char *	string)	/* find string in symbol table at run time */
{
    Symbol *	sp;

    if ((sp = lookup(string)) == 0) {
#ifndef _WINDOWS
	fflush(outFP);
	fprintf(errFP,
	    "\n*** Error: cexe.c: Lookup could not find Symbol '%s' at run time.\n"
	      "*** Usually this is a term in a C function which does not match.\n"
	      "*** Check that 'cexe.c' was built from '%s'\n"
	      "*** Rebuild compiler using '%s -c %s'\n"
	      , string, inpNM, progname, inpNM);
#endif
	quit(-3);
    }
    return sp;				/* found */
} /* Lookup */

/********************************************************************
 *
 *	Literal blocks and embedded C fragment cases
 *
 *******************************************************************/

Q
int
c_exec(int pp_index, Gate * _cexe_gf)
{
    switch (pp_index) {
V
    default:
#ifndef _WINDOWS
	fflush(outFP);
	fprintf(errFP,
	    "\n*** Error: cexe.c: C function 'F(%d)' is unknown.\n"
	      "*** Check that 'cexe.c' was built from '%s'\n"
	      "*** Rebuild compiler using '%s -c %s'\n"
	      , pp_index, inpNM, progname, inpNM);
	quit(-1);
#endif
	break;
    }
#ifndef _WINDOWS
    fflush(outFP);
    fprintf(errFP,
	"\n%s: line %d: Function fragment without return ???\n",
	__FILE__, __LINE__);
    quit(-2);
#else
    return 0;	/* for those cases where no return has been programmed */
#endif
} /* c_exec */
