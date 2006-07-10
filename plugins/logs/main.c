/* $Id$ */

/*
 *  (C) Copyright 2003-2005 Tomasz Torcz <zdzichu@irc.pl>
 *                          Leszek Krupi�ski <leafnode@wafel.com>
 *                          Adam Kuczy�ski <dredzik@ekg2.org>
 *                          Adam Mikuta <adamm@ekg2.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License Version
 *  2.1 as published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "ekg2-config.h"
#include <ekg/win32.h>

#ifndef __FreeBSD__
#define _XOPEN_SOURCE 600
#define __EXTENSIONS__
#endif

#if defined(__MINGW32__) || defined(__FreeBSD__)
#include <limits.h>
#endif

#include <stdint.h>

#include <ekg/char.h>
#include <ekg/debug.h>
#include <ekg/dynstuff.h>
#include <ekg/log.h>
#include <ekg/plugins.h>
#include <ekg/protocol.h>
#include <ekg/sessions.h>
#include <ekg/stuff.h>
#include <ekg/themes.h> //print()
#include <ekg/vars.h>
#include <ekg/windows.h>
#include <ekg/userlist.h>
#include <ekg/xmalloc.h>

#include <sys/stat.h>
#ifndef NO_POSIX_SYSTEM
#include <sys/mman.h>
#include <arpa/inet.h>
#endif

#include <errno.h>
#include <string.h>

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

#include "main.h"

#ifndef PATH_MAX
#  define PATH_MAX _POSIX_PATH_MAX
#endif


PLUGIN_DEFINE(logs, PLUGIN_LOG, NULL);

#ifdef EKG2_WIN32_SHARED_LIB
	EKG2_WIN32_SHARED_LIB_HELPER
#endif

/* fjuczery */
#undef HAVE_ZLIB		/* actually no avalible... */

logs_log_t *log_curlog = NULL;
	/* log ff types... */
#define LOG_FORMAT_NONE		0
#define LOG_FORMAT_SIMPLE 	1
#define LOG_FORMAT_XML   	2
#define LOG_FORMAT_IRSSI 	3 
#define LOG_GZIP		4  /* maska */
	/* irssi types */
#define LOG_IRSSI_MESSAGE	0
#define LOG_IRSSI_EVENT		1
#define LOG_IRSSI_STATUS	2
#define LOG_IRSSI_INFO		3
#define LOG_IRSSI_ACTION	4
	/* irssi style info messages */
#define IRSSI_LOG_EKG2_OPENED	"--- Log opened %a %b %d %H:%M:%S %Y" 	/* defaultowy log_open_string irssi , jak cos to dodac zmienna... */
#define IRSSI_LOG_EKG2_CLOSED	"--- Log closed %a %b %d %H:%M:%S %Y"	/* defaultowy log_close_string irssi, jak cos to dodac zmienna... */
#define IRSSI_LOG_DAY_CHANGED	"--- Day changed %a %b %d %Y"		/* defaultowy log_day_changed irssi , jak cos to dodac zmienna... */

QUERY(logs_setvar_default)
{
	xfree(config_logs_path);
	xfree(config_logs_timestamp);
	config_logs_path = xstrdup("~/.ekg2/logs/%S/%u");
	config_logs_timestamp = NULL;
	return 0;
}

/* 
 * zwraca irssi lub simple lub xml lub NULL
 * w zaleznosci od ustawien log_format w sesji i log:logs 
 */

int logs_log_format(session_t *s) {
	const char *log_formats;

	if (config_logs_log == LOG_FORMAT_NONE)
		return LOG_FORMAT_NONE;

	if (!s || !(log_formats = session_get(s, "log_formats")))
		return LOG_FORMAT_NONE;

	if (xstrstr(log_formats, "irssi")) 
		return LOG_FORMAT_IRSSI;
	if (config_logs_log == LOG_FORMAT_SIMPLE && xstrstr(log_formats, "simple"))
		return LOG_FORMAT_SIMPLE;
	if (config_logs_log == LOG_FORMAT_XML && xstrstr(log_formats, "xml"))
		return LOG_FORMAT_XML;

	return LOG_FORMAT_NONE;
}


/* zwraca 1 lub  2 lub 3 jesli cos sie zmienilo. (log_format, sciezka, t) i zmienia path w log_window_t i jak cos zamyka plik / otwiera na nowo.
 *        0 jesli nie.
 *        -1 jesli cos sie zjebalo.
 */

