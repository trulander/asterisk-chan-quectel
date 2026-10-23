/*
 * MT delivery task.
 *
 * Delivers incoming SMS (received on SIM and stored in mt_queue) to a SIP
 * endpoint (Flexisip) via res_pjsip MESSAGE, tracks the delivery state in the
 * SMS DB, retries on failure with exponential backoff and serves a small
 * READ-ONLY HTTP page (SIM status + unified SMS history in HTML/JSON).
 */

/* Because ast_config.h expects _GNU_SOURCE for some headers (see at_read.c) */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "ast_config.h"

#include <asterisk/astobj2.h>
#include <asterisk/channel.h> /* ast_waitfor_n_fd() */
#include <asterisk/json.h>
#include <asterisk/logger.h>
#include <asterisk/res_pjsip.h>

#include "at_command.h"
#include "at_response.h"
#include "at_read.h" /* at_wait() */
#include "chan_quectel.h"
#include "eventfd.h"
#include "helpers.h" /* fd_write_all() */
#include "mt_task.h"
#include "smsdb.h"
#include "web_util.h"
#include "web_incoming_parts.h"

// ================================================================

#define MT_CMD_STOP 2

/* one per mid; lives for one MESSAGE transaction send */
struct mt_msg_token {
    char mid[MT_MID_LEN];
    int attempts; /*!< attempts at the moment we scheduled this send */
};

static void mt_msg_token_destroy(void* obj)
{
    /* token carries only inline data; nothing to do */
}

/* one per fire-and-forget IMDN receipt transaction */
struct mt_imdn_token {
    char imdn_id[MT_IMDN_ID_LEN];
    char user[DEVNAMELEN];
};

static void mt_imdn_token_destroy(void* obj)
{
    /* token carries only inline data; nothing to do */
}

static pthread_t mt_retry_thread = AST_PTHREADT_NULL;
static pthread_t mt_web_thread   = AST_PTHREADT_NULL;

static int mt_kick_fd = -1;         /*!< eventfd: retry thread wake-up (kick/stop) */
static int mt_stop_fd = -1;         /*!< eventfd: web thread stop */
static int mt_web_listen_fd = -1;   /*!< mini web listen socket */
static volatile int mt_task_started = 0; /*!< state guard */
static int mt_inflight = 0;         /*!< MESSAGE transactions still awaiting a final callback */

// ================================================================

/*!
 * \brief Generate a unique message id for the MT queue, e.g. sim1-<sec>-<seq>.
 */
void mt_gen_mid(const char* device, char* mid, size_t len)
{
    static unsigned long seq;
    struct timeval tv;

    gettimeofday(&tv, NULL);
    const unsigned long n = __sync_add_and_fetch(&seq, 1);
    snprintf(mid, len, "%s-%ld-%lx", S_OR(device, "dev"), (long)tv.tv_sec, n);
}

/*!
 * \brief Extract the host part of a SIP URI (strips ';' params and trailing '>').
 */
static void mt_extract_domain(const char* sip_uri, char* domain, size_t len)
{
    if (domain && len) {
        domain[0] = '\0';
    }

    if (ast_strlen_zero(sip_uri) || !domain || len == 0) {
        return;
    }

    const char* const at = strrchr(sip_uri, '@');
    if (!at) {
        return;
    }

    const char* const start = at + 1;
    const size_t n = strcspn(start, ">;");
    if (n < len) {
        memcpy(domain, start, n);
        domain[n] = '\0';
    }
}

/*!
 * \brief Compute the retry backoff delay in seconds for the given attempt count.
 */
static int mt_backoff_seconds(int attempts)
{
    const int base  = CONF_GLOBAL(mt_retry_base);
    const int maxd  = CONF_GLOBAL(mt_retry_max_delay);
    const int shift = (attempts < 8) ? attempts : 8;

    long delay = (long)base << shift; /* base * 2^min(attempts, 8) */
    if (delay > maxd) {
        delay = maxd;
    }
    delay += (long)(ast_random() % 11); /* jitter 0..10 */

    return (int)delay;
}

/*!
 * \brief Record a failed delivery attempt: state=retrying with backoff.
 */
static void mt_send_fail(const struct smsdb_mt_row* row, const char* reason)
{
    const long now  = time(NULL);
    const int attempts = row->attempts + 1;
    const int max_attempts = CONF_GLOBAL(mt_retry_max_attempts);

    if (attempts >= max_attempts) {
        ast_log(LOG_ERROR, "mt: MESSAGE mid=%s failed, max attempts (%d) reached (%s)\n",
            row->mid, max_attempts, S_OR(reason, "?"));
        smsdb_mt_set_result(row->mid, "retrying", attempts, 0, "max attempts reached", 0, 0);
        return;
    }

    const int this_delay = mt_backoff_seconds(attempts);
    const int next_retry_at = (int)now + this_delay;
    ast_log(LOG_NOTICE, "mt: MESSAGE mid=%s send failed (%s) -> retrying, attempt %d/%d, next in ~%ds\n",
        row->mid, S_OR(reason, "?"), attempts, max_attempts, this_delay);
    smsdb_mt_set_result(row->mid, "retrying", attempts, next_retry_at, reason, 0, 0);
}

/*!
 * \brief Callback invoked when the MESSAGE transaction terminates.
 */
static void mt_msg_response_callback(void* token, pjsip_event* e)
{
    struct mt_msg_token* const tok = token;
    const long now                  = time(NULL);
    char last_result[MT_RESULT_LEN];
    int status_code = 0;
    int delivered   = 0;
    int queued      = 0;

    if (ast_strlen_zero(tok->mid)) {
        ao2_ref(tok, -1);
        __sync_sub_and_fetch(&mt_inflight, 1);
        return;
    }

    switch (e->body.tsx_state.type) {
        case PJSIP_EVENT_RX_MSG:
            status_code = e->body.tsx_state.tsx ? e->body.tsx_state.tsx->status_code : 0;
            break;

        case PJSIP_EVENT_TIMER:
            status_code = 408; /* request timed out */
            break;

        default:
            /* transport / tsx state failure */
            status_code = 0;
            break;
    }

    if (status_code == 202) {
        /* Flexisip accepted while the phone is offline (will hold the message) */
        ast_copy_string(last_result, "202 accepted (queued)", sizeof(last_result));
        queued = 1;
        ast_log(LOG_NOTICE, "mt: MESSAGE mid=%s -> queued (%s)\n", tok->mid, last_result);
    } else if (status_code >= 200 && status_code < 300) {
        /* phone online, Flexisip forwarded and got a 2xx from the phone */
        snprintf(last_result, sizeof(last_result), "%d delivered", status_code);
        delivered = 1;
        ast_log(LOG_NOTICE, "mt: MESSAGE mid=%s -> delivered (SIP %d)\n", tok->mid, status_code);
    } else if (status_code >= 100 && status_code < 200) {
        /* 110 Push / provisional: phone offline, Flexisip keeps the message */
        snprintf(last_result, sizeof(last_result), "%d provisional (queued)", status_code);
        queued = 1;
        ast_log(LOG_NOTICE, "mt: MESSAGE mid=%s -> queued (%s)\n", tok->mid, last_result);
    } else {
        /* 3xx/4xx/5xx/transport: schedule a retry with backoff */
        const int attempts = tok->attempts + 1;
        const int max_attempts = CONF_GLOBAL(mt_retry_max_attempts);
        if (status_code) {
            snprintf(last_result, sizeof(last_result), "SIP %d", status_code);
        } else {
            ast_copy_string(last_result, "transport error", sizeof(last_result));
        }
        if (attempts >= max_attempts) {
            ast_log(LOG_ERROR, "mt: MESSAGE mid=%s max attempts (%d) reached (%s)\n",
                tok->mid, max_attempts, last_result);
            smsdb_mt_set_result(tok->mid, "retrying", attempts, 0, "max attempts reached", 0, 0);
        } else {
            const int this_delay = mt_backoff_seconds(attempts);
            const int next_retry_at = (int)now + this_delay;
            ast_log(LOG_NOTICE, "mt: MESSAGE mid=%s -> retrying (attempt %d/%d, %s, next in ~%ds)\n",
                tok->mid, attempts, max_attempts, last_result, this_delay);
            smsdb_mt_set_result(tok->mid, "retrying", attempts, next_retry_at, last_result, 0, 0);
        }
        ao2_ref(tok, -1);
        __sync_sub_and_fetch(&mt_inflight, 1);
        return;
    }

    if (delivered) {
        smsdb_mt_set_result(tok->mid, "delivered", tok->attempts, 0, last_result, (int)now, (int)now);
    } else if (queued) {
        smsdb_mt_set_result(tok->mid, "queued", tok->attempts, 0, last_result, (int)now, 0);
    }

    ao2_ref(tok, -1);
    __sync_sub_and_fetch(&mt_inflight, 1);
}

/*!
 * \brief Extract the user part of a sip:user@domain URI.
 * Returns a pointer to \a out, or NULL when there is no user.
 */
