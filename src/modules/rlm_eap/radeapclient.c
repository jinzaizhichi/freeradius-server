/*
 * radeapclient.c	EAP specific radius packet debug tool.
 *
 * Version:	$Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2000,2006  The FreeRADIUS server project
 * Copyright 2000  Miquel van Smoorenburg <miquels@cistron.nl>
 * Copyright 2000  Alan DeKok <aland@ox.org>
 */

RCSID("$Id$")

#include <freeradius-devel/libradius.h>

#include <ctype.h>
#include <assert.h>

#if HAVE_GETOPT_H
#  include <getopt.h>
#endif

#include <freeradius-devel/conf.h>
#include <freeradius-devel/md5.h>

#include "eap_types.h"
#include "eap_sim.h"

#ifdef WITH_TLS
#  include <freeradius-devel/tls.h>
#endif

extern int sha1_data_problems;

#define USEC 1000000


/*
 *  Global variables.
 */
static char const *progname = "radeapclient";
main_config_t main_config;

char const *radiusd_version = "FreeRADIUS Version " RADIUSD_VERSION_STRING
#ifdef RADIUSD_VERSION_COMMIT
" (git #" STRINGIFY(RADIUSD_VERSION_COMMIT) ")"
#endif
", built on " __DATE__ " at " __TIME__;


static uint32_t parallel = 1;
static uint32_t rate_limit = 0;
static unsigned int retries = 3;
static float timeout = 5;
static struct timeval tv_timeout;
static unsigned int recycle_count = 1;
static char const *secret = NULL;
static bool do_output = true;
static bool do_summary = false;
static char filesecret[256];
static float progress_interval = 0;
static struct timeval tv_progress_interval;
static char const *radius_dir = NULL;

//TODO: move structures to a header file.

typedef struct rc_input_vps_list rc_input_vps_list_t;
typedef struct rc_input_vps rc_input_vps_t;
typedef struct rc_transaction rc_transaction_t;

/** Structure which contains EAP context, necessary to perform the full EAP transaction.
 */
typedef struct rc_eap_sim_context {
	struct eap_sim_keys keys;
} rc_eap_sim_context_t;

typedef struct rc_eap_md5_context {
	int tried;
} rc_eap_md5_context_t;

typedef struct rc_eap_context {
	int eap_type;	//!< contains the EAP-Type
	char password[256];	//!< copy of User-Password (or CHAP-Password).
	union {
		rc_eap_sim_context_t sim;
		rc_eap_md5_context_t md5;
	} eap;
} rc_eap_context_t;


/** Structure which holds a list of available input vps.
 */
struct rc_input_vps_list {
	rc_input_vps_t *head;
	rc_input_vps_t *tail;
	uint32_t size;
};

/** Structure which holds an input vps entry (read from file or stdin),
 *  and linkage to previous / next entries.
 */
struct rc_input_vps {
	uint32_t num;	//!< The number (within the file) of the input we're reading.

	VALUE_PAIR *vps_in;	//!< the list of attribute/value pairs.

	rc_input_vps_list_t *list;	//!< the list to which this entry belongs (NULL for an unchained entry).

	uint32_t recycle;	//!< number of times this input has been used to start a transaction.

	rc_input_vps_t *prev;
	rc_input_vps_t *next;
};


/** Structure which holds a transaction: sent packet, reply received...
 */
struct rc_transaction {
	uint32_t id;	//!< id of transaction (0 for the first one).

	uint32_t num_packet;	//!< number of packets sent for this transaction.

	struct timeval timestamp;	//!< when the transaction is started.
	RADIUS_PACKET *packet;
	RADIUS_PACKET *reply;

	rc_input_vps_t *input_vps;

	rc_eap_context_t *eap_context;

	uint32_t tries;

	fr_event_t *event;	//!< armed event (if any).

	char		password[256];
	char const	*name;	//!< Test name (as specified in the request).
};


/** Define workflow types (transactions for which we got a response)
 */
typedef enum {
	RC_WF_ALL = 0,
	RC_WF_ACCESS_REQUEST_ACCEPT,
	RC_WF_COA_REQUEST_ACK,
	RC_WF_EAP_REQUEST_SUCCESS,
	RC_WF_ACCOUNTING_REQUEST_RESPONSE,
	RC_WF_MAX
} rc_wf_type_t;

#define LG_PAD_STATS    20
#define LG_PAD_WF_TYPES 25

static char const *rc_wf_types[RC_WF_MAX] = {
	"(All)",
	"Access-Request - Accept",
	"CoA-Request - Ack",
	"EAP Request - Success",
	"Accounting-Request - Response"
};

/** Structure which holds per-workflow statistics information
 */
typedef struct rc_wf_stats {
	uint32_t       num;
	struct timeval tv_rtt_cumul;
	struct timeval tv_rtt_min;
	struct timeval tv_rtt_max;
} rc_wf_stats_t;

/** Structure which holds global statistics information
 */
typedef struct rc_stats {
	uint32_t nb_started;			//!< number of transactions started
	uint32_t nb_eap;				//!< number of EAP transactions started
	uint32_t nb_success;			//!< number of successful transactions
	uint32_t nb_fail;				//!< number of failed transactions
	uint32_t nb_lost;				//!< number of packets to which we received no response
	uint32_t nb_packets_sent;		//!< number of packets sent (including retransmissions)
	uint32_t nb_packets_retries;	//!< number of packets retransmissions
	uint32_t nb_packets_recv;		//!< number of packets received

	rc_wf_stats_t wf_stats[RC_WF_MAX];

} rc_stats_t;

#define STATS_INC(_stat_type) { \
	stats._stat_type ++; \
}


static TALLOC_CTX *autofree;
static rc_stats_t stats;
static struct timeval tv_start;
static struct timeval tv_end;
static uint32_t num_input = 0; //!< number of input entries loaded.
static uint32_t num_trans = 0; //!< number of transactions initialized.
static uint32_t num_started = 0; //!< number of transactions started.
static uint32_t num_ongoing = 0; //!< number of ongoing transactions.
static uint32_t num_finished = 0; //!< number of finished transactions.

static rc_input_vps_list_t rc_vps_list_in; //!< list of available input vps entries.
static fr_packet_list_t *pl = NULL;	//!< list of outgoing packets.
static unsigned int num_sockets = 0;	//!< number of allocated sockets.
static fr_event_list_t *ev_list = NULL; //!< list of armed events.
static char ch_elapsed[12+1];
#define ELAPSED rc_print_elapsed(ch_elapsed, 3)

static int force_af = AF_UNSPEC;
static int ipproto = IPPROTO_UDP;
static fr_ipaddr_t server_ipaddr;
static bool server_addr_init = false;
static uint16_t server_port = 0;
static int packet_code = PW_CODE_UNDEFINED;


static int rc_map_eap_methods(RADIUS_PACKET *req);
static void rc_unmap_eap_methods(RADIUS_PACKET *rep);
static int rc_map_eap_sim_types(RADIUS_PACKET *r);
static int rc_unmap_eap_sim_types(RADIUS_PACKET *r);

static void rc_get_radius_port(PW_CODE type, uint16_t *port);
static void rc_evprep_packet_timeout(rc_transaction_t *trans);
static void rc_evprep_progress_stat(void);
static void rc_deallocate_id(rc_transaction_t *trans);
static void rc_wf_stat_update(rc_transaction_t *trans, rc_wf_type_t wf_type);
static void rc_do_progress_stat(void);
static uint32_t rc_get_elapsed(void);
static float rc_get_wf_rate(rc_wf_type_t i);



/** Display usage and exit.
 */
static void NEVER_RETURNS usage(void)
{
	fprintf(stdout, "Usage: radeapclient [options] server[:port] <command> [<secret>]\n");

	fprintf(stdout, "  <command>              One of auth, acct, status, coa, disconnect or auto.\n");
	fprintf(stdout, "  -4                     Use IPv4 address of server\n");
	fprintf(stdout, "  -6                     Use IPv6 address of server.\n");
	fprintf(stdout, "  -c <count>             Send each packet 'count' times.\n");
	fprintf(stdout, "  -d <raddb>             Set user dictionary directory (defaults to " RADDBDIR ").\n");
	fprintf(stdout, "  -D <dictdir>           Set main dictionary directory (defaults to " DICTDIR ").\n");
	fprintf(stdout, "  -f <file>              Read packets from file, not stdin.\n");
	fprintf(stdout, "  -h                     Print usage help information.\n");
	fprintf(stdout, "  -n <num>               Rate limit. Send at most N requests/s.\n");
	fprintf(stdout, "  -o <time>              Output progress statistics each 'time' seconds.\n");
	fprintf(stdout, "  -p <num>               Send 'num' packets in parallel.\n");
	fprintf(stdout, "  -q                     Do not print anything out.\n");
	fprintf(stdout, "  -r <retries>           If timeout, retry sending the packet 'retries' times.\n");
	fprintf(stdout, "  -s                     Print out summary statistics information.\n");
	fprintf(stdout, "  -S <file>              read secret from file, not command line.\n");
	fprintf(stdout, "  -t <timeout>           Wait 'timeout' seconds before retrying (may be a floating point number).\n");
	fprintf(stdout, "  -v                     Show program version information.\n");
	fprintf(stdout, "  -x                     Debugging mode.\n");

	exit(1);
}

/* This is not called, but is required by libfreeradius-eap.so */
int rad_virtual_server(REQUEST UNUSED *request)
{
  /* We're not the server so we cannot do this */
  abort();
}

rlm_rcode_t process_authorize(UNUSED int type, UNUSED REQUEST *request)
{
  /*We're not the server so we cannot do this*/
  abort();
}

/** Set the global radius config directory.
 *
 *  (copied from main/mainconfig.c)
 */
void set_radius_dir(TALLOC_CTX *ctx, char const *path)
{
	if (radius_dir) {
		char *p;

		memcpy(&p, &radius_dir, sizeof(p));
		talloc_free(p);
		radius_dir = NULL;
	}
	if (path) radius_dir = talloc_strdup(ctx, path);
}

/** Print a elapsed time buffer (SS.uuuuuu).
 */
static char *rc_print_elapsed(char *out, uint8_t decimals)
{
	if (!out || !timerisset(&tv_start)) return NULL;

	if (decimals > 6) decimals = 6;

	struct timeval tv_now;
	struct timeval tv_delta;

	gettimeofday(&tv_now, NULL);
	timersub(&tv_now, &tv_start, &tv_delta);

	uint32_t u_sec = (uint32_t)(tv_delta.tv_sec);
	sprintf(out, "%d", u_sec);

	/* assuming tv_usec < USEC */
	if (decimals) {
		char buffer[8] = "";
		sprintf(buffer, ".%06d", (int) tv_delta.tv_usec);
		strncat(out, buffer, decimals+1); /* (this is always terminated with 0). */
	}

	return out;
}

/** Print a "hexstring" buffer (with optional separator each N octets)
 */