int logs_window_check(logs_log_t *ll, time_t t)
{
	session_t *s;
	log_window_t *l = ll->lw;
	int chan = 0;
	int tmp;

	if (!l || !(s = session_find(ll->session)))
		return -1;	

	if (l->logformat != (tmp = logs_log_format(s))) {
		l->logformat = tmp;
		chan = 1;
	}
	if (!(l->path)) {
		chan = 2;
	} else {
		int datechanged = 0; /* bitmaska 0x01 (dzien) 0x02 (miesiac) 0x04 (rok) */
		struct tm *tm	= xmemdup(localtime(&(ll->t)), sizeof(struct tm));
		struct tm *tm2	= localtime(&t);

		/* sprawdzic czy dane z (tm == tm2) */
		if (tm->tm_mday != tm2->tm_mday)	datechanged |= 0x01;
		if (tm->tm_mon != tm2->tm_mon)		datechanged |= 0x02;
		if (tm->tm_year != tm2->tm_year)	datechanged |= 0x04;
		if (
				((datechanged & 0x04) && xstrstr(config_logs_path, "%Y")) ||
				((datechanged & 0x02) && xstrstr(config_logs_path, "%M")) ||
		   		((datechanged & 0x01) && xstrstr(config_logs_path, "%D"))
				)
			chan = 3;
		/* zalogowac jak sie zmienila data */
		if (datechanged && l->logformat == LOG_FORMAT_IRSSI) { /* yes i know it's wrong place for doing this but .... */
			if (!(l->file)) 
				l->file = logs_open_file(l->path, LOG_FORMAT_IRSSI);
			logs_irssi(l->file, ll->session, NULL,
					prepare_timestamp_format(IRSSI_LOG_DAY_CHANGED, time(NULL)),
					0, LOG_IRSSI_INFO, NULL);
		}
		xfree(tm);
	}
	ll->t = t;

	if (chan > 1) {
		char *tmp = l->path;

		l->path = logs_prepare_path(s, ll->uid, t);
		debug("[logs] logs_window_check chan = %d oldpath = %s newpath = %s\n", chan, tmp, l->path);
#if 0 /* TODO: nie moze byc - bo mogl logformat sie zmienic... */
		if (chan != 3 && !xstrcmp(tmp, l->path))  /* jesli sciezka sie nie zmienila to nie otwieraj na nowo pliku */
			chan = -chan; 
#endif
		xfree(tmp);
	}
	if (chan > 0) {
		if (l->file) { /* jesli plik byl otwarty otwieramy go z nowa sciezka */
			fclose(l->file);
			l->file = logs_open_file(l->path, l->logformat);
		} 
	}	
	return chan;
}


logs_log_t *logs_log_find(const char *session, const char *uid, int create) {
	list_t l;
	logs_log_t *temp = NULL;

	if (log_curlog && !xstrcmp(log_curlog->session, session) && !xstrcmp(log_curlog->uid, uid))
		return log_curlog->lw ? log_curlog : logs_log_new(log_curlog, session, uid);

	for (l=log_logs; l; l = l->next) {
		logs_log_t *ll = l->data;
		if ( (!ll->session || !xstrcmp(ll->session, session)) && !xstrcmp(ll->uid, uid)) {
			log_window_t *lw = ll->lw;
			if (lw || !create) {
				if (lw) logs_window_check(ll, time(NULL)); /* tutaj ? */
				return ll;
			} else {
				temp = ll;
				break;
			}
		}
	}
	if (log_curlog && log_curlog->lw) {
		log_window_t *lw = log_curlog->lw;
		xfree(lw->path);
		if (lw->file) /* w sumie jesli jest NULL to byl jakis blad przy fopen()... */
			fclose(lw->file);
		lw->file = NULL;
		xfree(lw);
		log_curlog->lw = NULL;
	}

	if (!create)
		return NULL;

	return (log_curlog = logs_log_new(temp, session, uid));
}

logs_log_t *logs_log_new(logs_log_t *l, const char *session, const char *uid) {
	logs_log_t *ll;
	int created = 0;
		
	debug("[logs] log_new uid = %s session %s", uid, session);
	ll = l ? l : logs_log_find(session, uid, 0);
	debug(" logs_log_t %x\n", ll);

	if (!ll) {
		ll = xmalloc(sizeof(logs_log_t));
		ll->session = xstrdup(session);
		ll->uid = xstrdup(uid);
		created = 1;
	}
	if (!(ll->lw)) {
		ll->lw = xmalloc(sizeof(log_window_t));
		logs_window_check(ll, time(NULL)); /* l->log_format i l->path, l->t */
		ll->lw->file = logs_open_file(ll->lw->path, ll->lw->logformat);
	}
	if (created) {
		time_t t = time(NULL);
		if (ll->lw->logformat == LOG_FORMAT_IRSSI && xstrlen(IRSSI_LOG_EKG2_OPENED)) {
			logs_irssi(ll->lw->file, session, NULL,
					prepare_timestamp_format(IRSSI_LOG_EKG2_OPENED, t),
					0, LOG_IRSSI_INFO, NULL);
		} 
		list_add(&log_logs, ll, 0);
	}
	return ll;
}

void logs_window_new(window_t *w) {
	if (!w->target || !w->session || w->id == 1000)
		return;
	logs_log_new(NULL, session_uid_get(w->session), get_uid(w->session, w->target));
}

FILE *logs_window_close(logs_log_t *l, int close) {
	FILE *f;
	log_window_t *lw;
	if (!l || !(lw = l->lw))
		return NULL;

	f = lw->file;

	xfree(lw->path);
	xfree(lw);
	l->lw = NULL;
	if (close && f) {
		fclose(f);
		return NULL;
	}
	return f;
}

