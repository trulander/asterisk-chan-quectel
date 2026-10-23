#ifndef CHAN_QUECTEL_SIM_METRICS_H_INCLUDED
#define CHAN_QUECTEL_SIM_METRICS_H_INCLUDED

#include <sqlite3.h>

/** Per-SIM счётчики общей SMS-истории, недоставленных SMS и всех вызовов.
 * Каждый результат принадлежит устройству device; функции не изменяют БД.
 */
struct sim_metrics {
    int sms_total; /*!< Все строки mt_queue и mt_out для одной SIM. */
    int sms_undelivered; /*!< Все SMS, кроме подтверждённых состояний доставки. */
    int calls_total; /*!< Все строки call_log для одной SIM, включая активные. */
};

/** Прочитать метрики одним SQL statement.
 * db и device принадлежат вызывающему коду; mutex должен удерживаться владельцем db.
 * Обновляются sms_undelivered и calls_total; sms_total заполняет wrapper существующим pager count.
 * 0 — оба SQL результата получены, -1 — ошибка подготовки/выполнения SQL либо аргументов.
 */
int sim_metrics_read(sqlite3 *db, const char *device, struct sim_metrics *out);

#endif
