#ifndef SDF_TESTS_GOLDEN_JSON_H
#define SDF_TESTS_GOLDEN_JSON_H

/*
 * Minimal JSON-subset reader for the golden vector JSON files (plan
 * prompt W0.4, ADR-0009). Test-only infrastructure: allocates freely,
 * no hot-path constraints, and it errors loudly on anything outside
 * the subset rather than guessing — a golden file that fails to parse
 * is a broken golden.
 *
 * Supported: objects, arrays, strings with \\ \" \/ \b \f \n \r \t
 * escapes, decimal numbers (int and float via strtod), true/false/null.
 * Rejected: \u escapes, trailing garbage, over-deep nesting (> 32).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype_t;

typedef struct jval jval_t;
struct jval {
    jtype_t type;
    double num;    /* J_NUM */
    int boolean;   /* J_BOOL */
    char *str;     /* J_STR (malloc'd) */
    jval_t **items; /* J_ARR */
    size_t n_items;
    char **keys;   /* J_OBJ: keys[i] pairs with vals[i] */
    jval_t **vals;
    size_t n_fields;
};

typedef struct {
    const char *p;
    int depth;
} jparse_t;

static void golden_json_free(jval_t *v) {
    if (v == NULL)
        return;
    free(v->str);
    for (size_t i = 0; i < v->n_items; i++)
        golden_json_free(v->items[i]);
    free(v->items);
    for (size_t i = 0; i < v->n_fields; i++) {
        if (v->keys != NULL)
            free(v->keys[i]);
        if (v->vals != NULL)
            golden_json_free(v->vals[i]);
    }
    free(v->keys);
    free(v->vals);
    free(v);
}

static void jskip_ws(jparse_t *j) {
    while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')
        j->p++;
}

static jval_t *jnew(jtype_t t) {
    jval_t *v = calloc(1, sizeof(*v));
    if (v != NULL)
        v->type = t;
    return v;
}

static jval_t *jparse_value(jparse_t *j);

static int jparse_string_raw(jparse_t *j, char **out) {
    if (*j->p != '"')
        return -1;
    j->p++;
    size_t cap = 64, len = 0;
    char *buf = malloc(cap);
    if (buf == NULL)
        return -1;
    while (*j->p != '"') {
        if (*j->p == '\0') {
            free(buf);
            return -1; /* unterminated */
        }
        char c = *j->p;
        if (c == '\\') {
            j->p++;
            switch (*j->p) {
            case '\\': c = '\\'; break;
            case '"': c = '"'; break;
            case '/': c = '/'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            default:
                free(buf);
                return -1; /* \u and unknown escapes rejected */
            }
        }
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (nb == NULL) {
                free(buf);
                return -1;
            }
            buf = nb;
        }
        buf[len++] = c;
        j->p++;
    }
    j->p++; /* closing quote */
    buf[len] = '\0';
    *out = buf;
    return 0;
}

static jval_t *jparse_object(jparse_t *j) {
    jval_t *v = jnew(J_OBJ);
    if (v == NULL)
        return NULL;
    j->p++; /* '{' */
    size_t cap = 0;
    for (;;) {
        jskip_ws(j);
        if (*j->p == '}') {
            j->p++;
            return v;
        }
        char *key = NULL;
        if (jparse_string_raw(j, &key) != 0)
            goto fail;
        jskip_ws(j);
        if (*j->p != ':') {
            free(key);
            goto fail;
        }
        j->p++;
        jval_t *val = jparse_value(j);
        if (val == NULL) {
            free(key);
            goto fail;
        }
        if (v->n_fields == cap) {
            size_t nc = cap ? cap * 2 : 8;
            char **nk = realloc(v->keys, nc * sizeof(*nk));
            jval_t **nv = realloc(v->vals, nc * sizeof(*nv));
            if (nk == NULL || nv == NULL) {
                /* test-only reader: OOM on a golden file is fatal */
                fprintf(stderr, "golden_json: oom\n");
                exit(1);
            }
            v->keys = nk;
            v->vals = nv;
            cap = nc;
        }
        v->keys[v->n_fields] = key;
        v->vals[v->n_fields] = val;
        v->n_fields++;
        jskip_ws(j);
        if (*j->p == ',') {
            j->p++;
            continue;
        }
        if (*j->p == '}') {
            j->p++;
            return v;
        }
        goto fail;
    }
fail:
    golden_json_free(v);
    return NULL;
}

