#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* The purpose of this test is to verify the internal static function `fill_lut`
   in display_falsecolor.c, which maps a normalized value to a color using the
   VIRIDIS colormap. We #include the file directly to access its static data
   and functions, in keeping with the repository's pattern for testing plugins
   (e.g., test_prnu.c).
*/

#include "../display_falsecolor.c"

int main() {
    // 1. Test the fast path: exactly 256 entries
    uint8_t rgb256[256 * 3];
    memset(rgb256, 0, sizeof(rgb256));
    fill_lut(rgb256, 256);
    if (memcmp(rgb256, VIRIDIS, sizeof(VIRIDIS)) != 0) {
        printf("FAIL: 256 entries mismatch\n");
        return 1;
    }

    // 2. Test 1 entry (edge case for division by zero)
    uint8_t rgb1[3] = {0, 0, 0};
    fill_lut(rgb1, 1);
    if (rgb1[0] != VIRIDIS[0] || rgb1[1] != VIRIDIS[1] || rgb1[2] != VIRIDIS[2]) {
        printf("FAIL: 1 entry mismatch\n");
        return 1;
    }

    // 3. The resample path, at 10 entries. EVERY entry is asserted, not just
    // the ends: the ends are where the mapping is pinned by construction
    // (i=0 -> 0, i=9 -> 255) and so they are exactly where a wrong formula
    // still looks right. An earlier draft asserted indices 0, 1 and 9 only,
    // and a mutant that rounds to nearest instead of truncating - s =
    // (i*255 + 4)/9 - agreed at all three and survived; it disagrees at
    // i = 2, 5 and 8 (56/57, 141/142, 226/227), which is the interior.
    // The expected index is written out here rather than reusing the
    // expression under test, so a change to that expression is a mismatch
    // and not a matching pair of edits.
    uint8_t rgb10[10 * 3];
    memset(rgb10, 0, sizeof(rgb10));
    fill_lut(rgb10, 10);

    for (uint32_t i = 0; i < 10; i++) {
        uint32_t s = (i * 255u) / 9u;
        if (rgb10[i*3] != VIRIDIS[s*3] ||
            rgb10[i*3+1] != VIRIDIS[s*3+1] ||
            rgb10[i*3+2] != VIRIDIS[s*3+2]) {
            printf("FAIL: 10 entries [%u]\n", i);
            return 1;
        }
    }

    printf("PASS\n");
    return 0;
}