static const char* mt_sip_to_user(const char* uri, char* out, size_t len)
{
    const char* p = uri;
    const char* at;
    size_t n;

    if (ast_strlen_zero(uri)) {
        return NULL;
    }
    if (ast_begins_with(p, "sips:")) {
        p += 5;
    } else if (ast_begins_with(p, "sip:")) {
        p += 4;
    }
    at = strchr(p, '@');
    if (!at) {
        return NULL; /* user must be followed by @host; no @ => no user */
    }
    n = (size_t)(at - p);
    if (n >= len) {
        n = len - 1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    return out;
}

/*!
 * \brief Format the current UTC time as RFC 3339 (\c YYYY-MM-DDTHH:MM:SSZ).
 */
static void mt_rfc3339_now(char* out, size_t len)
{
    const time_t now = time(NULL);
    struct tm tm;

    if (!len) {
        return;
    }
    out[0] = '\0';
    if (gmtime_r(&now, &tm)) {
        strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
    }
}


/*!
 * \brief Build and send one MESSAGE for a mt_queue row.
 */
static void mt_send_row(const struct smsdb_mt_row* row)
{
    struct smsdb_mt_row cur = *row;
    char sip_to[PATHLEN];
    char dialtarget[PATHLEN];
    char endpoint_name[DEVNAMELEN];
    char domain[128];
    char from_buf[256];
    char msgid_hdr[320]; /* <mid>@<domain>: mid<=128 + '@' + domain<=128 fits */
    pjsip_tx_data* tdata = NULL;
    struct mt_msg_token* token;
    int res;

    if (ast_strlen_zero(cur.mid) || ast_strlen_zero(cur.device)) {
        return;
    }

    /* Protect against re-picking while the async send is in flight. */
    smsdb_mt_set_result(cur.mid, "delivering", cur.attempts, cur.next_retry_at, cur.last_result, (int)cur.sent_at, (int)cur.delivered_at);

    /* read the per-device SIP config (short lock window) */
    {
        RAII_VAR(struct pvt*, pvt, pvt_find(cur.device), pvt_unlock);
        if (!pvt) {
            mt_send_fail(&cur, "device not found");
            return;
        }
        ast_copy_string(sip_to, S_OR(CONF_UNIQ(pvt, sms_sip_to), ""), sizeof(sip_to));
        ast_copy_string(endpoint_name, S_OR(CONF_UNIQ(pvt, sms_sip_endpoint), ""), sizeof(endpoint_name));
    }

    if (ast_strlen_zero(sip_to) || ast_strlen_zero(endpoint_name)) {
        mt_send_fail(&cur, "sms_sip_to/sms_sip_endpoint not configured");
        return;
    }

    RAII_VAR(char*, uri, NULL, ast_free);
    RAII_VAR(struct ast_sip_endpoint*, endpoint, NULL, ao2_cleanup);

    /*
     * Resolve by dialstring form PJSIP/user@endpoint (same as the known-good
     * MessageSend path). ast_sip_get_endpoint() then returns uri = the AOR contact
     * URI WITH the destination user inserted, e.g. sip:1001@sipsis.keblog.ru:443;transport=tls.
     * Routing (and Flexisip's 404-vs-2xx decision) is keyed on the Request-URI userpart,
     * so the R-URI MUST carry the user
     *   - a bare contact URI (no user) makes Flexisip answer 404 (no-contact-for-aor);
     *   - a bare user URI without host:port;transport=tls fails transport selection.
     * To keeps the real destination URI from sms_sip_to.
     */
    {
        char user[DEVNAMELEN];
        const char* u = mt_sip_to_user(sip_to, user, sizeof(user));
        if (!u || ast_strlen_zero(u)) {
            mt_send_fail(&cur, "sms_sip_to has no user part");
            return;
        }
        snprintf(dialtarget, sizeof(dialtarget), "PJSIP/%s@%s", u, endpoint_name);
    }
    endpoint = ast_sip_get_endpoint(dialtarget, 1, &uri);
    if (!endpoint) {
        mt_send_fail(&cur, "SIP endpoint not found");
        return;
    }

    if (ast_sip_create_request("MESSAGE", NULL, endpoint, uri, NULL, &tdata)) {
        mt_send_fail(&cur, "could not create MESSAGE request");
        return;
    }

    ast_sip_update_to_uri(tdata, sip_to);

    /* From: sip:<from_num>@<domain> */
    mt_extract_domain(sip_to, domain, sizeof(domain));
    if (!ast_strlen_zero(cur.from_num) && !ast_strlen_zero(domain)) {
        snprintf(from_buf, sizeof(from_buf), "sip:%s@%s", cur.from_num, domain);
        ast_sip_update_from(tdata, from_buf);
    }

    /* Stable Message-ID so retries of the same mid are de-duplicated. */
    if (!ast_strlen_zero(domain)) {
        snprintf(msgid_hdr, sizeof(msgid_hdr), "%s@%s", cur.mid, domain);
        ast_sip_add_header(tdata, "Message-ID", msgid_hdr);
    }

    /* Plain text body (known-good path): message/cpim was rejected by the
     * receiving client (488) and made it auto-echo IMDN receipts back, which
     * Asterisk's res_pjsip_messaging rejects with 415. The phone accepts and
     * delivers text/plain; Flexisip answers the terminal 200 when online. */
    struct ast_sip_body body = {
        .type      = "text",
        .subtype   = "plain",
        .body_text = cur.msg,
    };
    ast_sip_add_body(tdata, &body);
    if (!tdata->msg->body) {
        pjsip_tx_data_dec_ref(tdata);
        mt_send_fail(&cur, "could not add message body");
        return;
    }

    token = ao2_alloc_options(sizeof(*token), mt_msg_token_destroy, AO2_ALLOC_OPT_LOCK_NOLOCK);
    if (!token) {
        pjsip_tx_data_dec_ref(tdata);
        mt_send_fail(&cur, "out of memory");
        return;
    }
    ast_copy_string(token->mid, cur.mid, sizeof(token->mid));
    token->attempts = cur.attempts;

    /* count before send so a synchronous callback cannot leave the counter stale */
    __sync_add_and_fetch(&mt_inflight, 1);
    ast_log(LOG_NOTICE, "mt: sending MESSAGE mid=%s (device %s from %s to %s, RURI %s) attempt %d/%d\n",
        cur.mid, cur.device, S_OR(cur.from_num, "?"), sip_to, uri, cur.attempts + 1,
        CONF_GLOBAL(mt_retry_max_attempts));
    res = ast_sip_send_request(tdata, NULL, endpoint, token, mt_msg_response_callback);
    if (res) {
        __sync_sub_and_fetch(&mt_inflight, 1);
        ao2_ref(token, -1);
        mt_send_fail(&cur, "send request failed");
        return;
    }
    /* success: the callback will finalize the row state */
}

/*!
 * \brief Light callback for a fire-and-forget IMDN receipt (log only, no retry).
 */
static void mt_imdn_response_callback(void* token, pjsip_event* e)
{
    struct mt_imdn_token* const tok = token;
    int status_code = 0;

    switch (e->body.tsx_state.type) {
        case PJSIP_EVENT_RX_MSG:
            status_code = e->body.tsx_state.tsx ? e->body.tsx_state.tsx->status_code : 0;
            break;
        case PJSIP_EVENT_TIMER:
            status_code = 408; /* request timed out */
            break;
        default:
            status_code = 0;
            break;
    }

    ast_log(LOG_NOTICE, "mt: IMDN receipt to %s (imdn=%s) status=%d\n",
        S_OR(tok->user, "?"), S_OR(tok->imdn_id, "?"), status_code);
    ao2_ref(tok, -1);
    __sync_sub_and_fetch(&mt_inflight, 1);
}

/*!
 * \brief Send a fire-and-forget RFC 5438 delivery notification
 * (message/imdn+xml, Content-Disposition: notification) to the phone AOR
 * after the modem reports the outgoing SMS as smsc_ok/delivered_to_handset
 * (delivered form) or failed (failed form). The phone maps it to
 * DeliveredToUser for the matching local MO message by imdn_id.
 *
 * Does not touch mt_queue and never retries.
 */
void mt_send_imdn_receipt(const char* device, const char* imdn_id, const char* to_gsm_node_hint, const char* state)
{
    char sip_to[PATHLEN];
    char dialtarget[PATHLEN];
    char endpoint_name[DEVNAMELEN];
    char user[DEVNAMELEN];
    char domain[128];
    char from_buf[256];
    char msgid_hdr[320];
    char body[1024];
    char dt[64];
    pjsip_tx_data* tdata = NULL;
    struct mt_imdn_token* token;
    int res;

    if (ast_strlen_zero(device) || ast_strlen_zero(imdn_id)) {
        return;
    }

    /* read the per-device SIP config (short lock window) */
    {
        RAII_VAR(struct pvt*, pvt, pvt_find(device), pvt_unlock);
        if (!pvt) {
            ast_log(LOG_WARNING, "mt: IMDN receipt: device '%s' not found\n", device);
            return;
        }
        ast_copy_string(sip_to, S_OR(CONF_UNIQ(pvt, sms_sip_to), ""), sizeof(sip_to));
        ast_copy_string(endpoint_name, S_OR(CONF_UNIQ(pvt, sms_sip_endpoint), ""), sizeof(endpoint_name));
    }

    if (ast_strlen_zero(sip_to) || ast_strlen_zero(endpoint_name)) {
        ast_log(LOG_WARNING, "mt: IMDN receipt: sms_sip_to/sms_sip_endpoint not configured for device %s\n", device);
        return;
    }

    const char* const u = mt_sip_to_user(sip_to, user, sizeof(user));
    if (ast_strlen_zero(u)) {
        ast_log(LOG_WARNING, "mt: IMDN receipt: sms_sip_to has no user part\n");
        return;
    }

    RAII_VAR(char*, uri, NULL, ast_free);
    RAII_VAR(struct ast_sip_endpoint*, endpoint, NULL, ao2_cleanup);

    /* Same dialstring resolution as mt_send_row: R-URI carries user+port+TLS. */
    snprintf(dialtarget, sizeof(dialtarget), "PJSIP/%s@%s", user, endpoint_name);
    endpoint = ast_sip_get_endpoint(dialtarget, 1, &uri);
    if (!endpoint) {
        ast_log(LOG_WARNING, "mt: IMDN receipt: SIP endpoint %s not found\n", endpoint_name);
        return;
    }

    if (ast_sip_create_request("MESSAGE", NULL, endpoint, uri, NULL, &tdata)) {
        ast_log(LOG_ERROR, "mt: IMDN receipt: could not create MESSAGE request\n");
        return;
    }

    ast_sip_update_to_uri(tdata, sip_to);

    /* From: sip:<gsm-number>@<domain> (the SMS recipient number is the sender of the receipt). */
    mt_extract_domain(sip_to, domain, sizeof(domain));
    if (!ast_strlen_zero(to_gsm_node_hint) && !ast_strlen_zero(domain)) {
        snprintf(from_buf, sizeof(from_buf), "sip:%s@%s", to_gsm_node_hint, domain);
        ast_sip_update_from(tdata, from_buf);
    }

    /* Unique Message-ID, disjoint from MT message ids. */
    if (!ast_strlen_zero(domain)) {
        char imdn_id_short[200];
        ast_copy_string(imdn_id_short, imdn_id, sizeof(imdn_id_short));
        snprintf(msgid_hdr, sizeof(msgid_hdr), "imdn-%s@%s", imdn_id_short, domain);
        ast_sip_add_header(tdata, "Message-ID", msgid_hdr);
    }

    /* IMDN XML body: delivered for smsc_ok/delivered_to_handset, failed otherwise. */
    {
        const int ok = ast_strlen_zero(state) || strcmp(state, "failed") != 0;
        mt_rfc3339_now(dt, sizeof(dt));
        if (ok) {
            snprintf(body, sizeof(body),
                     "<imdn xmlns=\"urn:ietf:params:xml:ns:imdn\"><message-id>%s</message-id>"
                     "<datetime>%s</datetime><delivery-notification><status><delivered/></status>"
                     "</delivery-notification></imdn>", imdn_id, dt);
        } else {
            snprintf(body, sizeof(body),
                     "<imdn xmlns=\"urn:ietf:params:xml:ns:imdn\"><message-id>%s</message-id>"
                     "<datetime>%s</datetime><delivery-notification><status><failed/></status>"
                     "<reason>SMSC error</reason></delivery-notification></imdn>", imdn_id, dt);
        }
    }

    struct ast_sip_body body_msg = {
        .type      = "message",
        .subtype   = "imdn+xml",
        .body_text = body,
    };
    ast_sip_add_body(tdata, &body_msg);
    if (!tdata->msg->body) {
        pjsip_tx_data_dec_ref(tdata);
        ast_log(LOG_WARNING, "mt: IMDN receipt: could not add body\n");
        return;
    }
    ast_sip_add_header(tdata, "Content-Disposition", "notification");

    token = ao2_alloc_options(sizeof(*token), mt_imdn_token_destroy, AO2_ALLOC_OPT_LOCK_NOLOCK);
    if (!token) {
        pjsip_tx_data_dec_ref(tdata);
        ast_log(LOG_WARNING, "mt: IMDN receipt: out of memory\n");
        return;
    }
    ast_copy_string(token->imdn_id, imdn_id, sizeof(token->imdn_id));
    ast_copy_string(token->user, user, sizeof(token->user));

    __sync_add_and_fetch(&mt_inflight, 1);
    ast_log(LOG_NOTICE, "mt: sending IMDN receipt imdn=%s (state=%s) to %s via %s\n", imdn_id, S_OR(state, "delivered"), sip_to, endpoint_name);
    res = ast_sip_send_request(tdata, NULL, endpoint, token, mt_imdn_response_callback);
    if (res) {
        __sync_sub_and_fetch(&mt_inflight, 1);
        ao2_ref(token, -1);
        ast_log(LOG_ERROR, "mt: IMDN receipt imdn=%s: send request failed (%d)\n", imdn_id, res);
        return;
    }
    /* fire-and-forget: the callback only logs the final status */
}

/*!
 * \brief Process all due mt_queue rows (new or retrying) once.
 */
static void mt_process_sms_db_read_retries(void)
{
    if (!gpublic) {
        return;
    }
    const int now = (int)time(NULL);
    struct pvt* pvt;
    AST_RWLIST_RDLOCK(&gpublic->devices);
    AST_RWLIST_TRAVERSE(&gpublic->devices, pvt, entry) {
        ast_mutex_lock(&pvt->lock);
        if (pvt->sms_db_retry_index >= 0 && pvt->sms_db_retry_at <= now && pvt->initialized &&
            CONF_SHARED(pvt, sms_autodelete) && !pvt->sms_verify_active) {
            const int idx = pvt->sms_db_retry_index;
            if (!at_enqueue_retrieve_sms(&pvt->sys_chan, idx, RES_CMTI)) {
                pvt->sms_db_retry_at = now + CONF_GLOBAL(mt_retry_interval);
            }
        }
        ast_mutex_unlock(&pvt->lock);
    }
    AST_RWLIST_UNLOCK(&gpublic->devices);
}

/** Выполнить один due delete-job за проход; недоступную SIM defer-ить для fairness.
 * SQLite mutex держится внутри smsdb API, не вокруг AT ожидания; AT queue получает rowid.
 */
static void mt_process_sms_delete_due(void)
{
    long long rowid = 0;
    char device[DEVNAMELEN];
    int index = -1;
    const int due = smsdb_incoming_delete_job(&rowid, device, sizeof(device), &index);
    if (due <= 0) {
        return;
    }
    RAII_VAR(struct pvt*, pvt, pvt_find(device), pvt_unlock);
    if (!pvt || !CONF_SHARED(pvt, sms_autodelete) || pvt->sms_verify_active || !pvt->initialized) {
        /* Недоступная/disabled/busy SIM не должна удерживать первый due row:
         * defer выполняется без расхода deletion attempt, следующий periodic pass
         * выберет другую job. При autodelete-off AT не отправляется. */
        if (smsdb_incoming_delete_defer(rowid, (int)time(NULL) + CONF_GLOBAL(mt_retry_interval))) {
            ast_log(LOG_WARNING, "[%s][SMS:%d] Could not defer temporarily non-actionable SIM delete job\n", device, index);
        }
        return;
    }
    if (at_enqueue_sms_delete_verify(&pvt->sys_chan, rowid, index)) {
        ast_log(LOG_WARNING, "[%s][SMS:%d] Could not queue periodic SIM-cell verification\n", device, index);
    }
}

static void mt_process_due(void)
{
    const int max_attempts = CONF_GLOBAL(mt_retry_max_attempts);
    struct smsdb_mt_row row;

    while (smsdb_mt_pick_retry((long)time(NULL), max_attempts, &row) == 1) {
        if (ast_strlen_zero(row.mid)) {
            break;
        }
        mt_send_row(&row);
    }
}

/*!
 * \brief Retry thread: process due rows, sleep until timeout/kick/stop.
 */
static void* mt_retry_threadproc(void* arg)
{
    while (1) {
        if (!mt_task_started) {
            break;
        }

        mt_process_due();
        mt_process_sms_delete_due();
        mt_process_sms_db_read_retries();
        if (!mt_task_started) {
            break;
        }

        int t = CONF_GLOBAL(mt_retry_interval) * 1000;
        if (t <= 0) {
            t = 1000;
        }

        const int outfd = at_wait(mt_kick_fd, &t);
        if (!mt_task_started) {
            break;
        }
        if (outfd == mt_kick_fd) {
            eventfd_t val = 0;
            if (eventfd_read(mt_kick_fd, &val)) {
                ast_log(LOG_ERROR, "mt: fail to read kick event - stopping retry thread\n");
                break;
            }
            /* kick: fall through to process the queue on the next iteration;
             * stop is noticed via the mt_task_started flag checked above and on
             * the next loop iteration (mt_task_stop clears the flag before
             * waking this thread). */
        }
        /* timeout or kick: loop and process */
    }
    return NULL;
}

// ================================================================
// Mini web: read-only SIM status + SMS history
//   GET /                   SIM status cards + recent SMS (paged)
//   GET /sms                index of per-SIM SMS pages
//   GET /sms/<sim>          SMS history of one SIM (paged, HTML)
//   GET /sms/<sim>.json     SMS history of one SIM (paged, JSON)
//   GET /status.json        read-only SIM status (JSON)
//   GET /json               legacy SMS JSON (mt_queue + mt_out)
// Only GET is accepted (anything else -> 405); unknown paths -> 404.
// No control/management endpoints are exposed.
// ================================================================

#define MT_WEB_PAGE_SIZE 10 /*!< SMS rows shown per page */

/*! Max length of the /sms/<sim> device-name tail we accept (device ids are
 * DEVNAMELEN=31 in practice; keep a little headroom). */
#define MT_WEB_SIM_MAX 64

static void mt_web_send(int client_fd, const char* status, const char* ctype, const char* body, int body_len)
{
    RAII_VAR(struct ast_str*, hdr, ast_str_create(256), ast_free);

    ast_str_set(&hdr, 0, "HTTP/1.1 %s\r\nContent-Type: %s\r\nConnection: close\r\nContent-Length: %d\r\n\r\n", status, ctype, body_len);

    fd_write_all(client_fd, ast_str_buffer(hdr), ast_str_strlen(hdr));
    fd_write_all(client_fd, body, body_len);
}

static void mt_web_ts(struct ast_str** buf, long epoch)
{
    char tmp[32];
    struct timeval tv;
    struct ast_tm tmv;

    if (epoch <= 0) {
        return;
    }

    tv.tv_sec  = epoch;
    tv.tv_usec = 0;
    ast_localtime(&tv, &tmv, NULL);
    ast_strftime(tmp, sizeof(tmp), "%Y-%m-%d %H:%M:%S", &tmv);
    ast_str_append(buf, 0, "%s", tmp);
}

/*! Append a numeric cell or an em-dash when the value carries no data
    (sentinels <= 0, or the 99 "unknown" convention). */
static void mt_web_num_cell(struct ast_str** buf, long v)
{
    ast_str_append(buf, 0, (v > 0 && v != 99) ? "%ld" : "&mdash;", v);
}

/*! Append a formatted epoch time or an em-dash when there is no data. */
static void mt_web_ts_cell(struct ast_str** buf, long epoch)
{
    if (epoch > 0) {
        mt_web_ts(buf, epoch);
    } else {
        ast_str_append(buf, 0, "&mdash;");
    }
}

static void mt_html_escape(struct ast_str** buf, const char* in)
{
    if (!in) {
        return;
    }
    for (const char* p = in; *p; ++p) {
        switch (*p) {
            case '&': ast_str_append(buf, 0, "&amp;"); break;
            case '<': ast_str_append(buf, 0, "&lt;"); break;
            case '>': ast_str_append(buf, 0, "&gt;"); break;
            case '"': ast_str_append(buf, 0, "&quot;"); break;
            case '\'': ast_str_append(buf, 0, "&#39;"); break;
            default: ast_str_append(buf, 0, "%c", *p); break;
        }
    }
}

/*! Percent-encode \a in for use inside an href (path segment, no query). */
static void mt_web_url_escape(struct ast_str** buf, const char* in)
{
    static const char hex[] = "0123456789ABCDEF";

    if (!in) {
        return;
    }
    for (const unsigned char* p = (const unsigned char*)in; *p; ++p) {
        const unsigned char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            ast_str_append(buf, 0, "%c", c);
        } else {
            ast_str_append(buf, 0, "%%%c%c", hex[c >> 4], hex[c & 0x0F]);
        }
    }
}

