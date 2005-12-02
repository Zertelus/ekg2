/* $Id$ */

/*
 *  (C) Copyright 2003-2005 Wojtek Kaniewski <wojtekka@irc.pl>
 *                          Tomasz Torcz <zdzichu@irc.pl>
 *                          Leszek Krupi�ski <leafnode@pld-linux.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License Version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "ekg2-config.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/utsname.h> /* dla jabber:iq:version */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <netdb.h>

#ifdef HAVE_EXPAT_H
#  include <expat.h>
#endif

#ifdef __sun      /* Solaris, thanks to Beeth */
#include <sys/filio.h>
#endif

#include <ekg/debug.h>
#include <ekg/dynstuff.h>
#include <ekg/protocol.h>
#include <ekg/sessions.h>
#include <ekg/stuff.h>
#include <ekg/userlist.h>
#include <ekg/themes.h>
#include <ekg/vars.h>
#include <ekg/xmalloc.h>
#include <ekg/log.h>

#include "jabber.h"

#define jabberfix(x,a) ((x) ? x : a)

static int jabber_theme_init();

PLUGIN_DEFINE(jabber, PLUGIN_PROTOCOL, jabber_theme_init);

/*
 * jabber_private_destroy()
 *
 * inicjuje jabber_private_t danej sesji.
 */
static void jabber_private_init(session_t *s)
{
        const char *uid = session_uid_get(s);
        jabber_private_t *j;

        if (xstrncasecmp(uid, "jid:", 4))
                return;

        if (session_private_get(s))
                return;

        j = xmalloc(sizeof(jabber_private_t));
        j->fd = -1;
        session_private_set(s, j);
}

/*
 * jabber_private_destroy()
 *
 * zwalnia jabber_private_t danej sesji.
 */
static void jabber_private_destroy(session_t *s)
{
        jabber_private_t *j = session_private_get(s);
        const char *uid = session_uid_get(s);

        if (xstrncasecmp(uid, "jid:", 4) || !j)
                return;

        xfree(j->server);
        xfree(j->stream_id);

        if (j->parser)
                XML_ParserFree(j->parser);

        xfree(j);

        session_private_set(s, NULL);
}

/*
 * jabber_session()
 *
 * obs�uguje dodawanie i usuwanie sesji -- inicjalizuje lub zwalnia
 * struktur� odpowiedzialn� za wn�trzno�ci jabberowej sesji.
 */
QUERY(jabber_session)
{
        char **session = va_arg(ap, char**);
        session_t *s = session_find(*session);

        if (!s)
                return -1;

        if (data)
                jabber_private_init(s);
        else
                jabber_private_destroy(s);

        return 0;
}

/*
 * jabber_print_version()
 *
 * wy�wietla wersj� pluginu i biblioteki.
 */
QUERY(jabber_print_version)
{
        print("generic", XML_ExpatVersion());

        return 0;
}

/*
 * jabber_validate_uid()
 *
 * sprawdza, czy dany uid jest poprawny i czy plugin do obs�uguje.
 */
QUERY(jabber_validate_uid)
{
        char *uid = *(va_arg(ap, char **)), *m;
        int *valid = va_arg(ap, int *);

        if (!uid)
                return 0;

	/* minimum: jid:a@b */
        if (!xstrncasecmp(uid, "jid:", 4) && (m=xstrchr(uid, '@')) &&
			((uid+4)<m) && xstrlen(m+1)) {
                (*valid)++;
		return -1;
	}

        return 0;
}

int jabber_write_status(session_t *s)
{
        jabber_private_t *j = session_private_get(s);
        int priority = session_int_get(s, "priority");
        const char *status;
        char *descr;
	char *real = NULL;

        if (!s || !j)
                return -1;

        if (!session_connected_get(s))
                return 0;

        status = session_status_get(s);
	if ((descr = jabber_escape(session_descr_get(s)))) {
		real = saprintf("<status>%s</status>", descr);
		xfree(descr);
	}

	if (!xstrcmp(status, EKG_STATUS_AVAIL))			jabber_write(j, "<presence>%s<priority>%d</priority></presence>", 			real ? real : "", priority);
	else if (!xstrcmp(status, EKG_STATUS_INVISIBLE))	jabber_write(j, "<presence type=\"invisible\">%s<priority>%d</priority></presence>", 	real ? real : "", priority);
	else							jabber_write(j, "<presence><show>%s</show>%s<priority>%d</priority></presence>", 	status, real ? real : "", priority);

        xfree(real);
        return 0;
}

void jabber_handle(void *data, xmlnode_t *n)
{
        jabber_handler_data_t *jdh = (jabber_handler_data_t*) data;
        session_t *s = jdh->session;
        jabber_private_t *j;

        if (!s || !(j = jabber_private(s)) || !n) {
                debug("[jabber] jabber_handle() invalid parameters\n");
                return;
        }

        debug("[jabber] jabber_handle() <%s>\n", n->name);

        if (!xstrcmp(n->name, "message")) {
		jabber_handle_message(n, s, j);
	} else if (!xstrcmp(n->name, "iq")) {
		jabber_handle_iq(n, jdh);
	} else if (!xstrcmp(n->name, "presence")) {
		jabber_handle_presence(n, s);
	} else {
		debug("[jabber] what's that: %s ?\n", n->name);
	}
};

