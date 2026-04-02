/// example.c — Demonstrates the zero-copy BunValue embedding API.
///
/// Build (assuming bun is built as a shared library):
///   cc -o example example.c -L<bun-lib-dir> -lbun -I.
///
/// This file is for illustration only and is NOT compiled as part of Bun.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "bun_embed.h"

#define BUN_LITERAL(str) (str), sizeof(str) - 1
#define BUN_CSTR(str) (str), strlen(str)

typedef struct {
    int value;
} Counter;

typedef struct {
    int x;
    int y;
} NativeView;

typedef struct {
    NativeView view;
    char text[64];
} NativeText;

static char view_static_tag[64] = "view-static";

static void text_assign_utf8(NativeText* text, const char* utf8, size_t len)
{
    if (!text || !utf8) return;
    if (len >= sizeof(text->text)) len = sizeof(text->text) - 1;
    memcpy(text->text, utf8, len);
    text->text[len] = '\0';
}

static void text_assign_value(BunContext* ctx, NativeText* text, BunValue value)
{
    size_t len = 0;
    char* utf8 = bun_to_utf8(ctx, value, &len);
    if (!utf8) return;
    text_assign_utf8(text, utf8, len);
    free(utf8);
}

static BunValue view_get_x(BunContext* ctx, BunValue this_value, void* native_ptr, void* userdata)
{
    (void)ctx;
    (void)this_value;
    (void)userdata;
    NativeView* view = (NativeView*)native_ptr;
    return view ? bun_int32(view->x) : BUN_UNDEFINED;
}

static void view_set_x(BunContext* ctx, BunValue this_value, void* native_ptr, BunValue value, void* userdata)
{
    (void)ctx;
    (void)this_value;
    (void)userdata;
    NativeView* view = (NativeView*)native_ptr;
    if (!view) return;
    view->x = bun_to_int32(value);
}

static BunValue view_get_y(BunContext* ctx, BunValue this_value, void* native_ptr, void* userdata)
{
    (void)ctx;
    (void)this_value;
    (void)userdata;
    NativeView* view = (NativeView*)native_ptr;
    return view ? bun_int32(view->y) : BUN_UNDEFINED;
}

static void view_set_y(BunContext* ctx, BunValue this_value, void* native_ptr, BunValue value, void* userdata)
{
    (void)ctx;
    (void)this_value;
    (void)userdata;
    NativeView* view = (NativeView*)native_ptr;
    if (!view) return;
    view->y = bun_to_int32(value);
}

static BunValue view_move_by(BunContext* ctx, BunValue this_value, void* native_ptr, int argc, const BunValue* argv, void* userdata)
{
    (void)this_value;
    (void)userdata;
    NativeView* view = (NativeView*)native_ptr;
    if (!view) return BUN_UNDEFINED;

    if (argc >= 1 && argv) view->x += bun_to_int32(argv[0]);
    if (argc >= 2 && argv) view->y += bun_to_int32(argv[1]);

    BunValue result = bun_object(ctx);
    bun_set(ctx, result, "x", 1, bun_int32(view->x));
    bun_set(ctx, result, "y", 1, bun_int32(view->y));
    return result;
}

static BunValue text_get_content(BunContext* ctx, BunValue this_value, void* native_ptr, void* userdata)
{
    (void)this_value;
    (void)userdata;
    NativeText* text = (NativeText*)native_ptr;
    if (!text) return BUN_UNDEFINED;
    return bun_string(ctx, text->text, strlen(text->text));
}

static void text_set_content(BunContext* ctx, BunValue this_value, void* native_ptr, BunValue value, void* userdata)
{
    (void)this_value;
    (void)userdata;
    NativeText* text = (NativeText*)native_ptr;
    if (!text) return;

    text_assign_value(ctx, text, value);
}

static BunValue text_measure(BunContext* ctx, BunValue this_value, void* native_ptr, int argc, const BunValue* argv, void* userdata)
{
    (void)ctx;
    (void)this_value;
    (void)argc;
    (void)argv;
    (void)userdata;
    NativeText* text = (NativeText*)native_ptr;
    return text ? bun_int32((int32_t)strlen(text->text)) : BUN_UNDEFINED;
}

static BunValue view_static_describe(BunContext* ctx, BunValue this_value, void* userdata, int argc, const BunValue* argv)
{
    (void)userdata;
    (void)argc;
    (void)argv;
    return bun_get(ctx, this_value, "name", 4);
}

