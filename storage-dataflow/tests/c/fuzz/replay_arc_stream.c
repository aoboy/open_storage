/*
 * Deterministic ARC stream replay (plan prompt W6.11).
 *
 * The binary format records every submit result and the exact producer
 * and receiver timestamps used, so replay never samples the wall clock.
 * Payload identities are checked byte-for-byte: accepted entries release
 * once, rejected entries never release, and QG releases in submit order.
 * The checked-in corpus includes the W5 QG park/stranding stream at fuzz
 * iteration 632 and the W6.9 saturation/backpressure sequence.
 */

#include "arc_internal.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPLAY_MAGIC "ARCSTRM1"
#define MAX_CAPACITY 64u
#define MAX_ENTRY 256u
#define MAX_OPS 512u
#define MAX_POLL_NS (UINT64_C(1) << 60)

enum {
    OP_SUBMIT = 1,
    OP_POLL = 2,
};

typedef struct {
    uint32_t id;
    size_t len;
    uint8_t bytes[MAX_ENTRY];
    int accepted;
    int released;
} replay_entry_t;

typedef struct {
    uint64_t released_ts[MAX_CAPACITY];
    uint32_t release_order[MAX_CAPACITY];
    replay_entry_t entries[MAX_CAPACITY];
    uint32_t accepted_order[MAX_CAPACITY];
    uint32_t entry_count;
    uint32_t accepted_count;
    uint32_t release_count;
    arc_mode_t mode;
} replay_ctx_t;

static int fail(const char *path, const char *what) {
    fprintf(stderr, "ARC REPLAY FAIL %s: %s\n", path, what);
    return 1;
}

static int read_u32(FILE *f, uint32_t *out) {
    uint8_t b[4];
    if (fread(b, 1, sizeof(b), f) != sizeof(b)) {
        return -1;
    }
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 0;
}

static int read_u64(FILE *f, uint64_t *out) {
    uint8_t b[8];
    if (fread(b, 1, sizeof(b), f) != sizeof(b)) {
        return -1;
    }
    *out = 0;
    for (uint32_t i = 0; i < 8; i++) {
        *out |= (uint64_t)b[i] << (8u * i);
    }
    return 0;
}

static replay_entry_t *find_entry(replay_ctx_t *ctx, uint32_t id) {
    for (uint32_t i = 0; i < ctx->entry_count; i++) {
        if (ctx->entries[i].id == id) {
            return &ctx->entries[i];
        }
    }
    return NULL;
}

static int replay_apply(void *opaque, const void *entry, size_t len, uint64_t release_ts) {
    replay_ctx_t *ctx = opaque;
    replay_entry_t *match = NULL;
    for (uint32_t i = 0; i < ctx->entry_count; i++) {
        replay_entry_t *candidate = &ctx->entries[i];
        if (!candidate->accepted || candidate->released || candidate->len != len) {
            continue;
        }
        if (memcmp(candidate->bytes, entry, len) == 0) {
            if (match != NULL) {
                return -1; /* payload identity is not unique */
            }
            match = candidate;
        }
    }
    if (match == NULL || ctx->release_count >= MAX_CAPACITY) {
        return -1;
    }
    if (ctx->mode == ARC_MODE_QG) {
        uint32_t expected = ctx->accepted_order[ctx->release_count];
        replay_entry_t *ordered = find_entry(ctx, expected);
        if (ordered == NULL || ordered != match) {
            return -1; /* QG release order differs from accepted submit order */
        }
    }
    match->released = 1;
    ctx->release_order[ctx->release_count] = match->id;
    ctx->released_ts[ctx->release_count] = release_ts;
    ctx->release_count++;
    return 0;
}