void jabber_handle_message(xmlnode_t *n, session_t *s, jabber_private_t *j) {
	xmlnode_t *nbody = xmlnode_find_child(n, "body");
	xmlnode_t *nerr = xmlnode_find_child(n, "error");
	xmlnode_t *nsubject, *xitem;
	
	const char *from = jabber_attr(n->atts, "from");

	char *juid 	= jabber_unescape(from); /* was tmp */
	char *uid 	= saprintf("jid:%s", juid);

	string_t body;
	time_t sent;

	xfree(juid);

	if (nerr) {
		char *ecode = jabber_attr(nerr->atts, "code");
		char *etext = jabber_unescape(nerr->data);
		char *recipient = get_nickname(s, uid);

		if (nbody && nbody->data) {
			char *tmp2 = jabber_unescape(nbody->data);
			char *mbody = xstrndup(tmp2, 15);
			xstrtr(mbody, '\n', ' ');

			print("jabber_msg_failed_long", recipient, ecode, etext, mbody);

			xfree(mbody);
			xfree(tmp2);
		} else
			print("jabber_msg_failed", recipient, ecode, etext);

		xfree(etext);
		xfree(uid);
		return;
	} /* <error> */

	body = string_init("");

	if ((nsubject = xmlnode_find_child(n, "subject"))) {
		string_append(body, "Subject: ");
		string_append(body, nsubject->data);
		string_append(body, "\n\n");
	}

	if (nbody)
		string_append(body, nbody->data);

	if ((xitem = xmlnode_find_child(n, "x"))) {
		const char *ns = jabber_attr(xitem->atts, "xmlns");

		/* try to parse timestamp */
		sent = jabber_try_xdelay(xitem, ns);

		if (!xstrncmp(ns, "jabber:x:event", 14)) {
			int acktype = 0; /* 2 - queued ; 1 - delivered */

			/* jesli jest body, to mamy do czynienia z prosba o potwierdzenie */
			if (nbody && (xmlnode_find_child(xitem, "delivered") 
						|| xmlnode_find_child(xitem, "displayed")) ) {
				char *id = jabber_attr(n->atts, "id");
				const char *our_status = session_status_get(s);

				jabber_write(j, "<message to=\"%s\">", from);
				jabber_write(j, "<x xmlns=\"jabber:x:event\">");

				if (!xstrcmp(our_status, EKG_STATUS_INVISIBLE)) {
					jabber_write(j, "<offline/>");
				} else {
					if (xmlnode_find_child(xitem, "delivered"))
						jabber_write(j, "<delivered/>");
					if (xmlnode_find_child(xitem, "displayed"))
						jabber_write(j, "<displayed/>");
				};

				jabber_write(j, "<id>%s</id></x></message>",id);
			};

			/* je�li body nie ma, to odpowiedz na nasza prosbe */

			acktype = xmlnode_find_child(xitem, "delivered") ? 1	/* delivered */
				: xmlnode_find_child(xitem, "offline") ? 2	/* queued */
				: 0;
/* TODO: wbudowac composing w protocol-message-ack ? */
			if (!nbody && acktype) {
				char *__session = xstrdup(session_uid_get(s));
				char *__rcpt	= xstrdup(uid); /* was uid+4 */
				char *__status  = xstrdup(
						acktype == 1 ? "delivered" : 
						acktype == 2 ? "queued" : 
/*						acktype == 3 ? "?" : */
						NULL);
				char *__seq	= NULL;

				/* protocol_message_ack; sesja ; uid + 4 ; seq (NULL ? ) ; status - delivered ; queued ) */
				query_emit(NULL, "protocol-message-ack", &__session, &__rcpt, &__seq, &__status, NULL);
				
				xfree(__session);
				xfree(__rcpt);
				xfree(__status);
				/* xfree(__seq); */
			}

			if (!nbody && xmlnode_find_child(xitem, "composing")) {
				if (session_int_get(s, "show_typing_notify")) 
					print("jabber_typing_notify", uid+4);
			} /* composing */
		} /* jabber:x:event */

		if (!xstrncmp(ns, "jabber:x:oob", 12)) {
			xmlnode_t *xurl;
			xmlnode_t *xdesc;

			if ( ( xurl = xmlnode_find_child(xitem, "url") ) ) {
				string_append(body, "\n\n");
				string_append(body, "URL: ");
				string_append(body, xurl->data);
				string_append(body, "\n");
				if ((xdesc = xmlnode_find_child(xitem, "desc"))) {
					string_append(body, xdesc->data);
					string_append(body, "\n");
				}
			}
		} /* jabber:x:oob */
	} /* if !nerr && <x>; TODO: split as functions */
	else sent = time(NULL);

	if (nbody || nsubject) {
		const char *type = jabber_attr(n->atts, "type");

		char *me	= xstrdup(session_uid_get(s));
		char *text;	

		int class 	= EKG_MSGCLASS_CHAT;
		int ekgbeep 	= EKG_TRY_BEEP;
		int secure	= 0;

		char **rcpts 	= NULL;
		char *seq 	= NULL;
		uint32_t *format = NULL;

		debug("[jabber,message] type = %s\n", type);
		if (!xstrcmp(type, "groupchat")) {
			int prv = 0;
			int isour = 0;
			const char *frname = format_find(isour ? 
				prv ? "jabber_muc_send_private" : "jabber_muc_send_public" : 
				prv ? "jabber_muc_recv_private" : "jabber_muc_recv_public");
			char *temp = jabber_unescape(body->str);
			class |= EKG_NO_THEMEBIT;
			text = format_string(frname, session_name(s), uid, temp);
			xfree(temp);
		} else {
			text = jabber_unescape(body->str);
		}

		query_emit(NULL, "protocol-message", &me, &uid, &rcpts, &text, &format, &sent, &class, &seq, &ekgbeep, &secure);

		xfree(me);
		xfree(text);
/*
		array_free(rcpts);
		xfree(seq);
		xfree(format);
*/
	}
	string_free(body, 1);
	xfree(uid);
} /* */

