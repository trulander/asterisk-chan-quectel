#include "web_sim_metrics.h"
#include <stdio.h>
#include <string.h>

/** Экранировать имя SIM перед включением в HTML-текст; размер выхода ограничен буфером. */
static int html_escape(char *dst, size_t cap, const char *src)
{
    size_t used = 0;
    for (; *src; ++src) {
        const char *rep = *src == '&' ? "&amp;" : *src == '<' ? "&lt;" : *src == '>' ? "&gt;" : *src == '\"' ? "&quot;" : *src == '\'' ? "&#39;" : NULL;
        size_t n = rep ? strlen(rep) : 1;
        if (used + n >= cap) return -1;
        if (rep) memcpy(dst + used, rep, n); else dst[used] = *src;
        used += n;
    }
    dst[used] = '\0';
    return 0;
}

/** Рендерить единый видимый HTML-фрагмент, используемый home-card production кодом.
 * Экранированное имя не может внедрить разметку; числа выводятся только десятичными значениями.
 */
int web_sim_metrics_render(char *dst, size_t capacity, const char *device, const struct sim_metrics *metrics)
{
    char escaped[256];
    int n;
    if (!dst || !capacity || !device || !metrics || html_escape(escaped, sizeof(escaped), device)) return -1;
    n = snprintf(dst, capacity,
        "<tr><th>Всего SMS</th><td>%d</td></tr><tr><th>SMS не доставлено</th><td>%d</td></tr>"
        "<tr><th>Всего звонков</th><td>%d</td></tr><!-- SIM: %s -->",
        metrics->sms_total, metrics->sms_undelivered, metrics->calls_total, escaped);
    return n >= 0 && (size_t)n < capacity ? 0 : -1;
}
