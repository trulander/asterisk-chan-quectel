#include "call_paging.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/** Копирует SQLite TEXT в caller-owned ограниченный буфер, NULL становится пустой строкой. */
static void copy_text(char* dst, size_t size, sqlite3_stmt* st, int col)
{
    const unsigned char* s = sqlite3_column_text(st, col);
    snprintf(dst, size, "%s", s ? (const char*)s : "");
}

/** SQL count имеет точное равенство device: чужая SIM никогда не попадает в результат. */
int call_paging_count(sqlite3* db, const char* device, int* total)
{
    sqlite3_stmt* st = NULL;
    int rc;
    /* Точное равенство device фильтрует SIM; отсутствующая SIM даёт успешный 0, ошибка SQL — -1. */
    if (!db || !device || !total || sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM call_log WHERE device=?", -1, &st, NULL) != SQLITE_OK) return -1;
    if (sqlite3_bind_text(st, 1, device, -1, SQLITE_TRANSIENT) != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); return -1; }
    *total = sqlite3_column_int(st, 0);
    rc = sqlite3_finalize(st);
    return rc == SQLITE_OK ? 0 : -1;
}

/** Считать фиксированную группу ASCII-цифр без whitespace-семантики scanf.
 * offset и width задаются форматом UTC; любое нецифровое значение или выход за
 * границу строки возвращает -1, иначе записывает десятичное число в value.
 */
static int parse_ascii_digits(const char* text, size_t length, size_t offset, size_t width, int* value)
{
    int parsed = 0;
    if (!text || !value || offset > length || width > length - offset) return -1;
    for (size_t i = 0; i < width; ++i) {
        const unsigned char ch = (unsigned char)text[offset + i];
        if (ch < '0' || ch > '9') return -1;
        parsed = parsed * 10 + (ch - '0');
    }
    *value = parsed;
    return 0;
}

/** Преобразовать точную UTC-строку в секунды Unix.
 * Каждый фиксированный символ проверяется явно, поэтому whitespace и иные символы
 * вместо цифр отвергаются до timegm. Round-trip через gmtime_r сохраняет проверку
 * календарной даты и диапазона time_t; локальная timezone не участвует.
 */
static int parse_utc_timestamp(const char* text, int64_t* seconds)
{
    int year, month, day, hour, minute, second;
    struct tm parsed = {0}, normalized;
    time_t value;
    if (!text || !seconds || strlen(text) != 23 || text[4] != '-' || text[7] != '-' ||
        text[10] != ' ' || text[13] != ':' || text[16] != ':' || text[19] != ' ' ||
        text[20] != 'U' || text[21] != 'T' || text[22] != 'C' ||
        parse_ascii_digits(text, 23, 0, 4, &year) || parse_ascii_digits(text, 23, 5, 2, &month) ||
        parse_ascii_digits(text, 23, 8, 2, &day) || parse_ascii_digits(text, 23, 11, 2, &hour) ||
        parse_ascii_digits(text, 23, 14, 2, &minute) || parse_ascii_digits(text, 23, 17, 2, &second) ||
        month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
        return -1;
    }
    parsed.tm_year = year - 1900;
    parsed.tm_mon = month - 1;
    parsed.tm_mday = day;
    parsed.tm_hour = hour;
    parsed.tm_min = minute;
    parsed.tm_sec = second;
    errno = 0;
    value = timegm(&parsed);
    if ((value == (time_t)-1 && errno == EOVERFLOW) || !gmtime_r(&value, &normalized) ||
        normalized.tm_year != year - 1900 || normalized.tm_mon != month - 1 || normalized.tm_mday != day ||
        normalized.tm_hour != hour || normalized.tm_min != minute || normalized.tm_sec != second) {
        return -1;
    }
    *seconds = (int64_t)value;
    return 0;
}

/** Вычислить человекочитаемую длительность только из двух корректных UTC дат.
 * Пустой output — обязательный результат для NULL/невалидного/обратного интервала;
 * арифметика выполняется после проверки диапазона времён, дни не теряются в часах.
 */
