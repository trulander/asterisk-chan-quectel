#include "web_at_command.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/** Проверяет базовый разбор с URL-декодированием и перестановкой полей.
 * Тело содержит корректный пробел-код и обратный порядок ключей; ожидается
 * точная команда AT+COPS? и выбранное устройство sim1 без побочных вызовов.
 */
static void test_valid_form(void)
{
    web_at_form f;
    const char body[] = "command=AT%2BCOPS%3F&device=sim1";
    assert(web_at_form_parse(body, sizeof(body) - 1, &f) == 0);
    assert(!strcmp(f.device, "sim1"));
    assert(!strcmp(f.command, "AT+COPS?"));
}

/** Счётчики тестовых адаптеров и заданный результат постановки для проверки границы побочного эффекта. */
static int checks, enqueues, enqueue_result;
/** Тестовый адаптер разрешает только sim1 и считает проверки привязки устройства. */
static int device_check(void *ctx, const char *device)
{
    (void)ctx;
    ++checks;
    return !strcmp(device, "sim1");
}

/** Подменяет только конечный адаптер очереди и сверяет выбранную SIM с командой. */
static int enqueue(void *ctx, const char *device, const char *command)
{
    (void)ctx;
    ++enqueues;
    assert(!strcmp(device, "sim1"));
    assert(!strcmp(command, "AT"));
    return enqueue_result;
}