static BunValue view_static_get_tag(BunContext* ctx, BunValue this_value, void* userdata)
{
    (void)this_value;
    (void)userdata;
    return bun_string(ctx, view_static_tag, strlen(view_static_tag));
}

static void view_static_assign_tag(BunContext* ctx, BunValue value)
{
    size_t len = 0;
    char* utf8 = bun_to_utf8(ctx, value, &len);
    if (!utf8) return;
    if (len >= sizeof(view_static_tag)) len = sizeof(view_static_tag) - 1;
    memcpy(view_static_tag, utf8, len);
    view_static_tag[len] = '\0';
    free(utf8);
}

static void view_static_set_tag(BunContext* ctx, BunValue this_value, BunValue value, void* userdata)
{
    (void)this_value;
    (void)userdata;
    view_static_assign_tag(ctx, value);
}

static void native_view_finalize(void* native_ptr, void* userdata)
{
    const char* class_name = (const char*)userdata;
    printf("  [class finalizer] freeing %s at %p\n", class_name ? class_name : "instance", native_ptr);
    free(native_ptr);
}

static BunValue view_construct(BunContext* ctx, BunClass* klass, int argc, const BunValue* argv, void* userdata)
{
    (void)userdata;
    NativeView* view = (NativeView*)calloc(1, sizeof(*view));
    if (!view) return BUN_UNDEFINED;

    if (argc >= 1 && argv) view->x = bun_to_int32(argv[0]);
    if (argc >= 2 && argv) view->y = bun_to_int32(argv[1]);

    return bun_class_new(ctx, klass, view, native_view_finalize, "View");
}

static BunValue text_construct(BunContext* ctx, BunClass* klass, int argc, const BunValue* argv, void* userdata)
{
    (void)userdata;
    NativeText* text = (NativeText*)calloc(1, sizeof(*text));
    if (!text) return BUN_UNDEFINED;

    if (argc >= 1 && argv) text->view.x = bun_to_int32(argv[0]);
    if (argc >= 2 && argv) text->view.y = bun_to_int32(argv[1]);
    if (argc >= 3 && argv) text_assign_value(ctx, text, argv[2]);

    return bun_class_new(ctx, klass, text, native_view_finalize, "Text");
}

static const BunClassPropertyDescriptor VIEW_PROPERTIES[] = {
    { "x", 1, view_get_x, view_set_x, NULL, 0, 0, 0 },
    { "y", 1, view_get_y, view_set_y, NULL, 0, 0, 0 },
};

static const BunClassMethodDescriptor VIEW_METHODS[] = {
    { "moveBy", 6, view_move_by, NULL, 2, 0, 0 },
};

static const BunClassStaticPropertyDescriptor VIEW_STATIC_PROPERTIES[] = {
    { "tag", 3, view_static_get_tag, view_static_set_tag, NULL, 0, 0, 0 },
};

static const BunClassStaticMethodDescriptor VIEW_STATIC_METHODS[] = {
    { "describe", 8, view_static_describe, NULL, 0, 0, 0 },
};

static const BunClassDescriptor VIEW_CLASS = {
    "View",
    4,
    VIEW_PROPERTIES,
    sizeof(VIEW_PROPERTIES) / sizeof(VIEW_PROPERTIES[0]),
    VIEW_METHODS,
    sizeof(VIEW_METHODS) / sizeof(VIEW_METHODS[0]),
    view_construct,
    NULL,
    2,
    VIEW_STATIC_PROPERTIES,
    sizeof(VIEW_STATIC_PROPERTIES) / sizeof(VIEW_STATIC_PROPERTIES[0]),
    VIEW_STATIC_METHODS,
    sizeof(VIEW_STATIC_METHODS) / sizeof(VIEW_STATIC_METHODS[0]),
};

static const BunClassPropertyDescriptor TEXT_PROPERTIES[] = {
    { "text", 4, text_get_content, text_set_content, NULL, 0, 0, 0 },
};

static const BunClassMethodDescriptor TEXT_METHODS[] = {
    { "measure", 7, text_measure, NULL, 0, 0, 0 },
};

static const BunClassDescriptor TEXT_CLASS = {
    "Text",
    4,
    TEXT_PROPERTIES,
    sizeof(TEXT_PROPERTIES) / sizeof(TEXT_PROPERTIES[0]),
    TEXT_METHODS,
    sizeof(TEXT_METHODS) / sizeof(TEXT_METHODS[0]),
    text_construct,
    NULL,
    3,
    NULL,
    0,
    NULL,
    0,
};