static char *rc_print_hexstr(char *pch_out, const uint8_t *in, int size, int separ_i, char sep)
{
	int i, j = 0;

	for (i = 0; i < size; i++) {
		if ((separ_i) && (j == separ_i)) {
			*pch_out = sep;
			pch_out += 1;
			j = 0;
		}
		j++;
		sprintf(pch_out, "%02x", in[i]);
		pch_out += 2;
	}
	*pch_out = '\0';
	return pch_out;
}

/** Convert a float to struct timeval.
 */
static void rc_float_to_timeval(struct timeval *tv, float f_val)
{
	tv->tv_sec = (time_t)f_val;
	uint64_t usec = (uint64_t)(f_val * USEC) - (tv->tv_sec * USEC);
	tv->tv_usec = usec;
}

/** Convert a struct timeval to float
 */
static float rc_timeval_to_float(struct timeval *tv)
{
	return ((float)tv->tv_sec + ((float)tv->tv_usec / USEC));
}

/** Add an allocated rc_input_vps_t entry to the tail of the list.
 */
 static void rc_add_vps_entry(rc_input_vps_list_t *list, rc_input_vps_t *entry)
{
	if (!list || !entry) return;

	if (!list->head) {
		assert(list->tail == NULL);
		list->head = entry;
		entry->prev = NULL;
	} else {
		assert(list->tail != NULL);
		assert(list->tail->next == NULL);
		list->tail->next = entry;
		entry->prev = list->tail;
	}
	list->tail = entry;
	entry->next = NULL;
	entry->list = list;
	list->size ++;
}

/** Remove a selected rc_input_vps_t entry from its current list.
 */
static rc_input_vps_t *rc_yank_vps_entry(rc_input_vps_t *entry)
{
	if (!entry) return NULL;

	if (!entry->list) return entry; /* not in a list, nothing to do. Just return the entry. */

	rc_input_vps_t *prev, *next;

	prev = entry->prev;
	next = entry->next;

	rc_input_vps_list_t *list = entry->list;

	assert(list->head != NULL); /* entry belongs to a list, so the list can't be empty. */
	assert(list->tail != NULL); /* same. */

	if (prev) {
		assert(list->head != entry); /* if entry has a prev, then entry can't be head. */
		prev->next = next;
	}
	else {
		assert(list->head == entry); /* if entry has no prev, then entry must be head. */
		list->head = next;
	}

	if (next) {
		assert(list->tail != entry); /* if entry has a next, then entry can't be tail. */
		next->prev = prev;
	}
	else {
		assert(list->tail == entry); /* if entry has no next, then entry must be tail. */
		list->tail = prev;
	}

	entry->list = NULL;
	entry->prev = NULL;
	entry->next = NULL;
	list->size --;
	return entry;
}

/** Load input entries (list of vps) from a file or stdin, and add them to the list.
 *  They will be used to initiate transactions.
 */
static int rc_load_input(TALLOC_CTX *ctx, char const *filename, rc_input_vps_list_t *list, uint32_t max_entries)
{
	FILE *file_in = NULL;
	bool file_done = false;
	rc_input_vps_t *request;
	char const *input;
	uint32_t input_num = 0;

	/* Determine where to read the VP's from. */
	if (filename && strcmp(filename, "-") != 0) {
		DEBUG2("Opening input file: %s", filename);
		file_in = fopen(filename, "r");
		if (!file_in) {
			ERROR("Error opening %s: %s", filename, strerror(errno));
			return 0;
		}
		input = filename;
	} else {
		DEBUG2("Reading input vps from stdin");
		file_in = stdin;
		input = "stdin";
	}

	/* Loop over the file (or stdin). */
	do {
		input_num ++;
		MEM(request = talloc_zero(ctx, rc_input_vps_t));

		if (fr_pair_list_afrom_file(request, &request->vps_in, file_in, &file_done) < 0) {
			ERROR("Error parsing entry %u from input: %s", input_num, input);
			talloc_free(request);
			break;
		}
		if (NULL == request->vps_in) {
			/* Last line might be empty, in this case fr_pair_list_afrom_file will return a NULL vps pointer. Silently ignore this. */
			talloc_free(request);
			break;
		}

		/* Add that to the list */
		rc_add_vps_entry(list, request);

		request->num = list->size;

		if (max_entries && list->size >= max_entries) {
			/* Only load what we need. */
			break;
		}
	} while (!file_done);

	if (file_in != stdin) fclose(file_in);

	/* And we're done. */
	num_input += list->size;
	DEBUG("Read %d element(s) from input: %s", list->size, input);
	return 1;
}

/** Perform packet initialization for a transaction.
 */
static int rc_init_packet(rc_transaction_t *trans)
{
	if (!trans || !trans->packet) return 0;

	RADIUS_PACKET *packet = trans->packet;
	vp_cursor_t cursor;
	VALUE_PAIR *vp;

	/*
	 *	Process special attributes
	 */
	for (vp = fr_cursor_init(&cursor, &packet->vps);
		 vp;
		 vp = fr_cursor_next(&cursor)) {
		/*
		 *	Double quoted strings get marked up as xlat expansions,
		 *	but we don't support that in request.
		 */
		if (vp->type == VT_XLAT) {
			vp->type = VT_DATA;
			vp->vp_strvalue = vp->xlat;
			vp->vp_length = talloc_array_length(vp->vp_strvalue) - 1;
		}

		if (!vp->da->vendor) switch (vp->da->attr) {
		default:
			break;

		/*
		 *	Allow it to set the packet type in
		 *	the attributes read from the file.
		 */
		case PW_PACKET_TYPE:
			packet->code = vp->vp_integer;
			break;

		case PW_PACKET_DST_PORT:
			packet->dst_port = (vp->vp_integer & 0xffff);
			break;

		case PW_PACKET_DST_IP_ADDRESS:
			packet->dst_ipaddr.af = AF_INET;
			packet->dst_ipaddr.ipaddr.ip4addr.s_addr = vp->vp_ipaddr;
			packet->dst_ipaddr.prefix = 32;
			break;

		case PW_PACKET_DST_IPV6_ADDRESS:
			packet->dst_ipaddr.af = AF_INET6;
			packet->dst_ipaddr.ipaddr.ip6addr = vp->vp_ipv6addr;
			packet->dst_ipaddr.prefix = 128;
			break;

		case PW_PACKET_SRC_PORT:
			if ((vp->vp_integer < 1024) ||
				(vp->vp_integer > 65535)) {
				DEBUG("Invalid value '%u' for Packet-Src-Port", vp->vp_integer);
			} else {
				packet->src_port = (vp->vp_integer & 0xffff);
			}
			break;

		case PW_PACKET_SRC_IP_ADDRESS:
			packet->src_ipaddr.af = AF_INET;
			packet->src_ipaddr.ipaddr.ip4addr.s_addr = vp->vp_ipaddr;
			packet->src_ipaddr.prefix = 32;
			break;

		case PW_PACKET_SRC_IPV6_ADDRESS:
			packet->src_ipaddr.af = AF_INET6;
			packet->src_ipaddr.ipaddr.ip6addr = vp->vp_ipv6addr;
			packet->src_ipaddr.prefix = 128;
			break;

		case PW_DIGEST_REALM:
		case PW_DIGEST_NONCE:
		case PW_DIGEST_METHOD:
		case PW_DIGEST_URI:
		case PW_DIGEST_QOP:
		case PW_DIGEST_ALGORITHM:
		case PW_DIGEST_BODY_DIGEST:
		case PW_DIGEST_CNONCE:
		case PW_DIGEST_NONCE_COUNT:
		case PW_DIGEST_USER_NAME:
		/* overlapping! */
		{
			fr_dict_attr_t const *da;
			uint8_t *p, *q;

			p = talloc_array(vp, uint8_t, vp->vp_length + 2);

			memcpy(p + 2, vp->vp_octets, vp->vp_length);
			p[0] = vp->da->attr - PW_DIGEST_REALM + 1;
			vp->vp_length += 2;
			p[1] = vp->vp_length;

			da = fr_dict_attr_by_num(NULL, 0, PW_DIGEST_ATTRIBUTES);
			if (!da) {
				ERROR("Attribute 'Digest-Attributes' not found by value");
				exit(1);
			}
			vp->da = da;

			/*
			 *	Re-do fr_pair_value_memsteal ourselves,
			 *	because we play games with
			 *	vp->da, and fr_pair_value_memsteal goes
			 *	to GREAT lengths to sanitize
			 *	and fix and change and
			 *	double-check the various
			 *	fields.
			 */
			memcpy(&q, &vp->vp_octets, sizeof(q));
			talloc_free(q);

			vp->vp_octets = talloc_steal(vp, p);
			vp->type = VT_DATA;

			VERIFY_VP(vp);
		}
			break;

		/*
		 *	Keep a copy of the the password attribute.
		 */
		case PW_USER_PASSWORD:
		case PW_CHAP_PASSWORD:
		case PW_MS_CHAP_PASSWORD:
			strlcpy(trans->password, vp->vp_strvalue, sizeof(trans->password));
			break;

		case PW_RADCLIENT_TEST_NAME:
			trans->name = vp->vp_strvalue;
			break;
		}
	} /* loop over the VP's we read in */

	if (packet->dst_port == 0) packet->dst_port = server_port;

	if (packet->dst_ipaddr.af == AF_UNSPEC) {
		if (!server_addr_init) {
			DEBUG("No server was given, and input entry %u did not contain Packet-Dst-IP-Address, ignored.", trans->input_vps->num);
			return 0;
		}
		packet->dst_ipaddr = server_ipaddr;
	}

	/* Use the default set on the command line. */
	if (packet->code == PW_CODE_UNDEFINED) {
		if (packet_code == PW_CODE_UNDEFINED) {
			DEBUG("No packet type was given, and input entry %u did not contain Packet-Type, ignored.", trans->input_vps->num);
			return 0;
		}
		packet->code = packet_code;
	}

	/* Automatically set the dst port (if one wasn't already set). */
	if (packet->dst_port == 0) {
		rc_get_radius_port(packet->code, &packet->dst_port);
		if (packet->dst_port == 0) {
			DEBUG("Can't determine destination port for input entry %u, ignored.", trans->input_vps->num);
			return 0;
		}
	}

	packet->sockfd = -1;

	/* Done. */
	return 1;
}

/** Map EAP methods and build EAP-Message (if EAP is involved).
 *  Also allocate the EAP context.
 */
static void rc_build_eap_context(rc_transaction_t *trans)
{
	if (!trans || !trans->packet) return;

	RADIUS_PACKET *packet = trans->packet;

	/* Build EAP-Message (if EAP is involved. Otherwise, do nothing). */
	int eap_type = rc_map_eap_methods(packet);

	if (eap_type) {
		if (!trans->eap_context) {
			MEM(trans->eap_context = talloc_zero(trans, rc_eap_context_t));
			STATS_INC(nb_eap);
		}
		trans->eap_context->eap_type = eap_type;

		/*
		 *	Keep a copy of the the User-Password or CHAP-Password.
		 *	Note: this is not useful for EAP-SIM, but we cannot know what kind
		 *	of challenge the server will issue.
		 */
		VALUE_PAIR *vp;
		vp = fr_pair_find_by_num(packet->vps, 0, PW_CLEARTEXT_PASSWORD, TAG_ANY);
		if (!vp) vp = fr_pair_find_by_num(packet->vps, 0, PW_USER_PASSWORD, TAG_ANY);
		if (!vp) vp = fr_pair_find_by_num(packet->vps, 0, PW_CHAP_PASSWORD, TAG_ANY);
		if (vp) {
			strlcpy(trans->eap_context->password, vp->vp_strvalue, sizeof(trans->eap_context->password));
		}
	}
}

