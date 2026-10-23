#include "web_at_command.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/** Декодировать один компонент формы с проверкой размера и управляющих байтов.
 * \param src Входной срез без NUL-термина, длиной len.
 * \param len Число входных байт.
 * \param dst Буфер результата; принадлежит вызывающему.
 * \param cap Полная ёмкость dst с местом для NUL.
 * \return 0 для непустого безопасного значения, -1 при ошибке; '+' и %HH декодируются.
 */
static int decode(const char *src, size_t len, char *dst, size_t cap)
{
    size_t i, out = 0;
    for (i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == '+') c = ' ';
        else if (c == '%') {
            int hi, lo;
            if (i + 2 >= len) return -1;
            hi = src[i + 1] >= '0' && src[i + 1] <= '9' ? src[i + 1] - '0' :
                 src[i + 1] >= 'a' && src[i + 1] <= 'f' ? src[i + 1] - 'a' + 10 :
                 src[i + 1] >= 'A' && src[i + 1] <= 'F' ? src[i + 1] - 'A' + 10 : -1;
            lo = src[i + 2] >= '0' && src[i + 2] <= '9' ? src[i + 2] - '0' :
                 src[i + 2] >= 'a' && src[i + 2] <= 'f' ? src[i + 2] - 'a' + 10 :
                 src[i + 2] >= 'A' && src[i + 2] <= 'F' ? src[i + 2] - 'A' + 10 : -1;
            if (hi < 0 || lo < 0) return -1;
            c = (unsigned char)((hi << 4) | lo);
            i += 2;
        }
        if (!c || c == '\r' || c == '\n' || c < 0x20 || c == 0x7f || out + 1 >= cap) return -1;
        dst[out++] = (char)c;
    }
    dst[out] = '\0';
    return out ? 0 : -1;
}

/** Сравнить ограниченный срез имени HTTP-заголовка с ожидаемым ASCII именем.
 * \param p Начало среза; \param n его точная длина; \param name NUL-терминированный эталон.
 * \return Ненулевое значение только при полном совпадении без учёта ASCII регистра.
 */
static int header_name_is(const char *p, size_t n, const char *name)
{
    size_t i;
    if (strlen(name) != n) return 0;
    for (i = 0; i < n; ++i) if (tolower((unsigned char)p[i]) != tolower((unsigned char)name[i])) return 0;
    return 1;
}

/** Прочитать и проверить один HTTP request с учётом дробления TCP потока.
 * \param reader Callback чтения; 0/отрицательное значение означает EOF/ошибку.
 * \param ctx Непрозрачный контекст reader, не освобождается этим модулем.
 * \param buffer Буфер caller-owned для заголовков и тела.
 * \param capacity Ёмкость; header и body ограничены опубликованными константами.
 * \param request Результат с точными длинами; владеет только метаданными.
 * \return 0 при корректном framing, иначе -1 для malformed, oversized или неполного запроса.
 */
