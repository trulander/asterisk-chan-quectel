#ifndef CHAN_QUECTEL_CLCC_PARSER_H_INCLUDED
#define CHAN_QUECTEL_CLCC_PARSER_H_INCLUDED

/** Строго разобрать одну строку +CLCC, используемую at_parse_clcc().
 * Строка изменяется: number указывает на внутренний NUL-терминированный номер.
 * Успех — 0; любой мусор, переполнение, лишнее поле или неверное enum — -1.
 */
int clcc_parse_line(char* line, unsigned* call_idx, unsigned* dir, unsigned* state, unsigned* mode,
                    unsigned* mpty, char** number, unsigned* toa);

/** Выбрать severity для строки, отклонённой строгим parser-ом.
 * Вызывается только после попытки release-prefix fallback: ненулевой результат
 * означает, что строка также отвергнута fallback и требует ERROR; ноль означает
 * распознанный частичный state=6 и требует WARNING. Чистая функция нужна, чтобы
 * тест проверял ту же production-классификацию, которую использует AT handler.
 */
int clcc_parse_failure_is_error(int release_prefix_result);

/** Разобрать строгий префикс CLCC для vendor release status 6.
 * Принимает только индекс, направление, state=6, voice mode и mpty в допустимом
 * диапазоне; поля номера намеренно не требуются, так как modem может оборвать их.
 * Успех — 0 и заполненные identity поля; любая неоднозначность — -1.
 */
int clcc_parse_release_prefix(char* line, unsigned* call_idx, unsigned* dir, unsigned* mode,
                              unsigned* mpty);

#endif