static void counter_finalize(void* userdata)
{
    Counter* counter = (Counter*)userdata;
    free(counter);
}

static BunValue native_add(BunContext* ctx, int argc, const BunValue* argv, void* userdata)
{
    (void)ctx;
    (void)userdata;
    if (argc < 2 || !argv) return BUN_UNDEFINED;

    double a = bun_to_number(ctx, argv[0]);
    double b = bun_to_number(ctx, argv[1]);
    return bun_number(a + b);
}

static BunValue counter_inc(BunContext* ctx, int argc, const BunValue* argv, void* userdata)
{
    (void)argc;
    (void)argv;
    (void)userdata;

    BunValue self = bun_get(ctx, bun_global(ctx), "counter", 7);
    Counter* counter = (Counter*)bun_get_opaque(ctx, self);
    if (!counter) return BUN_UNDEFINED;

    counter->value += 1;
    return bun_int32(counter->value);
}

static BunValue counter_get(BunContext* ctx, BunValue this_value, void* userdata)
{
    (void)userdata;
    Counter* counter = (Counter*)bun_get_opaque(ctx, this_value);
    if (!counter) return BUN_UNDEFINED;
    return bun_int32(counter->value);
}

static void counter_set(BunContext* ctx, BunValue this_value, BunValue value, void* userdata)
{
    (void)userdata;
    Counter* counter = (Counter*)bun_get_opaque(ctx, this_value);
    if (!counter) return;
    counter->value = bun_to_int32(value);
}

static BunValue native_greet(BunContext* ctx, int argc, const BunValue* argv, void* userdata)
{
    (void)userdata;

    const char* default_name = "World";
    const char* name = default_name;
    char* owned_name = NULL;
    size_t owned_len = 0;

    if (argc >= 1 && argv) {
        owned_name = bun_to_utf8(ctx, argv[0], &owned_len);
        if (owned_name && owned_len > 0) name = owned_name;
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "Hello, %s!", name);

    if (owned_name) free(owned_name);
    return bun_string(ctx, buf, strlen(buf));
}

static BunValue native_async_tick(BunContext* ctx, int argc, const BunValue* argv, void* userdata)
{
    (void)ctx;
    (void)argc;
    (void)argv;
    int* tick_count = (int*)userdata;
    if (!tick_count) return BUN_UNDEFINED;

    *tick_count += 1;
    printf("Async tick %d\n", *tick_count);
    return BUN_UNDEFINED;
}

static void float_buffer_finalize(void* userdata)
{
    float* buf = (float*)userdata;
    printf("  [finalizer] freeing float buffer at %p\n", (void*)buf);
    free(buf);
}