int web_at_http_read(web_at_read_fn reader, void *ctx, char *buffer, size_t capacity,
                     web_at_http_request *request)
{
    size_t used = 0, marker = 0, header_end = 0, content_length = 0;
    int seen_length = 0, seen_type = 0;
    if (!reader || !buffer || !request || capacity < WEB_AT_HEADER_MAX + 1) return -1;
    memset(request, 0, sizeof(*request));
    while (!header_end) {
        ssize_t got;
        if (used >= WEB_AT_HEADER_MAX || used >= capacity - 1) return -1;
        got = reader(ctx, buffer + used, capacity - 1 - used);
        if (got <= 0 || (size_t)got > capacity - 1 - used) return -1;
        for (ssize_t i = 0; i < got; ++i) {
            const char expected[] = "\r\n\r\n";
            if (buffer[used + (size_t)i] == expected[marker]) {
                if (++marker == sizeof(expected) - 1) { header_end = used + (size_t)i + 1; break; }
            } else marker = buffer[used + (size_t)i] == '\r' ? 1 : 0;
        }
        used += (size_t)got;
    }
    request->header_len = header_end;
    if (header_end > WEB_AT_HEADER_MAX) return -1;
    for (size_t i = 0; i < header_end; ++i) {
        unsigned char c = (unsigned char)buffer[i];
        if ((c < 0x20 && c != '\r' && c != '\n' && c != '\t') || c == 0x7f) return -1;
    }
    /* Header parser iterates bounded CRLF lines; any malformed/duplicate framing header fails closed. */
    {
        size_t line = 0;
        while (line + 2 < header_end) {
            size_t end = line;
            while (end + 1 < header_end && !(buffer[end] == '\r' && buffer[end + 1] == '\n')) ++end;
            if (end + 1 >= header_end) return -1;
            if (line == 0) {
                if (!end || memchr(buffer, '\n', end) || memchr(buffer, '\r', end)) return -1;
            } else if (end == line) break;
            else {
                size_t colon = line;
                while (colon < end && buffer[colon] != ':') ++colon;
                if (colon == end || colon == line) return -1;
                size_t v = colon + 1, ve = end;
                while (v < ve && (buffer[v] == ' ' || buffer[v] == '\t')) ++v;
                while (ve > v && (buffer[ve - 1] == ' ' || buffer[ve - 1] == '\t')) --ve;
                if (header_name_is(buffer + line, colon - line, "Content-Length")) {
                    size_t i;
                    if (seen_length++ || v == ve) return -1;
                    for (i = v; i < ve; ++i) {
                        if (buffer[i] < '0' || buffer[i] > '9') return -1;
                        if (content_length > (WEB_AT_FORM_MAX - (size_t)(buffer[i] - '0')) / 10) return -1;
                        content_length = content_length * 10 + (size_t)(buffer[i] - '0');
                    }
                } else if (header_name_is(buffer + line, colon - line, "Content-Type")) {
                    static const char form_type[] = "application/x-www-form-urlencoded";
                    if (seen_type++ || ve - v != sizeof(form_type) - 1 || strncasecmp(buffer + v, form_type, sizeof(form_type) - 1)) return -1;
                    request->has_content_type = 1;
                } else if (header_name_is(buffer + line, colon - line, "Transfer-Encoding")) return -1;
            }
            line = end + 2;
        }
    }
    if (used > header_end + content_length || header_end + content_length > capacity - 1) return -1;
    while (used < header_end + content_length) {
        ssize_t got = reader(ctx, buffer + used, header_end + content_length - used);
        if (got <= 0 || (size_t)got > header_end + content_length - used) return -1;
        used += (size_t)got;
    }
    buffer[used] = '\0';
    request->body_len = content_length;
    request->request_len = used;
    return 0;
}

/** Разобрать URL-encoded тело: обязательны ровно device и command, без пустых пар.
 * body не меняется, form принадлежит вызывающему; 0 означает соответствие лимитам,
 * -1 — пустое/переполненное тело, неизвестное/дублированное поле или недопустимый байт.
 */
int web_at_form_parse(const char *body, size_t body_len, web_at_form *form)
{
    size_t pos = 0;
    int have_device = 0, have_command = 0;
    if (!body || !form || !body_len || body_len > WEB_AT_FORM_MAX) return -1;
    memset(form, 0, sizeof(*form));
    while (pos < body_len) {
        size_t end = pos, eq, key_len;
        char value[WEB_AT_COMMAND_MAX + 1];
        while (end < body_len && body[end] != '&') ++end;
        eq = pos;
        while (eq < end && body[eq] != '=') ++eq;
        if (eq == end) return -1;
        key_len = eq - pos;
        if (key_len == 6 && !memcmp(body + pos, "device", 6)) {
            if (have_device || decode(body + eq + 1, end - eq - 1, form->device, sizeof(form->device))) return -1;
            have_device = 1;
        } else if (key_len == 7 && !memcmp(body + pos, "command", 7)) {
            if (have_command || decode(body + eq + 1, end - eq - 1, value, sizeof(value))) return -1;
            memcpy(form->command, value, strlen(value) + 1);
            have_command = 1;
        } else return -1;
        if (end == body_len) pos = body_len;
        else {
            if (end + 1 == body_len) return -1;
            pos = end + 1;
        }
    }
    return have_device && have_command ? 0 : -1;
}

/** Проверить форму и binding устройства до единственного обращения к enqueue adapter.
 * \param body Тело формы и точная длина без завершающего NUL.
 * \param form Caller-owned структура для разобранных значений.
 * \param device_check Adapter проверки существования выбранной SIM.
 * \param enqueue Adapter очереди; вызывается не более одного раза.
 * \param ctx Общий непрозрачный контекст обоих adapter-ов.
 * \return 0 queued, -1 input/adapter error, -2 unknown device, -3 enqueue failure.
 * До прохождения form и device validation никаких side effects не возникает.
 */
