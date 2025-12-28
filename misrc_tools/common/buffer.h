/*
 * MISRC Common - Aligned Buffer Allocation
 *
 * Provides platform-agnostic aligned memory allocation for SIMD operations.
 * SSE/AVX require 16/32-byte alignment for optimal performance.
 */

#ifndef MISRC_BUFFER_H
#define MISRC_BUFFER_H

#include <stddef.h>
#include <stdlib.h>

#ifdef _WIN32
  #include <malloc.h>
  #define aligned_alloc(align, size) _aligned_malloc(size, align)
  #define aligned_free(ptr) _aligned_free(ptr)
#else
  /* POSIX aligned_alloc is available in C11, but macOS needs posix_memalign */
  #if defined(__APPLE__)
    #include <stdlib.h>
    static inline void* _misrc_aligned_alloc(size_t align, size_t size) {
      void *ptr = NULL;
      if (posix_memalign(&ptr, align, size) != 0) return NULL;
      return ptr;
    }
    #define aligned_alloc(align, size) _misrc_aligned_alloc(align, size)
  #endif
  /* Linux/BSD: aligned_alloc is in stdlib.h for C11 */
  #define aligned_free(ptr) free(ptr)
#endif

/* Common alignment values */
#define ALIGN_SSE  16   /* SSE requires 16-byte alignment */
#define ALIGN_AVX  32   /* AVX requires 32-byte alignment */
#define ALIGN_PAGE 4096 /* Page alignment for large buffers */

#endif /* MISRC_BUFFER_H */
