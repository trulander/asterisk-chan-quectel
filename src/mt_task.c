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
#include "mt_retry.h"
#include "mt_from.h"
#include "smsdb.h"
#include "web_util.h"
#include "web_incoming_parts.h"
#include "web_sim_metrics.h"
#include "web_at_command.h"

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

/** Построить retry transition с настроенными пределами и случайным jitter.
 * Время задаёт вызывающий callback; helper чистой политики проверяется отдельно,
 * чтобы retry boundary не зависел от реальных часов в unit-тесте.
 */
static int mt_retry_make_plan(int status_code, int attempts, int now, mt_retry_plan_t* plan)
{
    return mt_retry_plan_response(status_code, attempts, now,
                                  CONF_GLOBAL(mt_retry_max_attempts),
                                  CONF_GLOBAL(mt_retry_base), CONF_GLOBAL(mt_retry_max_delay),
                                  (int)(ast_random() % 11), plan);
}

/** Сохранить локальную ошибку подготовки/отправки с тем же backoff, что SIP failure.
 * Функция вызывается только MT retry thread; smsdb обновляет именно mt_queue row по MID.
 */
static void mt_send_fail(const struct smsdb_mt_row* row, const char* reason)
{
    mt_retry_plan_t plan;
    const int now = (int)time(NULL);
    if (mt_retry_make_plan(0, row->attempts, now, &plan)) {
        ast_log(LOG_ERROR, "mt: cannot plan retry mid=%s\n", row->mid);
        return;
    }
    if (plan.action == MT_RETRY_ACTION_EXHAUSTED) {
        ast_log(LOG_ERROR, "mt: MESSAGE mid=%s failed, max attempts (%d) reached (%s)\n",
                row->mid, CONF_GLOBAL(mt_retry_max_attempts), S_OR(reason, "?"));
        smsdb_mt_set_result(row->mid, plan.state, plan.attempts, 0, "max attempts reached", 0, 0);
        return;
    }
    ast_log(LOG_NOTICE, "mt: MESSAGE mid=%s send failed (%s) -> retrying, attempt %d/%d, next at %d\n",
            row->mid, S_OR(reason, "?"), plan.attempts, CONF_GLOBAL(mt_retry_max_attempts), plan.next_retry_at);
    smsdb_mt_set_result(row->mid, plan.state, plan.attempts, plan.next_retry_at, reason, 0, 0);
}