void jabber_handle_iq(xmlnode_t *n, jabber_handler_data_t *jdh) {
	const char *type = jabber_attr(n->atts, "type");
	const char *id   = jabber_attr(n->atts, "id");
	const char *from = jabber_attr(n->atts, "from");

	session_t *s = jdh->session;
	jabber_private_t *j = jabber_private(s);

	if (!type) {
		debug("[jabber] <iq> without type!\n");
		return;
	}

	if (!xstrcmp(id, "auth")) {
		s->last_conn = time(NULL);
		j->connecting = 0;

		if (!xstrcmp(type, "result")) {
			session_connected_set(s, 1);
			session_unidle(s);
			{
				char *__session = xstrdup(session_uid_get(s));
				query_emit(NULL, "protocol-connected", &__session);
				xfree(__session);
			}
			jdh->roster_retrieved = 0;
			userlist_free(s);
			jabber_write(j, "<iq type=\"get\"><query xmlns=\"jabber:iq:roster\"/></iq>");
			jabber_write_status(s);
		} else if (!xstrcmp(type, "error")) { /* TODO: try to merge with <message>'s <error> parsing */
			xmlnode_t *e = xmlnode_find_child(n, "error");

			if (e && e->data) {
				char *data = jabber_unescape(e->data);
				print("conn_failed", data, session_name(s));
			} else
				print("jabber_generic_conn_failed", session_name(s));
		}
	}

	if (!xstrncmp(id, "passwd", 6)) {
		if (!xstrcmp(type, "result")) {
			char *new_passwd = (char *) session_get(s, "__new_password");

			session_set(s, "password", new_passwd);
			xfree(new_passwd);
			print("passwd");
		} else if (!xstrcmp(type, "error")) {
			xmlnode_t *e = xmlnode_find_child(n, "error");
			char *reason = (e && e->data) ? jabber_unescape(e->data) : NULL;

			print("passwd_failed", jabberfix(reason, "?"));
			xfree(reason);
		}
		session_set(s, "__new_password", NULL);
	}
	/* XXX: temporary hack: roster przychodzi jako typ 'set' (przy dodawaniu), jak
	        i typ "result" (przy za��daniu rostera od serwera) */
	if (!xstrncmp(type, "result", 6) || !xstrncmp(type, "set", 3)) {
		xmlnode_t *q;

		/* First we check if there is vCard... */
		if ((q = xmlnode_find_child(n, "vCard"))) {
			const char *ns = jabber_attr(q->atts, "xmlns");

			if (!xstrncmp(ns, "vcard-temp", 10)) {
				xmlnode_t *fullname = xmlnode_find_child(q, "FN");
				xmlnode_t *nickname = xmlnode_find_child(q, "NICKNAME");
				xmlnode_t *birthday = xmlnode_find_child(q, "BDAY");
				xmlnode_t *adr = xmlnode_find_child(q, "ADR");
				xmlnode_t *city = xmlnode_find_child(adr, "LOCALITY");
				xmlnode_t *desc = xmlnode_find_child(q, "DESC");

				char *from_str     = (from) ? jabber_unescape(from) : NULL;
				char *fullname_str = (fullname && fullname->data) ? jabber_unescape(fullname->data) : NULL;
				char *nickname_str = (nickname && nickname->data) ? jabber_unescape(nickname->data) : NULL;
				char *bday_str     = (birthday && birthday->data) ? jabber_unescape(birthday->data) : NULL;
				char *city_str     = (city && city->data) ? jabber_unescape(city->data) : NULL;
				char *desc_str     = (desc && desc->data) ? jabber_unescape(desc->data) : NULL;

				print("jabber_userinfo_response", 
						jabberfix(from_str, _("unknown")), 	jabberfix(fullname_str, _("unknown")),
						jabberfix(nickname_str, _("unknown")),	jabberfix(bday_str, _("unknown")),
						jabberfix(city_str, _("unknown")),	jabberfix(desc_str, _("unknown")));
				xfree(desc_str);
				xfree(city_str);
				xfree(bday_str);
				xfree(nickname_str);
				xfree(fullname_str);
				xfree(from_str);
			}
		}

		if ((q = xmlnode_find_child(n, "query"))) {
			const char *ns = jabber_attr(q->atts, "xmlns");
			if (!xstrcmp(ns, "http://jabber.org/protocol/disco#items")) {
				xmlnode_t *item;
				print("generic", "Poczatek!");
				for (item = xmlnode_find_child(q, "item"); item; item = item->next) {
					char *sdesc = jabber_unescape(jabber_attr(item->atts, "name"));
					char *sjid  = jabber_unescape(jabber_attr(item->atts, "jid"));
					print("jabber_transport_list", session_name(s), sjid, sdesc);
					xfree(sdesc);
					xfree(sjid);
				}
				print("generic", "Koniec!");
			} else if (!xstrcmp(ns, "jabber:iq:register")) {
				xmlnode_t *reg;
				xmlnode_t *xdata = NULL;
				
				char *from_str = (from) ? jabber_unescape(from) : xstrdup(_("unknown"));
				string_t str = string_init(NULL);

				for (reg = q->children; reg; reg = reg->next) {
					if (!xstrcmp(reg->name, "x") && !xstrcmp("jabber:x:data", jabber_attr(reg->atts, "xmlns"))) {
						xdata = reg->children;
						break;
					}
				}
				if (xdata) {
					while (xdata) {
						char *sname = jabber_unescape(xdata->name);
						char *sdata = jabber_unescape(xdata->data);

						if (!xstrcmp(xdata->name, "title")) {
							char *s = saprintf("%s %s", sname, sdata);
							print("generic", s);
							xfree(s);
						} else if (!xstrcmp(xdata->name, "instructions")) {
							char *s = saprintf("%s %s", sname, sdata);
							print("generic", s);
							xfree(s);
						} else if (!xstrcmp(xdata->name, "field")) {
							char *type = jabber_attr(xdata->atts, "type");				/* typ zmiennej */
							char *var  = jabber_attr(xdata->atts, "var");				/* nazwa zmiennej */
							char *label= jabber_unescape(jabber_attr(xdata->atts, "label"));	/* etykietka */
							char *poss = NULL;

							char required = 0;	/* czy wymagane */
							char *value = NULL;	/* domyslna wartosc dla tego pola... */

							xmlnode_t *child;
							char *fstr = NULL;

							if (!xstrcmp(type, "text-single"))	/* zwykly tekst */;
							if (!xstrcmp(type, "text-private")) 	/* ***** */;
							if (!xstrcmp(type, "boolean"))		/* 0/1 */
								poss = xstrdup("0/1");
							if (!xstrcmp(type, "list-single"))	/* lista mozliwosci */;

							for (child = xdata->children; child; child = child->next) {
								char *sname = jabber_unescape(child->name);
								if (!xstrcmp(sname, "required"))
									required = 1;
								else if (!xstrcmp(sname, "value")) {
									xfree(value);
									value = jabber_unescape(child->data);
								} else if (!xstrcmp(sname, "option")) { /* type == list-single */
									xmlnode_t *val = xmlnode_find_child(child, "value");
									char *jlabel = jabber_unescape(jabber_attr(child->atts, "label"));
									char *poss_ = saprintf("%s%s [%s] ", 
											poss ? poss : "", 
											val ? val->data : "",
											jlabel ? jlabel : "");
									xfree(poss);
									poss = poss_;

									xfree(jlabel);
								} else { /* debug pursuit only */
									char *sdata = jabber_unescape(child->data);
									debug("[jabber,iq] %d NOT_IMPLEMENT %s %s\n", __LINE__, sname, sdata);
									xfree(sdata);
								}
								xfree(sname);
							}
							fstr = format_string(format_find("jabber_register_param_ext"), var, value, label, 
									poss, required ? "tak" : "nie");

							string_append(str, fstr);
							if (xdata->next)
								string_append_c(str, ' ');
							xfree(fstr);

							xfree(label);
							xfree(value);
							xfree(poss);
						} else
							debug("[jabber,iq] %d NOT_IMPLEMENT %s %s\n", __LINE__, sname, sdata); /* debug only */
						xfree(sdata);
						xfree(sname);

						xdata = xdata->next;
					}
				} else {
					xmlnode_t *instr = xmlnode_find_child(q, "instructions");
					char *instr_str = (instr && instr->data) ? jabber_unescape(instr->data) : NULL;

					print("jabber_registration_instruction", session_name(s), from_str, jabberfix(instr_str, "?"));
					xfree(instr_str);

					for (reg = q->children; reg; reg = reg->next) 
						if (xstrcmp(reg->name, "instructions")) {
							char *jname = jabber_unescape(reg->name);
							char *fstr;
							if (reg->data) { /* with value */
								char *jdata = jabber_unescape(reg->data);
								fstr = format_string(format_find("jabber_register_param_value"), jname, jdata);
								xfree(jdata);
							} else {
								fstr = format_string(format_find("jabber_register_param"), jname, jname);
							}
							string_append(str, fstr);
							xfree(fstr);
							if (reg->next)
								string_append_c(str, ' ');
							xfree(jname);
						}
				}
				print("jabber_ekg2_registration_instruction", session_name(s), from_str, str->str);
				xfree(from_str);
				string_free(str, 1);
			} else if (!xstrncmp(ns, "jabber:iq:version", 17)) {
				xmlnode_t *name = xmlnode_find_child(q, "name");
				xmlnode_t *version = xmlnode_find_child(q, "version");
				xmlnode_t *os = xmlnode_find_child(q, "os");

				char *from_str = (from) ? jabber_unescape(from) : NULL;
				char *name_str = (name && name->data) ? jabber_unescape(name->data) : NULL;
				char *version_str = (version && version->data) ? jabber_unescape(version->data) : NULL;
				char *os_str = (os && os->data) ? jabber_unescape(os->data) : NULL;

				print("jabber_version_response",
						jabberfix(from_str, "unknown"), jabberfix(name_str, "unknown"), 
						jabberfix(version_str, "unknown"), jabberfix(os_str, "unknown"));
				xfree(os_str);
				xfree(version_str);
				xfree(name_str);
				xfree(from_str);
			} else if (!xstrncmp(ns, "jabber:iq:last", 14)) {
				const char *last = jabber_attr(q->atts, "seconds");
				int seconds = 0;
				char buff[21];
				char *from_str, *lastseen_str;

				seconds = atoi(last);

				/*TODO If user is online: display user's status; */

				if ((seconds>=0) && (seconds < 999 * 24 * 60 * 60  - 1) )
					/* days, hours, minutes, seconds... */
					snprintf (buff, 21, _("%03dd %02dh %02dm %02ds ago"),seconds / 86400 , \
						(seconds / 3600) % 24, (seconds / 60) % 60, seconds % 60);
				else
					strcpy (buff, _("very long ago"));

				from_str = (from) ? jabber_unescape(from) : NULL;
				lastseen_str = xstrdup(buff);

				print("jabber_lastseen_response", jabberfix(from_str, "unknown"), lastseen_str);
				xfree(lastseen_str);
				xfree(from_str);
			} else if (!xstrncmp(ns, "jabber:iq:roster", 16)) {
				xmlnode_t *item = xmlnode_find_child(q, "item");

				for (; item ; item = item->next) {
					char *uid 	= saprintf("jid:%s",jabber_attr(item->atts, "jid"));
					userlist_t *u;

					/* je�li element rostera ma subscription = remove to tak naprawde u�ytkownik jest usuwany;
					w przeciwnym wypadku - nalezy go dopisa� do userlisty; dodatkowo, jesli uzytkownika
					mamy ju� w liscie, to przyszla do nas zmiana rostera; usunmy wiec najpierw, a potem
					sprawdzmy, czy warto dodawac :) */
					if (jdh->roster_retrieved && (u = userlist_find(s, uid)) )
						userlist_remove(s, u);

					if (!xstrncmp(jabber_attr(item->atts, "subscription"), "remove", 6)) {
						/* nic nie robimy, bo juz usuniete */
					} else {
						char *nickname 	= jabber_unescape(jabber_attr(item->atts, "name"));
						xmlnode_t *group = xmlnode_find_child(item,"group");
						/* czemu sluzy dodanie usera z nickname uid jesli nie ma nickname ? */
						u = userlist_add(s, uid, nickname ? nickname : uid); 

						if (jabber_attr(item->atts, "subscription"))
							u->authtype = xstrdup(jabber_attr(item->atts, "subscription"));
						
						for (; group ; group = group->next ) {
							char *gname = jabber_unescape(group->data);
							ekg_group_add(u, gname);
							xfree(gname);
						}

						if (jdh->roster_retrieved) {
							char *ctmp = saprintf("/auth --probe %s", uid);
							command_exec(NULL, s, ctmp, 1);
							xfree(ctmp);
						}
						xfree(nickname); 
					}
					xfree(uid);
				}; /* for */
				jdh->roster_retrieved = 1;
			} /* jabber:iq:roster */
		} /* if query */
	} /* type == set */

	if (!xstrncmp(type, "get", 3)) {
		xmlnode_t *q;

		if ((q = xmlnode_find_child(n, "query"))) {
			const char *ns = jabber_attr(q->atts, "xmlns");

			if (!xstrncmp(ns, "jabber:iq:version", 17) && id && from) {
				const char *ver_os;
				char *osversion;
				const char *tmp;

				char *escaped_client_name	= jabber_escape( jabberfix((tmp = session_get(s, "ver_client_name")), DEFAULT_CLIENT_NAME) );
				char *escaped_client_version	= jabber_escape( jabberfix((tmp = session_get(s, "ver_client_version")), VERSION) );

				if (!(ver_os = session_get(s, "ver_os"))) {
					struct utsname buf;

					if (uname(&buf) != -1) {
						char *osver = saprintf("%s %s %s", buf.sysname, buf.release, buf.machine);
						osversion = jabber_escape(tmp);
						xfree(osver);
					} else {
						osversion = xstrdup("unknown"); /* uname failed and not ver_os session variable */
					}
				} else {
					osversion = jabber_escape(ver_os);	/* ver_os session variable */
				}

				jabber_write(j, "<iq to=\"%s\" type=\"result\" id=\"%s\">" 
						"<query xmlns=\"jabber:iq:version\"><name>%s</name>"
						"<version>%s</version>"
						"<os>%s</os></query></iq>", 
						from, id, 
						escaped_client_name, escaped_client_version, osversion);

				xfree(escaped_client_name);
				xfree(escaped_client_version);
				xfree(osversion);
			} /* jabber:iq:version */
		} /* if query */
	} /* type == get */
} /* iq */

