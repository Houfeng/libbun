/// test_eval_nudge.c — Verify that bun_eval_string nudges the watcher thread
/// so that a timer registered via eval fires even if the host does NOT call
/// bun_run_pending_jobs() between the eval and the expected callback.
///
/// Sequence under test:
///   1. bun_run_pending_jobs() — drain startup (watcher hint becomes -1)
///   2. bun_eval_string("setTimeout(fn, 500)") — registers 500ms timer
///   3. Host does NOT call bun_run_pending_jobs; just waits for callback
///   4. Callback should fire at ~500ms thanks to nudgeWatcher()
///   5. Host calls bun_run_pending_jobs() to drain the timer
///
/// Before the fix, the watcher would stay in Futex.wait(infinite) forever
/// because nobody woke it after eval changed the timer heap.

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <time.h>
#include "../bun_embed.h"

#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#endif

static atomic_int callback_count = 0;
static int64_t callback_time_ms = 0;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void event_callback(void* userdata)
{
    (void)userdata;
    int prev = atomic_fetch_add(&callback_count, 1);
    if (prev == 0) {
        callback_time_ms = now_ms();
    }
}

int main(void)
{
    printf("--- test_eval_nudge ---\n");

    BunRuntime* rt = bun_initialize(NULL);
    if (!rt) {
        printf("[FAIL] bun_initialize failed\n");
        return 1;
    }

    BunContext* ctx = bun_context(rt);
    if (!ctx) {
        printf("[FAIL] bun_context failed\n");
        bun_destroy(rt);
        return 1;
    }

    bun_set_event_callback(rt, event_callback, NULL);

    /* Drain startup work + ACK so the watcher settles to hint=-1 (idle). */
    bun_run_pending_jobs(rt);

    /* Let the watcher thread settle into its idle Futex.wait(infinite). */
#ifndef _WIN32
    usleep(100000); /* 100ms */
#else
    Sleep(100);
#endif
    /* Drain any startup callback and reset counter. */
    bun_run_pending_jobs(rt);
    atomic_store(&callback_count, 0);

    /* ---- Critical section: eval WITHOUT subsequent bun_run_pending_jobs ---- */
    int64_t t0 = now_ms();

    static const char js_code[] = "globalThis.__nudge_timer_fired = false;"
                                  "setTimeout(() => { globalThis.__nudge_timer_fired = true; }, 500);";

    BunValue r = bun_eval_string(ctx, js_code, sizeof(js_code) - 1);
    if (r == BUN_EXCEPTION) {
        printf("[FAIL] eval failed\n");
        bun_destroy(rt);
        return 1;
    }

    /* Do NOT call bun_run_pending_jobs() here — the whole point is that
       nudgeWatcher() inside bun_eval_string wakes the watcher thread so
       it recomputes its timeout and fires the callback at ~500ms. */

    /* Wait up to 2 seconds for the callback. */
    int64_t deadline = now_ms() + 2000;
    while (atomic_load(&callback_count) == 0 && now_ms() < deadline) {
#ifndef _WIN32
        usleep(5000); /* 5ms */
#else
        Sleep(5);
#endif
    }

    int wakes = atomic_load(&callback_count);

    if (wakes == 0) {
        printf("[FAIL] callback never fired (watcher not nudged after eval)\n");
        bun_set_event_callback(rt, NULL, NULL);
        bun_destroy(rt);
        return 1;
    }

    int64_t latency = callback_time_ms - t0;

    /* Sanity check: callback should fire between 400ms and 1500ms. */
    if (latency < 400 || latency > 1500) {
        printf("[FAIL] callback at %lldms, expected ~500ms\n", (long long)latency);
        bun_set_event_callback(rt, NULL, NULL);
        bun_destroy(rt);
        return 1;
    }

    /* Now drain the timer so it actually executes the JS callback. */
    bun_run_pending_jobs(rt);

    printf("[PASS] eval nudge: callback at %lldms (expected ~500ms), %d wake(s)\n",
        (long long)latency, wakes);

    bun_set_event_callback(rt, NULL, NULL);
    bun_destroy(rt);
    return 0;
}
