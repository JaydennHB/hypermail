/* 
** Copyright (C) 1994, 1995 Enterprise Integration Technologies Corp.
**         VeriFone Inc./Hewlett-Packard. All Rights Reserved.
** Kevin Hughes, kev@kevcom.com 3/11/94
** Kent Landfield, kent@landfield.com 4/6/97
** 
** This program and library is free software; you can redistribute it and/or 
** modify it under the terms of the GNU (Library) General Public License 
** as published by the Free Software Foundation; either version 2 
** of the License, or any later version. 
** 
** This program is distributed in the hope that it will be useful, 
** but WITHOUT ANY WARRANTY; without even the implied warranty of 
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the 
** GNU (Library) General Public License for more details. 
** 
** You should have received a copy of the GNU (Library) General Public License
** along with this program; if not, write to the Free Software 
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA 
*/

#include "hypermail.h"
#include "setup.h"
#include "struct.h"
#include "uudecode.h"
#include "base64.h"
#include "getname.h"
#include "parse.h"

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#ifdef HAVE_DIRENT_H
#include <dirent.h>
#else
#include <sys/dir.h>
#endif

extern char *mktemp(char *);

/*
 * Suffix to prepend to all saved attachments' filenames when the
 * headers don't propose a filename
 */
#define FILE_SUFFIXER "part"

/*
 * Prefix to prepend to all saved attachments' directory names (before
 * the article number)
 */
#define DIR_PREFIXER "att-"

/* 
 * Used to replace invalid characters in supplied attachment filenames
 */
#define REPLACEMENT_CHAR '_'

/* 
 * Path separator for attachment file path generation
 */
#define PATH_SEPARATOR '/'

/*
 * Directory where meta information will be stored
 */
#define META_DIR ".meta"

/*
 * Extension to add to meta files
 */
#define META_EXTENSION ".meta"

typedef enum {
    ENCODE_NORMAL,
    ENCODE_QP,			/* quoted printable */

    ENCODE_MULTILINED,		/* this is not a real type, but just a separator showing
				   that the types below are encoded in a way that makes
				   one line in the indata may become one or more lines
				   in the outdata */

    ENCODE_BASE64,		/* base64 */
    ENCODE_UUENCODE,		/* well, it seems there exist some kind of semi-standard
				   for uu-encoded attachments. */

    ENCODE_UNKNOWN		/* must be the last one */
} EncodeType;

typedef enum {
    CONTENT_TEXT,		/* normal mails are text based default */
    CONTENT_BINARY,		/* this kind we store separately and href to */
    CONTENT_HTML,		/* this is html formated text */
    CONTENT_IGNORE,		/* don't care about this content */

    CONTENT_UNKNOWN		/* must be the last one */
} ContentType;

static int hasblack(char *p)
{
   while(p && *p && isspace(*p++));
   return (*p ? TRUE : FALSE);
}

int ignorecontent(char *type)
{
    return (inlist(set_ignore_types, type));
}

int inlinecontent(char *type)
{
    return (inlist(set_inline_types, type));
}

int preferedcontent(int *current_weight, char *type)
{
    int weight;
    int status;

    status = 0;

    /* We let plain text remain PREFERED at all times */
    if (!strcasecmp("text/plain", type)) {
	if (*current_weight != 0) {
	    /* to avoid having two text/plain alternatives */
	    *current_weight = 0;
	    status = 1;
	}
    }
    /* find the weight of the type arg. If the weight is
       inferior to the current_weight, we make it the
       prefered content */
    else if (set_prefered_types) {
	weight = inlist_pos(set_prefered_types, type);
	if (weight != -1) {
	    /* +1 so that weight 0 is reserved for text/plain */
	    weight++;
	    if (*current_weight == -1) {
		*current_weight = weight;
		status = 1;
	    }
	    else if (*current_weight > weight) {
		*current_weight = weight;
		status = 1;
	    }
	}
    }

    return status;
}

int textcontent(char *type)
{
    /* We let text/plain remain text at all times.  Appearantly, older mailers
     * can still use just "text" as content-type, and we better treat that as
     * text/plain to make all those users happy.  */
    if (!strcasecmp("text/plain", type) || !strcasecmp("text", type))
	return 1;

    if (set_text_types) {
	return (inlist(set_text_types, type));
    }

    return 0;
}


/*
 * Should return TRUE if the input is a Re: start. The end pointer should
 * then point on the first character after the Re:
 *
 * Identifies "Re:", "Fw:" as well as "Re[<number>]:" strings.
 */

int isre(char *re, char **end)
{
    char *endp = NULL;
    if (!strncasecmp("Re:", re, 3)) {
	endp = re + 3;
    }
    else if (!strncasecmp("Fw:", re, 3)) {
	endp = re + 3;
    }
    else if (!strncasecmp("Re[", re, 3)) {
	long level;
	re += 3;
	level = strtol(re, &re, 10);	/* eat the number */
	if (!strncmp("]:", re, 2)) {
	    /* we have an end "]:" and therefore it qualifies as a Re */
	    endp = re + 2;
	}
    }
    if (endp) {
	if (end)
	    *end = endp;
	return TRUE;
    }
    return FALSE;
}

/*
 * Find the first re-substring in the input and return the position
 * where it is. The 'end' parameter will be filled in the first position
 * *after* the re.
 */

char *findre(char *in, char **end)
{
    while (*in) {
	if (isre(in, end))
	    return in;
	in++;
    }
    return NULL;
}


void print_progress(int num, char *msg, char *filename)
{
    char bufstr[256];
    register int i;
    static int lastlen = 0;
    static int longest = 0;
    int len = 0;
    int newline = 0;

    newline = 0;

    if (msg != NULL) {
	if (filename != NULL) {
	    sprintf(bufstr, "%4d %-s %-s", num, msg, filename);
	    if (set_showprogress > 1)
		newline = 1;
	}
	else {
	    sprintf(bufstr, "%4d %-s.", num, msg);
	    newline = 1;
	}
    }
    else
	sprintf(bufstr, "%4d", num);

    for (i = 0; i < lastlen; i++)	/* Back up to the beginning of line */
	fputc('\b', stdout);

    fputs(bufstr, stdout);	/* put out the string */
    len = strlen(bufstr);	/* get length of new string */

    /* 
     * If there is a new message then erase 
     * the trailing info from the enw string 
     */

    if (msg != NULL) {
	for (i = len; i <= longest; i++)
	    fputc(' ', stdout);
	for (i = len; i <= longest; i++)
	    fputc('\b', stdout);
    }

    lastlen = len;
    if (lastlen > longest)
	longest = lastlen;

    if (newline)
	fputc('\n', stdout);
    fflush(stdout);
}

char *tmpname(char *dir, char *pfx)
{
    char *f, *name;
    static int cntr = 0;

    name = maprintf("%s/%s%dXXXXXX", dir, pfx, cntr++);
    if (NULL == name)
	return (NULL);

    if ((f = mktemp(name)) != NULL)
	return (f);

    /* this means failure: */
    free(name);
    return (NULL);
}

char *safe_filename(char *name)
{
    register char *sp;
    register char *np;

    np = name;
    while (*np && (*np == ' ' || *np == '\t'))
	np++;

    if (!*np)
	return (NULL);

    for (sp = name, np = name; *np && *np != '\n';) {
	/* if valid character then store it */
	if ((*np >= 'a' && *np <= 'z') || (*np >= '0' && *np <= '9') ||
	    (*np >= 'A' && *np <= 'Z') || (*np == '-') || (*np == '.') ||
	    (*np == ':') || (*np == '_')) {
	    *sp = *np;
	}
	else	/* Need to replace the character with a safe one */
	    *sp = REPLACEMENT_CHAR;
	sp++;
	np++;
    }
    *sp = '\0';

    return name;
}

/*
** Cross-indexes - adds to a list of replies. If a message is a reply to
** another, the number of the message it's replying to is added to the list.
** This list is searched upon printing.
*/

void crossindex(void)
{
    int num, status, maybereply;
    struct emailinfo *email;

    num = 0;
    replylist = NULL;

    while (hashnumlookup(num, &email)) {
	status = hashreplynumlookup(email->msgnum,
				    email->inreplyto, email->subject,
				    &maybereply);
	if (status != -1)
	    replylist = addreply(replylist, status, email, maybereply);
	num++;
    }
#if DEBUG_THREAD
    {
	struct reply *r;
	r = replylist;
	fprintf(stderr, "START of replylist after crossindex\n");
	fprintf(stderr, "- msgnum frommsgnum maybereply msgid\n");
	while (r != NULL) {
	    fprintf(stderr, "- %d %d %d '%s'\n",
		    r->data->msgnum,
		    r->frommsgnum, r->maybereply, r->data->msgid);
	    r = r->next;
	}
	fprintf(stderr, "END of replylist after crossindex\n");
    }
#endif
}

/* 
** Recursively checks for replies to replies to a message, etc.
** Replies are added to the thread list.
*/

void crossindexthread2(int num)
{
    struct reply *rp;

    for (rp = replylist; rp != NULL; rp = rp->next) {
	if (!(rp->data->flags & USED_THREAD) && (rp->frommsgnum == num)) {
	    rp->data->flags |= USED_THREAD;
	    threadlist = addreply(threadlist, num, rp->data, 0);
	    printedlist = markasprinted(printedthreadlist, rp->msgnum);
	    crossindexthread2(rp->msgnum);
	}
    }
}


