#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include "xml.h"
#include "compat.h"

enum { FeedTypeNone = 0, FeedTypeRSS = 1, FeedTypeAtom = 2 };
const char *feedtypes[] = {	"", "rss", "atom" };

enum { ContentTypeNone = 0, ContentTypePlain = 1, ContentTypeHTML = 2 };
const char *contenttypes[] = { "", "plain", "html" };

typedef struct string { /* String data / pool */
	char *data; /* data */
	size_t len; /* string length */
	size_t bufsiz; /* allocated size */
} String;

typedef struct feeditem { /* Feed item */
	String timestamp;
	String title;
	String link;
	String content;
	int contenttype; /* ContentTypePlain or ContentTypeHTML */
	String id;
	String author;
	int feedtype; /* FeedTypeRSS or FeedTypeAtom */
} FeedItem;

void die(const char *s);
void cleanup(void);

String *currentfield = NULL; /* TODO */
const int FieldSeparator = '\t';
FeedItem feeditem; /* data for current feed item */
char feeditemtag[256] = ""; /* current tag _inside_ a feeditem */
size_t feeditemtaglen = 0;
int feeditemtagid = 0;
int iscontent = 0;
int iscontenttag = 0;
size_t attrcount = 0;
char *standardtz = NULL; /* TZ variable at start of program */
XMLParser parser; /* XML parser state */

enum {
	TagUnknown = 0,
	/* RSS */
	RSSTagDcdate, RSSTagPubdate, RSSTagTitle,
	RSSTagLink, RSSTagDescription, RSSTagContentencoded,
	RSSTagGuid, RSSTagAuthor, RSSTagDccreator,
	/* Atom */
	AtomTagPublished, AtomTagUpdated, AtomTagTitle,
	AtomTagSummary, AtomTagContent,
	AtomTagId, AtomTagLink, AtomTagAuthor
};

typedef struct feedtag {
	char *name;
	size_t namelen;
	int id;
} FeedTag;

/* TODO: optimize lookup */
int
gettag(int feedtype, const char *name, size_t namelen) {
	/* RSS, alphabetical order */
	static FeedTag rsstag[] = {
		{ "author", 6, RSSTagAuthor },
		{ "content:encoded", 15, RSSTagContentencoded },
		{ "dc:creator", 10, RSSTagDccreator },
		{ "dc:date", 7, RSSTagDcdate },
		{ "description", 11, RSSTagDescription },
		{ "guid", 4, RSSTagGuid },
		{ "link", 4, RSSTagLink },
		{ "pubdate", 7, RSSTagPubdate },
		{ "title", 5, RSSTagTitle },
		{ NULL, 0, -1 }
	};
	/* Atom, alphabetical order */
	static FeedTag atomtag[] = {
		{ "author", 6, AtomTagAuthor }, /* assume this is: <author><name></name></author> */
		{ "content", 7, AtomTagContent },
		{ "id", 2, AtomTagId },
		{ "link", 4, AtomTagLink },
		{ "published", 9, AtomTagPublished },
		{ "summary", 7, AtomTagSummary },
		{ "title", 5, AtomTagTitle },
		{ "updated", 7, AtomTagUpdated },
		{ NULL, 0, -1 }
	};
	int i, n;
	
	if(namelen >= 2 && namelen <= 15) {
		if(feedtype == FeedTypeRSS) {
			for(i = 0; rsstag[i].name; i++) {
				if(!(n = xstrncasecmp(rsstag[i].name, name, rsstag[i].namelen)))
					return rsstag[i].id;
				/* optimization: it's sorted so nothing after it matches. */
				if(n > 0)
					return TagUnknown;
			}
		} else if(feedtype == FeedTypeAtom) {
			for(i = 0; atomtag[i].name; i++) {
				if(!(n = xstrncasecmp(atomtag[i].name, name, atomtag[i].namelen)))
					return atomtag[i].id;
				/* optimization: it's sorted so nothing after it matches. */
				if(n > 0)
					return TagUnknown;
			}
		}
	}
	return TagUnknown;
}