/** Grab an element from the input list. Initialize a new transaction context, using this element.
 */
static rc_transaction_t *rc_init_transaction(TALLOC_CTX *ctx)
{
	if (!rc_vps_list_in.head || rc_vps_list_in.size == 0) {
		/* Empty list, can't create a new transaction. */
		return NULL;
	}

	rc_input_vps_t *vps_entry = rc_vps_list_in.head;

	rc_yank_vps_entry(vps_entry); /* This cannot fail (we checked the list beforehand.) */

	/* We grabbed an vps entry, now we can initialize a new transaction. */
	rc_transaction_t *trans;
	MEM(trans = talloc_zero(ctx, rc_transaction_t));

	trans->input_vps = vps_entry;
	trans->id = num_trans ++;

	talloc_steal(trans, vps_entry); /* It's ours now. */

	RADIUS_PACKET *packet;
	MEM(packet = fr_radius_alloc(trans, 1));
	trans->packet = packet;

	/* Fill in the packet value pairs. */
	packet->vps = fr_pair_list_copy(packet, vps_entry->vps_in);

	/* Initialize the transaction packet. */
	if (!rc_init_packet(trans)) {
		/* Failed... */
		talloc_free(trans);
		return NULL;
	}

	vps_entry->recycle ++;

	gettimeofday(&trans->timestamp, NULL);

	/* Update transactions counters. */
	num_started ++;
	num_ongoing ++;

	STATS_INC(nb_started);

	return trans;
}

/** Terminate a transaction.
 */
static void rc_finish_transaction(rc_transaction_t *trans)
{
	if (!trans) return;

	if (trans->event) fr_event_delete(ev_list, &trans->event);
	rc_deallocate_id(trans);

	rc_input_vps_t *vps_entry = trans->input_vps;
	if (vps_entry->recycle < recycle_count) {
		/* Not done yet with this input. Put it back into the list of available entries. */
		talloc_steal(autofree, vps_entry);
		rc_add_vps_entry(&rc_vps_list_in, vps_entry);
		trans->input_vps = NULL;
	}

	talloc_free(trans);

	/* Update transactions counters. */
	num_ongoing --;
	num_finished ++;

	DEBUG4("pl: %d, ev: %d, in: %d", fr_packet_list_num_outgoing(pl), fr_event_list_num_elements(ev_list), rc_vps_list_in.size);
}


static void rc_cleanresp(RADIUS_PACKET *resp)
{
	VALUE_PAIR *vp;
	vp_cursor_t cursor;

	/*
	 * maybe should just copy things we care about, or keep
	 * a copy of the original input and start from there again?
	 */
	fr_pair_delete_by_num(&resp->vps, 0, PW_EAP_MESSAGE, TAG_ANY);
	fr_pair_delete_by_num(&resp->vps, 0, PW_EAP_TYPE_BASE + PW_EAP_IDENTITY, TAG_ANY);

	for (vp = fr_cursor_init(&cursor, &resp->vps);
	     vp;
	     vp = fr_cursor_next(&cursor)) {
		if ((vp->da->attr >= PW_EAP_TYPE_BASE &&
		     vp->da->attr < PW_EAP_TYPE_BASE+256) ||
		   (vp->da->attr >= PW_EAP_SIM_BASE &&
		    vp->da->attr < PW_EAP_SIM_BASE+256))
		{
			vp = fr_cursor_remove(&cursor);
			talloc_free(vp);
		}
	}
}

/** We got an EAP-Request/Sim/Start message in a legal state.
 *
 *  pick a supported version, put it into the reply, and insert a nonce.
 */
static int rc_process_eap_start(rc_eap_context_t *eap_context,
                                RADIUS_PACKET *req, RADIUS_PACKET *rep)
{
	VALUE_PAIR *vp, *newvp;
	VALUE_PAIR *anyidreq_vp, *fullauthidreq_vp, *permanentidreq_vp;
	uint16_t const *versions;
	uint16_t selectedversion;
	unsigned int i,versioncount;

	/* form new response clear of any EAP stuff */
	rc_cleanresp(rep);

	if ((vp = fr_pair_find_by_num(req->vps, 0, PW_EAP_SIM_VERSION_LIST, TAG_ANY)) == NULL) {
		ERROR("illegal start message has no VERSION_LIST");
		return 0;
	}

	versions = (uint16_t const *) vp->vp_strvalue;

	/* verify that the attribute length is big enough for a length field */
	if (vp->vp_length < 4)
	{
		ERROR("start message has illegal VERSION_LIST. Too short: %u", (unsigned int) vp->vp_length);
		return 0;
	}

	versioncount = ntohs(versions[0])/2;
	/* verify that the attribute length is big enough for the given number
	 * of versions present.
	 */
	if ((unsigned)vp->vp_length <= (versioncount*2 + 2))
	{
		ERROR("start message is too short. Claimed %d versions does not fit in %u bytes", versioncount, (unsigned int) vp->vp_length);
		return 0;
	}

	/*
	 * record the versionlist for the MK calculation.
	 */
	eap_context->eap.sim.keys.versionlistlen = versioncount*2;
	memcpy(eap_context->eap.sim.keys.versionlist, (unsigned char const *)(versions+1),
	       eap_context->eap.sim.keys.versionlistlen);

	/* walk the version list, and pick the one we support, which
	 * at present, is 1, EAP_SIM_VERSION.
	 */
	selectedversion=0;
	for (i=0; i < versioncount; i++)
	{
		if (ntohs(versions[i+1]) == EAP_SIM_VERSION)
		{
			selectedversion=EAP_SIM_VERSION;
			break;
		}
	}
	if (selectedversion == 0)
	{
		ERROR("eap-sim start message. No compatible version found. We need %d", EAP_SIM_VERSION);
		for (i=0; i < versioncount; i++)
		{
			ERROR("\tfound version %d",
				ntohs(versions[i+1]));
		}
	}

	/*
	 * now make sure that we have only FULLAUTH_ID_REQ.
	 * I think that it actually might not matter - we can answer in
	 * anyway we like, but it is illegal to have more than one
	 * present.
	 */
	anyidreq_vp = fr_pair_find_by_num(req->vps, 0, PW_EAP_SIM_ANY_ID_REQ, TAG_ANY);
	fullauthidreq_vp = fr_pair_find_by_num(req->vps, 0, PW_EAP_SIM_FULLAUTH_ID_REQ, TAG_ANY);
	permanentidreq_vp = fr_pair_find_by_num(req->vps, 0, PW_EAP_SIM_PERMANENT_ID_REQ, TAG_ANY);

	if (!fullauthidreq_vp ||
	    anyidreq_vp != NULL ||
	    permanentidreq_vp != NULL) {
		ERROR("start message has %sanyidreq, %sfullauthid and %spermanentid. Illegal combination.",
			(anyidreq_vp != NULL ? "a ": "no "),
			(fullauthidreq_vp != NULL ? "a ": "no "),
			(permanentidreq_vp != NULL ? "a ": "no "));
		return 0;
	}

	/* okay, we have just any_id_req there, so fill in response */

	/* mark the subtype as being EAP-SIM/Response/Start */
	newvp = fr_pair_afrom_num(rep, 0, PW_EAP_SIM_SUBTYPE);
	newvp->vp_integer = EAPSIM_START;
	fr_pair_replace(&(rep->vps), newvp);

	/* insert selected version into response. */
	{
		uint16_t no_versions;

		no_versions = htons(selectedversion);

		newvp = fr_pair_afrom_num(rep, 0, PW_EAP_SIM_SELECTED_VERSION);
		fr_pair_value_memcpy(newvp, (uint8_t *) &no_versions, 2);
		fr_pair_replace(&(rep->vps), newvp);

		/* record the selected version */
		memcpy(eap_context->eap.sim.keys.versionselect, &no_versions, 2);
	}

	vp = newvp = NULL;

	{
		uint32_t nonce[4];
		uint8_t *p;
		/*
		 * insert a nonce_mt that we make up.
		 */
		nonce[0]=fr_rand();
		nonce[1]=fr_rand();
		nonce[2]=fr_rand();
		nonce[3]=fr_rand();

		newvp = fr_pair_afrom_num(rep, 0, PW_EAP_SIM_NONCE_MT);

		p = talloc_zero_array(newvp, uint8_t, 18); /* 18 = 16 bytes of nonce + padding */
		memcpy(&p[2], nonce, 16);
		fr_pair_value_memsteal(newvp, p);

		fr_pair_replace(&(rep->vps), newvp);

		/* also keep a copy of the nonce! */
		memcpy(eap_context->eap.sim.keys.nonce_mt, nonce, 16);
	}

	{
		uint16_t idlen;
		uint8_t *p;
		uint16_t no_idlen;

		/*
		 * insert the identity here.
		 */
		vp = fr_pair_find_by_num(rep->vps, 0, PW_USER_NAME, TAG_ANY);
		if (!vp)
		{
			ERROR("eap-sim: We need to have a User-Name attribute!");
			return 0;
		}
		newvp = fr_pair_afrom_num(rep, 0, PW_EAP_SIM_IDENTITY);

		idlen = strlen(vp->vp_strvalue);
		p = talloc_zero_array(newvp, uint8_t, idlen + 2);
		no_idlen = htons(idlen);
		memcpy(p, &no_idlen, 2);
		memcpy(p + 2, vp->vp_strvalue, idlen);
		fr_pair_value_memsteal(newvp, p);

		fr_pair_replace(&(rep->vps), newvp);

		/* record it */
		memcpy(eap_context->eap.sim.keys.identity, vp->vp_strvalue, idlen);
		eap_context->eap.sim.keys.identitylen = idlen;
	}

	return 1;
}

/** We got an EAP-Request/Sim/Challenge message in a legal state.
 *
 *  use the RAND challenge to produce the SRES result, and then
 *  use that to generate a new MAC.
 *
 *  for the moment, we ignore the RANDs, then just plug in the SRES
 *  values.
 */