/*
** First, print out the threads in order by date...
** Each message number is appended to a thread list. Threads and individual
** messages are separated by a -1.
*/

void crossindexthread1(struct header *hp)
{
    int isreply;
    struct reply *rp;

    if (hp) {
	crossindexthread1(hp->left);

	for (isreply = 0, rp = replylist; rp != NULL; rp = rp->next) {
	    if (rp->msgnum == hp->data->msgnum) {
		isreply = 1;
		break;
	    }
	}

	/* If this message is not a reply to any other messages then it
	 * is the first message in a thread.  If it hasn't already
	 * been dealt with, then add it to the thread list, followed by
	 * any descendants and then the end of thread marker.
	 */
	if (!isreply && !wasprinted(printedthreadlist, hp->data->msgnum) &&
	    !(hp->data->flags & USED_THREAD)) {
	    hp->data->flags |= USED_THREAD;
	    threadlist =
		addreply(threadlist, hp->data->msgnum, hp->data, 0);
	    crossindexthread2(hp->data->msgnum);
	    threadlist = addreply(threadlist, -1, NULL, 0);
	}

	crossindexthread1(hp->right);
    }
}

/*
** Grabs the date string from a Date: header. (Y2K OK)
*/

char *getmaildate(char *line)
{
    int i;
    int len;
    char *c;
    struct Push buff;

    INIT_PUSH(buff);

    c = strchr(line, ':');
    if ((*(c + 1) && *(c + 1) == '\n') || (*(c + 2) && *(c + 2) == '\n')) {
	PushString(&buff, NODATE);
	RETURN_PUSH(buff);
    }
    c += 2;
    while (*c == ' ' || *c == '\t')
	c++;
    for (i = 0, len = DATESTRLEN - 1; *c && *c != '\n' && i < len; c++)
	PushByte(&buff, *c);

    RETURN_PUSH(buff);
}

/*
** Grabs the date string from a From article separator. (Y2K OK)
*/

char *getfromdate(char *line)
{
    static char tmpdate[DATESTRLEN];
    int i;
    int len;
    char *c = NULL;

    for (i = 0; days[i] != NULL &&
	 ((c = strstr(line, days[i])) == NULL); i++);
    if (days[i] == NULL)
	tmpdate[0] = '\0';
    else {
	for (i = 0, len = DATESTRLEN - 1; *c && *c != '\n' && i < len; c++)
	    tmpdate[i++] = *c;

	tmpdate[i] = '\0';
    }
    return tmpdate;
}


/* 
** Grabs the message ID, like <...> from the Message-ID: header.
*/

char *getid(char *line)
{
    int i;
    char *c;

    struct Push buff;

    INIT_PUSH(buff);

    if (strrchr(line, '<') == NULL) {
	/* 
         * bozo alert!
	 *   msg-id = "<" addr-spec ">" 
	 * try to recover as best we can
	 */
	c = strchr(line, ':') + 1;	/* we know this exists! */

	/* skip spaces before message ID */
	while (*c && (*c == ' ' || *c == '\t'))
	    c++;
    }
    else
	c = strrchr(line, '<') + 1;

    for (i = 0; *c && *c != '>' && *c != '\n'; c++) {
	if (*c == '\\')
	    continue;
	PushByte(&buff, *c);
	i++;
    }

    if (i == 0)
	PushString(&buff, "BOZO");

    RETURN_PUSH(buff);
}


/*
** Grabs the subject from the Subject: header.
**
** Need to add a table of Re: equivalents (different languages, MUA, etc...)
**
** Returns ALLOCATED string.
*/

char *getsubject(char *line)
{
    int i;
    int len;
    char *c;
    char *startp;
    char *strip_subject = NULL;
    char *postre = NULL;

    struct Push buff;

    INIT_PUSH(buff);

    c = strchr(line, ':');
    if (!c)
	return NULL;

    c += 2;

    if (set_stripsubject) {
	/* compute a new subject */
	strip_subject = replace(c, set_stripsubject, "");
	/* point to it */
	c = strip_subject;
    }

    while (isspace(*c))
	c++;

    startp = c;

    for (i = len = 0; c && *c && (*c != '\n'); c++) {
	i++;
	/* keep track of the max length without trailing white spaces: */
	if (!isspace(*c))
	    len = i;
    }

    if (isre(startp, &postre)) {
	if (!*postre || (*postre == '\n'))
	    len = 0;
    }

    if (!len)
	PushString(&buff, NOSUBJECT);
    else
	PushNString(&buff, startp, len);

    if (set_stripsubject && (strip_subject != NULL))
	free(strip_subject);

    RETURN_PUSH(buff);
}

/*
** Grabs the message ID, or date, from the In-reply-to: header.
**
** Maybe I'm confused but....
**     What either ? Should it not be consistent and choose to return 
**     one (the msgid) as the default and fall back to date when a 
**     msgid cannot be found ?
**
** Who knows what other formats are out there...
**
** In-Reply-To: <1DD9B854E27@everett.pitt.cc.nc.us>
** In-Reply-To: <199709181645.MAA02097@mail.clark.net> from "Marcus J. Ranum" at Sep 18, 97 12:41:40 pm
** In-Reply-To: <199709181645.MAA02097@mail.clark.net> from 
** In-Reply-To: "L. Detweiler"'s message of Fri, 04 Feb 94 22:51:22 -0700 <199402050551.WAA16189@longs.lance.colostate.edu>
**
** The message id should always be returned for threading purposes. Mixing
** message-ids and dates just does not allow for proper threading lookups.
**
** Returns ALLOCATED string.  */

char *getreply(char *line)
{
    char *c;
    char *m;

    struct Push buff;

    INIT_PUSH(buff);

    /* Check for blank line */

    /* 
     * Check for line with " from " and " at ".  Format of the line is 
     *     <msgid> from "quoted user name" at date-string
     */

    if (strstr(line, " from ") != NULL) {
	if ((strstr(line, " at ")) != NULL) {
	    if ((m = strchr(line, '<')) != NULL) {
		for (m++; *m && *m != '>' && *m != '\n'; m++) {
		    PushByte(&buff, *m);
		}
		RETURN_PUSH(buff);
	    }
	}

	/* 
	 * If no 'at' the line may be a continued line or a truncated line.
	 * Both will be picked up later.
	 */
    }

    /* 
     * Check for line with " message of ".  Format of the line is 
     *     "quoted user name"'s message of date-string <msgid>
     */

    if ((c = strstr(line, "message of ")) != NULL) {
	/*
	 * Check to see if there is a message ID on the line. 
	 * If not this is a continued line and when you add a readline()
	 * function that concatenates continuation lines collapsing
	 * white space, you might want to revisit this...
	 */

	if ((m = strchr(line, '<')) != NULL) {
	    for (m++; *m && *m != '>' && *m != '\n'; m++) {
		PushByte(&buff, *m);
	    }
	    RETURN_PUSH(buff);
	}

	/* Nope... Go for the Date info... Bug... */
	c += 11;
	while (isspace(*c))
	    c++;
	if (*c == '"')
	    c++;

	for (; *c && *c != '.' && *c != '\n'; c++) {
	    PushByte(&buff, *c);
	}
	RETURN_PUSH(buff);
    }

    if ((c = strstr(line, "dated: ")) != NULL) {
	c += 7;
	for (; *c && *c != '.' && *c != '\n'; c++) {
	    PushByte(&buff, *c);
	}
	RETURN_PUSH(buff);
    }

    if ((c = strstr(line, "dated ")) != NULL) {
	c += 6;
	for (; *c && *c != '.' && *c != '\n'; c++) {
	    PushByte(&buff, *c);
	}
	RETURN_PUSH(buff);

    }

    if ((c = strchr(line, '<')) != NULL) {
	c++;
	for (; *c && *c != '>' && *c != '\n'; c++) {
	    if (*c == '\\')
		continue;
	    PushByte(&buff, *c);
	}
	RETURN_PUSH(buff);
    }

    if ((c = strstr(line, "sage of ")) != NULL) {
	c += 8;
	if (*c == '\"')
	    c++;

	for (; *c && *c != '.' && *c != '\n' && *c != 'f'; c++) {
	    PushByte(&buff, *c);
	}
	RETURN_PUSH(buff);
    }

    PushByte(&buff, '\0');
    RETURN_PUSH(buff);
}

/*
** RFC 2047 defines MIME extensions for mail headers.
**
** This function decodes that into binary/8bit data.
**
** Example:
**   =?iso-8859-1?q?I'm_called_?= =?iso-8859-1?q?Daniel?=
**
** Should result in "I'm called Daniel", but:
**
**   =?iso-8859-1?q?I'm_called?= Daniel
**
** Should result in "I'm called Daniel" too.
**
** Returns the newly allcated string, or the previous if nothing changed 
*/