void logs_changed_maxfd(const CHAR_T *var)
{
	int maxfd = config_logs_max_files;
	if (in_autoexec) 
		return;
	debug("maxfd limited to %d\n", maxfd);
/* TODO: sprawdzic ile fd aktualnie jest otwartych jak cos to zamykac najstarsze... dodac kiedy otwarlismy plik i zapisalismy ostatnio cos do log_window_t ? */
}

void logs_changed_path(const CHAR_T *var)
{
	list_t l;
	if (in_autoexec || !log_logs) 
		return;

	for (l = log_logs; l; l = l->next) {
		logs_log_t *ll = l->data;

		if (ll->lw) {
			FILE *f   = ll->lw->file;
			char *tmp = ll->lw->path;
			ll->lw->path = NULL;
			ll->lw->file = NULL;
			if (f) fclose(f);
			xfree(tmp);
/* We don't need reopening file../ recreate magic struct.. because it'd be done when we try log smth into it. */
		}
	}
}

QUERY(logs_postinit) 
{
	list_t w;
	for (w = windows; w; w = w->next) {
		logs_window_new((window_t *) w->data);
	}
	return 0;
}

log_away_t *logs_away_find(char *session) {
	list_t l;
	if (!session)
		return NULL;
	if (!config_away_log)
		return NULL;
	
	for (l = log_awaylog; l; l = l->next) {
		log_away_t *la = l->data;
		if (!xstrcmp(session, la->sname))
			return la;
	}
	return NULL;
}

int logs_away_append(log_away_t *la, const char *channel, const char *uid, const char *message)
{
	log_session_away_t *lsa;
	if (!la)
		return 0;

	lsa = xmalloc(sizeof(log_session_away_t));
	lsa->chname	= xstrdup(channel);
	lsa->uid 	= xstrdup(uid);
	lsa->msg	= xstrdup(message);
	lsa->t 		= time(NULL);

	list_add(&(la->messages), lsa, 0);

	return 1;
}

int logs_away_display(log_away_t *la, int quiet, int free) {
	list_t l;
	if (!la)
		return 0;
	if (list_count(la->messages) != 0) {
		print_window("__status", session_current, 0, "away_log_begin", la->sname);
		for (l = la->messages; l; l = l->next) {
			log_session_away_t *lsa = l->data;
			print_window("__status", session_current, 0, "away_log_msg",
					prepare_timestamp_format(format_find("away_log_timestamp"), lsa->t),
					lsa->chname ? (lsa->chname)+4 : "", (lsa->uid)+4, lsa->msg);
			if (free) {
				xfree(lsa->chname);
				xfree(lsa->uid);
				xfree(lsa->msg);
			}
		}
		print_window("__status", session_current, 0, "away_log_end");
	}
	if (free) {
		list_destroy(la->messages, 1);
		xfree(la->sname);
		list_remove(&log_awaylog, la, 1);
	}
	return 0;
}

log_away_t *logs_away_create(char *session) 
{
	log_away_t *la;

	if (!session_check(session_find(session), 0, "irc")) /* na razie dla irca... */
		return NULL;
/* session_int_get(session_find(session), "awaylog")) ? */

	if (logs_away_find(session))
		return NULL;

	debug("[logs] turning awaylog on for session %s\n", session);

	la = xmalloc(sizeof(log_away_t));
	la->sname = xstrdup(session);
	return list_add(&log_awaylog, la, 0);
}

void logs_changed_awaylog(const CHAR_T *var)
{
	list_t l;
	if (in_autoexec)
		return;
#if USE_UNICODE
	debug("%ls: %d\n", var, config_away_log);
#else
	debug("%s: %d\n", var, config_away_log);
#endif

	if (config_away_log) {
		for (l = sessions; l; l = l->next) {
			session_t *s = l->data;
			if (!xstrcmp(s->status, EKG_STATUS_AWAY) || !xstrcmp(s->status, EKG_STATUS_AUTOAWAY))
				logs_away_create(s->uid);
		}
	} else {
		for (l = log_awaylog; l;) {
			log_away_t *a = l->data;
			l = l->next;
			logs_away_display(a, 0, 1);
		}
	}
}

QUERY(logs_sestatus_handler)
{
	char *session = *(va_arg(ap, char **));
	char *status  = *(va_arg(ap, char **));

	debug("[LOGS_SESTATUS HANDLER %s %s\n", session, status);

	if (!config_away_log)
		return 0;

	if (!session_check(session_find(session), 0, "irc")) /* na razie dla irca... */
		return 0;
/* session_int_get(session_find(session), "awaylog")) ? */

	if (!xstrcmp(status, EKG_STATUS_AWAY) || !xstrcmp(status, EKG_STATUS_AUTOAWAY)) {
		logs_away_create(session);
	} else if (!xstrcmp(status, EKG_STATUS_AVAIL)) {
		if (logs_away_display(logs_away_find(session), 0, 1)) { /* strange */
			debug("[LOGS_SESTATUS] strange no away turned on for this sesssion = %s\n", session);
			return 0; 
		}
	}
	return 0;
}