static int rc_process_eap_challenge(rc_eap_context_t *eap_context,
                                    RADIUS_PACKET *req, RADIUS_PACKET *rep)
{
	VALUE_PAIR *newvp;
	VALUE_PAIR *mac, *randvp;
	VALUE_PAIR *sres1, *sres2, *sres3;
	VALUE_PAIR *Kc1, *Kc2, *Kc3;
	uint8_t calcmac[EAPSIM_CALCMAC_SIZE];

	/* look for the AT_MAC and the challenge data */
	mac = fr_pair_find_by_num(req->vps, 0, PW_EAP_SIM_MAC, TAG_ANY);
	randvp = fr_pair_find_by_num(req->vps, 0, PW_EAP_SIM_RAND, TAG_ANY);
	if (!mac || !randvp) {
		ERROR("Challenge message needs to contain RAND and MAC");
		return 0;
	}

	/*
	 * compare RAND with randX, to verify this is the right response
	 * to this challenge.
	 */
	{
		VALUE_PAIR *randcfgvp[3];
		uint8_t const *randcfg[3];

		randcfg[0] = &randvp->vp_octets[2];
		randcfg[1] = &randvp->vp_octets[2+EAPSIM_RAND_SIZE];
		randcfg[2] = &randvp->vp_octets[2+EAPSIM_RAND_SIZE*2];

		randcfgvp[0] = fr_pair_find_by_num(rep->vps, 0, PW_EAP_SIM_RAND1, TAG_ANY);
		randcfgvp[1] = fr_pair_find_by_num(rep->vps, 0, PW_EAP_SIM_RAND2, TAG_ANY);
		randcfgvp[2] = fr_pair_find_by_num(rep->vps, 0, PW_EAP_SIM_RAND3, TAG_ANY);

		if (!randcfgvp[0] ||
		    !randcfgvp[1] ||
		    !randcfgvp[2]) {
			ERROR("Need to have RAND 1, 2 and 3 set");
			return 0;
		}

		if (memcmp(randcfg[0], randcfgvp[0]->vp_octets, EAPSIM_RAND_SIZE) != 0 ||
		    memcmp(randcfg[1], randcfgvp[1]->vp_octets, EAPSIM_RAND_SIZE) != 0 ||
		    memcmp(randcfg[2], randcfgvp[2]->vp_octets, EAPSIM_RAND_SIZE) != 0)
		{
			int rnum;

			ERROR("one of RAND 1, 2, or 3 didn't match");

			char ch_rand[EAPSIM_RAND_SIZE*2 +1 +3] = ""; // +3 for separators.
			for (rnum = 0; rnum < 3; rnum++) {
				rc_print_hexstr(ch_rand, randcfg[rnum], EAPSIM_RAND_SIZE, 4, '_');
				ERROR("Received   rand %d: %s", rnum, ch_rand);

				rc_print_hexstr(ch_rand, randcfgvp[rnum]->vp_octets, EAPSIM_RAND_SIZE, 4, '_');
				ERROR("Configured rand %d: %s", rnum, ch_rand);
			}
			return 0;
		}
	}

	/*
	 * now dig up the sres values from the response packet,
	 * which were put there when we read things in.
	 *
	 * Really, they should be calculated from the RAND!
	 *
	 */
	sres1 = fr_pair_find_by_num(rep->vps, 0, PW_EAP_SIM_SRES1, TAG_ANY);
	sres2 = fr_pair_find_by_num(rep->vps, 0, PW_EAP_SIM_SRES2, TAG_ANY);
	sres3 = fr_pair_find_by_num(rep->vps, 0, PW_EAP_SIM_SRES3, TAG_ANY);

	if (!sres1 ||
	    !sres2 ||
	    !sres3) {
		ERROR("Need to have SRES 1, 2, and 3 set");
		return 0;
	}
	memcpy(eap_context->eap.sim.keys.sres[0], sres1->vp_strvalue, sizeof(eap_context->eap.sim.keys.sres[0]));
	memcpy(eap_context->eap.sim.keys.sres[1], sres2->vp_strvalue, sizeof(eap_context->eap.sim.keys.sres[1]));
	memcpy(eap_context->eap.sim.keys.sres[2], sres3->vp_strvalue, sizeof(eap_context->eap.sim.keys.sres[2]));

	Kc1 = fr_pair_find_by_num(rep->vps, 0, PW_EAP_SIM_KC1, TAG_ANY);
	Kc2 = fr_pair_find_by_num(rep->vps, 0, PW_EAP_SIM_KC2, TAG_ANY);
	Kc3 = fr_pair_find_by_num(rep->vps, 0, PW_EAP_SIM_KC3, TAG_ANY);

	if (!Kc1 ||
	    !Kc2 ||
	    !Kc3) {
		ERROR("Need to have Kc 1, 2, and 3 set");
		return 0;
	}
	memcpy(eap_context->eap.sim.keys.kc[0], Kc1->vp_strvalue, sizeof(eap_context->eap.sim.keys.kc[0]));
	memcpy(eap_context->eap.sim.keys.kc[1], Kc2->vp_strvalue, sizeof(eap_context->eap.sim.keys.kc[1]));
	memcpy(eap_context->eap.sim.keys.kc[2], Kc3->vp_strvalue, sizeof(eap_context->eap.sim.keys.kc[2]));

	/* all set, calculate keys */
	eap_sim_calculate_keys(&eap_context->eap.sim.keys);

	if (rad_debug_lvl) {
		eap_sim_dump_mk(&eap_context->eap.sim.keys);
	}

	/* verify the MAC, now that we have all the keys. */
	int rcode_mac = eap_sim_check_mac(NULL, req->vps, eap_context->eap.sim.keys.K_aut,
	                                eap_context->eap.sim.keys.nonce_mt, sizeof(eap_context->eap.sim.keys.nonce_mt),
	                                calcmac);

	char ch_calc_mac[EAPSIM_CALCMAC_SIZE*2 +1 +4] = ""; // +4 for separators.
	rc_print_hexstr(ch_calc_mac, calcmac, EAPSIM_CALCMAC_SIZE, 4, '_');

	if (rcode_mac) {
		DEBUG2("MAC check succeeded (%s)", ch_calc_mac);
	}
	else {
		ERROR("Challenge MAC check failed. Calculated MAC (%s) did not match", ch_calc_mac);
		return 0;
	}

	/* form new response clear of any EAP stuff */
	rc_cleanresp(rep);

	/* mark the subtype as being EAP-SIM/Response/Start */
	newvp = fr_pair_afrom_num(rep, 0, PW_EAP_SIM_SUBTYPE);
	newvp->vp_integer = EAPSIM_CHALLENGE;
	fr_pair_replace(&(rep->vps), newvp);

	{
		uint8_t *p;
		/*
		 * fill the SIM_MAC with a field that will in fact get appended
		 * to the packet before the MAC is calculated
		 */
		newvp = fr_pair_afrom_num(rep, 0, PW_EAP_SIM_MAC);

		p = talloc_zero_array(newvp, uint8_t, EAPSIM_SRES_SIZE*3);
		memcpy(p+EAPSIM_SRES_SIZE * 0, sres1->vp_strvalue, EAPSIM_SRES_SIZE);
		memcpy(p+EAPSIM_SRES_SIZE * 1, sres2->vp_strvalue, EAPSIM_SRES_SIZE);
		memcpy(p+EAPSIM_SRES_SIZE * 2, sres3->vp_strvalue, EAPSIM_SRES_SIZE);
		fr_pair_value_memsteal(newvp, p);

		fr_pair_replace(&(rep->vps), newvp);
	}

	newvp = fr_pair_afrom_num(rep, 0, PW_EAP_SIM_KEY);
	fr_pair_value_memcpy(newvp, eap_context->eap.sim.keys.K_aut, EAPSIM_AUTH_SIZE);

	fr_pair_replace(&(rep->vps), newvp);

	return 1;
}

/** This runs the EAP-SIM client state machine.
 *  the *request* is from the server.
 *  the *reponse* is to the server.
 */
static int rc_respond_eap_sim(rc_eap_context_t *eap_context,
                              RADIUS_PACKET *req, RADIUS_PACKET *resp)
{
	eap_sim_client_states_t state, newstate;
	eap_sim_subtype_t subtype;
	VALUE_PAIR *vp, *statevp, *radstate, *eapid;
	char statenamebuf[32], subtypenamebuf[32];
	int rcode_eap;

	if ((radstate = fr_pair_list_copy_by_num(NULL, req->vps, 0, PW_STATE, TAG_ANY)) == NULL)
	{
		return 0;
	}

	if ((eapid = fr_pair_list_copy_by_num(NULL, req->vps, 0, PW_EAP_ID, TAG_ANY)) == NULL)
	{
		return 0;
	}

	/* first, dig up the state from the request packet, setting
	 * ourselves to be in EAP-SIM-Start state if there is none.
	 */

	if ((statevp = fr_pair_find_by_num(resp->vps, 0, PW_EAP_SIM_STATE, TAG_ANY)) == NULL)
	{
		/* must be initial request */
		statevp = fr_pair_afrom_num(resp, 0, PW_EAP_SIM_STATE);
		statevp->vp_integer = EAPSIM_CLIENT_INIT;
		fr_pair_replace(&(resp->vps), statevp);
	}
	state = statevp->vp_integer;

	/*
	 * map the attributes, and authenticate them.
	 */
	rc_unmap_eap_sim_types(req);

	if ((vp = fr_pair_find_by_num(req->vps, 0, PW_EAP_SIM_SUBTYPE, TAG_ANY)) == NULL)
	{
		return 0;
	}
	subtype = vp->vp_integer;

	/*
	 * look for the appropriate state, and process incoming message
	 */
	switch (state) {
	case EAPSIM_CLIENT_INIT:
		switch (subtype) {
		case EAPSIM_START:
			rcode_eap = rc_process_eap_start(eap_context, req, resp);
			break;

		case EAPSIM_CHALLENGE:
		case EAPSIM_NOTIFICATION:
		case EAPSIM_REAUTH:
		default:
			ERROR("sim in state '%s' (%d), message '%s' (%d) is illegal. Reply dropped.",
				eap_sim_state_to_name(statenamebuf, sizeof(statenamebuf), state), state,
				eap_sim_subtype_to_name(subtypenamebuf, sizeof(subtypenamebuf), subtype), subtype);
			/* invalid state, drop message */
			return 0;
		}
		break;

	case EAPSIM_CLIENT_START:
		switch (subtype) {
		case EAPSIM_START:
			/* NOT SURE ABOUT THIS ONE, retransmit, I guess */
			rcode_eap = rc_process_eap_start(eap_context, req, resp);
			break;

		case EAPSIM_CHALLENGE:
			rcode_eap = rc_process_eap_challenge(eap_context, req, resp);
			break;

		default:
			ERROR("sim in state %s message %s is illegal. Reply dropped.",
				eap_sim_state_to_name(statenamebuf, sizeof(statenamebuf), state),
				eap_sim_subtype_to_name(subtypenamebuf, sizeof(subtypenamebuf), subtype));
			/* invalid state, drop message */
			return 0;
		}
		break;

	default:
		ERROR("sim in illegal state '%s' (%d)",
			eap_sim_state_to_name(statenamebuf, sizeof(statenamebuf), state), state);
		return 0;
	}

	/* process_eap_* functions return 0 if fail, 1 if success. */
	if (!rcode_eap) {
		ERROR("EAP process failed, aborting EAP-SIM transaction.");
		return 0;
	}
	newstate = EAPSIM_CLIENT_START; // (1)

	/* copy the eap state object in */
	fr_pair_replace(&(resp->vps), eapid);

	/* update state info, and send new packet */
	rc_map_eap_sim_types(resp);

	/* copy the radius state object in */
	fr_pair_replace(&(resp->vps), radstate);

	statevp->vp_integer = newstate;
	return 1;
}