typedef struct fake_reader {
    const char *data; /**< Неперезаписываемые байты запроса, принадлежат тесту. */
    size_t length; /**< Полная длина данных без завершающего NUL. */
    size_t offset; /**< Смещение следующего непрочитанного байта; начинается с нуля. */
    size_t fragment; /**< Максимум байт за одно чтение; ноль в тестах не используется. */
} fake_reader;
/** Возвращает не более fragment байт запроса, моделируя произвольную сегментацию TCP. */
static ssize_t read_fragment(void *ctx, void *buffer, size_t capacity)
{
    fake_reader *r = ctx;
    size_t n = r->length - r->offset;
    if (!n) return 0;
    if (n > capacity) n = capacity;
    if (n > r->fragment) n = r->fragment;
    memcpy(buffer, r->data + r->offset, n);
    r->offset += n;
    return (ssize_t)n;
}
/** Проверяет накопление фрагментов запроса и точность проверки границ HTTP-заголовков/тела. */
static void test_http_reader(void)
{
    char buffer[WEB_AT_REQUEST_MAX + 1];
    web_at_http_request parsed;
    const char good[] = "POST /at HTTP/1.1\r\nContent-Length: 22\r\nContent-Type: application/x-www-form-urlencoded\r\n\r\ndevice=sim1&command=AT";
    const char no_length[] = "POST /at HTTP/1.1\r\nContent-Type: application/x-www-form-urlencoded\r\n\r\ndevice=sim1&command=AT";
    const char valid_get[] = "GET /at HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const char get_with_zero_length[] = "GET /at HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
    const char garbage_length[] = "POST /at HTTP/1.1\r\nContent-Length: 22x\r\nContent-Type: application/x-www-form-urlencoded\r\n\r\ndevice=sim1&command=AT";
    const char signed_length[] = "POST /at HTTP/1.1\r\nContent-Length: +22\r\nContent-Type: application/x-www-form-urlencoded\r\n\r\ndevice=sim1&command=AT";
    const char oversized_length[] = "POST /at HTTP/1.1\r\nContent-Length: 999999999999999999999999999999999999999999\r\nContent-Type: application/x-www-form-urlencoded\r\n\r\n";
    const char duplicate_length[] = "POST /at HTTP/1.1\r\nContent-Length: 22\r\ncontent-length: 22\r\nContent-Type: application/x-www-form-urlencoded\r\n\r\ndevice=sim1&command=AT";
    const char duplicate_type[] = "POST /at HTTP/1.1\r\nContent-Length: 22\r\nContent-Type: application/x-www-form-urlencoded\r\ncontent-type: application/x-www-form-urlencoded\r\n\r\ndevice=sim1&command=AT";
    const char unsupported_type[] = "POST /at HTTP/1.1\r\nContent-Length: 22\r\nContent-Type: text/plain\r\n\r\ndevice=sim1&command=AT";
    const char malformed_type[] = "POST /at HTTP/1.1\r\nContent-Length: 22\r\nContent-Type: application/x-www-form-urlencoded; charset=utf-8\r\n\r\ndevice=sim1&command=AT";
    const char transfer_encoding[] = "POST /at HTTP/1.1\r\nContent-Length: 22\r\nContent-Type: application/x-www-form-urlencoded\r\nTransfer-Encoding: chunked\r\n\r\ndevice=sim1&command=AT";
    const char body_header_spoof[] = "POST /at HTTP/1.1\r\nContent-Length: 0\r\nContent-Type: application/x-www-form-urlencoded\r\n\r\nContent-Length: 22";
    const char incomplete[] = "POST /at HTTP/1.1\r\nContent-Length: 22\r\nContent-Type: application/x-www-form-urlencoded\r\n\r\ndevice=sim1";
    const char eof_in_header[] = "POST /at HTTP/1.1\r\nContent-Length: 22\r\n";
    const char pipeline[] = "GET /at HTTP/1.1\r\n\r\nGET /json HTTP/1.1\r\n\r\n";
    const char trailing_body[] = "POST /at HTTP/1.1\r\nContent-Length: 22\r\nContent-Type: application/x-www-form-urlencoded\r\n\r\ndevice=sim1&command=ATX";
    fake_reader r = { good, sizeof(good)-1, 0, 3 };
    assert(web_at_http_read(read_fragment, &r, buffer, sizeof(buffer), &parsed) == 0);
    assert(parsed.has_content_type && parsed.body_len == 22 && parsed.request_len == sizeof(good)-1);
    assert(!memcmp(buffer + parsed.header_len, "device=sim1&command=AT", 22));
    /* GET без тела и Content-Length допустим; отсутствие тела не требует POST MIME. */
    r.data = valid_get; r.length = sizeof(valid_get)-1; r.offset = 0; r.fragment = 2;
    assert(web_at_http_read(read_fragment, &r, buffer, sizeof(buffer), &parsed) == 0);
    assert(parsed.body_len == 0 && parsed.request_len == sizeof(valid_get)-1 && !parsed.has_content_type);
    r.data = get_with_zero_length; r.length = sizeof(get_with_zero_length)-1; r.offset = 0; r.fragment = sizeof(get_with_zero_length);
    assert(web_at_http_read(read_fragment, &r, buffer, sizeof(buffer), &parsed) == 0);
    assert(parsed.body_len == 0 && parsed.request_len == sizeof(get_with_zero_length)-1);
/** Передаёт точную длину литерала reader-у и требует отклонения некорректного запроса. */
#define REJECT_HTTP(literal) do { fake_reader bad = { literal, sizeof(literal)-1, 0, sizeof(literal) }; assert(web_at_http_read(read_fragment, &bad, buffer, sizeof(buffer), &parsed) != 0); } while (0)
    REJECT_HTTP(no_length);
    REJECT_HTTP(garbage_length);
    REJECT_HTTP(signed_length);
    REJECT_HTTP(oversized_length);
    REJECT_HTTP(duplicate_length);
    REJECT_HTTP(duplicate_type);
    REJECT_HTTP(unsupported_type);
    REJECT_HTTP(malformed_type);
    REJECT_HTTP(transfer_encoding);
    REJECT_HTTP(body_header_spoof);
    REJECT_HTTP(pipeline);
    REJECT_HTTP(trailing_body);
#undef REJECT_HTTP
    r.data = incomplete; r.length = sizeof(incomplete)-1; r.offset = 0; r.fragment = 5;
    assert(web_at_http_read(read_fragment, &r, buffer, sizeof(buffer), &parsed) != 0);
    r.data = eof_in_header; r.length = sizeof(eof_in_header)-1; r.offset = 0; r.fragment = 4;
    assert(web_at_http_read(read_fragment, &r, buffer, sizeof(buffer), &parsed) != 0);
    /* Однобайтовые чтения делят разделитель CRLFCRLF и начало body по каждому рубежу. */
    r.data = good; r.length = sizeof(good)-1; r.offset = 0; r.fragment = 1;
    assert(web_at_http_read(read_fragment, &r, buffer, sizeof(buffer), &parsed) == 0);
}

/** Проверяет, что reader останавливается на завершённом frame, не дожидаясь поздней pipeline-части.
 * Callback сначала отдаёт ровно полный GET /at, хотя позже располагает GET /json;
 * успех и offset первого request доказывают, что второй запрос не читался и не отвергался.
 */
