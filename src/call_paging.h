#ifndef CHAN_QUECTEL_CALL_PAGING_H
#define CHAN_QUECTEL_CALL_PAGING_H
#include <sqlite3.h>
#include <stddef.h>
#define CALL_PAGE_SIZE 30
#define CALL_PAGE_TEXT_LEN 256
/** Единое JSON тело ошибки call routes; длина всегда вычисляется sizeof-1. */
#define CALL_WEB_JSON_ERROR "{\"error\":\"call database unavailable\"}"
struct call_page_row {
    long long id; /*!< rowid AUTOINCREMENT: устойчивый ID, UI формирует call-<id>. */
    char device[64]; /*!< Сохранённое имя SIM; SQL фильтрует его точным равенством. */
    char number[CALL_PAGE_TEXT_LEN]; /*!< Сохранённый номер; длинный текст ограничивается буфером UI. */
    char started_at[64]; /*!< UTC время начала, сохранённое в БД и используемое сортировкой. */
    char direction[16]; /*!< Сохранённая классификация incoming/outgoing/missed. */
    char ended_at[64]; /*!< UTC время окончания; пусто для SQL NULL. */
    int answered; /*!< Существующий durable флаг ответа, без выводов рендерера. */
    int finished; /*!< Существующий durable флаг окончания, без выводов рендерера. */
};
/* Callback получает временную копию строки: вернуть 0 для продолжения, nonzero для успешной остановки обхода. */
typedef int (*call_page_cb)(void*, const struct call_page_row*);
/** Сосчитать строки только указанной SIM; db и выходной total принадлежат caller. Ошибка SQL возвращает -1. */
int call_paging_count(sqlite3* db, const char* device, int* total);
/** Вычислить UTC duration в `Dд HH:MM:SS`.
 * Входы должны иметь точный формат `YYYY-MM-DD HH:MM:SS UTC`; отсутствующие,
 * невозможные, переполненные даты и end раньше start дают -1 и пустой output.
 * Нулевой интервал допустим. Функция не меняет данные и предназначена для HTML,
 * поэтому длительность не добавляется в SQL/JSON schema.
 */
int call_paging_duration(const char* started_at, const char* ended_at, char* output, size_t output_size);
/** Собрать точный HTML-фрагмент заголовка или ячейки Duration для renderer-а.
 * header!=0 создаёт `<th>Duration</th>`; иначе выдаёт рассчитанную ячейку либо
 * `<td>&mdash;</td>` при отсутствующей/некорректной дате. В output не попадают
 * исходные данные пользователя, только вычисленный текст и фиксированная разметка.
 * Возвращает 0 при полном фрагменте, -1 при неверном output/недостатке ёмкости.
 */
int call_paging_html_duration_fragment(int header, const char* started_at, const char* ended_at,
                                       char* output, size_t output_size);
/** Выдать страницу в порядке started_at/id DESC; callback получает временную row copy: 0 продолжает, nonzero прерывает обход успешно (pager возвращает 0). Callback-specific failure caller должен сохранить отдельно в ctx и проверить после возврата pager; ошибки SQL возвращают -1. */
int call_paging_page(sqlite3* db, const char* device, int limit, int offset, void* ctx, call_page_cb cb);
#endif