int
entitytostr(const char *e, char *buffer, size_t bufsiz) {
	/* TODO: optimize lookup? */
	char *entities[6][2] = {
		{ "&lt;", "<" },
		{ "&gt;", ">" },
		{ "&apos;", "'" },
		{ "&amp;", "&" },
		{ "&quot;", "\"" },
		{ NULL, NULL }
	};
	size_t i;
	if(*e != '&' || bufsiz < 2) /* doesnt start with & */
		return 0;
	for(i = 0; entities[i][0]; i++) {
		/* NOTE: compares max 7 chars */
		if(!xstrncasecmp(e, entities[i][0], 6)) {
			buffer[0] = *(entities[i][1]);
			buffer[1] = '\0';
			return 1;
		}
	}
	return 0;
}

void
string_clear(String *s) {
	if(s->data)
		s->data[0] = '\0'; /* clear string only; don't free, prevents
		                      unnecessary reallocation */
	s->len = 0;
}

void
string_buffer_init(String *s, size_t len) {
	if(!(s->data = malloc(len)))
		die("can't allocate enough memory");
	s->bufsiz = len;
	string_clear(s);
}

void
string_free(String *s) {
	free(s->data);
	s->data = NULL;
	s->bufsiz = 0;
	s->len = 0;
}

int
string_buffer_expand(String *s, size_t newlen) {
	char *p;
	size_t alloclen;
	/* check if allocation is necesary, dont shrink buffer
	   should be more than bufsiz ofcourse */
	for(alloclen = 16; alloclen <= newlen; alloclen *= 2);
	if(!(p = realloc(s->data, alloclen))) {
		string_free(s); /* free previous allocation */
		die("can't allocate enough memory");
	}
	s->bufsiz = alloclen;
	s->data = p;
	return s->bufsiz;
}

void
string_append(String *s, const char *data, size_t len) {
	if(!len || *data == '\0')
		return;
	if(s->len + len > s->bufsiz)
		string_buffer_expand(s, s->len + len);
	memcpy(s->data + s->len, data, len);
	s->len += len;
	s->data[s->len] = '\0';
}

void /* cleanup parser, free allocated memory, etc */
cleanup(void) {
	string_free(&feeditem.timestamp);
	string_free(&feeditem.title);
	string_free(&feeditem.link);
	string_free(&feeditem.content);
	string_free(&feeditem.id);
	string_free(&feeditem.author);
}

void /* print error message to stderr */
die(const char *s) {
	fputs("sfeed: ", stderr);
	fputs(s, stderr);
	fputc('\n', stderr);
	exit(EXIT_FAILURE);
}

/* get timezone from string, return as formatted string and time offset,
 * for the offset it assumes GMT */
int
gettimetz(const char *s, char *buf, size_t bufsiz) {
	const char *p = s;
	char tzname[16] = "", *t = NULL;
	int tzhour = 0, tzmin = 0;
	unsigned int i;
	char c;

	buf[0] = '\0';
	if(bufsiz < sizeof(tzname) + 7)
		return 0;
	for(; *p && isspace((int)*p); p++); /* skip whitespace */
	/* loop until some common timezone delimiters are found */
	for(;*p && (*p != '+' && *p != '-' && *p != 'Z' && *p != 'z'); p++);

	/* TODO: cleanup / simplify */
	if(isalpha((int)*p)) {
		if(*p == 'Z' || *p == 'z') {
			memcpy(buf, "GMT+00:00", strlen("GMT+00:00") + 1);
			return 0;
		} else {
			for(i = 0, t = &tzname[0]; i < (sizeof(tzname) - 1) &&
				(*p && isalpha((int)*p)); i++)
				*(t++) = *(p++);
			*t = '\0';
		}
	} else
		memcpy(tzname, "GMT", strlen("GMT") + 1);
	if(!(*p)) {	
		strncpy(buf, tzname, bufsiz);
		return 0;
	}
	if((sscanf(p, "%c%02d:%02d", &c, &tzhour, &tzmin)) > 0);
	else if(sscanf(p, "%c%02d%02d", &c, &tzhour, &tzmin) > 0);
	else if(sscanf(p, "%c%d", &c, &tzhour) > 0)
		tzmin = 0;
	sprintf(buf, "%s%c%02d%02d", tzname, c, tzhour, tzmin);
	/* TODO: test + or - offset */
	return (tzhour * 3600) + (tzmin * 60) * (c == '-' ? -1 : 1);
}

