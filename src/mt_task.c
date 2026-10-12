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
        smsdb_mt_set_result(row->mid, "retrying", attempts, 0, "max attempts reached", 0, 0);
        return;
    }

    const int next_retry_at = (int)now + mt_backoff_seconds(attempts);
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
    } else if (status_code >= 200 && status_code < 300) {
        /* phone online, Flexisip forwarded and got a 2xx from the phone */
        snprintf(last_result, sizeof(last_result), "%d delivered", status_code);
        delivered = 1;
    } else if (status_code >= 100 && status_code < 200) {
        /* 110 Push / provisional: phone offline, Flexisip keeps the message */
        snprintf(last_result, sizeof(last_result), "%d provisional (queued)", status_code);
        queued = 1;
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
            smsdb_mt_set_result(tok->mid, "retrying", attempts, 0, "max attempts reached", 0, 0);
        } else {
            const int next_retry_at = (int)now + mt_backoff_seconds(attempts);
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
 * \brief Build and send one MESSAGE for a mt_queue row.
 */
static void mt_send_row(const struct smsdb_mt_row* row)
{
    struct smsdb_mt_row cur = *row;
    char sip_to[PATHLEN];
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
    endpoint = ast_sip_get_endpoint(endpoint_name, 1, &uri);
    if (!endpoint) {
        mt_send_fail(&cur, "SIP endpoint not found");
        return;
    }

    if (ast_sip_create_request("MESSAGE", NULL, endpoint, sip_to, NULL, &tdata)) {
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