/// test_array_range.c — Regression tests for bun_array_get_range / bun_array_set_range.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bun_embed.h"

#define BUN_LITERAL(str) (str), sizeof(str) - 1

#define PASS(msg) (printf("[PASS] %s\n", (msg)), passed++)
#define FAIL(msg, ...) (fprintf(stderr, "[FAIL] " msg "\n", ##__VA_ARGS__), failed++)

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    int passed = 0;
    int failed = 0;

    BunRuntime* runtime = bun_initialize(NULL);
    BunContext* ctx = runtime ? bun_context(runtime) : NULL;
    if (!runtime || !ctx) {
        fprintf(stderr, "[FAIL] unable to initialize Bun runtime\n");
        bun_destroy(runtime);
        return 1;
    }

    printf("=== test_array_range ===\n\n");

    {
        BunValue array = bun_array(ctx, 4);
        BunValue write_values[4] = {
            bun_int32(10),
            bun_int32(20),
            bun_int32(30),
            bun_int32(40),
        };
        BunValue read_values[2] = { BUN_UNDEFINED, BUN_UNDEFINED };

        if (!bun_array_set_range(ctx, array, 0, 4, write_values)) {
            FAIL("bun_array_set_range(dense) failed: %s", bun_last_error(ctx, NULL));
        } else if (!bun_array_get_range(ctx, array, 1, 2, read_values)) {
            FAIL("bun_array_get_range(dense) failed: %s", bun_last_error(ctx, NULL));
        } else if (bun_to_int32(read_values[0]) == 20 && bun_to_int32(read_values[1]) == 30) {
            PASS("bun_array_get_range/bun_array_set_range handle dense ranges");
        } else {
            FAIL("dense range read returned [%d, %d] (expected [20, 30])",
                bun_to_int32(read_values[0]),
                bun_to_int32(read_values[1]));
        }
    }

    {
        BunValue array = bun_array(ctx, 2);
        BunValue initial_values[2] = { bun_int32(1), bun_int32(2) };
        BunValue overflow_values[2] = { bun_int32(9), bun_int32(10) };

        if (!bun_array_set_range(ctx, array, 0, 2, initial_values)) {
            FAIL("bun_array_set_range(initial) failed: %s", bun_last_error(ctx, NULL));
        } else if (bun_array_set_range(ctx, array, 1, 2, overflow_values)) {
            FAIL("bun_array_set_range(no-growth) unexpectedly succeeded");
        } else {
            const char* err = bun_last_error(ctx, NULL);
            BunValue slot = bun_get_index(ctx, array, 1);
            if (err && bun_to_int32(slot) == 2) {
                PASS("bun_array_set_range rejects out-of-bounds writes before mutating");
            } else {
                FAIL("bun_array_set_range(no-growth) left slot[1]=%d err=%s",
                    bun_to_int32(slot),
                    err ? err : "(null)");
            }
        }
    }

    {
        BunValue array = bun_array(ctx, 3);
        if (bun_array_get_range(ctx, array, 3, 0, NULL) && bun_array_set_range(ctx, array, 99, 0, NULL)) {
            PASS("zero-count range operations succeed with NULL buffers");
        } else {
            FAIL("zero-count range operations should succeed");
        }
    }

    {
        BunValue prototype_array = bun_eval_string(
            ctx,
            BUN_LITERAL(
                "(() => {"
                "  globalThis.__embed_array_range_getter_hits = 0;"
                "  const proto = Object.create(Array.prototype);"
                "  Object.defineProperty(proto, '1', {"
                "    get() {"
                "      globalThis.__embed_array_range_getter_hits++;"
                "      return 99;"
                "    },"
                "    configurable: true"
                "  });"
                "  const arr = new Array(2);"
                "  Object.setPrototypeOf(arr, proto);"
                "  return arr;"
                "})()"));

        if (prototype_array == BUN_EXCEPTION) {
            FAIL("creating prototype-backed array failed: %s", bun_last_error(ctx, NULL));
        } else {
            BunValue range_values[2] = { BUN_UNDEFINED, BUN_UNDEFINED };
            if (!bun_array_get_range(ctx, prototype_array, 0, 2, range_values)) {
                FAIL("bun_array_get_range(prototype getter) failed: %s", bun_last_error(ctx, NULL));
            } else {
                BunValue getter_hits = bun_eval_string(ctx, BUN_LITERAL("globalThis.__embed_array_range_getter_hits"));
                if (bun_is_undefined(range_values[0]) && bun_to_int32(range_values[1]) == 99 && getter_hits != BUN_EXCEPTION && bun_to_int32(getter_hits) == 1) {
                    PASS("bun_array_get_range preserves holes and prototype getters");
                } else {
                    FAIL("prototype getter range read returned unexpected values");
                }
            }
        }
    }

    {
        BunValue non_array = bun_object(ctx);
        BunValue range_values[1] = { BUN_UNDEFINED };
        if (bun_array_get_range(ctx, non_array, 0, 1, range_values)) {
            FAIL("bun_array_get_range(non-array) unexpectedly succeeded");
        } else if (bun_last_error(ctx, NULL)) {
            PASS("bun_array_get_range rejects non-array targets");
        } else {
            FAIL("bun_array_get_range(non-array) did not set bun_last_error");
        }
    }

    {
        BunValue frozen = bun_eval_string(ctx, BUN_LITERAL("Object.freeze([1, 2])"));
        BunValue value = bun_int32(7);
        if (frozen == BUN_EXCEPTION) {
            FAIL("creating frozen array failed: %s", bun_last_error(ctx, NULL));
        } else if (bun_array_set_range(ctx, frozen, 0, 1, &value)) {
            FAIL("bun_array_set_range(frozen array) unexpectedly succeeded");
        } else if (bun_last_error(ctx, NULL)) {
            PASS("bun_array_set_range surfaces JS write failures");
        } else {
            FAIL("bun_array_set_range(frozen array) did not set bun_last_error");
        }
    }

    bun_destroy(runtime);

    printf("\n=== Result: %d passed, %d failed ===\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
