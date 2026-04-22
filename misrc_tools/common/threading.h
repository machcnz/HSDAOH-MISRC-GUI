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
  #define THRD_PRIORITY_NORMAL    0
  #define THRD_PRIORITY_ABOVE     1
  #define THRD_PRIORITY_HIGH      2
  #define THRD_PRIORITY_CRITICAL  3

  /* Set thread priority for current thread */
  static inline void thrd_set_priority(int priority) {
    extern __declspec(dllimport) void* __stdcall GetCurrentThread(void);
    extern __declspec(dllimport) int __stdcall SetThreadPriority(void*, int);
    /* Windows thread priorities: NORMAL=0, ABOVE_NORMAL=1, HIGHEST=2, TIME_CRITICAL=15 */
    int winprio = 0;  /* THREAD_PRIORITY_NORMAL */
    if (priority == THRD_PRIORITY_ABOVE) winprio = 1;          /* THREAD_PRIORITY_ABOVE_NORMAL */
    else if (priority == THRD_PRIORITY_HIGH) winprio = 2;      /* THREAD_PRIORITY_HIGHEST */
    else if (priority >= THRD_PRIORITY_CRITICAL) winprio = 15; /* THREAD_PRIORITY_TIME_CRITICAL */
    SetThreadPriority(GetCurrentThread(), winprio);
  }

  /* Process priority constants */
  #define PROC_PRIORITY_NORMAL      0
  #define PROC_PRIORITY_ABOVE       1
  #define PROC_PRIORITY_HIGH        2
  #define PROC_PRIORITY_CRITICAL    3

  /* Set process priority class - affects all threads including library-spawned ones */
  static inline void proc_set_priority(int priority) {
    extern __declspec(dllimport) void* __stdcall GetCurrentProcess(void);
    extern __declspec(dllimport) int __stdcall SetPriorityClass(void*, unsigned long);
    /* Windows priority classes */
    unsigned long prio_class = 0x00000020;  /* NORMAL_PRIORITY_CLASS */
    if (priority == PROC_PRIORITY_ABOVE) prio_class = 0x00008000;          /* ABOVE_NORMAL_PRIORITY_CLASS */
    else if (priority == PROC_PRIORITY_HIGH) prio_class = 0x00000080;      /* HIGH_PRIORITY_CLASS */
    else if (priority >= PROC_PRIORITY_CRITICAL) prio_class = 0x00000100;  /* REALTIME_PRIORITY_CLASS */
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
  #include <sched.h>
  #include <errno.h>
  #include <stdio.h>
  #include <stdlib.h>
  #include <sys/resource.h>
  #include <sys/types.h>
  #include <time.h>
  #include <sys/time.h>
  #include <unistd.h>
#if defined(__APPLE__)
  #include <pthread/qos.h>
#endif
#if defined(__linux__)
  #include <dirent.h>
  #include <sys/syscall.h>
#endif

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
  #define THRD_PRIORITY_NORMAL    0
  #define THRD_PRIORITY_ABOVE     1
  #define THRD_PRIORITY_HIGH      2
  #define THRD_PRIORITY_CRITICAL  3

  /* Set absolute nice value for current thread on Linux, otherwise process-wide fallback. */
  static inline int thrd_set_nice_target(int nice_target) {
#if defined(__linux__)
    pid_t tid = 0;
#if defined(SYS_gettid)
    tid = (pid_t)syscall(SYS_gettid);
#elif defined(__NR_gettid)
    tid = (pid_t)syscall(__NR_gettid);
#endif
    if (tid > 0) {
      return setpriority(PRIO_PROCESS, (id_t)tid, nice_target);
    }
#endif
    return setpriority(PRIO_PROCESS, 0, nice_target);
  }

  static inline int thrd_priority_to_nice_target(int priority) {
    if (priority == THRD_PRIORITY_ABOVE) return -5;
    if (priority == THRD_PRIORITY_HIGH) return -10;
    if (priority >= THRD_PRIORITY_CRITICAL) return -15;
    return 0;
  }

  static inline void thrd_log_priority_failure_once(const char *scope,
                                                    int priority,
                                                    int rt_err,
                                                    int nice_err) {
    static int warned_once = 0;
    if (warned_once) return;
    warned_once = 1;
    fprintf(stderr,
            "[THREAD] %s priority request %d could not be elevated (rt_err=%d, nice_err=%d)\n",
            scope, priority, rt_err, nice_err);
  }

  /* Try realtime scheduler for this thread (best-effort, usually requires elevated privileges). */
  static inline int thrd_try_realtime(int priority) {
#if defined(SCHED_FIFO) && defined(SCHED_RR)
    int policy = (priority >= THRD_PRIORITY_CRITICAL) ? SCHED_FIFO : SCHED_RR;
    int min_prio = sched_get_priority_min(policy);
    int max_prio = sched_get_priority_max(policy);
    if (min_prio < 0 || max_prio < min_prio) {
      return ENOTSUP;
    }

    struct sched_param param;
    if (priority >= THRD_PRIORITY_CRITICAL) {
      param.sched_priority = max_prio;
    } else {
      param.sched_priority = min_prio + ((max_prio - min_prio) * 3) / 4;
    }

    int rc = pthread_setschedparam(pthread_self(), policy, &param);
    if (rc == 0) {
      return 0;
    }

#if defined(__linux__)
    pid_t tid = 0;
#if defined(SYS_gettid)
    tid = (pid_t)syscall(SYS_gettid);
#elif defined(__NR_gettid)
    tid = (pid_t)syscall(__NR_gettid);
#endif
    if (tid > 0 && sched_setscheduler(tid, policy, &param) == 0) {
      return 0;
    }
    if (errno != 0) {
      return errno;
    }
#endif

    return (rc > 0) ? rc : EINVAL;
#else
    (void)priority;
    return ENOTSUP;
#endif
  }

#if defined(__linux__)
  static inline int proc_set_nice_all_threads(int nice_target) {
    DIR *task_dir = opendir("/proc/self/task");
    if (!task_dir) {
      return setpriority(PRIO_PROCESS, 0, nice_target);
    }

    int first_err = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(task_dir)) != NULL) {
      char *end = NULL;
      long tid = strtol(entry->d_name, &end, 10);
      if (end == entry->d_name || (end && *end != '\0') || tid <= 0) {
        continue;
      }
      if (setpriority(PRIO_PROCESS, (id_t)tid, nice_target) != 0 && first_err == 0) {
        first_err = (errno != 0) ? errno : EINVAL;
      }
    }
    closedir(task_dir);

    if (first_err != 0) {
      errno = first_err;
      return -1;
    }
    return 0;
  }