/*! Human readable RSSI dBm text; "unknown" when there is no usable signal. */
static void mt_web_rssi_dbm(int rssi, char* out, size_t len)
{
    RAII_VAR(struct ast_str*, s, (rssi > 0 && rssi < 32) ? rssi2dBm(rssi) : NULL, ast_free);

    if (s) {
        ast_copy_string(out, ast_str_buffer(s), len);
        return;
    }
    ast_copy_string(out, "unknown", len);
}

/*! Visual 0-4 signal bars (four slots, filled ones via the .on class). */
static void mt_web_bars(struct ast_str** buf, int bars)
{
    for (int i = 0; i < 4; ++i) {
        ast_str_append(buf, 0, i < bars ? "<span class=\"bar on\"></span>" : "<span class=\"bar\"></span>");
    }
}

/*! Previous/next page navigation for the SMS tables. base is the (already
 * percent-encoded) path prefix for the page links. */
static void mt_web_pagination(struct ast_str** buf, const char* base, int page, int pages)
{
    ast_str_append(buf, 0, "<div class=\"pager\">");
    if (page > 1) {
        ast_str_append(buf, 0, "<a href=\"%s?page=%d\">&laquo; prev</a> ", base, page - 1);
    } else {
        ast_str_append(buf, 0, "<span class=\"off\">&laquo; prev</span> ");
    }
    ast_str_append(buf, 0, "page %d / %d ", page, pages);
    if (page < pages) {
        ast_str_append(buf, 0, "<a href=\"%s?page=%d\">next &raquo;</a>", base, page + 1);
    } else {
        ast_str_append(buf, 0, "<span class=\"off\">next &raquo;</span>");
    }
    ast_str_append(buf, 0, "</div>");
}

