/* bc_generate.c -- sieve bytecode- almost flattened bytecode
 * Rob Siemborski
 *
 * Copyright (c) 1994-2008 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any legal
 *    details, please contact
 *      Carnegie Mellon University
 *      Center for Technology Transfer and Enterprise Creation
 *      4615 Forbes Avenue
 *      Suite 302
 *      Pittsburgh, PA  15213
 *      (412) 268-7393, fax: (412) 268-7395
 *      innovation@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
 
#include "sieve_interface.h"
#include "bytecode.h"

#if DUMPCODE

/*this would work a lot better if we actually could tell how many levels deep in if statements we were.  currently it doesn't know*/

static void print_spaces(int n)
{
    int temp_n=0;
    while(temp_n++ < (n)) {
	putchar(' ');
	putchar(' ');
	putchar(' ');
	putchar(' ');
    }
}


/* Dump a stringlist.  Return the last address used by the list */
static int dump_sl(bytecode_info_t *d, int ip, int level) 
{
    int numstr = d->data[ip].listlen;
    int i;
    
    for(i=0; i<numstr; i++) {
	print_spaces(level);
	printf(" {%d}",d->data[++ip].len);
	printf("%s\n",d->data[++ip].str);
    }
    
    return ip;
}

static int dump_test(bytecode_info_t *d, int ip, int level);

/* Dump a testlist.  Return the last address used by the list */
static int dump_tl(bytecode_info_t *d, int ip, int level)
{
    int numtest = d->data[ip].listlen;
    int i;
    
    for(i=0; i<numtest; i++) {
	print_spaces(level);
	printf(" (until %d)\n", d->data[++ip].jump);
	ip = dump_test(d, ++ip, level);
    }
    
    return ip;
}

