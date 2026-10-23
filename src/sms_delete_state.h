/* Чистая dependency-light state machine для durable deletion полей incoming_msg. */
#ifndef CHAN_QUECTEL_SMS_DELETE_STATE_H_INCLUDED
#define CHAN_QUECTEL_SMS_DELETE_STATE_H_INCLUDED

/** Persisted значения incoming_msg.delete_status; числовые коды миграционно стабильны. */
typedef enum sms_delete_status {
    SMS_DELETE_PENDING = 0, /*!< Сегмент durable сохранён; ожидает первого/повторного чтения ячейки. */
    SMS_DELETE_VERIFY = 1, /*!< CMGD завершился ERROR/TIMEOUT; нужна свежая проверка слота. */
    SMS_DELETE_COMPLETE = 2, /*!< Пустой слот подтверждён либо CMGD получил OK. */
    SMS_DELETE_STOPPED = 3, /*!< Достигнут предел подтверждённых ошибок; автоматика остановлена. */
    SMS_DELETE_CMGD_SENT = 4, /*!< CMGD поставлен; при restart его нельзя повторять, только проверить ячейку. */
    SMS_DELETE_VERIFYING = 5, /*!< Проверка после подтверждённой ошибки CMGD; durable presence засчитывается. */
    SMS_DELETE_VERIFYING_UNCOUNTED = 6, /*!< Восстановительная/неоднозначная проверка; presence сам по себе не засчитывается. */
} sms_delete_status_t;

/** Состояние одной durable segment row; хранится рядом с rowid/source identity. */
typedef struct sms_delete_state {
    sms_delete_status_t status; /*!< Код из sms_delete_status_t, сохраняемый в incoming_msg.delete_status. */
    unsigned attempts; /*!< Подтверждённые CMGD failure: fresh read снова нашёл сохранённую SMS. */
} sms_delete_state_t;

/** Входные события state machine, порождённые SQLite save и AT callbacks. */
typedef enum sms_delete_event {
    SMS_DELETE_EVENT_SAVED, /*!< Новая либо заново сохранённая часть ячейки. */
    SMS_DELETE_EVENT_DELETE_QUEUED, /*!< Новая CMGD задача принята AT queue. */
    SMS_DELETE_EVENT_QUEUE_FAILED, /*!< CMGD не поставлен; вернуть запись в retryable pending. */
    SMS_DELETE_EVENT_CMGD_OK, /*!< Модем подтвердил выполнение CMGD. */
    SMS_DELETE_EVENT_CMGD_ERROR, /*!< Модем отказал; старую команду не повторять, читать ячейку заново. */
    SMS_DELETE_EVENT_CMGD_TIMEOUT, /*!< Результат неоднозначен; обязательно свежее чтение, без replay CMGD. */
    SMS_DELETE_EVENT_VERIFY_START, /*!< Начата свежая проверка SIM/index. */
    SMS_DELETE_EVENT_VERIFY_ABSENT, /*!< Fresh read достоверно подтвердил пустую ячейку. */
    SMS_DELETE_EVENT_VERIFY_PRESENT_SAVED, /*!< Ячейка есть и её содержание durable сохранено после CMGD failure. */
    SMS_DELETE_EVENT_VERIFY_READ_ERROR, /*!< Проверка не удалась; retry без увеличения attempts. */
    SMS_DELETE_EVENT_DB_ERROR, /*!< Save/state transaction не удалась; retry без увеличения attempts. */
} sms_delete_event_t;

/** Применить одно событие к копии состояния.
 * \param state ненулевое состояние; изменяется только при разрешённом переходе.
 * \param event событие из sms_delete_event_t.
 * \return 0 при допустимом переходе, отрицательное значение при неизвестном событии,
 *         NULL state или запрещённом переходе.
 * Функция не выполняет SQL и не блокирует mutex. Caller должен сохранить итоговый
 * status/attempts в той же transaction, что связанное durable событие. Попытка растёт
 * только для VERIFY_PRESENT_SAVED, то есть после CMGD failure + fresh present + save.
 */
int sms_delete_state_apply(sms_delete_state_t* state, sms_delete_event_t event);

/** Максимум подтверждённых неудач CMGD до статуса STOPPED; не включает read/DB errors. */
#define SMS_DELETE_MAX_ATTEMPTS 5u
#endif