static int rc_respond_eap_md5(rc_eap_context_t *eap_context,
                              RADIUS_PACKET *req, RADIUS_PACKET *rep)
{
	VALUE_PAIR *vp, *id, *state;
	size_t valuesize;
	uint8_t identifier;
	uint8_t const *value;
	FR_MD5_CTX	context;
	uint8_t    response[16];

	rc_cleanresp(rep);

	if ((state = fr_pair_list_copy_by_num(NULL, req->vps, 0, PW_STATE, TAG_ANY)) == NULL)
	{
		ERROR("no state attribute found");
		return 0;
	}

	if ((id = fr_pair_list_copy_by_num(NULL, req->vps, 0, PW_EAP_ID, TAG_ANY)) == NULL)
	{
		ERROR("no EAP-ID attribute found");
		return 0;
	}
	identifier = id->vp_integer;

	if ((vp = fr_pair_find_by_num(req->vps, 0, PW_EAP_TYPE_BASE + PW_EAP_MD5, TAG_ANY)) == NULL)
	{
		ERROR("no EAP-MD5 attribute found");
		return 0;
	}

	/* got the details of the MD5 challenge */
	valuesize = vp->vp_octets[0];
	value = &vp->vp_octets[1];

	/* sanitize items */
	if (valuesize > vp->vp_length)
	{
		ERROR("md5 valuesize if too big (%u > %u)",
			(unsigned int) valuesize, (unsigned int) vp->vp_length);
		return 0;
	}

	/* now do the CHAP operation ourself, rather than build the
	 * buffer. We could also call fr_radius_encode_chap_password, but it wants
	 * a CHAP-Challenge, which we don't want to bother with.
	 */
	fr_md5_init(&context);
	fr_md5_update(&context, &identifier, 1);
	fr_md5_update(&context, (uint8_t *) eap_context->password, strlen(eap_context->password));
	fr_md5_update(&context, value, valuesize);
	fr_md5_final(response, &context);

	{
		uint8_t *p;
		uint8_t lg_response;

		vp = fr_pair_afrom_num(rep, 0, PW_EAP_TYPE_BASE + PW_EAP_MD5);
		vp->vp_length = 17;

		p = talloc_zero_array(vp, uint8_t, 17);
		lg_response = 16;
		memcpy(p, &lg_response, 1);
		memcpy(p + 1, response, 16);
		fr_pair_value_memsteal(vp, p);
	}
	fr_pair_replace(&(rep->vps), vp);

	fr_pair_replace(&(rep->vps), id);

	/* copy the state object in */
	fr_pair_replace(&(rep->vps), state);

	return 1;
}


/** Allocate a new socket, and add it to the packet list.
 */
static void rc_add_socket(fr_ipaddr_t *src_ipaddr, uint16_t src_port, fr_ipaddr_t *dst_ipaddr, uint16_t dst_port)
{
	int mysockfd;

	/* Trace what we're doing. */
	char src_addr[INET6_ADDRSTRLEN] = "";
	char dst_addr[INET6_ADDRSTRLEN] = "";
	inet_ntop(AF_INET, &(src_ipaddr->ipaddr.ip4addr.s_addr), src_addr, sizeof(src_addr));
	inet_ntop(AF_INET, &(dst_ipaddr->ipaddr.ip4addr.s_addr), dst_addr, sizeof(dst_addr));

	INFO("Adding new socket: src: %s:%d, dst: %s:%d", src_addr, src_port, dst_addr, dst_port);

	mysockfd = fr_socket(src_ipaddr, src_port);
	if (mysockfd < 0) {
		ERROR("Failed to create new socket: %s", fr_strerror());
		exit(1);
	}

	if (!fr_packet_list_socket_add(pl, mysockfd, ipproto, dst_ipaddr, dst_port, NULL)) {
		ERROR("Failed to add new socket: %s", fr_strerror());
		exit(1);
	}

	num_sockets ++;
	DEBUG("Added new socket: %d (num sockets: %d)", mysockfd, num_sockets);
}

/** Send one packet for a transaction.
 */
static int rc_send_one_packet(rc_transaction_t *trans, RADIUS_PACKET **packet_p)
{
	if (!trans || !packet_p || !*packet_p) return -1;

	assert(pl != NULL);

	RADIUS_PACKET *packet = *packet_p;

	if (packet->id == -1) {
		/* Haven't sent the packet yet.  Initialize it. */
		bool rcode;
		int i;

		rc_build_eap_context(trans); /* In case of EAP, build EAP-Message and initialize EAP context. */

		assert(trans->reply == NULL);

		trans->tries = 0;
		packet->src_ipaddr.af = server_ipaddr.af;
		int nb_sock_add = 0;
		while (1) {
			/* Allocate a RADIUS packet ID from a suitable socket of the packet list. */
			rcode = fr_packet_list_id_alloc(pl, ipproto, packet_p, NULL);

			if (rcode) { /* Got an ID. */
				break;
			}
			if (nb_sock_add >= 1) {
				ERROR("Added %d new socket(s), but still could not get an ID (currently: %d outgoing requests).",
					nb_sock_add, fr_packet_list_num_outgoing(pl));
				exit(1);
			}

			/* Could not find a free packet ID. Allocate a new socket, then try again. */
			rc_add_socket(&packet->src_ipaddr, packet->src_port, &packet->dst_ipaddr, packet->dst_port);

			nb_sock_add ++;
		}

		assert(packet->id != -1);
		assert(packet->data == NULL);

		for (i = 0; i < 4; i++) {
			((uint32_t *) packet->vector)[i] = fr_rand();
		}
	}

	/*
	 *	Send the packet.
	 */
	DEBUG("Transaction: %u, sending packet: %u (id: %u)...", trans->id, trans->num_packet, packet->id);

	gettimeofday(&packet->timestamp, NULL); /* set outgoing packet timestamp. */

	if (fr_radius_send(packet, NULL, secret) < 0) {
		ERROR("Failed to send packet (sockfd: %d, id: %d): %s",
			packet->sockfd, packet->id, fr_strerror());
	}

	STATS_INC(nb_packets_sent);
	if (trans->tries) STATS_INC(nb_packets_retries);

	trans->num_packet ++;
	trans->tries ++;

	if (fr_debug_lvl > 0) fr_packet_header_print(fr_log_fp, packet, false);
	if (fr_debug_lvl > 0) fr_pair_list_fprint(fr_log_fp, packet->vps);

	return 1;
}

/** Send current packet of a transaction. Arm timeout event.
 */
static int rc_send_transaction_packet(rc_transaction_t *trans, RADIUS_PACKET **packet_p)
// note: we need a 'RADIUS_PACKET **' for fr_packet_list_id_alloc.
{
	if (!trans || !packet_p || !*packet_p) return -1;

	int ret = rc_send_one_packet(trans, packet_p);
	if (ret == 1) {
		/* Send successful: arm the timeout callback. */
		rc_evprep_packet_timeout(trans);
	}
	return ret;
}

/** Deallocate RADIUS packet ID.
 */
static void rc_deallocate_id(rc_transaction_t *trans)
{
	if (!trans || !trans->packet ||
	    (trans->packet->id < 0)) {
		return;
	}

	RADIUS_PACKET *packet = trans->packet;

	DEBUG2("Deallocating (sockfd: %d, id: %d)", packet->sockfd, packet->id);

	/*
	 *	One more unused RADIUS ID.
	 */
	fr_packet_list_id_free(pl, packet, true);
	/* note: "true" means automatically yank, so we must *not* yank ourselves before calling (otherwise, it does nothing)
	 * so, *don't*: fr_packet_list_yank(pl, request->packet); */

	/* free more stuff to ensure next allocate won't be stuck on a "full" socket. */
	packet->id = -1;
	packet->sockfd = -1;
	packet->src_ipaddr.af = AF_UNSPEC;
	packet->src_port = 0;

	/*
	 *	If we've already sent a packet, free up the old one,
	 *	and ensure that the next packet has a unique
	 *	authentication vector.
	 */
	if (packet->data) {
		talloc_free(packet->data);
		packet->data = NULL;
	}

	if (trans->reply) fr_radius_free(&trans->reply);
}

/** Receive one packet, maybe.
 */
