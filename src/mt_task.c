/*
 * MT delivery task.
 *
 * Delivers incoming SMS (received on SIM and stored in mt_queue) to a SIP
 * endpoint (Flexisip) via res_pjsip MESSAGE, tracks the delivery state in the
 * SMS DB, retries on failure with exponential backoff and serves a small HTTP
 * status page (HTML + JSON) with the delivery queues.
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

#include "at_read.h" /* at_wait() */
#include "chan_quectel.h"
#include "eventfd.h"
#include "helpers.h" /* fd_write_all() */
#include "mt_task.h"
#include "smsdb.h"

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
 * \brief Build a message/cpim (RFC 3862) body carrying the IMDN request
 * headers (RFC 5438) so the Linphone client auto-sends a delivery receipt
 * back to us. Every line is CRLF terminated; the CPIM Content-length is the
 * byte length of the SMS text (not of the whole message).
 *
 * \param from_num sender number (may include '+'; used inside <sip:...>)
 * \param phone_user SIP user of the recipient (from sms_sip_to)
 * \param domain host part of sms_sip_to
 * \param mid our mt_queue message id (IMDN correlation secret, no @domain)
 * \param msg plain SMS text
 * \param buf output buffer (NUL terminated on return)
 * \param len size of \a buf
 * \retval 0 success, -1 when the body had to be truncated (logged)
 */
