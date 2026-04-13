/// test_throw.c — Regression tests for bun_throw() / bun_error() across embed callbacks.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../bun_embed.h"

#define BUN_LITERAL(str) (str), sizeof(str) - 1

#define PASS(msg) (printf("[PASS] %s\n", (msg)), passed++)
#define FAIL(msg, ...) (fprintf(stderr, "[FAIL] " msg "\n", ##__VA_ARGS__), failed++)

typedef struct {
    int tag;
} ThrowyState;

static int passed = 0;
static int failed = 0;

static BunValue native_throw_value(BunContext* ctx, int argc, const BunValue* argv, void* userdata)
{
    (void)userdata;
    if (argc >= 1 && argv) return bun_throw(ctx, argv[0]);
    return bun_throw(ctx, bun_error(ctx, BUN_LITERAL("native value boom")));
}

static BunValue native_throw_error(BunContext* ctx, int argc, const BunValue* argv, void* userdata)
{
    (void)argc;
    (void)argv;
    (void)userdata;
    return bun_throw(ctx, bun_error(ctx, BUN_LITERAL("native error")));
}

static BunValue accessor_throw_get(BunContext* ctx, BunValue this_value, void* userdata)
{
    (void)this_value;
    (void)userdata;
    return bun_throw(ctx, bun_error(ctx, BUN_LITERAL("accessor getter boom")));
}

static void accessor_throw_set(BunContext* ctx, BunValue this_value, BunValue value, void* userdata)
{
    (void)this_value;
    (void)value;
    (void)userdata;
    bun_throw(ctx, bun_error(ctx, BUN_LITERAL("accessor setter boom")));
}

static void throwy_finalize(void* native_ptr, void* userdata)
{
    (void)userdata;
    free(native_ptr);
}

static BunValue throwy_construct(BunContext* ctx, BunClass* klass, int argc, const BunValue* argv, void* userdata)
{
    (void)userdata;
    if (argc >= 1 && argv && bun_to_bool(argv[0])) {
        return bun_throw(ctx, bun_error(ctx, BUN_LITERAL("ctor boom")));
    }

    ThrowyState* state = (ThrowyState*)calloc(1, sizeof(*state));
    if (!state) return BUN_UNDEFINED;
    state->tag = 42;

    BunValue instance = bun_class_new(ctx, klass, state, throwy_finalize, NULL);
    if (instance == BUN_UNDEFINED) free(state);
    return instance;
}

static BunValue throwy_method(BunContext* ctx, BunValue this_value, void* native_ptr, int argc, const BunValue* argv, void* userdata)
{
    (void)this_value;
    (void)native_ptr;
    (void)argc;
    (void)argv;
    (void)userdata;
    return bun_throw(ctx, bun_error(ctx, BUN_LITERAL("method boom")));
}

static BunValue throwy_get(BunContext* ctx, BunValue this_value, void* native_ptr, void* userdata)
{
    (void)this_value;
    (void)native_ptr;
    (void)userdata;
    return bun_throw(ctx, bun_error(ctx, BUN_LITERAL("getter boom")));
}

static void throwy_set(BunContext* ctx, BunValue this_value, void* native_ptr, BunValue value, void* userdata)
{
    (void)this_value;
    (void)native_ptr;
    (void)value;
    (void)userdata;
    bun_throw(ctx, bun_error(ctx, BUN_LITERAL("setter boom")));
}

static BunValue throwy_static_method(BunContext* ctx, BunValue this_value, void* userdata, int argc, const BunValue* argv)
{
    (void)this_value;
    (void)userdata;
    (void)argc;
    (void)argv;
    return bun_throw(ctx, bun_error(ctx, BUN_LITERAL("static method boom")));
}

static BunValue throwy_static_get(BunContext* ctx, BunValue this_value, void* userdata)
{
    (void)this_value;
    (void)userdata;
    return bun_throw(ctx, bun_error(ctx, BUN_LITERAL("static getter boom")));
}