/* parses everything in a format similar to:
 * "%a, %d %b %Y %H:%M:%S" or "%Y-%m-%d %H:%M:%S" */
/* TODO: calculate time offset (GMT only) from gettimetz ? */
int
parsetimeformat(const char *s, struct tm *t, const char **end) {
	static const char *months[] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct",
		"Nov", "Dec"
	};
	const char *p = s;
	unsigned int i, fm;
	unsigned long l;

	memset(t, 0, sizeof(struct tm));
	if((l = strtoul(p, (void *)&p, 10))) {
		t->tm_year = abs(l) - 1900;
		if(!(l = strtoul(p, (void *)&p, 10)))
			return 0;
		t->tm_mon = abs(l) - 1;
		if(!(t->tm_mday = abs(strtoul(p, (void *)&p, 10))))
			return 0;
	} else {
		for(; *p && !isdigit((int)*p); p++);
		if(!(t->tm_mday = abs(strtoul(p, (void *)&p, 10))))
			return 0;
		for(; *p && !isalpha((int)*p); p++); /* skip non-alpha */
		for(fm = 0, i = 0; i < 12; i++) { /* parse month names */
			if(!xstrncasecmp(p, months[i], 3)) {
				t->tm_mon = i;
				fm = 1;
				break;
			}
		}
		if(!fm) /* can't find month */
			return 0;
		for(; *p && !isdigit((int)*p); p++); /* skip non-digit */
		if(!(l = strtoul(p, (void *)&p, 10)))
			return 0;
		t->tm_year = abs(l) - 1900;
	}
	for(; *p && !isdigit((int)*p); p++); /* skip non-digit */
	if((t->tm_hour = abs(strtoul(p, (void *)&p, 10))) > 23)
		return 0;
	for(; *p && !isdigit((int)*p); p++); /* skip non-digit */
	if((t->tm_min = abs(strtoul(p, (void *)&p, 10))) > 59)
		return 0;
	for(; *p && !isdigit((int)*p); p++); /* skip non-digit */
	if((t->tm_sec = abs(strtoul(p, (void *)&p, 10))) > 60)
		return 0;
	if(end)
		*end = p;
	return 1;
}

/* C defines the rounding for division in a nonsensical way */
#define Q(a,b) ((a)>0 ? (a)/(b) : -(((b)-(a)-1)/(b)))

/* copied from Musl C awesome small implementation, see LICENSE. */
time_t
tm_to_time(struct tm *tm) {
	time_t year = tm->tm_year - 100;
	int month = tm->tm_mon;
	int day = tm->tm_mday;
	int daysbeforemon[] = { 0,31,59,90,120,151,181,212,243,273,304,334 };
	int z4, z100, z400;

	/* normalize month */
	if(month >= 12) {
		year += month / 12;
		month %= 12;
	} else if(month < 0) {
		year += month / 12;
		month %= 12;
		if(month) {
			month += 12;
			year--;
		}
	}
	z4 = Q(year - (month < 2), 4); /* is leap? */
	z100 = Q(z4, 25);
	z400 = Q(z100, 4);
	day += year * 365 + z4 - z100 + z400 + daysbeforemon[month];
	return (time_t)day * 86400 +
	       tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec +
	       946684800; /* the dawn of time, aka 1970 (30 years of seconds) :) */
}

