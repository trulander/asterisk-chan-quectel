/*
    mt_task.h

    MT delivery task: delivers incoming SMS (from SIM) to a SIP endpoint
    (Flexisip) via res_pjsip MESSAGE, tracks delivery state in the SMS DB,
    retries on failure and serves a small HTTP status page.
*/

#ifndef CHAN_QUECTEL_MT_TASK_H_INCLUDED
#define CHAN_QUECTEL_MT_TASK_H_INCLUDED

#include <stddef.h>

/* Start the MT retry and web threads (idempotent). */
int mt_task_start(void);
/* Stop all MT threads and release resources. */
void mt_task_stop(void);
/* Wake the retry thread so it processes the queue immediately. */
void mt_kick(void);
/* Generate a unique message id for a device (e.g. sim1-<time>-<seq>). */
void mt_gen_mid(const char* device, char* mid, size_t len);
/* Test hook: insert an "incoming" SMS into mt_queue (state=new) and kick. No AT involved. */
int mt_test_inject(const char* device, const char* from, const char* msg);
/* Send a fire-and-forget RFC 5438 delivery notification for an outgoing SMS. */
void mt_send_imdn_receipt(const char* device, const char* imdn_id, const char* to_gsm_node_hint, const char* state);

#endif /* CHAN_QUECTEL_MT_TASK_H_INCLUDED */