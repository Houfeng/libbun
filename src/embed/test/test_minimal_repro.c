/// test_minimal_repro.c — Minimal reproducer for the Uint8Array + eval_file crash
///
/// BUG: Creating a Uint8Array (or any TypedArray?) in a bun_eval_file module
///      corrupts the embed class constructor's internal pointer at offset +0x30,
///      causing a segfault when the embed class constructor is invoked.
///
/// The crash is in ___lldb_unnamed_symbol2176 (the embed class construct trampoline):
///   +104: ldr x20, [x24, #0x30]   ; load class def pointer from constructor
///   +108: cbz x20, ...            ; null check passes (x20 != 0)
///   +112: ldr x8, [x20, #0x38]   ; CRASH — x20 is a corrupted/NaN-boxed value

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include "../bun_embed.h"

#define BUN_LITERAL(s) (s), (sizeof(s) - 1)
#define BUN_CSTR(s) (s), strlen(s)

static BunValue construct(BunContext* ctx, BunClass* klass,
    int argc, const BunValue* argv, void* ud)
{
    (void)argc;
    (void)argv;
    (void)ud;
    int* data = calloc(1, sizeof(int));
    return bun_class_new(ctx, klass, data, (BunClassFinalizerFn)free, NULL);
}

static int write_tmp(const char* path, const char* src)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return 0;
    size_t len = strlen(src);
    ssize_t w = write(fd, src, len);
    close(fd);
    return (w == (ssize_t)len);
}

typedef int (*TestFn)(void);

/* Test 1: eval_file with new MyClass() — no Uint8Array → should pass */
static int test1_no_typedarray(void)
{
    BunRuntime* rt = bun_initialize(NULL);
    BunContext* ctx = bun_context(rt);
    BunClassDescriptor desc = { "MyClass", 7, NULL, 0, NULL, 0, construct, NULL, 0, NULL, 0, NULL, 0 };
    BunClass* k = bun_class_register(ctx, &desc, NULL);
    bun_set(ctx, bun_global(ctx), BUN_LITERAL("MyClass"), bun_class_constructor(ctx, k));
    char tmp[128];
    snprintf(tmp, 128, "/tmp/bun-repro-1-%ld.mjs", (long)getpid());
    write_tmp(tmp, "const x = new MyClass();\nconsole.log('ok');\n");
    BunValue r = bun_eval_file(ctx, BUN_CSTR(tmp));
    int ok = (r != BUN_EXCEPTION);
    unlink(tmp);
    bun_destroy(rt);
    return ok;
}

/* Test 2: eval_file with new Uint8Array + new MyClass → CRASH */
static int test2_uint8array_then_class(void)
{
    BunRuntime* rt = bun_initialize(NULL);
    BunContext* ctx = bun_context(rt);
    BunClassDescriptor desc = { "MyClass", 7, NULL, 0, NULL, 0, construct, NULL, 0, NULL, 0, NULL, 0 };
    BunClass* k = bun_class_register(ctx, &desc, NULL);
    bun_set(ctx, bun_global(ctx), BUN_LITERAL("MyClass"), bun_class_constructor(ctx, k));
    char tmp[128];
    snprintf(tmp, 128, "/tmp/bun-repro-2-%ld.mjs", (long)getpid());
    write_tmp(tmp, "const a = new Uint8Array([1]);\nconst x = new MyClass();\nconsole.log('ok');\n");
    BunValue r = bun_eval_file(ctx, BUN_CSTR(tmp));
    int ok = (r != BUN_EXCEPTION);
    unlink(tmp);
    bun_destroy(rt);
    return ok;
}

/* Test 3: eval_file with new MyClass(new Uint8Array) → CRASH */
static int test3_uint8array_as_arg(void)
{
    BunRuntime* rt = bun_initialize(NULL);
    BunContext* ctx = bun_context(rt);
    BunClassDescriptor desc = { "MyClass", 7, NULL, 0, NULL, 0, construct, NULL, 0, NULL, 0, NULL, 0 };
    BunClass* k = bun_class_register(ctx, &desc, NULL);
    bun_set(ctx, bun_global(ctx), BUN_LITERAL("MyClass"), bun_class_constructor(ctx, k));
    char tmp[128];
    snprintf(tmp, 128, "/tmp/bun-repro-3-%ld.mjs", (long)getpid());
    write_tmp(tmp, "const x = new MyClass(new Uint8Array([1]));\nconsole.log('ok');\n");
    BunValue r = bun_eval_file(ctx, BUN_CSTR(tmp));
    int ok = (r != BUN_EXCEPTION);
    unlink(tmp);
    bun_destroy(rt);
    return ok;
}