time_t
parsetime(const char *s, char *buf) {
	struct tm tm;
	char tz[64];
	const char *end;
	int offset;

	if(buf)
		buf[0] = '\0';
	if(parsetimeformat(s, &tm, &end)) {
		offset = gettimetz(end, tz, sizeof(tz) - 1);
		/* TODO: make sure snprintf cant overflow */
		if(buf)
		   sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d %-.16s",
					tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
					tm.tm_hour, tm.tm_min, tm.tm_sec, tz);
		/* return UNIX time, reverse offset to GMT+0 */
		return tm_to_time(&tm) - offset;
	}
	return -1; /* can't parse */
}

/* print text, ignore tabs, newline and carriage return etc
 * print some HTML 2.0 / XML 1.0 as normal text */
void
string_print_trimmed(String *s) {
/*	const char *entities[] = {
		"&amp;", "&", "&lt;", "<", "&gt;", ">",	"&apos;", "'",
		"&quot;", "\"",
		NULL, NULL
	};
	unsigned char entlen[] = { 5, 4, 4, 6, 6 };*/
	/*unsigned int len, found, i;*/
	const char *p, *n/*, **e*/;
	char buffer[BUFSIZ + 4];
	size_t buflen = 0;

	if(!s->len)
		return;
	for(p = s->data; isspace((int)*p); p++); /* strip leading whitespace */
	for(; *p; ) { /* ignore tabs, newline and carriage return etc, except space */
		/*if(!isspace((int)*p) || *p == ' ') {*/
		if(!((unsigned)*p - '\t' < 5)) {
			if(*p == '<') { /* skip tags */
				if((n = strchr(p, '>'))) {
					p = n + 1;
					continue;
				}
			}
			/* TODO: not necesary anymore because xml_handler_data_entity is used with entitytostr ? */
			/* else if(*p == '&') { 
				for(e = entities, i = 0, found = 0; *e; e += 2, i++) {
					len = entlen[i];
					if(!strncmp(*e, p, len)) {
						buffer[buflen++] = *(e + 1)[0];
						p += len;
						found = 1;
						break;
					}
				}
				if(found)
					continue;
			}*/
			buffer[buflen++] = *p;
		}
		if(buflen >= BUFSIZ) {
			fwrite(buffer, 1, buflen, stdout);
			buflen = 0;
		}
		p++;
	}
/*	printf("%d |", buflen);*/
	if(buflen)
		fwrite(buffer, 1, buflen, stdout);
/*	printf("|\n");*/
}

void /* print text, escape tabs, newline and carriage return etc */
string_print_textblock(String *s) {
	const char *p;
	char buffer[BUFSIZ + 4];
	size_t i;

	if(!s->len)
		return;	
	/* skip leading whitespace */
	for(p = s->data; *p && isspace((int)*p); p++);
	for(i = 0; *p; p++) {
		if(((unsigned)*p - '\t') < 5) {
			if(*p == '\n') { /* escape newline */
				buffer[i++] = '\\';
				buffer[i++] = 'n';
			} else if(*p == '\\') { /* escape \ */
				buffer[i++] = '\\';
				buffer[i++] = '\\';
			} else if(*p == '\t') { /* tab */
				buffer[i++] = '\\';
				buffer[i++] = 't';
			}
			/* ignore other whitespace chars, except space */
		} else {
			buffer[i++] = *p;
		}
		if(i >= BUFSIZ) { /* TODO: align */
			fwrite(buffer, 1, i, stdout);
			i = 0;
		}
	}
	if(i)
		fwrite(buffer, 1, i, stdout);
}

int
istag(const char *name, size_t len, const char *name2, size_t len2) {
	return (len == len2 && !xstrcasecmp(name, name2));
}

int
isattr(const char *name, size_t len, const char *name2, size_t len2) {
	return (len == len2 && !xstrcasecmp(name, name2));
}

