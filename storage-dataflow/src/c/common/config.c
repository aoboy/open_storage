/*
 * Strict TOML loader — plan prompt W0.3, ADR-0005.
 *
 * Parser core adapted from wack_a_mole: src/config/config.c @ a0f0422
 * (plan K.2, ~1.4 kloc, hardened by tests/test_config.c there): the
 * tokenizer, table/array headers, value grammar, duplicate detection
 * and line-accurate diagnostics are the proven code. The schema layer
 * is this repository's own: the sdf key table below validates types,
 * ranges, enums and required keys at load time, then entries convert
 * to the flat "table.key" / "table[i].key" contract of
 * include/sdf/common/config.h. Errors are reported through
 * sdf/common/log.h (the sdf API has no error out-param).
 *
 * Authorized by plan prompt W0.3 and ADR-0009 (provenance + parity).
 *
 * Supported: [table], [[array of tables]], bare keys, basic strings
 * with \\ \" \n \t \r escapes, decimals, floats, booleans, homogeneous
 * numeric arrays (multi-line allowed), '#' comments, and ';' as an
 * optional key separator. Deliberately rejected: dotted keys, literal
 * strings, hex/oct/bin, datetimes, inline tables, string arrays. A
 * subset that errors loudly beats a permissive parser that mis-reads
 * ops files.
 */

#include "sdf/common/config.h"

#include "sdf/common/log.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- parser core (wack_a_mole plan K.2) ------------------------------ */

/* TOML value kinds for entry_t.type */
typedef enum { V_STR, V_INT, V_FLOAT, V_BOOL, V_ARR } vtype_t;

/* One parsed TOML key=value, array-slot aware */
typedef struct {
    char table[32]; /* "" = document root */
    int arr_idx;    /* kth [[table]] occurrence; 0 for [table] */
    char key[48];
    int line;
    vtype_t type;
    char *sval;
    int64_t ival;
    double fval;
    int bval;
    double nums[64];
    int n_nums;
} entry_t;

/* [table] or [[table]] occurrence header */
typedef struct {
    char name[32];
    int arr_idx;
} hdr_t;

/* Flat parse result: entries + array-table headers */
typedef struct {
    entry_t *e;
    size_t n, cap;
    hdr_t *h;
    size_t nh, hcap;
} doc_t;

/* Parser cursor with line-accurate error buffer */
typedef struct {
    const char *p, *end, *path;
    int line;
    char *err;
    size_t errsz;
    char table[32];
    int arr_idx;
} ps_t;

static int fail(ps_t *s, int line, const char *msg) {
    if (s->err && s->errsz)
        snprintf(s->err, s->errsz, "%s:%d: %s", s->path ? s->path : "?", line, msg);
    return -1;
}

static int failf(char *err, size_t errsz, const char *path, int line,
                 const char *what, const char *msg) {
    if (err && errsz)
        snprintf(err, errsz, "%s:%d: %s: %s", path ? path : "?", line, what, msg);
    return -1;
}

static void doc_free(doc_t *d) {
    for (size_t i = 0; i < d->n; i++)
        free(d->e[i].sval);
    free(d->e);
    free(d->h);
    memset(d, 0, sizeof(*d));
}

static int doc_add_hdr(doc_t *d, const char *name, int arr_idx) {
    if (d->nh == d->hcap) {
        size_t nc = d->hcap ? d->hcap * 2 : 8;
        hdr_t *h = realloc(d->h, nc * sizeof(*h));
        if (!h)
            return -1;
        d->h = h;
        d->hcap = nc;
    }
    snprintf(d->h[d->nh].name, sizeof(d->h[d->nh].name), "%s", name);
    d->h[d->nh].arr_idx = arr_idx;
    d->nh++;
    return 0;
}

static entry_t *doc_add_entry(doc_t *d) {
    if (d->n == d->cap) {
        size_t nc = d->cap ? d->cap * 2 : 32;
        entry_t *e = realloc(d->e, nc * sizeof(*e));
        if (!e)
            return NULL;
        d->e = e;
        d->cap = nc;
    }
    memset(&d->e[d->n], 0, sizeof(d->e[0]));
    return &d->e[d->n++];
}

/* ---- scanner helpers ------------------------------------------------- */

static int at_end(ps_t *s) { return s->p >= s->end; }
static char peek(ps_t *s) { return at_end(s) ? '\0' : *s->p; }

