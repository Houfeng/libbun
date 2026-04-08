/// test_eval_file_class.c — 验证 bun_class_new 在 bun_eval_file (ES module) 路径中的正确性。
///
/// 这是针对以下 bug 的回归测试：
///   当通过 bun_class_register 注册的类的第一次 bun_class_new 调用
///   发生在 bun_eval_file（ES module 上下文）内部时，会触发 segfault。
///
/// 根本原因：JSBunClassInstance::subspaceFor 错误地返回了 &vm.plainObjectSpace()
///   (IsoSubspace，按 sizeof(JSNonFinalObject) 分配)，而 JSBunClassInstance 本身
///   更大，且需要析构回调。loadEntryPoint 调用 performGC() 扰动了 bump allocator
///   cursor 状态，导致第一次 allocateCell 走慢路径时暴露尺寸不匹配。
///
/// 修复：将 subspaceFor 改为返回 &vm.destructibleObjectSpace()。
///
/// 编译（假设 libbun 在 build/shared-release/）:
///   cc -o test_eval_file_class test_eval_file_class.c \
///       -L../../../build/shared-release -lbun -I.. \
///       -Wl,-rpath,../../../build/shared-release
///   ./test_eval_file_class

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "../bun_embed.h"

/* ── 宏 ─────────────────────────────────────────── */
#define BUN_LITERAL(s) (s), (sizeof(s) - 1)
#define BUN_CSTR(s) (s), strlen(s)

#define PASS(msg) (printf("[PASS] %s\n", (msg)), passed++)
#define FAIL(msg, ...) (fprintf(stderr, "[FAIL] " msg "\n", ##__VA_ARGS__), failed++)

/* ── native 数据结构 ────────────────────────────── */
typedef struct {
    int x;
    int y;
} Vec2;
typedef struct {
    Vec2 pos;
    int radius;
} Circle;

/* ── 全局 finalizer 计数器（不可在 finalizer 中调用 JS API） ── */
static int g_vec2_finalized = 0;
static int g_circle_finalized = 0;

/* ── Vec2 class ─────────────────────────────────── */
static BunValue vec2_get_x(BunContext* ctx, BunValue this_, void* np, void* ud)
{
    (void)ctx;
    (void)this_;
    (void)ud;
    return np ? bun_int32(((Vec2*)np)->x) : BUN_UNDEFINED;
}
static void vec2_set_x(BunContext* ctx, BunValue this_, void* np, BunValue v, void* ud)
{
    (void)ctx;
    (void)this_;
    (void)ud;
    if (np) ((Vec2*)np)->x = bun_to_int32(v);
}
static BunValue vec2_get_y(BunContext* ctx, BunValue this_, void* np, void* ud)
{
    (void)ctx;
    (void)this_;
    (void)ud;
    return np ? bun_int32(((Vec2*)np)->y) : BUN_UNDEFINED;
}
static void vec2_set_y(BunContext* ctx, BunValue this_, void* np, BunValue v, void* ud)
{
    (void)ctx;
    (void)this_;
    (void)ud;
    if (np) ((Vec2*)np)->y = bun_to_int32(v);
}

static BunValue vec2_length(BunContext* ctx, BunValue this_, void* np,
    int argc, const BunValue* argv, void* ud)
{
    (void)ctx;
    (void)this_;
    (void)argc;
    (void)argv;
    (void)ud;
    Vec2* self = (Vec2*)np;
    if (!self) return BUN_UNDEFINED;
    /* 返回 x+y（故意简化，避免引入 math.h） */
    return bun_int32(self->x + self->y);
}
static void vec2_finalize(void* np, void* ud)
{
    (void)ud;
    free(np);
    g_vec2_finalized++;
}
static BunValue vec2_construct(BunContext* ctx, BunClass* klass,
    int argc, const BunValue* argv, void* ud)
{
    (void)ud;
    Vec2* v = (Vec2*)calloc(1, sizeof(Vec2));
    if (!v) return BUN_UNDEFINED;
    if (argc >= 1 && argv) v->x = bun_to_int32(argv[0]);
    if (argc >= 2 && argv) v->y = bun_to_int32(argv[1]);
    return bun_class_new(ctx, klass, v, vec2_finalize, NULL);
}

