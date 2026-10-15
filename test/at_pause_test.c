/*
   Pure test for the queue-native pause deadline arming (src/at_pause.h).

   The CFUN soft-modem-reset pause must arm its absolute deadline exactly
   once: the first time the pause becomes the head command. at_queue_run() is
   re-entered on every URC/response, so any later call that re-bases the
   deadline would let modem chatter postpone the pause indefinitely (stalling
   the soft reset). This test pins that contract with an explicit flag bit,
   never a time heuristic.

   Covered:
     - first arming converts the relative duration into an absolute deadline
     - arming is reported once (return 1 only on the first call)
     - later calls (standing in for unrelated URC/response re-entries) report
       0 and leave the absolute deadline byte-for-byte unchanged
     - an unrelated flag bit already set in the flags word does not trigger a
       false "already armed" (mask is exact)
*/

#include <stdio.h>
#include <string.h>

#include "at_pause.h"

static int ok;
static int faults;

#define CHECK(cond, ...)                                                                                                        \
    do {                                                                                                                        \
        if (cond) {                                                                                                             \
            ok++;                                                                                                               \
        } else {                                                                                                                \
            faults++;                                                                                                           \
            printf("FAIL " __FILE__ ":%d: ", __LINE__);                                                                        \
            printf(__VA_ARGS__);                                                                                                \
            printf("\n");                                                                                                       \
        }                                                                                                                       \
    } while (0)

#define ARM_MASK 0x08u /* mirrors ATQ_CMD_FLAG_TIMEOUT_ARMED; kept local so the test is Hermetic */

static void test_first_arming_arms_deadline(void)
{
    struct timeval timeout = { 3, 0 }; /* relative pause duration (PVT_CFUN_SOFT_RESET_PAUSE_SEC) */
    unsigned flags         = 0u;       /* no unrelated bits */
    const struct timeval now = { 1000, 500000 }; /* arbitrary wall clock */

    CHECK(at_pause_arm_once(&timeout, &flags, ARM_MASK, &now) == 1, "first arming returns 1");
    CHECK(timeout.tv_sec == 1003, "deadline sec = now + relative (got %ld)", (long)timeout.tv_sec);
    CHECK(timeout.tv_usec == 500000, "deadline usec = now + relative (got %ld)", (long)timeout.tv_usec);
    CHECK((flags & ARM_MASK) != 0, "armed flag set after first arming");
}

static void test_later_calls_do_not_move_deadline(void)
{
    struct timeval timeout = { 3, 0 };
    unsigned flags         = 0u;
    const struct timeval now = { 1000, 0 };

    CHECK(at_pause_arm_once(&timeout, &flags, ARM_MASK, &now) == 1, "first arming returns 1");
    const struct timeval armed = timeout;

    /* re-entries at a later wall clock (any URC/response arriving while the
       pause waits) must NOT re-base the absolute deadline */
    const struct timeval later = { 2000, 250000 };
    CHECK(at_pause_arm_once(&timeout, &flags, ARM_MASK, &later) == 0, "later arming returns 0");
    CHECK(timeout.tv_sec == armed.tv_sec, "deadline sec unchanged on re-entry");
    CHECK(timeout.tv_usec == armed.tv_usec, "deadline usec unchanged on re-entry");
}

static void test_unrelated_flag_bits_do_not_confuse_arm_state(void)
{
    struct timeval timeout = { 3, 0 };
    /* an unrelated flag (e.g. ATQ_CMD_FLAG_IGNORE) must not count as armed */
    unsigned flags         = 0x02u;
    const struct timeval now = { 1000, 0 };

    CHECK(at_pause_arm_once(&timeout, &flags, ARM_MASK, &now) == 1, "unrelated bits present still arms on first call");
    CHECK(timeout.tv_sec == 1003, "deadline sec armed despite unrelated bits (got %ld)", (long)timeout.tv_sec);
    CHECK((flags & ARM_MASK) != 0, "armed bit set; unrelated bits preserved");
    CHECK((flags & 0x02u) != 0, "unrelated flag bit preserved");
}

static void test_usec_carry(void)
{
    struct timeval timeout = { 0, 800000 };
    unsigned flags         = 0u;
    const struct timeval now = { 4, 500000 };

    CHECK(at_pause_arm_once(&timeout, &flags, ARM_MASK, &now) == 1, "first arming returns 1");
    CHECK(timeout.tv_sec == 5 && timeout.tv_usec == 300000, "usec carry folded into secs (got %ld.%06ld)", (long)timeout.tv_sec,
          (long)timeout.tv_usec);
}

int main(void)
{
    test_first_arming_arms_deadline();
    test_later_calls_do_not_move_deadline();
    test_unrelated_flag_bits_do_not_confuse_arm_state();
    test_usec_carry();

    printf("\n%s: done %d tests: %d OK %d FAILS\n", "at_pause_test", ok + faults, ok, faults);
    return faults ? 1 : 0;
}