#ifndef SDF_VERSION_H
#define SDF_VERSION_H

/*
 * Single source of truth for the library version (plan prompt W5.2,
 * the wam version.h pattern): CMake parses these for the pkg-config
 * Version: and the find_package config. The ON-WIRE formats (fountain
 * ESI conventions, etc.) are separate contracts and never move with
 * this file.
 */

#define SDF_VERSION_MAJOR 0
#define SDF_VERSION_MINOR 2
#define SDF_VERSION_PATCH 0

#define SDF_VERSION_STRING "0.2.0"

#define SDF_VERSION_AT_LEAST(maj, min, pat)                                   \
    ((maj) < SDF_VERSION_MAJOR ||                                             \
     ((maj) == SDF_VERSION_MAJOR && (min) < SDF_VERSION_MINOR) ||            \
     ((maj) == SDF_VERSION_MAJOR && (min) == SDF_VERSION_MINOR &&             \
      (pat) <= SDF_VERSION_PATCH))

#endif /* SDF_VERSION_H */