#endif

  /* Set thread priority for current thread with realtime->nice fallback (best-effort). */
  static inline void thrd_set_priority(int priority) {
#if defined(__APPLE__)
    /* macOS: use per-thread QoS classes as the primary priority control. */
    qos_class_t qos = QOS_CLASS_DEFAULT;
    int relpri = 0;
#if defined(__arm64__) || defined(__aarch64__)
    /* Apple Silicon: bias critical ingest/encode work toward P-cores. */
    if (priority >= THRD_PRIORITY_ABOVE) {
      qos = QOS_CLASS_USER_INTERACTIVE;
      if (priority >= THRD_PRIORITY_CRITICAL) relpri = 15;
      else if (priority >= THRD_PRIORITY_HIGH) relpri = 8;
      else relpri = 4;
    }
#else
    if (priority == THRD_PRIORITY_ABOVE) qos = QOS_CLASS_USER_INITIATED;
    else if (priority >= THRD_PRIORITY_HIGH) qos = QOS_CLASS_USER_INTERACTIVE;
    if (priority >= THRD_PRIORITY_CRITICAL) relpri = 15;
    else if (priority >= THRD_PRIORITY_HIGH) relpri = 8;
    else if (priority >= THRD_PRIORITY_ABOVE) relpri = 4;
#endif
    if (pthread_set_qos_class_self_np(qos, relpri) == 0) return;
#endif
    int rt_err = 0;
    int nice_err = 0;

    if (priority >= THRD_PRIORITY_HIGH) {
      rt_err = thrd_try_realtime(priority);
      if (rt_err == 0) return;
    }

    if (thrd_set_nice_target(thrd_priority_to_nice_target(priority)) == 0) {
      return;
    }

    nice_err = (errno != 0) ? errno : EINVAL;
    if (priority >= THRD_PRIORITY_ABOVE) {
      thrd_log_priority_failure_once("Thread", priority, rt_err, nice_err);
    }
  }

  /* Process priority constants */
  #define PROC_PRIORITY_NORMAL      0
  #define PROC_PRIORITY_ABOVE       1
  #define PROC_PRIORITY_HIGH        2
  #define PROC_PRIORITY_CRITICAL    3

  /* Set process priority with realtime->nice fallback (best-effort). */
  static inline void proc_set_priority(int priority) {
    int rt_err = 0;
    int nice_err = 0;
#if defined(__APPLE__)
    qos_class_t qos = QOS_CLASS_DEFAULT;
    int relpri = 0;
#if defined(__arm64__) || defined(__aarch64__)
    if (priority >= PROC_PRIORITY_ABOVE) {
      qos = QOS_CLASS_USER_INTERACTIVE;
      if (priority >= PROC_PRIORITY_CRITICAL) relpri = 15;
      else if (priority >= PROC_PRIORITY_HIGH) relpri = 8;
      else relpri = 4;
    }
#else
    if (priority == PROC_PRIORITY_ABOVE) qos = QOS_CLASS_USER_INITIATED;
    else if (priority >= PROC_PRIORITY_HIGH) qos = QOS_CLASS_USER_INTERACTIVE;
    if (priority >= PROC_PRIORITY_CRITICAL) relpri = 15;
    else if (priority >= PROC_PRIORITY_HIGH) relpri = 8;
    else if (priority >= PROC_PRIORITY_ABOVE) relpri = 4;
#endif
    (void)pthread_set_qos_class_self_np(qos, relpri);
#endif

#if defined(__linux__) && defined(SCHED_FIFO) && defined(SCHED_RR)
    if (priority >= PROC_PRIORITY_HIGH) {
      int policy = (priority >= PROC_PRIORITY_CRITICAL) ? SCHED_FIFO : SCHED_RR;
      int min_prio = sched_get_priority_min(policy);
      int max_prio = sched_get_priority_max(policy);
      if (min_prio >= 0 && max_prio >= min_prio) {
        struct sched_param param;
        param.sched_priority = (priority >= PROC_PRIORITY_CRITICAL)
                               ? max_prio
                               : (min_prio + ((max_prio - min_prio) * 3) / 4);
        if (sched_setscheduler(0, policy, &param) == 0) return;
        rt_err = (errno != 0) ? errno : EINVAL;
      } else {
        rt_err = ENOTSUP;
      }
    }
#endif

    int nice_target = 0;
    if (priority == PROC_PRIORITY_ABOVE) nice_target = -5;
    else if (priority == PROC_PRIORITY_HIGH) nice_target = -10;
    else if (priority >= PROC_PRIORITY_CRITICAL) nice_target = -15;

#if defined(__linux__)
    if (proc_set_nice_all_threads(nice_target) == 0) return;
#else
    if (setpriority(PRIO_PROCESS, 0, nice_target) == 0) return;
#endif
    nice_err = (errno != 0) ? errno : EINVAL;
    if (priority >= PROC_PRIORITY_ABOVE) {
      thrd_log_priority_failure_once("Process", priority, rt_err, nice_err);
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
