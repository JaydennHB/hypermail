#include "hypermail.h"
#include "setup.h"
#include "print.h"
#include "printfile.h"
#include "struct.h"

/*
** printfile - print html header/footer file and fill in values 
**             substituting for magic cookies. Return allocated
**             memory with the header/footer filled in.
**
**      Substitution cookies supported
**
**              %% - '%' character
**              %~ - storage directory
**              %a - Other Archives URL
**              %b - About Archive URL
**              %c - Charset META TAG - Not valid on index pages
**              %e - email addr of message author - Not valid on index pages
**              %f - file name of the HTML document
**              %g - date and time archive generated
**              %h - HMURL
**              %i - Message-id - Not valid on index pages
**              %l - archive label
**              %m - Mailto address
**              %p - PROGNAME
**              %s - Subject of message or Index Title
**              %t - path to top directory ("" if no folders; usually "../",
**                                          sometimes "../../" with folders)
**              %v - VERSION
**              %u - Expanded version link (HMURL,PROGNAME,VERSION)
**              %S - Subject META TAG - Not valid on index pages
**              %A - Author META TAG - Not valid on index pages
**              %D - Date META TAG - Not valid on index pages
**              %G - Two character language
**              \n - newline character
**              \t - tab character
**
*/

int printfile(FILE *fp, char *format, char *label, char *subject,
	      char *dir, char *name, char *email, char *message_id,
	      char *charset, char *date, char *filename)
{
    register char *cp;
    register char *aptr;
    char c;
    char *ptr,*tmpptr=NULL, *tmp_oea=NULL;
    size_t tmplen;

    aptr = format;

    while ((c = *aptr++)) {
	if (c == '\\') {
	    switch (*aptr++) {
	    case 'n':		/* Add the \n character */
		putc('\n', fp);
		continue;
	    case 't':		/* Add the \t character */
		putc('\t', fp);
		continue;
	    default:
		break;
	    }			/* end switch */
	}
	else if (c == '%') {
            char nextone = *aptr++;
	    switch (nextone) {
	    case '%':		/* Add the % character */
		putc('%', fp);
		continue;
	    case '~':		/* %~ - storage directory */
		for (cp = dir; *cp; cp++)
		    putc(*cp, fp);
		continue;
	    case 'A':		/* %e - email address of message author */
		if (email && name) {
#ifdef HAVE_ICONV
		  tmpptr=i18n_convstring(name,"UTF-8",charset,&tmplen);
		  cp = convchars(tmpptr,charset);
		  if(tmpptr)
		    free(tmpptr);
		  fprintf(fp,
			"<meta name=\"Author\" content=\"%s (%s)\" />",
			cp, tmp_oea = obfuscate_email_address(email));
		  if (cp)
		    free(cp);
#else
		fprintf(fp,
			"<meta name=\"Author\" content=\"%s (%s)\" />",
			tmpptr=convchars(name,charset), tmp_oea = obfuscate_email_address(email));
		if (tmpptr)
		  free(tmpptr);
#endif
		if (set_email_address_obfuscation && tmp_oea)
		  free(tmp_oea);
		}
		continue;
	    case 'a':		/* %a - Other Archives URL */
		if (set_archives) {
		    for (cp = set_archives; *cp; cp++)
			putc(*cp, fp);
		}
		continue;
	    case 'B':
	      printf("Warning: the %%B option has been disabled. Use a\n"
		     "style sheet instead. See the INSTALL file for more info.\n");
	      continue;
	    case 'b':		/* %b - About this archive URL */
		if (set_about) {
		    for (cp = set_about; *cp; cp++)
			putc(*cp, fp);
		}
		continue;
	    case 'c':
		if (charset && *charset) {
		    /* only output this if we have a charset */
		    fprintf(fp, "<meta http-equiv=\"Content-Type\""
			    " content=\"text/html; charset=%s\" />\n",
			    charset);
		}
		continue;

	    case 'D':		/* %D - date of message */
		if (date) {
		    fprintf(fp,
			    "<meta name=\"Date\" content=\"%s\" />",
			    date);
		}
		continue;
	    case 'e':		/* %e - email address of message author */
		if (email) {
		    for (cp = email; *cp; cp++)
			putc(*cp, fp);
		}
		continue;
	    case 'f':		/* %f - file name */
		if (filename) {
		    for (cp = filename; *cp; cp++)
			putc(*cp, fp);
		}
		continue;
	    case 'g':		/* %g - date and time archive generated */
		for (cp = getlocaltime(); *cp; cp++)
		    putc(*cp, fp);
		continue;
	    case 'G':		/* %G - Language code */
		if (set_language) {
		    for (cp = set_language; *cp; cp++)
			putc(*cp, fp);
		}
		continue;
	    case 'h':		/* %h - Hypermail Resource Center */
		for (cp = HMURL; *cp; cp++)
		    putc(*cp, fp);
		continue;
	    case 'i':		/* %m - Message-ID of message */
		if (message_id) {
		    for (cp = message_id; *cp; cp++)
			putc(*cp, fp);
		}
		continue;
	    case 'l':		/* %l - Archive label  */
		for (cp = label; *cp; cp++)
		    putc(*cp, fp);
		continue;
	    case 'm':		/* %m - mailto */
		if (set_mailto) {
		    for (cp = set_mailto; *cp; cp++)
			putc(*cp, fp);
		}
		continue;
	    case 'p':		/* %p - PROGNAME */
		for (cp = PROGNAME; *cp; cp++)
		    putc(*cp, fp);
		continue;
	    case 's':		/* %s - Subject of message or Index Title */
		for (ptr = cp = convchars(subject, charset); *cp; cp++)
		    putc(*cp, fp);
		free(ptr);
		continue;
	    case 'S':		/* %s - Subject of message or Index Title */
#ifdef HAVE_ICONV
	        tmpptr=i18n_convstring(subject,"UTF-8",charset, &tmplen);
		fprintf(fp, "<meta name=\"Subject\" content=\"%s\" />",
			cp = convchars(tmpptr,charset));
#else
		fprintf(fp, "<meta name=\"Subject\" content=\"%s\" />",
			cp = convchars(subject, charset));
#endif
		free(cp);
		continue;
	    case 't':
	      {
		struct emailinfo *ep;
		if(hashnumlookup(0, &ep))
		    fprintf(fp, ep->subdir ? ep->subdir->rel_path_to_top : "");
		continue;
	      }
	    case 'v':		/* %v - VERSION */
		for (cp = VERSION; *cp; cp++)
		    putc(*cp, fp);
		continue;
	    case 'u':		/* %u - Expanded Version link */
		fprintf(fp, "<a href=\"%s\">%s %s</a>",
			HMURL, PROGNAME, VERSION);
		continue;
	    default:
		putc('%', fp);
		putc(nextone, fp);
		continue;
	    }			/* end switch */
	}
	putc(c, fp);
    }				/* end while */

    fflush(fp);
    return (0);
}

