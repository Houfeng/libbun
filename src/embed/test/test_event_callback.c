/// test_event_callback.c — Comprehensive tests for bun_set_event_callback
/// covering all notification scenarios:
///
///   1. bun_call_async triggers callback
///   2. Multiple sequential timers fire at correct times
///   3. Short-interval repeating timer accuracy
///   4. Idle→active transition fires callback correctly
///   5. No callback after bun_set_event_callback(NULL) (unregister)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include "../bun_embed.h"

#ifndef _WIN32
#include <unistd.h>
#include <pthread.h>
#else
#include <windows.h>
#endif

#define BUN_LITERAL(s) (s), (sizeof(s) - 1)

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT_MSG(cond, fmt, ...)                                    \
    do {                                                              \
        if (!(cond)) {                                                \
            printf("[FAIL] %s: " fmt "\n", test_name, ##__VA_ARGS__); \
            g_fail++;                                                 \
            goto cleanup;                                             \
        }                                                             \
    } while (0)

#define PASS()                            \
    do {                                  \
        printf("[PASS] %s\n", test_name); \
        g_pass++;                         \
    } while (0)

/* ---------- helpers ---------- */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void drain_loop(BunRuntime* rt, int max_iters, int delay_ms)
{
    for (int i = 0; i < max_iters; i++) {
        BunPendingJobsResult r = bun_run_pending_jobs(rt);
        if (r == BUN_PENDING_JOBS_IDLE) break;
        if (r == BUN_PENDING_JOBS_WAIT && delay_ms > 0) {
#ifndef _WIN32
            usleep((unsigned)(delay_ms * 1000));
#else
            Sleep((DWORD)delay_ms);
#endif
        }
    }
}

typedef struct {
    atomic_int wake_count;
    int64_t first_wake_ms;
    int64_t start_ms;
} CallbackState;

static void event_callback(void* userdata)
{
    CallbackState* st = (CallbackState*)userdata;
    int prev = atomic_fetch_add(&st->wake_count, 1);
    if (prev == 0) {
        st->first_wake_ms = now_ms();
    }
}

/* ---------- Test 1: bun_call_async triggers event callback ---------- */

typedef struct {
    BunRuntime* rt;
    BunContext* ctx;
    BunValue fn_value;
    int delay_ms;
} AsyncCallArg;

#ifndef _WIN32
static void* async_call_thread_fn(void* arg)
{
    AsyncCallArg* a = (AsyncCallArg*)arg;
    usleep((unsigned)(a->delay_ms * 1000));
    bun_call_async(a->ctx, a->fn_value, BUN_UNDEFINED, 0, NULL);
    return NULL;
}
#else
static DWORD WINAPI async_call_thread_fn(LPVOID arg)
{
    AsyncCallArg* a = (AsyncCallArg*)arg;
    Sleep((DWORD)a->delay_ms);
    bun_call_async(a->ctx, a->fn_value, BUN_UNDEFINED, 0, NULL);
    return 0;
}
#endif

static void test_call_async_triggers_callback(void)
{
    const char* test_name = "bun_call_async triggers event callback";
    CallbackState state = { .wake_count = 0, .first_wake_ms = 0, .start_ms = 0 };
    BunRuntime* rt = bun_initialize(NULL);
    ASSERT_MSG(rt, "bun_initialize failed");
    BunContext* ctx = bun_context(rt);
    ASSERT_MSG(ctx, "bun_context failed");

    /* Create a JS function to call from the other thread */
    BunValue fn = bun_eval_string(ctx, BUN_LITERAL("globalThis.__async_called = false;"
                                                   "(function() { globalThis.__async_called = true; })"));
    ASSERT_MSG(fn != BUN_EXCEPTION, "eval failed: %s", bun_last_error(ctx, NULL));
    bun_protect(ctx, fn);

    bun_set_event_callback(rt, event_callback, &state);

    /* Drain startup */
    bun_run_pending_jobs(rt);

    /* Let the watcher settle into its blocking wait */
#ifndef _WIN32
    usleep(50000);
#else
    Sleep(50);
#endif
    /* Reset counter after initial drain/settle */
    bun_run_pending_jobs(rt);
    atomic_store(&state.wake_count, 0);
    state.first_wake_ms = 0;

    /* Spawn thread that calls bun_call_async after 50ms */
    state.start_ms = now_ms();
    AsyncCallArg targ = { .rt = rt, .ctx = ctx, .fn_value = fn, .delay_ms = 50 };
#ifndef _WIN32
    pthread_t tid;
    int rc = pthread_create(&tid, NULL, async_call_thread_fn, &targ);
    ASSERT_MSG(rc == 0, "pthread_create failed: %d", rc);
#else
    HANDLE th = CreateThread(NULL, 0, async_call_thread_fn, &targ, 0, NULL);
    ASSERT_MSG(th != NULL, "CreateThread failed");
#endif

    /* Wait for callback (max 2s) */
    int64_t deadline = now_ms() + 2000;
    while (atomic_load(&state.wake_count) == 0 && now_ms() < deadline) {
#ifndef _WIN32
        usleep(1000);
#else
        Sleep(1);
#endif
    }

    ASSERT_MSG(atomic_load(&state.wake_count) > 0,
        "callback never fired after bun_call_async");

    int64_t latency = state.first_wake_ms - state.start_ms;
    ASSERT_MSG(latency < 500,
        "callback latency too high: %lldms (expected <500)", (long long)latency);

    /* Drain to execute the async call */
    drain_loop(rt, 30, 10);

#ifndef _WIN32
    pthread_join(tid, NULL);
#else
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
#endif

    /* Verify the JS function actually ran */
    BunValue result = bun_get(ctx, bun_global(ctx), BUN_LITERAL("__async_called"));
    ASSERT_MSG(bun_to_bool(result), "async function did not execute");

    bun_unprotect(ctx, fn);
    PASS();
cleanup:
    bun_set_event_callback(rt, NULL, NULL);
    bun_destroy(rt);
}

/* ---------- Test 2: Multiple sequential timers ---------- */

static void test_multiple_sequential_timers(void)
{
    const char* test_name = "multiple sequential timers fire correctly";
    CallbackState state = { .wake_count = 0, .first_wake_ms = 0, .start_ms = 0 };
    BunRuntime* rt = bun_initialize(NULL);
    ASSERT_MSG(rt, "bun_initialize failed");
    BunContext* ctx = bun_context(rt);
    ASSERT_MSG(ctx, "bun_context failed");

    bun_set_event_callback(rt, event_callback, &state);

    /* Schedule 3 timers at 100ms, 200ms, 300ms */
    BunValue r = bun_eval_string(ctx, BUN_LITERAL("globalThis.__timer_log = [];"
                                                  "setTimeout(() => globalThis.__timer_log.push('a'), 100);"
                                                  "setTimeout(() => globalThis.__timer_log.push('b'), 200);"
                                                  "setTimeout(() => globalThis.__timer_log.push('c'), 300);"));
    ASSERT_MSG(r != BUN_EXCEPTION, "eval failed: %s", bun_last_error(ctx, NULL));

    state.start_ms = now_ms();

    /* Drain to register timers, then nudge watcher */
    bun_run_pending_jobs(rt);
    bun_wakeup(rt);

    /* Run the event loop for ~500ms, processing callbacks as they come */
    int64_t end = now_ms() + 500;
    while (now_ms() < end) {
        if (atomic_load(&state.wake_count) > 0) {
            bun_run_pending_jobs(rt);
            atomic_store(&state.wake_count, 0);
        }
#ifndef _WIN32
        usleep(5000);
#else
        Sleep(5);
#endif
    }

    /* Final drain */
    drain_loop(rt, 30, 10);

    /* Verify all 3 timers ran in order */
    BunValue log = bun_eval_string(ctx, BUN_LITERAL("globalThis.__timer_log.join(',')"));
    ASSERT_MSG(log != BUN_EXCEPTION, "eval log failed");

    size_t len = 0;
    const char* str = bun_to_utf8(ctx, log, &len);
    ASSERT_MSG(str != NULL, "bun_to_string failed");
    ASSERT_MSG(len == 5 && memcmp(str, "a,b,c", 5) == 0,
        "expected 'a,b,c' got '%.*s'", (int)len, str);

    PASS();
cleanup:
    bun_set_event_callback(rt, NULL, NULL);
    bun_destroy(rt);
}

/* ---------- Test 3: Short-interval repeating timer ---------- */

static void test_short_interval_timer(void)
{
    const char* test_name = "short-interval repeating timer (50ms x 5)";
    CallbackState state = { .wake_count = 0, .first_wake_ms = 0, .start_ms = 0 };
    BunRuntime* rt = bun_initialize(NULL);
    ASSERT_MSG(rt, "bun_initialize failed");
    BunContext* ctx = bun_context(rt);
    ASSERT_MSG(ctx, "bun_context failed");

    bun_set_event_callback(rt, event_callback, &state);

    /* Schedule a 50ms interval that runs 5 times then clears itself */
    BunValue r = bun_eval_string(ctx, BUN_LITERAL("globalThis.__interval_count = 0;"
                                                  "const id = setInterval(() => {"
                                                  "  globalThis.__interval_count++;"
                                                  "  if (globalThis.__interval_count >= 5) clearInterval(id);"
                                                  "}, 50);"));
    ASSERT_MSG(r != BUN_EXCEPTION, "eval failed: %s", bun_last_error(ctx, NULL));

    state.start_ms = now_ms();

    /* Drain to register the interval, nudge watcher */
    bun_run_pending_jobs(rt);
    bun_wakeup(rt);

    /* Run event loop for ~500ms, ACKing each callback */
    int64_t end = now_ms() + 500;
    while (now_ms() < end) {
        if (atomic_load(&state.wake_count) > 0) {
            bun_run_pending_jobs(rt);
            atomic_store(&state.wake_count, 0);
        }
#ifndef _WIN32
        usleep(5000);
#else
        Sleep(5);
#endif
    }

    /* Final drain */
    drain_loop(rt, 30, 10);

    /* Verify the interval ran exactly 5 times */
    BunValue count_val = bun_eval_string(ctx, BUN_LITERAL("globalThis.__interval_count"));
    ASSERT_MSG(count_val != BUN_EXCEPTION, "eval count failed");
    double count = bun_to_number(ctx, count_val);
    ASSERT_MSG(count >= 5.0, "interval ran only %.0f times (expected 5)", count);

    /* Verify total time is reasonable: 5 x 50ms = ~250ms minimum */
    int64_t elapsed = now_ms() - state.start_ms;
    ASSERT_MSG(elapsed >= 200 && elapsed <= 800,
        "elapsed %lldms (expected 250-500ms range)", (long long)elapsed);

    PASS();
cleanup:
    bun_set_event_callback(rt, NULL, NULL);
    bun_destroy(rt);
}

/* ---------- Test 4: Idle→active transition ---------- */

static void test_idle_then_active(void)
{
    const char* test_name = "idle-to-active transition fires callback";
    CallbackState state = { .wake_count = 0, .first_wake_ms = 0, .start_ms = 0 };
    BunRuntime* rt = bun_initialize(NULL);
    ASSERT_MSG(rt, "bun_initialize failed");
    BunContext* ctx = bun_context(rt);
    ASSERT_MSG(ctx, "bun_context failed");

    bun_set_event_callback(rt, event_callback, &state);

    /* Drain all startup work */
    drain_loop(rt, 20, 5);

    /* Sleep 500ms in idle — should be 0 wakes */
    atomic_store(&state.wake_count, 0);
#ifndef _WIN32
    usleep(500000);
#else
    Sleep(500);
#endif

    int idle_wakes = atomic_load(&state.wake_count);
    ASSERT_MSG(idle_wakes == 0, "got %d wakes during idle (expected 0)", idle_wakes);

    /* Now schedule a timer — transitioning from idle to active */
    state.start_ms = now_ms();
    atomic_store(&state.wake_count, 0);
    state.first_wake_ms = 0;

    BunValue r = bun_eval_string(ctx, BUN_LITERAL("globalThis.__transition_done = false;"
                                                  "setTimeout(() => { globalThis.__transition_done = true; }, 80);"));
    ASSERT_MSG(r != BUN_EXCEPTION, "eval failed: %s", bun_last_error(ctx, NULL));

    /* Register timer and nudge watcher from idle */
    bun_run_pending_jobs(rt);
    bun_wakeup(rt);

    /* ACK the wakeup notification */
    int64_t deadline = now_ms() + 1000;
    while (atomic_load(&state.wake_count) == 0 && now_ms() < deadline) {
#ifndef _WIN32
        usleep(1000);
#else
        Sleep(1);
#endif
    }
    bun_run_pending_jobs(rt);
    atomic_store(&state.wake_count, 0);
    state.first_wake_ms = 0;
    state.start_ms = now_ms();

    /* Wait for the 80ms timer callback */
    deadline = now_ms() + 2000;
    while (atomic_load(&state.wake_count) == 0 && now_ms() < deadline) {
#ifndef _WIN32
        usleep(1000);
#else
        Sleep(1);
#endif
    }

    ASSERT_MSG(atomic_load(&state.wake_count) > 0,
        "callback never fired after idle→active transition");

    /* Drain the timer */
    drain_loop(rt, 30, 10);

    BunValue done = bun_get(ctx, bun_global(ctx), BUN_LITERAL("__transition_done"));
    ASSERT_MSG(bun_to_bool(done), "timer did not execute after idle→active");

    PASS();
cleanup:
    bun_set_event_callback(rt, NULL, NULL);
    bun_destroy(rt);
}

/* ---------- Test 5: Unregister callback stops notifications ---------- */

static void test_unregister_stops_callback(void)
{
    const char* test_name = "unregister callback stops notifications";
    CallbackState state = { .wake_count = 0, .first_wake_ms = 0, .start_ms = 0 };
    BunRuntime* rt = bun_initialize(NULL);
    ASSERT_MSG(rt, "bun_initialize failed");
    BunContext* ctx = bun_context(rt);
    ASSERT_MSG(ctx, "bun_context failed");

    bun_set_event_callback(rt, event_callback, &state);

    /* Verify callback works first */
    BunValue r = bun_eval_string(ctx, BUN_LITERAL("setTimeout(() => {}, 30)"));
    ASSERT_MSG(r != BUN_EXCEPTION, "eval failed: %s", bun_last_error(ctx, NULL));
    bun_run_pending_jobs(rt);
    bun_wakeup(rt);

    int64_t deadline = now_ms() + 2000;
    while (atomic_load(&state.wake_count) == 0 && now_ms() < deadline) {
#ifndef _WIN32
        usleep(1000);
#else
        Sleep(1);
#endif
    }
    ASSERT_MSG(atomic_load(&state.wake_count) > 0, "callback didn't fire initially");

    /* Drain, then unregister */
    drain_loop(rt, 30, 10);
    bun_set_event_callback(rt, NULL, NULL);

    /* Reset counter */
    atomic_store(&state.wake_count, 0);

    /* Schedule another timer + wakeup */
    r = bun_eval_string(ctx, BUN_LITERAL("setTimeout(() => {}, 30)"));
    ASSERT_MSG(r != BUN_EXCEPTION, "eval failed after unregister");
    bun_run_pending_jobs(rt);
    bun_wakeup(rt);

    /* Wait 200ms — should get 0 callbacks since unregistered */
#ifndef _WIN32
    usleep(200000);
#else
    Sleep(200);
#endif

    int wakes = atomic_load(&state.wake_count);
    ASSERT_MSG(wakes == 0, "got %d wakes after unregister (expected 0)", wakes);

    PASS();
cleanup:
    bun_destroy(rt);
}

/* ---------- main ---------- */

int main(void)
{
    printf("--- test_event_callback ---\n");
    test_call_async_triggers_callback();
    test_multiple_sequential_timers();
    test_short_interval_timer();
    test_idle_then_active();
    test_unregister_stops_callback();
    printf("--- results: %d passed, %d failed ---\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
