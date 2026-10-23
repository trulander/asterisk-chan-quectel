#include "mt_retry.h"

#include <limits.h>

/** Рассчитать retry/accept/delivery transition без Asterisk/PJSIP зависимостей.
 * Политика совпадает с MT callback: provisional/202 сохраняют очередь в queued,
 * 2xx завершают delivered, ошибки увеличивают attempts и после лимита исключаются
 * из picker-а. Время и jitter инъецируются, чтобы тестировать точную due boundary.
 */
int mt_retry_plan_response(int status_code, int current_attempts, int now,
                           int max_attempts, int base_delay, int max_delay,
                           int jitter, mt_retry_plan_t* plan)
{
    if (!plan || current_attempts < 0 || max_attempts <= 0 || base_delay < 0 ||
        max_delay < 0 || jitter < 0 || now < 0) return -1;

    plan->attempts = current_attempts;
    plan->next_retry_at = 0;
    if (status_code == 202 || (status_code >= 100 && status_code < 200)) {
        plan->action = MT_RETRY_ACTION_QUEUED;
        plan->state = "queued";
        return 0;
    }
    if (status_code >= 200 && status_code < 300) {
        plan->action = MT_RETRY_ACTION_DELIVERED;
        plan->state = "delivered";
        return 0;
    }

    plan->attempts = current_attempts + 1;
    if (plan->attempts >= max_attempts) {
        plan->action = MT_RETRY_ACTION_EXHAUSTED;
        plan->state = "retrying";
        return 0;
    }

    const int shift = plan->attempts < 8 ? plan->attempts : 8;
    long delay = (long)base_delay << shift;
    if (delay > max_delay) delay = max_delay;
    delay += jitter;
    if (delay > INT_MAX - now) return -1;

    plan->action = MT_RETRY_ACTION_RETRY;
    plan->state = "retrying";
    plan->next_retry_at = now + (int)delay;
    return 0;
}
