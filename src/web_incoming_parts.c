/* Чистые вспомогательные функции mini-web; production mt_task.c использует их напрямую. */
#include <stdio.h>
#include <string.h>
#include "web_incoming_parts.h"

/** Выделить только incoming-parts URL family; tail указывает в исходный path. */
enum web_incoming_route_kind web_incoming_route(const char *path, const char **tail, int *is_json)
{
    static const char all_json[] = "/sms/parts.json";
    static const char all_html[] = "/sms/parts";
    static const char sim_prefix[] = "/sms/parts/";
    size_t prefix_len;
    if (!path || !tail || !is_json) return WEB_INCOMING_NOT_ROUTE;
    *tail = NULL;
    *is_json = 0;
    if (!strcmp(path, all_html)) return WEB_INCOMING_ALL_HTML;
    if (!strcmp(path, all_json)) { *is_json = 1; return WEB_INCOMING_ALL_JSON; }
    prefix_len = sizeof(sim_prefix) - 1;
    if (!strncmp(path, sim_prefix, prefix_len) && path[prefix_len]) {
        size_t tail_len;
        *tail = path + prefix_len;
        tail_len = strlen(*tail);
        /* Режим задаёт только конечное .json; внутренние точки остаются частью SIM имени. */
        if (tail_len >= 5 && !strcmp(*tail + tail_len - 5, ".json")) *is_json = 1;
        return WEB_INCOMING_SIM;
    }
    return WEB_INCOMING_NOT_ROUTE;
}

/** Описать общий 404; статическая body живёт всё время процесса. */
void web_incoming_not_found_response(struct web_incoming_error *out)
{
    static const char body[] = "not found";
    if (!out) return;
    out->status = "404 Not Found";
    out->content_type = "text/plain";
    out->body = body;
    out->body_len = sizeof(body) - 1;
}

/** Описать 500 с точной длиной literal, независимо от способа отправки. */
void web_incoming_error_response(int is_json, struct web_incoming_error *out)
{
    static const char json_body[] = "{\"error\":\"sms database unavailable\"}";
    static const char html_body[] = "internal error: SMS database unavailable";
    if (!out) return;
    out->status = "500 Internal Server Error";
    out->content_type = is_json ? "application/json" : "text/plain";
    out->body = is_json ? json_body : html_body;
    out->body_len = is_json ? sizeof(json_body) - 1 : sizeof(html_body) - 1;
}

/** Экранировать пять HTML special chars; остальные UTF-8/control bytes копируются как есть. */
size_t web_incoming_html_escape(const char *input, char *output, size_t output_size)
{
    const unsigned char *p = (const unsigned char *)(input ? input : "");
    size_t used = 0;
    while (*p) {
        const char *replacement = NULL;
        switch (*p) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '\"': replacement = "&quot;"; break;
        case '\'': replacement = "&#39;"; break;
        default: break;
        }
        if (replacement) {
            size_t n = strlen(replacement);
            if (output && output_size && used + n < output_size) memcpy(output + used, replacement, n);
            used += n;
        } else {
            if (output && output_size && used + 1 < output_size) output[used] = (char)*p;
            used++;
        }
        p++;
    }
    if (output && output_size) output[used < output_size ? used : output_size - 1] = '\0';
    return used;
}

/** Построить относительную ссылку; snprintf сообщает переполнение без усечения URL. */
int web_incoming_page_url(char *out, size_t size, const char *device, int page, int json)
{
    int n;
    if (!out || !size || page < 1) return -1;
    if (device && *device) n = snprintf(out, size, "/sms/parts/%s%s?page=%d", device, json ? ".json" : "", page);
    else n = snprintf(out, size, "/sms/parts%s?page=%d", json ? ".json" : "", page);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}