static void skip_inline_ws(ps_t *s) {
    while (!at_end(s) && (*s->p == ' ' || *s->p == '\t'))
        s->p++;
}

static void skip_blank(ps_t *s) { /* whitespace, comments, newlines */
    for (;;) {
        if (at_end(s))
            return;
        if (*s->p == '\n') {
            s->p++;
            s->line++;
            continue;
        }
        if (*s->p == '\r') {
            s->p++;
            continue;
        }
        if (*s->p == ' ' || *s->p == '\t') {
            s->p++;
            continue;
        }
        if (*s->p == '#') {
            while (!at_end(s) && *s->p != '\n')
                s->p++;
            continue;
        }
        return;
    }
}

static int is_key_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static int parse_bare_key(ps_t *s, char *out, size_t osz) {
    const char *start = s->p;
    while (!at_end(s) && is_key_char(*s->p))
        s->p++;
    size_t len = (size_t)(s->p - start);
    if (len == 0)
        return fail(s, s->line, "expected a key");
    if (len >= osz)
        return fail(s, s->line, "key too long");
    memcpy(out, start, len);
    out[len] = '\0';
    return 0;
}

static int parse_string(ps_t *s, char **out) {
    int line = s->line;
    if (s->p + 2 <= s->end && s->p[0] == '"' && s->p[1] == '"')
        return fail(s, line, "multi-line strings are not supported");
    s->p++; /* opening quote */
    size_t cap = 64, len = 0;
    char *buf = malloc(cap);
    if (!buf)
        return fail(s, line, "oom");
    while (!at_end(s) && *s->p != '"') {
        char c = *s->p;
        if (c == '\n')
            goto unterminated;
        if (c == '\\') {
            s->p++;
            if (at_end(s))
                goto unterminated;
            switch (*s->p) {
            case '\\':
                c = '\\';
                break;
            case '"':
                c = '"';
                break;
            case 'n':
                c = '\n';
                break;
            case 't':
                c = '\t';
                break;
            case 'r':
                c = '\r';
                break;
            default:
                free(buf);
                return fail(s, line, "unsupported escape (use \\\\ \\\" \\n \\t \\r)");
            }
            s->p++;
        } else {
            s->p++;
        }
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                return fail(s, line, "oom");
            }
            buf = nb;
        }
        buf[len++] = c;
        continue;
    unterminated:
        free(buf);
        return fail(s, line, "unterminated string");
    }
    if (at_end(s)) {
        free(buf);
        return fail(s, line, "unterminated string");
    }
    s->p++; /* closing quote */
    buf[len] = '\0';
    *out = buf;
    return 0;
}

static int word_boundary(char c) {
    return c == '\0' || c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == ',' || c == ']' || c == '#' || c == ';';
}

static int parse_value(ps_t *s, entry_t *e);

static int parse_array(ps_t *s, entry_t *e) {
    int line = s->line;
    s->p++; /* '[' */
    e->type = V_ARR;
    e->n_nums = 0;
    for (;;) {
        skip_blank(s);
        if (at_end(s))
            return fail(s, line, "unterminated array");
        if (*s->p == ']') {
            s->p++;
            return 0;
        }
        if (e->n_nums >= (int)(sizeof(e->nums) / sizeof(e->nums[0])))
            return fail(s, s->line, "array too long (max 64)");
        entry_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        if (parse_value(s, &tmp) != 0)
            return -1;
        if (tmp.type != V_INT && tmp.type != V_FLOAT) {
            free(tmp.sval);
            return fail(s, line, "only numeric arrays are supported");
        }
        e->nums[e->n_nums++] = tmp.type == V_INT ? (double)tmp.ival : tmp.fval;
        skip_blank(s);
        if (at_end(s))
            return fail(s, line, "unterminated array");
        if (*s->p == ',') {
            s->p++;
            continue;
        }
        if (*s->p == ']') {
            s->p++;
            return 0;
        }
        return fail(s, s->line, "expected ',' or ']' in array");
    }
}

