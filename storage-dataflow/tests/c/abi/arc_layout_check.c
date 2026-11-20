/* Compare the C header's ARC layout against the cbindgen-generated view. */
#include <stdio.h>
#include <string.h>

#include "arc_layout.h"

extern const arc_layout_entry arc_layout_entries_c[];
extern const size_t arc_layout_count_c;
extern const size_t arc_type_sizes_c[];
extern const size_t arc_type_size_count_c;

extern const arc_layout_entry arc_layout_entries_rust[];
extern const size_t arc_layout_count_rust;
extern const size_t arc_type_sizes_rust[];
extern const size_t arc_type_size_count_rust;

static int failures;

static void check_size(const char *what, size_t c, size_t rust) {
    if (c != rust) {
        fprintf(stderr, "arc layout: %s: C=%zu Rust=%zu\n", what, c, rust);
        failures++;
    }
}

int main(void) {
    check_size("entry count", arc_layout_count_c, arc_layout_count_rust);
    check_size("type-size count", arc_type_size_count_c, arc_type_size_count_rust);

    if (arc_layout_count_c == arc_layout_count_rust) {
        for (size_t i = 0; i < arc_layout_count_c; i++) {
            const arc_layout_entry *c = &arc_layout_entries_c[i];
            const arc_layout_entry *rust = &arc_layout_entries_rust[i];
            if (strcmp(c->kind, rust->kind) != 0 || strcmp(c->name, rust->name) != 0) {
                fprintf(stderr, "arc layout: entry %zu: C=%s/%s Rust=%s/%s\n", i, c->kind, c->name,
                        rust->kind, rust->name);
                failures++;
                continue;
            }
            if (c->size != rust->size || c->offset != rust->offset) {
                fprintf(stderr, "arc layout: %s %s: C=size %zu off %zu Rust=size %zu off %zu\n",
                        c->kind, c->name, c->size, c->offset, rust->size, rust->offset);
                failures++;
            }
        }
    }

    if (arc_type_size_count_c == arc_type_size_count_rust) {
        for (size_t i = 0; i < arc_type_size_count_c; i++) {
            if (arc_type_sizes_c[i] != arc_type_sizes_rust[i]) {
                fprintf(stderr, "arc layout: type %zu size: C=%zu Rust=%zu\n", i,
                        arc_type_sizes_c[i], arc_type_sizes_rust[i]);
                failures++;
            }
        }
    }

    if (failures != 0) {
        fprintf(stderr, "arc layout: FAIL (%d mismatches)\n", failures);
        return 1;
    }
    printf("arc layout: OK (%zu field/enum/define probes, %zu type sizes identical)\n",
           arc_layout_count_c, arc_type_size_count_c);
    return 0;
}
