#include "sms_delete_state.h"

int sms_delete_state_apply(sms_delete_state_t* const state, sms_delete_event_t event)
{
    if (!state) return -1;
    switch (event) {
        case SMS_DELETE_EVENT_SAVED:
            if (state->status == SMS_DELETE_COMPLETE || state->status == SMS_DELETE_STOPPED) return -1;
            state->status = SMS_DELETE_PENDING;
            return 0;
        case SMS_DELETE_EVENT_DELETE_QUEUED:
            if (state->status != SMS_DELETE_PENDING) return -1;
            state->status = SMS_DELETE_CMGD_SENT;
            return 0;
        case SMS_DELETE_EVENT_QUEUE_FAILED:
            if (state->status != SMS_DELETE_CMGD_SENT) return -1;
            state->status = SMS_DELETE_PENDING;
            return 0;
        case SMS_DELETE_EVENT_CMGD_OK:
            if (state->status != SMS_DELETE_CMGD_SENT) return -1;
            state->status = SMS_DELETE_COMPLETE;
            return 0;
        case SMS_DELETE_EVENT_CMGD_ERROR:
        case SMS_DELETE_EVENT_CMGD_TIMEOUT:
            if (state->status != SMS_DELETE_CMGD_SENT) return -1;
            state->status = SMS_DELETE_VERIFY;
            return 0;
        case SMS_DELETE_EVENT_VERIFY_START:
            if (state->status == SMS_DELETE_VERIFY) {
                state->status = SMS_DELETE_VERIFYING;
                return 0;
            }
            if (state->status == SMS_DELETE_VERIFYING) return 0;
            if (state->status == SMS_DELETE_PENDING || state->status == SMS_DELETE_CMGD_SENT || state->status == SMS_DELETE_VERIFYING_UNCOUNTED) {
                state->status = SMS_DELETE_VERIFYING_UNCOUNTED;
                return 0;
            }
            return -1;
        case SMS_DELETE_EVENT_VERIFY_ABSENT:
            if (state->status != SMS_DELETE_VERIFYING && state->status != SMS_DELETE_VERIFYING_UNCOUNTED) return -1;
            state->status = SMS_DELETE_COMPLETE;
            return 0;
        case SMS_DELETE_EVENT_VERIFY_PRESENT_SAVED:
            /* VERIFYING is entered from VERIFY, which is set only after CMGD
             * ERROR/TIMEOUT. Count only after the fresh-present segment COMMIT.
             * A recovered CMGD_SENT row instead uses VERIFYING_UNCOUNTED. */
            if (state->status == SMS_DELETE_VERIFYING) {
                if (state->attempts < SMS_DELETE_MAX_ATTEMPTS) ++state->attempts;
                state->status = state->attempts >= SMS_DELETE_MAX_ATTEMPTS ? SMS_DELETE_STOPPED : SMS_DELETE_PENDING;
                return 0;
            }
            if (state->status == SMS_DELETE_VERIFYING_UNCOUNTED) {
                state->status = SMS_DELETE_PENDING;
                return 0;
            }
            return -1;
        case SMS_DELETE_EVENT_VERIFY_READ_ERROR:
        case SMS_DELETE_EVENT_DB_ERROR:
            if (state->status == SMS_DELETE_VERIFYING) {
                state->status = SMS_DELETE_VERIFY;
                return 0;
            }
            if (state->status == SMS_DELETE_VERIFYING_UNCOUNTED || state->status == SMS_DELETE_PENDING) {
                state->status = SMS_DELETE_PENDING;
                return 0;
            }
            if (state->status == SMS_DELETE_VERIFY) return 0;
            return -1;
    }
    return -1;
}
