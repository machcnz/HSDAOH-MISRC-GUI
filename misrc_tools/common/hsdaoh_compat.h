#ifndef MISRC_HSDAOH_COMPAT_H
#define MISRC_HSDAOH_COMPAT_H

#include <hsdaoh.h>

/*
 * hsdaoh_start_stream() API compatibility:
 * - Newer headers expose: hsdaoh_start_stream(dev, cb, ctx, buf_num)
 * - Some installed system headers expose: hsdaoh_start_stream(dev, cb, ctx)
 *
 * HSDAOH_MAX_BUF_SIZE exists in newer headers where buf_num is supported.
 */
#if defined(HSDAOH_MAX_BUF_SIZE)
#define MISRC_HSDAOH_START_STREAM(dev, cb, ctx) hsdaoh_start_stream((dev), (cb), (ctx), 0)
#else
#define MISRC_HSDAOH_START_STREAM(dev, cb, ctx) hsdaoh_start_stream((dev), (cb), (ctx))
#endif

#endif /* MISRC_HSDAOH_COMPAT_H */