static int parse_value(ps_t *s, entry_t *e) {
    int line = s->line;
    e->line = line;
    if (at_end(s))
        return fail(s, line, "expected a value");
    char c = *s->p;
    if (c == '\n' || c == '\r' || c == '#' || c == ';')
        return fail(s, line, "expected a value");
    if (c == '"') {
        e->type = V_STR;
        return parse_string(s, &e->sval);
    }
    if (c == '[')
        return parse_array(s, e);
    if (strncmp(s->p, "true", 4) == 0 && word_boundary(s->p[4])) {
        s->p += 4;
        e->type = V_BOOL;
        e->bval = 1;
        return 0;
    }
    if (strncmp(s->p, "false", 5) == 0 && word_boundary(s->p[5])) {
        s->p += 5;
        e->type = V_BOOL;
        e->bval = 0;
        return 0;
    }
    if (c == '-' || c == '+' || (c >= '0' && c <= '9')) {
        const char *start = s->p;
        if (c == '-' || c == '+')
            s->p++;
        int digits = 0, dot = 0, expo = 0;
        while (!at_end(s)) {
            char d = *s->p;
            if (d >= '0' && d <= '9') {
                digits++;
                s->p++;
            } else if (d == '.' && !dot && !expo) {
                dot = 1;
                s->p++;
            } else if ((d == 'e' || d == 'E') && !expo && digits) {
                expo = 1;
                s->p++;
                if (!at_end(s) && (*s->p == '+' || *s->p == '-'))
                    s->p++;
            } else {
                break;
            }
        }
        if (digits == 0 || !word_boundary(peek(s)))
            return fail(s, line, "malformed number");
        char tmpbuf[64];
        size_t tlen = (size_t)(s->p - start);
        if (tlen >= sizeof(tmpbuf))
            return fail(s, line, "number too long");
        memcpy(tmpbuf, start, tlen);
        tmpbuf[tlen] = '\0';
        if (dot || expo) {
            char *end = NULL;
            e->fval = strtod(tmpbuf, &end);
            if (!end || *end != '\0')
                return fail(s, line, "malformed float");
            e->type = V_FLOAT;
        } else {
            errno = 0;
            char *end = NULL;
            e->ival = strtoll(tmpbuf, &end, 10);
            if (!end || *end != '\0')
                return fail(s, line, "malformed integer");
            e->type = V_INT;
        }
        return 0;
    }
    return fail(s, line, "unsupported value (string/number/bool/array only)");
}

static int parse_header(ps_t *s, doc_t *d) {
    int line = s->line;
    s->p++; /* '[' */
    int dbl = 0;
    if (peek(s) == '[') {
        dbl = 1;
        s->p++;
    }
    skip_inline_ws(s);
    char name[32];
    if (parse_bare_key(s, name, sizeof(name)) != 0)
        return -1;
    skip_inline_ws(s);
    if (peek(s) != ']')
        return fail(s, line, "expected ']'");
    s->p++;
    if (dbl) {
        if (peek(s) != ']')
            return fail(s, line, "expected ']]'");
        s->p++;
    }
    /* uniqueness: [x] twice is an error; [[x]] indices count up */
    int idx = 0;
    for (size_t i = 0; i < d->nh; i++) {
        if (strcmp(d->h[i].name, name) == 0) {
            if (!dbl)
                return fail(s, line, "table redefined");
            idx++;
        }
    }
    if (doc_add_hdr(d, name, idx) != 0)
        return fail(s, line, "oom");
    snprintf(s->table, sizeof(s->table), "%s", name);
    s->arr_idx = idx;
    return 0;
}