static char *mdecodeRFC2047(char *string, int length)
{
    char *iptr = string;
    char *oldptr;
    char *storage = (char *)emalloc(length + 1);

    char *output = storage;

    char charset[129];
    char encoding[33];
    char blurb[129];
    char equal;
    int value;

    char didanything = FALSE;

    while (*iptr) {
	if (!strncmp(iptr, "=?", 2) &&
	    (3 == sscanf(iptr + 2, "%128[^?]?%32[^?]?%128[^ ]",
			 charset, encoding, blurb))) {
	    /* This is a full, valid 'encoded-word'. Decode! */
	    char *ptr = blurb;

	    ptr = strstr(blurb, "?=");
	    if (ptr) {
		*ptr = 0;
	    }
	    else {
		*output++ = *iptr++;
		/* it wasn't a real encoded-word */
		continue;
	    }
	    ptr = blurb;

	    didanything = TRUE;	/* yes, we decode something */

	    /* we could've done this with a %n in the sscanf, but we know all
	       sscanfs don't grok that */

	    iptr +=
		2 + strlen(charset) + 1 + strlen(encoding) + 1 +
		strlen(blurb) + 2;

	    if (!strcasecmp("q", encoding)) {
		/* quoted printable decoding */

		for (; *ptr; ptr++) {
		    switch (*ptr) {
		    case '=':
			sscanf(ptr + 1, "%02X", &value);
			*output++ = value;
			ptr += 2;
			break;
		    case '_':
			*output++ = ' ';
			break;
		    default:
			*output++ = *ptr;
			break;
		    }
		}
	    }
	    else if (!strcasecmp("b", encoding)) {
		/* base64 decoding */
		int len;
		base64Decode(ptr, output, &len);
		output += len - 1;
	    }
	    else {
		/* unsupported encoding type */
		strcpy(output, "<unknown>");
		output += 9;
	    }

	    oldptr = iptr;	/* save start position */

	    while (*iptr && isspace(*iptr))
		iptr++;		/* pass all whitespaces */

	    /* if this is an encoded word here, we should skip the passed
	       whitespaces. If it isn't an encoded-word, we should include the
	       whitespaces in the output. */

	    if (!strncmp(iptr, "=?", 2) &&
		(4 == sscanf(iptr + 2, "%128[^?]?%32[^?]?%128[^?]?%c",
			     charset, encoding, blurb, &equal)) &&
		('=' == equal)) {
		continue;	/* this IS an encoded-word, continue from here */
	    }
	    else
		/* this IS NOT an encoded-word, move back to the first whitespace */
		iptr = oldptr;
	}
	else
	    *output++ = *iptr++;
    }
    *output = 0;

    if (didanything) {
	/* this check prevents unneccessary strsav() calls if not needed */
	free(string);		/* free old memory */

#if DEBUG_PARSE
	/* debug display */
	printf("NEW: %s\n", storage);

	{
	    unsigned char *f;
	    puts("NEW:");
	    for (f = storage; f < output; f++) {
		if (isgraph(*f))
		    printf("%c", *f);
		else
		    printf("%02X", (unsigned char)*f);
	    }
	    puts("");
	}
#endif
	return storage;		/* return new */
    }
    else {
	free(storage);
	return string;
    }
}

/*
** Decode this [virtual] Quoted-Printable line as defined by RFC2045.
** Written by Daniel.Stenberg@haxx.nu
*/

static void mdecodeQP(FILE *file, char *input, char **result, int *length)
{
    int outcount = 0;
    char *buffer = input;
    unsigned char inchar;
    char *output;

    int len = strlen(input);
    output = strsav(input);

    while ((inchar = *input) != '\0') {

	if (outcount >= len - 1) {
	    /* we need to enlarge the destination area! */
	    /* double the size each time enlargement is needed */
	    char *newp = (char *)realloc(output, len * 2);
	    if (newp) {
		output = newp;
		len *= 2;
	    }
	    else
		break;
	}

	input++;
	if ('=' == inchar) {
	    int value;
	    if ('\n' == *input) {
		if (!fgets(buffer, MAXLINE, file))
		    break;
		input = buffer;
		continue;
	    }
	    else if ('=' == *input) {
		inchar = '=';
		input++;	/* pass this */
	    }
	    else if (isxdigit(*input)) {
		sscanf(input, "%02X", &value);
		inchar = (unsigned char)value;
		input += 2;	/* pass the two letters */
	    }
	    else
		inchar = '=';
	}
	output[outcount++] = inchar;
    }
    output[outcount] = 0;	/* zero terminate */

    *result = output;
    *length = outcount;
}

char *createlink(char *format, char *dir, char *file, int num, char *type)
{
    struct Push buff;
    char buffer[16];

    INIT_PUSH(buff);

    if (!format || !*format)
	/* nothing set, use internal default: */
	format = "%p";

    while (*format) {
	if ('%' == *format) {
	    format++;
	    switch (*format) {
	    default:
		PushByte(&buff, '%');
		PushByte(&buff, *format);
		break;
	    case '%':
		PushByte(&buff, '%');
		break;
	    case 'p':		/* the full path+file */
		PushString(&buff, dir);
		PushByte(&buff, '/');	/* this is for a HTML link and always uses
					   this path separator */
		if (file)
		    PushString(&buff, file);
		else
		    PushString(&buff, "<void>");
		break;
	    case 'f':		/* file name */
		PushString(&buff, file);
		break;
	    case 'd':		/* dir name */
		PushString(&buff, dir);
		break;
	    case 'n':		/* message number */
		sprintf(buffer, "%04d", num);
		PushString(&buff, buffer);
		break;
	    case 'c':		/* content-type (TODO: URL-encode this) */
		PushString(&buff, type);
		break;
	    }
	}
	else {
	    PushByte(&buff, *format);
	}
	format++;
    }

    RETURN_PUSH(buff);
}


void emptydir(char *directory)
{
    struct stat fileinfo;

    char *realdir = directory;

    if (!lstat(realdir, &fileinfo)) {
	if (S_ISDIR(fileinfo.st_mode)) {
	    /* It exists AND it is a dir */
	    DIR *dir = opendir(realdir);
	    char *filename;
	    if (dir) {
#ifdef HAVE_DIRENT_H
		struct dirent *entry;
#else
		struct direct *entry;
#endif
		while ((entry = readdir(dir))) {
		    if (!strcmp(".", entry->d_name) ||
			!strcmp("..", entry->d_name)) continue;
		    filename = maprintf("%s%c%s", realdir,
					PATH_SEPARATOR, entry->d_name);
		    fprintf(stderr, "\nWe delete %s\n", filename);
		    unlink(filename);
		    free(filename);
		}
		closedir(dir);
	    }
	}
    }
}

/*
** Parsing...the heart of Hypermail!
** This loads in the articles from stdin or a mailbox, adding the right
** field variables to the right structures. If readone is set, it will
** think anything it reads in is one article only. Increment should be set
** if this updates an archive.
*/