/*! Key/value row for the SIM status card; k is a static literal, v is escaped. */
static void mt_web_kv(struct ast_str** buf, const char* k, const char* v)
{
    ast_str_append(buf, 0, "<tr><th>%s</th><td>", k);
    mt_html_escape(buf, v);
    ast_str_append(buf, 0, "</td></tr>");
}

static void mt_web_html_head(struct ast_str** buf, const char* title)
{
    ast_str_append(buf, 0,
                   "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>%s</title>"
                   "<style>"
                   "body{font-family:sans-serif;margin:1em}"
                   "a{color:#06c}"
                   ".nav{font-size:0.9em;margin:0.2em 0}"
                   ".hint{color:#555;font-size:0.85em}"
                   ".sim{border:1px solid #bbb;border-radius:6px;padding:0 1em 0.5em;margin-bottom:1em;max-width:64em}"
                   ".sim h3{margin:0.5em 0 0.3em}"
                   ".sim a.small{font-size:0.8em}"
                   ".state{font-size:0.8em;color:#fff;background:#4682b4;border-radius:3px;padding:1px 6px}"
                   "table{border-collapse:collapse;margin:0.5em 0 1em}"
                   "th,td{border:1px solid #999;padding:4px 8px;text-align:left;vertical-align:top}"
                   "th{background:#eee}"
                   "table.kv th{width:13em}"
                   "table.kv th,table.kv td{border:none;border-bottom:1px dotted #ccc;padding:2px 6px}"
                   ".bar{display:inline-block;width:8px;height:12px;background:#ddd;margin-right:2px;border:1px solid #bbb}"
                   ".bar.on{background:#4caf50;border-color:#2e7d32}"
                   ".dir{font-size:0.85em;white-space:nowrap;padding:1px 4px;border-radius:3px}"
                   ".dir.mt{background:#e3f2fd;color:#1565c0}"
                   ".dir.mo{background:#fff3e0;color:#e65100}"
                   ".pager{margin:0.5em 0 1.5em}"
                   ".pager .off{color:#999}"
                   ".empty{color:#777}"
                   "</style></head><body>", title);
}

/** Добавить общий navigation bar; ссылка Incoming parts не меняет legacy route links. */
static void mt_web_nav(struct ast_str** buf)
{
    ast_str_append(buf, 0,
                   "<p class=\"nav\"><a href=\"/\">SIM status</a> | <a href=\"/sms\">SMS history</a> | <a href=\"/sms/parts\">Incoming parts</a> | "
                   "<a href=\"/status.json\">Status JSON</a> | <a href=\"/json\">SMS JSON</a> &nbsp;|&nbsp; "
                   "<label for=\"mt-refresh\">auto-refresh:</label> "
                   "<select id=\"mt-refresh\" onchange=\"mt_setRefresh(this.value)\">"
                   "<option value=\"off\">off</option><option value=\"1\">1s</option>"
                   "<option value=\"2\">2s</option><option value=\"3\">3s</option>"
                   "<option value=\"4\">4s</option><option value=\"5\">5s</option>"
                   "<option value=\"10\">10s</option><option value=\"15\">15s</option>"
                   "<option value=\"30\">30s</option></select></p>"
                   "<script>"
                   "function mt_setRefresh(v){try{localStorage.setItem(\"mt_refresh\",v);}catch(e){}}"
                   "(function(){var sel=document.getElementById(\"mt-refresh\"),v=\"off\";"
                   "try{v=localStorage.getItem(\"mt_refresh\")||\"off\";}catch(e){}"
                   "if(sel)sel.value=v;var n=parseInt(v,10);if(n>0){setTimeout(function(){location.reload();},n*1000);}}" ")();"
                   "</script>");
}

/*! Parse and validate the ?page= query parameter (default/clamped to >= 1). */
static int mt_web_page_param(const char* query)
{
    int page = 1;
    char buf[32];

    if (web_util_query_param(query, "page", buf, sizeof(buf)) == 0 && web_util_parse_int(buf, &page) == 0 && page >= 1) {
        return page;
    }
    return 1;
}

struct mt_web_sms_ctx {
    struct ast_str** buf;
};

/*! Row renderer for the unified SMS history (direction explicitly tagged).
    The MT/mt_queue and MO/mt_out extra columns are shown in the same table
    with em-dashes for the columns the row's own direction does not carry. */
static int mt_html_sms_cb(void* ctx, const struct sms_paging_row* row)
{
    struct ast_str** const buf = ((struct mt_web_sms_ctx*)ctx)->buf;
    const int is_mt = strcmp(row->direction, "mt") == 0;

    /* time */
    ast_str_append(buf, 0, "<tr><td>");
    mt_web_ts(buf, row->created_at);
    ast_str_append(buf, 0, "</td>");
    /* direction */
    ast_str_append(buf, 0, "<td>");
    if (is_mt) {
        ast_str_append(buf, 0, "<span class=\"dir mt\">MT&nbsp;SIM&rarr;SIP</span>");
    } else {
        ast_str_append(buf, 0, "<span class=\"dir mo\">MO&nbsp;SIP&rarr;SIM</span>");
    }
    ast_str_append(buf, 0, "</td>");
    /* device */
    ast_str_append(buf, 0, "<td>");
    mt_html_escape(buf, row->device);
    ast_str_append(buf, 0, "</td>");
    /* peer */
    ast_str_append(buf, 0, "<td>");
    mt_html_escape(buf, row->peer);
    ast_str_append(buf, 0, "</td>");
    /* message */
    ast_str_append(buf, 0, "<td>");
    mt_html_escape(buf, row->msg);
    ast_str_append(buf, 0, "</td>");
    /* state */
    ast_str_append(buf, 0, "<td>");
    mt_html_escape(buf, row->state);
    ast_str_append(buf, 0, "</td>");
    /* retries */
    ast_str_append(buf, 0, "<td>");
    mt_web_num_cell(buf, row->attempts);
    ast_str_append(buf, 0, "</td>");
    /* last retry */
    ast_str_append(buf, 0, "<td>");
    if (ast_strlen_zero(row->last_result)) {
        ast_str_append(buf, 0, "&mdash;");
    } else {
        mt_html_escape(buf, row->last_result);
    }
    ast_str_append(buf, 0, "</td>");
    /* next retry */
    ast_str_append(buf, 0, "<td>");
    mt_web_ts_cell(buf, row->next_retry_at);
    ast_str_append(buf, 0, "</td>");
    /* sent */
    ast_str_append(buf, 0, "<td>");
    mt_web_ts_cell(buf, row->sent_at);
    ast_str_append(buf, 0, "</td>");
    /* delivered */
    ast_str_append(buf, 0, "<td>");
    mt_web_ts_cell(buf, row->delivered_at);
    ast_str_append(buf, 0, "</td>");
    /* delivery status report code */
    ast_str_append(buf, 0, "<td>");
    mt_web_num_cell(buf, row->srr_status);
    ast_str_append(buf, 0, "</td>");
    /* mr */
    ast_str_append(buf, 0, "<td>");
    mt_web_num_cell(buf, row->mr);
    ast_str_append(buf, 0, "</td></tr>");
    return 0;
}

/*! Log and send an HTTP 500 after a database error reading SMS history. */
static void mt_web_db_error(int client_fd, const char* what, const char* ctype, const char* body, int body_len)
{
    ast_log(LOG_ERROR, "mt: web: %s failed, returning HTTP 500\n", what);
    mt_web_send(client_fd, "500 Internal Server Error", ctype, body, body_len);
}

/*! Render one page of the unified SMS table. device == NULL means all SIMs.
 * base is the URL prefix for the page links (already percent-encoded).
 * Returns 0 on success and -1 on a database error; the caller must turn a -1
 * into an HTTP 500 so a DB failure never looks like a successful empty page. */
static int mt_web_sms_section(struct ast_str** buf, const char* device, const char* base, int page)
{
    struct mt_web_sms_ctx ctx = { buf };
    int total = 0;
    int pages = 1;
    int offset = 0;

    if (smsdb_sms_count(device, &total)) {
        return -1;
    }
    (void)web_util_page_meta(total, MT_WEB_PAGE_SIZE, &page, &pages, &offset);
    if (total <= 0) {
        ast_str_append(buf, 0, "<p class=\"empty\">No SMS records.</p>");
        return 0;
    }
    ast_str_append(buf, 0, "<table><tr><th>time</th><th>dir</th><th>device</th><th>peer</th><th>message</th>"
                   "<th>state</th><th>retries</th><th>last retry</th><th>next retry</th>"
                   "<th>sent</th><th>delivered</th><th>srr status</th><th>mr</th></tr>");
    if (smsdb_sms_page(device, MT_WEB_PAGE_SIZE, offset, &ctx, mt_html_sms_cb)) {
        return -1;
    }
    ast_str_append(buf, 0, "</table>");
    mt_web_pagination(buf, base, page, pages);
    return 0;
}