/* NOTE: this handler can be called multiple times if the data in this
 * block is bigger than the buffer */
void
xml_handler_data(XMLParser *p, const char *s, size_t len) {
	if(currentfield) {
		if(feeditemtagid != AtomTagAuthor || !strcmp(p->tag, "name")) /* author>name */
			string_append(currentfield, s, len);
	}
}

void
xml_handler_cdata(XMLParser *p, const char *s, size_t len) {
	if(currentfield)
		string_append(currentfield, s, len);
}

void
xml_handler_attr_start(struct xmlparser *p, const char *tag, size_t taglen, const char *name, size_t namelen) {
	if(iscontent && !iscontenttag) {
		if(!attrcount)
			xml_handler_data(p, " ", 1);
		attrcount++;
		xml_handler_data(p, name, namelen);
		xml_handler_data(p, "=\"", 2);
		return;
	}
}

void
xml_handler_attr_end(struct xmlparser *p, const char *tag, size_t taglen, const char *name, size_t namelen) {
	if(iscontent && !iscontenttag) {
		xml_handler_data(p, "\"", 1);
		attrcount = 0;
	}
}

void
xml_handler_start_element_parsed(XMLParser *p, const char *tag, size_t taglen, int isshort) {
	if(iscontent && !iscontenttag) {
		if(isshort)
			xml_handler_data(p, "/>", 2);
		else
			xml_handler_data(p, ">", 1);
	}
}

void
xml_handler_attr(XMLParser *p, const char *tag, size_t taglen,
                 const char *name, size_t namelen, const char *value,
                 size_t valuelen) {
	if(iscontent && !iscontenttag) {
		xml_handler_data(p, value, valuelen);
		return;
	}
	if(feeditem.feedtype == FeedTypeAtom) {
		/*if(feeditemtagid == AtomTagContent || feeditemtagid == AtomTagSummary) {*/
		if(iscontenttag) {
			if(isattr(name, namelen, "type", strlen("type")) &&
			   (isattr(value, valuelen, "xhtml", strlen("xhtml")) || isattr(value, valuelen, "text/xhtml", strlen("text/xhtml")) ||
			    isattr(value, valuelen, "html", strlen("html")) || isattr(value, valuelen, "text/html", strlen("text/html")))) {
				feeditem.contenttype = ContentTypeHTML;
				iscontent = 1;
/*				p->xmldataentity = NULL;*/
				p->xmlattrstart = xml_handler_attr_start;
				p->xmlattrend = xml_handler_attr_end;
				p->xmltagstartparsed = xml_handler_start_element_parsed;
			}
		} else if(feeditemtagid == AtomTagLink && isattr(name, namelen, "href", strlen("href"))) /* link href attribute */
			string_append(&feeditem.link, value, valuelen);
	}
}