static int toml_parse(char *buf, size_t len, const char *path, doc_t *d,
                      char *err, size_t errsz) {
    ps_t s;
    memset(&s, 0, sizeof(s));
    s.p = buf;
    s.end = buf + len;
    s.path = path;
    s.line = 1;
    s.err = err;
    s.errsz = errsz;
    s.table[0] = '\0';
    s.arr_idx = 0;

    for (;;) {
        skip_blank(&s);
        if (at_end(&s))
            return 0;
        int line = s.line;
        if (peek(&s) == '[') {
            if (parse_header(&s, d) != 0)
                return -1;
            /* header ends the "line": require EOL/comment/';'-less */
            skip_inline_ws(&s);
            if (!at_end(&s) && peek(&s) != '\n' && peek(&s) != '\r' &&
                peek(&s) != '#' && peek(&s) != ';')
                return fail(&s, s.line, "unexpected characters after table header");
            continue;
        }
        /* key = value */
        char key[48];
        if (parse_bare_key(&s, key, sizeof(key)) != 0)
            return -1;
        skip_inline_ws(&s);
        if (peek(&s) != '=')
            return fail(&s, s.line, "expected '=' after key");
        s.p++;
        skip_inline_ws(&s);
        entry_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        if (parse_value(&s, &tmp) != 0)
            return -1;
        /* duplicate detection */
        for (size_t i = 0; i < d->n; i++) {
            if (d->e[i].arr_idx == s.arr_idx && strcmp(d->e[i].table, s.table) == 0 &&
                strcmp(d->e[i].key, key) == 0) {
                free(tmp.sval);
                return fail(&s, line, "duplicate key");
            }
        }
        entry_t *e = doc_add_entry(d);
        if (!e) {
            free(tmp.sval);
            return fail(&s, line, "oom");
        }
        *e = tmp;
        snprintf(e->table, sizeof(e->table), "%s", s.table);
        e->arr_idx = s.arr_idx;
        snprintf(e->key, sizeof(e->key), "%s", key);
        e->line = line;
        skip_inline_ws(&s);
        if (peek(&s) == ';') {
            s.p++;
            continue;
        }
        if (!at_end(&s) && peek(&s) != '\n' && peek(&s) != '\r' && peek(&s) != '#')
            return fail(&s, s.line, "unexpected characters after value");
    }
}

/* ---- sdf schema (this repository's layer on the imported core) ----- */

typedef enum { K_STR, K_INT, K_NUM, K_BOOL, K_ARR } keykind_t;

#define KF_POW2 (1u << 0) /* INT: must be a power of two */
#define KF_ODD (1u << 1)  /* INT: must be odd */
#define KF_REQ (1u << 2)  /* required in every [[table]] block */

typedef struct {
    const char *table;
    const char *key;
    keykind_t kind;
    uint32_t flags;
    int64_t imin, imax; /* K_INT range */
    double fmin, fmax;  /* K_NUM range */
    const char *enums;  /* K_STR: space-separated whitelist; NULL = any */
} skey_t;

/* [wm] — design 01 + ADR-0007 (method/seeds land with W2.1).
 * Ranges follow design 01 §Configuration and the wm.h contract
 * (m a power of two, sb odd). */
/* [fluid] / [[fluid_class]] — design 02 §Classes. */
/* [arc] — design 03 + ADR-0006 parameter set; static ranges are the
 * ARC paper's evaluated bands (plan §12: rho_u 0.3..0.7, rho_l 1.0..2.0,
 * lambda_u 0.10..1.0, lambda_l 0.01..0.10, U 50..200 ms, delta 30..100 ms,
 * gamma 8..20 ms, J 0..5 ms, G 20..200 ms, T_idle 0.5..2 s). */
/* [[origin]] — ADR-0008 / design 09 §4: the loopback driver's
 * degradation knobs are schema keys so the e2e harness is config-only. */
