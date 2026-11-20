#ifndef SDF_TEST_ARC_LAYOUT_H
#define SDF_TEST_ARC_LAYOUT_H

#include <stddef.h>

/*
 * W6.18 layout gate: the same probe source is compiled twice, once against
 * include/sdf/arc.h and once against the cbindgen-generated header, and the
 * resulting observations are compared. Kind is "field" (size/offset), or
 * "enum"/"define" (value in offset, size 0).
 */
typedef struct {
    const char *kind;
    const char *name;
    size_t size;
    size_t offset;
} arc_layout_entry;

#endif