int call_paging_duration(const char* started_at, const char* ended_at, char* output, size_t output_size)
{
    int64_t start, end, duration;
    int written;
    if (!output || !output_size) return -1;
    output[0] = '\0';
    if (parse_utc_timestamp(started_at, &start) || parse_utc_timestamp(ended_at, &end) || end < start) return -1;
    duration = end - start;
    written = snprintf(output, output_size, "%" PRId64 "д %02" PRId64 ":%02" PRId64 ":%02" PRId64,
                       duration / 86400, (duration / 3600) % 24, (duration / 60) % 60, duration % 60);
    if (written < 0 || (size_t)written >= output_size) {
        output[0] = '\0';
        return -1;
    }
    return 0;
}

/** Построить ровно HTML-фрагмент заголовка или ячейки Duration.
 * Для header=1 возвращает `<th>Duration</th>`; иначе строит `<td>` из рассчитанного
 * production duration либо явной отметки `<td>&mdash;</td>` при отсутствующих/ошибочных
 * временах. Пользовательские строки не копируются в fragment, поэтому он безопасен
 * без повторного HTML escaping; выход caller-owned и очищается при ошибке/усечении.
 */
int call_paging_html_duration_fragment(int header, const char* started_at, const char* ended_at,
                                       char* output, size_t output_size)
{
    char duration[64];
    int written;
    if (!output || !output_size) return -1;
    output[0] = '\0';
    if (header) {
        written = snprintf(output, output_size, "<th>Duration</th>");
    } else if (!call_paging_duration(started_at, ended_at, duration, sizeof(duration))) {
        written = snprintf(output, output_size, "<td>%s</td>", duration);
    } else {
        written = snprintf(output, output_size, "<td>&mdash;</td>");
    }
    if (written < 0 || (size_t)written >= output_size) {
        output[0] = '\0';
        return -1;
    }
    return 0;
}

/** SQL page сортирует durable rows новыми первыми с ID tie-break; statement всегда final-ится. */
int call_paging_page(sqlite3* db, const char* device, int limit, int offset, void* ctx, call_page_cb cb)
{
    sqlite3_stmt* st = NULL;
    int rc;
    /* LIMIT/OFFSET ограничивают страницу; durable id DESC разрешает совпавшие timestamp детерминированно. */
    if (!db || !device || !cb || limit <= 0 || offset < 0 || sqlite3_prepare_v2(db,
        "SELECT id,device,number,started_at,direction,answered,finished,ended_at FROM call_log WHERE device=? ORDER BY started_at DESC,id DESC LIMIT ? OFFSET ?", -1, &st, NULL) != SQLITE_OK) return -1;
    if (sqlite3_bind_text(st,1,device,-1,SQLITE_TRANSIENT)!=SQLITE_OK || sqlite3_bind_int(st,2,limit)!=SQLITE_OK || sqlite3_bind_int(st,3,offset)!=SQLITE_OK) { sqlite3_finalize(st); return -1; }
    while ((rc=sqlite3_step(st))==SQLITE_ROW) {
        struct call_page_row row;
        memset(&row,0,sizeof(row)); row.id=sqlite3_column_int64(st,0); copy_text(row.device,sizeof(row.device),st,1); copy_text(row.number,sizeof(row.number),st,2); copy_text(row.started_at,sizeof(row.started_at),st,3); copy_text(row.direction,sizeof(row.direction),st,4); row.answered=sqlite3_column_int(st,5); row.finished=sqlite3_column_int(st,6); copy_text(row.ended_at,sizeof(row.ended_at),st,7);
        if (cb(ctx,&row)) { sqlite3_finalize(st); return 0; }
    }
    if (rc != SQLITE_DONE) { sqlite3_finalize(st); return -1; }
    return sqlite3_finalize(st)==SQLITE_OK ? 0 : -1;
}
