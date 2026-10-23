#ifndef CHAN_QUECTEL_CLCC_PARSER_H_INCLUDED
#define CHAN_QUECTEL_CLCC_PARSER_H_INCLUDED

/** Строго разобрать одну строку +CLCC, используемую at_parse_clcc().
 * Строка изменяется: number указывает на внутренний NUL-терминированный номер.
 * Успех — 0; любой мусор, переполнение, лишнее поле или неверное enum — -1.
 */
int clcc_parse_line(char* line, unsigned* call_idx, unsigned* dir, unsigned* state, unsigned* mode,
                    unsigned* mpty, char** number, unsigned* toa);

#endif