struct mt_status_section_ctx {
    struct ast_str** buf;
    int count;
};

/*! SIM status card renderer (called under a locking-safe snapshot). */
static int mt_html_status_cb(void* ctx, const struct pvt_status_snapshot* snap)
{
    struct mt_status_section_ctx* const sec = ctx;
    struct ast_str** const buf = sec->buf;
    char dbm[32];
    const int bars = web_util_rssi_bars(snap->rssi);

    mt_web_rssi_dbm(snap->rssi, dbm, sizeof(dbm));

    ast_str_append(buf, 0, "<div class=\"sim\"><h3>");
    mt_html_escape(buf, snap->name);
    ast_str_append(buf, 0, " <span class=\"state\">");
    mt_html_escape(buf, snap->state);
    ast_str_append(buf, 0, "</span> <a class=\"small\" href=\"/sms/");
    mt_web_url_escape(buf, snap->name);
    ast_str_append(buf, 0, "\">SMS history</a></h3><table class=\"kv\">");

    ast_str_append(buf, 0, "<tr><th>Link</th><td>");
    if (!snap->connected) {
        ast_str_append(buf, 0, "not connected");
    } else if (!snap->initialized) {
        ast_str_append(buf, 0, "connected, not initialized");
    } else {
        ast_str_append(buf, 0, "connected &amp; initialized");
    }
    ast_str_append(buf, 0, "</td></tr>");

    ast_str_append(buf, 0, "<tr><th>Operator / network</th><td>");
    if (ast_strlen_zero(snap->provider_name) && ast_strlen_zero(snap->network_name) && ast_strlen_zero(snap->short_network_name)) {
        ast_str_append(buf, 0, "n/a");
    } else {
        int first = 1;
        if (!ast_strlen_zero(snap->provider_name)) {
            mt_html_escape(buf, snap->provider_name);
            first = 0;
        }
        if (!ast_strlen_zero(snap->network_name)) {
            if (!first) {
                ast_str_append(buf, 0, " / ");
            }
            mt_html_escape(buf, snap->network_name);
            first = 0;
        }
        if (!ast_strlen_zero(snap->short_network_name)) {
            if (!first) {
                ast_str_append(buf, 0, " / ");
            }
            mt_html_escape(buf, snap->short_network_name);
        }
    }
    if (snap->operator > 0) {
        ast_str_append(buf, 0, " (PLMN %d)", snap->operator);
    }
    ast_str_append(buf, 0, "</td></tr>");

    ast_str_append(buf, 0, "<tr><th>Access technology</th><td>");
    mt_html_escape(buf, sys_act2str(snap->act));
    ast_str_append(buf, 0, "</td></tr>");

    ast_str_append(buf, 0, "<tr><th>Registration</th><td>");
    mt_html_escape(buf, gsm_regstate2str(snap->gsm_reg_status));
    ast_str_append(buf, 0, "</td></tr>");

    ast_str_append(buf, 0, "<tr><th>RSSI</th><td>%d, %s ", snap->rssi, dbm);
    mt_web_bars(buf, bars);
    ast_str_append(buf, 0, "</td></tr>");

    ast_str_append(buf, 0, "<tr><th>Audio</th><td>");
    if (ast_strlen_zero(snap->audio_format)) {
        ast_str_append(buf, 0, "n/a");
    } else {
        mt_html_escape(buf, snap->audio_format);
        ast_str_append(buf, 0, " (16 kHz: %s)", snap->audio_16khz ? "Yes" : "No");
    }
    ast_str_append(buf, 0, "</td></tr>");

    if (!ast_strlen_zero(snap->imei)) mt_web_kv(buf, "IMEI", snap->imei);
    if (!ast_strlen_zero(snap->imsi)) mt_web_kv(buf, "IMSI", snap->imsi);
    if (!ast_strlen_zero(snap->iccid)) mt_web_kv(buf, "ICCID", snap->iccid);
    if (!ast_strlen_zero(snap->location_area_code)) mt_web_kv(buf, "Location area code", snap->location_area_code);
    if (!ast_strlen_zero(snap->cell_id)) mt_web_kv(buf, "Cell ID", snap->cell_id);
    if (!ast_strlen_zero(snap->band)) mt_web_kv(buf, "Band", snap->band);

    ast_str_append(buf, 0, "</table></div>");
    sec->count++;
    return 0;
}

/*! GET / - SIM status cards only (SMS history is a separate, per-SIM page). */
static void mt_web_status_html(int client_fd)
{
    struct mt_status_section_ctx sec = { NULL, 0 };

    RAII_VAR(struct ast_str*, buf, ast_str_create(16384), ast_free);
    sec.buf = &buf;

    mt_web_html_head(&buf, "chan_quectel &mdash; SIM status");
    mt_web_nav(&buf);
    ast_str_append(&buf, 0, "<h1>SIM status</h1>");
    pvt_status_each(&sec, mt_html_status_cb);
    if (sec.count == 0) {
        ast_str_append(&buf, 0, "<p class=\"empty\">No SIMs configured.</p>");
    }
    ast_str_append(&buf, 0, "</body></html>");

    mt_web_send(client_fd, "200 OK", "text/html; charset=utf-8", ast_str_buffer(buf), ast_str_strlen(buf));
}

/*! SIM table row for GET /sms (links to the per-SIM pages). */
static int mt_html_sms_index_cb(void* ctx, const struct pvt_status_snapshot* snap)
{
    struct ast_str** const buf = (struct ast_str**)ctx;

    ast_str_append(buf, 0, "<tr><td><a href=\"/sms/");
    mt_web_url_escape(buf, snap->name);
    ast_str_append(buf, 0, "\">");
    mt_html_escape(buf, snap->name);
    ast_str_append(buf, 0, "</a></td><td>");
    mt_html_escape(buf, snap->state);
    ast_str_append(buf, 0, "</td><td><a href=\"/sms/");
    mt_web_url_escape(buf, snap->name);
    ast_str_append(buf, 0, ".json\">json</a></td></tr>");
    return 0;
}

/*! GET /sms - index page with links to every SIM's SMS history. */
static void mt_web_sms_index_html(int client_fd)
{
    RAII_VAR(struct ast_str*, buf, ast_str_create(8192), ast_free);

    mt_web_html_head(&buf, "chan_quectel &mdash; SMS history");
    mt_web_nav(&buf);
    ast_str_append(&buf, 0, "<h1>SMS history by SIM</h1>");
    ast_str_append(&buf, 0, "<table><tr><th>SIM</th><th>state</th><th>json</th></tr>");
    pvt_status_each(&buf, mt_html_sms_index_cb);
    ast_str_append(&buf, 0, "</table>");
    ast_str_append(&buf, 0, "</body></html>");

    mt_web_send(client_fd, "200 OK", "text/html; charset=utf-8", ast_str_buffer(buf), ast_str_strlen(buf));
}

struct mt_sim_find_ctx {
    const char* name;
    int found;
};

static int mt_sim_find_cb(void* ctx, const struct pvt_status_snapshot* snap)
{
    struct mt_sim_find_ctx* const f = ctx;

    if (!strcmp(snap->name, f->name)) {
        f->found = 1;
        return 1;
    }
    return 0;
}

/*! True when a SIM with exactly this name is configured. */
static int mt_web_sim_exists(const char* name)
{
    struct mt_sim_find_ctx f = { name, 0 };

    pvt_status_each(&f, mt_sim_find_cb);
    return f.found;
}

/*! Parse the /sms/<sim>[.json] tail. Returns 0 and fills sim/is_json on a
 * well-formed single component (no nested '/', printable identifier only). */
static int mt_web_parse_sim_path(const char* tail, char* sim, size_t sim_len, int* is_json)
{
    const char* const slash = strchr(tail, '/');
    const char* const dot   = strrchr(tail, '.');
    const char* p;
    size_t n;

    if (!is_json || sim_len == 0 || ast_strlen_zero(tail) || slash) {
        return -1;
    }

    *is_json = 0;
    if (dot && strcmp(dot, ".json") == 0) {
        *is_json = 1;
        n = (size_t)(dot - tail);
    } else {
        n = strlen(tail);
    }

    if (n == 0 || n >= sim_len) {
        return -1;
    }
    memcpy(sim, tail, n);
    sim[n] = '\0';

    /* device ids are config-defined identifiers; only accept a printable,
     * slash-free name so the router never reaches odd paths */
    for (p = sim; *p; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
            return -1;
        }
    }
    return 0;
}

/*! GET /sms/<sim> - SMS history of a single SIM (paged, HTML). */
static void mt_web_sms_html(int client_fd, const char* sim, int page)
{
    RAII_VAR(struct ast_str*, buf, ast_str_create(16384), ast_free);
    RAII_VAR(struct ast_str*, base, ast_str_create(64), ast_free);

    ast_str_set(&base, 0, "/sms/");
    mt_web_url_escape(&base, sim);

    mt_web_html_head(&buf, "chan_quectel &mdash; SMS per SIM");
    mt_web_nav(&buf);
    ast_str_append(&buf, 0, "<h1>SMS history for SIM ");
    mt_html_escape(&buf, sim);
    ast_str_append(&buf, 0, "</h1>");
    ast_str_append(&buf, 0, "<p class=\"nav\"><a href=\"");
    mt_html_escape(&buf, ast_str_buffer(base));
    ast_str_append(&buf, 0, ".json\">JSON for this SIM</a></p>");
    if (mt_web_sms_section(&buf, sim, ast_str_buffer(base), page)) {
        mt_web_db_error(client_fd, "SMS history read", "text/plain",
                        "internal error: SMS database unavailable", sizeof("internal error: SMS database unavailable") - 1);
        return;
    }
    ast_str_append(&buf, 0, "</body></html>");

    mt_web_send(client_fd, "200 OK", "text/html; charset=utf-8", ast_str_buffer(buf), ast_str_strlen(buf));
}

struct mt_status_json_ctx {
    struct ast_json* arr;
};