/* Test 4: eval_file with just Uint8Array alone (no class) → should be fine */
static int test4_just_uint8array(void)
{
    BunRuntime* rt = bun_initialize(NULL);
    BunContext* ctx = bun_context(rt);
    BunClassDescriptor desc = { "MyClass", 7, NULL, 0, NULL, 0, construct, NULL, 0, NULL, 0, NULL, 0 };
    BunClass* k = bun_class_register(ctx, &desc, NULL);
    bun_set(ctx, bun_global(ctx), BUN_LITERAL("MyClass"), bun_class_constructor(ctx, k));
    char tmp[128];
    snprintf(tmp, 128, "/tmp/bun-repro-4-%ld.mjs", (long)getpid());
    write_tmp(tmp, "const a = new Uint8Array([1]);\nconsole.log('ok', a.length);\n");
    BunValue r = bun_eval_file(ctx, BUN_CSTR(tmp));
    int ok = (r != BUN_EXCEPTION);
    unlink(tmp);
    bun_destroy(rt);
    return ok;
}

/* Test 5: eval_string (not eval_file) with Uint8Array + class → should pass */
static int test5_eval_string_uint8array(void)
{
    BunRuntime* rt = bun_initialize(NULL);
    BunContext* ctx = bun_context(rt);
    BunClassDescriptor desc = { "MyClass", 7, NULL, 0, NULL, 0, construct, NULL, 0, NULL, 0, NULL, 0 };
    BunClass* k = bun_class_register(ctx, &desc, NULL);
    bun_set(ctx, bun_global(ctx), BUN_LITERAL("MyClass"), bun_class_constructor(ctx, k));
    BunValue r = bun_eval_string(ctx, BUN_LITERAL("const a = new Uint8Array([1]); const x = new MyClass(); 'ok'"));
    int ok = (r != BUN_EXCEPTION);
    bun_destroy(rt);
    return ok;
}

/* Test 6: new ArrayBuffer in eval_file + class → crash? */
static int test6_arraybuffer(void)
{
    BunRuntime* rt = bun_initialize(NULL);
    BunContext* ctx = bun_context(rt);
    BunClassDescriptor desc = { "MyClass", 7, NULL, 0, NULL, 0, construct, NULL, 0, NULL, 0, NULL, 0 };
    BunClass* k = bun_class_register(ctx, &desc, NULL);
    bun_set(ctx, bun_global(ctx), BUN_LITERAL("MyClass"), bun_class_constructor(ctx, k));
    char tmp[128];
    snprintf(tmp, 128, "/tmp/bun-repro-6-%ld.mjs", (long)getpid());
    write_tmp(tmp, "const a = new ArrayBuffer(8);\nconst x = new MyClass();\nconsole.log('ok');\n");
    BunValue r = bun_eval_file(ctx, BUN_CSTR(tmp));
    int ok = (r != BUN_EXCEPTION);
    unlink(tmp);
    bun_destroy(rt);
    return ok;
}

/* Test 7: new Float32Array in eval_file + class → crash? */
static int test7_float32array(void)
{
    BunRuntime* rt = bun_initialize(NULL);
    BunContext* ctx = bun_context(rt);
    BunClassDescriptor desc = { "MyClass", 7, NULL, 0, NULL, 0, construct, NULL, 0, NULL, 0, NULL, 0 };
    BunClass* k = bun_class_register(ctx, &desc, NULL);
    bun_set(ctx, bun_global(ctx), BUN_LITERAL("MyClass"), bun_class_constructor(ctx, k));
    char tmp[128];
    snprintf(tmp, 128, "/tmp/bun-repro-7-%ld.mjs", (long)getpid());
    write_tmp(tmp, "const a = new Float32Array([1.0]);\nconst x = new MyClass();\nconsole.log('ok');\n");
    BunValue r = bun_eval_file(ctx, BUN_CSTR(tmp));
    int ok = (r != BUN_EXCEPTION);
    unlink(tmp);
    bun_destroy(rt);
    return ok;
}