static void test_later_pipeline_bytes_remain_unread(void)
{
    char buffer[WEB_AT_REQUEST_MAX + 1];
    web_at_http_request parsed;
    const char first[] = "GET /at HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const char later[] = "GET /json HTTP/1.1\r\nHost: localhost\r\n\r\n";
    char stream[sizeof(first) + sizeof(later)];
    size_t first_len = sizeof(first) - 1;
    memcpy(stream, first, first_len);
    memcpy(stream + first_len, later, sizeof(later));
    fake_reader r = { stream, first_len + sizeof(later) - 1, 0, first_len };

    assert(web_at_http_read(read_fragment, &r, buffer, sizeof(buffer), &parsed) == 0);
    assert(parsed.request_len == first_len && parsed.header_len == first_len && parsed.body_len == 0);
    assert(r.offset == first_len);
}

/** Проверяет точные максимумы header и body и следующий недопустимый размер.
 * Неизвестное безопасное X-заголовочное поле заполняет header до предела;
 * body ограничен 640 байтами reader-а, а 641 байт отклоняется до выдачи запроса.
 */
static void test_http_exact_size_boundaries(void)
{
    char buffer[WEB_AT_REQUEST_MAX + 1];
    char request[WEB_AT_REQUEST_MAX + 2];
    web_at_http_request parsed;
    const char prefix[] = "GET /at HTTP/1.1\r\nX-Pad: ";
    const char suffix[] = "\r\n\r\n";
    size_t suffix_len = sizeof(suffix) - 1;
    size_t pad_len = WEB_AT_HEADER_MAX - (sizeof(prefix) - 1) - suffix_len;
    memcpy(request, prefix, sizeof(prefix) - 1);
    memset(request + sizeof(prefix) - 1, 'x', pad_len);
    memcpy(request + sizeof(prefix) - 1 + pad_len, suffix, suffix_len);
    request[WEB_AT_HEADER_MAX] = '\0';
    fake_reader max_header = { request, WEB_AT_HEADER_MAX, 0, 17 };
    assert(web_at_http_read(read_fragment, &max_header, buffer, sizeof(buffer), &parsed) == 0);
    assert(parsed.header_len == WEB_AT_HEADER_MAX && parsed.body_len == 0);
    memcpy(request, prefix, sizeof(prefix) - 1);
    memset(request + sizeof(prefix) - 1, 'x', pad_len + 1);
    memcpy(request + sizeof(prefix) - 1 + pad_len + 1, suffix, suffix_len);
    fake_reader over_header = { request, WEB_AT_HEADER_MAX + 1, 0, 128 };
    assert(web_at_http_read(read_fragment, &over_header, buffer, sizeof(buffer), &parsed) != 0);

    {
        const char body_prefix[] = "POST /at HTTP/1.1\r\nContent-Length: 640\r\nContent-Type: application/x-www-form-urlencoded\r\n\r\n";
        size_t head_len = sizeof(body_prefix) - 1;
        memcpy(request, body_prefix, head_len);
        memset(request + head_len, 'x', WEB_AT_FORM_MAX);
        fake_reader max_body = { request, head_len + WEB_AT_FORM_MAX, 0, 113 };
        assert(web_at_http_read(read_fragment, &max_body, buffer, sizeof(buffer), &parsed) == 0);
        assert(parsed.body_len == WEB_AT_FORM_MAX && parsed.request_len == head_len + WEB_AT_FORM_MAX);
        request[head_len + WEB_AT_FORM_MAX] = 'x';
        fake_reader over_body = { request, head_len + WEB_AT_FORM_MAX + 1, 0, sizeof(request) };
        assert(web_at_http_read(read_fragment, &over_body, buffer, sizeof(buffer), &parsed) != 0);
    }
}