/*! Status object builder for GET /status.json (locking-safe snapshot). */
static int mt_json_status_cb(void* ctx, const struct pvt_status_snapshot* snap)
{
    struct ast_json* const j = ast_json_object_create();
    char dbm[32];

    mt_web_rssi_dbm(snap->rssi, dbm, sizeof(dbm));

    ast_json_object_set(j, "name", ast_json_string_create(snap->name));
    ast_json_object_set(j, "state", ast_json_string_create(snap->state));
    ast_json_object_set(j, "connected", ast_json_boolean(snap->connected));
    ast_json_object_set(j, "initialized", ast_json_boolean(snap->initialized));
    ast_json_object_set(j, "gsm_reg_status", ast_json_integer_create(snap->gsm_reg_status));
    ast_json_object_set(j, "gsm_registration", ast_json_string_create(gsm_regstate2str_json(snap->gsm_reg_status)));
    ast_json_object_set(j, "act", ast_json_integer_create(snap->act));
    ast_json_object_set(j, "access_technology", ast_json_string_create(sys_act2str(snap->act)));
    ast_json_object_set(j, "rssi", ast_json_integer_create(snap->rssi));
    ast_json_object_set(j, "rssi_dbm", ast_json_string_create(dbm));
    ast_json_object_set(j, "rssi_bars", ast_json_integer_create(web_util_rssi_bars(snap->rssi)));
    ast_json_object_set(j, "audio_format", ast_json_string_create(snap->audio_format));
    ast_json_object_set(j, "audio_16khz", ast_json_boolean(snap->audio_16khz));
    if (snap->operator > 0) {
        struct ast_json* const plmn = ast_json_object_create();
        ast_json_object_set(plmn, "value", ast_json_integer_create(snap->operator));
        ast_json_object_set(plmn, "mcc", ast_json_integer_create(snap->operator / 100));
        ast_json_object_set(plmn, "mnc", ast_json_stringf("%02d", snap->operator % 100));
        ast_json_object_set(j, "plmn", plmn);
    }
    if (!ast_strlen_zero(snap->imei)) ast_json_object_set(j, "imei", ast_json_string_create(snap->imei));
    if (!ast_strlen_zero(snap->imsi)) ast_json_object_set(j, "imsi", ast_json_string_create(snap->imsi));
    if (!ast_strlen_zero(snap->iccid)) ast_json_object_set(j, "iccid", ast_json_string_create(snap->iccid));
    if (!ast_strlen_zero(snap->location_area_code)) ast_json_object_set(j, "lac", ast_json_string_create(snap->location_area_code));
    if (!ast_strlen_zero(snap->cell_id)) ast_json_object_set(j, "cell_id", ast_json_string_create(snap->cell_id));
    if (!ast_strlen_zero(snap->band)) ast_json_object_set(j, "band", ast_json_string_create(snap->band));

    ast_json_array_append(((struct mt_status_json_ctx*)ctx)->arr, j);
    return 0;
}

/*! GET /status.json - read-only status of all SIMs. */
static void mt_web_status_json(int client_fd)
{
    struct mt_status_json_ctx ctx = { ast_json_array_create() };

    if (!ctx.arr) {
        mt_web_send(client_fd, "500 Internal Server Error", "text/plain", "json error", 10);
        return;
    }
    pvt_status_each(&ctx, mt_json_status_cb);

    RAII_VAR(struct ast_json*, root, ast_json_object_create(), ast_json_unref);
    ast_json_object_set(root, "sims", ctx.arr);

    char* const s = ast_json_dump_string_format(root, AST_JSON_PRETTY);
    mt_web_send(client_fd, "200 OK", "application/json", s ? s : "{}", s ? strlen(s) : 2);
    ast_json_free(s);
}

struct mt_sms_json_ctx {
    struct ast_json* arr;
};

/*! Item builder for GET /sms/<sim>.json (direction explicitly tagged, with
    the direction-specific delivery/retry columns). */
static int mt_json_sms_cb(void* ctx, const struct sms_paging_row* row)
{
    struct ast_json* const j = ast_json_object_create();
    const int is_mt = strcmp(row->direction, "mt") == 0;

    ast_json_object_set(j, "direction", ast_json_string_create(row->direction));
    ast_json_object_set(j, "type", ast_json_string_create(is_mt ? "incoming" : "outgoing"));
    ast_json_object_set(j, "device", ast_json_string_create(row->device));
    if (is_mt) {
        ast_json_object_set(j, "mid", ast_json_string_create(row->ref_id));
        ast_json_object_set(j, "from", ast_json_string_create(row->peer));
    } else {
        ast_json_object_set(j, "mt_out_id", ast_json_integer_create(row->id));
        ast_json_object_set(j, "dst", ast_json_string_create(row->peer));
    }
    ast_json_object_set(j, "msg", ast_json_string_create(row->msg));
    ast_json_object_set(j, "ts", ast_json_string_create(row->ts));
    ast_json_object_set(j, "state", ast_json_string_create(row->state));
    ast_json_object_set(j, "created_at", ast_json_integer_create(row->created_at));
    if (is_mt) {
        ast_json_object_set(j, "attempts", ast_json_integer_create(row->attempts));
        ast_json_object_set(j, "last_result", ast_json_string_create(row->last_result));
        ast_json_object_set(j, "next_retry_at", ast_json_integer_create(row->next_retry_at));
        ast_json_object_set(j, "sent_at", ast_json_integer_create(row->sent_at));
        ast_json_object_set(j, "delivered_at", ast_json_integer_create(row->delivered_at));
    } else {
        ast_json_object_set(j, "srr_status", ast_json_integer_create(row->srr_status));
        ast_json_object_set(j, "mr", ast_json_integer_create(row->mr));
    }
    ast_json_array_append(((struct mt_sms_json_ctx*)ctx)->arr, j);
    return 0;
}

/*! GET /sms/<sim>.json?page=N - validated paginated SMS JSON for one SIM. */
static void mt_web_sms_json(int client_fd, const char* sim, int page)
{
    struct mt_sms_json_ctx ctx = { ast_json_array_create() };
    int total = 0;
    int pages = 1;
    int offset = 0;

    if (!ctx.arr) {
        mt_web_send(client_fd, "500 Internal Server Error", "text/plain", "json error", 10);
        return;
    }
    if (smsdb_sms_count(sim, &total)) {
        ast_json_unref(ctx.arr);
        mt_web_db_error(client_fd, "SMS count read", "application/json",
                        "{\"error\":\"sms database unavailable\"}", sizeof("{\"error\":\"sms database unavailable\"}") - 1);
        return;
    }
    (void)web_util_page_meta(total, MT_WEB_PAGE_SIZE, &page, &pages, &offset);
    if (smsdb_sms_page(sim, MT_WEB_PAGE_SIZE, offset, &ctx, mt_json_sms_cb)) {
        ast_json_unref(ctx.arr);
        mt_web_db_error(client_fd, "SMS history read", "application/json",
                        "{\"error\":\"sms database unavailable\"}", sizeof("{\"error\":\"sms database unavailable\"}") - 1);
        return;
    }

    RAII_VAR(struct ast_json*, root, ast_json_object_create(), ast_json_unref);
    ast_json_object_set(root, "sim", ast_json_string_create(sim));
    ast_json_object_set(root, "page", ast_json_integer_create(page));
    ast_json_object_set(root, "page_size", ast_json_integer_create(MT_WEB_PAGE_SIZE));
    ast_json_object_set(root, "total", ast_json_integer_create(total));
    ast_json_object_set(root, "pages", ast_json_integer_create(pages));
    ast_json_object_set(root, "items", ctx.arr);

    char* const s = ast_json_dump_string_format(root, AST_JSON_PRETTY);
    mt_web_send(client_fd, "200 OK", "application/json", s ? s : "{}", s ? strlen(s) : 2);
    ast_json_free(s);
}

struct mt_json_ctx {
    struct ast_json* arr;
};

static int mt_json_queue_cb(void* ctx, const struct smsdb_mt_row* row)
{
    struct ast_json* const j = ast_json_object_create();
    ast_json_object_set(j, "device", ast_json_string_create(row->device));
    ast_json_object_set(j, "mid", ast_json_string_create(row->mid));
    ast_json_object_set(j, "from", ast_json_string_create(row->from_num));
    ast_json_object_set(j, "msg", ast_json_string_create(row->msg));
    ast_json_object_set(j, "ts", ast_json_string_create(row->ts));
    ast_json_object_set(j, "state", ast_json_string_create(row->state));
    ast_json_object_set(j, "attempts", ast_json_integer_create(row->attempts));
    ast_json_object_set(j, "last_result", ast_json_string_create(row->last_result));
    ast_json_object_set(j, "created_at", ast_json_integer_create(row->created_at));
    ast_json_object_set(j, "sent_at", ast_json_integer_create(row->sent_at));
    ast_json_object_set(j, "delivered_at", ast_json_integer_create(row->delivered_at));
    ast_json_array_append(((struct mt_json_ctx*)ctx)->arr, j);
    return 0;
}

static int mt_json_out_cb(void* ctx, const struct smsdb_mt_out_row* row)
{
    struct ast_json* const j = ast_json_object_create();
    ast_json_object_set(j, "id", ast_json_integer_create(row->id));
    ast_json_object_set(j, "device", ast_json_string_create(row->device));
    ast_json_object_set(j, "dst", ast_json_string_create(row->dst));
    ast_json_object_set(j, "msg", ast_json_string_create(row->msg));
    ast_json_object_set(j, "ts", ast_json_string_create(row->ts));
    ast_json_object_set(j, "state", ast_json_string_create(row->state));
    ast_json_object_set(j, "srr_status", ast_json_integer_create(row->srr_status));
    ast_json_object_set(j, "mr", ast_json_integer_create(row->mr));
    ast_json_object_set(j, "imdn_id", ast_json_string_create(row->imdn_id));
    ast_json_object_set(j, "created_at", ast_json_integer_create(row->created_at));
    ast_json_object_set(j, "updated_at", ast_json_integer_create(row->updated_at));
    ast_json_array_append(((struct mt_json_ctx*)ctx)->arr, j);
    return 0;
}

