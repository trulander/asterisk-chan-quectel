/* Dependency-light state transitions for incoming SIM SMS deletion. */
#ifndef CHAN_QUECTEL_SMS_DELETE_STATE_H_INCLUDED
#define CHAN_QUECTEL_SMS_DELETE_STATE_H_INCLUDED

/*! Persisted incoming_msg.delete_status values; keep numeric values migration-stable. */
typedef enum sms_delete_status {
    SMS_DELETE_PENDING = 0, /*!< Durable SIM part exists; first/recovery cell check is due. */
    SMS_DELETE_VERIFY = 1, /*!< A CMGD error/timeout awaits a fresh cell read. */
    SMS_DELETE_COMPLETE = 2, /*!< Cell is confirmed absent or CMGD succeeded. */
    SMS_DELETE_STOPPED = 3, /*!< Five confirmed failures; automatic checks stop. */
    SMS_DELETE_CMGD_SENT = 4, /*!< CMGD was queued; restart must verify, never replay it. */
    SMS_DELETE_VERIFYING = 5, /*!< Fresh read following a confirmed CMGD error; presence counts. */
    SMS_DELETE_VERIFYING_UNCOUNTED = 6, /*!< Recovery/read after ambiguous or initial queue; presence alone does not count. */
} sms_delete_status_t;

/*! State persisted per incoming_msg segment. */
typedef struct sms_delete_state {
    sms_delete_status_t status; /*!< One of the persisted statuses above. */
    unsigned attempts; /*!< Confirmed CMGD failures only; read/DB failures do not change it. */
} sms_delete_state_t;

/*! Events from persistence and AT callbacks; VERIFY_PRESENT_SAVED is only sent
 * after a prior CMGD ERROR/TIMEOUT and a fresh, durable present-cell read. */
typedef enum sms_delete_event {
    SMS_DELETE_EVENT_SAVED, /*!< New or re-saved SIM segment. */
    SMS_DELETE_EVENT_DELETE_QUEUED, /*!< A fresh CMGD task is attached to the row. */
    SMS_DELETE_EVENT_QUEUE_FAILED, /*!< AT queue rejected CMGD; return to pending. */
    SMS_DELETE_EVENT_CMGD_OK, /*!< Modem confirmed CMGD success. */
    SMS_DELETE_EVENT_CMGD_ERROR, /*!< Modem rejected CMGD; verify actual cell contents. */
    SMS_DELETE_EVENT_CMGD_TIMEOUT, /*!< CMGD result is uncertain; verify, never replay. */
    SMS_DELETE_EVENT_VERIFY_START, /*!< Begin a fresh SIM read after failure/recovery. */
    SMS_DELETE_EVENT_VERIFY_ABSENT, /*!< Fresh read proves the cell is empty. */
    SMS_DELETE_EVENT_VERIFY_PRESENT_SAVED, /*!< Present cell durably saved after prior failed CMGD. */
    SMS_DELETE_EVENT_VERIFY_READ_ERROR, /*!< Read failed; retry without counting. */
    SMS_DELETE_EVENT_DB_ERROR, /*!< Durable save/state operation failed; retry without counting. */
} sms_delete_event_t;

/*! Apply one event to the in-memory state; callers persist it in the same DB transaction as related changes. */
int sms_delete_state_apply(sms_delete_state_t* state, sms_delete_event_t event);
#define SMS_DELETE_MAX_ATTEMPTS 5u /*!< Confirmed failures allowed before automatic checking stops. */
#endif