static void throwy_static_set(BunContext* ctx, BunValue this_value, BunValue value, void* userdata)
{
    (void)this_value;
    (void)value;
    (void)userdata;
    bun_throw(ctx, bun_error(ctx, BUN_LITERAL("static setter boom")));
}

static int check_js_true(BunContext* ctx, const char* label, const char* code)
{
    BunValue result = bun_eval_string(ctx, code, strlen(code));
    if (result == BUN_EXCEPTION) {
        FAIL("%s threw unexpectedly: %s", label, bun_last_error(ctx, NULL));
        return 0;
    }

    if (bun_to_bool(result)) {
        PASS(label);
        return 1;
    }

    FAIL("%s returned false", label);
    return 0;
}

static int check_eval_ok(BunContext* ctx, const char* label, const char* code)
{
    BunValue result = bun_eval_string(ctx, code, strlen(code));
    if (result == BUN_EXCEPTION) {
        FAIL("%s threw unexpectedly: %s", label, bun_last_error(ctx, NULL));
        return 0;
    }

    PASS(label);
    return 1;
}

static void check_uncaught_call(BunContext* ctx, BunValue global)
{
    BunValue fn = bun_get(ctx, global, BUN_LITERAL("nativeThrowError"));
    BunValue result = bun_call(ctx, fn, BUN_UNDEFINED, 0, NULL);
    if (result != BUN_EXCEPTION) {
        FAIL("bun_call should surface uncaught bun_throw exceptions");
        return;
    }

    const char* err = bun_last_error(ctx, NULL);
    if (err && err[0] != '\0') {
        PASS("bun_call surfaces bun_throw through bun_last_error");
    } else {
        FAIL("bun_call returned unexpected error text: %s", err ? err : "(null)");
    }
}

