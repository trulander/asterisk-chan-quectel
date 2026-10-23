#ifndef CHAN_QUECTEL_SMS_DELETE_FLOW_H_INCLUDED
#define CHAN_QUECTEL_SMS_DELETE_FLOW_H_INCLUDED

/** Результат чтения текущей ячейки SM, полученный после CMGR или свежего CMGL.
 * Значение отличает достоверное отсутствие SMS от ошибки чтения: только первое
 * разрешает завершить удаление, ошибка должна оставить durable job на retry.
 */
typedef enum sms_flow_read_result {
    SMS_FLOW_READ_ERROR, /*!< Ячейка не прочитана достоверно; требуется только retry без CMGD. */
    SMS_FLOW_CELL_ABSENT, /*!< Свежий список доказал, что адресуемая ячейка пуста. */
    SMS_FLOW_CELL_PRESENT, /*!< Содержимое ячейки успешно прочитано и может быть сохранено. */
} sms_flow_read_result_t;

/** Источник SMS: только SIM-cell имеет storage/index и подлежит удалению. */
typedef enum sms_flow_source {
    SMS_FLOW_SIM_CELL, /*!< SMS прочитана из адресуемой ячейки SIM-хранилища. */
    SMS_FLOW_RES_CMT, /*!< Прямое уведомление без ячейки; delete callback запрещён. */
} sms_flow_source_t;

/** Входная политика и идентичность одного события приёма SMS.
 * Вызывающий владеет строками на всё время sms_delete_flow_process(); функция
 * их не сохраняет после возврата. Удаление допускается только при gate и SM.
 */
typedef struct sms_flow_request {
    int sms_autodelete; /*!< Ненулевое значение включает обработку SIM-cell callbacks. */
    sms_flow_source_t source; /*!< SIM_CELL разрешает pipeline; RES_CMT исключает его. */
    const char* device; /*!< Имя устройства, связывающее ячейку с durable source row. */
    const char* storage; /*!< Storage модема; допустимый delete storage только "SM". */
    int index; /*!< Текущий неотрицательный индекс ячейки; сам по себе не является durable ID. */
    int complete; /*!< Подсказка полноты сборки; результат save callback является авторитетным. */
} sms_flow_request_t;

/** Результат durable save, используемый далее для queue и delete callbacks.
 * Rowid должен точно идентифицировать сохранённый сегмент, а не один лишь слот SIM.
 */
typedef struct sms_flow_result {
    int complete; /*!< Ненулевое значение означает полную собранную SMS. */
    long long durable_rowid; /*!< SQLite rowid сегмента, сохранённого до любых действий SIM. */
} sms_flow_result_t;

/** Набор побочных операций production policy и in-memory SQLite теста.
 * Callback success означает успешное durable завершение своей операции; следующий
 * callback до этого результата вызываться не должен. Контекст принадлежит caller.
 */
typedef struct sms_flow_ops {
    int (*save_cell)(void* context, sms_flow_result_t* result); /*!< COMMIT сегмента; заполнить rowid/assembly result. */
    int (*queue_complete_sms)(void* context, const sms_flow_result_t* result); /*!< Атомарно сохранить MT row и group marker. */
    int (*delete_cell)(void* context, const sms_flow_result_t* result); /*!< Сохранить delete state и только затем поставить свежий CMGD. */
    int (*mark_absent)(void* context); /*!< Сохранить подтверждённое отсутствие и проверить group GC. */
    void (*defer_retry)(void* context); /*!< Отложить временную ошибку, не расходуя deletion attempts. */
} sms_flow_ops_t;

/** Выполнить общий порядок действий после результата чтения AT ячейки.
 * \param request политика, источник и точная SIM identity; обязателен.
 * \param read_result достоверный результат свежего чтения или READ_ERROR.
 * \param ops полный набор callbacks; callbacks синхронны и выполняют свои DB commit.
 * \param context непрозрачный контекст, передаваемый callbacks без передачи владения.
 * \param result выходная структура durable save; может быть NULL только если вызывающий
 *        не использует детали (production передаёт указатель).
 * \return 0 обработано; 1 исключено gate/source/storage; -1 временная ошибка.
 * Pipeline сохраняет → при complete ставит durable queue → ставит CMGD; absent
 * и read error не удаляют ячейку. Функция сама не захватывает SQLite mutex: это
 * ответственность callback-владельца и не должно расширять lock через AT enqueue.
 */
int sms_delete_flow_process(const sms_flow_request_t* request, sms_flow_read_result_t read_result,
                            const sms_flow_ops_t* ops, void* context, sms_flow_result_t* result);

#endif
