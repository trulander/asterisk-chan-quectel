#ifndef CHAN_QUECTEL_SMS_DELETE_FLOW_H_INCLUDED
#define CHAN_QUECTEL_SMS_DELETE_FLOW_H_INCLUDED

/*! Result of the current AT/CMGL read, passed to the common orchestration seam. */
typedef enum sms_flow_read_result {
    SMS_FLOW_READ_ERROR, /*!< No reliable cell contents; schedule retry only. */
    SMS_FLOW_CELL_ABSENT, /*!< Fresh list proves the target cell is empty. */
    SMS_FLOW_CELL_PRESENT, /*!< Target cell was read and parsed successfully. */
} sms_flow_read_result_t;

/*! Distinguishes a SIM-backed cell from a direct-delivery indication. */
typedef enum sms_flow_source {
    SMS_FLOW_SIM_CELL,
    SMS_FLOW_RES_CMT, /*!< Unstored indication; it has no addressable cell to delete. */
} sms_flow_source_t;

/*! Cell identity and policy for one production incoming-message event. */
typedef struct sms_flow_request {
    int sms_autodelete; /*!< Explicit deletion gate; false bypasses every callback. */
    sms_flow_source_t source; /*!< Only SMS_FLOW_SIM_CELL can reach save/delete callbacks. */
    const char* device; /*!< Source modem identity used to correlate durable rows. */
    const char* storage; /*!< Must be "SM"; ME is intentionally unsupported. */
    int index; /*!< Current modem index, not a durable identity by itself. */
    int complete; /*!< Initial assembly hint; save callback returns the authoritative result. */
} sms_flow_request_t;

/*! Outputs from the durable-save callback, consumed before queue/delete callbacks. */
typedef struct sms_flow_result {
    int complete; /*!< True only after all parts assemble in sequence. */
    long long durable_rowid; /*!< Exact segment row to which delete state belongs. */
} sms_flow_result_t;

/*! Side effects supplied by production and SQLite-backed tests. Each successful
 * callback denotes a durable operation before the next callback may run. */
typedef struct sms_flow_ops {
    int (*save_cell)(void* context, sms_flow_result_t* result); /*!< COMMIT the current segment and return rowid/assembly status. */
    int (*queue_complete_sms)(void* context, const sms_flow_result_t* result); /*!< Persist full MT row and group marker. */
    int (*delete_cell)(void* context, const sms_flow_result_t* result); /*!< Persist queue state, then enqueue fresh CMGD. */
    int (*mark_absent)(void* context); /*!< Persist confirmed absence and eligible GC. */
    void (*defer_retry)(void* context); /*!< Record a retry without consuming a confirmed deletion failure. */
} sms_flow_ops_t;

/* Runs the stateful production sequence after an AT read: save -> optional
 * complete-message queue -> delete; absent and read-error outcomes never CMGD.
 * Returns 0 on success, 1 when policy/source excludes the event, -1 on failure. */
int sms_delete_flow_process(const sms_flow_request_t* request, sms_flow_read_result_t read_result,
                            const sms_flow_ops_t* ops, void* context, sms_flow_result_t* result);

#endif