static void mt_web_json(int client_fd)
{
    struct mt_json_ctx qctx = { ast_json_array_create() };
    struct mt_json_ctx octx = { ast_json_array_create() };

    if (!qctx.arr || !octx.arr) {
        ast_json_unref(qctx.arr);
        ast_json_unref(octx.arr);
        mt_web_send(client_fd, "500 Internal Server Error", "text/plain", "json error", 10);
        return;
    }

    smsdb_mt_list(&qctx, mt_json_queue_cb);
    smsdb_mt_out_list(&octx, mt_json_out_cb);

    RAII_VAR(struct ast_json*, root, ast_json_object_create(), ast_json_unref);
    ast_json_object_set(root, "mt_queue", qctx.arr);
    ast_json_object_set(root, "mt_out", octx.arr);

    char* const s = ast_json_dump_string_format(root, AST_JSON_PRETTY);
    mt_web_send(client_fd, "200 OK", "application/json", s ? s : "{}", s ? strlen(s) : 2);
    ast_json_free(s);
}

/*!
 * \brief Serve a single accepted connection, then close it.
 */
struct incoming_html_ctx {
    struct ast_str **buf; /*!< Указатель на ast_str renderer-а; владеет самим буфером внешний renderer. */
};

/** Использовать tested pure HTML escaper для каждой строки из incoming_msg.
 * Выделенный буфер принадлежит этому вызову; ast_str получает его копию через append.
 */
static void mt_web_incoming_html_escape(struct ast_str **buf, const char *text)
{
    size_t needed = web_incoming_html_escape(text, NULL, 0);
    RAII_VAR(char *, escaped, ast_malloc(needed + 1), ast_free);
    if (!escaped) return;
    web_incoming_html_escape(text, escaped, needed + 1);
    ast_str_append(buf, 0, "%s", escaped);
}

/** Добавить одну скопированную pager row в HTML таблицу диагностики.
 * ctx ссылается на renderer-owned ast_str; row действует до конца callback.
 */
static int incoming_html_row(void *ctx, const struct sms_incoming_page_row *row)
{
    struct ast_str **buf = ((struct incoming_html_ctx *)ctx)->buf;
    ast_str_append(buf, 0, "<tr><td>%lld</td><td>", (long long)row->rowid);
    mt_web_incoming_html_escape(buf, row->device);
    ast_str_append(buf, 0, "</td><td>");
    mt_web_incoming_html_escape(buf, row->storage);
    ast_str_append(buf, 0, "</td><td>%d</td><td>", row->source_index);
    mt_web_incoming_html_escape(buf, row->key);
    ast_str_append(buf, 0, "</td><td>%d</td><td>", row->seqorder);
    mt_web_incoming_html_escape(buf, row->message);
    ast_str_append(buf, 0, "</td><td>%lld</td><td>%d</td><td>%d</td><td>%lld</td><td>%d</td><td>",
                   (long long)row->expiration, row->delete_status, row->delete_attempts,
                   (long long)row->retry_at, row->mt_queued);
    mt_web_incoming_html_escape(buf, row->mt_mid);
    ast_str_append(buf, 0, "</td></tr>");
    return 0;
}

struct incoming_json_ctx {
    struct ast_json *items; /*!< JSON array, принадлежащий контексту до передачи envelope/root. */
    int failed; /*!< Ненулевой флаг: callback прекратил обход из-за allocation/encoding setup error. */
};

/** Безопасно установить JSON value; при неудаче освобождает value, владение object остаётся caller. */
static int incoming_json_set(struct ast_json *object, const char *key, struct ast_json *value)
{
    if (!value) return -1;
    if (ast_json_object_set(object, key, value)) {
        ast_json_unref(value);
        return -1;
    }
    return 0;
}

/** Собрать JSON item из одной durable segment row через Asterisk JSON API.
 * ctx владеет items; callback возвращает nonzero при allocation failure, вызывающий
 * pager останавливается и renderer отбрасывает весь partial array с HTTP 500.
 */
static int incoming_json_row(void *ctx, const struct sms_incoming_page_row *row)
{
    struct incoming_json_ctx *builder = ctx;
    struct ast_json *item = ast_json_object_create();
    if (!item) goto failed;
    if (incoming_json_set(item, "rowid", ast_json_integer_create(row->rowid)) ||
        incoming_json_set(item, "device", ast_json_string_create(row->device)) ||
        incoming_json_set(item, "storage", ast_json_string_create(row->storage)) ||
        incoming_json_set(item, "source_index", ast_json_integer_create(row->source_index)) ||
        incoming_json_set(item, "key", ast_json_string_create(row->key)) ||
        incoming_json_set(item, "seqorder", ast_json_integer_create(row->seqorder)) ||
        incoming_json_set(item, "message", ast_json_string_create(row->message)) ||
        incoming_json_set(item, "expiration", ast_json_integer_create(row->expiration)) ||
        incoming_json_set(item, "delete_status", ast_json_integer_create(row->delete_status)) ||
        incoming_json_set(item, "delete_attempts", ast_json_integer_create(row->delete_attempts)) ||
        incoming_json_set(item, "retry_at", ast_json_integer_create(row->retry_at)) ||
        incoming_json_set(item, "mt_queued", ast_json_boolean(row->mt_queued)) ||
        incoming_json_set(item, "mt_mid", ast_json_string_create(row->mt_mid))) {
        ast_json_unref(item);
        goto failed;
    }
    if (ast_json_array_append(builder->items, item)) {
        ast_json_unref(item);
        goto failed;
    }
    return 0;
failed:
    builder->failed = 1;
    return 1; /* pager stops; caller discards partial JSON and answers HTTP 500 */
}

/** Отправить HTTP 500 с MIME/body для выбранного HTML/JSON режима и exact byte length. */
static void mt_web_incoming_error(int fd, int is_json)
{
    struct web_incoming_error response;
    web_incoming_error_response(is_json, &response);
    mt_web_db_error(fd, "incoming parts", response.content_type,
                    response.body, (int)response.body_len);
}

/** Отправить тестируемый exact 404 text/plain ответ для невалидной/неизвестной SIM. */
static void mt_web_incoming_not_found(int fd)
{
    struct web_incoming_error response;
    web_incoming_not_found_response(&response);
    mt_web_send(fd, response.status, response.content_type,
                response.body, (int)response.body_len);
}

/** Обслужить read-only HTML/JSON страницу retained частей и pagination metadata.
 * device NULL означает все SIM; page уже parsed, затем clamp-ится web_util_page_meta.
 * SQL failure и JSON allocation failure дают 500, empty query остаётся успешным 200.
 * smsdb wrappers кратко захватывают DB mutex; сокет send выполняется после SQL callback.
 */
static void mt_web_incoming_parts(int fd, const char *device, int page, int is_json)
{
    int total = 0, pages = 1, offset = 0;
    if (smsdb_incoming_parts_count(device, &total)) {
        mt_web_incoming_error(fd, is_json);
        return;
    }
    (void)web_util_page_meta(total, MT_WEB_PAGE_SIZE, &page, &pages, &offset);

    if (is_json) {
        struct incoming_json_ctx ctx = { ast_json_array_create(), 0 };
        struct ast_json *root;
        char *serialized;
        if (!ctx.items) {
            mt_web_incoming_error(fd, 1);
            return;
        }
        if (smsdb_incoming_parts_page(device, MT_WEB_PAGE_SIZE, offset, &ctx, incoming_json_row) || ctx.failed) {
            ast_json_unref(ctx.items);
            mt_web_incoming_error(fd, 1);
            return;
        }
        root = ast_json_object_create();
        if (!root) {
            ast_json_unref(ctx.items);
            mt_web_incoming_error(fd, 1);
            return;
        }
        if (incoming_json_set(root, "device", device ? ast_json_string_create(device) : ast_json_null()) ||
            incoming_json_set(root, "page", ast_json_integer_create(page)) ||
            incoming_json_set(root, "page_size", ast_json_integer_create(MT_WEB_PAGE_SIZE)) ||
            incoming_json_set(root, "total", ast_json_integer_create(total)) ||
            incoming_json_set(root, "pages", ast_json_integer_create(pages)) ||
            ast_json_object_set(root, "items", ctx.items)) {
            ast_json_unref(ctx.items);
            ast_json_unref(root);
            mt_web_incoming_error(fd, 1);
            return;
        }
        ctx.items = NULL; /* После успешного object_set массивом владеет root. */
        serialized = ast_json_dump_string_format(root, AST_JSON_PRETTY);
        if (!serialized) {
            ast_json_unref(root);
            mt_web_incoming_error(fd, 1);
            return;
        }
        mt_web_send(fd, "200 OK", "application/json", serialized, strlen(serialized));
        ast_json_free(serialized);
        ast_json_unref(root);
        return;
    }

    RAII_VAR(struct ast_str *, buf, ast_str_create(8192), ast_free);
    RAII_VAR(struct ast_str *, url, ast_str_create(128), ast_free);
    mt_web_html_head(&buf, "Incoming SMS parts");
    mt_web_nav(&buf);
    ast_str_append(&buf, 0, "<h1>Incoming SMS parts</h1><p><a href=\"");
    if (web_incoming_page_url(ast_str_buffer(url), 128, device, page, 1) == 0)
        mt_html_escape(&buf, ast_str_buffer(url));
    ast_str_append(&buf, 0, "\">JSON</a> | total: %d</p>", total);
    if (!total) {
        ast_str_append(&buf, 0, "<p class=\"empty\">Нет накопленных частей.</p>");
    } else {
        struct incoming_html_ctx ctx = { &buf };
        ast_str_append(&buf, 0, "<table><tr><th>rowid</th><th>device</th><th>storage</th><th>index</th><th>group key</th><th>seqorder</th><th>message</th><th>expiration</th><th>delete state</th><th>attempts</th><th>retry</th><th>queued</th><th>MID</th></tr>");
        if (smsdb_incoming_parts_page(device, MT_WEB_PAGE_SIZE, offset, &ctx, incoming_html_row)) {
            mt_web_incoming_error(fd, 0);
            return;
        }
        ast_str_append(&buf, 0, "</table>");
    }
    ast_str_append(&buf, 0, "<p>Page %d / %d", page, pages);
    if (page > 1 && web_incoming_page_url(ast_str_buffer(url), 128, device, page - 1, 0) == 0) {
        ast_str_append(&buf, 0, " <a href=\"");
        mt_html_escape(&buf, ast_str_buffer(url));
        ast_str_append(&buf, 0, "\">Previous</a>");
    }
    if (page < pages && web_incoming_page_url(ast_str_buffer(url), 128, device, page + 1, 0) == 0) {
        ast_str_append(&buf, 0, " <a href=\"");
        mt_html_escape(&buf, ast_str_buffer(url));
        ast_str_append(&buf, 0, "\">Next</a>");
    }
    ast_str_append(&buf, 0, "</p></body></html>");
    mt_web_send(fd, "200 OK", "text/html; charset=utf-8", ast_str_buffer(buf), ast_str_strlen(buf));
}