QUERY(logs_handler_killwin) 
{
	window_t *w = *(va_arg(ap, window_t **));
	logs_window_close(logs_log_find(w->session ? w->session->uid : NULL, w->target, 0), 1);
	return 0;
}

int logs_plugin_init(int prio)
{
	plugin_register(&logs_plugin, prio);

        logs_setvar_default(NULL, NULL);

	query_connect(&logs_plugin, TEXT("set-vars-default"), logs_setvar_default, NULL);
	query_connect(&logs_plugin, TEXT("protocol-message-post"), logs_handler, NULL);
	query_connect(&logs_plugin, TEXT("irc-protocol-message"), logs_handler_irc, NULL);
	query_connect(&logs_plugin, TEXT("ui-window-new"), logs_handler_newwin, NULL);
	query_connect(&logs_plugin, TEXT("ui-window-kill"),logs_handler_killwin, NULL);
	query_connect(&logs_plugin, TEXT("protocol-status"), logs_status_handler, NULL);
	query_connect(&logs_plugin, TEXT("config-postinit"), logs_postinit, NULL);
	query_connect(&logs_plugin, TEXT("session-status"), logs_sestatus_handler, NULL);
	/* TODO: moze zmienna sesyjna ? ;> */
	variable_add(&logs_plugin, TEXT("away_log"), VAR_INT, 1, &config_away_log, &logs_changed_awaylog, NULL, NULL);
	/* TODO: maksymalna ilosc plikow otwartych przez plugin logs */
	variable_add(&logs_plugin, TEXT("log_max_open_files"), VAR_INT, 1, &config_logs_max_files, &logs_changed_maxfd, NULL, NULL); 
	variable_add(&logs_plugin, TEXT("log"), VAR_MAP, 1, &config_logs_log, &logs_changed_path, 
			variable_map(3, 
				LOG_FORMAT_NONE, 0, "none", 
				LOG_FORMAT_SIMPLE, LOG_FORMAT_XML, "simple", 
				LOG_FORMAT_XML, LOG_FORMAT_SIMPLE, "xml"), 
			NULL);
	variable_add(&logs_plugin, TEXT("log_ignored"), VAR_INT, 1, &config_logs_log_ignored, NULL, NULL, NULL);
	variable_add(&logs_plugin, TEXT("log_status"), VAR_BOOL, 1, &config_logs_log_status, &logs_changed_path, NULL, NULL);
	variable_add(&logs_plugin, TEXT("path"), VAR_DIR, 1, &config_logs_path, NULL, NULL, NULL);
	variable_add(&logs_plugin, TEXT("timestamp"), VAR_STR, 1, &config_logs_timestamp, NULL, NULL, NULL);

	logs_changed_awaylog(NULL); /* nie robi sie automagicznie to trzeba sila. */

	return 0;
}

static int logs_plugin_destroy()
{
	list_t l = log_logs;

	for (l = log_logs; l; l = l->next) {
		logs_log_t *ll = l->data;
		FILE *f = NULL;
		time_t t = time(NULL);
		int ff = (ll->lw) ? ll->lw->logformat : logs_log_format(session_find(ll->session));

		/* TODO: rewrite */
		if (ff == LOG_FORMAT_IRSSI && xstrlen(IRSSI_LOG_EKG2_CLOSED)) {
			char *path	= (ll->lw) ? xstrdup(ll->lw->path) : logs_prepare_path(session_find(ll->session), ll->uid, t);
			f		= (ll->lw) ? logs_window_close(l->data, 0) : NULL; 
			
			if (!f) 
				f = logs_open_file(path, ff);
			xfree(path);
		} else 
			logs_window_close(l->data, 1);

		if (f) {
			if (ff == LOG_FORMAT_IRSSI && xstrlen(IRSSI_LOG_EKG2_CLOSED)) {
				logs_irssi(f, ll->session, NULL,
						prepare_timestamp_format(IRSSI_LOG_EKG2_CLOSED, t), 0,
						LOG_IRSSI_INFO, NULL);
			}
			fclose(f);
		}

		xfree(ll->session);
		xfree(ll->uid);
	}
	list_destroy(log_logs, 1);

	for (l = log_awaylog; l;) {
		log_away_t *a = l->data;
		l = l->next;	
		logs_away_display(a, 1, 1);
	}

	plugin_unregister(&logs_plugin);
	return 0;
}


/*
 * przygotowanie nazwy pliku bez rozszerzenia
 * %S - sesja nasza
 * %u - u�ytkownik (uid), z kt�rym piszemy
 * %U - u�ytkownik (nick)   -||-
 * %Y, %M, %D - rok, miesi�c, dzie�
 * zwraca �cie�k�, kt�ra nale�y r�cznie zwolni� przez xfree()
 */