/* Test 8: Buffer.from in eval_file + class → crash? */
static int test8_buffer(void)
{
    BunRuntime* rt = bun_initialize(NULL);
    BunContext* ctx = bun_context(rt);
    BunClassDescriptor desc = { "MyClass", 7, NULL, 0, NULL, 0, construct, NULL, 0, NULL, 0, NULL, 0 };
    BunClass* k = bun_class_register(ctx, &desc, NULL);
    bun_set(ctx, bun_global(ctx), BUN_LITERAL("MyClass"), bun_class_constructor(ctx, k));
    char tmp[128];
    snprintf(tmp, 128, "/tmp/bun-repro-8-%ld.mjs", (long)getpid());
    write_tmp(tmp, "const a = Buffer.from([1]);\nconst x = new MyClass();\nconsole.log('ok');\n");
    BunValue r = bun_eval_file(ctx, BUN_CSTR(tmp));
    int ok = (r != BUN_EXCEPTION);
    unlink(tmp);
    bun_destroy(rt);
    return ok;
}

/* Test 9: Uint8Array created AFTER class instance */
static int test9_class_before_uint8array(void)
{
    BunRuntime* rt = bun_initialize(NULL);
    BunContext* ctx = bun_context(rt);
    BunClassDescriptor desc = { "MyClass", 7, NULL, 0, NULL, 0, construct, NULL, 0, NULL, 0, NULL, 0 };
    BunClass* k = bun_class_register(ctx, &desc, NULL);
    bun_set(ctx, bun_global(ctx), BUN_LITERAL("MyClass"), bun_class_constructor(ctx, k));
    char tmp[128];
    snprintf(tmp, 128, "/tmp/bun-repro-9-%ld.mjs", (long)getpid());
    write_tmp(tmp, "const x = new MyClass();\nconst a = new Uint8Array([1]);\nconsole.log('ok');\n");
    BunValue r = bun_eval_file(ctx, BUN_CSTR(tmp));
    int ok = (r != BUN_EXCEPTION);
    unlink(tmp);
    bun_destroy(rt);
    return ok;
}

/* --- accessor map GC-cleanup tests --- */

static int g_getter_call_count = 0;

static BunValue accessor_getter(BunContext* ctx, BunValue this_val, void* ud)
{
    (void)ctx;
    (void)this_val;
    (void)ud;
    g_getter_call_count++;
    return bun_number(42.0);
}

/* Test 10: eval_string — accessor on short-lived object, GC, no stale callback */
static int test10_eval_string_accessor_gc(void)
{
    BunRuntime* rt = bun_initialize(NULL);
    BunContext* ctx = bun_context(rt);

    /* Create an object, attach an accessor, let it go out of scope, force GC.
       Then create a new object and verify the accessor is NOT called on it. */
    BunValue result = bun_eval_string(ctx,
        BUN_LITERAL(
            "let obj = {}; obj; // returned so C can attach accessor\n"));
    if (result == BUN_EXCEPTION) {
        bun_destroy(rt);
        return 0;
    }

    bun_define_accessor(ctx, result, "x", 1,
        accessor_getter, NULL, NULL,
        0, 0, 0);

    /* Let obj become unreachable and force GC */
    bun_eval_string(ctx, BUN_LITERAL("globalThis.__tmp = null; Bun.gc(true);"));

    /* New object at potentially the same address — accessor should NOT fire */
    int before = g_getter_call_count;
    bun_eval_string(ctx, BUN_LITERAL("let obj2 = {}; obj2.x;"));
    int after = g_getter_call_count;

    bun_destroy(rt);
    /* If stale accessor fires, after > before (bug). With fix, after == before. */
    return (after == before);
}