/** Проверяет HTTP status/MIME/body, HTML-экранирование и честное описание результата постановки в очередь. */
static void test_response_contract(void)
{
    web_at_response response;
    assert(web_at_response_build(WEB_AT_RESPONSE_FORM, NULL, NULL, "<option value=\"sim1\">sim1</option>", &response) == 0);
    assert(!strcmp(response.status, "200 OK") && !strcmp(response.content_type, "text/html; charset=utf-8"));
    assert(strlen(response.body) == response.body_len);
    assert(strstr(response.body, "method=\"post\"") && strstr(response.body, "submit") && strstr(response.body, "логах Asterisk"));
    assert(web_at_response_build(WEB_AT_RESPONSE_QUEUED, "<sim&\"", "AT<>&\"", NULL, &response) == 0);
    assert(!strcmp(response.status, "200 OK") && !strcmp(response.content_type, "text/html; charset=utf-8"));
    assert(strlen(response.body) == response.body_len);
    assert(strstr(response.body, "&lt;sim&amp;&quot;") && strstr(response.body, "AT&lt;&gt;&amp;&quot;") && strstr(response.body, "только enqueue") && strstr(response.body, "логах Asterisk"));
    assert(!strstr(response.body, "выполнена") && !strstr(response.body, "ответ модема получен"));
    assert(web_at_response_build(WEB_AT_RESPONSE_ERROR, "sim1", "AT", NULL, &response) == 0);
    assert(!strcmp(response.status, "503 Service Unavailable") && !strcmp(response.content_type, "text/html; charset=utf-8"));
    assert(strlen(response.body) == response.body_len);
    assert(strstr(response.body, "Ошибка постановки команды в очередь"));
    assert(web_at_response_build(WEB_AT_RESPONSE_BAD_REQUEST, NULL, NULL, NULL, &response) == 0);
    assert(!strcmp(response.status, "400 Bad Request") && !strcmp(response.content_type, "text/plain") && response.body_len == 12);
    {
        char too_large[WEB_AT_RESPONSE_MAX + 1];
        memset(too_large, 'x', sizeof(too_large) - 1);
        too_large[sizeof(too_large) - 1] = '\0';
        assert(web_at_response_build(WEB_AT_RESPONSE_FORM, NULL, NULL, too_large, &response) != 0);
    }
}

/** Проверяет контракт submit: ровно один enqueue только после валидного binding.
 * Повторной постановки внутри одного вызова быть не должно; неизвестная SIM,
 * некорректная форма и ошибка настоящего по контракту adapter различаются.
 */
static void test_submit_contract(void)
{
    web_at_form f;
    const char valid[] = "device=sim1&command=AT";
    const char invalid[] = "device=bogus&command=AT";
    const char injected[] = "device=sim1&command=AT%0A";
    checks = enqueues = 0;
    enqueue_result = 0;
    assert(web_at_submit(valid, sizeof(valid) - 1, &f, device_check, enqueue, NULL) == 0);
    assert(checks == 1 && enqueues == 1);
    assert(web_at_submit(injected, sizeof(injected) - 1, &f, device_check, enqueue, NULL) == -1);
    assert(checks == 1 && enqueues == 1);
    assert(web_at_submit(invalid, sizeof(invalid) - 1, &f, device_check, enqueue, NULL) == -2);
    assert(checks == 2 && enqueues == 1);
    enqueue_result = -1;
    assert(web_at_submit(valid, sizeof(valid) - 1, &f, device_check, enqueue, NULL) == -3);
    assert(checks == 3 && enqueues == 2);
}

/** Проверяет длины команды в байтах на нижней/верхней границе.
 * Однобайтовая команда допустима, 512 ASCII-байтов допустимы, 513 отклоняются;
 * это предотвращает off-by-one и выход за массив при production decode.
 */
static void test_command_length_boundaries(void)
{
    web_at_form f;
    const char one[] = "device=sim1&command=A";
    char body[WEB_AT_FORM_MAX + 2];
    size_t prefix_len = (size_t)snprintf(body, sizeof(body), "device=sim1&command=");
    assert(web_at_form_parse(one, sizeof(one) - 1, &f) == 0);
    assert(strlen(f.command) == 1);
    memset(body + prefix_len, 'X', WEB_AT_COMMAND_MAX);
    assert(web_at_form_parse(body, prefix_len + WEB_AT_COMMAND_MAX, &f) == 0);
    assert(strlen(f.command) == WEB_AT_COMMAND_MAX);
    body[prefix_len + WEB_AT_COMMAND_MAX] = 'X';
    assert(web_at_form_parse(body, prefix_len + WEB_AT_COMMAND_MAX + 1, &f) != 0);
}

/** Проверяет строгий синтаксис формы и отбрасывание управляющих/инъекционных данных.
 * Каждая строковая длина передаётся по sizeof(literal)-1, а бинарное тело — явно,
 * чтобы NUL внутри запроса не маскировал байты после него.
 */