static const skey_t SKEYS[] = {
    /* [wm] */
    {"wm", "m", K_INT, KF_POW2, 2, 65536, 0, 0, NULL},
    {"wm", "method", K_STR, 0, 0, 0, 0, 0, "plain shuffle1 shuffle2"},
    {"wm", "sa", K_INT, 0, 0, 4294967295ll, 0, 0, NULL},
    {"wm", "sb", K_INT, KF_ODD, 1, 4294967295ll, 0, 0, NULL},
    {"wm", "rotate_on_wrap", K_BOOL, 0, 0, 0, 0, 0, NULL},
    {"wm", "init_fracs", K_ARR, 0, 0, 0, 0, 0, NULL}, /* 1..16, sum 1 ± 0.01 */
    {"wm", "control_hz", K_INT, 0, 1, 100, 0, 0, NULL},
    {"wm", "alpha", K_NUM, 0, 0, 0, 0.0, 1.0, NULL},
    {"wm", "beta", K_NUM, 0, 0, 0, 0.0, 1.0, NULL},
    {"wm", "rollout_pct", K_INT, 0, 0, 100, 0, 0, NULL},
    {"wm", "kill_switch", K_BOOL, 0, 0, 0, 0, 0, NULL},
    /* [fluid] */
    {"fluid", "arq_fallback", K_BOOL, 0, 0, 0, 0, 0, NULL},
    {"fluid", "overhead_abort_ratio", K_NUM, 0, 0, 0, 1.0, 10.0, NULL},
    /* [[fluid_class]] */
    {"fluid_class", "name", K_STR, KF_REQ, 0, 0, 0, 0, NULL},
    {"fluid_class", "eps", K_NUM, 0, 0, 0, 0.0, 1.0, NULL},
    {"fluid_class", "codec", K_STR, 0, 0, 0, 0, 0, "lt nanorq"},
    {"fluid_class", "rollout_pct", K_INT, 0, 0, 100, 0, 0, NULL},
    /* [arc] */
    {"arc", "pipeline", K_STR, 0, 0, 0, 0, 0, NULL},
    {"arc", "mode", K_STR, 0, 0, 0, 0, 0, "base arc_q arc_qg"},
    {"arc", "shadow_mode", K_BOOL, 0, 0, 0, 0, 0, NULL},
    {"arc", "kill_switch", K_BOOL, 0, 0, 0, 0, 0, NULL},
    {"arc", "batch_max", K_INT, 0, 1, 65536, 0, 0, NULL},
    {"arc", "batch_deadline_ns", K_INT, 0, 0, 1000000000ll, 0, 0, NULL},
    {"arc", "offset_min_ns", K_INT, 0, 0, 60000000000ll, 0, 0, NULL},
    {"arc", "offset_max_ns", K_INT, 0, 0, 60000000000ll, 0, 0, NULL},
    {"arc", "increase_gain", K_NUM, 0, 0, 0, 0.0, 1.0, NULL},
    {"arc", "decrease_gain", K_NUM, 0, 0, 0, 0.0, 1.0, NULL},
    {"arc", "rho_u", K_NUM, 0, 0, 0, 0.3, 0.7, NULL},
    {"arc", "rho_l", K_NUM, 0, 0, 0, 1.0, 2.0, NULL},
    {"arc", "lambda_u", K_NUM, 0, 0, 0, 0.01, 1.0, NULL}, /* incl. paper default 0.05 */
    {"arc", "lambda_l", K_NUM, 0, 0, 0, 0.01, 0.10, NULL},
    {"arc", "U_ns", K_INT, 0, 50000000ll, 200000000ll, 0, 0, NULL},
    {"arc", "delta_ns", K_INT, 0, 30000000ll, 100000000ll, 0, 0, NULL},
    {"arc", "gamma_ns", K_INT, 0, 8000000ll, 20000000ll, 0, 0, NULL},
    {"arc", "J_ns", K_INT, 0, 0, 5000000ll, 0, 0, NULL},
    {"arc", "G_ns", K_INT, 0, 20000000ll, 200000000ll, 0, 0, NULL},
    {"arc", "T_idle_ns", K_INT, 0, 500000000ll, 2000000000ll, 0, 0, NULL},
    {"arc", "auto_params", K_BOOL, 0, 0, 0, 0, 0, NULL},
    {"arc", "c_U", K_NUM, 0, 0, 0, 0.5, 2.0, NULL},
    {"arc", "c_delta", K_NUM, 0, 0, 0, 0.0, 10.0, NULL},
    {"arc", "min_smooth_bytes", K_INT, 0, 1, 65536, 0, 0, NULL},
    {"arc", "budget_ms", K_INT, 0, 0, 86400000ll, 0, 0, NULL},
    /* [[origin]] */
    {"origin", "id", K_INT, 0, 0, 63, 0, 0, NULL},
    {"origin", "cls", K_STR, KF_REQ, 0, 0, 0, 0, "loopback http s3 peer"},
    {"origin", "dst", K_STR, KF_REQ, 0, 0, 0, 0, NULL},
    {"origin", "throttle_ns", K_INT, 0, 0, 60000000000ll, 0, 0, NULL},
    {"origin", "loss_pct", K_NUM, 0, 0, 0, 0.0, 100.0, NULL},
    {"origin", "err_pct", K_NUM, 0, 0, 0, 0.0, 100.0, NULL},
    {"origin", "lat_jitter_ns", K_INT, 0, 0, 60000000000ll, 0, 0, NULL},
    {"origin", "max_inflight", K_INT, 0, 1, 1024, 0, 0, NULL},
    {"origin", "timeout_ms", K_INT, 0, 1, 600000, 0, 0, NULL},
    /* [ab] — design 01 §A/B assignment */
    {"ab", "bucket_mod", K_INT, 0, 1, 1000, 0, 0, NULL},
    {"ab", "enabled", K_BOOL, 0, 0, 0, 0, 0, NULL},
    /* [metrics] */
    {"metrics", "endpoint", K_STR, 0, 0, 0, 0, 0, NULL},
    {"metrics", "level", K_STR, 0, 0, 0, 0, 0, "debug info warn error"},
};