static int rc_recv_one_packet(struct timeval *tv_wait_time)
{
	fd_set set;
	struct timeval tv;
	rc_transaction_t *trans;
	RADIUS_PACKET *reply, **packet_p;
	volatile int max_fd;
	bool ongoing_trans = false;
	char buffer[INET6_ADDRSTRLEN];

	/* Wait for reply, timing out as necessary */
	FD_ZERO(&set);

	max_fd = fr_packet_list_fd_set(pl, &set);
	if (max_fd < 0) {
		/* no sockets to listen on! */
		return 0;
	}

	if (NULL == tv_wait_time) {
		timerclear(&tv);
	} else {
		tv.tv_sec = tv_wait_time->tv_sec;
		tv.tv_usec = tv_wait_time->tv_usec;
	}

	if (select(max_fd, &set, NULL, NULL, &tv) <= 0) {
		/* No packet was received. */
		return 0;
	}

	/*
	 *	Receive the reply.
	 */
	reply = fr_packet_list_recv(pl, &set);
	if (!reply) {
		ERROR("Received bad packet: %s", fr_strerror());
		return -1;	/* bad packet */
	}

	/*
	 *	Look for the packet which matches the reply.
	 */
	reply->src_ipaddr = server_ipaddr;
	reply->src_port = server_port;

	/*
	 * Note: this only works if all packets have the same destination (IP, port).
	 * We should handle a list of destinations. But we don't. radclient doesn't do it either).
	 */

	packet_p = fr_packet_list_find_byreply(pl, reply);

	if (!packet_p) {
		/* got reply to packet we didn't send.
		 * (or maybe we sent it, got no response, freed the ID. Then server responds to first request.)
		 */
		DEBUG("No outstanding request was found for reply from %s, port %d (sockfd: %d, id: %d)",
			inet_ntop(reply->src_ipaddr.af, &reply->src_ipaddr.ipaddr, buffer, sizeof(buffer)),
			reply->src_port, reply->sockfd, reply->id);
		fr_radius_free(&reply);
		return -1;
	}

	STATS_INC(nb_packets_recv);

	trans = fr_packet2myptr(rc_transaction_t, packet, packet_p);

	if (trans->event) fr_event_delete(ev_list, &trans->event);

	/*
	 *	Fails the signature validation: not a valid reply.
	 */
	if (fr_radius_verify(reply, trans->packet, secret) < 0) {
		/* shared secret is incorrect.
		 * (or maybe this is a response to another packet we sent, for which we got no response,
		 * freed the ID, then reused it. Then server responds to first packet.)
		 */
		DEBUG("Conflicting response authenticator for reply from %s (sockfd: %d, id: %d)",
			inet_ntop(reply->src_ipaddr.af, &reply->src_ipaddr.ipaddr, buffer, sizeof(buffer)),
			reply->sockfd, reply->id);

		goto packet_done;
	}

	/* Set reply destination = packet source. */
	reply->dst_ipaddr = trans->packet->src_ipaddr;
	reply->dst_port = trans->packet->src_port;

	trans->reply = reply;
	reply = NULL;

	if (fr_radius_decode(trans->reply, trans->packet, secret) != 0) {
		/* This can fail if packet contains too many attributes. */
		DEBUG("Failed decoding reply");
		goto packet_done;
	}

	gettimeofday(&trans->reply->timestamp, NULL); /* set received packet timestamp. */

	if (trans->eap_context) {
		/* Call unmap before packet print (so we can see the decoded EAP stuff). */
		rc_unmap_eap_methods(trans->reply);
	}

	DEBUG("Transaction: %u, received packet (id: %u).", trans->id, trans->reply->id);

	if (fr_debug_lvl > 0) fr_packet_header_print(fr_log_fp, trans->reply, true);
	if (fr_debug_lvl > 0) fr_pair_list_fprint(fr_log_fp, trans->reply->vps);

	if (!trans->eap_context) {
		goto packet_done;
	}

	/* now look for the code type. */
	VALUE_PAIR *vp, *vpnext;
	for (vp = trans->reply->vps; vp != NULL; vp = vpnext) {
		vpnext = vp->next;

		switch (vp->da->attr) {
		default:
			break;

		case PW_EAP_TYPE_BASE + PW_EAP_MD5:
			if (rc_respond_eap_md5(trans->eap_context, trans->reply, trans->packet) && trans->eap_context->eap.md5.tried < 3)
			{
				/* answer the challenge from server. */
				trans->eap_context->eap.md5.tried ++;
				rc_deallocate_id(trans);
				rc_send_transaction_packet(trans, &trans->packet);
				ongoing_trans = true; // don't free the transaction yet.
			}
			goto packet_done;

		case PW_EAP_TYPE_BASE + PW_EAP_SIM:
			if (rc_respond_eap_sim(trans->eap_context, trans->reply, trans->packet)) {
				/* answer the challenge from server. */
				rc_deallocate_id(trans);
				rc_send_transaction_packet(trans, &trans->packet);
				ongoing_trans = true; // don't free the transaction yet.
			}
			goto packet_done;
		}
	}

	goto eap_done;

eap_done:
	/* EAP transaction ends here (no more requests from EAP server). */

	/*
	 * success: if we have EAP-Code = Success, and reply is an Access-Accept.
	 */
	if (trans->reply->code != PW_CODE_ACCESS_ACCEPT) {
		DEBUG("EAP transaction finished, but reply is not an Access-Accept");
		STATS_INC(nb_fail);
		goto packet_done;
	}
	vp = fr_pair_find_by_num(trans->reply->vps, 0, PW_EAP_CODE, TAG_ANY);
	if ( (!vp) || (vp->vp_integer != 3) ) {
		DEBUG("EAP transaction finished, but reply does not contain EAP-Code = Success");
		STATS_INC(nb_fail);
		goto packet_done;
	}
	STATS_INC(nb_success);
	rc_wf_stat_update(trans, RC_WF_EAP_REQUEST_SUCCESS);

	goto packet_done;

packet_done:

	if (trans->reply && !trans->eap_context) {
		/* Statistics for non-EAP transactions */
		switch (trans->reply->code) {
			case PW_CODE_ACCESS_ACCEPT:
				STATS_INC(nb_success);
				rc_wf_stat_update(trans, RC_WF_ACCESS_REQUEST_ACCEPT);
				break;
			case PW_CODE_COA_ACK:
				STATS_INC(nb_success);
				rc_wf_stat_update(trans, RC_WF_COA_REQUEST_ACK);
				break;
			case PW_CODE_ACCOUNTING_RESPONSE:
				STATS_INC(nb_success);
				rc_wf_stat_update(trans, RC_WF_ACCOUNTING_REQUEST_RESPONSE);
				break;
			case PW_CODE_ACCESS_REJECT:
			case PW_CODE_COA_NAK:
				STATS_INC(nb_fail);
				break;
			default:
				break;
		}
	}

	fr_radius_free(&trans->reply);
	fr_radius_free(&reply);	/* may be NULL */

	if (!ongoing_trans) {
		rc_deallocate_id(trans);
		rc_finish_transaction(trans);
	}

	return 1;
}

/** Event callback: packet timeout.
 */
static void rc_evcb_packet_timeout(void *ctx, UNUSED struct timeval *now)
{
	rc_transaction_t *trans = ctx;
	if (!trans || !trans->packet) return;

	DEBUG("Timeout for transaction: %d, tries (so far): %d (max: %d)", trans->id, trans->tries, retries);

	if (trans->event) fr_event_delete(ev_list, &trans->event);

	if (trans->tries < retries) {
		/* Try again. */
		rc_send_transaction_packet(trans, &trans->packet);
	} else {
		DEBUG("No response for transaction: %d, giving up", trans->id);
		rc_finish_transaction(trans);

		STATS_INC(nb_lost);
	}
}

/** Prepare event: packet timeout.
 */
static void rc_evprep_packet_timeout(rc_transaction_t *trans)
{
	struct timeval tv_event;
	gettimeofday(&tv_event, NULL);
	timeradd(&tv_event, &tv_timeout, &tv_event);

	if (!fr_event_insert(ev_list, rc_evcb_packet_timeout, (void *)trans, &tv_event, &trans->event)) {
		ERROR("Failed to insert event");
		exit(1);
	}
}

/** Event callback: report progress statistics.
 */
static void rc_evcb_progress_stat(void UNUSED *ctx, UNUSED struct timeval *now)
{
	/* print the progress statistics */
	rc_do_progress_stat();

	/* schedule the next */
	rc_evprep_progress_stat();
}

/** Prepare event: report progress statistics.
 */
static void rc_evprep_progress_stat(void)
{
	if (!timerisset(&tv_progress_interval)) return;

	struct timeval tv_event;
	gettimeofday(&tv_event, NULL);

	timeradd(&tv_event, &tv_progress_interval, &tv_event);

	static fr_event_t *event; /* only one of this kind. */

	if (!fr_event_insert(ev_list, rc_evcb_progress_stat, (void *) NULL, &tv_event, &event)) {
		ERROR("Failed to insert event");
		exit(1);
	}
}

/** Trigger all armed events for which time is reached.
 */
static uint32_t rc_loop_events(void)
{
	struct timeval when;
	uint32_t nb_processed = 0;

	if (!fr_event_list_num_elements(ev_list)) return 0;

	while (1) {
		gettimeofday(&when, NULL);
		if (!fr_event_run(ev_list, &when)) {
			/* no more. */
			break;
		}
		nb_processed ++;
	}
	return nb_processed;
}

/** Receive loop.
 *  Handle incoming packets, until nothing more is received.
 */
static uint32_t rc_loop_recv(void)
{
	uint32_t nb_received = 0;
	while (rc_recv_one_packet(NULL) > 0) {
		nb_received ++;
	}
	return nb_received;
}

/** Compute maximum number of new requests that can be started
 *  while conforming to the specified rate limit.
 */
static uint32_t rc_rate_limit(bool *do_limit)
{
	uint32_t max_start_new = 0;
	*do_limit = false;

	if (rate_limit) {
		/* get elapsed time so far */
		struct timeval tv_now, tv_elapsed;
		gettimeofday(&tv_now, NULL);
		timersub(&tv_now, &tv_start, &tv_elapsed);
		float elapsed = rc_timeval_to_float(&tv_elapsed);

		if (elapsed > 0) {
			*do_limit = true; /* enforce a limit */

			/* project ourselves a small amount of time in the future to perform calculation */
			float elapsed_p = elapsed + 0.01;

			/* Compute:
			 * The maximum number of started requests (according to the rate limit and elapsed time),
			 * and the maximum number of new requests that can be started according to the rate limit.
			 */
			uint32_t num_start_limit = (float)rate_limit * elapsed_p;
			if (num_start_limit > num_started) max_start_new = num_start_limit - num_started;

//#define DEBUG_RATE_LIMIT 1
#ifdef DEBUG_RATE_LIMIT
			if (max_start_new) {
				float cur_rate = num_started / elapsed;
				float target_rate = (float)(num_started + max_start_new) / elapsed_p;
				printf("RATE LIMIT - elapsed: %.3f, started: %d, rate: %.3f, limit: %d, new: %d, target rate: %.6f\n",
					elapsed, num_started, cur_rate, num_start_limit, max_start_new, target_rate);
			}
#endif
		}
	}
	return max_start_new;
}

/** Loop starting new transactions, until a limit is reached
 *  (max parallelism, or no more input available.)
 */
static uint32_t rc_loop_start_transactions(void)
{
	uint32_t nb_started = 0;
	bool do_limit = false;

	uint32_t max_start = rc_rate_limit(&do_limit);

	while (1) {
		if (num_ongoing >= parallel) break; /* parallel limit */
		if (do_limit && nb_started >= max_start) break; /* rate limit */

		/* Try to initialize a new transaction. */
		rc_transaction_t *trans = rc_init_transaction(autofree);
		if (!trans) break;

		nb_started ++;
		rc_send_transaction_packet(trans, &trans->packet);
	}
	return nb_started;
}

/** Main loop: Handle events. Receive and process responses. Start new transactions.
 *  Until we're done.
 */
static void rc_main_loop(void)
{
	while (1) {
		/* Handle events. */
		rc_loop_events();

		/* Receive and process response until no more are received (don't wait). */
		rc_loop_recv();

		/* Start new transactions and send the associated packet. */
		rc_loop_start_transactions();

		/* Check if we're done. */
		if ( (rc_vps_list_in.size == 0)
			&& (fr_packet_list_num_outgoing(pl) == 0) ) {
			break;
		}
	}
	INFO("Main loop: done.");
}

/** Get port number for a given service name.
 */
static uint16_t rc_getport(char const *name)
{
	struct	servent		*svp;

	svp = getservbyname(name, "udp");
	if (!svp) return 0;

	return ntohs(svp->s_port);
}

/** Set a port from the request type if we don't already have one.
 */
static void rc_get_radius_port(PW_CODE type, uint16_t *port)
{
	switch (type) {
	default:
	case PW_CODE_ACCESS_REQUEST:
	case PW_CODE_ACCESS_CHALLENGE:
	case PW_CODE_STATUS_SERVER:
		if (*port == 0) *port = rc_getport("radius");
		if (*port == 0) *port = PW_AUTH_UDP_PORT;
		return;

	case PW_CODE_ACCOUNTING_REQUEST:
		if (*port == 0) *port = rc_getport("radacct");
		if (*port == 0) *port = PW_ACCT_UDP_PORT;
		return;

	case PW_CODE_DISCONNECT_REQUEST:
		if (*port == 0) *port = PW_POD_UDP_PORT;
		return;

	case PW_CODE_COA_REQUEST:
		if (*port == 0) *port = PW_COA_UDP_PORT;
		return;

	case PW_CODE_UNDEFINED:
		if (*port == 0) *port = 0;
		return;
	}
}

