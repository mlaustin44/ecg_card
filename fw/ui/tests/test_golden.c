/* Golden-image regression: FNV-1a hashes of key frames. When a change is
 * intended, look at the new images (cmake --build <dir> --target
 * ui_render_images) and then refresh the table:
 *     UI_GOLDEN_PRINT=1 ./test_ui_golden                                   */
#include <stdlib.h>
#include <string.h>

#include "scenarios.h"
#include "test.h"

typedef struct {
    const char *name;
    uint32_t hash;
} golden_t;

#include "golden.h"

static uint32_t fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--)
        h = (h ^ *p++) * 16777619u;
    return h;
}

int main(void)
{
    bool print = getenv("UI_GOLDEN_PRINT") != NULL;
    uint8_t fb[OLED_FB_BYTES];
    int n = (int)(sizeof golden / sizeof golden[0]);
    for (int i = 0; i < n; i++) {
        const scenario_t *s = scenario_find(golden[i].name);
        CHECK(s != NULL);
        if (!s)
            continue;
        ui_model_t m;
        s->setup(&m);
        ui_render(&m, fb);
        uint32_t h = fnv1a(fb, sizeof fb);
        if (print)
            printf("    {\"%s\", 0x%08Xu},\n", golden[i].name, (unsigned)h);
        else if (h != golden[i].hash)
            printf("  %s: frame changed (0x%08X, golden 0x%08X)\n", golden[i].name, (unsigned)h,
                   (unsigned)golden[i].hash);
        if (!print)
            CHECK_EQ(h, golden[i].hash);
    }
    return TEST_RESULT();
}