void jabber_handle_presence(xmlnode_t *n, session_t *s) {
	const char *from = jabber_attr(n->atts, "from");
	const char *type = jabber_attr(n->atts, "type");
	char *jid, *uid;
	xmlnode_t *q;
	int ismuc = 0;

	if (from && !xstrcmp(type, "subscribe")) {
		print("jabber_auth_subscribe", from, session_name(s));
		return;
	}

	jid = jabber_unescape(from);

	if (from && !xstrcmp(type, "unsubscribe")) {
		print("jabber_auth_unsubscribe", jid, session_name(s));
		xfree(jid);
		return;
	}
	uid = saprintf("jid:%s", jid);

	for (q = n->children; q; q = q->next) {
		char *tmp	= xstrrchr(uid, '/');
		char *mucuid	= xstrndup(uid, tmp ? tmp - uid : xstrlen(uid));
		if (!xstrcmp(q->name, "x") && !xstrcmp(jabber_attr(q->atts, "xmlns"), "http://jabber.org/protocol/muc#user")) {
			xmlnode_t *child;
			ismuc = 1;

			for (child = q->children; child; child = child->next) {
				if (!xstrcmp(child->name, "item")) { /* lista userow */
					char *jid	  = jabber_unescape(jabber_attr(child->atts, "jid"));		/* jid */
					char *role	  = jabber_unescape(jabber_attr(child->atts, "role"));		/* ? */
					char *affiliation = jabber_unescape(jabber_attr(child->atts, "affiliation"));	/* ? */

					char *uid; 
					window_t *w;
					userlist_t *ulist;

					if (!(w = window_find_s(s, mucuid))) /* co robimy jak okno == NULL ? */
						w = window_new(mucuid, s, 0); /* tworzymy ? */
					uid = saprintf("jid:%s", jid);
					ulist = userlist_add_u(&(w->userlist), uid, jid);
					xfree(uid);

					xfree(jid); xfree(role); xfree(affiliation);
				} else { /* debug pursuit only */
					char *s = saprintf("\tMUC: %s", child->name);
					print("generic", s);
					xfree(s);
				}
			}
		}
		xfree(mucuid);
	}
	if (ismuc) {
		/* presence type = "unavalible" -> part...
		 * 	 we recv join as item... look upper.. */
		xfree(jid);
		xfree(uid);
		return;
	}

	if (!type || ( !xstrcmp(type, "unavailable") || !xstrcmp(type, "error") || !xstrcmp(type, "available"))) {
		xmlnode_t *nshow, *nstatus, *nerr;
		char *status = NULL, *descr = NULL;
		char *jstatus = NULL;
		char *tmp2;

		if ((nshow = xmlnode_find_child(n, "show"))) {	/* typ */
			jstatus = jabber_unescape(nshow->data);
			if (!xstrcmp(jstatus, "na") || !xstrcmp(type, "unavailable")) {
				status = xstrdup(EKG_STATUS_NA);
			}
		} else {
			status = xstrdup(EKG_STATUS_AVAIL);
		}

		if ((nerr = xmlnode_find_child(n, "error"))) { /* bledny */
			char *ecode = jabber_attr(nerr->atts, "code");
			char *etext = jabber_unescape(nerr->data);
			xfree(status);
			status = xstrdup(EKG_STATUS_ERROR);
			descr = saprintf("(%s) %s", ecode, etext);
			xfree(etext);
		} 
		if ((nstatus = xmlnode_find_child(n, "status"))) { /* opisowy */
			xfree(descr);
			descr = jabber_unescape(nstatus->data);
		}

		if ((tmp2 = xstrchr(uid, '/'))) {
			char *tmp = xstrndup(uid, tmp2-uid);
			userlist_t *ut;
			if ((ut = userlist_find(s, tmp)))
				ut->resource = xstrdup(tmp2+1);
			xfree(tmp);
		}
		if (status) {
			xfree(jstatus);
		} else if (jstatus && (!xstrcasecmp(jstatus, EKG_STATUS_AWAY)		|| !xstrcasecmp(jstatus, EKG_STATUS_INVISIBLE)	||
					!xstrcasecmp(jstatus, EKG_STATUS_XA)		|| !xstrcasecmp(jstatus, EKG_STATUS_DND) 	|| 
					!xstrcasecmp(jstatus, EKG_STATUS_FREE_FOR_CHAT) || !xstrcasecmp(jstatus, EKG_STATUS_BLOCKED))) {
			status = jstatus;
		} else {
			debug("[jabber] Unknown presence: %s from %s. Please report!\n", jstatus, uid);
			xfree(jstatus);
			status = xstrdup(EKG_STATUS_AVAIL);
		}
		
		{
			char *session 	= xstrdup(session_uid_get(s));
			time_t when 	= jabber_try_xdelay(q, NULL);
			char *host 	= NULL;
			int port 	= 0;

			query_emit(NULL, "protocol-status", &session, &uid, &status, &descr, &host, &port, &when, NULL);
			
			xfree(session);
/*			xfree(host); */
		}
		xfree(status);
		xfree(descr);
	}
	xfree(uid);
	xfree(jid);
} /* <presence> */

