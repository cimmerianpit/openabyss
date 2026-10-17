/* SPDX-License-Identifier: MIT */
#include "uw_lighting.h"
#include <string.h>

bool uw_light_load(uw_light *l, const char *path) {
    uw_blob b = uw_read_file(path);
    if (!b.data) return false;
    /* Exactly 0x1000: shade_set_level reads that many bytes and nothing
     * bounds-checks the file, so a short one would be read past. The row
     * width of 256 is not a guess either -- shade_reload_or_scramble walks
     * `for (i = 0; i < 0x10; i++)` zeroing `[i * 0x100]` and `[i*0x100+1]`. */
    bool ok = b.size == UW_LIGHT_SIZE;
    if (ok) memcpy(l->ramp, b.data, UW_LIGHT_SIZE);
    uw_free(&b);
    return ok;
}

int uw_light_identity_count(const uw_light *l, int row) {
    int n = 0;
    for (int i = 0; i < UW_LIGHT_ROW; i++)
        n += l->ramp[row][i] == i;
    return n;
}

int uw_light_distinct(const uw_light *l, int row) {
    bool seen[UW_LIGHT_ROW] = {0};
    int n = 0;
    for (int i = 0; i < UW_LIGHT_ROW; i++) {
        uint8_t v = l->ramp[row][i];
        if (!seen[v]) { seen[v] = true; n++; }
    }
    return n;
}

bool uw_shades_load(uw_shades *s, const char *path) {
    uw_blob b = uw_read_file(path);
    if (!b.data) return false;
    bool ok = b.size == UW_SHADE_LEVELS * UW_SHADE_WORDS * 2;
    if (ok)
        for (int i = 0; i < UW_SHADE_LEVELS; i++)
            for (int j = 0; j < UW_SHADE_WORDS; j++)
                s->w[i][j] = (int16_t)uw_u16(b.data + (i * UW_SHADE_WORDS + j) * 2);
    uw_free(&b);
    return ok;
}
