/*
 * Install-leg hello world (plan prompt W5.2): links ONLY against the
 * installed pkg-config surface, exercising the public API the way an
 * external consumer would — version macro, WM dispatch, and one ARC
 * schedule. This file deliberately lives outside the CMake build: the
 * CI leg compiles it with $(pkg-config --cflags --libs sdf_wm sdf_arc)
 * against DESTDIR.
 */

#include "sdf/version.h"
#include "sdf/wm.h"
#include "sdf/arc.h"

#include <stdio.h>
#include <stdlib.h>

int main(void) {
    if (!SDF_VERSION_AT_LEAST(0, 2, 0)) {
        fprintf(stderr, "install-hello: version macro older than expected\n");
        return 1;
    }

    /* WM: compile a two-path profile and select across a window */
    wm_profile_t p;
    wm_profile_t zero = {0};
    p = zero;
    p.version = 1;
    p.m = 1024;
    p.n = 2;
    p.b[0] = 512;
    p.b[1] = 512;
    wm_compiled_t c;
    if (wm_profile_compile(&p, &c) != 0) {
        fprintf(stderr, "install-hello: wm compile failed\n");
        return 1;
    }
    unsigned counts[2] = {0, 0};
    for (unsigned i = 0; i < 1024; i++)
        counts[wm_select(&c, i)]++;
    if (counts[0] != 512 || counts[1] != 512) {
        fprintf(stderr, "install-hello: wm select counts wrong\n");
        return 1;
    }

    /* ARC: one entry schedules and releases */
    arc_cfg_t cfg;
    arc_cfg_t zcfg = {0};
    cfg = zcfg;
    cfg.capacity = 8;
    cfg.entry_max = 64;
    cfg.mode = ARC_MODE_BASE;
    size_t sz = arc_sizeof(&cfg);
    if (sz == 0) {
        fprintf(stderr, "install-hello: arc size failed\n");
        return 1;
    }
    void *pacer = malloc(sz);
    if (pacer == NULL || arc_init(pacer, &cfg, NULL, NULL) != 0) {
        fprintf(stderr, "install-hello: arc init failed\n");
        return 1;
    }
    char entry[64] = {1};
    if (arc_submit(pacer, entry, sizeof(entry), 0) != 0) {
        fprintf(stderr, "install-hello: arc submit failed\n");
        return 1;
    }
    (void)arc_poll(pacer, ~0ull >> 2);
    arc_stats_t st = arc_stats(pacer);
    if (st.releases != 1) {
        fprintf(stderr, "install-hello: arc release count wrong\n");
        return 1;
    }
    free(pacer);

    printf("install-hello: OK (sdf %s: wm dispatch + arc release)\n",
           SDF_VERSION_STRING);
    return 0;
}