time_t jabber_try_xdelay(xmlnode_t *xitem, const char *ns_)
{
        if (xitem) {
		const char *ns, *stamp;

		ns = ns_ ? ns_ : jabber_attr(xitem->atts, "xmlns");
		stamp = jabber_attr(xitem->atts, "stamp");

		if (stamp && !xstrncmp(ns, "jabber:x:delay", 14)) {
	        	struct tm tm;
        	        memset(&tm, 0, sizeof(tm));
                	sscanf(stamp, "%4d%2d%2dT%2d:%2d:%2d",
	                        &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                        	&tm.tm_hour, &tm.tm_min, &tm.tm_sec);
        	        tm.tm_year -= 1900;
                	tm.tm_mon -= 1;
	                return mktime(&tm);
		}
        }
	return time(NULL);

}

void jabber_handle_disconnect(session_t *s, const char *reason, int type)
{
        jabber_private_t *j = jabber_private(s);

        if (!j)
                return;

        if (j->obuf || j->connecting)
                watch_remove(&jabber_plugin, j->fd, WATCH_WRITE);

        if (j->obuf) {
                xfree(j->obuf);
                j->obuf = NULL;
                j->obuf_len = 0;
        }

#ifdef HAVE_GNUTLS
        if (j->using_ssl && j->ssl_session)
                gnutls_bye(j->ssl_session, GNUTLS_SHUT_RDWR);
#endif
        session_connected_set(s, 0);
        j->connecting = 0;
        if (j->parser)
                XML_ParserFree(j->parser);
        j->parser = NULL;
        close(j->fd);
        j->fd = -1;

#ifdef HAVE_GNUTLS
        if (j->using_ssl && j->ssl_session)
                gnutls_deinit(j->ssl_session);
#endif

        userlist_clear_status(s, NULL);
	{
		char *__session = xstrdup(session_uid_get(s));
		char *__reason = xstrdup(reason);
		
		query_emit(NULL, "protocol-disconnected", &__session, &__reason, &type, NULL);

		xfree(__session);
		xfree(__reason);
	}

}

