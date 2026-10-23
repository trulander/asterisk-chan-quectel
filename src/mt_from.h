#ifndef CHAN_QUECTEL_MT_FROM_H_INCLUDED
#define CHAN_QUECTEL_MT_FROM_H_INCLUDED

#include <stddef.h>

/** Построить безопасный SIP From URI из identity отправителя и host назначения.
 * URI delimiters в user-part percent-encode-ятся, чтобы сохранять identity без
 * изменения структуры URI; control bytes/пустые поля/недостаточный буфер отклоняются.
 * Возвращает 0 при успехе, -1 при некорректном вводе.
 */
int mt_from_uri_build(const char* sender, const char* domain, char* out, size_t out_len);

#endif