/* Dump a test, return the last address used by the test */
static int dump_test(bytecode_info_t *d, int ip, int level ) {
    int has_index=0;

    const int opcode = d->data[ip].op;

    print_spaces(level);
    switch(opcode) {
    case BC_TRUE:
	printf("%d: TRUE\n",ip);
	break;

    case BC_FALSE:
	printf("%d: FALSE\n",ip);
	break;

    case BC_NOT:
	printf("%d: NOT TEST(\n",ip++);
	/*   printf("  (until %d)\n", d->data[ip++].jump);*/
	ip = dump_test(d,ip, level);
	print_spaces(level);
	printf("    )\n");
	break;

    case BC_SIZE:
	printf("%d: SIZE TAG(%d) NUM(%d)\n",ip,
	       d->data[ip+1].value, d->data[ip+2].value);
	ip+=2;
	break;

    case BC_EXISTS:
	printf("%d: EXISTS\n",ip++);
	ip = dump_sl(d,ip,level);
	break;

    case BC_ALLOF:
    case BC_ANYOF:
	printf("%d: %s (\n",ip,
	       d->data[ip++].op == BC_ALLOF ? "ALLOF" : "ANYOF");
	ip = dump_tl(d,ip,level);
	print_spaces(level);
	printf(")\n");
	break;

    case BC_HEADER:
	has_index=1;
    case BC_HEADER_PRE_INDEX:
	printf("%d: HEADER (\n",ip++);

	if (has_index) {
		print_spaces(level);
		printf("      INDEX:%d\n", d->data[ip++].value);
	}

	print_spaces(level);
	if (d->data[ip].value == B_COUNT || d->data[ip].value == B_VALUE)
	{
	    printf("      MATCH:%d RELATION:%d COMP:%d HEADERS:\n",
		   d->data[ip].value, d->data[ip+1].value,d->data[ip+2].value);
	} else {
	    printf("      MATCH:%d COMP:%d HEADERS:\n",d->data[ip].value, d->data[ip+2].value);
	}
	ip+=3;
	ip = dump_sl(d,ip,level);
	ip++;
	print_spaces(level);
	printf("      DATA:\n");
	ip = dump_sl(d,ip,level);
	break;
	
    case BC_ADDRESS:
	has_index = 1;
    case BC_ADDRESS_PRE_INDEX:
    case BC_ENVELOPE:
	if (d->data[ip].op == BC_ADDRESS) {
		printf("%d: ADDRESS (\n",ip++);
	}
	else {
		printf("%d: ENVELOPE (\n",ip++);
	}

	if (has_index) {
		print_spaces(level);
		printf("      INDEX:%d\n", d->data[ip++].value);
	}

	print_spaces(level);
	if (d->data[ip].value == B_COUNT || d->data[ip].value == B_VALUE)
	{
	    printf("      MATCH:%d RELATION:%d COMP: %d TYPE: %d HEADERS:\n",
		   d->data[ip].value, d->data[ip+1].value, d->data[ip+2].value, d->data[ip+3].value);
	} else {
	    printf("      MATCH:%d COMP:%d TYPE:%d HEADERS:\n",
		   d->data[ip].value,d->data[ip+1].value,d->data[ip+3].value);
	}
	ip+=4;
	ip = dump_sl(d,ip,level); ip++;
	print_spaces(level);
	printf("      DATA:\n");
	ip = dump_sl(d,ip,level);
	break;

    case BC_BODY:
	printf("%d: BODY (\n",ip++);
	print_spaces(level);
	if (d->data[ip].value == B_COUNT || d->data[ip].value == B_VALUE)
	{
	    printf("      MATCH:%d RELATION: %d COMP: %d TRANSFORM: %d OFFSET: %d CONTENT-TYPES:\n", 
		   d->data[ip].value,d->data[ip+1].value,d->data[ip+2].value,
		   d->data[ip+3].value,d->data[ip+4].value);
	} else {
	    printf("      MATCH:%d COMP:%d TRANSFORM:%d OFFSET: %d CONTENT-TYPES:\n",
		   d->data[ip].value,d->data[ip+1].value,d->data[ip+3].value,
		   d->data[ip+4].value);
	}
	ip+=5;
	ip = dump_sl(d,ip,level); ip++;
	print_spaces(level);
	printf("      DATA:\n");
	ip = dump_sl(d,ip,level);
	break;

    case BC_DATE:
	has_index=1;
    case BC_CURRENTDATE:
	if (BC_DATE == opcode) {
		printf("%d: DATE (\n", ip++);
	}
	else {
		printf("%d: CURRENTDATE (\n", ip++);
	}

	/* index */
	if (has_index) {
		print_spaces(level);
		printf("      INDEX:%d\n", d->data[ip++].value);
	}

	/* zone tag */
	print_spaces(level);
	printf("      ZONE-TAG:%d ", d->data[ip].value);

	switch (d->data[ip++].value) {
	case B_TIMEZONE:
		printf("TIMEZONE:%d\n", d->data[ip++].value);
		break;
	case B_ORIGINALZONE:
		printf("ORIGINALZONE\n");
		break;
	}

	/* comparison */
	print_spaces(level);
	if (d->data[ip].value == B_COUNT || d->data[ip].value == B_VALUE)
	{
	    printf("      MATCH:%d RELATION:%d COMP:%d\n",
		   d->data[ip].value, d->data[ip+1].value, d->data[ip+2].value);
	} else {
	    printf("      MATCH:%d COMP:%d\n", d->data[ip].value, d->data[ip+2].value);
	}
	ip+=3;

	/* date type */
	print_spaces(level);
	printf("      DATE-TYPE:%d\n", d->data[ip++].value);

	/* header name */
	if (BC_DATE == opcode) {
		print_spaces(level);
		printf("      HEADER NAME: {%d}", d->data[ip++].len);
		printf("%s\n",d->data[ip++].str);
	}

	print_spaces(level);
	printf("      KEY LIST:\n");
	ip = dump_sl(d,ip,level);
	break;

	break;

    default:
	printf("%d: TEST(%d)\n",ip,d->data[ip].op);
	break;
    }

    return ip;
}

