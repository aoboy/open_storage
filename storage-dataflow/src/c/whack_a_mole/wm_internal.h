#ifndef SDF_WM_INTERNAL_H
#define SDF_WM_INTERNAL_H

#include "sdf/wm.h"

/*
 * Rebuild the derived tables after the down mask changes. cdf is left intact
 * so the original profile can be restored by clearing the mask.
 */
int sdf_wm_rebuild(wm_compiled_t *c);

#endif /* SDF_WM_INTERNAL_H */
