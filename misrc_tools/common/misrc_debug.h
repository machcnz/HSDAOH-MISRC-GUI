#ifndef MISRC_DEBUG_H
#define MISRC_DEBUG_H

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/*
 * Enable verbose debug logging by setting:
 *   MISRC_GUI_DEBUG=1
 * or (shared across tools):
 *   MISRC_DEBUG=1
 */
static inline bool misrc_debug_enabled(void) {
    static int cached = -1;
    if (cached == -1) {
        const char *v = getenv("MISRC_GUI_DEBUG");
        if (!v) v = getenv("MISRC_DEBUG");
        cached = (v && v[0] && strcmp(v, "0") != 0) ? 1 : 0;
    }
    return cached == 1;
}

#endif /* MISRC_DEBUG_H */
