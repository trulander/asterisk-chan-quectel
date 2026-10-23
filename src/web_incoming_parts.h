/* Узкие чистые mini-web helpers для incoming-parts маршрута и HTTP контрактов. */
#ifndef CHAN_QUECTEL_WEB_INCOMING_PARTS_H
#define CHAN_QUECTEL_WEB_INCOMING_PARTS_H
#include <stddef.h>

/** Класс результата классификации только нового /sms/parts семейства. */
enum web_incoming_route_kind {
    WEB_INCOMING_NOT_ROUTE, /*!< Путь принадлежит существующему/неизвестному handler. */
    WEB_INCOMING_ALL_HTML, /*!< Коллекция сегментов всех устройств в HTML. */
    WEB_INCOMING_ALL_JSON, /*!< Та же коллекция в JSON. */
    WEB_INCOMING_SIM, /*!< Per-device route; tail требует отдельной SIM валидации. */
};

/** Константные поля небольшого HTTP error ответа; body не принадлежит caller. */
struct web_incoming_error {
    const char *status; /*!< HTTP status line без CRLF. */
    const char *content_type; /*!< MIME type response body. */
    const char *body; /*!< Статический NUL-terminated body. */
    size_t body_len; /*!< Точная длина body без NUL; передаётся в socket send. */
};

/** Классифицировать новый маршрут и определить представление.
 * \param path NUL-terminated path без query string.
 * \param tail обязательный выход: полный per-SIM компонент либо NULL для коллекции.
 * \param is_json обязательный выходной mode: JSON=true для финального .json suffix.
 * \return route kind; некорректная/пустая SIM далее получает 404 от caller parser.
 * Строка tail ссылается внутрь path и не передаёт владение. Dots до последнего .json
 * остаются частью SIM; legacy /sms и /json сюда не маршрутизируются.
 */
enum web_incoming_route_kind web_incoming_route(const char *path, const char **tail, int *is_json);

/** Вернуть статический ответ 404 text/plain; output должен быть ненулевым. */
void web_incoming_not_found_response(struct web_incoming_error *out);
/** Вернуть статический HTTP 500, выбирая JSON или HTML-request text/plain error.
 * body_len всегда равен длине литерала без NUL, чтобы socket не читал за объектом.
 */
void web_incoming_error_response(int is_json, struct web_incoming_error *out);
/** Экранировать HTML text node: вернуть требуемый byte count без NUL.
 * NULL input трактуется как пустая строка; output может быть NULL для подсчёта.
 * При ограниченном буфере сохраняется NUL и уже помещённый префикс; caller должен
 * выделять returned length + 1, чтобы получить полное значение.
 */
size_t web_incoming_html_escape(const char *input, char *output, size_t output_size);
/** Построить relative pagination URL для all/per-device страницы и HTML/JSON режима.
 * \param out writable output; \param size его размер с NUL; \param device NULL/empty для all.
 * \param page 1-based строго положительный номер; \param json любой ненулевой mode.
 * \return 0 при полном URL, -1 при аргументе/переполнении буфера. Device должен быть
 * предварительно проверенным конфигурационным SIM identifier, а не произвольным вводом.
 */
int web_incoming_page_url(char *out, size_t size, const char *device, int page, int json);
#endif
