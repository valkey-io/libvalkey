/*
 * Unit tests for src/timer.c
 */

#define _DEFAULT_SOURCE /* for usleep() */

#include "timer.h"

#include <assert.h>
#include <stdio.h>
#ifdef _MSC_VER
#include <windows.h>
#define usleep(us) Sleep((us) / 1000)
#else
#include <unistd.h>
#endif

static int fired_count;
static void *fired_data;
static int reschedule_count;
static struct timeval reschedule_tv;

static void test_reschedule_cb(void *privdata, struct timeval tv) {
    (void)privdata;
    reschedule_count++;
    reschedule_tv = tv;
}

static void test_cb(void *privdata) {
    fired_count++;
    fired_data = privdata;
}

static void test_add_and_fire(void) {
    printf("  test_add_and_fire: ");
    valkeyTimerList list;
    valkeyTimerListInit(&list);

    fired_count = 0;
    fired_data = NULL;
    int data = 42;
    struct timeval iv = {.tv_sec = 0, .tv_usec = 10000}; /* 10ms */
    valkeyTimer *t = valkeyTimerAdd(&list, iv, test_cb, &data);
    assert(t != NULL);
    assert(list.head == t);

    /* Not yet expired. */
    valkeyProcessTimers(&list, NULL, NULL);
    assert(fired_count == 0);

    /* Wait for timer to expire. */
    usleep(15000); /* 15ms */
    valkeyProcessTimers(&list, NULL, NULL);
    assert(fired_count == 1);
    assert(fired_data == &data);

    /* Timer is one-shot, list should be empty. */
    assert(list.head == NULL);

    valkeyTimerListFree(&list);
    printf("PASSED\n");
}

static void test_ordering(void) {
    printf("  test_ordering: ");
    valkeyTimerList list;
    valkeyTimerListInit(&list);

    struct timeval iv1 = {.tv_sec = 0, .tv_usec = 50000}; /* 50ms */
    struct timeval iv2 = {.tv_sec = 0, .tv_usec = 10000}; /* 10ms */
    struct timeval iv3 = {.tv_sec = 0, .tv_usec = 30000}; /* 30ms */

    valkeyTimer *t1 = valkeyTimerAdd(&list, iv1, test_cb, NULL);
    valkeyTimer *t2 = valkeyTimerAdd(&list, iv2, test_cb, NULL);
    valkeyTimer *t3 = valkeyTimerAdd(&list, iv3, test_cb, NULL);

    /* Should be ordered: t2 (10ms) -> t3 (30ms) -> t1 (50ms) */
    assert(list.head == t2);
    assert(t2->next == t3);
    assert(t3->next == t1);
    assert(t1->next == NULL);

    valkeyTimerListFree(&list);
    printf("PASSED\n");
}

static void test_cancel(void) {
    printf("  test_cancel: ");
    valkeyTimerList list;
    valkeyTimerListInit(&list);

    fired_count = 0;
    struct timeval iv = {.tv_sec = 0, .tv_usec = 10000};
    valkeyTimer *t = valkeyTimerAdd(&list, iv, test_cb, NULL);
    assert(list.head == t);

    valkeyTimerDel(&list, t);
    assert(list.head == NULL);

    usleep(15000);
    reschedule_count = 0;
    valkeyProcessTimers(&list, test_reschedule_cb, NULL);
    assert(reschedule_count == 0); /* No timers */
    assert(fired_count == 0);

    printf("PASSED\n");
}

static void test_next_deadline(void) {
    printf("  test_next_deadline: ");
    valkeyTimerList list;
    valkeyTimerListInit(&list);

    struct timeval iv = {.tv_sec = 1, .tv_usec = 0}; /* 1s */
    valkeyTimerAdd(&list, iv, test_cb, NULL);

    /* Nothing has expired, so the pending deadline is reported as is. */
    reschedule_count = 0;
    valkeyProcessTimers(&list, test_reschedule_cb, NULL);
    assert(reschedule_count == 1);
    long remaining_us = reschedule_tv.tv_sec * 1000000 + reschedule_tv.tv_usec;
    assert(remaining_us > 900000 && remaining_us <= 1000000);

    valkeyTimerListFree(&list);
    printf("PASSED\n");
}

/* A timer firing while another is queued must report the next deadline,
 * otherwise an event-driven adapter never wakes up for it. */
static void test_reschedule_when_more_pending(void) {
    printf("  test_reschedule_when_more_pending: ");
    valkeyTimerList list;
    valkeyTimerListInit(&list);

    fired_count = 0;
    reschedule_count = 0;
    struct timeval expired = {.tv_sec = 0, .tv_usec = 0};
    struct timeval pending = {.tv_sec = 0, .tv_usec = 200000}; /* 200ms */
    valkeyTimerAdd(&list, expired, test_cb, NULL);
    valkeyTimer *t2 = valkeyTimerAdd(&list, pending, test_cb, NULL);

    valkeyProcessTimers(&list, test_reschedule_cb, NULL);

    assert(fired_count == 1);

    /* The queued timer was reported once, with time still remaining. */
    assert(reschedule_count == 1);
    long remaining_us = reschedule_tv.tv_sec * 1000000 + reschedule_tv.tv_usec;
    assert(remaining_us > 100000 && remaining_us <= 200000);
    assert(list.head == t2);

    valkeyTimerListFree(&list);
    printf("PASSED\n");
}

/* The last timer firing leaves nothing to wake for. */
static void test_no_reschedule_when_last(void) {
    printf("  test_no_reschedule_when_last: ");
    valkeyTimerList list;
    valkeyTimerListInit(&list);

    fired_count = 0;
    reschedule_count = 0;
    struct timeval expired = {.tv_sec = 0, .tv_usec = 0};
    valkeyTimerAdd(&list, expired, test_cb, NULL);

    valkeyProcessTimers(&list, test_reschedule_cb, NULL);

    assert(fired_count == 1);
    assert(reschedule_count == 0);
    assert(list.head == NULL);

    valkeyTimerListFree(&list);
    printf("PASSED\n");
}

/* An already due timer must still get a non-zero delay: adapters take an
 * all-zero timeval as a request to cancel the timer. */
static void test_reschedule_when_already_due(void) {
    printf("  test_reschedule_when_already_due: ");
    valkeyTimerList list;
    valkeyTimerListInit(&list);

    fired_count = 0;
    reschedule_count = 0;
    struct timeval expired = {.tv_sec = 0, .tv_usec = 0};
    valkeyTimerAdd(&list, expired, test_cb, NULL);
    valkeyTimerAdd(&list, expired, test_cb, NULL);
    usleep(2000); /* Let both deadlines pass. */

    valkeyProcessTimers(&list, test_reschedule_cb, NULL);

    assert(fired_count == 1);
    assert(reschedule_count == 1);
    assert(reschedule_tv.tv_sec > 0 || reschedule_tv.tv_usec > 0);

    valkeyTimerListFree(&list);
    printf("PASSED\n");
}

int main(void) {
    printf("Testing timer module:\n");
    test_add_and_fire();
    test_ordering();
    test_cancel();
    test_next_deadline();
    test_reschedule_when_more_pending();
    test_no_reschedule_when_last();
    test_reschedule_when_already_due();
    printf("All timer tests passed.\n");
    return 0;
}