void
xml_handler_start_element(XMLParser *p, const char *name, size_t namelen) {
	if(iscontenttag) {
		/* starts with div, handle as XML, dont convert entities */
		/* TODO: test properly and do printf() to debug */
		if(feeditem.feedtype == FeedTypeAtom && !strncmp(name, "div", strlen("div")))
			p->xmldataentity = NULL;
	}
	if(iscontent) {
		attrcount = 0;
		iscontenttag = 0;
		xml_handler_data(p, "<", 1);
		xml_handler_data(p, name, namelen);
		return;
	}

	/* TODO: cleanup, merge with code below ?, return function if FeedTypeNone */
/*	iscontenttag = 0;*/
	if(feeditem.feedtype != FeedTypeNone) { /* in item */
		if(feeditemtag[0] == '\0') { /* set tag if not already set. */
/*			strncpy(feeditemtag, name, sizeof(feeditemtag) - 1);*/
			if(namelen >= sizeof(feeditemtag) - 2)
				return;
			memcpy(feeditemtag, name, namelen + 1);
			feeditemtaglen = namelen; /* XXX: assumes feeditemtag had enough space */
			feeditemtagid = gettag(feeditem.feedtype, feeditemtag, feeditemtaglen);

			if(feeditem.feedtype == FeedTypeRSS) {
				if(feeditemtagid == TagUnknown)
					currentfield = NULL;
				else if(feeditemtagid == RSSTagPubdate || feeditemtagid == RSSTagDcdate)
					currentfield = &feeditem.timestamp;
				else if(feeditemtagid == RSSTagTitle)
					currentfield = &feeditem.title;
				else if(feeditemtagid == RSSTagLink)
					currentfield = &feeditem.link;
				else if(feeditemtagid == RSSTagDescription || feeditemtagid == RSSTagContentencoded) {
					/* clear previous summary, assumes previous content was not a summary text */
					if(feeditemtagid == RSSTagContentencoded && feeditem.content.len)
						string_clear(&feeditem.content);
					/* ignore, prefer content:encoded over description */
					if(!(feeditemtagid == RSSTagDescription && feeditem.content.len)) {
						iscontenttag = 1;
						currentfield = &feeditem.content;
					}
				} else if(feeditemtagid == RSSTagGuid)
					currentfield = &feeditem.id;
				else if(feeditemtagid == RSSTagAuthor || feeditemtagid == RSSTagDccreator)
					currentfield = &feeditem.author;
			} else if(feeditem.feedtype == FeedTypeAtom) {
				if(feeditemtagid == TagUnknown)
					currentfield = NULL;
				else if(feeditemtagid == AtomTagPublished || feeditemtagid == AtomTagUpdated)
					currentfield = &feeditem.timestamp;
				else if(feeditemtagid == AtomTagTitle)
					currentfield = &feeditem.title;
				else if(feeditemtagid == AtomTagSummary || feeditemtagid == AtomTagContent) {
					/* clear previous summary, assumes previous content was not a summary text */
					if(feeditemtagid == AtomTagContent && feeditem.content.len)
						string_clear(&feeditem.content);
					/* ignore, prefer content:encoded over description */
					if(!(feeditemtagid == AtomTagSummary && feeditem.content.len)) {
						iscontenttag = 1;
						currentfield = &feeditem.content;
					}
				} else if(feeditemtagid == AtomTagId)
					currentfield = &feeditem.id;
				else if(feeditemtagid == AtomTagLink)
					currentfield = &feeditem.link;
				else if(feeditemtagid == AtomTagAuthor)
					currentfield = &feeditem.author;
			}
			/* TODO: prefer content encoded over content? */
		}
	} else { /* start of RSS or Atom item / entry */
		if(istag(name, namelen, "entry", strlen("entry"))) { /* Atom */
			feeditem.feedtype = FeedTypeAtom;
			feeditem.contenttype = ContentTypePlain; /* Default content type */
			currentfield = NULL; /* XXX: optimization */
		} else if(istag(name, namelen, "item", strlen("item"))) { /* RSS */
			feeditem.feedtype = FeedTypeRSS;
			feeditem.contenttype = ContentTypeHTML; /* Default content type */
			currentfield = NULL; /* XXX: optimization */
		}
	}
}

void
xml_handler_data_entity(XMLParser *p, const char *data, size_t datalen) {
	char buffer[16];
	size_t len;

#if 0
	if(iscontent) {
		xml_handler_data(p, data, datalen); /* TODO: for now, dont convert entities */
		return;
	}
#endif
	/* TODO: for content HTML data entities, convert &amp; to &? */
	if((len = entitytostr(data, buffer, sizeof(buffer))))
		xml_handler_data(p, buffer, len);
	else
		xml_handler_data(p, data, datalen); /* can't convert entity, just use it's data */
}