/** Resolve a port to a request type.
 */
static PW_CODE rc_get_code(uint16_t port)
{
	/*
	 *	rc_getport returns 0 if the service doesn't exist
	 *	so we need to return early, to avoid incorrect
	 *	codes.
	 */
	if (port == 0) return PW_CODE_UNDEFINED;

	if ((port == rc_getport("radius")) || (port == PW_AUTH_UDP_PORT) || (port == PW_AUTH_UDP_PORT_ALT)) {
		return PW_CODE_ACCESS_REQUEST;
	}
	if ((port == rc_getport("radacct")) || (port == PW_ACCT_UDP_PORT) || (port == PW_ACCT_UDP_PORT_ALT)) {
		return PW_CODE_ACCOUNTING_REQUEST;
	}
	if (port == PW_COA_UDP_PORT) return PW_CODE_COA_REQUEST;
	if (port == PW_POD_UDP_PORT) return PW_CODE_DISCONNECT_REQUEST;

	return PW_CODE_UNDEFINED;
}

/** Resolve server hostname.
 */
static void rc_resolve_hostname(char *server_arg)
{
	if (force_af == AF_UNSPEC) force_af = AF_INET;
	server_ipaddr.af = force_af;
	if (strcmp(server_arg, "-") != 0) {
		char *p;
		char const *hostname = server_arg;
		char const *portname = server_arg;
		char buffer[256];

		if (*server_arg == '[') { /* IPv6 URL encoded */
			p = strchr(server_arg, ']');
			if ((size_t) (p - server_arg) >= sizeof(buffer)) {
				usage();
			}

			memcpy(buffer, server_arg + 1, p - server_arg - 1);
			buffer[p - server_arg - 1] = '\0';

			hostname = buffer;
			portname = p + 1;

		}
		p = strchr(portname, ':');
		if (p && (strchr(p + 1, ':') == NULL)) {
			*p = '\0';
			portname = p + 1;
		} else {
			portname = NULL;
		}

		if (fr_inet_hton(&server_ipaddr, force_af, hostname, false) < 0) {
			ERROR("Failed to find IP address for host %s: %s", hostname, strerror(errno));
			exit(1);
		}
		server_addr_init = true;

		/* Strip port from hostname if needed. */
		if (portname) server_port = atoi(portname);

		/*
		 *	Work backwards from the port to determine the packet type
		 */
		if (packet_code == PW_CODE_UNDEFINED) packet_code = rc_get_code(server_port);
	}
	rc_get_radius_port(packet_code, &server_port);
}

/** Update per-workflow statistics (number of transactions, rtt min, max, and cumulated).
 */
static void rc_wf_stat_update(rc_transaction_t *trans, rc_wf_type_t wf_type)
{
	if (!trans || !trans->packet || !trans->reply) return;
	if (!wf_type || wf_type >= RC_WF_MAX) return;

	struct timeval tv_rtt;
	timersub(&trans->reply->timestamp, &trans->timestamp, &tv_rtt);
	/* The reference timestamp is that of the transaction, because several packets can be involved, e.g. EAP. */

	int i;
	for (i=0; i<2; i++) { /* update the specified workflow type, and also "All" (0) */
		rc_wf_stats_t *my_stats = &stats.wf_stats[i*wf_type];

		if ((0 == my_stats->num) || (timercmp(&tv_rtt, &my_stats->tv_rtt_min, <))) {
			my_stats->tv_rtt_min.tv_sec = tv_rtt.tv_sec;
			my_stats->tv_rtt_min.tv_usec = tv_rtt.tv_usec;
		}
		if ((0 == my_stats->num) || (timercmp(&tv_rtt, &my_stats->tv_rtt_max, >=))) {
			my_stats->tv_rtt_max.tv_sec = tv_rtt.tv_sec;
			my_stats->tv_rtt_max.tv_usec = tv_rtt.tv_usec;
		}

		timeradd(&my_stats->tv_rtt_cumul, &tv_rtt, &my_stats->tv_rtt_cumul);
		my_stats->num ++;
	}
}

/** Print per-workflow detailed statistics.
 */
static void rc_print_wf_stats(FILE *fp)
{
	/* ensure there is something to print */
	int i;
	int i_start = 0;
	int num_stat = 0;
	for (i=1; i<RC_WF_MAX; i++) {
		if (stats.wf_stats[i].num > 0) num_stat ++;
	}
	if (num_stat == 0) return;

	fprintf(fp, "*** Statistics (per-workflow):\n");

	if (num_stat == 1) i_start = 1; /* only print "All" if we have more than one (otherwise it's redundant). */

	for (i=i_start; i<RC_WF_MAX; i++) {
		rc_wf_stats_t *my_stats = &stats.wf_stats[i];

		if (my_stats->num == 0) continue;

		float avg_rtt = 1000 * rc_timeval_to_float(&my_stats->tv_rtt_cumul) / my_stats->num;
		float min_rtt = 1000 * rc_timeval_to_float(&my_stats->tv_rtt_min);
		float max_rtt = 1000 * rc_timeval_to_float(&my_stats->tv_rtt_max);

		/* Only print rate if scenario lasted at least a little time. */
		if (rc_get_elapsed() < 200) {
			fprintf(fp, "\t%-*.*s:  nb: %d, RTT (ms): [avg: %.3f, min: %.3f, max: %.3f]\n",
				LG_PAD_WF_TYPES, LG_PAD_WF_TYPES, rc_wf_types[i], my_stats->num, avg_rtt, min_rtt, max_rtt);
		} else {
			fprintf(fp, "\t%-*.*s:  nb: %d, RTT (ms): [avg: %.3f, min: %.3f, max: %.3f], rate (avg/s): %.3f\n",
				LG_PAD_WF_TYPES, LG_PAD_WF_TYPES, rc_wf_types[i], my_stats->num, avg_rtt, min_rtt, max_rtt,
				rc_get_wf_rate(i));
		}
	}
}

/** Do summary / statistics (if asked for).
 */
static void rc_summary(void)
{
	if (!do_summary) return;

	FILE *fp = stdout;

	fprintf(fp, "*** Statistics summary:\n");

	if (stats.nb_started == stats.nb_eap) {
		/* Only EAP. */
		fprintf(fp, "\t%-*.*s: %u\n", LG_PAD_STATS, LG_PAD_STATS, "EAP transactions", stats.nb_started);
	} else if (stats.nb_eap == 0) {
		/* No EAP. Label those as "Requests". */
		fprintf(fp, "\t%-*.*s: %u\n", LG_PAD_STATS, LG_PAD_STATS, "Requests", stats.nb_started);
	} else {
		/* Bit of both. */
		fprintf(fp, "\t%-*.*s: %u (with EAP: %u)\n", LG_PAD_STATS, LG_PAD_STATS, "Transactions", stats.nb_started, stats.nb_eap);
	}

	fprintf(fp, "\t%-*.*s: %u\n", LG_PAD_STATS, LG_PAD_STATS, "Success", stats.nb_success);
	fprintf(fp, "\t%-*.*s: %u\n", LG_PAD_STATS, LG_PAD_STATS, "Fail", stats.nb_fail);
	fprintf(fp, "\t%-*.*s: %u\n", LG_PAD_STATS, LG_PAD_STATS, "Lost", stats.nb_lost);
	fprintf(fp, "\t%-*.*s: %u (retries: %u)\n", LG_PAD_STATS, LG_PAD_STATS, "Packets sent", stats.nb_packets_sent, stats.nb_packets_retries);
	fprintf(fp, "\t%-*.*s: %u\n", LG_PAD_STATS, LG_PAD_STATS, "Packets received", stats.nb_packets_recv);

	rc_print_wf_stats(fp);
}

/** Get elapsed time (in ms).
 */
static uint32_t rc_get_elapsed(void)
{
	uint32_t u_ms_elapsed;
	struct timeval tv_elapsed;

	if (timerisset(&tv_end)) {
		timersub(&tv_end, &tv_start, &tv_elapsed);
	} else {
		struct timeval tv_now;
		gettimeofday(&tv_now, NULL);
		timersub(&tv_now, &tv_start, &tv_elapsed);
	}

	u_ms_elapsed = (tv_elapsed.tv_sec * 1000) + (tv_elapsed.tv_usec/1000);

	return u_ms_elapsed;
}

/** Compute the started transactions rate /s.
 */
static float rc_get_start_rate(void)
{
	uint32_t u_ms_elapsed = rc_get_elapsed();

	if (u_ms_elapsed > 0) { /* should always be the case, but just to be sure. */
		return (float)(num_started * 1000) / (float)u_ms_elapsed;
	}
	return 0;
}

/** Compute the rate /s of a given workflow type.
 */
static float rc_get_wf_rate(rc_wf_type_t i)
{
	rc_wf_stats_t *my_stats = &stats.wf_stats[i];
	uint32_t u_ms_elapsed = rc_get_elapsed();

	if (u_ms_elapsed > 0) { // should always be the case, just to be sure.
		return (float)(my_stats->num * 1000) / (float)u_ms_elapsed;
	}
	return 0;
}

/** Display simple progress statistics.
 */
static void rc_do_progress_stat(void)
{
	if (!do_output || !progress_interval) return;

	printf("STAT (%s):", ELAPSED);

	printf(" %.2f%%", (100 * (float)num_started / num_input));
	printf(", start: %u (on: %u, ok: %u, fail: %u, lost: %u)",
		num_started, num_ongoing, stats.nb_success, stats.nb_fail, stats.nb_lost);

	printf(", rate (/s): %.1f", rc_get_start_rate());

	printf("\n");
}



