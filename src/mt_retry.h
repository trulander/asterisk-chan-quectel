#ifndef CHAN_QUECTEL_MT_RETRY_H_INCLUDED
#define CHAN_QUECTEL_MT_RETRY_H_INCLUDED

/** SQL общего production picker-а mt_queue; параметры: now, max_attempts.
 * `new` готов к первой попытке, `retrying` выбирается только после due времени и
 * пока не исчерпан конечный лимит. Порядок по created_at сохраняет FIFO доставки.
 */
#define MT_RETRY_PICK_SQL \
    "SELECT device, mid, from_num, msg, ts, state, attempts, next_retry_at, last_result, created_at, sent_at, delivered_at " \
    "FROM mt_queue WHERE (state = 'new') OR (state = 'retrying' AND next_retry_at <= ? AND attempts < ?) " \
    "ORDER BY created_at LIMIT 1"

typedef enum mt_retry_action {
    MT_RETRY_ACTION_RETRY = 0,
    MT_RETRY_ACTION_QUEUED,
    MT_RETRY_ACTION_DELIVERED,
    MT_RETRY_ACTION_EXHAUSTED
} mt_retry_action_t;

typedef struct mt_retry_plan {
    mt_retry_action_t action; /**< Итоговый переход для queue row. */
    int attempts;             /**< Число неудачных попыток после ответа. */
    int next_retry_at;        /**< Epoch seconds; 0 для accepted/delivered/exhausted. */
    const char* state;        /**< Имя состояния mt_queue для smsdb_mt_set_result(). */
} mt_retry_plan_t;

/** Рассчитать production-переход MT callback для SIP-кода или transport timeout.
 * status_code=0 означает transport error, 1xx/202 — принято/ожидает endpoint,
 * 2xx — delivered; остальные ответы планируют retry. now и backoff задаются caller-ом,
 * jitter — уже ограниченное неотрицательное число секунд. Возвращает 0 либо -1.
 */
int mt_retry_plan_response(int status_code, int current_attempts, int now,
                           int max_attempts, int base_delay, int max_delay,
                           int jitter, mt_retry_plan_t* plan);

#endif
