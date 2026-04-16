/// test_timer_interval.c — Regression test for the original bug:
/// setTimeout(() => {}, N) should fire the event callback roughly once
/// at ~N ms, NOT every ~1s due to JSC's internal GC WTFTimer.
///
/// This test schedules a 3-second setTimeout, then monitors how many
/// times the event callback fires before the timer is due.  Before the
/// fix, the WTFTimer (1s GC interval) caused ~3 spurious callbacks.
/// After the fix, the callback should fire 0 times before ~3s, then
/// once when the timer is actually due.

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

typedef struct {
    atomic_int wake_count;
    int64_t first_wake_ms;
    int64_t start_ms;
} CallbackState;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void event_callback(void* userdata)
{
    CallbackState* st = (CallbackState*)userdata;
    int prev = atomic_fetch_add(&st->wake_count, 1);
    if (prev == 0) {
        st->first_wake_ms = now_ms();
    }
}

int main(void)
{
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

    CallbackState state = { .wake_count = 0, .first_wake_ms = 0, .start_ms = 0 };

    /* Register event callback */
    bun_set_event_callback(rt, event_callback, &state);

    /* Drain startup work and ACK */
    bun_run_pending_jobs(rt);

    /* Schedule a 3-second timer */
    state.start_ms = now_ms();
    static const char js_code[] = "globalThis.__timer_fired = false;"
                                  "setTimeout(() => { globalThis.__timer_fired = true; }, 3000);";
    BunValue r = bun_eval_string(ctx, js_code, sizeof(js_code) - 1);
    if (r == BUN_EXCEPTION) {
        printf("[FAIL] eval failed\n");
        bun_destroy(rt);
        return 1;
    }

    /* Drain once to register the timer, then wake the watcher thread so it
       recomputes its timeout with the new 3s timer in the heap.  The watcher
       may be blocked indefinitely (hint was -1 before the timer existed). */
    bun_run_pending_jobs(rt);
    bun_wakeup(rt);

    /* The bun_wakeup() itself is a genuine event that fires the callback
       once.  Wait for that initial callback, ACK it, then reset the counter
       so we can measure ONLY the interval behavior. */
    int64_t deadline0 = now_ms() + 2000;
    while (atomic_load(&state.wake_count) == 0 && now_ms() < deadline0) {
#ifndef _WIN32
        usleep(1000);
#else
        Sleep(1);
#endif
    }
    /* ACK so the watcher can proceed */
    bun_run_pending_jobs(rt);

    /* Reset counter — everything from here measures the actual timer wait */
    atomic_store(&state.wake_count, 0);
    state.first_wake_ms = 0;

    /* Small sleep to let the watcher thread loop back, recompute the hint
       (should be ~2.5s remaining), and enter its Futex/poll wait.  Without
       this, there's a race where the watcher fires a callback between ACK
       and the counter reset. */
#ifndef _WIN32
    usleep(100000); /* 100ms */
#else
    Sleep(100);
#endif
    /* Re-drain in case the watcher fired during the settle window */
    bun_run_pending_jobs(rt);
    atomic_store(&state.wake_count, 0);
    state.first_wake_ms = 0;

    /* Wait 2 seconds — well before the 3s timer fires.
       Before the fix, the callback would fire ~2 times (every ~1s).
       After the fix, 0 callbacks should fire because the watcher thread
       is sleeping on a Futex with the correct 3s timeout. */
#ifndef _WIN32
    usleep(2000000);
#else
    Sleep(2000);
#endif

    int wakes_before_due = atomic_load(&state.wake_count);

    if (wakes_before_due > 0) {
        printf("[FAIL] %d spurious callbacks in 2s before 3s timer due (expected 0)\n",
            wakes_before_due);
        bun_set_event_callback(rt, NULL, NULL);
        bun_destroy(rt);
        return 1;
    }

    /* Now wait for the timer to actually fire (another 2s, total ~4s) */
    int64_t deadline = now_ms() + 2000;
    while (atomic_load(&state.wake_count) == 0 && now_ms() < deadline) {
#ifndef _WIN32
        usleep(10000);
#else
        Sleep(10);
#endif
    }

    int wakes_after = atomic_load(&state.wake_count);

    /* ACK and drain the timer */
    bun_run_pending_jobs(rt);

    /* Verify the callback fired */
    if (wakes_after == 0) {
        printf("[FAIL] callback never fired after 3s timer was due\n");
        bun_set_event_callback(rt, NULL, NULL);
        bun_destroy(rt);
        return 1;
    }

    /* Verify latency: first wake should be ~3s after the original eval
       (state.start_ms), accounting for the time spent ACKing the initial
       wakeup callback. */
    int64_t latency = state.first_wake_ms - state.start_ms;
    if (latency < 2500 || latency > 5000) {
        printf("[FAIL] first callback at %lldms (expected ~3000ms)\n", (long long)latency);
        bun_set_event_callback(rt, NULL, NULL);
        bun_destroy(rt);
        return 1;
    }

    printf("[PASS] 0 spurious wakes before timer; first callback at %lldms; total wakes: %d\n",
        (long long)latency, wakes_after);

    bun_set_event_callback(rt, NULL, NULL);
    bun_destroy(rt);
    return 0;
}