static jval_t *jparse_array(jparse_t *j) {
    jval_t *v = jnew(J_ARR);
    if (v == NULL)
        return NULL;
    j->p++; /* '[' */
    size_t cap = 0;
    for (;;) {
        jskip_ws(j);
        if (*j->p == ']') {
            j->p++;
            return v;
        }
        jval_t *item = jparse_value(j);
        if (item == NULL)
            goto fail;
        if (v->n_items == cap) {
            size_t nc = cap ? cap * 2 : 8;
            jval_t **ni = realloc(v->items, nc * sizeof(*ni));
            if (ni == NULL) {
                /* test-only reader: OOM on a golden file is fatal */
                fprintf(stderr, "golden_json: oom\n");
                exit(1);
            }
            v->items = ni;
            cap = nc;
        }
        v->items[v->n_items++] = item;
        jskip_ws(j);
        if (*j->p == ',') {
            j->p++;
            continue;
        }
        if (*j->p == ']') {
            j->p++;
            return v;
        }
        goto fail;
    }
fail:
    golden_json_free(v);
    return NULL;
}

static jval_t *jparse_value(jparse_t *j) {
    jskip_ws(j);
    if (++j->depth > 32)
        return NULL;
    jval_t *v = NULL;
    char c = *j->p;
    if (c == '{') {
        v = jparse_object(j);
    } else if (c == '[') {
        v = jparse_array(j);
    } else if (c == '"') {
        char *s = NULL;
        if (jparse_string_raw(j, &s) == 0) {
            v = jnew(J_STR);
            if (v == NULL) {
                free(s);
            } else {
                v->str = s;
            }
        }
    } else if (strncmp(j->p, "true", 4) == 0) {
        j->p += 4;
        v = jnew(J_BOOL);
        if (v != NULL)
            v->boolean = 1;
    } else if (strncmp(j->p, "false", 5) == 0) {
        j->p += 5;
        v = jnew(J_BOOL);
    } else if (strncmp(j->p, "null", 4) == 0) {
        j->p += 4;
        v = jnew(J_NULL);
    } else {
        char *end = NULL;
        double num = strtod(j->p, &end);
        if (end == j->p || end == NULL)
            return NULL;
        j->p = end;
        v = jnew(J_NUM);
        if (v != NULL)
            v->num = num;
    }
    j->depth--;
    return v;
}

static jval_t *golden_json_parse(const char *text) {
    if (text == NULL)
        return NULL;
    jparse_t j = {.p = text, .depth = 0};
    jval_t *v = jparse_value(&j);
    if (v == NULL)
        return NULL;
    jskip_ws(&j);
    if (*j.p != '\0') {
        golden_json_free(v);
        return NULL; /* trailing garbage */
    }
    return v;
}

static const jval_t *golden_json_get(const jval_t *obj, const char *key) {
    if (obj == NULL || obj->type != J_OBJ)
        return NULL;
    for (size_t i = 0; i < obj->n_fields; i++)
        if (strcmp(obj->keys[i], key) == 0)
            return obj->vals[i];
    return NULL;
}

/* Convenience: field as long (rejects non-integers to avoid silent
 * truncation of 1.5 into 1). */
static long golden_json_int(const jval_t *v, long fallback) {
    if (v == NULL || v->type != J_NUM)
        return fallback;
    if (v->num != (double)(long)v->num)
        return fallback;
    return (long)v->num;
}

#endif /* SDF_TESTS_GOLDEN_JSON_H */