static const BunClassPropertyDescriptor VEC2_PROPS[] = {
    { "x", 1, vec2_get_x, vec2_set_x, NULL, 0, 0, 0 },
    { "y", 1, vec2_get_y, vec2_set_y, NULL, 0, 0, 0 },
};
static const BunClassMethodDescriptor VEC2_METHODS[] = {
    { "sum", 3, vec2_length, NULL, 0, 0, 0 },
};
static const BunClassDescriptor VEC2_CLASS = {
    "Vec2",
    4,
    VEC2_PROPS,
    sizeof(VEC2_PROPS) / sizeof(VEC2_PROPS[0]),
    VEC2_METHODS,
    sizeof(VEC2_METHODS) / sizeof(VEC2_METHODS[0]),
    vec2_construct,
    NULL,
    2,
    NULL,
    0,
    NULL,
    0,
};

/* ── Circle class (继承自 Vec2) ─────────────────── */
static BunValue circle_get_r(BunContext* ctx, BunValue this_, void* np, void* ud)
{
    (void)ctx;
    (void)this_;
    (void)ud;
    return np ? bun_int32(((Circle*)np)->radius) : BUN_UNDEFINED;
}
static void circle_set_r(BunContext* ctx, BunValue this_, void* np, BunValue v, void* ud)
{
    (void)ctx;
    (void)this_;
    (void)ud;
    if (np) ((Circle*)np)->radius = bun_to_int32(v);
}
static void circle_finalize(void* np, void* ud)
{
    (void)ud;
    free(np);
    g_circle_finalized++;
}
static BunValue circle_construct(BunContext* ctx, BunClass* klass,
    int argc, const BunValue* argv, void* ud)
{
    (void)ud;
    Circle* c = (Circle*)calloc(1, sizeof(Circle));
    if (!c) return BUN_UNDEFINED;
    if (argc >= 1 && argv) c->pos.x = bun_to_int32(argv[0]);
    if (argc >= 2 && argv) c->pos.y = bun_to_int32(argv[1]);
    if (argc >= 3 && argv) c->radius = bun_to_int32(argv[2]);
    return bun_class_new(ctx, klass, c, circle_finalize, NULL);
}

static const BunClassPropertyDescriptor CIRCLE_PROPS[] = {
    { "radius", 6, circle_get_r, circle_set_r, NULL, 0, 0, 0 },
};
static const BunClassDescriptor CIRCLE_CLASS = {
    "Circle",
    6,
    CIRCLE_PROPS,
    sizeof(CIRCLE_PROPS) / sizeof(CIRCLE_PROPS[0]),
    NULL,
    0,
    circle_construct,
    NULL,
    3,
    NULL,
    0,
    NULL,
    0,
};

/* ── 工具：写临时模块文件 ──────────────────────── */
static int write_tmp_module(const char* path, const char* src)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return 0;
    size_t len = strlen(src);
    ssize_t wrote = write(fd, src, len);
    close(fd);
    return (wrote == (ssize_t)len);
}