char *logs_prepare_path(session_t *session, const char *uid, time_t sent)
{
	char *tmp, *uidtmp, datetime[5];
	struct tm *tm = NULL;
	string_t buf;

	if (!(tmp = config_logs_path))
		return NULL;

	buf = string_init(NULL);

	while (*tmp) {
		if ((char)*tmp == '%' && (tmp+1) != NULL) {
			switch (*(tmp+1)) {
				case 'S':	string_append_n(buf, session->uid, -1);
						break;
				case 'u':	uidtmp = xstrdup(get_uid(session, uid));
						goto attach; /* avoid code duplication */
				case 'U':	uidtmp = xstrdup(get_nickname(session, uid));
					attach:
						if (xstrchr(uidtmp, '/'))
							*(xstrchr(uidtmp, '/')) = 0; // strip resource
						string_append_n(buf, uidtmp, -1);
						xfree(uidtmp);
						break;
				case 'Y':	if (!tm) tm = localtime(&sent);
						snprintf(datetime, 5, "%4d", tm->tm_year+1900);
						string_append_n(buf, datetime, 4);
						break;
				case 'M':	if (!tm) tm = localtime(&sent);
						snprintf(datetime, 3, "%02d", tm->tm_mon+1);
						string_append_n(buf, datetime, 2);
						break;
				case 'D':       if (!tm) tm = localtime(&sent);
						snprintf(datetime, 3, "%02d", tm->tm_mday);
						string_append_n(buf, datetime, 2);
						break;
				default:	string_append_c(buf, *(tmp+1));
			};

			tmp++;
		} else if (*tmp == '~' && (*(tmp+1) == '/' || *(tmp+1) == '\0')) {
			string_append_n(buf, home_dir, -1);
			//string_append_c(buf, '/');
		} else
			string_append_c(buf, *tmp);
		tmp++;
	};

	// sanityzacja sciezki - wywalic "../", zamienic znaki spec. na inne
	// zamieniamy szkodliwe znaki na minusy, spacje na podkreslenia
	// TODO
	xstrtr(buf->str, ' ', '_');

	return string_free(buf, 0);
}


/*
 * otwarcie pliku do zapisu/odczytu
 * tworzy wszystkie katalogi po drodze, je�li nie istniej� i mkdir =1
 * ff - xml 2 || irssi 3 || simple 1
 * zwraca numer deskryptora b�d� NULL
 */

FILE* logs_open_file(char *path, int ff)
{
	char fullname[PATH_MAX+1];
	int len;
	int slash_pos = 0;
#ifdef HAVE_ZLIB
	int zlibmode = 0;
#endif
	if (ff != LOG_FORMAT_IRSSI && ff != LOG_FORMAT_SIMPLE && ff != LOG_FORMAT_XML) {
		if (ff == LOG_FORMAT_NONE)
			debug("[logs] opening log file %s with ff == LOG_FORMAT_NONE CANCELLED\n", path, ff);
		else	debug("[logs] opening log file %s with ff == %d CANCELED\n", path, ff);
		return NULL;
	}

        debug("[logs] opening log file %s ff:%d\n", path, ff);

	if (!path) {
		errno = EACCES; /* = 0 ? */
		return NULL;
	}
	xstrncpy(fullname, path, PATH_MAX);
	len = xstrlen(path);
	while (1) {
		struct stat statbuf;
		char *slash, *dir;

		if (!(slash = xstrchr(path + slash_pos, '/'))) {
			// nie ma juz slashy - zostala tylko nazwa pliku
			break; // konczymy petle
		};

		slash_pos = slash - path + 1;
		dir = xstrndup(path, slash_pos);

		if (stat(dir, &statbuf) != 0 && 
#ifndef NO_POSIX_SYSTEM
			mkdir(dir, 0700) == -1
#else
			mkdir(dir) == -1
#endif
			) {

			char *bo = saprintf("[logs_mkdir]: nie mo�na %s bo %s", dir, strerror(errno));
			print("generic_error",bo); // XXX usun�� !! 
			xfree(bo);
			xfree(dir);
			return NULL;
		}
		xfree(dir);
	} // while 1

	if (len+5 < PATH_MAX) {
		if (ff == LOG_FORMAT_IRSSI)		xstrcat(fullname, ".log");
		else if (ff == LOG_FORMAT_SIMPLE)	xstrcat(fullname, ".txt");
		else if (ff == LOG_FORMAT_XML)		xstrcat(fullname, ".xml");
		len+=4;
#ifdef HAVE_ZLIB /* z log.c i starego ekg1. Wypadaloby zaimplementowac... */
	/* nawet je�li chcemy gzipowane logi, a istnieje nieskompresowany log,
	 * olewamy kompresj�. je�li loga nieskompresowanego nie ma, dodajemy
	 * rozszerzenie .gz i balujemy. */
		if (config_log & 4) {
			struct stat st;
			if (stat(fullname, &st) == -1) {
				gzFile f;

				if (!(f = gzopen(path, "a")))
					goto cleanup;

				gzputs(f, buf);
				gzclose(f);

				zlibmode = 1;
			}
		}
		if (zlibmode && len+4 < PATH_MAX) {
			ff |= LOG_GZIP;
			xstrcat(fullname, ".gz");
		}
#endif
	}
	/* if xml, prepare xml file */
	if (ff == LOG_FORMAT_XML) {
		FILE *fdesc = fopen(fullname, "r+");
		if (!fdesc) {
			if (!(fdesc = fopen(fullname, "w+")))
				return NULL;
			fputs("<?xml version=\"1.0\"?>\n", fdesc);
			fputs("<!DOCTYPE ekg2log PUBLIC \"-//ekg2log//DTD ekg2log 1.0//EN\" ", fdesc);
			fputs("\"http://www.ekg2.org/DTD/ekg2log.dtd\">\n", fdesc);
			fputs("<ekg2log xmlns=\"http://www.ekg2.org/DTD/\">\n", fdesc);
			fputs("</ekg2log>\n", fdesc);
		} 
		return fdesc;
	}

	return fopen(fullname, "a+");
}

