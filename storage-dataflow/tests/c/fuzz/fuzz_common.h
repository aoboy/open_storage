#ifndef SDF_TEST_FUZZ_COMMON_H
#define SDF_TEST_FUZZ_COMMON_H

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

static long fuzz_budget(long default_iters) {
    const char *text = getenv("SDF_FUZZ_ITERS");
    if (text == NULL || text[0] == '\0') {
        return default_iters;
    }

    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 || value > LONG_MAX) {
        fprintf(stderr, "invalid SDF_FUZZ_ITERS=%s (expected 1..%ld)\n", text, LONG_MAX);
        exit(2);
    }
    return (long)value;
}

#endif /* SDF_TEST_FUZZ_COMMON_H */