/*
** Prints the standard page header 
*/

void print_main_header(FILE *fp, bool index_header, char *label, char *name,
		       char *email, char *subject, char *charset,
		       char *date, char *filename, char *rel_path_to_top,
		       int is_deleted, int annotation_robot)
{
    char *title;
    char *rp;
    char *rp2;
    char *css_url;
    char *buffer;
    char *tmp_oea=NULL;
    
    /* JK: Don't know what to do with US-ASCII. If there's no charset,
       assume the default one is ISO-8859-1 */
    if (charset && *charset)
      rp = charset;
    else
      rp = "ISO-8859-1";
    fprintf(fp,
	    "<!DOCTYPE html>\n"
	    "<html xmlns=\"http://www.w3.org/1999/xhtml\" lang=\"%s\">\n",
	    set_language);
    fprintf(fp, "<head>\n");

    if (charset && *charset) {
	/* charset info "as early as possible within the HEAD of the document"
	 */
	fprintf(fp, "<meta charset=\"%s\" />\n", rp);
    }
    fprintf(fp, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />\n");
    fprintf(fp, "<meta name=\"generator\" content=\"%s %s, see %s\" />\n",
                PROGNAME, VERSION, HMURL);

    /* 
     * Strip off any trailing whitespace in TITLE so weblint is happy. 
     */

    /* trio_asprintf(&title, "%s: %s", label, rp = convchars(subject, charset)); */
    rp = convchars(subject, charset);
    
    if (name && date) {
      /* assume that this is the title of a message and not the title of an index */
      rp2 = convchars(name, charset);
      trio_asprintf(&title, "%s %s %s %s %s (%s)", rp,
		    lang[MSG_FROM_TITLE], rp2, lang[MSG_ON_TITLE], date, label);
      free(rp2);
    }
    else
      trio_asprintf(&title, "%s: %s", label, rp);
    free(rp);

    rp = title + (strlen(title) - 1);
    while (isspace(*rp))
	*rp-- = '\0';

    /* 
     * Assure the title meets HTML recommendations of no longer 
     * than 64 characters. Truncate it if needed.
     *
     * JK 13/Aug/2003: This more a style suggestion given in:
     * http://www.w3.org/Provider/Style/TITLE.html
     * As the title is truncated regardless of its contents, it can
     * generate invalid HTML if the cut happens in the middle
     * of an entity. Dom suggested not enforcing this rule of
     * thumb so strictly in this case, so I commented it out.
     */

    /*
      if (strlen(title) > 64)
      *(title+64) = '\0';
      */

    fprintf(fp, "<title>%s</title>\n", (is_deleted) ? lang[MSG_SUBJECT_DELETED] : title);
    free(title);

    if (name && email){
      fprintf(fp, "<meta name=\"Author\" content=\"%s (%s)\" />\n",convchars(name,charset),
	      tmp_oea = obfuscate_email_address(email));
      if (set_email_address_obfuscation && tmp_oea)
          free(tmp_oea);
    }
    fprintf(fp, "<meta name=\"Subject\" content=\"%s\" />\n", rp =
	    convchars(subject, charset));
    free(rp);
    if (date)
	fprintf(fp, "<meta name=\"Date\" content=\"%s\" />\n",date);
    if (use_mailto)
	fprintf(fp, "<link rev=\"made\" href=\"mailto:%s\" />\n", set_mailto);

    /* robot handling */
    if (index_header && set_noindex_onindexes) {
      fprintf(fp,"<meta name=\"robots\" content=\"noindex\" />\n");

    } else if (is_deleted || annotation_robot) {
        char *value;

      /* if the message is deleted, avoid bots, else set the value
	 of the robots robots meta tag according to the info supplied by the message */
        if (is_deleted)
            value = "noindex";
        else if (annotation_robot == 1) 
            value = "nofollow";
        else if (annotation_robot == 2)
            value = "noindex";
        else if (annotation_robot == 3)
            value = "nofollow, noindex";
        else
            value = "noindex"; /* default value, removing gcc warning */
        
        fprintf(fp,"<meta name=\"robots\" content=\"%s\" />\n", value);
    }

    /* print the css url according to the type of header */
    if (index_header && set_icss_url && *set_icss_url) {
        css_url = set_icss_url;

    } else if (!index_header && set_mcss_url && *set_mcss_url) {
        css_url = set_mcss_url;

    } else {
        /* no custom css. Use the default css */
        css_url = set_default_css_url;
    }

    /* concatenate rel_path_to_top if we are using a non-absolute css URL */
    if (rel_path_to_top && !strchr(css_url, ':')  /* urls with : */
        && css_url[0] != '/' && css_url[1] != '/' /* relative protocol urls //foo  */
        && css_url[0] != PATH_SEPARATOR) {        /* absolute local path */
        
        trio_asprintf (&buffer, "%s%s", rel_path_to_top, css_url);
    } else {
        buffer = NULL;
    }
    
    fprintf(fp, "<link rel=\"stylesheet\" href=\"%s\" />\n",
            (buffer) ? buffer: css_url);
    if (buffer) {
        free(buffer);
    }
    
    if (ihtmlheadfile)
      fprintf (fp, "%s", ihtmlheadfile);

    fprintf(fp, "</head>\n");
    fprintf(fp, "<body class=\"%s\">\n",
            (index_header) ? "index" : "message");
}

/*
** Prints the header for an index page.
*/

void print_msg_header(FILE *fp, char *label, char *subject,
		      char *dir, char *name,
                      struct emailinfo *email, 
		      char *filename, int is_deleted)
{
    char *rel_path_to_top;
    char *email_date;
    
    rel_path_to_top = (email->subdir) ? email->subdir->rel_path_to_top : NULL;
    email_date = secs_to_iso_meta(email->date);
    
    if (mhtmlheaderfile) {
	printfile(fp, mhtmlheaderfile, label, subject, dir, name, 
		  email->emailaddr, email->msgid, email->charset,
                  email_date, filename);
    } else {
	print_main_header(fp, FALSE, label, name,
                          email->emailaddr, subject,
			  email->charset, email_date,
                          filename, rel_path_to_top,
			  is_deleted, email->annotation_robot);
    }
}

/*
** Prints the header for the by_date index page.
*/

void print_index_header(FILE *fp, char *label, char *dir, char *subject,
			char *filename, struct emailinfo *email)
{
    char *rel_path_to_top;

    rel_path_to_top = (email && email->subdir) ? email->subdir->rel_path_to_top : NULL;
  
    if (ihtmlheaderfile)
#ifdef HAVE_ICONV
      if (set_i18n){
          printfile(fp, ihtmlheaderfile, label, subject, dir, NULL, NULL,
                    "UTF-8", NULL, NULL, filename);
      }else {
          printfile(fp, ihtmlheaderfile, label, subject, dir, NULL, NULL,
                    NULL, NULL, NULL, filename);
      }
#else
       printfile(fp, ihtmlheaderfile, label, subject, dir, NULL, NULL,
                 NULL, NULL, NULL, filename);
#endif
    else {
	/* print the navigation bar to upper levels */
#ifdef HAVE_ICONV
        if (set_i18n){
            print_main_header(fp, TRUE, label, NULL, NULL, subject, "UTF-8", NULL, NULL,
                              rel_path_to_top, 0, 0);
	} else {
            print_main_header(fp, TRUE, label, NULL, NULL, subject, NULL, NULL, NULL,
                              rel_path_to_top, 0, 0);
	}
#else
        print_main_header(fp, TRUE, label, NULL, NULL, subject, NULL, NULL, NULL,
                          rel_path_to_top, 0, 0);
#endif
	fprintf (fp, "<header class=\"head\">\n");
	if (ihtmlnavbar2upfile)
	  fprintf(fp, "<nav class=\"breadcrumb\" id=\"upper\">\n%s</nav>\n", 
		  ihtmlnavbar2upfile);

	fprintf(fp, "<h1><span class=\"archive-label\">%s</span> %s</h1>\n", label, subject);
    }
}

/*
** Prints the HTML page footer.
*/

void printfooter(FILE *fp, char *htmlfooter, char *label, char *dir,
		 char *subject, char *filename, bool close_div)
{
    printcomment(fp, "trailer", "footer");

    if (htmlfooter)
	printfile(fp, htmlfooter, label, subject,
		  dir, NULL, NULL, NULL, NULL, NULL, filename);
    else if (set_hypermail_colophon) {
	fprintf(fp, "<p class=\"colophon\">\n");
	fprintf(fp, "%s ", lang[MSG_ARCHIVE_GENERATED_BY]);
	fprintf(fp, "<a href=\"%s\">%s %s</a>", HMURL, PROGNAME, VERSION);
	fprintf(fp, ": %s\n", getlocaltime());
	fprintf(fp, "</p>\n");
    }
    if (close_div) {
        fprintf (fp, "</footer>\n");
        fprintf(fp, "</body>\n</html>\n");
    }
}

/*
** Prints the HTML last message and last archived date (used in the indexes).
*/

void printlaststats (FILE *fp, long lastdatenum)
{
    fprintf (fp, "<p id=\"end\" class=\"last-message-date\"><span class=\"heading\">%s</span>: %s</p>\n",
             lang[MSG_LAST_MESSAGE_DATE], getdatestr(lastdatenum));

  if (set_archived_on) {
      fprintf (fp, "<p class=\"archived-on\"><span class=\"heading\">%s</span>: %s</p>\n",  lang[MSG_ARCHIVED_ON], getlocaltime());
  }

}