/* 
 * zwraca na przemian jeden z dw�ch statycznych bufor�w, wi�c w obr�bie
 * jednego wyra�enia mo�na wywo�a� t� funkcj� dwukrotnie.
 */
/* w sumie starczylby 1 statyczny bufor ... */
const char *prepare_timestamp_format(const char *format, time_t t) 
{
	static char buf[2][100];
	struct tm *tm = localtime(&t);
	static int i = 0;

	if (!format)
		return itoa(t);

	i = i % 2;
	if (!strftime(buf[i], sizeof(buf[0]), format, tm) && xstrlen(format)>0)
		xstrcpy(buf[i], "TOOLONG");
	return buf[i++];
}

/**
 * glowny handler
 */

QUERY(logs_handler)
{
	char *session	= *(va_arg(ap, char**));
	char *uid	= *(va_arg(ap, char**));
	char **rcpts	= *(va_arg(ap, char***));
	char *text	= *(va_arg(ap, char**));
	uint32_t *format= *(va_arg(ap, uint32_t**)); 
	time_t   sent	= *(va_arg(ap, time_t*));
	int  class	= *(va_arg(ap, int*));
	char *seq	= *(va_arg(ap, char**));

	session_t *s = session_find(session); // session pointer
	log_window_t *lw;
	char *ruid;

	/* olewamy jesli to irc i ma formatke irssi like, czekajac na irc-protocol-message */
	if (session_check(s, 0, "irc") && logs_log_format(s) == LOG_FORMAT_IRSSI) 
		return 0;

	class &= ~EKG_NO_THEMEBIT;
	ruid = (class == EKG_MSGCLASS_SENT || class == EKG_MSGCLASS_SENT_CHAT) ? rcpts[0] : uid;

	lw = logs_log_find(session, ruid, 1)->lw;

	if (!lw) {
		debug("[LOGS:%d] logs_handler, shit happen\n", __LINE__);
		return 0;
	}

	if ( !(lw->file) && !(lw->file = logs_open_file(lw->path, lw->logformat)) ) {
		debug("[LOGS:%d] logs_handler Cannot open/create file: %s\n", __LINE__, lw->path);
		return 0;
	}
	
/*	debug("[LOGS_MSG_HANDLER] %s : %s %d %x\n", ruid, lw->path, lw->logformat, lw->file); */

/* uid = uid | ruid ? */
	if (lw->logformat == LOG_FORMAT_IRSSI)
		logs_irssi(lw->file, session, uid, text, sent, LOG_IRSSI_MESSAGE, NULL);
	else if (lw->logformat == LOG_FORMAT_SIMPLE)
		logs_simple(lw->file, session, ruid, text, sent, class, (uint32_t)NULL, (uint16_t)NULL, (char*)NULL);
	else if (lw->logformat == LOG_FORMAT_XML)
		logs_xml(lw->file, session, uid, text, sent, class);
	// itd. dla innych formatow logow

	return 0;
}

/*
 * status handler
 */

QUERY(logs_status_handler)
{
	char *session	= *(va_arg(ap, char**));
	char *uid	= *(va_arg(ap, char**));
	char *status	= *(va_arg(ap, char**));
	char *descr	= *(va_arg(ap, char**));

	session_t *s; // session pointer
	userlist_t *userlist;
	uint32_t ip;
	uint16_t port;

	log_window_t *lw;

/* joiny, party	ircowe jakies inne query. lub zrobic to w pluginie irc... ? */
/* 
	if (session_check(s, 0, "irc") && !xstrcmp(logs_log_format(s), "irssi"))
		return 0;
*/
	if (config_logs_log_status <= 0)
		return 0;
	
	lw = logs_log_find(session, uid, 1)->lw;
	
	if (!lw) {
		debug("[LOGS:%d] logs_status_handler, shit happen\n", __LINE__);
		return 0;
	}

	if ( !(lw->file) && !(lw->file = logs_open_file(lw->path, lw->logformat)) ) {
		debug("[LOGS:%d] logs_status_handler Cannot open/create file: %s\n", __LINE__, lw->path);
		return 0;
	}

/*	debug("[LOGS_STATUS_HANDLER] %s : %s %d %x\n", uid, lw->path, lw->logformat, lw->file); */

/* jesli nie otwarl sie plik to po co mamy robic ? */
	s = session_find(session);
	userlist = userlist_find(s, uid);
	ip=userlist?userlist->ip:0;
	port=userlist?userlist->port:0;

	if (!descr)
		descr = "";

	if (lw->logformat == LOG_FORMAT_IRSSI) {
		char *_what = NULL;
		char *_ip = saprintf("~%s@%s:%d", "notirc", inet_ntoa((struct in_addr) {ip}), port);

		_what = saprintf("%s (%s)", descr, status);
		
		logs_irssi(lw->file, session, uid, _what, time(NULL), LOG_IRSSI_STATUS, _ip);
		
		xfree(_what);
		xfree(_ip);

	} else if (lw->logformat == LOG_FORMAT_SIMPLE) {
		logs_simple(lw->file, session, uid, descr, time(NULL), 6, ip, port, status);
	} else if (lw->logformat == LOG_FORMAT_XML) {
/*		logs_xml(lw->file, session, uid, descr, time(NULL), 6, ip, port, status); */
	}
	return 0;
}