int main(void)
{
    BunRuntime* rt = bun_initialize(NULL);
    BunContext* ctx = rt ? bun_context(rt) : NULL;
    if (!rt || !ctx) {
        fprintf(stderr, "[FAIL] failed to initialize Bun runtime\n");
        return 1;
    }

    BunValue global = bun_global(ctx);
    BunValue native_throw_value_fn = bun_function(ctx, BUN_LITERAL("nativeThrowValue"), native_throw_value, NULL, 1);
    BunValue native_throw_error_fn = bun_function(ctx, BUN_LITERAL("nativeThrowError"), native_throw_error, NULL, 0);
    bun_set(ctx, global, BUN_LITERAL("nativeThrowValue"), native_throw_value_fn);
    bun_set(ctx, global, BUN_LITERAL("nativeThrowError"), native_throw_error_fn);

    BunValue accessor_obj = bun_object(ctx);
    if (!bun_define_getter(ctx, accessor_obj, BUN_LITERAL("boomGet"), accessor_throw_get, NULL, 0, 0)) {
        FAIL("failed to define throwing accessor getter");
    }
    if (!bun_define_setter(ctx, accessor_obj, BUN_LITERAL("boomSet"), accessor_throw_set, NULL, 0, 0)) {
        FAIL("failed to define throwing accessor setter");
    }
    bun_set(ctx, global, BUN_LITERAL("accessorObj"), accessor_obj);

    static const BunClassPropertyDescriptor THROWY_PROPERTIES[] = {
        { "propBoom", 8, throwy_get, throwy_set, NULL, 0, 0, 0 },
    };
    static const BunClassMethodDescriptor THROWY_METHODS[] = {
        { "methodBoom", 10, throwy_method, NULL, 0, 0, 0 },
    };
    static const BunClassStaticPropertyDescriptor THROWY_STATIC_PROPERTIES[] = {
        { "staticProp", 10, throwy_static_get, throwy_static_set, NULL, 0, 0, 0 },
    };
    static const BunClassStaticMethodDescriptor THROWY_STATIC_METHODS[] = {
        { "staticBoom", 10, throwy_static_method, NULL, 0, 0, 0 },
    };
    static const BunClassDescriptor THROWY_CLASS = {
        "Throwy",
        6,
        THROWY_PROPERTIES,
        sizeof(THROWY_PROPERTIES) / sizeof(THROWY_PROPERTIES[0]),
        THROWY_METHODS,
        sizeof(THROWY_METHODS) / sizeof(THROWY_METHODS[0]),
        throwy_construct,
        NULL,
        1,
        THROWY_STATIC_PROPERTIES,
        sizeof(THROWY_STATIC_PROPERTIES) / sizeof(THROWY_STATIC_PROPERTIES[0]),
        THROWY_STATIC_METHODS,
        sizeof(THROWY_STATIC_METHODS) / sizeof(THROWY_STATIC_METHODS[0]),
    };

    BunClass* throwy_class = bun_class_register(ctx, &THROWY_CLASS, NULL);
    if (!throwy_class) {
        FAIL("failed to register Throwy class");
        bun_destroy(rt);
        return 1;
    }

    BunValue throwy_ctor = bun_class_constructor(ctx, throwy_class);
    bun_set(ctx, global, BUN_LITERAL("Throwy"), throwy_ctor);

    BunValue constructed_error = bun_error(ctx, BUN_LITERAL("constructed error"));
    if (constructed_error == BUN_UNDEFINED) {
        FAIL("bun_error failed to construct an Error instance");
    } else {
        bun_set(ctx, global, BUN_LITERAL("constructedError"), constructed_error);
    }

    check_js_true(ctx, "bun_error constructs Error objects",
        "constructedError instanceof Error && constructedError.message === 'constructed error'");
    check_js_true(ctx, "bun_throw passes raw values through bun_function",
        "(() => { try { nativeThrowValue('raw boom'); return false; } catch (e) { return e === 'raw boom'; } })()");
    check_js_true(ctx, "bun_throw passes Error instances through bun_function",
        "(() => { try { nativeThrowError(); return false; } catch (e) { return e instanceof Error && e.message === 'native error'; } })()");
    check_js_true(ctx, "bun_throw works in custom accessor getters",
        "(() => { try { return accessorObj.boomGet, false; } catch (e) { return e instanceof Error && e.message === 'accessor getter boom'; } })()");
    check_js_true(ctx, "bun_throw works in custom accessor setters",
        "(() => { try { accessorObj.boomSet = 1; return false; } catch (e) { return e instanceof Error && e.message === 'accessor setter boom'; } })()");
    check_js_true(ctx, "bun_throw works in class constructors",
        "(() => { try { new Throwy(true); return false; } catch (e) { return e instanceof Error && e.message === 'ctor boom'; } })()");
    check_eval_ok(ctx, "Throwy constructor still succeeds on non-throwing path",
        "globalThis.throwy = new Throwy(false)");
    check_js_true(ctx, "bun_throw works in class methods",
        "(() => { try { throwy.methodBoom(); return false; } catch (e) { return e instanceof Error && e.message === 'method boom'; } })()");
    check_js_true(ctx, "bun_throw works in class property getters",
        "(() => { try { return throwy.propBoom, false; } catch (e) { return e instanceof Error && e.message === 'getter boom'; } })()");
    check_js_true(ctx, "bun_throw works in class property setters",
        "(() => { try { throwy.propBoom = 1; return false; } catch (e) { return e instanceof Error && e.message === 'setter boom'; } })()");
    check_js_true(ctx, "bun_throw works in class static methods",
        "(() => { try { Throwy.staticBoom(); return false; } catch (e) { return e instanceof Error && e.message === 'static method boom'; } })()");
    check_js_true(ctx, "bun_throw works in class static property getters",
        "(() => { try { return Throwy.staticProp, false; } catch (e) { return e instanceof Error && e.message === 'static getter boom'; } })()");
    check_js_true(ctx, "bun_throw works in class static property setters",
        "(() => { try { Throwy.staticProp = 1; return false; } catch (e) { return e instanceof Error && e.message === 'static setter boom'; } })()");

    check_uncaught_call(ctx, global);

    bun_destroy(rt);
    return failed == 0 ? 0 : 1;
}