static void test_rejected_forms(void)
{
    web_at_form f;
    const char missing[] = "device=sim1";
    const char duplicate[] = "device=sim1&device=sim2&command=AT";
    const char crlf[] = "device=sim1&command=AT%0D%0AQUIT";
    const char nul_encoded[] = "device=sim1&command=AT%00X";
    const char control_encoded[] = "device=sim1&command=AT%01X";
    const char bad_hex[] = "device=sim1&command=AT%XZ";
    const char incomplete_escape[] = "device=sim1&command=AT%";
    const char extra[] = "device=sim1&command=AT&extra=x";
    const char trailing[] = "device=sim1&command=AT&";
    const char empty_command[] = "device=sim1&command=";
    const char raw_control[] = {'d','e','v','i','c','e','=','s','i','m','1','&','c','o','m','m','a','n','d','=','A',1,'T'};
    const char nul_body[] = {'d','e','v','i','c','e','=','s','i','m','1','&','c','o','m','m','a','n','d','=','A',0,'T'};
    assert(web_at_form_parse(missing, sizeof(missing) - 1, &f) != 0);
    assert(web_at_form_parse(duplicate, sizeof(duplicate) - 1, &f) != 0);
    assert(web_at_form_parse(crlf, sizeof(crlf) - 1, &f) != 0);
    assert(web_at_form_parse(nul_encoded, sizeof(nul_encoded) - 1, &f) != 0);
    assert(web_at_form_parse(control_encoded, sizeof(control_encoded) - 1, &f) != 0);
    assert(web_at_form_parse(bad_hex, sizeof(bad_hex) - 1, &f) != 0);
    assert(web_at_form_parse(incomplete_escape, sizeof(incomplete_escape) - 1, &f) != 0);
    assert(web_at_form_parse(extra, sizeof(extra) - 1, &f) != 0);
    assert(web_at_form_parse(trailing, sizeof(trailing) - 1, &f) != 0);
    assert(web_at_form_parse(empty_command, sizeof(empty_command) - 1, &f) != 0);
    assert(web_at_form_parse(raw_control, sizeof(raw_control), &f) != 0);
    assert(web_at_form_parse(nul_body, sizeof(nul_body), &f) != 0);
}

/** Проверяет ограничения целого HTTP body и размера имени устройства.
 * Точное максимальное тело принимается только если его синтаксис валиден;
 * нулевой/превышенный body и слишком длинное поле устройства отвергаются.
 */
static void test_body_and_device_boundaries(void)
{
    web_at_form f;
    const char valid[] = "device=sim1&command=AT";
    char too_long_device[WEB_AT_DEVICE_MAX + 64];
    assert(web_at_form_parse(NULL, sizeof(valid) - 1, &f) != 0);
    assert(web_at_form_parse(valid, 0, &f) != 0);
    assert(web_at_form_parse(valid, WEB_AT_FORM_MAX + 1, &f) != 0);
    memset(too_long_device, 'a', sizeof(too_long_device));
    memcpy(too_long_device, "device=", 7);
    memcpy(too_long_device + 7 + WEB_AT_DEVICE_MAX + 1, "&command=AT", 11);
    assert(web_at_form_parse(too_long_device, 7 + WEB_AT_DEVICE_MAX + 1 + 11, &f) != 0);
}

/** Проверяет ошибочные callback-аргументы: интерфейс не должен вызывать неполный pipeline. */
static void test_submit_invalid_callbacks(void)
{
    web_at_form f;
    const char body[] = "device=sim1&command=AT";
    assert(web_at_submit(body, sizeof(body) - 1, NULL, device_check, enqueue, NULL) == -1);
    assert(web_at_submit(body, sizeof(body) - 1, &f, NULL, enqueue, NULL) == -1);
    assert(web_at_submit(body, sizeof(body) - 1, &f, device_check, NULL, NULL) == -1);
}

/** Запускает независимые регрессионные сценарии формы, HTTP-границ, submit-адаптеров и ответа. */
int main(void)
{
    test_valid_form();
    test_http_reader();
    test_later_pipeline_bytes_remain_unread();
    test_http_exact_size_boundaries();
    test_response_contract();
    test_command_length_boundaries();
    test_rejected_forms();
    test_body_and_device_boundaries();
    test_submit_invalid_callbacks();
    test_submit_contract();
    return 0;
}