QUERY(logs_handler_irc) 
{
	char *session	= *(va_arg(ap, char**));
	char *uid	= *(va_arg(ap, char**));
	char *text	= *(va_arg(ap, char**));
	int  isour 	= *(va_arg(ap, int*));
	int  foryou	= *(va_arg(ap, int*));
	int  private	= *(va_arg(ap, int*));
	char *channame	= *(va_arg(ap, char**));

	log_window_t *lw = logs_log_find(session, channame, 1)->lw;

	if (foryou) { /* only messages to us */
		if (private) {
			logs_away_append(logs_away_find(session), NULL, channame, text);
		} else {
			char *tmp = saprintf("irc:%s", uid); /* czemu uid nie ma irc: ? */
			logs_away_append(logs_away_find(session), channame, tmp, text);
			xfree(tmp);
		}
	}

	if (!lw) {
		debug("[LOGS:%d] logs_handler_irc, shit happen\n", __LINE__);
		return 0;
	}

/*	debug("[LOGS_MSG_IRC_HANDLER] %s: %s %d %x\n", uid, lw->path, lw->logformat, lw->file); */

	if ( !(lw->file) && !(lw->file = logs_open_file(lw->path, lw->logformat)) ) { 
		debug("[LOGS:%d] logs_handler_irc Cannot open/create file: %s\n", __LINE__, lw->path);
		return 0;
	}

	if (lw->logformat == LOG_FORMAT_IRSSI) 
		logs_irssi(lw->file, session, uid, text, time(NULL), LOG_IRSSI_MESSAGE, NULL);
/* ITD dla innych formatow logow */
	return 0;
}

QUERY(logs_handler_newwin)
{
	window_t *w = *(va_arg(ap, window_t **));
	logs_window_new(w);
	return 0;
}

/*
 * zapis w formacie znanym z ekg1
 * typ,uid,nickname,timestamp,{timestamp wyslania dla odleglych}, text
 */

void logs_simple(FILE *file, const char *session, const char *uid, const char *text, time_t sent, int class, uint32_t ip, uint16_t port, const char *status)
{
	char *textcopy;
	const char *timestamp = prepare_timestamp_format(config_logs_timestamp, time(0));

	session_t *s = session_find((const char*)session);
	const char *gotten_uid = get_uid(s, uid);
	const char *gotten_nickname = get_nickname(s, uid);

	if (!file)
		return;
	textcopy = log_escape(text);

	if (!gotten_uid)	gotten_uid = uid;
	if (!gotten_nickname)	gotten_nickname = uid;

	if (class!=6) {
		switch ((enum msgclass_t)class) {
			case EKG_MSGCLASS_MESSAGE	: fputs("msgrecv,", file);
							  break;
			case EKG_MSGCLASS_CHAT		: fputs("chatrecv,", file);
							  break;
			case EKG_MSGCLASS_SENT		: fputs("msgsend,", file);
							  break;
			case EKG_MSGCLASS_SENT_CHAT	: fputs("chatsend,", file);
							  break;
			case EKG_MSGCLASS_SYSTEM	: fputs("msgsystem,", file);
							  break;
			default				: fputs("chatrecv,", file);
							  break;

		};
	} else {
		fputs("status,",file);
	}
	
	/*
	 * chatsend,<numer>,<nick>,<czas>,<tre��>
	 * chatrecv,<numer>,<nick>,<czas_otrzymania>,<czas_nadania>,<tre��>
	 * status,<numer>,<nick>,<ip>,<time>,<status>,<descr>
	 */

	fputs(gotten_uid, file);      fputc(',', file);
	fputs(gotten_nickname, file); fputc(',', file);
	if (class==6) {
		fputs(inet_ntoa((struct in_addr){ip}), file);
	       	fputc(':', file);
		fputs(itoa(port), file); 
	       	fputc(',', file);
	}

	fputs(timestamp, file); fputc(',', file);
	
	if (class == EKG_MSGCLASS_MESSAGE || class == EKG_MSGCLASS_CHAT) {
		const char *senttimestamp = prepare_timestamp_format(config_logs_timestamp, sent);
		fputs(senttimestamp, file);
		fputc(',', file);
	} else if (class==6) {
		fputs(status, file); 
		fputc(',', file);
	}
	if (textcopy) fputs(textcopy, file);
	fputs("\n", file);

	xfree(textcopy);
	fflush(file);
}