int web_at_submit(const char *body, size_t body_len, web_at_form *form,
                  web_at_device_check_fn device_check, web_at_enqueue_fn enqueue, void *ctx)
{
    if (!form || !device_check || !enqueue || web_at_form_parse(body, body_len, form)) return -1;
    if (!device_check(ctx, form->device)) return -2;
    return enqueue(ctx, form->device, form->command) == 0 ? 0 : -3;
}

/** Экранировать специальные HTML-символы в caller-owned destination.
 * \param dst Выходной буфер; \param cap полная ёмкость с местом под NUL.
 * \param src NUL-терминированный входной UTF-8 текст.
 * \return 0 при успехе, -1 при переполнении; частичный вывод при ошибке не используется.
 */
static int html_escape(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    for (; *src; ++src) {
        const char *rep = NULL;
        switch (*src) { case '&': rep="&amp;"; break; case '<': rep="&lt;"; break; case '>': rep="&gt;"; break; case '"': rep="&quot;"; break; case '\'': rep="&#39;"; break; }
        size_t amount = rep ? strlen(rep) : 1;
        if (n + amount + 1 > cap) return -1;
        if (rep) { memcpy(dst+n, rep, amount); n += amount; } else dst[n++] = *src;
    }
    dst[n] = '\0';
    return 0;
}

/** Построить production HTTP response для формы либо результата enqueue.
 * \param kind Выбранный сценарий формы/queued/error/bad request.
 * \param device Имя устройства для результата; может быть NULL только для формы/error-body.
 * \param command Команда для result HTML; NULL только для формового/bad request ответа.
 * \param options_html HTML options, ранее экранированные адаптером, только для формы.
 * \param response Caller-owned результат, содержащий HTTP metadata и NUL-terminated body.
 * \return 0 при успешной сборке, -1 при недопустимой комбинации или превышении буфера.
 * Команда/устройство экранируются; queued текст никогда не заявляет выполнение/ответ модема.
 */
int web_at_response_build(web_at_response_kind kind, const char *device, const char *command,
                          const char *options_html, web_at_response *response)
{
    char safe_device[WEB_AT_DEVICE_MAX * 6 + 1], safe_command[WEB_AT_COMMAND_MAX * 6 + 1];
    const char *heading, *notice;
    int written;
    if (!response || kind < WEB_AT_RESPONSE_FORM || kind > WEB_AT_RESPONSE_BAD_REQUEST) return -1;
    memset(response, 0, sizeof(*response));
    if (kind == WEB_AT_RESPONSE_BAD_REQUEST) {
        response->status = "400 Bad Request"; response->content_type = "text/plain";
        memcpy(response->body, "invalid form", 13); response->body_len = 12; return 0;
    }
    response->status = kind == WEB_AT_RESPONSE_ERROR ? "503 Service Unavailable" : "200 OK";
    response->content_type = "text/html; charset=utf-8";
    if (kind == WEB_AT_RESPONSE_FORM) {
        written = snprintf(response->body, sizeof(response->body), "<!doctype html><html><meta charset=\"utf-8\"><h1>Команда AT</h1><form method=\"post\" action=\"/at\"><label>SIM/устройство <select name=\"device\" required>%s</select></label><label>AT-команда <input name=\"command\" required maxlength=512></label><button type=\"submit\">Поставить в очередь</button></form><p>Команда отправляется только после явной отправки формы. Фактический асинхронный ответ появится в логах Asterisk; страница его не отображает.</p></html>", options_html ? options_html : "");
    } else {
        if (!device || !command || html_escape(safe_device, sizeof(safe_device), device) || html_escape(safe_command, sizeof(safe_command), command)) return -1;
        heading = kind == WEB_AT_RESPONSE_QUEUED ? "Команда поставлена в очередь" : "Ошибка постановки команды в очередь";
        notice = "Это подтверждение только enqueue, не выполнения команды. Фактический асинхронный ответ следует искать в логах Asterisk.";
        written = snprintf(response->body, sizeof(response->body), "<!doctype html><html><meta charset=\"utf-8\"><h1>%s</h1><p>SIM/устройство: %s</p><p>Команда: %s</p><p>%s</p><a href=\"/at\">Назад</a></html>", heading, safe_device, safe_command, notice);
    }
    if (written < 0 || (size_t)written >= sizeof(response->body)) return -1;
    response->body_len = (size_t)written;
    return 0;
}