static void jabber_handle_start(void *data, const char *name, const char **atts)
{
        jabber_handler_data_t *jdh = (jabber_handler_data_t*) data;
        jabber_private_t *j = session_private_get(jdh->session);
        session_t *s = jdh->session;

        if (!xstrcmp(name, "stream:stream")) {
                char *password = (char *) session_get(s, "password");
                char *resource = jabber_escape(session_get(s, "resource"));
                const char *uid = session_uid_get(s);
                char *username;
		char *authpass;

                username = xstrdup(uid + 4);
                *(xstrchr(username, '@')) = 0;

                if (!resource)
                        resource = xstrdup(JABBER_DEFAULT_RESOURCE);

                j->stream_id = xstrdup(jabber_attr((char **) atts, "id"));

		authpass = (session_int_get(s, "plaintext_passwd")) ? 
			saprintf("<password>%s</password>", password) :  				/* plaintext */
			saprintf("<digest>%s</digest>", jabber_digest(j->stream_id, password));		/* hash */
		jabber_write(j, "<iq type=\"set\" id=\"auth\" to=\"%s\"><query xmlns=\"jabber:iq:auth\"><username>%s</username>%s<resource>%s</resource></query></iq>", j->server, username, authpass, resource);
                xfree(username);
		xfree(resource);
		xfree(authpass);
        } else
		xmlnode_handle_start(data, name, atts);
}

WATCHER(jabber_handle_stream)
{
        jabber_handler_data_t *jdh = (jabber_handler_data_t*) data;
        session_t *s = (session_t*) jdh->session;
        jabber_private_t *j = session_private_get(s);
        char *buf;
        int len;

        s->activity = time(NULL);

        /* ojej, roz��czy�o nas */
        if (type == 1) {
                debug("[jabber] jabber_handle_stream() type == 1, exitting\n");
		/* todo, xfree data ? */
		if (s && session_connected_get(s))  /* hack to avoid reconnecting when we do /disconnect */
			jabber_handle_disconnect(s, NULL, EKG_DISCONNECT_NETWORK);
                return;
        }

        debug("[jabber] jabber_handle_stream()\n");

        if (!(buf = XML_GetBuffer(j->parser, 4096))) {
                print("generic_error", "XML_GetBuffer failed");
                goto fail;
        }

#ifdef HAVE_GNUTLS
        if (j->using_ssl && j->ssl_session) {
		len = gnutls_record_recv(j->ssl_session, buf, 4095);

		if ((len == GNUTLS_E_INTERRUPTED) || (len == GNUTLS_E_AGAIN)) {
			// will be called again
			ekg_yield_cpu();
			return;
		}

                if (len < 0) {
                        print("generic_error", gnutls_strerror(len));
                        goto fail;
                }
        } else
#endif
                if ((len = read(fd, buf, 4095)) < 1) {
                        print("generic_error", strerror(errno));
                        goto fail;
                }

        buf[len] = 0;

	debug("[jabber] recv %s\n", buf);

        if (!XML_ParseBuffer(j->parser, len, (len == 0))) {
                print("jabber_xmlerror", session_name(s));
                goto fail;
        }

        return;

fail:
        watch_remove(&jabber_plugin, fd, WATCH_READ);
}

WATCHER(jabber_handle_connect)
{
        jabber_handler_data_t *jdh = (jabber_handler_data_t*) data;
        int res = 0, res_size = sizeof(res);
        jabber_private_t *j = session_private_get(jdh->session);

        debug("[jabber] jabber_handle_connect()\n");

        if (type != 0) {
                return;
        }

        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &res, &res_size) || res) {
                jabber_handle_disconnect(jdh->session, strerror(res), EKG_DISCONNECT_FAILURE);
		xfree(data);
                return;
        }

        watch_add(&jabber_plugin, fd, WATCH_READ, 1, jabber_handle_stream, jdh);

        jabber_write(j, "<?xml version=\"1.0\" encoding=\"utf-8\"?><stream:stream to=\"%s\" xmlns=\"jabber:client\" xmlns:stream=\"http://etherx.jabber.org/streams\">", j->server);

        j->id = 1;
        j->parser = XML_ParserCreate("UTF-8");
        XML_SetUserData(j->parser, (void*)data);
        XML_SetElementHandler(j->parser, (XML_StartElementHandler) jabber_handle_start, (XML_EndElementHandler) xmlnode_handle_end);
        XML_SetCharacterDataHandler(j->parser, (XML_CharacterDataHandler) xmlnode_handle_cdata);
}

WATCHER(jabber_handle_resolver)
{
	jabber_handler_data_t *jdh = (jabber_handler_data_t*) data;
	session_t *s = jdh->session;
	jabber_private_t *j = jabber_private(s);
	struct in_addr a;
	int one = 1, res;
	struct sockaddr_in sin;
	const int port = session_int_get(s, "port");
#ifdef HAVE_GNUTLS
	/* Allow connections to servers that have OpenPGP keys as well. */
	const int cert_type_priority[3] = {GNUTLS_CRT_X509, GNUTLS_CRT_OPENPGP, 0};
	const int comp_type_priority[3] = {GNUTLS_COMP_ZLIB, GNUTLS_COMP_NULL, 0};
	int ssl_port = session_int_get(s, "ssl_port");
	int use_ssl = session_int_get(s, "use_ssl");
#endif
        if (type) {
                return;
	}

        debug("[jabber] jabber_handle_resolver()\n", type);

        if ((res = read(fd, &a, sizeof(a))) != sizeof(a) || (res && a.s_addr == INADDR_NONE /* INADDR_NONE kiedy NXDOMAIN */)) {
                if (res == -1)
                        debug("[jabber] unable to read data from resolver: %s\n", strerror(errno));
                else
                        debug("[jabber] read %d bytes from resolver. not good\n", res);
                close(fd);
                print("conn_failed", format_find("conn_failed_resolving"), session_name(jdh->session));
                /* no point in reconnecting by jabber_handle_disconnect() */
                j->connecting = 0;
                return;
        }

        debug("[jabber] resolved to %s\n", inet_ntoa(a));

        close(fd);

        if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
                debug("[jabber] socket() failed: %s\n", strerror(errno));
                jabber_handle_disconnect(jdh->session, strerror(errno), EKG_DISCONNECT_FAILURE);
                return;
        }

        debug("[jabber] socket() = %d\n", fd);

        j->fd = fd;

        if (ioctl(fd, FIONBIO, &one) == -1) {
                debug("[jabber] ioctl() failed: %s\n", strerror(errno));
                jabber_handle_disconnect(jdh->session, strerror(errno), EKG_DISCONNECT_FAILURE);
                return;
        }

        /* failure here isn't fatal, don't bother with checking return code */
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = a.s_addr;
#ifdef HAVE_GNUTLS
	j->using_ssl = 0;
        if (use_ssl)
                j->port = ssl_port < 1 ? 5223 : ssl_port;
        else
