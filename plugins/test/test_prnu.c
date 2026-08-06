#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Include the C file directly so we can test the static functions */
#include "../analyzer_prnu.c"

#define ASSERT_FLOAT_EQ(expected, actual, msg) \
    do { \
        if (fabs((expected) - (actual)) > 1e-5) { \
            fprintf(stderr, "FAIL: %s - expected %f, got %f (line %d)\n", msg, (double)(expected), (double)(actual), __LINE__); \
            exit(1); \
        } \
    } while (0)

static void test_boxblur2d_uniform() {
    int w = 10, h = 10;
    float *src = calloc((size_t)w * h, sizeof(float));
    float *dst = calloc((size_t)w * h, sizeof(float));
    float *tmp = calloc((size_t)w * h, sizeof(float));

    for (int i = 0; i < w * h; i++) {
        src[i] = 1.0f;
    }

    boxblur2d(src, dst, tmp, w, h);

    for (int i = 0; i < w * h; i++) {
        ASSERT_FLOAT_EQ(1.0f, dst[i], "uniform field should remain unchanged");
    }

    free(src);
    free(dst);
    free(tmp);
}

static void test_boxblur2d_impulse() {
    int w = 10, h = 10;
    float *src = calloc((size_t)w * h, sizeof(float));
    float *dst = calloc((size_t)w * h, sizeof(float));
    float *tmp = calloc((size_t)w * h, sizeof(float));

    /* Area of the blur kernel is (2 * RADIUS + 1)^2 = 9x9 = 81 */
    src[5 * w + 5] = 81.0f;

    boxblur2d(src, dst, tmp, w, h);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            /* The impulse is at (5, 5). With RADIUS=4, the blur spans [1, 9] in x and y. */
            if (x >= 1 && x <= 9 && y >= 1 && y <= 9) {
                ASSERT_FLOAT_EQ(1.0f, dst[y * w + x], "impulse response inside kernel");
            } else {
                ASSERT_FLOAT_EQ(0.0f, dst[y * w + x], "impulse response outside kernel");
            }
        }
    }

    free(src);
    free(dst);
    free(tmp);
}

static void test_boxblur2d_edge_clamp() {
    int w = 10, h = 10;
    float *src = calloc((size_t)w * h, sizeof(float));
    float *dst = calloc((size_t)w * h, sizeof(float));
    float *tmp = calloc((size_t)w * h, sizeof(float));

    src[0] = 81.0f;

    boxblur2d(src, dst, tmp, w, h);

    /*
     * Horizontal pass at (0,0): it clamps negative indices to 0.
     * k in [-4, 4], x=0. xx is 0 for k=-4,-3,-2,-1,0 (5 times).
     * Sum = 5 * 81.0 = 405.0. tmp[0] = 405.0 / 9 = 45.0.
     * Vertical pass at (0,0): it clamps negative indices to 0.
     * yy is 0 for k=-4,-3,-2,-1,0 (5 times).
     * Sum = 5 * tmp[0] = 5 * 45.0 = 225.0. dst[0] = 225.0 / 9 = 25.0.
     */
    ASSERT_FLOAT_EQ(25.0f, dst[0], "edge clamp corner");

    free(src);
    free(dst);
    free(tmp);
}

/*
 * The mirror of the case above, and it is not decoration: with only the (0,0)
 * corner asserted, dropping the HIGH clamp (xx >= w -> w-1, yy >= h -> h-1)
 * survives, because in a field that is zero everywhere else, wrapping to
 * column 0 reads the same zero that clamping to column 9 reads. Both mutants
 * die here.
 */
static void test_boxblur2d_edge_clamp_high() {
    int w = 10, h = 10;
    float *src = calloc((size_t)w * h, sizeof(float));
    float *dst = calloc((size_t)w * h, sizeof(float));
    float *tmp = calloc((size_t)w * h, sizeof(float));

    src[(h - 1) * w + (w - 1)] = 81.0f;

    boxblur2d(src, dst, tmp, w, h);

    /* xx clamps down to w-1 five times -> 5*81/9 = 45, then yy to h-1 five
     * times -> 5*45/9 = 25. The same 25 as the near corner, by symmetry. */
    ASSERT_FLOAT_EQ(25.0f, dst[(h - 1) * w + (w - 1)], "edge clamp far corner");

    free(src);
    free(dst);
    free(tmp);
}

int main() {
    test_boxblur2d_uniform();
    test_boxblur2d_impulse();
    test_boxblur2d_edge_clamp();
    test_boxblur2d_edge_clamp_high();

    printf("PASS\n");
    return 0;
}