/* Array-of-tables bounds (ADR-0008 origin set, design 02 class set). */
static const struct {
    const char *name;
    int max_idx;
} ARRAY_TABLES[] = {
    {"origin", 64},
    {"fluid_class", 8},
};

static int array_table_max(const char *name, int *max_idx) {
    for (size_t i = 0; i < sizeof(ARRAY_TABLES) / sizeof(ARRAY_TABLES[0]); i++) {
        if (strcmp(ARRAY_TABLES[i].name, name) == 0) {
            *max_idx = ARRAY_TABLES[i].max_idx;
            return 1;
        }
    }
    return 0;
}

static int table_known(const char *t) {
    for (size_t i = 0; i < sizeof(SKEYS) / sizeof(SKEYS[0]); i++) {
        if (strcmp(SKEYS[i].table, t) == 0)
            return 1;
    }
    return 0;
}

static const skey_t *find_skey(const char *table, const char *key) {
    for (size_t i = 0; i < sizeof(SKEYS) / sizeof(SKEYS[0]); i++) {
        if (strcmp(SKEYS[i].table, table) == 0 && strcmp(SKEYS[i].key, key) == 0)
            return &SKEYS[i];
    }
    return NULL;
}

static const entry_t *doc_find(const doc_t *d, const char *table, int arr_idx,
                               const char *key) {
    for (size_t i = 0; i < d->n; i++) {
        if (d->e[i].arr_idx == arr_idx && strcmp(d->e[i].table, table) == 0 &&
            strcmp(d->e[i].key, key) == 0)
            return &d->e[i];
    }
    return NULL;
}

static int enum_ok(const char *enums, const char *val) {
    /* space-separated whitelist walk (wam key_allowed idiom) */
    const char *k = enums;
    size_t vlen = strlen(val);
    while (*k) {
        const char *sp = strchr(k, ' ');
        size_t n = sp ? (size_t)(sp - k) : strlen(k);
        if (n == vlen && strncmp(k, val, n) == 0)
            return 1;
        k += n;
        if (*k == ' ')
            k++;
    }
    return 0;
}