void
xml_handler_end_element(XMLParser *p, const char *name, size_t namelen, int isshort) {
	char timebuf[64];
	int tagid;

/*	printf("%d end tag: </%s>\n", iscontent, name);*/
	if(iscontent) {
		attrcount = 0;
		/* TODO: optimize */
		tagid = gettag(feeditem.feedtype, name, namelen);
		if(feeditemtagid == tagid) { /* close content */
			iscontent = 0;
			iscontenttag = 0;

			p->xmldataentity = xml_handler_data_entity;
			p->xmlattrstart = NULL;
			p->xmlattrend = NULL;
			p->xmltagstartparsed = NULL;

			feeditemtag[0] = '\0'; /* unset tag */
			feeditemtaglen = 0;
			feeditemtagid = TagUnknown;

			return; /* TODO: not sure if !isshort check below should be skipped */
		}
		if(!isshort) {
			xml_handler_data(p, "</", 2);
			xml_handler_data(p, name, namelen);
			xml_handler_data(p, ">", 1);
		}
		return;
	}
	if(feeditem.feedtype != FeedTypeNone) {
		/* end of RSS or Atom entry / item */
		/* TODO: optimize, use gettag() ? to tagid? */
		if((feeditem.feedtype == FeedTypeAtom && istag(name, namelen, "entry", strlen("entry"))) || /* Atom */
		  (feeditem.feedtype == FeedTypeRSS && istag(name, namelen, "item", strlen("item")))) { /* RSS */
			printf("%ld", (long)parsetime((&feeditem.timestamp)->data, timebuf));
			putchar(FieldSeparator);
			fputs(timebuf, stdout);
			putchar(FieldSeparator);
			string_print_trimmed(&feeditem.title);
			putchar(FieldSeparator);
			string_print_trimmed(&feeditem.link);
			putchar(FieldSeparator);
			string_print_textblock(&feeditem.content);
			putchar(FieldSeparator);
			fputs(contenttypes[feeditem.contenttype], stdout);
			putchar(FieldSeparator);
			string_print_trimmed(&feeditem.id);
			putchar(FieldSeparator);
			string_print_trimmed(&feeditem.author);
			putchar(FieldSeparator);
			fputs(feedtypes[feeditem.feedtype], stdout);
			putchar('\n');

			/* clear strings */
			string_clear(&feeditem.timestamp);
			string_clear(&feeditem.title);
			string_clear(&feeditem.link);
			string_clear(&feeditem.content);
			string_clear(&feeditem.id);
			string_clear(&feeditem.author);
			feeditem.feedtype = FeedTypeNone;
			feeditem.contenttype = ContentTypePlain;
			feeditemtag[0] = '\0'; /* unset tag */
			feeditemtaglen = 0;
			feeditemtagid = TagUnknown;
			
			/* not sure if needed */
			iscontenttag = 0;
			iscontent = 0;
		} else if(!strcmp(feeditemtag, name)) { /* clear */ /* XXX: optimize ? */
			currentfield = NULL;
			feeditemtag[0] = '\0'; /* unset tag */
			feeditemtaglen = 0;
			feeditemtagid = TagUnknown;
			
			/* not sure if needed */
			iscontenttag = 0;
			iscontent = 0;
		}
	}
}

int
main(void) {
	atexit(cleanup);

	/* init strings and initial memory pool size */
	string_buffer_init(&feeditem.timestamp, 64);
	string_buffer_init(&feeditem.title, 256);
	string_buffer_init(&feeditem.link, 1024);
	string_buffer_init(&feeditem.content, 4096);
	string_buffer_init(&feeditem.id, 1024);
	string_buffer_init(&feeditem.author, 256);
	feeditem.contenttype = ContentTypePlain;
	feeditem.feedtype = FeedTypeNone;

	xmlparser_init(&parser);
	parser.xmltagstart = xml_handler_start_element;
	parser.xmltagend = xml_handler_end_element;
	parser.xmldata = xml_handler_data;
	parser.xmldataentity = xml_handler_data_entity;
	parser.xmlattr = xml_handler_attr;
	parser.xmlcdata = xml_handler_cdata;
	xmlparser_parse(&parser);

	return EXIT_SUCCESS;
}
