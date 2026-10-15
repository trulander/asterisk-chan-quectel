/*
   Pure (Asterisk-free) helper for the queue-native pause used by the CFUN
   soft-modem-reset recovery. Kept dependency-free so it can be unit tested
   without an Asterisk runtime (see test/at_pause_test.c).
*/
#ifndef CHAN_QUECTEL_AT_PAUSE_H_INCLUDED
#define CHAN_QUECTEL_AT_PAUSE_H_INCLUDED

#include <sys/time.h> /* struct timeval */

/*!
 * \brief Convert a queue-native pause's relative duration into an absolute
 * deadline, arming it exactly once.
 *
 * A pause command is never written to the device, so its timeout field is not
 * armed by the normal write path. The deadline must be armed the first time
 * the pause becomes the head command and MUST NOT be re-armed afterwards:
 * at_queue_run() is re-entered on every URC/response, and re-basing the
 * deadline on each pass would let modem chatter postpone the pause
 * indefinitely.
 *
 * The arming state is tracked with an explicit flag bit (arm_mask) inside the
 * caller-owned flags word -- never by comparing timestamps.
 *
 * \param timeout[in/out] - relative duration on entry, absolute deadline on
 *                          exit (armed); must be non-NULL
 * \param flags[in/out]   - caller-owned flags word that stores the armed state
 * \param arm_mask        - the flag bit that means "already armed"
 * \param now             - wall clock used to compute the absolute deadline
 * \retval 1 deadline armed by this call (first arming)
 * \retval 0 already armed; deadline left untouched
 */
static inline int at_pause_arm_once(struct timeval* timeout, unsigned* flags, unsigned arm_mask, const struct timeval* now)
{
    if (*flags & arm_mask) {
        return 0;
    }
    /* read the relative duration before overwriting the field */
    const long rsec  = timeout->tv_sec;
    const long rusec = timeout->tv_usec;

    timeout->tv_sec  = now->tv_sec + rsec;
    timeout->tv_usec = now->tv_usec + rusec;
    if (timeout->tv_usec >= 1000000L) {
        timeout->tv_usec -= 1000000L;
        timeout->tv_sec += 1L;
    }
    *flags |= arm_mask;
    return 1;
}

#endif /* CHAN_QUECTEL_AT_PAUSE_H_INCLUDED */