static int schema_validate(const doc_t *d, const char *path, char *err,
                           size_t errsz) {
    /* headers: known tables, array bounds */
    for (size_t i = 0; i < d->nh; i++) {
        const hdr_t *h = &d->h[i];
        if (!table_known(h->name))
            return failf(err, errsz, path, 0, h->name, "unknown table");
        int max_idx = 0;
        if (array_table_max(h->name, &max_idx) && h->arr_idx >= max_idx) {
            char msg[64];
            snprintf(msg, sizeof(msg), "too many blocks (max %d)", max_idx);
            return failf(err, errsz, path, 0, h->name, msg);
        }
    }

    /* entries: unknown keys, type mismatches, ranges, enums */
    for (size_t i = 0; i < d->n; i++) {
        const entry_t *e = &d->e[i];
        if (!e->table[0])
            return failf(err, errsz, path, e->line, e->key,
                         "key outside any table");
        const skey_t *k = find_skey(e->table, e->key);
        if (!k) {
            char what[96];
            snprintf(what, sizeof(what), "[%s] %s", e->table, e->key);
            return failf(err, errsz, path, e->line, what, "unknown key");
        }
        if (k->kind == K_INT && e->type != V_INT)
            return failf(err, errsz, path, e->line, e->key, "expected integer");
        if (k->kind == K_STR && e->type != V_STR)
            return failf(err, errsz, path, e->line, e->key, "expected string");
        if (k->kind == K_BOOL && e->type != V_BOOL)
            return failf(err, errsz, path, e->line, e->key, "expected boolean");
        if (k->kind == K_ARR && e->type != V_ARR)
            return failf(err, errsz, path, e->line, e->key,
                         "expected array of numbers");
        if (k->kind == K_STR && k->enums && !enum_ok(k->enums, e->sval)) {
            char msg[128];
            snprintf(msg, sizeof(msg), "must be one of: %s", k->enums);
            return failf(err, errsz, path, e->line, e->key, msg);
        }
        if (k->kind == K_INT) {
            if (e->ival < k->imin || e->ival > k->imax)
                return failf(err, errsz, path, e->line, e->key, "out of range");
            if ((k->flags & KF_POW2) &&
                (e->ival <= 0 || (e->ival & (e->ival - 1)) != 0))
                return failf(err, errsz, path, e->line, e->key,
                             "must be a power of two");
            if ((k->flags & KF_ODD) && (e->ival & 1) == 0)
                return failf(err, errsz, path, e->line, e->key, "must be odd");
        }
        if (k->kind == K_NUM) {
            double v = e->type == V_INT ? (double)e->ival : e->fval;
            if (v < k->fmin || v > k->fmax)
                return failf(err, errsz, path, e->line, e->key, "out of range");
        }
    }

    /* required keys in every [[array]] block */
    for (size_t i = 0; i < d->nh; i++) {
        const hdr_t *h = &d->h[i];
        int max_idx = 0;
        if (!array_table_max(h->name, &max_idx))
            continue;
        for (size_t s = 0; s < sizeof(SKEYS) / sizeof(SKEYS[0]); s++) {
            const skey_t *k = &SKEYS[s];
            if (strcmp(k->table, h->name) != 0 || !(k->flags & KF_REQ))
                continue;
            if (!doc_find(d, h->name, h->arr_idx, k->key))
                return failf(err, errsz, path, 0, k->key, "required");
        }
    }

    /* [wm] init_fracs: 1..16 values in [0,1] summing to 1 ± 0.01 */
    const entry_t *fr = doc_find(d, "wm", 0, "init_fracs");
    if (fr) {
        if (fr->n_nums < 1 || fr->n_nums > 16)
            return failf(err, errsz, path, fr->line, "init_fracs",
                         "need 1..16 values");
        double sum = 0;
        for (int n = 0; n < fr->n_nums; n++) {
            if (fr->nums[n] < 0.0 || fr->nums[n] > 1.0)
                return failf(err, errsz, path, fr->line, "init_fracs",
                             "each value in [0,1]");
            sum += fr->nums[n];
        }
        if (sum < 0.99 || sum > 1.01)
            return failf(err, errsz, path, fr->line, "init_fracs",
                         "values must sum to 1.0");
    }
    return 0;
}

/* ---- flat-entry conversion ------------------------------------------- */

static int entries_convert(const doc_t *d, sdf_config_t *cfg, const char *path,
                           char *err, size_t errsz) {
    if (d->n == 0) {
        cfg->entries = NULL;
        cfg->count = 0;
        cfg->capacity = 0;
        return 0;
    }
    cfg->entries = malloc(d->n * sizeof(sdf_config_entry_t));
    if (!cfg->entries)
        return failf(err, errsz, path, 0, "convert", "oom");

    for (size_t i = 0; i < d->n; i++) {
        const entry_t *e = &d->e[i];
        sdf_config_entry_t *out = &cfg->entries[i];
        int max_idx = 0;
        char flat[128]; /* worst case: 32 + "[nnn]" + '.' + 48 + NUL */
        int n;
        if (array_table_max(e->table, &max_idx))
            n = snprintf(flat, sizeof(flat), "%s[%d].%s", e->table, e->arr_idx,
                         e->key);
        else
            n = snprintf(flat, sizeof(flat), "%s.%s", e->table, e->key);
        if (n < 0 || (size_t)n >= sizeof(flat))
            return failf(err, errsz, path, e->line, e->key, "key encoding failed");
        if ((size_t)n >= sizeof(out->key))
            return failf(err, errsz, path, e->line, e->key,
                         "flat key too long (max 63)");
        memcpy(out->key, flat, (size_t)n + 1);

        switch (e->type) {
        case V_STR:
            if (strlen(e->sval) >= sizeof(out->value))
                return failf(err, errsz, path, e->line, e->key,
                              "string too long for flat entry");
            snprintf(out->value, sizeof(out->value), "%s", e->sval);
            break;
        case V_INT:
            snprintf(out->value, sizeof(out->value), "%lld",
                     (long long)e->ival);
            break;
        case V_FLOAT:
            /* %g canonical: human-written ops decimals round-trip
             * exactly (strtod("0.05") is the same double the parser
             * produced) and stay readable in dumps */
            snprintf(out->value, sizeof(out->value), "%g", e->fval);
            break;
        case V_BOOL:
            snprintf(out->value, sizeof(out->value), "%s",
                     e->bval ? "true" : "false");
            break;
        case V_ARR: {
            /* canonical "[v, v, ...]" with %g scalars; capacity-checked
             * (schema caps init_fracs at 16, parser caps all at 64) */
            size_t off = 0;
            out->value[off++] = '[';
            for (int n = 0; n < e->n_nums; n++) {
                if (off + 16 >= sizeof(out->value))
                    return failf(err, errsz, path, e->line, e->key,
                                 "array too long for flat entry");
                off += (size_t)snprintf(out->value + off,
                                        sizeof(out->value) - off, "%s%g",
                                        n ? ", " : "", e->nums[n]);
            }
            if (off + 1 >= sizeof(out->value))
                return failf(err, errsz, path, e->line, e->key,
                             "array too long for flat entry");
            out->value[off++] = ']';
            out->value[off] = '\0';
            break;
        }
        }
    }
    cfg->count = d->n;
    cfg->capacity = d->n;
    return 0;
}