static int submit_operation(FILE *f, arc_pacer_t *p, replay_ctx_t *ctx, const char *path) {
    uint32_t id = 0;
    uint32_t len = 0;
    int32_t expected = 0;
    uint64_t producer_ts = 0;
    uint64_t arrival_ts = 0;
    if (read_u32(f, &id) != 0 || read_u32(f, &len) != 0) {
        return fail(path, "truncated submit header");
    }
    uint8_t raw_rc[4];
    if (fread(raw_rc, 1, sizeof(raw_rc), f) != sizeof(raw_rc)) {
        return fail(path, "truncated submit result");
    }
    expected = (int32_t)((uint32_t)raw_rc[0] | ((uint32_t)raw_rc[1] << 8) |
                         ((uint32_t)raw_rc[2] << 16) | ((uint32_t)raw_rc[3] << 24));
    if (read_u64(f, &producer_ts) != 0 || read_u64(f, &arrival_ts) != 0) {
        return fail(path, "truncated submit timestamps");
    }
    if (id >= MAX_CAPACITY || len > MAX_ENTRY) {
        return fail(path, "submit id or length exceeds replay bounds");
    }

    uint8_t payload[MAX_ENTRY];
    if (len > 0 && fread(payload, 1, len, f) != len) {
        return fail(path, "truncated submit payload");
    }

    replay_entry_t *existing = find_entry(ctx, id);
    if (existing != NULL) {
        if (existing->len != len || memcmp(existing->bytes, payload, len) != 0) {
            return fail(path, "repeated identity changed payload");
        }
        if (existing->accepted) {
            return fail(path, "accepted identity was submitted again");
        }
    } else {
        if (ctx->entry_count >= MAX_CAPACITY) {
            return fail(path, "too many payload identities");
        }
        existing = &ctx->entries[ctx->entry_count++];
        memset(existing, 0, sizeof(*existing));
        existing->id = id;
        existing->len = len;
        memcpy(existing->bytes, payload, len);
        for (uint32_t i = 0; i + 1 < ctx->entry_count; i++) {
            replay_entry_t *other = &ctx->entries[i];
            if (other->len == len && memcmp(other->bytes, payload, len) == 0) {
                return fail(path, "payload identities are not unique");
            }
        }
    }

    int rc = arc_submit_at(p, payload, len, producer_ts, arrival_ts);
    if (rc != expected) {
        fprintf(stderr, "ARC REPLAY FAIL %s: submit id %u returned %d, expected %d\n", path, id, rc,
                expected);
        return 1;
    }
    if (rc == 0 || rc == 1) {
        if (existing->accepted) {
            return fail(path, "one identity was accepted twice");
        }
        existing->accepted = 1;
        ctx->accepted_order[ctx->accepted_count++] = id;
    } else if (existing->accepted) {
        return fail(path, "accepted identity was later refused");
    }
    return 0;
}