#endif
		j->port = port < 1 ? 5222 : port;
	sin.sin_port = htons(j->port);

        debug("[jabber] connecting to %s:%d\n", inet_ntoa(sin.sin_addr), j->port);

        res = connect(fd, (struct sockaddr*) &sin, sizeof(sin));

        if (res == -1 && errno != EINPROGRESS) {
                debug("[jabber] connect() failed: %s (errno=%d)\n", strerror(errno), errno);
                jabber_handle_disconnect(jdh->session, strerror(errno), EKG_DISCONNECT_FAILURE);
                return;
        }

#ifdef HAVE_GNUTLS
        if (use_ssl) {
		int ret = 0;
                gnutls_certificate_allocate_credentials(&(j->xcred));
                /* XXX - ~/.ekg/certs/server.pem */
                gnutls_certificate_set_x509_trust_file(j->xcred, "brak", GNUTLS_X509_FMT_PEM);

		if ((ret = gnutls_init(&(j->ssl_session), GNUTLS_CLIENT)) ) {
			print("conn_failed_tls");
			jabber_handle_disconnect(jdh->session, gnutls_strerror(ret), EKG_DISCONNECT_FAILURE);
			return;
		}

		gnutls_set_default_priority(j->ssl_session);
                gnutls_certificate_type_set_priority(j->ssl_session, cert_type_priority);
                gnutls_credentials_set(j->ssl_session, GNUTLS_CRD_CERTIFICATE, j->xcred);
                gnutls_compression_set_priority(j->ssl_session, comp_type_priority);

                /* we use read/write instead of recv/send */
                gnutls_transport_set_pull_function(j->ssl_session, (gnutls_pull_func)read);
                gnutls_transport_set_push_function(j->ssl_session, (gnutls_push_func)write);
                gnutls_transport_set_ptr(j->ssl_session, (gnutls_transport_ptr)(j->fd));

		watch_add(&jabber_plugin, fd, WATCH_WRITE, 0, jabber_handle_connect_tls, jdh);

		return;
        } // use_ssl
#endif
        watch_add(&jabber_plugin, fd, WATCH_WRITE, 0, jabber_handle_connect, jdh);
}

#ifdef HAVE_GNUTLS
WATCHER(jabber_handle_connect_tls)
{
        jabber_handler_data_t *jdh = (jabber_handler_data_t*) data;
        jabber_private_t *j = session_private_get(jdh->session);
	int ret;

	if (type) {
		return;
	}

	ret = gnutls_handshake(j->ssl_session);

	debug("[jabber] jabber_handle_connect_tls, handshake = %d\n", ret);
	if ((ret == GNUTLS_E_INTERRUPTED) || (ret == GNUTLS_E_AGAIN)) {
		debug("tls handler going to be set, way %d\n",
			gnutls_record_get_direction(j->ssl_session));

		watch_add(&jabber_plugin, (int) gnutls_transport_get_ptr(j->ssl_session),
			gnutls_record_get_direction(j->ssl_session) ? WATCH_WRITE : WATCH_READ,
			0, jabber_handle_connect_tls, jdh);

		ekg_yield_cpu();
		return;

	} else if (ret < 0) {
		debug("[jabber] ssl handshake failed: %d - %s\n", ret, gnutls_strerror(ret));
		gnutls_deinit(j->ssl_session);
		gnutls_certificate_free_credentials(j->xcred);
		jabber_handle_disconnect(jdh->session, gnutls_strerror(ret), EKG_DISCONNECT_FAILURE);
		return;
	}
	debug("tls handshake OK, ret %d\n", ret);

	// handshake successful
	j->using_ssl = 1;

	watch_add(&jabber_plugin, fd, WATCH_WRITE, 0, jabber_handle_connect, jdh);
}
#endif

QUERY(jabber_status_show_handle)
{
        char *uid	= *(va_arg(ap, char**));
        session_t *s	= session_find(uid);
        jabber_private_t *j = session_private_get(s);
        const char *resource = session_get(s, "resource");
        userlist_t *u;
        char *fulluid;
        char *tmp;

        if (!s || !j)
                return -1;

        fulluid = saprintf("%s/%s", uid, (resource ? resource : JABBER_DEFAULT_RESOURCE));

        // nasz stan
	if ((u = userlist_find(s, uid)) && u->nickname)
		print("show_status_uid_nick", fulluid, u->nickname);
	else
		print("show_status_uid", fulluid);

	xfree(fulluid);

        // nasz status
	tmp = (s->connected) ? 
		format_string(format_find(ekg_status_label(s->status, s->descr, "show_status_")),s->descr, "") :
		format_string(format_find("show_status_notavail"));

	print("show_status_status_simple", tmp);
	xfree(tmp);

        // serwer
	print(
#ifdef HAVE_GNUTLS
			j->using_ssl ? "show_status_server_tls" :
#endif
			"show_status_server", j->server, itoa(j->port));
        if (j->connecting)
                print("show_status_connecting");
	
	{
		// kiedy ostatnie polaczenie/rozlaczenie
        	time_t n = time(NULL);
        	struct tm *t = localtime(&n);
		int now_days = t->tm_yday;
		char buf[100];
		const char *format;

		t = localtime(&s->last_conn);
		format = format_find((t->tm_yday == now_days) ? "show_status_last_conn_event_today" : "show_status_last_conn_event");
		if (!strftime(buf, sizeof(buf), format, t) && xstrlen(format)>0)
			xstrcpy(buf, "TOOLONG");

		print( (s->connected) ? "show_status_connected_since" : "show_status_disconnected_since", buf);
	}
	return 0;
}