int parsemail(char *mbox,	/* file name */
	      int use_stdin,	/* read from stdin */
	      int readone,	/* only one mail */
	      int increment,	/* update an existing archive */
	      char *dir, int inlinehtml,	/* if HTML should be inlined */
	      int startnum)
{
    FILE *fp;
    char *date = NULL;
    char *subject = NULL;
    char *msgid = NULL;
    char *inreply = NULL;
    char *namep = NULL;
    char *emailp = NULL;
    char *line = NULL; 
    char line_buf[MAXLINE], fromdate[DATESTRLEN] = "";
    char *cp;
    char *dp = NULL;
    int num, isinheader, hassubject, hasdate;
    struct emailinfo *emp;
    char *att_dir = NULL;	/* directory name to store attachments in */
    char *meta_dir = NULL;	/* directory name where we're storing the meta data
				   that describes the attachments */
    typedef enum {
	NO_FILE,
	MAKE_FILE,
	MADE_FILE
    } FileStatus;		/* for attachments */

    /* -- variables for the multipart/alternative parser -- */
    struct body *origbp = NULL;	/* store the original bp */
    struct body *origlp = NULL;	/* ... and the original lp */
    char alternativeparser = FALSE;	/* set when inside alternative parser mode */
    int alternative_weight = -1;	/* the current weight of the prefered alternative content */
    struct body *alternative_lp = NULL;	/* the previous alternative lp */
    struct body *alternative_bp = NULL;	/* the previous alternative bp */
    FileStatus alternative_lastfile_created = NO_FILE;	/* previous alternative attachments, for non-inline MIME types */
    char alternative_file[129];	/* file name where we store the non-inline alternatives */
    char alternative_lastfile[129];	/* last file name where we store the non-inline alternatives */
    int att_counter = 0;	/* used to generate a unique name for attachments */

    /* -- end of alternative parser variables -- */

    struct body *bp;
    struct body *lp = NULL;	/* the last pointer, points to the last node in the
				   body list. Initially set to NULL since we have
				   none at the moment. */

    struct body *headp = NULL;	/* stored pointer to the point where we last
				   scanned the headers of this mail. */

    char Mime_B = FALSE;
    char boundbuffer[128] = "";

    struct boundary *boundp = NULL;	/* This variable is used to store a stack 
					   of boundary separators in cases with mimed 
					   mails inside mimed mails */

    char multilinenoend = FALSE;	/* This variable is set TRUE if we have read 
					   a partial line off a multiline-encoded line, 
					   and the next line we read is supposed to get
					   appended to the previous one */

    int bodyflags = 0;		/* This variable is set to extra flags that the 
				   addbody() calls should OR in the flag parameter */

    int binfile = -1;

    char *charset = NULL;	/* this is the LOCAL charset used in the mail */

    char *boundary = NULL;
    char type[129];		/* for Content-Type type */
    char charbuffer[129];	/* for Content-Type charset */
    FileStatus file_created = NO_FILE;	/* for attachments */

    char attachname[129];	/* for attachment file names */
    char inline_force = FALSE;	/* show a attachment in-line, regardles of
				   the content_disposition */
    char *description = NULL;	/* user-supplied description for an attachment */
    /* @@@ test for attachment */
    char attach_force;
    /* @@@ */

    EncodeType decode = ENCODE_NORMAL;
    ContentType content = CONTENT_TEXT;

    if (use_stdin || !mbox || !strcasecmp(mbox, "NONE"))
	fp = stdin;
    else if ((fp = fopen(mbox, "r")) == NULL) {
	sprintf(errmsg, "%s \"%s\".",
		lang[MSG_CANNOT_OPEN_MAIL_ARCHIVE], mbox);
	progerr(errmsg);
    }

    num = startnum;

    hassubject = 0;
    hasdate = 0;
    isinheader = 1;
    inreply = NULL;
    msgid = NULL;
    bp = NULL;
    subject = NOSUBJECT;

    if (!increment) {
	replylist = NULL;
	subjectlist = NULL;
	authorlist = NULL;
	datelist = NULL;
    }

    /* now what has this to do if readone is set or not? (Daniel) */
    if (set_showprogress) {
	if (readone)
	    printf("%s\n", lang[MSG_READING_NEW_HEADER]);
	else {
	    if ((mbox && !strcasecmp(mbox, "NONE")) || use_stdin)
		printf("%s...\n", lang[MSG_LOADING_MAILBOX]);
	    else
		printf("%s \"%s\"...\n", lang[MSG_LOADING_MAILBOX], mbox);
	}
    }

    while (fgets(line_buf, MAXLINE, fp) != NULL) {
#if DEBUG_PARSE
	printf("IN: %s", line);
#endif 
	line = line_buf + set_ietf_mbox; 
	if (isinheader) {
	    if (!strncasecmp(line_buf, "From ", 5))
		strcpymax(fromdate, dp = getfromdate(line), DATESTRLEN);
	    /* check for MIME */
	    else if (!strncasecmp(line, "MIME-Version:", 13))
		Mime_B = TRUE;
	    else if (isspace(line[0]) && ('\n' != line[0])) {
		/*
		 * since this begins with a whitespace, it means the 
		 * previous line is continued on this line, leave only 
		 * one space character and go! 
		 */
		char *ptr = line;
		while (isspace(*ptr))
		    ptr++;
		ptr--;		/* leave one space */
		*ptr = ' ';	/* make it a true space, no tabs here! */
		bp =
		    addbody(bp, &lp, ptr,
			    BODY_CONTINUE | BODY_HEADER | bodyflags);
	    }

	    else if (line[0] == '\n') {
		struct body *head;

		char savealternative;

		/* 
		 * we mark this as a header-line, and we use it to 
		 * track end-of-header displays 
		 */

		/* skip the alternate "\n", otherwise, we'll have
		   an extra "\n" in the HTMLized message */
		if (!alternativeparser)
		    bp = addbody(bp, &lp, line, BODY_HEADER | bodyflags);
		isinheader--;

		/*
		 * This signals us that we are no longer in the header, 
		 * let's fill in all those fields we are interested in. 
		 * Parse the headers up to now and copy to the target 
		 * variables 
		 */

		for (head = bp; head; head = head->next) {
		    if (head->header && !head->demimed) {
			head->line =
			    mdecodeRFC2047(head->line, strlen(head->line));
			head->demimed = TRUE;	/* don't do this again */
		    }

		    if (head->parsedheader || head->attached ||
			!head->header) {
			continue;
		    }

		    if (!strncasecmp(head->line, "Date:", 5)) {
			date = getmaildate(head->line);
			head->parsedheader = TRUE;
			hasdate = 1;
		    }
		    else if (!strncasecmp(head->line, "From:", 5)) {
			getname(head->line, &namep, &emailp);
			head->parsedheader = TRUE;
                        if(set_spamprotect) {
                          emailp=spamify(emailp);
                          /* we need to "fix" the name as well, as sometimes
                             the email ends up in the name part */
                          namep=spamify(namep);
                        }
		    }
		    else if (!strncasecmp(head->line, "Message-Id:", 11)) {
			msgid = getid(head->line);
			head->parsedheader = TRUE;
		    }
		    else if (!strncasecmp(head->line, "Subject:", 8)) {
			subject = getsubject(head->line);
			hassubject = 1;
			head->parsedheader = TRUE;
		    }
		    else if (!strncasecmp(head->line, "In-Reply-To:", 12)) {
			inreply = getreply(head->line);
			head->parsedheader = TRUE;
		    }
		    else if (!strncasecmp(head->line, "References:", 11)) {
			/*
			 * Adding threading capability for the "References" 
			 * header, ala RFC 822, used only for messages that 
			 * have "References" but do not have an "In-reply-to"
			 * field. This is partically a concession for Netscape's
			 * email composer, which erroneously uses "References"
			 * when it should use "In-reply-to". 
			 */
			if (!inreply)
			    inreply = getid(head->line);
		    }
		}

		if (!headp)
		    headp = bp;


		savealternative = FALSE;
		/*@@ */
		attach_force = FALSE;
		/*@@ */

		description = NULL;
		for (head = headp; head; head = head->next) {
		    if (head->parsedheader || !head->header)
			continue;
		    /* Content-Description is defined ... where?? */
		    if (!strncasecmp(head->line, "Content-Description:", 20)) {
			char *ptr = head->line;
			description = ptr + 21;
		    }
		    /* Content-Disposition is defined in RFC 2183 */
		    else
			if (!strncasecmp (head->line, "Content-Disposition:", 20)) {
			char *ptr = head->line + 20;
			char *fname;
			char *jp;
			char *np;

			while (*ptr && isspace(*ptr))
			    ptr++;
			if (!strncasecmp(ptr, "attachment;", 11)
			    && (content != CONTENT_IGNORE)) {
			    /* signal we want to attach, rather than embeed this MIME 
			       attachment */
			    attach_force = TRUE;

			    /* make sure it is binary */
			    content = CONTENT_BINARY;

			    /* see if there's a file name to use: */
			    fname = strcasestr(ptr, "filename=");
			    if (fname) {
                                np = fname+9;
                                if (*np == '"')
                                     np++;
                                for (jp = attachname; np && *np != '\n' && *np != '"';) {
                                     *jp++ = *np++;
                                }
                                *jp = '\0';
				safe_filename(attachname);
			    }
			    else {
				attachname[0] = '\0';	/* just clear it */
			    }
			    file_created = MAKE_FILE;	/* please make one */
			}
#if 0
/*
** Why was this limited to just type image ? There are more inline types than just image.
** I removed the image restriction and all of a sudden more attachments had the proper name.
*/

			else if (!strncasecmp(ptr, "inline;", 7)
				 && (content != CONTENT_IGNORE)
				 && (!strncasecmp(type, "image/", 5))) {	
                          /* @@@ <-- here I should use the inline thingy */
#endif
			else if (!strncasecmp(ptr, "inline;", 7)
				 && (content != CONTENT_IGNORE)
				 && inlinecontent(type)) {
			    inline_force = TRUE;
			    /* make sure it is binary */
			    content = CONTENT_BINARY;
			    /* see if there's a file name to use: */
			    fname = strcasestr(ptr, "filename=");
			    if (fname) {
                                np = fname+9;
                                if (*np == '"')
                                     np++;
                                for (jp = attachname; np && *np != '\n' && *np != '"';) {
                                     *jp++ = *np++;
                                }
                                *jp = '\0';
				safe_filename(attachname);
			    }
			    else {
				attachname[0] = '\0';	/* just clear it */
			    }
			    file_created = MAKE_FILE;	/* please make one */
			}
		    }

		    else if (!strncasecmp(head->line, "Content-Type:", 13)) {
			char *ptr = head->line + 13;
#define DISP_HREF 1
#define DISP_IMG  2
#define DISP_IGNORE 3
			/* we must make sure this is not parsed more times
			   than this */
			head->parsedheader = TRUE;

			while (isspace(*ptr))
			    ptr++;

			sscanf(ptr, "%128[^;]", type);
			if ((cp = strchr(type, '\n')) != NULL)
			    *cp = '\0';	/* rm newlines */

			/* now, check if there's a charset indicator here too! */
			cp = strcasestr(ptr, "charset=");
			if (cp) {
			    cp += 8;	/* pass charset= */
			    if ('\"' == *cp)
				cp++;	/* pass a quote too if one is there */

			    sscanf(cp, "%128[^;\"\n]", charbuffer);
			    if (strcasecmp("ISO-8859-1", charbuffer) &&
				strcasecmp("US-ASCII", charbuffer))
				/* If this isn't any of the charsets above,
				   then save the charset info! */
				charset = strsav(charbuffer);
			}

			if (alternativeparser) {
			    struct body *next, *temp_bp;

			    /* We are parsing alternatives... */

			    if (preferedcontent(&alternative_weight, type)) {
				/* ... this is a prefered type, we want to store
				   this [instead of the earlier one]. */
				/* erase the previous alternative info */
				temp_bp = alternative_bp;	/* remember the value of bp for GC */
				alternative_bp = alternative_lp = NULL;
				alternative_lastfile_created = NO_FILE;
				content = CONTENT_UNKNOWN;
				if (alternative_lastfile[0] != '\0') {
				    /* remove the previous attachment */
				    unlink(alternative_lastfile);
				    alternative_lastfile[0] = '\0';
				}
			    }
			    else {
				/* ...and this type is not a prefered one. Thus, we
				 * shall ignore it completely! */
				content = CONTENT_IGNORE;
				/* erase the current alternative info */
				temp_bp = bp;	/* remember the value of bp for GC */
				lp = alternative_lp;
				bp = alternative_bp;
				strcpy(alternative_file,
				       alternative_lastfile);
				file_created =
				    alternative_lastfile_created;
				alternative_bp = alternative_lp = NULL;
				alternative_lastfile_created = NO_FILE;
				alternative_lastfile[0] = '\0';
				/* we haven't yet created any attachment file, so there's no need
				   to erase it yet */
			    }
			    /* free any previous alternative */
			    while (temp_bp) {
				next = temp_bp->next;
				if (temp_bp->line)
				    free(temp_bp->line);
				free(temp_bp);
				temp_bp = next;
			    }
			    /* @@ not sure if I should add a diff flag to do this break */
			    if (content == CONTENT_IGNORE)
				/* end the header parsing... we already know what we want */
				break;
			}

			if (content == CONTENT_IGNORE)
			    continue;
			else if (ignorecontent(type))
			    /* don't save this */
			    content = CONTENT_IGNORE;
			else if (textcontent(type)
				 || (inlinehtml &&
				     !strcasecmp(type, "text/html"))) {
			    /* text content or text/html follows.
			     */

			    if (!strcasecmp(type, "text/html"))
				content = CONTENT_HTML;
			    else
				content = CONTENT_TEXT;
			    continue;
			}
			else if (!strncasecmp(type, "message/rfc822", 14)) {
			    /* 
			     * Here comes an attached mail! This can be ugly, 
			     * since the attached mail may very well itself 
			     * contain attached binaries, or why not another 
			     * attached mail? :-)
			     *
			     * We need to store the current boundary separator 
			     * in order to get it back when we're done parsing 
			     * this particular mail, since each attached mail 
			     * will have its own boundary separator that *might*
			     * be used.
			     */
			    bp = addbody(bp, &lp,
					 "<p><strong>attached mail follows:</strong><hr noshade>",
					 BODY_HTMLIZED | bodyflags);
			    bodyflags |= BODY_ATTACHED;
			    /* @@ should it be 1 or 2 ?? should we use another method? */
#if 0
			    isinheader = 2;
#endif
			    isinheader = 1;
			    continue;
			}
			else if (strncasecmp(type, "multipart/", 10)) {
			    /* 
			     * This is not a multipart and not text 
			     */
			    char *fname = NULL;	/* attachment filename */

			    /* 
                             * only do anything here if we're not 
                             * ignoring this content 
                             */
			    if (CONTENT_IGNORE != content) {

				fname = strcasestr(ptr, "name=");
				if (fname) {
				    fname += 5;
				    if ('\"' == *fname)
					fname++;
				    sscanf(fname, "%128[^\"]", attachname);
				    safe_filename(attachname);
				}
				else {
				    attachname[0] = '\0';	/* just clear it */
				}

				file_created = MAKE_FILE;	/* please make one */

				content = CONTENT_BINARY;	/* uknown turns into binary */
			    }
			    continue;
			}
			else {
			    /*
			     * Find the first boundary separator 
			     */

			    boundary = strcasestr(ptr, "boundary=");

			    if (boundary) {
				boundary = strchr(boundary, '=');
				if (boundary) {
				    boundary++;
				    while (isspace(*boundary))
					boundary++;
				    if ('\"' == *boundary) {
					sscanf(++boundary, "%[^\"]",
					       boundbuffer);
				    }
				    else
					sscanf(boundary, "%[^;\n]",
					       boundbuffer);
				    boundary = boundbuffer;
				}

				while (fgets(line_buf, MAXLINE, fp)) {
				    if (!strncmp(line_buf + set_ietf_mbox, "--", 2) &&
					!strncmp(line_buf + set_ietf_mbox + 2, boundbuffer,
						 strlen(boundbuffer))) {
					break;
				    }
				}

				/* 
				 * This stores the boundary string in a stack 
				 * of strings: 
				 */
				boundp = bound(boundp, boundbuffer);

				/* printf("set new boundary: %s\n", boundp->line); */

				/*
				 * We set ourselves, "back in header" since there is
				 * gonna come MIME headers now after the separator
				 */
				isinheader = 1;

				/* Daniel Stenberg started adding the
				 * "multipart/alternative" parser 13th of July
				 * 1998!  We check if this is a 'multipart/
				 * alternative' header, in which case we need to
				 * treat it very special.  
				 */

				if (!strncasecmp
				    (&ptr[10], "alternative", 11)) {
				    /* It *is* an alternative session!  Alternative
				     * means there will be X parts with the same text
				     * using different content-types. We are supposed
				     * to take the most prefered format of the ones
				     * used and only output that one. MIME defines
				     * the order of the texts to start with pure text
				     * and then continue with more and more obscure
				     * formats. (well, it doesn't use those terms but
				     * that's what it means! ;-)) 
				     */

				    /* How "we" are gonna deal with them:
				     *
				     * We create a "spare" linked list body for the
				     * very first part. Since the first part is
				     * defined to be the most readable, we save that
				     * in case no content-type present is prefered!
				     *
				     * We skip all parts that are not prefered. All
				     * prefered parts found will replace the first
				     * one that is saved. When we reach the end of
				     * the alternatives, we will use the last saved
				     * one as prefered.
				     */

				    savealternative = TRUE;
#if DEBUG_PARSE
				    printf("SAVEALTERNATIVE: yes\n");
#endif

				}

			    }
			    else
				boundary = NULL;
			}
		    }
		    else
			if (!strncasecmp
			    (head->line, "Content-Transfer-Encoding:", 26)) {
			char *ptr = head->line + 26;

			head->parsedheader = TRUE;
			while (isspace(*ptr))
			    ptr++;
			if (!strncasecmp(ptr, "QUOTED-PRINTABLE", 16)) {
			    decode = ENCODE_QP;
			}
			else if (!strncasecmp(ptr, "BASE64", 6)) {
			    decode = ENCODE_BASE64;
			}
			else if (!strncasecmp(ptr, "8BIT", 4)) {
			    decode = ENCODE_NORMAL;
			}
			else if (!strncasecmp(ptr, "7BIT", 4)) {
			    decode = ENCODE_NORMAL;
			}
			else if (!strncasecmp(ptr, "x-uue", 5)) {
			    decode = ENCODE_UUENCODE;

			    if (uudecode(fp, line, line, NULL, TRUE))
				/*
				 * oh gee, we failed this is chaos 
                                 */
				break;
			}
			else {
			    /* Unknown format, we use default decoding */
			    char code[64];

			    sscanf(ptr, "%63s", code);
			    sprintf(line, " ('%s' %s)\n",
				    code,
				    lang[MSG_ENCODING_IS_NOT_SUPPORTED]);

			    bp =
				addbody(bp, &lp, line,
					BODY_HTMLIZED | bodyflags);
			}
#if DEBUG_PARSE
			printf("DECODE set to %d\n", decode);
#endif
		    }
		}

		/* @@@ here we try to do a post parsing cleanup */
		/* have to find out all the conditions to turn it off */
		if (attach_force) {
		    savealternative = FALSE;
		    isinheader = 0;
		}

		if (savealternative) {
		    /* let's remember 'bp' and 'lp' */

		    origbp = bp;
		    origlp = lp;

		    alternativeparser = TRUE;
		    /* restart on a new list: */
		    lp = bp = NULL;
		    /* clean the alternative status variables */
		    alternative_weight = -1;
		    alternative_lp = alternative_bp = NULL;
		    alternative_lastfile_created = NO_FILE;
		    alternative_file[0] = alternative_lastfile[0] = '\0';
		}
		headp = lp;	/* start at this point next time */
	    }
	    else {
		bp = addbody(bp, &lp, line, BODY_HEADER | bodyflags);
	    }
	}
	else {
	    /* If this isn't a single mail: see if the line is a message
	     * separator. If there is a "^From " found, check to see if there
	     * is a valid date field in the line. If not then consider it a
	     * part of the body of the message and skip it.
	     * Daniel: I don't like this. I don't think there is something like
	     * "a valid date field" in that line 100%.
	     */
	    if (!readone &&
		!strncmp(line_buf, "From ", 5) &&
		(*(dp = getfromdate(line)) != '\0')) {
		if (-1 != binfile) {
		    close(binfile);
		    binfile = -1;
		}
		isinheader = 1;
		if (!hassubject)
		    subject = NOSUBJECT;
		else
		    hassubject = 1;

		if (!hasdate)
		    date = NODATE;
		else
		    hasdate = 1;

		if (!inreply)
		    inreply = oneunre(subject);

		while (rmlastlines(bp));

		emp =
		    addhash(num, date, namep, emailp, msgid, subject,
			    inreply, fromdate, charset, NULL, NULL, bp);
                /* 
                 * dp, if it has a value, has a date from the "From " line of
                 * the message after the one we are just finishing. 
                 * SMR 19 Oct 99: moved this *after* the addhash() call so it
                 * isn't erroneously associate with the previous message 
                 */
   
                strcpymax(fromdate, dp ? dp : "", DATESTRLEN);

		if (emp) {
		    /* only add actual mails */
		    authorlist = addheader(authorlist, emp, 1);

		    emp->unre_subject = unre(subject);
		    subjectlist = addheader(subjectlist, emp, 0);

		    datelist = addheader(datelist, emp, 2);

		    num++;
		}

		if (hasdate)
		    free(date);
		if (hassubject)
		    free(subject);
		if (inreply) {
		    free(inreply);
		    inreply = NULL;
		}
		if (charset) {
		    free(charset);
		    charset = NULL;
		}
		if (msgid) {
		    free(msgid);
		    msgid = NULL;
		}
		if (namep) {
		    free(namep);
		    namep = NULL;
		}
		if (emailp) {
		    free(emailp);
		    emailp = NULL;
		}

		bp = NULL;
		bodyflags = 0;	/* reset state flags */

		/* go back to default mode: */
		content = CONTENT_TEXT;
		decode = ENCODE_NORMAL;
		Mime_B = FALSE;
		headp = NULL;
		multilinenoend = FALSE;
		if (att_dir) {
		    free(att_dir);
		    att_dir = NULL;
		}
		if (set_usemeta && meta_dir) {
		    free(meta_dir);
		    meta_dir = NULL;
		}
		att_counter = 0;
		inline_force = FALSE;
		attachname[0] = '\0';

		/* by default we have none! */
		hassubject = 0;
		hasdate = 0;

                alternativeparser = FALSE; /* there is none anymore */

		if (!(num % 10) && set_showprogress && !readone) {
		    print_progress(num - startnum, NULL, NULL);
		}
#if DEBUG_PARSE
		printf("LAST: %s", line);
#endif
	    }
	    else {		/* decode MIME complient gibberish */
		char newbuffer[MAXLINE];
		char *data;
		int datalen = -1;	/* -1 means use strlen to get length */

		if (Mime_B) {
		    if (boundp &&
			!strncmp(line, "--", 2) &&
			!strncmp(line + 2, boundp->line,
				 strlen(boundp->line))) {
			/* right at this point, we have another part coming up */
#if DEBUG_PARSE
			printf("hit %s\n", line);
#endif
			if (!strncmp(line + 2 + strlen(boundp->line), "--", 2)) {
			    /* @@@ don't know why we had this line here. Doesn't hurt to take
			       it out, though */
#if 0
			    bp = addbody(bp, &lp, "\n",
					BODY_HTMLIZED | bodyflags);
#endif
			    isinheader = 0;	/* no header, the ending boundary
						   can't have any describing
						   headers */

#if DEBUG_PARSE
			    printf("End boundary %s\n", line);
#endif
			    boundp = bound(boundp, NULL);
			    if (!boundp) {
				bodyflags &= ~BODY_ATTACHED;
			    }
			    if (alternativeparser) {
				struct body *next;
				/* we no longer have alternatives */
				alternativeparser = FALSE;
				/* reset the alternative variables (I think we can skip
				   this step without problems */
				alternative_weight = -1;
				alternative_bp = NULL;
				alternative_lastfile_created = NO_FILE;
				alternative_file[0] =
				    alternative_lastfile[0] = '\0';
#if DEBUG_PARSE
				printf("We DUMP the chosen alternative\n");
#endif
				while (bp) {
				    origbp = addbody(origbp, &origlp,
						     bp->line,
						     (bp->
						      header ? BODY_HEADER
						      : 0) | (bp->
							      html ?
							      BODY_HTMLIZED
							      : 0) | (bp->
								      attached
								      ?
								      BODY_ATTACHED
								      : 0)
					);
				    next = bp->next;
				    free(bp->line);
				    free(bp);
				    bp = next;
				}
				bp = origbp;
				lp = origlp;

				headp = NULL;
			    }
#if DEBUG_PARSE
			    if (boundp)
				printf("back %s\n", boundp->line);
			    else
				printf("back to NONE\n");
#endif
			}
			else {
			    if (alternativeparser) {
				/*
				 * parsing another alternative, so we save the
				 * precedent values 
				 */
				alternative_bp = bp;
				alternative_lp = lp;
				alternative_lastfile_created =
				    file_created;
				strcpy(alternative_lastfile,
				       alternative_file);
				/* and now reset them */
				headp = bp = lp = NULL;
				alternative_file[0] = '\0';
			    }
			    else
				att_counter++;
			    isinheader = 1;	/* back on a kind-of-header */
			    /* @@@ why are we changing the status of this variable? */
			    file_created = NO_FILE;	/* not created any file yet */
			}
			/* go back to the MIME attachment default mode */
			content = CONTENT_TEXT;
			decode = ENCODE_NORMAL;
			multilinenoend = FALSE;

			if (-1 != binfile) {
			    close(binfile);
			    binfile = -1;
			}
			continue;
		    }
		}

		switch (decode) {
		case ENCODE_QP:
		    mdecodeQP(fp, line, &data, &datalen);
		    break;
		case ENCODE_BASE64:
		    base64Decode(line, newbuffer, &datalen);
		    data = newbuffer;
		    break;
		case ENCODE_UUENCODE:
		    uudecode(NULL, line, newbuffer, &datalen, FALSE);
		    data = newbuffer;
		    break;
		case ENCODE_NORMAL:
		    data = line;
		    break;
		default:
		    /* we have no clue! */
		    data = NULL;
		    break;
		}
#if DEBUG_PARSE
		printf("LINE %s\n", data);
#endif
		if (data) {
		    if ((content == CONTENT_TEXT) ||
			(content == CONTENT_HTML)) {
			if (decode > ENCODE_MULTILINED) {
			    /* 
			     * This can be more than one resulting line, 
			     * as the decoded the string may look like:
			     * "#!/bin/sh\r\n\r\nhelp() {\r\n echo 'Usage: difftree"
			     */
			    char *p = data;
			    char *n;
			    char store;

#if DEBUG_PARSE
			    printf("decode type %d\n", decode);
#endif

			    while ((n = strchr(p, '\n'))) {
				store = n[1];
				n[1] = 0;
#if DEBUG_PARSE
				printf("UNFOLDED %s", p);
#endif
				bp = addbody(bp, &lp, p,
					     (content == CONTENT_HTML ?
					      BODY_HTMLIZED : 0) |
					     (multilinenoend ?
					      BODY_CONTINUE : 0) |
					     bodyflags);
				multilinenoend = FALSE;	/* full line pushed */
				n[1] = store;
				p = n + 1;
			    }
			    if (strlen(p)) {
				/* 
				 * This line doesn't really end here, 
				 * we will get another line soon that 
				 * should get appended! 
				 */
#if DEBUG_PARSE
				printf("CONTINUE %s\n", p);
#endif
				bp = addbody(bp, &lp, p,
					     (content == CONTENT_HTML ?
					      BODY_HTMLIZED : 0) |
					     (multilinenoend ?
					      BODY_CONTINUE : 0) |
					     bodyflags);

				/*
				 * We want the next line to get appended to this!
				 */
				multilinenoend = TRUE;
			    }
			}
			else {
			    bp = addbody(bp, &lp, data,
					 (content == CONTENT_HTML ?
					  BODY_HTMLIZED : 0) | bodyflags);
			}
#if DEBUG_PARSE
			printf("ALIVE?\n");
#endif
		    }
		    else if (content == CONTENT_BINARY) {

#ifndef REMOVED_990310
			/* If there is no file created, we create and init one */
			if (file_created == MAKE_FILE) {
			    char *fname;
			    char *binname;
			    char *file = NULL;
			    char buffer[512];

			    file_created = MADE_FILE;	/* we have, or at least we tried */

			    /* create the attachment directory if it doesn't exist */
			    if (att_dir == NULL) {

				/* first check the DIR_PREFIXER */
				att_dir = maprintf("%s%c" DIR_PREFIXER "%04d",
					     dir, PATH_SEPARATOR, num);
				check1dir(att_dir);
				/* If this is a repeated run on the same archive we already
				 * have HTML'ized, we risk extracting the same attachments
				 * several times and therefore we need to remove all the 
				 * attachments currently present before we go ahead!
				 *(Daniel -- August 6, 1999) */
				/* jk: removed it for a while, as it's not so necessary
				   once we can generate the same file names */
#if DEBUG_PARSE
				emptydir(att_dir);
#endif
				if (set_usemeta) {
				    /* make the meta dir where we'll store the meta info,
				       such as content-type */
				    meta_dir = maprintf("%s%c" META_DIR, att_dir,
						 PATH_SEPARATOR);
				    check1dir(meta_dir);
				}
			    }

			    /* If the attachment has a name, we keep it and add the
			       current value of the counter, to guarantee that we
			       have a unique name. Otherwise, we use a fixed name +
			       the counter. We go thru all this trouble so that we
			       can easily regenerate the same archive, without breaking
			       any links */

			    if (att_counter > 99)
				binname = NULL;
			    else {
				if (attachname[0])
				    fname = attachname;
				else
				    fname = FILE_SUFFIXER;

				binname = maprintf("%s%c%.2d-%s",
						   att_dir, PATH_SEPARATOR,
						   att_counter, fname);
				/* @@ move this one up */
				/* att_counter++; */
			    }

			    /* 
                             * Saving of the attachments is being done 
                             * inline as they are encountered. The 
                             * directories must exist first...  
                             */

			    if (binname) {
				binfile = open(binname, O_WRONLY | O_CREAT,
					       set_filemode);

				if (-1 != binfile) {
				    chmod(binname, set_filemode);
				    if (set_showprogress)
					print_progress(num, lang
					       [MSG_CREATED_ATTACHMENT_FILE],
					       binname);
				    if (set_usemeta) {
					/* write the mime meta info */
					FILE *file_ptr;
					char *meta_file;
					char *ptr;

					ptr = strrchr(binname, PATH_SEPARATOR);
					*ptr = '\0';
					meta_file =
					    maprintf("%s%c%s"
						     META_EXTENSION,
						     meta_dir,
						     PATH_SEPARATOR,
						     ptr + 1);
					*ptr = PATH_SEPARATOR;
					file_ptr = fopen(meta_file, "w");
					if (file_ptr) {
					    if (type) {
						if (charset)
						    fprintf(file_ptr,
							    "Content-Type: %s; charset=\"%s\"\n",
							    type, charset);
						else
						    fprintf(file_ptr,
							    "Content-Type: %s\n",
							    type);
					    }
					    fclose(file_ptr);
					    chmod(meta_file, set_filemode);
					    free(meta_file);
					}
				    }
				    if (alternativeparser)
					/* save the last name, in case we need to supress it */
					strncpy(alternative_file, binname,
						sizeof(alternative_file) -
						1);

				}
				else {
				    if (alternativeparser)
					/* save the last name, in case we need to supress it */
					alternative_file[0] = '\0';
				}

				/* point to the filename and skip the separator */
				file = &binname[strlen(att_dir) + 1];

				/* protection against having a filename bigger than buffer */
				if (strlen(file) <= 500) {
				    char *desc;
				    char *sp;

				    if (description && description[0] != '\0'
                                        && hasblack(description))
                                            desc = description;
				    else if (inline_force ||
					     inlinecontent(type)) desc =
					    attachname[0] ? attachname :
					    "picture";
				    else
					desc =
					    attachname[0] ? attachname :
					    "stored";

				    if (description)
					description = NULL;

				    if (inline_force ||
					inlinecontent(type)) {
					/* if we know our browsers can show this type of context
					   as-is, we make a <img> tag instead of <a href>! */

					snprintf(buffer, sizeof(buffer),
						 "%s<img src=\"%s%c%s\" alt=\"%s\">\n",
						 (set_showhr ? "<hr noshade>\n" :
						  ""),
						 &att_dir[strlen(dir) + 1],
						 PATH_SEPARATOR, file,
						 desc);
				    }
				    else {
					char *created_link =
					    createlink(set_attachmentlink,
						       &att_dir[strlen(dir)
								+ 1],
						       file, num, type);

					if ((sp = strchr(desc, '\n')) !=
					    NULL) *sp = '\0';

					snprintf(buffer, sizeof(buffer),
						 "%s<ul>\n<li>%s %s: <a href=\"%s\">%s</a>\n</ul>\n",
						 (set_showhr ? "<hr noshade>\n" :
						  ""), type,
						 lang[MSG_ATTACHMENT],
						 created_link, desc);

					free(created_link);
				    }

				    /* Print attachment comment before attachment */
				    bp =
					addbody(bp, &lp, buffer,
						BODY_HTMLIZED | bodyflags);
				    snprintf(buffer, sizeof(buffer),
					     "<!-- attachment=\"%.80s\" -->\n",
					     file);
				    bp =
					addbody(bp, &lp, buffer,
						BODY_HTMLIZED | bodyflags);
				}
			    }

			    inline_force = FALSE;
			    attachname[0] = '\0';

			    if (binname && (binfile != -1))
				content = CONTENT_BINARY;
			    else
				content = CONTENT_UNKNOWN;

			    if (binname)
				free(binname);
			}
		    }
#endif
		    if (-1 != binfile) {
			if (datalen < 0)
			    datalen = strlen(data);

			write(binfile, data, datalen);
		    }
		}

		if (ENCODE_QP == decode)
		    free(data);	/* this was allocatd by mdecodeQP() */
	    }
	}
    }


    if (!isinheader || readone) {
	if (!hassubject)
	    subject = NOSUBJECT;

	if (!hasdate)
	    date = NODATE;

	if (!inreply)
	    inreply = oneunre(subject);

	while (rmlastlines(bp));

	strcpymax(fromdate, dp ? dp : "", DATESTRLEN);
	emp = addhash(num, date, namep, emailp, msgid, subject, inreply,
		      fromdate, charset, NULL, NULL, bp);
	if (emp) {
	    authorlist = addheader(authorlist, emp, 1);

	    emp->unre_subject = unre(subject);
	    subjectlist = addheader(subjectlist, emp, 0);

	    datelist = addheader(datelist, emp, 2);
	    num++;
	}

	/* @@@ if we didn't add the message, we should consider erasing the attdir
	   if it's there */

	if (hasdate)
	    free(date);
	if (hassubject)
	    free(subject);
	if (inreply) {
	    free(inreply);
	    inreply = NULL;
	}
	if (charset) {
	    free(charset);
	    charset = NULL;
	}
	if (msgid) {
	    free(msgid);
	    msgid = NULL;
	}
	if (namep) {
	    free(namep);
	    namep = NULL;
	}
	if (emailp) {
	    free(emailp);
	    emailp = NULL;
	}

	/* reset the status counters */
	/* @@ verify we're doing it everywhere */
	bodyflags = 0;		/* reset state flags */

	/* go back to default mode: */
	content = CONTENT_TEXT;
	decode = ENCODE_NORMAL;
	Mime_B = FALSE;
	headp = NULL;
	multilinenoend = FALSE;
	if (att_dir) {
	    free(att_dir);
	    att_dir = NULL;
	}
	if (set_usemeta && meta_dir) {
	    free(meta_dir);
	    meta_dir = NULL;
	}
	att_counter = 0;
	description = NULL;

	/* by default we have none! */
	hassubject = 0;
	hasdate = 0;
    }

    if (set_showprogress && !readone)
	print_progress(num, lang[MSG_ARTICLES], NULL);
#if DEBUG_PARSE
    printf("\b\b\b\b%4d %s.\n", num, lang[MSG_ARTICLES]);
#endif

    /* kpm - this is to prevent the closing of std and hypermail crashing
     * if the input is from stdin
     */
    if (fp != stdin)
	fclose(fp);

    crossindex();
    threadlist = NULL;
    printedthreadlist = NULL;
    crossindexthread1(datelist);
#if DEBUG_THREAD
    {
	struct reply *r;
	r = threadlist;
	fprintf(stderr, "START of threadlist after crossindexthread1\n");
	fprintf(stderr, "- msgnum frommsgnum maybereply msgid\n");
	while (r != NULL) {
	    if (r->data == NULL) {
		fprintf(stderr, "- XX %d %d XX\n",
			r->frommsgnum, r->maybereply);
	    }
	    else {
		fprintf(stderr, "- %d %d %d '%s'\n",
			r->data->msgnum,
			r->frommsgnum, r->maybereply, r->data->msgid);
	    }
	    r = r->next;
	}
	fprintf(stderr, "END of threadlist after crossindexthread1\n");
    }
#endif

    /* can we clean up a bit please... */

    if (boundp != NULL) {
	if (boundp->line)
	    free(boundp->line);
	free(boundp);
    }

    return num;			/* amount of mails read */
}

/*
** All this does is get all the relevant header information from the
** comment fields in existing archive files. Everything is loaded into
** structures in the exact same way as if articles were being read from
** stdin or a mailbox.
**
** Return the number of mails read.
*/

int loadoldheaders(char *dir)
{
    FILE *fp;
    char line[MAXLINE];
    char *name = NULL;
    char *email = NULL;
    char *date = NULL;
    char *msgid = NULL;
    char *subject = NULL;
    char *inreply = NULL;
    char *fromdate = NULL;
    char *charset = NULL;
    char *isodate = NULL;
    char *isofromdate = NULL;
    char *filename;
    char command[100];
    int num = 0;

    struct body *bp = NULL;
    struct body *lp = NULL;

    filename = maprintf("%s%s%.4d.%s", dir,
			(dir[strlen(dir) - 1] == '/') ? "" : "/",
			num, set_htmlsuffix);

    bp = addbody(bp, &lp, "\0", 0);

    authorlist = subjectlist = datelist = NULL;

    if (set_showprogress)
	printf("%s...\n", lang[MSG_READING_OLD_HEADERS]);

    /*
     * fromdate == <!-- received="Wed Jun  3 10:12:00 1998 CDT" -->
     * date     == <!-- sent="Wed, 3 Jun 1998 10:12:07 -0500 (CDT)" -->
     * name     == <!-- name="Kent Landfield" -->
     * email    == <!-- email="kent@landfield.com" -->
     * subject  == <!-- subject="Test of the testmail mail address." -->
     * msgid    == <!-- id="199806031512.KAA22323@landfield.com" -->
     * inreply  == <!-- inreplyto="" -->
     *
     * New for 2b10:
     * charset  == <!-- charset="iso-8859-2" -->
     *
     * New for 2b18:
     * isofromdate == <!-- isoreceived="19980603101200" -->
     * isodate     == <!-- isosent="19980603101207" -->
     */

    while ((fp = fopen(filename, "r")) != NULL) {
	char *valp;
	char legal = FALSE;

	while (fgets(line, sizeof(line), fp)) {

	    if (1 == sscanf(line, "<!-- %99[^=]=", command)) {

		if (!strcasecmp(command, "received"))
		    fromdate = getvalue(line);
		else if (!strcasecmp(command, "sent"))
		    date = getvalue(line);
		else if (!strcasecmp(command, "name"))
		    name = getvalue(line);
		else if (!strcasecmp(command, "email"))
		    email = getvalue(line);
		else if (!strcasecmp(command, "subject")) {
		    valp = getvalue(line);
		    {
			subject = unconvchars(valp);
			free(valp);
		    }
		}
		else if (!strcasecmp(command, "id"))
		    msgid = getvalue(line);
		else if (!strcasecmp(command, "charset"))
		    charset = getvalue(line);
		else if (!strcasecmp(command, "isosent"))
		    isodate = getvalue(line);
		else if (!strcasecmp(command, "isoreceived"))
		    isofromdate = getvalue(line);
		else if (!strcasecmp(command, "inreplyto")) {
		    valp = getvalue(line);
		    if (valp) {
			inreply = unconvchars(valp);
			free(valp);
		    }
		}
		else if (!strcasecmp(command, "body")) {
		    /*
		     * When we reach the mail body, we know we've got all the headers
		     * there were!
		     */
		    fclose(fp);
		    legal = TRUE;	/* with a body tag we consider this a valid syntax */
		    break;
		}
	    }
	}

	if (legal) {
	    struct emailinfo *emp;
	    /* only do this if the input was reliable */
	    emp = addhash(num, date, name, email, msgid, subject, inreply,
			  fromdate, charset, isodate, isofromdate, bp);
	    if (emp != NULL) {
		authorlist = addheader(authorlist, emp, 1);
		emp->unre_subject = unre(subject);
		subjectlist = addheader(subjectlist, emp, 0);
		datelist = addheader(datelist, emp, 2);
	    }
	}
	if (charset) {
	    free(charset);
	    charset = NULL;
	}
	if (name) {
	    free(name);
	    name = NULL;
	}
	if (subject) {
	    free(subject);
	    subject = NULL;
	}
	if (msgid) {
	    free(msgid);
	    msgid = NULL;
	}
	if (inreply) {
	    free(inreply);
	    inreply = NULL;
	}
	if (fromdate) {
	    free(fromdate);
	    fromdate = NULL;
	}
	if (date) {
	    free(date);
	    date = NULL;
	}
	if (email) {
	    free(email);
	    email = NULL;
	}
	if (isodate) {
	    free(isodate);
	    isodate = NULL;
	}
	if (isofromdate) {
	    free(isofromdate);
	    isofromdate = NULL;
	}

	num++;
	if (!(num % 10) && set_showprogress) {
	    printf("\r%4d", num);
	    fflush(stdout);
	}

	free(filename);
	filename = maprintf("%s%s%.4d.%s", dir,
			    (dir[strlen(dir) - 1] == '/') ? "" : "/",
			    num, set_htmlsuffix);
    }
    free(filename);

    if (set_showprogress)
	printf("\b\b\b\b%4d %s.\n", num, lang[MSG_ARTICLES]);


    /* can we clean up a bit please... */
    if (bp != NULL) {
	if (bp->line)
	    free(bp->line);
	free(bp);
    }

    return num;
}

/*
** Adds a "Next:" link in the proper article, after the archive has been
** incrementally updated.
*/

void fixnextheader(char *dir, int num)
{
    char filename[256];
    char line[MAXLINE];
    struct emailinfo *email;

    struct body *bp, *cp, *dp = NULL, *status, *lp = NULL;
    int ul;
    FILE *fp;
    char *ptr;

    dp = NULL;
    ul = 0;

    msnprintf(filename, sizeof(filename), "%s%s%.4d.%s", dir,
	      (dir[strlen(dir) - 1] == '/') ? "" : "/", num,
	      set_htmlsuffix);

    bp = NULL;
    fp = fopen(filename, "r");
    if (fp) {
	while ((fgets(line, MAXLINE, fp)) != NULL)
	    bp = addbody(bp, &lp, line, 0);
    }
    else
	return;
    fclose(fp);

    cp = bp;			/* save start of list to free later */

    fp = fopen(filename, "w+");
    if (fp) {
	while (bp) {
	    fprintf(fp, "%s", bp->line);
	    if (!strncmp(bp->line, "<!-- next=", 10)) {
		status = hashnumlookup(num + 1, &email);
		if (status != NULL) {
		    if (set_usetable) {
			dp = bp->next;
			if (!strncmp(dp->line, "<ul>", 4)) {
			    fprintf(fp, "%s", dp->line);
			    ul = 1;
			}
		    }
		    fprintf(fp, "<li><strong>%s:</strong> ",
			    lang[MSG_NEXT_MESSAGE]);
		    fprintf(fp, "<a href=\"%.4d.%s\">%s: \"%s\"</a>\n",
			    num + 1, set_htmlsuffix,
			    email->name, ptr = convchars(email->subject));
		    free(ptr);

		    if (ul) {
			bp = dp;
			ul = 0;
		    }
		}
	    }
	    bp = bp->next;
	}
    }

    fclose(fp);

    /* can we clean up a bit please... */
    bp = cp;
    while (bp != NULL) {
	cp = bp->next;
	if (bp->line)
	    free(bp->line);
	free(bp);
	bp = cp;
    }
}

/*
** Adds a "Reply:" link in the proper article, after the archive has been
** incrementally updated.
*/

void fixreplyheader(char *dir, int num)
{
    char filename[256];
    char line[MAXLINE];

    int subjmatch;
    int replynum = 0;

    struct body *bp, *cp, *status;
    struct body *lp = NULL;
    FILE *fp;
    char *ptr;

    struct emailinfo *email;
    struct emailinfo *email2;

    status = hashnumlookup(num, &email);

    if (status == NULL || (email->inreplyto && !email->inreplyto[0]))
	return;

    if (email->inreplyto && email->inreplyto[0]) {
	email2 =
	    hashreplylookup(email->msgnum, email->inreplyto, &subjmatch);
	if (!email2)
	    return;
	replynum = email2->msgnum;
    }

    msnprintf(filename, sizeof(filename), "%s%s%.4d.%s", dir,
	      (dir[strlen(dir) - 1] == '/') ? "" : "/",
	      replynum, set_htmlsuffix);

    bp = NULL;
    fp = fopen(filename, "r");
    if (fp) {
	while ((fgets(line, MAXLINE, fp)) != NULL)
	    bp = addbody(bp, &lp, line, 0);
    }
    else
	return;
    fclose(fp);

    cp = bp;			/* save start of list to free later */

    fp = fopen(filename, "w+");
    if (fp) {
	while (bp) {
	    if (!strncmp(bp->line, "<!-- reply", 10)) {
		fprintf(fp, "<li><strong>%s:</strong> ", lang[MSG_REPLY]);
		fprintf(fp, "<a href=\"%.4d.%s\">", num, set_htmlsuffix);
		fprintf(fp, "%s: \"%s\"</a>\n", email->name,
			ptr = convchars(email->subject));
		free(ptr);
	    }
	    fprintf(fp, "%s", bp->line);
	    bp = bp->next;
	}
    }
    fclose(fp);

    /* can we clean up a bit please... */
    bp = cp;
    while (bp) {
	cp = bp->next;
	if (bp->line)
	    free(bp->line);
	free(bp);
	bp = cp;
    }
}

/*
** Adds a "Next in thread:" link in the proper article, after the archive
** has been incrementally updated.
*/

void fixthreadheader(char *dir, int num)
{
    char filename[MAXFILELEN], line[MAXLINE];
    char *name = NULL;
    char *subject = NULL;
    FILE *fp;
    struct reply *rp;
    struct body *bp, *cp;
    struct body *lp = NULL;
    int threadnum = 0;
    char *ptr;

    for (rp = threadlist; rp != NULL; rp = rp->next) {
	if (rp->next != NULL &&
	    (rp->next->data && rp->next->data->msgnum == num) &&
	    (rp->data && rp->msgnum != -1)
	    ) {

	    threadnum = rp->msgnum;
	    name = rp->next->data->name;
	    subject = rp->next->data->subject;
	    break;
	}
    }

    if (rp == NULL)
	return;

    sprintf(filename, "%s%s%.4d.%s", dir,
	    (dir[strlen(dir) - 1] == '/') ? "" : "/",
	    threadnum, set_htmlsuffix);

    bp = NULL;
    if ((fp = fopen(filename, "r")) != NULL) {
	while ((fgets(line, MAXLINE, fp)) != NULL)
	    bp = addbody(bp, &lp, line, 0);
    }
    else
	return;

    fclose(fp);

    cp = bp;			/* save start of list to free later */

    if ((fp = fopen(filename, "w+")) != NULL) {
	while (bp != NULL) {
	    fprintf(fp, "%s", bp->line);
	    if (!strncmp(bp->line, "<!-- nextthr", 12)) {
		fprintf(fp, "<li><strong>%s:</strong> ",
			lang[MSG_NEXT_IN_THREAD]);
		fprintf(fp, "<a href=\"%.4d.%s\">", num, set_htmlsuffix);
		fprintf(fp, "%s: \"%s\"</a>\n",
			name, ptr = convchars(subject));
		free(ptr);
	    }
	    bp = bp->next;
	}
    }
    fclose(fp);

    /* can we clean up a bit please... */
    bp = cp;
    while (bp != NULL) {
	cp = bp->next;
	if (bp->line)
	    free(bp->line);
	free(bp);
	bp = cp;
    }
}