static int mt_build_cpim(const char* from_num, const char* phone_user, const char* domain, const char* mid, const char* msg, char* buf, size_t len)
{
    char dt[64];
    size_t pos = 0;
    int truncated = 0;

#define MT_CPIM_APPEND(fmt, ...)                                                           \
    do {                                                                                   \
        const int wr = snprintf(buf + pos, len > pos ? len - pos : 0, fmt, ##__VA_ARGS__); \
        if (wr < 0 || (size_t)wr >= (len > pos ? len - pos : 0)) {                         \
            truncated = 1;                                                                  \
            break;                                                                          \
        }                                                                                   \
        pos += (size_t)wr;                                                                  \
    } while (0)

    mt_rfc3339_now(dt, sizeof(dt));

    MT_CPIM_APPEND("From: <sip:%s@%s>\r\n", S_OR(from_num, ""), S_OR(domain, ""));
    if (!truncated) {
        MT_CPIM_APPEND("To: <sip:%s@%s>\r\n", S_OR(phone_user, ""), S_OR(domain, ""));
    }
    if (!truncated) {
        MT_CPIM_APPEND("DateTime: %s\r\n", S_OR(dt, ""));
    }
    if (!truncated) {
        MT_CPIM_APPEND("NS: urn:ietf:params:imdn\r\n");
    }
    if (!truncated) {
        MT_CPIM_APPEND("imdn.Message-ID: %s\r\n", S_OR(mid, ""));
    }
    if (!truncated) {
        MT_CPIM_APPEND("imdn.Disposition-Notification: positive-delivery, negative-delivery\r\n");
    }
    if (!truncated) {
        MT_CPIM_APPEND("Content-type: text/plain; charset=UTF-8\r\n");
    }
    if (!truncated) {
        MT_CPIM_APPEND("Content-length: %d\r\n", (int)strlen(S_OR(msg, "")));
    }
    if (!truncated) {
        MT_CPIM_APPEND("\r\n");
    }
    if (!truncated) {
        /* body: keep as much as fits, NUL-terminated */
        const size_t avail = len - pos;
        const char* const text = S_OR(msg, "");
        const size_t mlen = strlen(text);
        size_t copy = mlen;
        if (copy + 1 > avail) {
            copy = avail - 1;
            truncated = 1;
        }
        if (mlen) {
            memcpy(buf + pos, text, copy);
        }
        pos += copy;
        buf[pos] = '\0';
    }
#undef MT_CPIM_APPEND

    if (truncated) {
        ast_log(LOG_WARNING, "mt: CPIM body truncated to %zu bytes (mid=%s)\n", pos, S_OR(mid, ""));
    }
    return truncated ? -1 : 0;
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

    /* Wrap the SMS in message/cpim (RFC 3862) with imdn.Message-ID and
     * imdn.Disposition-Notification (RFC 5438): the Linphone client then
     * auto-sends a delivered IMDN receipt back, which we correlate by mid. */
    char cpim[16384];
    char phone_user[DEVNAMELEN];
    const char* const puser = mt_sip_to_user(sip_to, phone_user, sizeof(phone_user));
    if (ast_strlen_zero(puser)) {
        pjsip_tx_data_dec_ref(tdata);
        mt_send_fail(&cur, "sms_sip_to has no user part for CPIM To");
        return;
    }
    mt_build_cpim(cur.from_num, puser, domain, cur.mid, cur.msg, cpim, sizeof(cpim));

    struct ast_sip_body body = {
        .type      = "message",
        .subtype   = "cpim",
        .body_text = cpim,
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
// Mini web: GET / -> HTML tables, GET /json -> JSON
// ================================================================

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

struct mt_html_ctx {
    struct ast_str* buf;
};

static int mt_html_queue_cb(void* ctx, const struct smsdb_mt_row* row)
{
    struct ast_str** const buf = &(((struct mt_html_ctx*)ctx)->buf);

    ast_str_append(buf, 0, "<tr><td>");
    mt_web_ts(buf, row->created_at);
    ast_str_append(buf, 0, "</td><td>");
    mt_html_escape(buf, row->device);
    ast_str_append(buf, 0, "</td><td>");
    mt_html_escape(buf, row->from_num);
    ast_str_append(buf, 0, "</td><td>");
    mt_html_escape(buf, row->msg);
    ast_str_append(buf, 0, "</td><td>");
    mt_html_escape(buf, row->state);
    ast_str_append(buf, 0, "</td><td>%d</td><td>", row->attempts);
    mt_html_escape(buf, row->last_result);
    ast_str_append(buf, 0, "</td></tr>");
    return 0;
}

static int mt_html_out_cb(void* ctx, const struct smsdb_mt_out_row* row)
{
    struct ast_str** const buf = &(((struct mt_html_ctx*)ctx)->buf);

    ast_str_append(buf, 0, "<tr><td>");
    mt_web_ts(buf, row->created_at);
    ast_str_append(buf, 0, "</td><td>");
    mt_html_escape(buf, row->device);
    ast_str_append(buf, 0, "</td><td>");
    mt_html_escape(buf, row->dst);
    ast_str_append(buf, 0, "</td><td>");
    mt_html_escape(buf, row->msg);
    ast_str_append(buf, 0, "</td><td>");
    mt_html_escape(buf, row->state);
    ast_str_append(buf, 0, "</td><td>%d</td><td>%s</td></tr>", row->mr, row->srr_status > 0 ? "srr" : "");
    return 0;
}

static void mt_web_html(int client_fd)
{
    struct mt_html_ctx ctx = { NULL };

    RAII_VAR(struct ast_str*, buf, ast_str_create(8192), ast_free);
    ctx.buf = buf;

    ast_str_append(&buf, 0,
                   "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>chan_quectel MT</title>"
                   "<style>body{font-family:sans-serif;margin:1em}table{border-collapse:collapse;margin-bottom:2em}"
                   "th,td{border:1px solid #999;padding:4px 8px;text-align:left;vertical-align:top}th{background:#eee}"
                   "</style></head><body>");
    ast_str_append(&buf, 0, "<p><a href=\"/json\">JSON</a></p>");

    ast_str_append(&buf, 0, "<h2>Incoming (SIM&rarr;SIP) &mdash; mt_queue</h2>");
    ast_str_append(&buf, 0,
                   "<table><tr><th>time</th><th>device</th><th>from</th><th>msg</th><th>state</th><th>attempts</th><th>result</th></tr>");
    smsdb_mt_list(&ctx, mt_html_queue_cb);
    ast_str_append(&buf, 0, "</table>");

    ast_str_append(&buf, 0, "<h2>Outgoing (Android&rarr;SIM) &mdash; mt_out</h2>");
    ast_str_append(&buf, 0,
                   "<table><tr><th>time</th><th>device</th><th>dst</th><th>msg</th><th>state</th><th>mr</th><th>srr</th></tr>");
    smsdb_mt_out_list(&ctx, mt_html_out_cb);
    ast_str_append(&buf, 0, "</table>");

    ast_str_append(&buf, 0, "</body></html>");

    mt_web_send(client_fd, "200 OK", "text/html; charset=utf-8", ast_str_buffer(buf), ast_str_strlen(buf));
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
static void mt_web_serve_client(int client_fd)
{
    char req[2048];
    char method[16] = "";
    char path[512]  = "";
    ssize_t n;
    int ms  = 3000;
    int exception = 0;

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

    /* strip any query string */
    char* const q = strchr(path, '?');
    if (q) {
        *q = '\0';
    }

    if (strcasecmp(method, "GET") != 0) {
        mt_web_send(client_fd, "405 Method Not Allowed", "text/plain", "method not allowed", 19);
    } else if (!strcmp(path, "/json")) {
        mt_web_json(client_fd);
    } else if (!strcmp(path, "/") || ast_strlen_zero(path)) {
        mt_web_html(client_fd);
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