/* Test 11: eval_file — accessor on object created inside module, GC, no stale match */
static int test11_eval_file_accessor_gc(void)
{
    BunRuntime* rt = bun_initialize(NULL);
    BunContext* ctx = bun_context(rt);

    char tmp[128];
    snprintf(tmp, 128, "/tmp/bun-repro-11-%ld.mjs", (long)getpid());
    write_tmp(tmp,
        "const obj = {};\n"
        "globalThis.__exportedObj = obj;\n");

    BunValue r = bun_eval_file(ctx, BUN_CSTR(tmp));
    unlink(tmp);
    if (r == BUN_EXCEPTION) {
        bun_destroy(rt);
        return 0;
    }

    /* Attach accessor to globalThis.__exportedObj */
    BunValue obj = bun_get(ctx, bun_global(ctx), "__exportedObj", 13);
    if (obj == BUN_EXCEPTION || obj == BUN_UNDEFINED) {
        bun_destroy(rt);
        return 0;
    }

    bun_define_accessor(ctx, obj, "y", 1,
        accessor_getter, NULL, NULL,
        0, 0, 0);

    int before = g_getter_call_count;

    /* Clear the reference, let module scope go away, force GC */
    bun_eval_string(ctx, BUN_LITERAL("globalThis.__exportedObj = null; Bun.gc(true);"));

    /* New object from a fresh eval — should NOT trigger old accessor */
    bun_eval_string(ctx, BUN_LITERAL("let fresh = {}; fresh.y;"));

    int after = g_getter_call_count;

    bun_destroy(rt);
    return (after == before);
}

/* Test 12: accessor still works while object is alive (regression guard) */
static int test12_accessor_works_while_alive(void)
{
    BunRuntime* rt = bun_initialize(NULL);
    BunContext* ctx = bun_context(rt);

    BunValue obj = bun_eval_string(ctx, BUN_LITERAL("let o = {}; o;"));
    if (obj == BUN_EXCEPTION) {
        bun_destroy(rt);
        return 0;
    }

    bun_set(ctx, bun_global(ctx), "testObj12", 9, obj); /* keep alive */
    bun_define_accessor(ctx, obj, "val", 3,
        accessor_getter, NULL, NULL,
        0, 0, 0);

    int before = g_getter_call_count;
    BunValue got = bun_eval_string(ctx, BUN_LITERAL("testObj12.val;"));
    int after = g_getter_call_count;

    bun_destroy(rt);
    /* accessor must have fired exactly once */
    return (after == before + 1) && (got != BUN_EXCEPTION);
}

struct {
    const char* name;
    TestFn fn;
} tests[] = {
    { " 1: eval_file, new MyClass() only", test1_no_typedarray },
    { " 2: eval_file, Uint8Array THEN MyClass", test2_uint8array_then_class },
    { " 3: eval_file, MyClass(Uint8Array)", test3_uint8array_as_arg },
    { " 4: eval_file, Uint8Array only (no class use)", test4_just_uint8array },
    { " 5: eval_string, Uint8Array + MyClass", test5_eval_string_uint8array },
    { " 6: eval_file, ArrayBuffer + MyClass", test6_arraybuffer },
    { " 7: eval_file, Float32Array + MyClass", test7_float32array },
    { " 8: eval_file, Buffer.from + MyClass", test8_buffer },
    { " 9: eval_file, MyClass THEN Uint8Array", test9_class_before_uint8array },
    { "10: eval_string, accessor map GC cleanup", test10_eval_string_accessor_gc },
    { "11: eval_file, accessor map GC cleanup", test11_eval_file_accessor_gc },
    { "12: accessor works while object alive", test12_accessor_works_while_alive },
};
static const int NUM_TESTS = sizeof(tests) / sizeof(tests[0]);

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("=== Minimal Reproducer: Uint8Array + eval_file + embed class ===\n\n");
    int pass = 0, fail = 0;
    for (int i = 0; i < NUM_TESTS; i++) {
        printf("--- Test%s ---\n", tests[i].name);
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            _exit(tests[i].fn() ? 0 : 1);
        } else if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                printf("[PASS]\n\n");
                pass++;
            } else if (WIFSIGNALED(status)) {
                printf("[CRASH] signal %d\n\n", WTERMSIG(status));
                fail++;
            } else {
                printf("[FAIL] exit %d\n\n", WEXITSTATUS(status));
                fail++;
            }
        } else {
            perror("fork");
            fail++;
        }
    }
    printf("=== Results: %d/%d passed, %d failed ===\n", pass, pass + fail, fail);
    return fail > 0 ? 1 : 0;
}