/** Разобрать один HTTP GET, dispatch legacy handlers без изменения и новый parts route.
 * fd принадлежит этой функции и закрывается в общем epilogue; query/path локальны.
 * Новый classifier вызывается только для incoming route family; SIM parser/existence
 * check предшествуют DB read, malformed/unknown device получает 404.
 */
static void mt_web_serve_client(int client_fd)
{
    char req[2048];
    char method[16] = "";
    char path[512]  = "";
    char* query = NULL;
    ssize_t n;
    int ms  = 3000;
    int exception = 0;
    int incoming_json = 0;
    const char *incoming_tail = NULL;
    enum web_incoming_route_kind incoming_route;

    /* read the request line with a timeout so a stuck client cannot block us */
    if (ast_waitfor_n_fd(&client_fd, 1, &ms, &exception) != client_fd) {
        close(client_fd);
        return;
    }
    n = recv(client_fd, req, sizeof(req) - 1, 0);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    req[n] = '\0';

    char* end = strstr(req, "\r\n");
    if (!end) {
        end = memchr(req, '\n', n);
    }
    if (end) {
        *end = '\0';
    }

    if (sscanf(req, "%15s %511s", method, path) != 2) {
        mt_web_send(client_fd, "400 Bad Request", "text/plain", "bad request", 11);
        close(client_fd);
        return;
    }

    /* split off the query string; keep it for the ?page= parameter */
    char* const q = strchr(path, '?');
    if (q) {
        *q = '\0';
        query = q + 1;
    }

    incoming_route = web_incoming_route(path, &incoming_tail, &incoming_json);
    if (strcasecmp(method, "GET") != 0) {
        mt_web_send(client_fd, "405 Method Not Allowed", "text/plain", "method not allowed", 19);
    } else if (!strcmp(path, "/json")) {
        /* legacy SMS JSON: mt_queue + mt_out, contract unchanged */
        mt_web_json(client_fd);
    } else if (!strcmp(path, "/status.json")) {
        mt_web_status_json(client_fd);
    } else if (!strcmp(path, "/")) {
        mt_web_status_html(client_fd);
    } else if (!strcmp(path, "/sms")) {
        mt_web_sms_index_html(client_fd);
    } else if (incoming_route != WEB_INCOMING_NOT_ROUTE) {
        if (incoming_route == WEB_INCOMING_ALL_HTML || incoming_route == WEB_INCOMING_ALL_JSON) {
            mt_web_incoming_parts(client_fd, NULL, mt_web_page_param(query), incoming_json);
        } else {
            char sim[MT_WEB_SIM_MAX];
            int sim_json = 0;
            if (mt_web_parse_sim_path(incoming_tail, sim, sizeof(sim), &sim_json) != 0 || !mt_web_sim_exists(sim)) {
                mt_web_incoming_not_found(client_fd);
            } else {
                /* Классификатор выбирает формат ответа; SIM parser проверяет и извлекает имя устройства. */
                mt_web_incoming_parts(client_fd, sim, mt_web_page_param(query), incoming_json);
            }
        }
    } else if (ast_begins_with(path, "/sms/")) {
        const int page = mt_web_page_param(query);
        char sim[MT_WEB_SIM_MAX];
        int is_json = 0;
        if (mt_web_parse_sim_path(path + (sizeof("/sms/") - 1), sim, sizeof(sim), &is_json) != 0 || !mt_web_sim_exists(sim)) {
            /* Неизвестный SIM или некорректный путь: ответ 404. */
            mt_web_send(client_fd, "404 Not Found", "text/plain", "not found", 9);
        } else if (is_json) {
            mt_web_sms_json(client_fd, sim, page);
        } else {
            mt_web_sms_html(client_fd, sim, page);
        }
    } else {
        mt_web_send(client_fd, "404 Not Found", "text/plain", "not found", 9);
    }

    close(client_fd);
}

/*!
 * \brief Create and bind the mini HTTP listen socket (non-blocking).
 */
static int mt_web_listen(void)
{
    struct sockaddr_in addr;
    int fd;
    int flags;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((unsigned int)CONF_GLOBAL(mt_web_port));

    if (inet_pton(AF_INET, CONF_GLOBAL(mt_web_bind), &addr.sin_addr) != 1) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        ast_log(LOG_WARNING, "mt: web socket creation failed: %s\n", strerror(errno));
        return -1;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ast_log(LOG_WARNING, "mt: web bind %s:%d failed: %s\n", CONF_GLOBAL(mt_web_bind), CONF_GLOBAL(mt_web_port), strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 8) < 0) {
        ast_log(LOG_WARNING, "mt: web listen failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    /* non-blocking so the accept loop drains all pending clients without blocking */
    flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    return fd;
}

/*!
 * \brief Web thread: accept loop with clean shutdown via mt_stop_fd.
 */
static void* mt_web_threadproc(void* arg)
{
    int fds[2];

    fds[0] = mt_web_listen_fd;
    fds[1] = mt_stop_fd;

    while (1) {
        int ms        = 1000;
        int exception = 0;

        const int outfd = ast_waitfor_n_fd(fds, 2, &ms, &exception);
        if (!mt_task_started || outfd == mt_stop_fd) {
            break;
        }
        if (outfd == mt_web_listen_fd) {
            /* accept all pending connections */
            while (1) {
                struct sockaddr_in caddr;
                socklen_t clen   = sizeof(caddr);
                const int client = accept(mt_web_listen_fd, (struct sockaddr*)&caddr, &clen);
                if (client < 0) {
                    break; /* EAGAIN / EINTR / error: nothing left pending */
                }
                mt_web_serve_client(client);
            }
        }
        /* else: timeout - just loop (stop is delivered via mt_stop_fd) */
    }
    return NULL;
}

// ================================================================

/*!
 * \brief Wake the retry thread so it processes the queue immediately.
 */
void mt_kick(void)
{
    if (!mt_task_started || mt_kick_fd < 0) {
        return;
    }
    eventfd_signal(mt_kick_fd);
}

/*!
 * \brief Test hook: insert a fake incoming SMS into mt_queue (no AT involved).
 */
int mt_test_inject(const char* device, const char* from, const char* msg)
{
    char mid[MT_MID_LEN];

    if (ast_strlen_zero(device)) {
        return -1;
    }

    mt_gen_mid(device, mid, sizeof(mid));
    if (smsdb_mt_put(device, mid, S_OR(from, ""), S_OR(msg, ""), "")) {
        return -1;
    }

    ast_log(LOG_NOTICE, "mt: test-inject -> MT queue (device=%s from=%s mid=%s msg=%s)\n",
        device, S_OR(from, "?"), mid, S_OR(msg, "?"));
    mt_kick();
    return 0;
}

/*!
 * \brief Start the MT retry and web threads (idempotent).
 */
int mt_task_start(void)
{
    if (mt_task_started) {
        return 0;
    }

    mt_kick_fd = eventfd_create();
    if (mt_kick_fd < 0) {
        ast_log(LOG_ERROR, "mt: cannot create kick eventfd\n");
        return -1;
    }

    /* Set the flag before spawning threads so the retry thread cannot see it
     * as 0 and exit immediately after creation. */
    mt_task_started = 1;

    if (ast_pthread_create_background(&mt_retry_thread, NULL, mt_retry_threadproc, NULL) < 0) {
        ast_log(LOG_ERROR, "mt: cannot create retry thread\n");
        mt_task_started = 0;
        eventfd_close(&mt_kick_fd);
        mt_retry_thread = AST_PTHREADT_NULL;
        return -1;
    }

    if (CONF_GLOBAL(mt_web_enabled)) {
        mt_stop_fd       = eventfd_create();
        mt_web_listen_fd = mt_web_listen();

        if (mt_stop_fd < 0 || mt_web_listen_fd < 0) {
            ast_log(LOG_WARNING, "mt: web disabled - cannot listen on %s:%d\n", CONF_GLOBAL(mt_web_bind), CONF_GLOBAL(mt_web_port));
            eventfd_close(&mt_stop_fd);
        } else if (ast_pthread_create_background(&mt_web_thread, NULL, mt_web_threadproc, NULL) < 0) {
            ast_log(LOG_WARNING, "mt: web disabled - cannot create web thread\n");
            close(mt_web_listen_fd);
            mt_web_listen_fd = -1;
            eventfd_close(&mt_stop_fd);
            mt_web_thread = AST_PTHREADT_NULL;
        } else {
            ast_log(LOG_NOTICE, "mt: web on http://%s:%d/\n", CONF_GLOBAL(mt_web_bind), CONF_GLOBAL(mt_web_port));
        }
    }

    mt_task_started = 1;
    return 0;
}

/*!
 * \brief Stop all MT threads and release resources.
 */
void mt_task_stop(void)
{
    if (!mt_task_started) {
        return;
    }
    mt_task_started = 0;

    /* wake the retry thread for a clean exit */
    if (mt_kick_fd >= 0) {
        eventfd_t v = MT_CMD_STOP;
        eventfd_write(mt_kick_fd, v);
    }
    if (mt_stop_fd >= 0) {
        eventfd_signal(mt_stop_fd);
    }

    if (mt_retry_thread != AST_PTHREADT_NULL) {
        pthread_join(mt_retry_thread, NULL);
        mt_retry_thread = AST_PTHREADT_NULL;
    }
    if (mt_web_thread != AST_PTHREADT_NULL) {
        pthread_join(mt_web_thread, NULL);
        mt_web_thread = AST_PTHREADT_NULL;
    }

    /* give in-flight MESSAGE callbacks (pjsip threads) a chance to finish:
     * they only touch the SMS DB which we close right after this. */
    for (int i = 0; i < 100 && __sync_fetch_and_add(&mt_inflight, 0) > 0; ++i) {
        usleep(20 * 1000);
    }

    if (mt_web_listen_fd >= 0) {
        close(mt_web_listen_fd);
        mt_web_listen_fd = -1;
    }
    eventfd_close(&mt_kick_fd);
    eventfd_close(&mt_stop_fd);
}