void dump(bytecode_info_t *d, int level) 
{
    int i;
    printf("Dumping almost flattened bytecode\n\n");
    
    if(!d) return;

    for(i=0; i<d->scriptend; i++) {
	print_spaces(level);
	switch(d->data[i].op) {
	case B_REJECT:
	    printf("%d: REJECT {%d}%s\n",i,
		   d->data[i+1].len,d->data[i+2].str);
	    i+=2;
	    break;
	case B_IF:
	    if (d->data[i+3].jump== -1)
	    {
		printf("%d: IF THEN(%d) POST(%d) TEST(\n",i,
		       d->data[i+1].jump,d->data[i+2].jump);
	    }
	    else
	    {
		printf("%d: IF THEN(%d) ELSE(%d) POST(%d) TEST(\n",i,
		       d->data[i+1].jump,d->data[i+2].jump,
		       d->data[i+3].jump);
	    }
	    i = dump_test(d,i+4, level+1);
	    printf(")\n");
	    break;

	case B_STOP:
	    printf("%d: STOP\n",i);
	    break;

	case B_DISCARD:
	    printf("%d: DISCARD\n",i);
	    break;
	    
	case B_KEEP:
	    printf("%d: KEEP\n",i);
	    break;

	case B_MARK:
	    printf("%d: MARK\n",i);
	    break;

	case B_UNMARK:
	    printf("%d: UNMARK\n",i);
	    break;

	case B_FILEINTO:
	    printf("%d: FILEINTO COPY(%d) FOLDER({%d}%s)\n",i,
		   d->data[i+1].value,d->data[i+2].len,d->data[i+3].str);
	    i+=3;
	    break;

	case B_REDIRECT:
	    printf("%d: REDIRECT COPY(%d) ADDRESS({%d}%s)\n",i,
		   d->data[i+1].value,d->data[i+2].len,d->data[i+3].str);
	    i+=3;
	    break;

	case B_SETFLAG:
	    printf("%d: SETFLAG\n",i);
	    i=dump_sl(d,++i, level);
	    break;

	case B_ADDFLAG:
	    printf("%d: ADDFLAG\n",i);
	    i=dump_sl(d,++i,level);
	    break;

	case B_REMOVEFLAG:
	    printf("%d: REMOVEFLAG\n",i);
	    i=dump_sl(d,++i,level);
	    break;

	case B_DENOTIFY:
	    printf("%d: DENOTIFY priority %d,comp %d %d  %s\n", 
		   i,
		   d->data[i+1].value,
		   d->data[i+2].value,
		   d->data[i+3].value,
		   (d->data[i+4].len == -1 ? "[nil]" : d->data[i+5].str));
	    i+=5;
	    break;

	case B_NOTIFY: 
	    printf("%d: NOTIFY\n   METHOD(%s),\n   ID(%s),\n   OPTIONS",
		   i,
		   d->data[i+2].str,
		   (d->data[i+3].len == -1 ? "[nil]" : d->data[i+4].str));
	    i+=5;
	    i=dump_sl(d,i,level);
	    printf("   PRIORITY(%d),\n   MESSAGE({%d}%s)\n", 
		   d->data[i+1].value, d->data[i+2].len,d->data[i+3].str);
	    i+=3;
	    break;

	case B_VACATION_ORIG:
	case B_VACATION:
	    printf("%d:VACATION\n",i);
	    i++;
	    i=dump_sl(d,i,level);
	    printf("SUBJ({%d}%s) MESG({%d}%s)\n SECONDS(%d) MIME(%d)\n"
		   " FROM({%d}%s) HANDLE({%d}%s)\n",
		   d->data[i+1].len, (d->data[i+1].len == -1 ? "[nil]" : d->data[i+2].str),
		   d->data[i+3].len, (d->data[i+3].len == -1 ? "[nil]" : d->data[i+4].str),
		   d->data[i+5].value * (d->data[i].op == B_VACATION ? 1 : (24 * 60 * 60 /* 1 day */)), d->data[i+6].value,
		   d->data[i+7].len, (d->data[i+7].len == -1 ? "[nil]" : d->data[i+8].str),
		   d->data[i+9].len, (d->data[i+9].len == -1 ? "[nil]" : d->data[i+10].str));
	    i+=10;
	
	    break;
	case B_JUMP:
	    printf("%d: JUMP HUH?  this shouldn't be here>?!",i);
	    break;
	case B_NULL:
	    printf("%d: NULL\n",i);
	    break;

	case B_INCLUDE:
	    printf("%d: INCLUDE ONCE:%s OPTIONAL:%s LOCATION:%s {%d}%s\n",i,
		   d->data[i+1].value & 64 ? "yes" : "no",
		   d->data[i+1].value & 128 ? "yes" : "no",
		   (d->data[i+1].value & 63) == B_GLOBAL ? "global" : "local",
		   d->data[i+2].len,d->data[i+3].str);
	    i+=3;
	    break;

	case B_RETURN:
	    printf("%d: RETURN\n",i);
	    break;

	default:
	    printf("%d: %d\n",i,d->data[i].op);
	    break;
	}
    }
    printf("full len is: %d\n", d->scriptend);
}
#endif

