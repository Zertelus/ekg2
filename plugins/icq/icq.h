#ifndef __ICQ_ICQ_H
#define __ICQ_ICQ_H

#define SNAC_HANDLER(x) int x(session_t *s, guint16 cmd, unsigned char *buf, int len, private_data_t *data)
typedef int (*snac_handler_t) (session_t *, guint16 cmd, unsigned char *, int, private_data_t * );

#define SNAC_SUBHANDLER(x) int x(session_t *s, unsigned char *buf, int len, private_data_t *data)
typedef int (*snac_subhandler_t) (session_t *s, unsigned char *, int, private_data_t * );

typedef struct {
	int win_size;		// Window size
	int clear_lvl;		// Clear level
	int alert_lvl;		// Alert level
	int limit_lvl;		// Limit level
	int discn_lvl;		// Disconnect level
	int curr_lvl;		// Current level
	int max_lvl;		// Max level
	time_t last_time;	// Last time
	int n_groups;
	guint32 *groups;
} icq_rate_t;

typedef struct icq_snac_reference_list_s {
	struct icq_snac_reference_list_s *next;
	int ref;
	time_t timestamp;
	snac_subhandler_t subhandler;
	private_data_t *list;
} icq_snac_reference_list_t;

typedef struct {
	GCancellable *connect_cancellable;
	GDataOutputStream *send_stream;

	int flap_seq;		/* FLAP seq id */
	guint16 snac_seq;	/* SNAC seq id */
	int snacmeta_seq;	/* META SNAC seq id */
	int cookie_seq;		/* Cookie seq id */

	int ssi;		/* server-side-userlist? */
	int migrate;		/* client migration sequence */
	int aim;		/* aim-ok? */
	int default_group_id;	/* XXX ?wo? TEMP! We should support list of groups */
	int status_flags;
	int xstatus;		/* XXX ?wo? set it! */
	private_data_t *whoami;
	char *default_group_name;
	GString *cookie;	/* connection login cookie */
	GString *stream_buf;
	icq_snac_reference_list_t *snac_ref_list;
	int n_rates;
	icq_rate_t **rates;
} icq_private_t;

int icq_send_pkt(session_t *s, GString *buf);

void icq_session_connected(session_t *s);
int icq_write_status(session_t *s);
void icq_handle_disconnect(session_t *s, const char *reason, int type);

void icq_connect(session_t *session, const char *server, int port);

#define icq_uid(target) protocol_uid("icq", target)

#define MIRANDAOK 1
#define MIRANDA_COMPILANT_CLIENT 1

#define ICQ_DEBUG_UNUSED_INFORMATIONS 1

#endif
