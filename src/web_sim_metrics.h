#ifndef CHAN_QUECTEL_WEB_SIM_METRICS_H_INCLUDED
#define CHAN_QUECTEL_WEB_SIM_METRICS_H_INCLUDED
#include <stddef.h>
#include "sim_metrics.h"

/** Сформировать безопасный HTML-фрагмент со счётчиками карточки одной SIM.
 * Возвращает 0 при полном форматировании, -1 при недостаточном буфере/аргументе.
 */
int web_sim_metrics_render(char *dst, size_t capacity, const char *device, const struct sim_metrics *metrics);
#endif