static int jabber_theme_init()
{
	format_add("jabber_auth_subscribe", _("%> (%2) %T%1%n asks for authorisation. Use \"/auth -a %1\" to accept, \"/auth -d %1\" to refuse.%n\n"), 1);
	format_add("jabber_auth_unsubscribe", _("%> (%2) %T%1%n asks for removal. Use \"/auth -d %1\" to delete.%n\n"), 1);
	format_add("jabber_xmlerror", _("%! (%1) Error parsing XML%n\n"), 1);
	format_add("jabber_auth_request", _("%> (%2) Sent authorisation request to %T%1%n.\n"), 1);
	format_add("jabber_auth_accept", _("%> (%2) Authorised %T%1%n.\n"), 1);
	format_add("jabber_auth_unsubscribed", _("%> (%2) Asked %T%1%n to remove authorisation.\n"), 1);
	format_add("jabber_auth_cancel", _("%> (%2) Authorisation for %T%1%n revoked.\n"), 1);
	format_add("jabber_auth_denied", _("%> (%2) Authorisation for %T%1%n denied.\n"), 1);
	format_add("jabber_auth_probe", _("%> (%2) Sent presence probe to %T%1%n.\n"), 1);
	format_add("jabber_generic_conn_failed", _("%! (%1) Error connecting to Jabber server%n\n"), 1);
	format_add("jabber_msg_failed", _("%! Message to %T%1%n can't be delivered: %R(%2) %r%3%n\n"),1);
	format_add("jabber_msg_failed_long", _("%! Message to %T%1%n %y(%n%K%4(...)%y)%n can't be delivered: %R(%2) %r%3%n\n"),1);
	format_add("jabber_version_response", _("%> Jabber ID: %T%1%n\n%> Client name: %T%2%n\n%> Client version: %T%3%n\n%> Operating system: %T%4%n\n"), 1);
	format_add("jabber_userinfo_response", _("%> Jabber ID: %T%1%n\n%> Full Name: %T%2%n\n%> Nickname: %T%3%n\n%> Birthday: %T%4%n\n%> City: %T%5%n\n%> Desc: %T%6%n\n"), 1);
	format_add("jabber_lastseen_response", _("%> Jabber ID:  %T%1%n\n%> Logged out: %T%2%n\n"), 1);
	format_add("jabber_unknown_resource", _("%! (%1) User's resource unknown%n\n\n"), 1);
	format_add("jabber_status_notavail", _("%! (%1) Unable to check version, because %2 is unavailable%n\n"), 1);
	format_add("jabber_typing_notify", _("%> %T%1%n is typing to us ...%n\n"), 1);
	format_add("jabber_charset_init_error", _("%! Error initialising charset conversion (%1->%2): %3"), 1);

	format_add("jabber_transport_list", _("%> (%1) JID: %2 descr=%3"), 1);
	format_add("jabber_registration_instruction", _("%> (%1,%2) instr=%3"), 1);
	format_add("jabber_ekg2_registration_instruction", _("%> (%1) type \"/register %2 %3\" to register"), 1);

	format_add("jabber_register_param_ext", "\n--%1 %2 [%3, opcje: %4, wymagane: %5]", 1);
	format_add("jabber_register_param_descr", "--%1 %2 [%3]", 1);
	format_add("jabber_register_param_value", "--%1 %2", 1);
	format_add("jabber_register_param", "--%1 [%2]", 1);

	format_add("jabber_muc_send_public", _("<%2> %3"), 1);
	format_add("jabber_muc_send_private", _("<%2> %3"), 1);
	format_add("jabber_muc_recv_public", _("<%2> %3"), 1);
	format_add("jabber_muc_recv_private", _("<%2> %3"), 1);
	
        return 0;
}

int jabber_plugin_init(int prio)
{
        plugin_register(&jabber_plugin, prio);

        query_connect(&jabber_plugin, "protocol-validate-uid", jabber_validate_uid, NULL);
        query_connect(&jabber_plugin, "plugin-print-version", jabber_print_version, NULL);
        query_connect(&jabber_plugin, "session-added", jabber_session, (void*) 1);
        query_connect(&jabber_plugin, "session-removed", jabber_session, (void*) 0);
        query_connect(&jabber_plugin, "status-show", jabber_status_show_handle, NULL);

        jabber_register_commands();

        plugin_var_add(&jabber_plugin, "alias", VAR_STR, 0, 0, NULL);
        plugin_var_add(&jabber_plugin, "auto_away", VAR_INT, "0", 0, NULL);
        plugin_var_add(&jabber_plugin, "auto_back", VAR_INT, "0", 0, NULL);
        plugin_var_add(&jabber_plugin, "auto_connect", VAR_INT, "0", 0, NULL);
        plugin_var_add(&jabber_plugin, "auto_find", VAR_INT, "0", 0, NULL);
        plugin_var_add(&jabber_plugin, "auto_reconnect", VAR_INT, "0", 0, NULL);
        plugin_var_add(&jabber_plugin, "display_notify", VAR_INT, "0", 0, NULL);
        plugin_var_add(&jabber_plugin, "log_formats", VAR_STR, "xml,simple", 0, NULL);
        plugin_var_add(&jabber_plugin, "password", VAR_STR, "foo", 1, NULL);
        plugin_var_add(&jabber_plugin, "plaintext_passwd", VAR_INT, "0", 0, NULL);
        plugin_var_add(&jabber_plugin, "port", VAR_INT, "5222", 0, NULL);
        plugin_var_add(&jabber_plugin, "priority", VAR_INT, "5", 0, NULL);
        plugin_var_add(&jabber_plugin, "resource", VAR_STR, 0, 0, NULL);
        plugin_var_add(&jabber_plugin, "server", VAR_STR, 0, 0, NULL);
        plugin_var_add(&jabber_plugin, "ssl_port", VAR_INT, "5223", 0, NULL);
        plugin_var_add(&jabber_plugin, "show_typing_notify", VAR_INT, "1", 0, NULL);
        plugin_var_add(&jabber_plugin, "use_ssl", VAR_INT, "0", 0, NULL);
        plugin_var_add(&jabber_plugin, "ver_client_name", VAR_STR, 0, 0, NULL);
        plugin_var_add(&jabber_plugin, "ver_client_version", VAR_STR, 0, 0, NULL);
        plugin_var_add(&jabber_plugin, "ver_os", VAR_STR, 0, 0, NULL);

	config_jabber_console_charset = xstrdup("iso-8859-2");
	variable_add(&jabber_plugin, "console_charset", VAR_STR, 1, &config_jabber_console_charset, NULL, NULL, NULL);
#ifdef HAVE_GNUTLS
        gnutls_global_init();
#endif
        return 0;
}

static int jabber_plugin_destroy()
{
        list_t l;

#ifdef HAVE_GNUTLS
        gnutls_global_deinit();
#endif

        for (l = sessions; l; l = l->next)
                jabber_private_destroy((session_t*) l->data);

        plugin_unregister(&jabber_plugin);

        return 0;
}

/*
 * Local Variables:
 * mode: c
 * c-file-style: "k&r"
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 * vim: sts=8 sw=8
 */