static BunValue native_sum_typed(BunContext* ctx, int argc, const BunValue* argv, void* userdata)
{
    (void)userdata;
    /* Expects one Float32Array argument. Returns the sum of all elements. */
    if (argc < 1 || !argv) return bun_number(0.0);
    /* For the demo we just return the buffer length as a proxy for success */
    (void)ctx;
    return BUN_UNDEFINED; /* simplified — real code would read the typed array */
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("Initializing Bun runtime...\n");

    BunRuntime* rt = bun_initialize(NULL);
    if (!rt) {
        fprintf(stderr, "Failed to initialize Bun runtime\n");
        return 1;
    }

    BunContext* ctx = bun_context(rt);
    BunValue global = bun_global(ctx);
    int async_tick_count = 0;

    BunValue add_fn = bun_function(ctx, BUN_LITERAL("nativeAdd"), native_add, NULL, 2);
    BunValue greet_fn = bun_function(ctx, BUN_LITERAL("nativeGreet"), native_greet, NULL, 1);
    BunValue async_tick_fn = bun_function(ctx, BUN_LITERAL("nativeAsyncTick"), native_async_tick, &async_tick_count, 0);

    bun_set(ctx, global, "nativeAdd", 9, add_fn);
    bun_set(ctx, global, "nativeGreet", 11, greet_fn);
    bun_set(ctx, global, "nativeAsyncTick", 15, async_tick_fn);

    Counter* counter = malloc(sizeof(*counter));
    if (!counter) {
        fprintf(stderr, "Failed to allocate counter\n");
        bun_destroy(rt);
        return 1;
    }
    counter->value = 10;

    BunValue counter_obj = bun_object(ctx);
    BunValue inc_fn = bun_function(ctx, BUN_LITERAL("inc"), counter_inc, NULL, 0);
    bun_set_opaque(ctx, counter_obj, counter);
    bun_define_finalizer(ctx, counter_obj, counter_finalize, counter);
    bun_set(ctx, counter_obj, "inc", 3, inc_fn);
    bun_define_accessor(ctx, counter_obj, "value", 5, counter_get, counter_set, NULL, 0, 0, 0);
    bun_set(ctx, global, "counter", 7, counter_obj);

    BunClass* view_class = bun_class_register(ctx, &VIEW_CLASS, NULL);
    BunClass* text_class = bun_class_register(ctx, &TEXT_CLASS, view_class);
    if (!view_class || !text_class) {
        fprintf(stderr, "Failed to register BunClass descriptors\n");
        bun_destroy(rt);
        return 1;
    }

    NativeText* label = calloc(1, sizeof(*label));
    if (!label) {
        fprintf(stderr, "Failed to allocate label\n");
        bun_destroy(rt);
        return 1;
    }
    label->view.x = 12;
    label->view.y = 18;
    strcpy(label->text, "embed label");

    BunValue label_obj = bun_class_new(ctx, text_class, label, native_view_finalize, "Text");
    BunValue view_proto = bun_class_prototype(ctx, view_class);
    BunValue text_proto = bun_class_prototype(ctx, text_class);
    BunValue view_ctor = bun_class_constructor(ctx, view_class);
    BunValue text_ctor = bun_class_constructor(ctx, text_class);
    bun_set(ctx, global, "label", 5, label_obj);
    bun_set(ctx, global, "ViewProto", 9, view_proto);
    bun_set(ctx, global, "TextProto", 9, text_proto);
    bun_set(ctx, global, "View", 4, view_ctor);
    bun_set(ctx, global, "Text", 4, text_ctor);

    printf("class instance? %d\n", bun_is_class_instance(ctx, label_obj));
    printf("instanceof View? %d\n", bun_instanceof_class(ctx, label_obj, view_class));
    printf("instanceof Text? %d\n", bun_instanceof_class(ctx, label_obj, text_class));

    NativeView* unwrapped_view = (NativeView*)bun_class_unwrap(ctx, label_obj, view_class);
    NativeText* unwrapped_text = (NativeText*)bun_class_unwrap(ctx, label_obj, text_class);
    printf("unwrap(view)=%p unwrap(text)=%p\n", (void*)unwrapped_view, (void*)unwrapped_text);

    printf("\n--- Evaluating JS ---\n");

    printf("\n--- embed eval return-value regression ---\n");
    {
        BunValue add_result = bun_eval_string(ctx, BUN_LITERAL("(1+1)"));
        if (add_result == BUN_EXCEPTION) {
            fprintf(stderr, "[FAIL] bun_eval_string('(1+1)') threw: %s\n", bun_last_error(ctx, NULL));
        } else {
            double number = bun_to_number(ctx, add_result);
            if (number == 2.0) {
                printf("[PASS] bun_eval_string('(1+1)') -> 2\n");
            } else {
                fprintf(stderr, "[FAIL] bun_eval_string('(1+1)') -> %g (expected 2)\n", number);
            }
        }
    }

    {
        BunValue str_result = bun_eval_string(ctx, BUN_LITERAL("(String(1+1))"));
        if (str_result == BUN_EXCEPTION) {
            fprintf(stderr, "[FAIL] bun_eval_string('(String(1+1))') threw: %s\n", bun_last_error(ctx, NULL));
        } else {
            size_t str_len = 0;
            char* str = bun_to_utf8(ctx, str_result, &str_len);
            if (str && str_len == 1 && strcmp(str, "2") == 0) {
                printf("[PASS] bun_eval_string('(String(1+1))') -> \"2\"\n");
            } else {
                fprintf(stderr, "[FAIL] bun_eval_string('(String(1+1))') -> %s len=%zu (expected \"2\")\n", str ? str : "(null)", str_len);
            }
            free(str);
        }
    }

    {
        BunValue obj_result = bun_eval_string(ctx, BUN_LITERAL("({ answer: 42 })"));
        if (obj_result == BUN_EXCEPTION) {
            fprintf(stderr, "[FAIL] bun_eval_string('({ answer: 42 })') threw: %s\n", bun_last_error(ctx, NULL));
        } else if (!bun_is_object(obj_result)) {
            fprintf(stderr, "[FAIL] bun_eval_string('({ answer: 42 })') did not return an object\n");
        } else {
            BunValue answer = bun_get(ctx, obj_result, "answer", 6);
            if (!bun_is_number(answer)) {
                fprintf(stderr, "[FAIL] object literal answer property is not a number\n");
            } else {
                double number = bun_to_number(ctx, answer);
                if (number == 42.0) {
                    printf("[PASS] bun_eval_string('({ answer: 42 })') -> object.answer = 42\n");
                } else {
                    fprintf(stderr, "[FAIL] object literal answer = %g (expected 42)\n", number);
                }
            }
        }
    }

    {
        BunValue syntax_result = bun_eval_string(ctx, BUN_LITERAL("const x ="));
        if (syntax_result != BUN_EXCEPTION) {
            fprintf(stderr, "[FAIL] bun_eval_string('const x =') unexpectedly succeeded\n");
        } else {
            const char* err = bun_last_error(ctx, NULL);
            if (err && strstr(err, "SyntaxError") != NULL) {
                printf("[PASS] bun_eval_string(syntax error) -> BUN_EXCEPTION\n");
            } else {
                fprintf(stderr, "[FAIL] bun_eval_string(syntax error) returned unexpected message: %s\n", err ? err : "(null)");
            }
        }
    }

#define EVAL(code)                                                     \
    do {                                                               \
        if (bun_eval_string(ctx, BUN_CSTR(code)) == BUN_EXCEPTION)     \
            fprintf(stderr, "Error: %s\n", bun_last_error(ctx, NULL)); \
    } while (0)

    EVAL("console.log('Hello from embedded Bun!')");
    EVAL("console.log('nativeAdd(3, 4) =', nativeAdd(3, 4))");

    EVAL(
        "const fromCtor = new Text(5, 9, 'from constructor');"
        "console.log('fromCtor instanceof Text?', fromCtor instanceof Text);"
        "console.log('fromCtor instanceof View?', fromCtor instanceof View);"
        "console.log('fromCtor.measure() =', fromCtor.measure());"
        "console.log('fromCtor.moveBy(1, 2) =', fromCtor.moveBy(1, 2));"
        "console.log('fromCtor.constructor === Text?', fromCtor.constructor === Text);"
        "console.log('View.describe() =', View.describe());"
        "console.log('Text.describe() =', Text.describe());"
        "console.log('View.tag =', View.tag);"
        "Text.tag = 'updated-static';"
        "console.log('View.tag after Text.tag set =', View.tag);"
        "console.log('Text.tag =', Text.tag);");

    EVAL("console.log(nativeGreet('Bun'))");

    EVAL(
        "console.log('counter.value =', counter.value);"
        "counter.value = 42;"
        "console.log('counter.inc() =', counter.inc());"
        "console.log('counter.value =', counter.value);");

    EVAL(
        "console.log('label.text =', label.text);"
        "console.log('label.measure() =', label.measure());"
        "console.log('moveBy ->', label.moveBy(3, 4));"
        "console.log('label.x,label.y =', label.x, label.y);"
        "console.log('text proto === Object.getPrototypeOf(label):', TextProto === Object.getPrototypeOf(label));"
        "console.log('view proto === Object.getPrototypeOf(TextProto):', ViewProto === Object.getPrototypeOf(TextProto));");

    // Demonstrate bun_call with error detection.
    if (bun_eval_string(ctx, BUN_LITERAL("globalThis.throwingFn = () => { throw new Error('boom'); };")) != BUN_EXCEPTION) {
        BunValue throwing_fn = bun_get(ctx, global, "throwingFn", 10);
        BunValue result = bun_call(ctx, throwing_fn, BUN_UNDEFINED, 0, NULL);
        if (result == BUN_EXCEPTION) {
            const char* err = bun_last_error(ctx, NULL);
            printf("bun_call caught exception: %s\n", err ? err : "(no message)");
        }
    }

    // Regression test: non-Error throws should not degrade into
    // "TypeError: No default value" in bun_last_error().
    printf("\n--- embed exception formatting regression ---\n");
    if (bun_eval_string(
            ctx,
            BUN_LITERAL(
                "globalThis.__embed_to_primitive_called = false;"
                "const thrown = {"
                "  marker: 'embed-non-error-throw',"
                "  [Symbol.toPrimitive]() {"
                "    globalThis.__embed_to_primitive_called = true;"
                "    return 'coerced';"
                "  }"
                "};"
                "throw thrown;"))
        == BUN_EXCEPTION) {
        const char* err = bun_last_error(ctx, NULL);
        printf("bun_eval_string non-Error throw: %s\n", err ? err : "(no message)");

        if (err && strstr(err, "No default value") != NULL) {
            fprintf(stderr, "[FAIL] bun_last_error regressed to TypeError: No default value\n");
        } else {
            printf("[PASS] bun_last_error is stable for non-Error throws\n");
        }
    } else {
        fprintf(stderr, "[FAIL] expected non-Error throw regression test to fail eval\n");
    }

    printf("\n--- embed eval_file regression ---\n");
    {
        {
            BunRuntime* eval_file_rt = bun_initialize(NULL);
            BunContext* eval_file_ctx = bun_context(eval_file_rt);
            if (!eval_file_rt || !eval_file_ctx) {
                fprintf(stderr, "[FAIL] unable to initialize runtime for eval_file success case\n");
            } else {
                char tmp_path[128];
                snprintf(tmp_path, sizeof(tmp_path), "/tmp/bun-embed-ok-%ld.mjs", (long)getpid());
                int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (fd < 0) {
                    fprintf(stderr, "[FAIL] creating eval_file success module failed\n");
                } else {
                    const char* module_src = "globalThis.__embed_eval_file_ok = 41 + 1;\n";
                    ssize_t wrote = write(fd, module_src, strlen(module_src));
                    close(fd);

                    if (wrote < 0 || (size_t)wrote != strlen(module_src)) {
                        fprintf(stderr, "[FAIL] writing eval_file success module failed\n");
                    } else {
                        BunValue eval_file_ok = bun_eval_file(eval_file_ctx, BUN_CSTR(tmp_path));
                        if (eval_file_ok == BUN_EXCEPTION) {
                            fprintf(stderr, "[FAIL] bun_eval_file(success) threw: %s\n", bun_last_error(eval_file_ctx, NULL));
                        } else if (eval_file_ok != BUN_UNDEFINED) {
                            fprintf(stderr, "[FAIL] bun_eval_file(success) returned %llu (expected BUN_UNDEFINED)\n", (unsigned long long)eval_file_ok);
                        } else {
                            BunValue ok_value = bun_eval_string(eval_file_ctx, BUN_LITERAL("globalThis.__embed_eval_file_ok"));
                            if (ok_value == BUN_EXCEPTION) {
                                fprintf(stderr, "[FAIL] reading __embed_eval_file_ok threw: %s\n", bun_last_error(eval_file_ctx, NULL));
                            } else {
                                double n = bun_to_number(eval_file_ctx, ok_value);
                                if (n == 42.0) {
                                    printf("[PASS] bun_eval_file(success) -> BUN_UNDEFINED and module executed\n");
                                } else {
                                    fprintf(stderr, "[FAIL] __embed_eval_file_ok = %g (expected 42)\n", n);
                                }
                            }
                        }
                    }

                    unlink(tmp_path);
                }

                bun_destroy(eval_file_rt);
            }
        }

        {
            BunRuntime* eval_file_rt = bun_initialize(NULL);
            BunContext* eval_file_ctx = bun_context(eval_file_rt);
            if (!eval_file_rt || !eval_file_ctx) {
                fprintf(stderr, "[FAIL] unable to initialize runtime for eval_file top-level await case\n");
            } else {
                char tmp_path[128];
                snprintf(tmp_path, sizeof(tmp_path), "/tmp/bun-embed-await-%ld.mjs", (long)getpid());
                int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (fd < 0) {
                    fprintf(stderr, "[FAIL] creating eval_file top-level await module failed\n");
                } else {
                    const char* module_src = "globalThis.__embed_eval_file_await = await Promise.resolve(40 + 2);\n";
                    ssize_t wrote = write(fd, module_src, strlen(module_src));
                    close(fd);

                    if (wrote < 0 || (size_t)wrote != strlen(module_src)) {
                        fprintf(stderr, "[FAIL] writing eval_file top-level await module failed\n");
                    } else {
                        BunValue eval_file_await = bun_eval_file(eval_file_ctx, BUN_CSTR(tmp_path));
                        if (eval_file_await == BUN_EXCEPTION) {
                            fprintf(stderr, "[FAIL] bun_eval_file(top-level await) threw: %s\n", bun_last_error(eval_file_ctx, NULL));
                        } else if (eval_file_await != BUN_UNDEFINED) {
                            fprintf(stderr, "[FAIL] bun_eval_file(top-level await) returned %llu (expected BUN_UNDEFINED)\n", (unsigned long long)eval_file_await);
                        } else {
                            BunValue awaited = bun_eval_string(eval_file_ctx, BUN_LITERAL("globalThis.__embed_eval_file_await"));
                            if (awaited == BUN_EXCEPTION) {
                                fprintf(stderr, "[FAIL] reading __embed_eval_file_await threw: %s\n", bun_last_error(eval_file_ctx, NULL));
                            } else {
                                double n = bun_to_number(eval_file_ctx, awaited);
                                if (n == 42.0) {
                                    printf("[PASS] bun_eval_file(top-level await) -> BUN_UNDEFINED and module executed\n");
                                } else {
                                    fprintf(stderr, "[FAIL] __embed_eval_file_await = %g (expected 42)\n", n);
                                }
                            }
                        }
                    }

                    unlink(tmp_path);
                }

                bun_destroy(eval_file_rt);
            }
        }

        {
            BunRuntime* eval_file_rt = bun_initialize(NULL);
            BunContext* eval_file_ctx = bun_context(eval_file_rt);
            if (!eval_file_rt || !eval_file_ctx) {
                fprintf(stderr, "[FAIL] unable to initialize runtime for eval_file missing-file case\n");
            } else {
                char tmp_path[128];
                snprintf(tmp_path, sizeof(tmp_path), "/tmp/bun-embed-missing-%ld.mjs", (long)getpid());
                unlink(tmp_path);

                BunValue missing = bun_eval_file(eval_file_ctx, BUN_CSTR(tmp_path));
                if (missing != BUN_EXCEPTION) {
                    fprintf(stderr, "[FAIL] bun_eval_file(missing file) unexpectedly succeeded\n");
                } else {
                    size_t err_len = 0;
                    const char* err = bun_last_error(eval_file_ctx, &err_len);
                    if (err && err_len > 0) {
                        printf("[PASS] bun_eval_file(missing file) -> BUN_EXCEPTION\n");
                    } else {
                        fprintf(stderr, "[FAIL] bun_eval_file(missing file) did not provide an error message\n");
                    }
                }

                bun_destroy(eval_file_rt);
            }
        }

        {
            BunRuntime* eval_file_rt = bun_initialize(NULL);
            BunContext* eval_file_ctx = bun_context(eval_file_rt);
            if (!eval_file_rt || !eval_file_ctx) {
                fprintf(stderr, "[FAIL] unable to initialize runtime for eval_file syntax-error case\n");
            } else {
                char tmp_path[128];
                snprintf(tmp_path, sizeof(tmp_path), "/tmp/bun-embed-syntax-%ld.mjs", (long)getpid());
                int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (fd < 0) {
                    fprintf(stderr, "[FAIL] creating eval_file syntax-error module failed\n");
                } else {
                    const char* module_src = "const broken = ;\n";
                    ssize_t wrote = write(fd, module_src, strlen(module_src));
                    close(fd);

                    if (wrote < 0 || (size_t)wrote != strlen(module_src)) {
                        fprintf(stderr, "[FAIL] writing eval_file syntax-error module failed\n");
                    } else {
                        BunValue eval_file_syntax = bun_eval_file(eval_file_ctx, BUN_CSTR(tmp_path));
                        if (eval_file_syntax != BUN_EXCEPTION) {
                            fprintf(stderr, "[FAIL] bun_eval_file(syntax error) unexpectedly succeeded\n");
                        } else {
                            const char* err = bun_last_error(eval_file_ctx, NULL);
                            if (err && (strstr(err, "SyntaxError") != NULL || strstr(err, "Unexpected") != NULL || strstr(err, "BuildMessage") != NULL)) {
                                printf("[PASS] bun_eval_file(syntax error) -> readable error text\n");
                            } else {
                                fprintf(stderr, "[FAIL] bun_eval_file(syntax error) returned unexpected message: %s\n", err ? err : "(null)");
                            }
                        }
                    }

                    unlink(tmp_path);
                }

                bun_destroy(eval_file_rt);
            }
        }

        {
            BunRuntime* eval_file_rt = bun_initialize(NULL);
            BunContext* eval_file_ctx = bun_context(eval_file_rt);
            if (!eval_file_rt || !eval_file_ctx) {
                fprintf(stderr, "[FAIL] unable to initialize runtime for eval_file throw case\n");
            } else {
                char tmp_path[128];
                snprintf(tmp_path, sizeof(tmp_path), "/tmp/bun-embed-throw-%ld.mjs", (long)getpid());
                int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (fd < 0) {
                    fprintf(stderr, "[FAIL] creating eval_file throw module failed\n");
                } else {
                    const char* module_src = "throw { marker: 'embed-eval-file-throw' };\n";
                    ssize_t wrote = write(fd, module_src, strlen(module_src));
                    close(fd);

                    if (wrote < 0 || (size_t)wrote != strlen(module_src)) {
                        fprintf(stderr, "[FAIL] writing eval_file throw module failed\n");
                    } else {
                        BunValue eval_file_throw = bun_eval_file(eval_file_ctx, BUN_CSTR(tmp_path));
                        if (eval_file_throw != BUN_EXCEPTION) {
                            fprintf(stderr, "[FAIL] bun_eval_file(throw) unexpectedly succeeded\n");
                        } else {
                            const char* err = bun_last_error(eval_file_ctx, NULL);
                            if (err && strstr(err, "No default value") != NULL) {
                                fprintf(stderr, "[FAIL] bun_eval_file error regressed to TypeError: No default value\n");
                            } else {
                                printf("[PASS] bun_eval_file(non-Error throw) has stable error text\n");
                            }
                        }
                    }

                    unlink(tmp_path);
                }

                bun_destroy(eval_file_rt);
            }
        }
    }

    // Queue host-driven async calls to demonstrate event loop integration.
    bun_call_async(ctx, async_tick_fn, BUN_UNDEFINED, 0, NULL);
    bun_call_async(ctx, async_tick_fn, BUN_UNDEFINED, 0, NULL);
    bun_call_async(ctx, async_tick_fn, BUN_UNDEFINED, 0, NULL);

    // ------------------------------------------------------------------
    // Demonstrate bun_array_buffer and bun_typed_array (zero-copy)
    // ------------------------------------------------------------------
    printf("\n--- ArrayBuffer / TypedArray demo ---\n");

    // Heap-allocate a float buffer; ownership passes to the JS finalizer.
    const size_t num_floats = 4;
    float* floats = (float*)malloc(num_floats * sizeof(float));
    if (floats) {
        floats[0] = 1.5f;
        floats[1] = 2.5f;
        floats[2] = 3.0f;
        floats[3] = 4.0f;

        // Wrap as Float32Array — zero-copy, finalizer frees `floats` on GC.
        BunValue f32 = bun_typed_array(ctx, BUN_FLOAT32_ARRAY, floats,
            num_floats, float_buffer_finalize, floats);
        bun_set(ctx, global, "nativeFloats", 12, f32);

        BunTypedArrayInfo typed_info;
        if (bun_get_typed_array(ctx, f32, &typed_info)) {
            const float* host_floats = (const float*)typed_info.data;
            printf("Host typed array: kind=%u length=%zu first=%g\n",
                (unsigned)typed_info.kind,
                typed_info.element_count,
                host_floats ? host_floats[0] : 0.0f);
        }

        EVAL(
            "const a = nativeFloats;"
            "console.log('Float32Array length:', a.length);"
            "let sum = 0; for (const x of a) sum += x;"
            "console.log('Float32Array sum:', sum);");
    }

    // Wrap a static byte buffer as ArrayBuffer (no finalizer needed).
    static const uint8_t magic[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    BunValue ab = bun_array_buffer(ctx, (void*)(uintptr_t)magic, sizeof(magic), NULL, NULL);
    bun_set(ctx, global, "nativeBuf", 9, ab);

    BunArrayBufferInfo buffer_info;
    if (bun_get_array_buffer(ctx, ab, &buffer_info)) {
        const uint8_t* bytes = (const uint8_t*)buffer_info.data;
        printf("Host array buffer: length=%zu first=0x%02X\n",
            buffer_info.byte_length,
            bytes ? (unsigned)bytes[0] : 0);
    }

    EVAL(
        "const v = new DataView(nativeBuf);"
        "console.log('ArrayBuffer[0]:', v.getUint8(0).toString(16));"
        "console.log('ArrayBuffer length:', nativeBuf.byteLength);");

#undef EVAL

    printf("\n--- Running event loop ---\n");
    for (int i = 0; i < 100; i++) {
        int has_pending = bun_run_pending_jobs(rt);
        if (!has_pending) {
            printf("Event loop idle, stopping.\n");
            break;
        }
        usleep(50000); // 50ms - simulate frame rate
    }

    printf("\nDestroying Bun runtime...\n");
    printf("manual dispose(label) -> %d\n", bun_class_dispose(ctx, label_obj));
    printf("dispose(label) again -> %d\n", bun_class_dispose(ctx, label_obj));
    bun_destroy(rt);

    printf("Done.\n");
    return 0;
}
