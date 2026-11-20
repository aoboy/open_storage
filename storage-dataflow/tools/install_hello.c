/*
 * Install-leg hello world (plan prompt W5.2): links ONLY against the
 * installed pkg-config surface, exercising the public API the way an
 * external consumer would — version macro, WM dispatch, and one ARC
 * schedule. This file deliberately lives outside the CMake build: the
 * CI leg compiles it with $(pkg-config --cflags --libs sdf_wm sdf_arc)
 * against DESTDIR.
 */

#include "sdf/arc.h"
#include "sdf/common/config.h"
#include "sdf/origin/config.h"
#include "sdf/origin/http.h"
#include "sdf/version.h"
#include "sdf/wm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    if (!SDF_VERSION_AT_LEAST(1, 0, 0)) {
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
    for (unsigned i = 0; i < 1024; i++) {
        counts[wm_select(&c, i)]++;
    }
    if (counts[0] != 512 || counts[1] != 512) {
        fprintf(stderr, "install-hello: wm select counts wrong\n");
        return 1;
    }

    /* Origin seam: materialize a strict [[origin]] entry and select its
     * driver without doing network I/O. This proves the registry and both
     * driver archives are actually extracted from the installed surface. */
    sdf_config_entry_t entries[2];
    memset(entries, 0, sizeof(entries));
    snprintf(entries[0].key, sizeof(entries[0].key), "origin[0].cls");
    snprintf(entries[0].value, sizeof(entries[0].value), "http");
    snprintf(entries[1].key, sizeof(entries[1].key), "origin[0].dst");
    snprintf(entries[1].value, sizeof(entries[1].value), "http://installed.example/obj");
    sdf_config_t config = {.entries = entries, .count = 2, .capacity = 2};
    sdf_origin_cfg_t origin;
    uint32_t n_origins = 0;
    if (sdf_origin_config_load(&config, &origin, 1, &n_origins) != 0 || n_origins != 1) {
        fprintf(stderr, "install-hello: origin config load failed\n");
        return 1;
    }
    const sdf_origin_ops_t *origin_ops = NULL;
    sdf_origin_select_status_t select_status = SDF_ORIGIN_SELECT_INVALID;
    if (sdf_origin_select(&origin, 1, &origin_ops, &select_status) != 0 ||
        select_status != SDF_ORIGIN_SELECT_OK || origin_ops != sdf_http_ops()) {
        fprintf(stderr, "install-hello: origin selection failed\n");
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

    printf("install-hello: OK (sdf %s: origin select + wm dispatch + arc release)\n",
           SDF_VERSION_STRING);
    return 0;
}