static int replay_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return fail(path, "cannot open corpus");
    }

    char magic[8];
    if (fread(magic, 1, sizeof(magic), f) != sizeof(magic) ||
        memcmp(magic, REPLAY_MAGIC, sizeof(magic)) != 0) {
        fclose(f);
        return fail(path, "bad magic/version");
    }

    uint32_t capacity = 0;
    uint32_t entry_max = 0;
    uint32_t batch_max = 0;
    uint32_t mode = 0;
    if (read_u32(f, &capacity) != 0 || read_u32(f, &entry_max) != 0 ||
        read_u32(f, &batch_max) != 0 || read_u32(f, &mode) != 0) {
        fclose(f);
        return fail(path, "truncated configuration");
    }
    if (capacity < 2 || capacity > MAX_CAPACITY || (capacity & (capacity - 1u)) != 0 ||
        entry_max == 0 || entry_max > MAX_ENTRY || batch_max == 0 || batch_max > MAX_CAPACITY ||
        mode > ARC_MODE_IMMEDIATE) {
        fclose(f);
        return fail(path, "configuration exceeds replay bounds");
    }

    uint64_t min_smooth = 0;
    uint64_t U = 0;
    uint64_t delta = 0;
    uint64_t gamma = 0;
    uint64_t G = 0;
    uint64_t T_idle = 0;
    uint64_t batch_deadline = 0;
    uint64_t seed = 0;
    if (read_u64(f, &min_smooth) != 0 || read_u64(f, &U) != 0 || read_u64(f, &delta) != 0 ||
        read_u64(f, &gamma) != 0 || read_u64(f, &G) != 0 || read_u64(f, &T_idle) != 0 ||
        read_u64(f, &batch_deadline) != 0 || read_u64(f, &seed) != 0) {
        fclose(f);
        return fail(path, "truncated scheduling parameters");
    }
    uint32_t op_count = 0;
    uint32_t expected_releases = 0;
    if (read_u32(f, &op_count) != 0 || read_u32(f, &expected_releases) != 0) {
        fclose(f);
        return fail(path, "truncated operation counts");
    }
    if (op_count == 0 || op_count > MAX_OPS || expected_releases > MAX_CAPACITY) {
        fclose(f);
        return fail(path, "operation counts exceed replay bounds");
    }

    arc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.capacity = capacity;
    cfg.entry_max = entry_max;
    cfg.mode = (arc_mode_t)mode;
    cfg.min_smooth_bytes = min_smooth;
    cfg.U = U;
    cfg.delta = delta;
    cfg.gamma = gamma;
    cfg.G = G;
    cfg.T_idle_ns = T_idle;
    cfg.batch_max = batch_max;
    cfg.batch_deadline_ns = batch_deadline;

    size_t sz = arc_sizeof(&cfg);
    if (sz == 0) {
        fclose(f);
        return fail(path, "arc_sizeof rejected configuration");
    }
    arc_pacer_t *pacer = malloc(sz);
    if (pacer == NULL) {
        fclose(f);
        return fail(path, "pacer allocation failed");
    }
    replay_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        free(pacer);
        fclose(f);
        return fail(path, "replay state allocation failed");
    }
    ctx->mode = cfg.mode;
    if (arc_init(pacer, &cfg, replay_apply, ctx) != 0) {
        free(ctx);
        free(pacer);
        fclose(f);
        return fail(path, "arc_init rejected configuration");
    }

    int result = 0;
    for (uint32_t op = 0; op < op_count && result == 0; op++) {
        uint8_t tag = 0;
        if (fread(&tag, 1, 1, f) != 1) {
            result = fail(path, "truncated operation tag");
            break;
        }
        if (tag == OP_SUBMIT) {
            result = submit_operation(f, pacer, ctx, path);
        } else if (tag == OP_POLL) {
            uint64_t now = 0;
            if (read_u64(f, &now) != 0 || now > MAX_POLL_NS) {
                result = fail(path, "invalid poll timestamp");
                break;
            }
            if (arc_poll(pacer, now) < 0) {
                result = fail(path, "arc_poll returned an error");
            }
        } else {
            result = fail(path, "unknown operation tag");
        }
    }

    if (result == 0) {
        uint8_t trailing = 0;
        if (fread(&trailing, 1, 1, f) != 0 || ferror(f)) {
            result = fail(path, "trailing bytes after operations");
        }
    }
    fclose(f);

    if (result == 0 &&
        (ctx->accepted_count != expected_releases || ctx->release_count != expected_releases)) {
        result = fail(path, "release count differs from recorded expectation");
    }
    arc_stats_t stats = arc_stats(pacer);
    if (result == 0 &&
        (stats.releases != expected_releases || stats.in_flight != 0 || stats.heap_depth != 0)) {
        result = fail(path, "entries remain in flight after final poll");
    }
    if (result == 0 &&
        (!isfinite(stats.D) || !isfinite(stats.E) || stats.D < 0.0 || stats.E < stats.D - 1e-9)) {
        result = fail(path, "ARRIVAL estimator is not finite and ordered");
    }
    for (uint32_t i = 0; result == 0 && i < ctx->entry_count; i++) {
        replay_entry_t *entry = &ctx->entries[i];
        if (entry->accepted != entry->released) {
            result = fail(path, "accepted payload did not release exactly once");
        }
    }

    if (result == 0) {
        printf("arc_replay: ok (%s, releases=%u, ops=%u, seed=%llu)\n", path, expected_releases,
               op_count, (unsigned long long)seed);
    }
    free(ctx);
    free(pacer);
    return result;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s CORPUS...\n", argv[0]);
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        if (replay_file(argv[i]) != 0) {
            return 1;
        }
    }
    return 0;
}
