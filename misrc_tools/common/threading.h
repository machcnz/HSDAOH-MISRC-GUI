/*
 * MISRC Common - Platform-Agnostic Threading and Timing Primitives
 *
 * Provides a unified threading and timing API for Windows and POSIX systems.
 * For GUI code, avoids including windows.h to prevent conflicts with raylib.
 */

#ifndef MISRC_THREADING_H
#define MISRC_THREADING_H

#include <stdint.h>

#ifdef _WIN32
  #include <process.h>

  typedef void* thrd_t;
  typedef unsigned (__stdcall *thrd_start_t)(void*);

  #define thrd_success 0

  #define thrd_create(a,b,c) \
    (((*(a)=(thrd_t)_beginthreadex(NULL,0,(thrd_start_t)b,c,0,NULL))==0)?-1:thrd_success)

  /* Windows constants - define if not already defined (avoids windows.h) */
  #ifndef INFINITE
    #define INFINITE 0xFFFFFFFF
  #endif
  #ifndef WAIT_OBJECT_0
    #define WAIT_OBJECT_0 0
  #endif

  /* Thread join implementation that avoids windows.h */
  static inline int thrd_join_impl(thrd_t t, int *res) {
    /* Forward declare Windows API functions to avoid including windows.h */
    extern __declspec(dllimport) unsigned long __stdcall WaitForSingleObject(void*, unsigned long);
    extern __declspec(dllimport) int __stdcall GetExitCodeThread(void*, unsigned long*);
    extern __declspec(dllimport) int __stdcall CloseHandle(void*);

    unsigned long exitcode = 0;
    if (WaitForSingleObject(t, INFINITE) != WAIT_OBJECT_0) return -1;
    GetExitCodeThread(t, &exitcode);
    if (res) *res = (int)exitcode;
    CloseHandle(t);
    return thrd_success;
  }
  #define thrd_join(a,b) thrd_join_impl(a,b)

  /* Sleep for milliseconds */
  static inline void thrd_sleep_ms(int ms) {
    extern __declspec(dllimport) void __stdcall Sleep(unsigned long);
    Sleep((unsigned long)ms);
  }

  /* Alias for compatibility with CLI code */
  #define sleep_ms(x) thrd_sleep_ms(x)

  /* Get current time in milliseconds (for timeouts, elapsed time tracking) */
  static inline uint64_t get_time_ms(void) {
    extern __declspec(dllimport) unsigned long __stdcall GetTickCount(void);
    return (uint64_t)GetTickCount();
  }

  /* Get current time in microseconds (for performance measurements) */
  static inline uint64_t get_time_us(void) {
    /* Layout-compatible with LARGE_INTEGER for when windows.h is included */
    typedef union { int64_t i64; struct { unsigned long lo, hi; } parts; } perf_counter_t;
    static int64_t freq = 0;
    perf_counter_t counter, freq_val;
    #ifdef _PROFILEAPI_H_
    /* windows.h was included - use the already-declared functions with cast */
    if (freq == 0) {
      QueryPerformanceFrequency((LARGE_INTEGER*)&freq_val);
      freq = freq_val.i64;
    }
    QueryPerformanceCounter((LARGE_INTEGER*)&counter);
    #else
    /* Declare functions ourselves */
    extern __declspec(dllimport) int __stdcall QueryPerformanceCounter(perf_counter_t*);
    extern __declspec(dllimport) int __stdcall QueryPerformanceFrequency(perf_counter_t*);
    if (freq == 0) {
      QueryPerformanceFrequency(&freq_val);
      freq = freq_val.i64;
    }
    QueryPerformanceCounter(&counter);
    #endif
    return (uint64_t)(counter.i64 * 1000000 / freq);
  }

#else
  /* POSIX implementation */
  #include <pthread.h>
  #include <time.h>
  #include <sys/time.h>
  #include <unistd.h>

  typedef pthread_t thrd_t;

  #define thrd_success 0

  #define thrd_create(a,b,c) pthread_create(a,NULL,(void* (*)(void *))b,c)
  #define thrd_join(a,b) pthread_join(a,(void**)(b))

  /* Sleep for milliseconds */
  static inline void thrd_sleep_ms(int ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
  }

  /* Alias for compatibility with CLI code */
  #define sleep_ms(x) usleep((x)*1000)

  /* Get current time in milliseconds (for timeouts, elapsed time tracking) */
  static inline uint64_t get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
  }

  /* Get current time in microseconds (for performance measurements) */
  static inline uint64_t get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)(tv.tv_sec * 1000000 + tv.tv_usec);
  }

#endif

#endif /* MISRC_THREADING_H */