/* ── main ───────────────────────────────────────── */
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    int passed = 0, failed = 0;

    printf("=== test_eval_file_class: bun_class_new inside bun_eval_file ===\n\n");

    /* ------------------------------------------------------------------ */
    /* Test 1: 最简单的路径 — bun_eval_file 内 new Vec2()                 */
    /* ------------------------------------------------------------------ */
    printf("--- Test 1: first bun_class_new in bun_eval_file (smoke test) ---\n");
    {
        BunRuntime* rt = bun_initialize(NULL);
        BunContext* ctx = bun_context(rt);
        BunValue global = bun_global(ctx);

        BunClass* vec2_class = bun_class_register(ctx, &VEC2_CLASS, NULL);
        BunClass* circle_class = bun_class_register(ctx, &CIRCLE_CLASS, vec2_class);
        if (!vec2_class || !circle_class) {
            FAIL("bun_class_register failed");
            bun_destroy(rt);
            goto summary;
        }

        /* 将构造函数暴露到 globalThis */
        bun_set(ctx, global, "Vec2", 4, bun_class_constructor(ctx, vec2_class));
        bun_set(ctx, global, "Circle", 6, bun_class_constructor(ctx, circle_class));

        char tmp[128];
        snprintf(tmp, sizeof(tmp), "/tmp/bun-ecfc-t1-%ld.mjs", (long)getpid());

        /* 关键：第一次 new Vec2() 发生在 bun_eval_file 内（ES module 路径） */
        const char* src = "const v = new Vec2(3, 7);\n"
                          "globalThis.__t1_x = v.x;\n"
                          "globalThis.__t1_y = v.y;\n"
                          "globalThis.__t1_sum = v.sum();\n"
                          "globalThis.__t1_isVec2 = v instanceof Vec2;\n";

        if (!write_tmp_module(tmp, src)) {
            FAIL("cannot write tmp module for test 1");
        } else {
            BunValue r = bun_eval_file(ctx, BUN_CSTR(tmp));
            if (r == BUN_EXCEPTION) {
                FAIL("bun_eval_file threw: %s", bun_last_error(ctx, NULL));
            } else {
                int x = bun_to_int32(bun_eval_string(ctx, BUN_LITERAL("globalThis.__t1_x")));
                int y = bun_to_int32(bun_eval_string(ctx, BUN_LITERAL("globalThis.__t1_y")));
                int sum = bun_to_int32(bun_eval_string(ctx, BUN_LITERAL("globalThis.__t1_sum")));
                BunValue iv = bun_eval_string(ctx, BUN_LITERAL("globalThis.__t1_isVec2"));
                int is_v2 = (iv == BUN_TRUE);

                if (x == 3 && y == 7 && sum == 10 && is_v2)
                    PASS("first bun_class_new in bun_eval_file: Vec2(3,7) x=3 y=7 sum=10 instanceof=true");
                else
                    FAIL("Vec2(3,7): x=%d(want 3) y=%d(want 7) sum=%d(want 10) instanceof=%d(want 1)",
                        x, y, sum, is_v2);
            }
            unlink(tmp);
        }

        bun_destroy(rt);
    }

    /* ------------------------------------------------------------------ */
    /* Test 2: 多个实例 + 继承链                                          */
    /* ------------------------------------------------------------------ */
    printf("\n--- Test 2: multiple instances + inheritance inside bun_eval_file ---\n");
    {
        BunRuntime* rt = bun_initialize(NULL);
        BunContext* ctx = bun_context(rt);
        BunValue global = bun_global(ctx);

        BunClass* vec2_class = bun_class_register(ctx, &VEC2_CLASS, NULL);
        BunClass* circle_class = bun_class_register(ctx, &CIRCLE_CLASS, vec2_class);
        bun_set(ctx, global, "Vec2", 4, bun_class_constructor(ctx, vec2_class));
        bun_set(ctx, global, "Circle", 6, bun_class_constructor(ctx, circle_class));

        char tmp[128];
        snprintf(tmp, sizeof(tmp), "/tmp/bun-ecfc-t2-%ld.mjs", (long)getpid());

        const char* src = "const a = new Vec2(1, 2);\n"
                          "const b = new Vec2(10, 20);\n"
                          "const c = new Circle(5, 6, 9);\n"
                          "globalThis.__t2_a_x   = a.x;\n"
                          "globalThis.__t2_b_y   = b.y;\n"
                          "globalThis.__t2_c_r   = c.radius;\n"
                          "globalThis.__t2_c_x   = c.x;\n" /* 继承属性 */
                          "globalThis.__t2_ivc   = c instanceof Vec2;\n" /* instanceof 父类 */
                          "globalThis.__t2_icc   = c instanceof Circle;\n";

        if (!write_tmp_module(tmp, src)) {
            FAIL("cannot write tmp module for test 2");
        } else {
            BunValue r = bun_eval_file(ctx, BUN_CSTR(tmp));
            if (r == BUN_EXCEPTION) {
                FAIL("bun_eval_file threw: %s", bun_last_error(ctx, NULL));
            } else {
                int a_x = bun_to_int32(bun_eval_string(ctx, BUN_LITERAL("globalThis.__t2_a_x")));
                int b_y = bun_to_int32(bun_eval_string(ctx, BUN_LITERAL("globalThis.__t2_b_y")));
                int c_r = bun_to_int32(bun_eval_string(ctx, BUN_LITERAL("globalThis.__t2_c_r")));
                int c_x = bun_to_int32(bun_eval_string(ctx, BUN_LITERAL("globalThis.__t2_c_x")));
                int ivc = (bun_eval_string(ctx, BUN_LITERAL("globalThis.__t2_ivc")) == BUN_TRUE);
                int icc = (bun_eval_string(ctx, BUN_LITERAL("globalThis.__t2_icc")) == BUN_TRUE);

                if (a_x == 1 && b_y == 20 && c_r == 9 && c_x == 5 && ivc && icc)
                    PASS("multiple instances + inheritance: all values correct");
                else
                    FAIL("values: a.x=%d(1) b.y=%d(20) c.radius=%d(9) c.x=%d(5) ivc=%d(1) icc=%d(1)",
                        a_x, b_y, c_r, c_x, ivc, icc);
            }
            unlink(tmp);
        }

        bun_destroy(rt);
    }

    /* ------------------------------------------------------------------ */
    /* Test 3: native 属性写入 → bun_class_unwrap 读回                   */
    /* ------------------------------------------------------------------ */
    printf("\n--- Test 3: JS property mutation visible via bun_class_unwrap ---\n");
    {
        BunRuntime* rt = bun_initialize(NULL);
        BunContext* ctx = bun_context(rt);
        BunValue global = bun_global(ctx);

        BunClass* vec2_class = bun_class_register(ctx, &VEC2_CLASS, NULL);
        bun_set(ctx, global, "Vec2", 4, bun_class_constructor(ctx, vec2_class));

        char tmp[128];
        snprintf(tmp, sizeof(tmp), "/tmp/bun-ecfc-t3-%ld.mjs", (long)getpid());

        const char* src = "const v = new Vec2(0, 0);\n"
                          "v.x = 42;\n"
                          "v.y = 99;\n"
                          "globalThis.__t3_obj = v;\n"; /* 把实例暴露出来供 C 读取 */

        if (!write_tmp_module(tmp, src)) {
            FAIL("cannot write tmp module for test 3");
        } else {
            BunValue r = bun_eval_file(ctx, BUN_CSTR(tmp));
            if (r == BUN_EXCEPTION) {
                FAIL("bun_eval_file threw: %s", bun_last_error(ctx, NULL));
            } else {
                BunValue obj = bun_eval_string(ctx, BUN_LITERAL("globalThis.__t3_obj"));
                Vec2* native = (Vec2*)bun_class_unwrap(ctx, obj, vec2_class);
                if (!native) {
                    FAIL("bun_class_unwrap returned NULL");
                } else if (native->x == 42 && native->y == 99) {
                    PASS("JS mutations visible via bun_class_unwrap: x=42 y=99");
                } else {
                    FAIL("native->x=%d(want 42) native->y=%d(want 99)", native->x, native->y);
                }
            }
            unlink(tmp);
        }

        bun_destroy(rt);
    }

    /* ------------------------------------------------------------------ */
    /* Test 4: bun_eval_file → bun_eval_file (同一 runtime，两个模块)    */
    /* ------------------------------------------------------------------ */
    printf("\n--- Test 4: two bun_eval_file calls on the same runtime ---\n");
    {
        BunRuntime* rt = bun_initialize(NULL);
        BunContext* ctx = bun_context(rt);
        BunValue global = bun_global(ctx);

        BunClass* vec2_class = bun_class_register(ctx, &VEC2_CLASS, NULL);
        bun_set(ctx, global, "Vec2", 4, bun_class_constructor(ctx, vec2_class));

        char tmp1[128], tmp2[128];
        snprintf(tmp1, sizeof(tmp1), "/tmp/bun-ecfc-t4a-%ld.mjs", (long)getpid());
        snprintf(tmp2, sizeof(tmp2), "/tmp/bun-ecfc-t4b-%ld.mjs", (long)getpid());

        /* 第一个模块 */
        if (!write_tmp_module(tmp1,
                "const v1 = new Vec2(11, 22);\n"
                "globalThis.__t4_v1x = v1.x;\n")) {
            FAIL("cannot write tmp1 for test 4");
        }
        /* 第二个模块（第二次 bun_eval_file 也应正常触发 bun_class_new） */
        if (!write_tmp_module(tmp2,
                "const v2 = new Vec2(33, 44);\n"
                "globalThis.__t4_v2y = v2.y;\n")) {
            FAIL("cannot write tmp2 for test 4");
        }

        BunValue r1 = bun_eval_file(ctx, BUN_CSTR(tmp1));
        BunValue r2 = bun_eval_file(ctx, BUN_CSTR(tmp2));

        if (r1 == BUN_EXCEPTION)
            FAIL("first bun_eval_file threw: %s", bun_last_error(ctx, NULL));
        else if (r2 == BUN_EXCEPTION)
            FAIL("second bun_eval_file threw: %s", bun_last_error(ctx, NULL));
        else {
            int v1x = bun_to_int32(bun_eval_string(ctx, BUN_LITERAL("globalThis.__t4_v1x")));
            int v2y = bun_to_int32(bun_eval_string(ctx, BUN_LITERAL("globalThis.__t4_v2y")));
            if (v1x == 11 && v2y == 44)
                PASS("two sequential bun_eval_file: v1.x=11 v2.y=44");
            else
                FAIL("v1.x=%d(want 11) v2.y=%d(want 44)", v1x, v2y);
        }

        unlink(tmp1);
        unlink(tmp2);
        bun_destroy(rt);
    }

    /* ------------------------------------------------------------------ */
    /* Test 5: bun_eval_file 内部 + top-level await                       */
    /* ------------------------------------------------------------------ */
    printf("\n--- Test 5: bun_class_new inside bun_eval_file with top-level await ---\n");
    {
        BunRuntime* rt = bun_initialize(NULL);
        BunContext* ctx = bun_context(rt);
        BunValue global = bun_global(ctx);

        BunClass* vec2_class = bun_class_register(ctx, &VEC2_CLASS, NULL);
        bun_set(ctx, global, "Vec2", 4, bun_class_constructor(ctx, vec2_class));

        char tmp[128];
        snprintf(tmp, sizeof(tmp), "/tmp/bun-ecfc-t5-%ld.mjs", (long)getpid());

        const char* src = "const v = await Promise.resolve(new Vec2(7, 8));\n"
                          "globalThis.__t5_x = v.x;\n"
                          "globalThis.__t5_y = v.y;\n";

        if (!write_tmp_module(tmp, src)) {
            FAIL("cannot write tmp module for test 5");
        } else {
            BunValue r = bun_eval_file(ctx, BUN_CSTR(tmp));
            if (r == BUN_EXCEPTION) {
                FAIL("bun_eval_file threw: %s", bun_last_error(ctx, NULL));
            } else {
                int x = bun_to_int32(bun_eval_string(ctx, BUN_LITERAL("globalThis.__t5_x")));
                int y = bun_to_int32(bun_eval_string(ctx, BUN_LITERAL("globalThis.__t5_y")));
                if (x == 7 && y == 8)
                    PASS("bun_class_new with top-level await: Vec2(7,8) x=7 y=8");
                else
                    FAIL("x=%d(want 7) y=%d(want 8)", x, y);
            }
            unlink(tmp);
        }

        bun_destroy(rt);
    }

    /* ------------------------------------------------------------------ */
    /* Test 6: bun_class_new 直接在 bun_eval_string 中（对照组，应始终工作） */
    /* ------------------------------------------------------------------ */
    printf("\n--- Test 6: bun_class_new inside bun_eval_string (control group) ---\n");
    {
        BunRuntime* rt = bun_initialize(NULL);
        BunContext* ctx = bun_context(rt);
        BunValue global = bun_global(ctx);

        BunClass* vec2_class = bun_class_register(ctx, &VEC2_CLASS, NULL);
        bun_set(ctx, global, "Vec2", 4, bun_class_constructor(ctx, vec2_class));

        BunValue r = bun_eval_string(ctx,
            BUN_LITERAL("const v = new Vec2(100, 200); globalThis.__t6_sum = v.sum();"));
        if (r == BUN_EXCEPTION) {
            FAIL("bun_eval_string threw: %s", bun_last_error(ctx, NULL));
        } else {
            int sum = bun_to_int32(bun_eval_string(ctx, BUN_LITERAL("globalThis.__t6_sum")));
            if (sum == 300)
                PASS("bun_class_new in bun_eval_string: Vec2(100,200).sum()=300");
            else
                FAIL("sum=%d(want 300)", sum);
        }

        bun_destroy(rt);
    }

    /* ------------------------------------------------------------------ */
summary:
    printf("\n=== Result: %d passed, %d failed ===\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