/*
 * zapis w formacie xml
 */

void logs_xml(FILE *file, const char *session, const char *uid, const char *text, time_t sent, int class)
{
	session_t *s;
	CHAR_T *textcopy;
	const char *timestamp = prepare_timestamp_format(config_logs_timestamp, time(NULL));
/*	const char *senttimestamp = prepare_timestamp_format(config_logs_timestamp, sent); */
	CHAR_T *gotten_uid, *gotten_nickname;
	const char *tmp;

	if (!file)
		return;

	textcopy	= xml_escape( text);

	s = session_find((const char*)session);
	gotten_uid 	= xml_escape( (tmp = get_uid(s, uid)) 		? tmp : uid);
	gotten_nickname = xml_escape( (tmp = get_nickname(s, uid)) 	? tmp : uid);

	fseek(file, -11, SEEK_END); /* wracamy przed </ekg2log> */

	/*
	 * <message class="chatsend">
	 * <time>
	 *	<sent>...</sent>
	 *	<received>...</received>
	 * </time>
	 * <sender>
	 *	<uid>...</uid>
	 *	<nick>...</nick>
	 * </sender>
	 * <body>
	 *	(#PCDATA)
	 * </body>
	 * </message>
	 */

	fputs("<message class=\"",file);

	switch ((enum msgclass_t)class) {
		case EKG_MSGCLASS_MESSAGE	: fputs("msgrecv", file);
						  break;
		case EKG_MSGCLASS_CHAT		: fputs("chatrecv", file);
						  break;
		case EKG_MSGCLASS_SENT		: fputs("msgsend", file);
						  break;
		case EKG_MSGCLASS_SENT_CHAT	: fputs("chatsend", file);
						  break;
		case EKG_MSGCLASS_SYSTEM	: fputs("msgsystem", file);
						  break;
		default				: fputs("chatrecv", file);
						  break;

	};

	fputs("\">\n", file);

	fputs("\t<time>\n", file);
	fputs("\t\t<received>", file); fputs(timestamp, file); fputs("</received>\n", file);
	if (class == EKG_MSGCLASS_MESSAGE || class == EKG_MSGCLASS_CHAT) {
		fputs("\t\t<sent>", file); fputs(timestamp, file); fputs("</sent>\n", file);
	}
	fputs("\t</time>\n", file);

	fputs("\t<sender>\n", file);
	fputs("\t\t<uid>", file);   fputs(gotten_uid, file);       fputs("</uid>\n", file);
	fputs("\t\t<nick>", file);  fputs(gotten_nickname, file);  fputs("</nick>\n", file);
	fputs("\t</sender>\n", file);

	fputs("\t<body>\n", file);
	if (textcopy) fputs(textcopy, file);
	fputs("\t</body>\n", file);

	fputs("</message>\n", file);
	fputs("</ekg2log>\n", file);

	xfree(textcopy);
	xfree(gotten_uid);
	xfree(gotten_nickname);
	fflush(file);
}

/*
 * zapis w formacie gaim'a
 */

void logs_gaim()
{
}

/*
 * write to file like irssi do.
 */

void logs_irssi(FILE *file, const char *session, const char *uid, const char *text, time_t sent, int type, const char *ip) {
	const char *nuid;

	if (!file)
		return;

//	nuid = get_nickname(session_find(session), uid);
	nuid = uid;
	
	switch (type) {
		/* just normal message */
		case LOG_IRSSI_MESSAGE:	fprintf(file, "%s <%s> %s\n", prepare_timestamp_format(config_logs_timestamp, sent), nuid ? nuid : uid , text);
			break;
		/* status message (avalible -> unavalible (quit) ; na -> aval (join) */
		case LOG_IRSSI_EVENT:	fprintf(file, "%s -!- %s [%s] has %s #%s\n", prepare_timestamp_format(config_logs_timestamp, sent), nuid ? nuid : uid, ip, text /* join, part, quit */, session);
			break;
		/* other messages like session started, session closed and so on */
		case LOG_IRSSI_INFO:	fprintf(file, "%s\n", text);
			break;
		/* status message (other than @1) */
		case LOG_IRSSI_STATUS:	text = saprintf("reports status: %s [%s] /* {status} */", text, ip);
		/* irc ACTION messages */
		case LOG_IRSSI_ACTION:	fprintf(file, "%s * %s %s\n", prepare_timestamp_format(config_logs_timestamp, sent), nuid ? nuid : uid, text);
			if (type == 2) xfree((char *) text);
			break;
		/* everythink else */
		default: debug("[LOGS_IRSSI] UTYPE = %d\n", type);
			 return; /* to avoid flushisk file */
	}
	fflush(file);
}

/*
 * Local Variables:
 * mode: c
 * c-file-style: "k&r"
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 * vim: sts=8 sw=8 noexpandtab
 */
