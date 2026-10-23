#ifndef WEB_AT_COMMAND_H
#define WEB_AT_COMMAND_H

#include <stddef.h>
#include <sys/types.h>

#define WEB_AT_DEVICE_MAX 63
#define WEB_AT_COMMAND_MAX 512
#define WEB_AT_FORM_MAX 640
#define WEB_AT_HEADER_MAX 2048
#define WEB_AT_REQUEST_MAX (WEB_AT_HEADER_MAX + WEB_AT_FORM_MAX)
#define WEB_AT_RESPONSE_MAX 4096

typedef struct web_at_form {
    char device[WEB_AT_DEVICE_MAX + 1]; /**< Выбранное имя устройства: 1–63 декодированных байта. */
    char command[WEB_AT_COMMAND_MAX + 1]; /**< AT-команда: 1–512 байт без управляющих символов. */
} web_at_form;

typedef int (*web_at_device_check_fn)(void *ctx, const char *device);
typedef int (*web_at_enqueue_fn)(void *ctx, const char *device, const char *command);
typedef ssize_t (*web_at_read_fn)(void *ctx, void *buffer, size_t capacity);

typedef struct web_at_http_request {
    size_t header_len; /**< Число байт до конца HTTP-заголовков включительно. */
    size_t body_len; /**< Длина тела, совпадающая с единственным Content-Length. */
    size_t request_len; /**< Принятые байты ровно до конца заголовка и заявленного тела. */
    int has_content_type; /**< Ненулевое значение означает единственный поддерживаемый form MIME. */
} web_at_http_request;

typedef enum web_at_response_kind {
    WEB_AT_RESPONSE_FORM, /**< GET-форма; options_html содержит уже безопасные option элементы. */
    WEB_AT_RESPONSE_QUEUED, /**< Enqueue adapter вернул 0, но выполнение ещё не подтверждено. */
    WEB_AT_RESPONSE_ERROR, /**< Enqueue adapter вернул ошибку. */
    WEB_AT_RESPONSE_BAD_REQUEST /**< Некорректный/неподдерживаемый HTTP submit. */
} web_at_response_kind;

typedef struct web_at_response {
    const char *status; /**< Строка HTTP status без версии протокола. */
    const char *content_type; /**< MIME ответа, HTML UTF-8 либо plain text. */
    char body[WEB_AT_RESPONSE_MAX]; /**< Буфер ответа, владеется структурой и NUL-терминирован. */
    size_t body_len; /**< Точное число байт тела без завершающего NUL. */
} web_at_response;

/** Прочитать ровно один ограниченный HTTP request frame через callback с частичными recv.
 * Заголовок/тело сохраняются в buffer; Content-Length должен состоять из цифр, его дубли,
 * Transfer-Encoding и дубли/неподдерживаемый Content-Type отвергаются. Успех означает, что
 * заголовок и заявленное тело приняты точно; если callback уже принёс лишние байты в том же
 * чтении, frame отвергается. После завершения frame функция больше не читает: последующие
 * данные сокета могут оставаться непрочитанными. Сервер обслуживает один request на соединение
 * и закрывает его, поэтому такой следующий request не обрабатывается.
 * \param reader Callback чтения, возвращающий положительное число байт либо EOF/ошибку.
 * \param ctx Контекст callback; владение остаётся у вызывающего.
 * \param buffer Caller-owned буфер для заголовка и тела.
 * \param capacity Ёмкость buffer, достаточная для ограниченных заголовка и тела.
 * \param request Caller-owned выходные длины и MIME-флаг.
 * \return 0 для полного frame, -1 для malformed/oversized/incomplete frame или лишних
 * байт, уже возвращённых callback до момента завершения заявленного тела.
 */
int web_at_http_read(web_at_read_fn reader, void *ctx, char *buffer, size_t capacity,
                     web_at_http_request *request);

/** Разобрать URL-encoded тело: обязательны ровно device и command, без пустых пар.
 * Допустимые лимиты и управляющие байты определены web_at_form; возвращает 0 или -1.
 */
int web_at_form_parse(const char *body, size_t body_len, web_at_form *form);

/** Проверить form/device и только затем ровно один раз вызвать enqueue adapter.
 * Возвращает 0 queued, -1 malformed form/аргументы, -2 неизвестное устройство,
 * -3 ошибка очереди; adapter не вызывается при ошибках -1/-2.
 */
int web_at_submit(const char *body, size_t body_len, web_at_form *form,
                  web_at_device_check_fn device_check, web_at_enqueue_fn enqueue, void *ctx);

/** Построить production HTML/plain HTTP response для формы или результата submit.
 * Указатели device/command/options_html только читаются; response целиком владеет body.
 * Возвращает 0 при создании, -1 при неверных аргументах либо переполнении буфера.
 */
int web_at_response_build(web_at_response_kind kind, const char *device, const char *command,
                          const char *options_html, web_at_response *response);

#endif