/** Завершить асинхронную SIP MESSAGE попытку и сохранить её MT result.
 * token принадлежит ao2 transaction callback; PJSIP 408/transport error планирует
 * retry в той же mt_queue row, provisional/202 оставляют очередь у Flexisip,
 * terminal 2xx отмечает delivered. DB write выполняется вне AT/call state path.
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

    mt_retry_plan_t plan;
    if (mt_retry_make_plan(status_code, tok->attempts, (int)now, &plan)) {
        ast_log(LOG_ERROR, "mt: cannot plan SIP result mid=%s status=%d\n", tok->mid, status_code);
        ao2_ref(tok, -1);
        __sync_sub_and_fetch(&mt_inflight, 1);
        return;
    }

    if (plan.action == MT_RETRY_ACTION_QUEUED) {
        /* 202/1xx означает, что Flexisip принял сообщение и хранит его для endpoint. */
        if (status_code == 202) ast_copy_string(last_result, "202 accepted (queued)", sizeof(last_result));
        else snprintf(last_result, sizeof(last_result), "%d provisional (queued)", status_code);
        queued = 1;
        ast_log(LOG_NOTICE, "mt: MESSAGE mid=%s -> queued (%s)\n", tok->mid, last_result);
    } else if (plan.action == MT_RETRY_ACTION_DELIVERED) {
        snprintf(last_result, sizeof(last_result), "%d delivered", status_code);
        delivered = 1;
        ast_log(LOG_NOTICE, "mt: MESSAGE mid=%s -> delivered (SIP %d)\n", tok->mid, status_code);
    } else {
        /* SIP failure, including PJSIP transaction timeout/408, remains retryable
         * at next_retry_at; a later successful attempt changes the same MID to delivered. */
        if (status_code) snprintf(last_result, sizeof(last_result), "SIP %d", status_code);
        else ast_copy_string(last_result, "transport error", sizeof(last_result));
        if (plan.action == MT_RETRY_ACTION_EXHAUSTED) {
            ast_log(LOG_ERROR, "mt: MESSAGE mid=%s max attempts (%d) reached (%s)\n",
                    tok->mid, CONF_GLOBAL(mt_retry_max_attempts), last_result);
            smsdb_mt_set_result(tok->mid, plan.state, plan.attempts, 0, "max attempts reached", 0, 0);
            ao2_ref(tok, -1);
            __sync_sub_and_fetch(&mt_inflight, 1);
            return;
        }
        ast_log(LOG_NOTICE, "mt: MESSAGE mid=%s -> retrying (attempt %d/%d, %s, next at %d)\n",
                tok->mid, plan.attempts, CONF_GLOBAL(mt_retry_max_attempts), last_result, plan.next_retry_at);
        smsdb_mt_set_result(tok->mid, plan.state, plan.attempts, plan.next_retry_at, last_result, 0, 0);
        ao2_ref(tok, -1);
        __sync_sub_and_fetch(&mt_inflight, 1);
        return;
    }

    if (delivered) {
        smsdb_mt_set_result(tok->mid, plan.state, tok->attempts, 0, last_result, (int)now, (int)now);
    } else if (queued) {
        smsdb_mt_set_result(tok->mid, plan.state, tok->attempts, 0, last_result, (int)now, 0);
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

    /* ast_sip_update_from() принимает полную SIP URI либо user[@domain] и сам парсит
     * её как PJSIP URI; сначала экранируем sender user-part, чтобы '@' из SMS From
     * не превратился в разделитель user/host и не допускал header injection. */
    mt_extract_domain(sip_to, domain, sizeof(domain));
    if (mt_from_uri_build(cur.from_num, domain, from_buf, sizeof(from_buf)) ||
        ast_sip_update_from(tdata, from_buf)) {
        pjsip_tx_data_dec_ref(tdata);
        mt_send_fail(&cur, "invalid MT From identity or domain");
        return;
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

    /* From user-part is escaped by the same production URI builder as outbound SMS. */
    mt_extract_domain(sip_to, domain, sizeof(domain));
    if (!ast_strlen_zero(to_gsm_node_hint)) {
        if (mt_from_uri_build(to_gsm_node_hint, domain, from_buf, sizeof(from_buf)) ||
            ast_sip_update_from(tdata, from_buf)) {
            ast_log(LOG_WARNING, "mt: IMDN receipt: invalid From identity or domain\n");
            pjsip_tx_data_dec_ref(tdata);
            return;
        }
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
#define MT_WEB_CALL_PAGE_SIZE 30 /*!< Строгий лимит строк журнала вызовов на страницу. */

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
    struct ast_str** buf; /*!< HTML buffer, принадлежащий обработчику GET /. */
    int count; /*!< Число отрисованных SIM-карточек. */
    int db_error; /*!< Флаг ошибки метрик: при нём страница не выдаёт ложные нули. */
};

/** Добавить per-SIM счётчики только после успешного чтения всех таблиц.
 * Snapshot не удерживает SQLite mutex; smsdb wrapper захватывает его отдельно и освобождает
 * до возврата. Ошибка отмечается в контексте и приводит к HTTP 500 в верхнем renderer.
 */
static int mt_html_status_cb(void* ctx, const struct pvt_status_snapshot* snap)
{
    struct mt_status_section_ctx* const sec = ctx;
    struct ast_str** const buf = sec->buf;
    char dbm[32];
    const int bars = web_util_rssi_bars(snap->rssi);
    struct sim_metrics metrics;
    char metrics_html[768];

    if (smsdb_sim_metrics(snap->name, &metrics) ||
        web_sim_metrics_render(metrics_html, sizeof(metrics_html), snap->name, &metrics)) {
        sec->db_error = 1;
        return -1;
    }
    mt_web_rssi_dbm(snap->rssi, dbm, sizeof(dbm));

    ast_str_append(buf, 0, "<div class=\"sim\"><h3>");
    mt_html_escape(buf, snap->name);
    ast_str_append(buf, 0, " <span class=\"state\">");
    mt_html_escape(buf, snap->state);
    ast_str_append(buf, 0, "</span> <a class=\"small\" href=\"/sms/");
    mt_web_url_escape(buf, snap->name);
    ast_str_append(buf, 0, "\">SMS history</a> | <a class=\"small\" href=\"/calls/");
    mt_web_url_escape(buf, snap->name);
    ast_str_append(buf, 0, "\">Call history</a></h3><table class=\"kv\">");

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

    ast_str_append(buf, 0, "%s", metrics_html);
    ast_str_append(buf, 0, "</table></div>");
    sec->count++;
    return 0;
}

/** Обслужить главную GET / с карточками и счётчиками каждой SIM.
 * Status snapshot читается отдельно от SQLite; callback собирает метрики под smsdb mutex.
 * При ошибке любой SIM возвращается HTTP 500 вместо неполной страницы с ложными нулями.
 */
static void mt_web_status_html(int client_fd)
{
    struct mt_status_section_ctx sec = { NULL, 0, 0 };

    RAII_VAR(struct ast_str*, buf, ast_str_create(16384), ast_free);
    sec.buf = &buf;

    mt_web_html_head(&buf, "chan_quectel &mdash; SIM status");
    mt_web_nav(&buf);
    ast_str_append(&buf, 0, "<h1>SIM status</h1>");
    pvt_status_each(&sec, mt_html_status_cb);
    if (sec.db_error) {
        mt_web_db_error(client_fd, "home SIM metrics", "text/plain",
                        "internal error: SIM metrics unavailable", sizeof("internal error: SIM metrics unavailable") - 1);
        return;
    }
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

/* Контекст принадлежит одному запросу; HTML либо массив JSON живут до завершения рендера. */
struct call_render_ctx {
    struct ast_str **html; /*!< Буфер HTML; NULL для JSON callback. */
    struct ast_json *items; /*!< Caller-owned массив; callback передаёт ему владение элементом. */
    int failed; /*!< Sticky flag ошибки создания/добавления JSON, чтобы отбросить partial response. */
};

/** Установить созданное значение, учитывая transfer ownership API Asterisk JSON.
 * value при непустом входе передаётся object_set и им поглощается даже при ошибке;
 * NULL до вызова считается ошибкой без передачи владения.
 */
static int mt_call_json_set(struct ast_json *object, const char *key, struct ast_json *value)
{
    if (!value) return -1;
    return ast_json_object_set(object, key, value) == 0 ? 0 : -1;
}

/** Добавить durable call row в JSON массив.
 * Все значения checked до установки; object/array set steal reference даже при ошибке,
 * поэтому cleanup освобождает только ещё локально принадлежащий объект. Callback работает под DB mutex.
 */
static int mt_call_json_row(void *opaque, const struct call_page_row *row)
{
    struct call_render_ctx *ctx = opaque;
    struct ast_json *item = ast_json_object_create();
    char id[CALL_LOG_ID_LEN];
    int failed = 0;
    if (!item) { ctx->failed = 1; return 1; }
    snprintf(id, sizeof(id), "call-%lld", row->id);
    failed |= mt_call_json_set(item, "id", ast_json_string_create(id));
    failed |= mt_call_json_set(item, "device", ast_json_string_create(row->device));
    failed |= mt_call_json_set(item, "number", ast_json_string_create(row->number));
    failed |= mt_call_json_set(item, "started_at", ast_json_string_create(row->started_at));
    failed |= mt_call_json_set(item, "direction", ast_json_string_create(row->direction));
    failed |= mt_call_json_set(item, "answered", ast_json_boolean(row->answered));
    failed |= mt_call_json_set(item, "finished", ast_json_boolean(row->finished));
    failed |= mt_call_json_set(item, "ended_at", ast_strlen_zero(row->ended_at) ? ast_json_null() : ast_json_string_create(row->ended_at));
    if (!failed) {
        /* Pass an extra reference: append consumes it on either result; local item remains ours. */
        if (ast_json_array_append(ctx->items, ast_json_ref(item)) == 0) {
            ast_json_unref(item);
            return 0;
        }
    }
    ast_json_unref(item);
    ctx->failed = 1;
    return 1;
}

/** Render one persisted row to HTML, экранируя каждую ячейку из БД.
 * Duration вычисляется только из точных UTC start/end через production helper;
 * при NULL/ошибке выводится явная пустая отметка. JSON renderer и его schema не меняются.
 * Идентификатор SIM заранее ограничен маршрутом символами [A-Za-z0-9_.-].
 */
static int mt_call_html_row(void *opaque, const struct call_page_row *row)
{
    struct call_render_ctx *ctx = opaque;
    struct ast_str **buf = ctx->html;
    char id[CALL_LOG_ID_LEN];
    snprintf(id, sizeof(id), "call-%lld", row->id);
    ast_str_append(buf, 0, "<tr><td>"); mt_html_escape(buf, id);
    ast_str_append(buf, 0, "</td><td>"); mt_html_escape(buf, row->number);
    ast_str_append(buf, 0, "</td><td>"); mt_html_escape(buf, row->started_at);
    ast_str_append(buf, 0, "</td><td>"); mt_html_escape(buf, row->direction);
    ast_str_append(buf, 0, "</td><td>%s</td><td>%s</td><td>", row->answered ? "yes" : "no", row->finished ? "yes" : "no");
    if (!ast_strlen_zero(row->ended_at)) mt_html_escape(buf, row->ended_at); else ast_str_append(buf, 0, "&mdash;");
    char duration_cell[64];
    if (call_paging_html_duration_fragment(0, row->started_at, row->ended_at,
                                           duration_cell, sizeof(duration_cell))) {
        ast_copy_string(duration_cell, "<td>&mdash;</td>", sizeof(duration_cell));
    }
    ast_str_append(buf, 0, "</td>%s</tr>", duration_cell);
    return 0;
}

/** Serve one per-SIM page. Count/page share SMSDB locking; all DB/JSON allocation errors return 500.
 * Empty histories still use page 1/1; requested page is clamped by web_util_page_meta.
 */
static void mt_web_calls(int fd, const char *sim, int page, int json)
{
    int total = 0, pages = 1, offset = 0;
    if (smsdb_call_count(sim, &total) || web_util_page_meta(total, MT_WEB_CALL_PAGE_SIZE, &page, &pages, &offset)) {
        if (json) mt_web_db_error(fd, "Call history count", "application/json", CALL_WEB_JSON_ERROR, sizeof(CALL_WEB_JSON_ERROR) - 1);
        else mt_web_db_error(fd, "Call history count", "text/plain", "call database unavailable", sizeof("call database unavailable") - 1);
        return;
    }
    if (json) {
        struct call_render_ctx ctx = { NULL, ast_json_array_create(), 0 };
        struct ast_json *root = NULL;
        char *serialized = NULL;
        int failed = 0;
        if (!ctx.items || smsdb_call_page(sim, MT_WEB_CALL_PAGE_SIZE, offset, &ctx, mt_call_json_row) || ctx.failed) failed = 1;
        if (!failed) {
            root = ast_json_object_create();
            if (!root) failed = 1;
        }
        if (!failed) {
            failed |= mt_call_json_set(root, "device", ast_json_string_create(sim));
            failed |= mt_call_json_set(root, "page", ast_json_integer_create(page));
            failed |= mt_call_json_set(root, "page_size", ast_json_integer_create(MT_WEB_CALL_PAGE_SIZE));
            failed |= mt_call_json_set(root, "total", ast_json_integer_create(total));
            failed |= mt_call_json_set(root, "pages", ast_json_integer_create(pages));
            /* Pass a ref because object_set steals it even on error; ctx retains cleanup ownership. */
            failed |= mt_call_json_set(root, "items", ast_json_ref(ctx.items));
        }
        if (!failed) {
            serialized = ast_json_dump_string_format(root, AST_JSON_PRETTY);
            if (!serialized) failed = 1;
        }
        if (failed) mt_web_db_error(fd, "Call history JSON", "application/json", CALL_WEB_JSON_ERROR, sizeof(CALL_WEB_JSON_ERROR) - 1);
        else mt_web_send(fd, "200 OK", "application/json", serialized, strlen(serialized));
        ast_json_free(serialized);
        if (ctx.items) ast_json_unref(ctx.items);
        if (root) ast_json_unref(root);
        return;
    }

    RAII_VAR(struct ast_str *, buf, ast_str_create(8192), ast_free);
    char base[128];
    if (!buf) { mt_web_db_error(fd, "Call history HTML", "text/plain", "internal error", sizeof("internal error") - 1); return; }
    snprintf(base, sizeof(base), "/calls/%s", sim);
    mt_web_html_head(&buf, "Call history"); mt_web_nav(&buf);
    ast_str_append(&buf, 0, "<h1>Call history for SIM "); mt_html_escape(&buf, sim);
    ast_str_append(&buf, 0, "</h1><p><a href=\""); mt_html_escape(&buf, base);
    ast_str_append(&buf, 0, ".json?page=%d\">JSON</a></p><table><tr><th>ID</th><th>Number</th><th>Started (UTC)</th><th>Direction/type</th><th>Answered</th><th>Finished</th><th>Ended at (UTC)</th>", page);
    char duration_header[64];
    if (call_paging_html_duration_fragment(1, NULL, NULL, duration_header, sizeof(duration_header))) {
        ast_copy_string(duration_header, "<th>Duration</th>", sizeof(duration_header));
    }
    ast_str_append(&buf, 0, "%s</tr>", duration_header);
    struct call_render_ctx ctx = { &buf, NULL, 0 };
    if (smsdb_call_page(sim, MT_WEB_CALL_PAGE_SIZE, offset, &ctx, mt_call_html_row)) {
        mt_web_db_error(fd, "Call history read", "text/plain", "call database unavailable", sizeof("call database unavailable") - 1); return;
    }
    ast_str_append(&buf, 0, "</table><p>Page %d / %d", page, pages);
    if (page > 1) ast_str_append(&buf, 0, " <a href=\"%s?page=%d\">Previous</a>", base, page - 1);
    if (page < pages) ast_str_append(&buf, 0, " <a href=\"%s?page=%d\">Next</a>", base, page + 1);
    ast_str_append(&buf, 0, "</p></body></html>");
    mt_web_send(fd, "200 OK", "text/html; charset=utf-8", ast_str_buffer(buf), ast_str_strlen(buf));
}

/** Адаптировать socket recv к production HTTP reader с ограниченным ожиданием.
 * \param ctx Указатель на fd принятого соединения; сокет принадлежит вызывающему.
 * \param buffer Буфер назначения, предоставленный web_at_http_read.
 * \param capacity Максимальное число запрашиваемых байт.
 * \return Положительное число байт при чтении, -1 при timeout/ошибке.
 */
static ssize_t mt_web_at_socket_read(void *ctx, void *buffer, size_t capacity)
{
    int fd = *(int *)ctx;
    int timeout_ms = 3000, exception = 0;
    if (ast_waitfor_n_fd(&fd, 1, &timeout_ms, &exception) != fd) return -1;
    return recv(fd, buffer, capacity, 0);
}

/** Проверить binding SIM по действующему списку устройств.
 * \param ctx Неиспользуемый контекст callback.
 * \param device NUL-терминированное имя из разобранной формы.
 * \return Ненулевое значение при точном совпадении; не меняет состояние и не ставит команду.
 */
static int mt_web_at_device_check(void *ctx, const char *device)
{
    (void)ctx;
    return mt_web_sim_exists(device);
}

/** Добавить в select одну опцию текущего настроенного устройства.
 * \param ctx Указатель на ast_str ** формы; принадлежит вызывающей функции.
 * \param snap Неизменяемый snapshot одного устройства на время callback.
 * \return 0 для продолжения обхода; оба вывода имени проходят HTML escaping.
 */
static int mt_web_at_option(void *ctx, const struct pvt_status_snapshot *snap)
{
    struct ast_str **page = ctx;
    ast_str_append(page, 0, "<option value=\"");
    mt_html_escape(page, snap->name);
    ast_str_append(page, 0, "\">");
    mt_html_escape(page, snap->name);
    ast_str_append(page, 0, "</option>");
    return 0;
}

/** Передать проверенные device/command существующему AT enqueue API.
 * \param ctx Неиспользуемый контекст callback.
 * \param device Настроенное имя устройства, binding которого уже проверен.
 * \param command Строка команды без CR/LF/NUL, подготовленная production parser.
 * \return Результат send_at_command: 0 означает enqueue, отрицательное значение — ошибку.
 * Функция не ожидает ответ модема и не выполняет прямого I/O с устройством.
 */
static int mt_web_at_enqueue(void *ctx, const char *device, const char *command)
{
    (void)ctx;
    return send_at_command(device, command);
}

/** Обслужить один запрос mini-web через существующий listener.
 * \param client_fd Принятый socket; функция закрывает его на каждом выходе. Ограниченный reader
 * накапливает заголовки/тело; GET /at строит только форму, POST допускает enqueue после
 * проверки framing, MIME, полей и выбранного SIM. Прочие legacy dispatch ветви сохранены.
 * HTTP сообщает только enqueue/error: фактический ответ AT асинхронен и не коррелируется
 * с socket-запросом; синтаксические ошибки не доходят до send_at_command().
 */
static void mt_web_serve_client(int client_fd)
{
    char req[4096];
    char method[16] = "";
    char path[512]  = "";
    char* query = NULL;
    web_at_http_request parsed_request;
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
    if (web_at_http_read(mt_web_at_socket_read, &client_fd, req, sizeof(req), &parsed_request)) {
        mt_web_send(client_fd, "400 Bad Request", "text/plain", "malformed request", 17);
        close(client_fd);
        return;
    }
    char* end = strstr(req, "\r\n");
    if (!end) {
        end = memchr(req, '\n', parsed_request.header_len);
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
    /* GET намеренно отображает только форму: никакой AT pipeline отсюда не вызывается. */
    if (!strcmp(path, "/at") && strcasecmp(method, "GET") == 0) {
        RAII_VAR(struct ast_str *, options, ast_str_create(1024), ast_free);
        web_at_response response;
        if (!options || pvt_status_each(&options, mt_web_at_option) ||
            web_at_response_build(WEB_AT_RESPONSE_FORM, NULL, NULL, ast_str_buffer(options), &response))
            mt_web_send(client_fd, "500 Internal Server Error", "text/plain", "internal error", 14);
        else mt_web_send(client_fd, response.status, response.content_type, response.body, (int)response.body_len);
    /* Только явный POST может пройти validation и вызвать один submit adapter. */
    } else if (!strcmp(path, "/at") && strcasecmp(method, "POST") == 0) {
        web_at_form form;
        const char *body = req + parsed_request.header_len;
        int submitted = -1;
        if (parsed_request.has_content_type && parsed_request.body_len > 0)
            submitted = web_at_submit(body, parsed_request.body_len, &form, mt_web_at_device_check, mt_web_at_enqueue, NULL);
        if (submitted == -1) {
            web_at_response response;
            if (web_at_response_build(WEB_AT_RESPONSE_BAD_REQUEST, NULL, NULL, NULL, &response))
                mt_web_send(client_fd, "500 Internal Server Error", "text/plain", "internal error", 14);
            else mt_web_send(client_fd, response.status, response.content_type, response.body, (int)response.body_len);
        } else if (submitted == -2) {
            mt_web_send(client_fd, "400 Bad Request", "text/plain", "invalid SIM/device", 18);
        } else {
            int queued = submitted == 0;
            web_at_response response;
            if (web_at_response_build(queued ? WEB_AT_RESPONSE_QUEUED : WEB_AT_RESPONSE_ERROR,
                                      form.device, form.command, NULL, &response))
                mt_web_send(client_fd, "500 Internal Server Error", "text/plain", "internal error", 14);
            else mt_web_send(client_fd, response.status, response.content_type, response.body, (int)response.body_len);
        }
    } else if (strcasecmp(method, "GET") != 0) {
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
    } else if (ast_begins_with(path, "/calls/")) {
        char sim[MT_WEB_SIM_MAX]; int is_json = 0;
        int page = mt_web_page_param(query);
        if (mt_web_parse_sim_path(path + 7, sim, sizeof(sim), &is_json) != 0 || !mt_web_sim_exists(sim)) {
            mt_web_send(client_fd, "404 Not Found", "text/plain", "not found", 9);
        } else {
            mt_web_calls(client_fd,sim,page,is_json);
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