/* ---- public API ------------------------------------------------------- */

static int read_file(const char *path, char **buf, size_t *len, char *err,
                     size_t errsz) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return failf(err, errsz, path, 0, "open", "cannot open");
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return failf(err, errsz, path, 0, "open", "seek failed");
    }
    long flen = ftell(f);
    if (flen < 0 || flen > 262144 || fseek(f, SEEK_SET, 0) != 0) {
        fclose(f);
        return failf(err, errsz, path, 0, "open",
                     "empty or oversized (max 256 KiB)");
    }
    char *b = malloc((size_t)flen + 1);
    if (!b) {
        fclose(f);
        return failf(err, errsz, path, 0, "open", "oom");
    }
    size_t got = fread(b, 1, (size_t)flen, f);
    fclose(f);
    if (got != (size_t)flen) {
        free(b);
        return failf(err, errsz, path, 0, "open", "read failed");
    }
    b[got] = '\0';
    *buf = b;
    *len = got;
    return 0;
}

int sdf_config_load(sdf_config_t *cfg, const char *path) {
    if (cfg == NULL || path == NULL)
        return -1;
    memset(cfg, 0, sizeof(*cfg));

    char err[256];
    err[0] = '\0';

    char *buf = NULL;
    size_t len = 0;
    if (read_file(path, &buf, &len, err, sizeof(err)) != 0)
        goto bad;

    {
        doc_t doc;
        memset(&doc, 0, sizeof(doc));
        if (toml_parse(buf, len, path, &doc, err, sizeof(err)) != 0) {
            doc_free(&doc);
            goto bad;
        }
        if (schema_validate(&doc, path, err, sizeof(err)) != 0) {
            doc_free(&doc);
            goto bad;
        }
        if (entries_convert(&doc, cfg, path, err, sizeof(err)) != 0) {
            doc_free(&doc);
            goto bad;
        }
        doc_free(&doc);
    }

    free(buf);
    return 0;

bad:
    SDF_LOGE("config: %s", err);
    free(buf);
    if (cfg->entries != NULL) {
        free(cfg->entries);
        cfg->entries = NULL;
    }
    cfg->count = 0;
    cfg->capacity = 0;
    return -1;
}

const char *sdf_config_get(const sdf_config_t *cfg, const char *key,
                           const char *fallback) {
    if (cfg == NULL)
        return fallback;
    for (size_t i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].key, key) == 0)
            return cfg->entries[i].value;
    }
    return fallback;
}

int sdf_config_get_u64(const sdf_config_t *cfg, const char *key, uint64_t *out) {
    if (out == NULL)
        return -1;
    const char *v = sdf_config_get(cfg, key, NULL);
    if (v == NULL || *v == '\0' || *v == '-')
        return -1; /* missing or not an unsigned integer */
    errno = 0;
    char *end = NULL;
    unsigned long long u = strtoull(v, &end, 10);
    if (end == v || *end != '\0' || errno == ERANGE)
        return -1;
    *out = (uint64_t)u;
    return 0;
}

void sdf_config_free(sdf_config_t *cfg) {
    if (cfg == NULL)
        return;
    free(cfg->entries);
    cfg->entries = NULL;
    cfg->count = 0;
    cfg->capacity = 0;
}
