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

  /* Thread priority constants */
  #define THRD_PRIORITY_NORMAL  0
  #define THRD_PRIORITY_ABOVE   1
  #define THRD_PRIORITY_HIGH    2

  /* Set thread priority for current thread */
  static inline void thrd_set_priority(int priority) {
    extern __declspec(dllimport) void* __stdcall GetCurrentThread(void);
    extern __declspec(dllimport) int __stdcall SetThreadPriority(void*, int);
    /* Windows priority values: ABOVE_NORMAL=1, HIGH=2 */
    int winprio = 0;  /* THREAD_PRIORITY_NORMAL */
    if (priority == THRD_PRIORITY_ABOVE) winprio = 1;  /* THREAD_PRIORITY_ABOVE_NORMAL */
    else if (priority == THRD_PRIORITY_HIGH) winprio = 2;  /* THREAD_PRIORITY_HIGHEST */
    SetThreadPriority(GetCurrentThread(), winprio);
  }

  /* Process priority constants */
  #define PROC_PRIORITY_NORMAL      0
  #define PROC_PRIORITY_ABOVE       1
  #define PROC_PRIORITY_HIGH        2

  /* Set process priority class - affects all threads including library-spawned ones */
  static inline void proc_set_priority(int priority) {
    extern __declspec(dllimport) void* __stdcall GetCurrentProcess(void);
    extern __declspec(dllimport) int __stdcall SetPriorityClass(void*, unsigned long);
    /* Windows priority class values */
    unsigned long prio_class = 0x00000020;  /* NORMAL_PRIORITY_CLASS */
    if (priority == PROC_PRIORITY_ABOVE) prio_class = 0x00008000;  /* ABOVE_NORMAL_PRIORITY_CLASS */
    else if (priority == PROC_PRIORITY_HIGH) prio_class = 0x00000080;  /* HIGH_PRIORITY_CLASS */
    SetPriorityClass(GetCurrentProcess(), prio_class);
  }

  /* Get current time in milliseconds (for timeouts, elapsed time tracking) */
  static inline uint64_t get_time_ms(void) {
    extern __declspec(dllimport) unsigned long __stdcall GetTickCount(void);
    return (uint64_t)GetTickCount();
  }

  /* Get current time in microseconds (for performance measurements) */
  static inline uint64_t get_time_us(void) {
    extern __declspec(dllimport) unsigned long long __stdcall GetTickCount64(void);
    return (uint64_t)GetTickCount64() * 1000ULL;
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

  /* Thread priority constants */
  #define THRD_PRIORITY_NORMAL  0
  #define THRD_PRIORITY_ABOVE   1
  #define THRD_PRIORITY_HIGH    2

  /* Set thread priority for current thread using nice value (lower = higher priority) */
  static inline void thrd_set_priority(int priority) {
    /* Use nice() to adjust priority. Normal threads run at nice 0.
     * We can decrease nice value (increase priority) without root on most systems
     * if we're going from 0 to negative values within the same session. */
    int nice_val = 0;
    if (priority == THRD_PRIORITY_ABOVE) nice_val = -5;
    else if (priority == THRD_PRIORITY_HIGH) nice_val = -10;

    /* Try pthread_setschedparam first for finer control */
    struct sched_param param;
    int policy;
    if (pthread_getschedparam(pthread_self(), &policy, &param) == 0) {
      /* For SCHED_OTHER (normal scheduling), priority is typically 0.
       * Switch to SCHED_RR or SCHED_FIFO requires root, so just try nice. */
      (void)param;
      (void)policy;
    }

    /* Fall back to nice - this affects the whole thread group but is simpler */
    if (nice_val != 0) {
      /* nice() returns the new nice value or -1 on error (but -1 is also valid).
       * We ignore errors since this is best-effort. */
      (void)nice(nice_val);
    }
  }

  /* Process priority constants */
  #define PROC_PRIORITY_NORMAL      0
  #define PROC_PRIORITY_ABOVE       1
  #define PROC_PRIORITY_HIGH        2

  /* Set process priority - on POSIX, nice() already affects the whole process */
  static inline void proc_set_priority(int priority) {
    int nice_val = 0;
    if (priority == PROC_PRIORITY_ABOVE) nice_val = -5;
    else if (priority == PROC_PRIORITY_HIGH) nice_val = -10;
    if (nice_val != 0) {
      (void)nice(nice_val);
    }
  }

  /* Get current time in milliseconds (for timeouts, elapsed time tracking) */
  static inline uint64_t get_time_ms(void) {
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
      return (uint64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    }
#endif
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
  }

  /* Get current time in microseconds (for performance measurements) */
  static inline uint64_t get_time_us(void) {
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
      return (uint64_t)(ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
    }
#endif
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)(tv.tv_sec * 1000000 + tv.tv_usec);
  }

#endif

#endif /* MISRC_THREADING_H */
