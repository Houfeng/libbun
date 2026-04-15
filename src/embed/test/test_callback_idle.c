/// test_callback_idle.c — Verify that bun_set_event_callback does NOT fire
/// continuously when no JS work is scheduled (idle state).
///
/// This is a regression test for a bug where the GC repeating timer's
/// EVFILT_TIMER made the kqueue fd permanently readable, causing the
/// watcher thread to fire the callback in a tight loop.

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

static atomic_int g_wake_count = 0;

static void idle_callback(void* userdata)
{
    (void)userdata;
    atomic_fetch_add(&g_wake_count, 1);
}

int main(void)
{
    BunRuntime* rt = bun_initialize(NULL);
    if (!rt) {
        printf("[FAIL] bun_initialize failed\n");
        return 1;
    }

    /* Drain any startup work */
    bun_run_pending_jobs(rt);

    /* Register the event callback */
    bun_set_event_callback(rt, idle_callback, NULL);

    /* ACK the first notification (if any) so the watcher thread can proceed */
    bun_run_pending_jobs(rt);

    /* Reset counter after initial drain */
    atomic_store(&g_wake_count, 0);

    /* Sleep 1.5s — long enough for the GC timer (1s) to fire at least once.
       Before the fix, the callback would fire hundreds/thousands of times
       here.  After the fix it should fire 0 times (no real work). */
#ifndef _WIN32
    usleep(1500000);
#else
    Sleep(1500);
#endif

    int wakes = atomic_load(&g_wake_count);

    /* Clean up */
    bun_set_event_callback(rt, NULL, NULL);
    bun_destroy(rt);

    if (wakes == 0) {
        printf("[PASS] idle callback: 0 spurious wakes in 1.5s\n");
        return 0;
    } else {
        printf("[FAIL] idle callback: %d spurious wakes in 1.5s (expected 0)\n", wakes);
        return 1;
    }
}