int main(int argc, char **argv)
{
	char *p;
	int c;
	char *filename = NULL;
	FILE *fp;
	fr_dict_t *dict = NULL;

	static fr_log_t radclient_log = {
		.colourise = true,
		.fd = STDOUT_FILENO,
		.dst = L_DST_STDOUT,
		.file = NULL,
	};

	/*
	 *	We probably don't want to free the talloc autofree context
	 *	directly, so we'll allocate a new context beneath it, and
	 *	free that before any leak reports.
	 */
	autofree = talloc_init("main");

	fr_debug_lvl = 0;
	fr_log_fp = stdout;

	set_radius_dir(autofree, RADIUS_DIR);

	while ((c = getopt(argc, argv, "46c:d:D:f:hn:o:p:qr:sS:t:vxX")) != EOF)
	{
		switch (c) {
		case '4':
			force_af = AF_INET;
			break;

		case '6':
			force_af = AF_INET6;
			break;

		case 'c':
			if (!isdigit((int) *optarg)) usage();
			recycle_count = atoi(optarg);
			if (recycle_count == 0) recycle_count = 1;
			break;

		case 'd':
			set_radius_dir(autofree, optarg);
			break;

		case 'D':
			main_config.dictionary_dir = talloc_typed_strdup(NULL, optarg);
			break;

		case 'f':
			filename = optarg;
			break;

		case 'n':
			rate_limit = atoi(optarg);
			if (rate_limit == 0) usage();
			break;

		case 'o':
			progress_interval = atof(optarg);
			if (progress_interval < 0.1) usage();
			break;

		case 'p':
			parallel = atoi(optarg);
			if (parallel == 0) parallel = 1;
			if (parallel > 65536) parallel = 65536;
			break;

		case 'q':
			do_output = false;
			break;

		case 'x':
			rad_debug_lvl++;
			fr_debug_lvl++;
			break;

		case 'X':
#if 0
			sha1_data_problems = 1; /* for debugging only */
#endif
			break;

		case 'r':
			if (!isdigit((int) *optarg))
				usage();
			retries = atoi(optarg);
			break;

		case 's':
			do_summary = true;
			break;

		case 't':
			if (!isdigit((int) *optarg))
				usage();
			timeout = atof(optarg);
			break;

		case 'v':
			printf("%s: %s\n", progname, radiusd_version);
			exit(EXIT_SUCCESS);

		case 'S':
			fp = fopen(optarg, "r");
			if (!fp) {
				ERROR("Error opening %s: %s", optarg, fr_syserror(errno));
				exit(1);
			}
			if (fgets(filesecret, sizeof(filesecret), fp) == NULL) {
				ERROR("Error reading %s: %s",
				       optarg, fr_syserror(errno));
				exit(1);
			}
			fclose(fp);

			/* truncate newline */
			p = filesecret + strlen(filesecret) - 1;
			while ((p >= filesecret) &&
			       (*p < ' ')) {
					*p = '\0';
					--p;
			}

			if (strlen(filesecret) < 2) {
				ERROR("Secret in %s is too short", optarg);
				exit(1);
			}
			secret = filesecret;
			break;

		case 'h':
		default:
			usage();
		}
	}
	argc -= (optind - 1);
	argv += (optind - 1);

	if ((argc < 3)  ||
	    ((!secret) && (argc < 4))) {
		usage();
	}

	/* Initialize logging */
	if (!do_output) {
		rad_debug_lvl = 0;
		fr_debug_lvl = 0;
		radclient_log.dst = L_DST_NULL;
		radclient_log.fd = 0;
	}
	radlog_init(&radclient_log, false);

	/* Prepare progress report time. */
	rc_float_to_timeval(&tv_progress_interval, progress_interval);

	/* Prepare the timeout. */
	rc_float_to_timeval(&tv_timeout, timeout);

	if (!main_config.dictionary_dir) {
		main_config.dictionary_dir = DICTDIR;
	}

	/*
	 *	Read the distribution dictionaries first, then
	 *	the ones in raddb.
	 */
	DEBUG2("including dictionary file %s/%s", main_config.dictionary_dir, RADIUS_DICTIONARY);
	if (fr_dict_init(autofree, &dict, main_config.dictionary_dir, RADIUS_DICTIONARY, "radius") != 0) {
		ERROR("Errors reading dictionary: %s", fr_strerror());
		exit(1);
	}

	/*
	 *	It's OK if this one doesn't exist.
	 */
	int rcode = fr_dict_read(dict, radius_dir, RADIUS_DICTIONARY);
	if (rcode == -1) {
		ERROR("Errors reading %s/%s: %s", radius_dir, RADIUS_DICTIONARY, fr_strerror());
		exit(1);
	}

	/*
	 *	We print this after reading it.  That way if
	 *	it doesn't exist, it's OK, and we don't print
	 *	anything.
	 */
	if (rcode == 0) {
		DEBUG2("Including dictionary file %s/%s", radius_dir, RADIUS_DICTIONARY);
	}

	/*
	 *	Get the request type
	 */
	if (!isdigit((int) argv[2][0])) {
		packet_code = fr_str2int(fr_request_types, argv[2], -2);
		if (packet_code == -2) {
			ERROR("Unrecognised request type \"%s\"", argv[2]);
			usage();
		}
	} else {
		packet_code = atoi(argv[2]);
	}

	/*
	 *	Resolve hostname.
	 */
	rc_resolve_hostname(argv[1]);

	/*
	 *	Add the secret.
	 */
	if (argv[3]) secret = argv[3];

	/*
	 *	Read input data vp(s) from the file (or stdin).
	 */
	INFO("Loading input data...");
	if (!rc_load_input(autofree, filename, &rc_vps_list_in, 0)
	    || rc_vps_list_in.size == 0) {
		ERROR("No valid input. Nothing to send.");
		exit(EXIT_FAILURE);
	}
	INFO("Loaded: %d input element(s).", rc_vps_list_in.size);

	/* Initialize the packets list. */
	MEM(pl = fr_packet_list_create(1));

	/* Initialize the events list. */
	ev_list = fr_event_list_create(autofree, NULL);
	if (!ev_list) {
		ERROR("Failed to create event list");
		exit(1);
	}

	/* Keep track of elapsed time. */
	gettimeofday(&tv_start, NULL);

	/* Arm progress statistics */
	rc_evprep_progress_stat();

	/*
	 *	Start main loop.
	 */
	rc_main_loop();

	rc_do_progress_stat(); /* one last time. */
	gettimeofday(&tv_end, NULL);

	/*
	 *	Do summary / statistics (if asked for).
	 */
	rc_summary();

	talloc_free(autofree);

	return 0;
}



/** Given a radius request with some attributes in the EAP range, build
 *  them all into a single EAP-Message body.
 *
 *  If there are multiple eligibles EAP-Type, the first one is picked.
 *  Function returns 0 if no EAP is involved, or the EAP-Type otherwise.
 */
static int rc_map_eap_methods(RADIUS_PACKET *req)
{
	VALUE_PAIR *vp, *vpnext;
	int id, eapcode;
	int eap_method = 0;

	eap_packet_t *pt_ep = talloc_zero(req, eap_packet_t);

	vp = fr_pair_find_by_num(req->vps, 0, PW_EAP_ID, TAG_ANY);
	if (!vp) {
		id = ((int)getpid() & 0xff);
	} else {
		id = vp->vp_integer;
	}

	vp = fr_pair_find_by_num(req->vps, 0, PW_EAP_CODE, TAG_ANY);
	if (!vp) {
		eapcode = PW_EAP_REQUEST;
	} else {
		eapcode = vp->vp_integer;
	}

	for (vp = req->vps; vp != NULL; vp = vpnext) {
		/* save it in case it changes! */
		vpnext = vp->next;

		if (vp->da->attr >= PW_EAP_TYPE_BASE &&
		   vp->da->attr < PW_EAP_TYPE_BASE+256) {
			break;
		}
	}

	if (!vp) {
		return 0;
	}

	eap_method = vp->da->attr - PW_EAP_TYPE_BASE;

	switch (eap_method) {
	case PW_EAP_IDENTITY:
	case PW_EAP_NOTIFICATION:
	case PW_EAP_NAK:
	case PW_EAP_MD5:
	case PW_EAP_OTP:
	case PW_EAP_GTC:
	case PW_EAP_TLS:
	case PW_EAP_LEAP:
	case PW_EAP_TTLS:
	case PW_EAP_PEAP:
	default:
		/*
		 * no known special handling, it is just encoded as an
		 * EAP-message with the given type.
		 */

		/* nuke any existing EAP-Messages */
		fr_pair_delete_by_num(&req->vps, 0, PW_EAP_MESSAGE, TAG_ANY);

		pt_ep->code = eapcode;
		pt_ep->id = id;
		pt_ep->type.num = eap_method;
		pt_ep->type.length = vp->vp_length;

		pt_ep->type.data = talloc_memdup(vp, vp->vp_octets, vp->vp_length);
		talloc_set_type(pt_ep->type.data, uint8_t);

		eap_basic_compose(req, pt_ep);
	}

	return eap_method;
}

/** Given a radius request with an EAP-Message body, decode its specific
 *  attributes.
 */
static void rc_unmap_eap_methods(RADIUS_PACKET *rep)
{
	VALUE_PAIR *eap1;
	eap_packet_raw_t *e;
	int len;
	int type;

	if (!rep) return;

	/* find eap message */
	e = eap_vp2packet(NULL, rep->vps);
	if (!e) {
		ERROR("%s", fr_strerror());
		return;
	}
	/* create EAP-ID and EAP-CODE attributes to start */
	eap1 = fr_pair_afrom_num(rep, 0, PW_EAP_ID);
	eap1->vp_integer = e->id;
	fr_pair_add(&(rep->vps), eap1);

	eap1 = fr_pair_afrom_num(rep, 0, PW_EAP_CODE);
	eap1->vp_integer = e->code;
	fr_pair_add(&(rep->vps), eap1);

	switch (e->code) {
	default:
	case PW_EAP_SUCCESS:
	case PW_EAP_FAILURE:
		/* no data */
		break;

	case PW_EAP_REQUEST:
	case PW_EAP_RESPONSE:
		/* there is a type field, which we use to create
		 * a new attribute */

		/* the length was decode already into the attribute
		 * length, and was checked already. Network byte
		 * order, just pull it out using math.
		 */
		len = e->length[0]*256 + e->length[1];

		/* verify the length is big enough to hold type */
		if (len < 5)
		{
			talloc_free(e);
			return;
		}

		type = e->data[0];

		type += PW_EAP_TYPE_BASE;
		len -= 5;

		if (len > FR_MAX_STRING_LEN) {
			len = FR_MAX_STRING_LEN;
		}

		eap1 = fr_pair_afrom_num(rep, 0, type);
		fr_pair_value_memcpy(eap1, e->data + 1, len);

		fr_pair_add(&(rep->vps), eap1);
		break;
	}

	talloc_free(e);
	return;
}

static int rc_map_eap_sim_types(RADIUS_PACKET *r)
{
	int ret;

	eap_packet_t *pt_ep = talloc_zero(r, eap_packet_t);

	ret = eap_sim_encode(r, pt_ep);

	if (ret != 1) {
		return ret;
	}

	eap_basic_compose(r, pt_ep);

	return 1;
}

static int rc_unmap_eap_sim_types(RADIUS_PACKET *r)
{
	VALUE_PAIR	     *esvp;
	uint8_t *eap_data;
	int rcode_unmap;

	esvp = fr_pair_find_by_num(r->vps, 0, PW_EAP_TYPE_BASE + PW_EAP_SIM, TAG_ANY);
	if (!esvp) {
		ERROR("eap: EAP-Sim attribute not found");
		return 0;
	}

	eap_data = talloc_memdup(esvp, esvp->vp_octets, esvp->vp_length);
	talloc_set_type(eap_data, uint8_t);

	rcode_unmap = eap_sim_decode(r, eap_data, esvp->vp_length);

	talloc_free(eap_data);
	return rcode_unmap;
}

