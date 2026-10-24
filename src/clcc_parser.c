#include "clcc_parser.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/** Пропустить только AT-разрешённые пробельные символы между полями. */
static char* skip_spaces(char* cursor)
{
    while (*cursor && isspace((unsigned char)*cursor)) ++cursor;
    return cursor;
}

/** Прочитать unsigned decimal до точного разделителя с защитой от переполнения. */
static int parse_uint_field(char** cursor, char delimiter, unsigned* value)
{
    char* start = skip_spaces(*cursor);
    if (!isdigit((unsigned char)*start)) return -1;

    errno = 0;
    char* end = NULL;
    const unsigned long parsed = strtoul(start, &end, 10);
    if (errno == ERANGE || parsed > UINT_MAX || end == start) return -1;
    end = skip_spaces(end);
    if (*end != delimiter) return -1;

    *value = (unsigned)parsed;
    *cursor = end + 1;
    return 0;
}

/** Проверить одну CLCC-строку целиком, включая поля, кавычки и конец. */
int clcc_parse_line(char* line, unsigned* call_idx, unsigned* dir, unsigned* state, unsigned* mode,
                    unsigned* mpty, char** number, unsigned* toa)
{
    if (!line || !call_idx || !dir || !state || !mode || !mpty || !number || !toa) return -1;

    char* cursor = skip_spaces(line);
    static const char prefix[] = "+CLCC:";
    if (strncmp(cursor, prefix, sizeof(prefix) - 1) != 0) return -1;
    cursor = skip_spaces(cursor + sizeof(prefix) - 1);

    unsigned parsed_idx, parsed_dir, parsed_state, parsed_mode, parsed_mpty;
    if (parse_uint_field(&cursor, ',', &parsed_idx) ||
        parse_uint_field(&cursor, ',', &parsed_dir) ||
        parse_uint_field(&cursor, ',', &parsed_state) ||
        parse_uint_field(&cursor, ',', &parsed_mode) ||
        parse_uint_field(&cursor, ',', &parsed_mpty)) return -1;

    cursor = skip_spaces(cursor);
    char* parsed_number;
    if (*cursor == '"') {
        parsed_number = ++cursor;
        while (*cursor && *cursor != '"') ++cursor;
        if (*cursor != '"') return -1;
        char* quote = cursor;
        cursor = skip_spaces(cursor + 1);
        if (*cursor != ',') return -1;
        *quote = '\0';
        cursor = skip_spaces(cursor + 1);
    } else if (*cursor == ',') {
        /* +CLCC допускает опущенный номер, но разделитель type остаётся обязательным. */
        parsed_number = cursor;
        *cursor = '\0';
        cursor = skip_spaces(cursor + 1);
    } else {
        return -1;
    }

    if (!isdigit((unsigned char)*cursor)) return -1;
    errno = 0;
    char* end = NULL;
    const unsigned long toa_value = strtoul(cursor, &end, 10);
    if (errno == ERANGE || toa_value > 255u || end == cursor) return -1;
    end = skip_spaces(end);
    if (*end != '\0') return -1;

    /* Поля с определённым диапазоном проверяются до публикации результата парсинга. */
    if (!parsed_idx || parsed_dir > 1u || parsed_state > 5u || parsed_mode > 2u || parsed_mpty > 1u) return -1;

    *call_idx = parsed_idx;
    *dir = parsed_dir;
    *state = parsed_state;
    *mode = parsed_mode;
    *mpty = parsed_mpty;
    *number = parsed_number;
    *toa = (unsigned)toa_value;
    return 0;
}

/** Сопоставить результат fallback с уровнем журнала strict parse failure.
 * Результат release-prefix parser равен 0 только для допустимой partial RELEASED
 * identity; это значение должно предупреждаться, любой отказ — логироваться как ошибка.
 */
int clcc_parse_failure_is_error(int release_prefix_result)
{
    return release_prefix_result != 0;
}

/** Принимать только однозначную identity-часть vendor CLCC release status.
 * Номер может быть оборван после запятой/кавычки; он не влияет на выбор текущего
 * call_idx, но текущий release всё равно требует следующего полного snapshot barrier.
 */
int clcc_parse_release_prefix(char* line, unsigned* call_idx, unsigned* dir, unsigned* mode,
                              unsigned* mpty)
{
    if (!line || !call_idx || !dir || !mode || !mpty) return -1;

    char* cursor = skip_spaces(line);
    static const char prefix[] = "+CLCC:";
    if (strncmp(cursor, prefix, sizeof(prefix) - 1) != 0) return -1;
    cursor = skip_spaces(cursor + sizeof(prefix) - 1);

    unsigned parsed_idx, parsed_dir, parsed_state, parsed_mode, parsed_mpty;
    if (parse_uint_field(&cursor, ',', &parsed_idx) ||
        parse_uint_field(&cursor, ',', &parsed_dir) ||
        parse_uint_field(&cursor, ',', &parsed_state) ||
        parse_uint_field(&cursor, ',', &parsed_mode) ||
        parse_uint_field(&cursor, ',', &parsed_mpty)) return -1;

    /* state 6 документирован драйвером для ^DSCI как CALL_STATE_RELEASED;
     * не распространять это исключение на другие состояния или типы звонков. */
    if (!parsed_idx || parsed_dir > 1u || parsed_state != 6u || parsed_mode != 0u || parsed_mpty > 1u) return -1;

    *call_idx = parsed_idx;
    *dir = parsed_dir;
    *mode = parsed_mode;
    *mpty = parsed_mpty;
    return 0;
}
