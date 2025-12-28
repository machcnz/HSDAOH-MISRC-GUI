/*
 * MISRC Common - Ringbuffer Event Primitives Implementation
 */

#include "rb_event.h"

#ifdef _WIN32

/* Windows implementation using Events */

/* Forward declare Windows API functions to avoid including windows.h */
extern __declspec(dllimport) void* __stdcall CreateEventA(void*, int, int, const char*);
extern __declspec(dllimport) int __stdcall SetEvent(void*);
extern __declspec(dllimport) int __stdcall ResetEvent(void*);
extern __declspec(dllimport) unsigned long __stdcall WaitForSingleObject(void*, unsigned long);
extern __declspec(dllimport) int __stdcall CloseHandle(void*);

#define INFINITE_WAIT 0xFFFFFFFF
#define WAIT_OBJECT_0_VAL 0

int rb_event_init(rb_event_t *event) {
    if (!event) return -1;

    /* Create auto-reset event, initially non-signaled */
    event->handle = CreateEventA(NULL, 0, 0, NULL);
    if (!event->handle) {
        event->initialized = false;
        return -1;
    }

    event->initialized = true;
    return 0;
}

void rb_event_signal(rb_event_t *event) {
    if (!event || !event->initialized) return;
    SetEvent(event->handle);
}

void rb_event_wait(rb_event_t *event) {
    if (!event || !event->initialized) return;
    WaitForSingleObject(event->handle, INFINITE_WAIT);
}

bool rb_event_wait_timeout(rb_event_t *event, uint32_t timeout_ms) {
    if (!event || !event->initialized) return false;
    return WaitForSingleObject(event->handle, timeout_ms) == WAIT_OBJECT_0_VAL;
}

void rb_event_destroy(rb_event_t *event) {
    if (!event || !event->initialized) return;
    CloseHandle(event->handle);
    event->handle = NULL;
    event->initialized = false;
}

#else

/* POSIX implementation using condition variables */

#include <errno.h>
#include <sys/time.h>
#include <time.h>

int rb_event_init(rb_event_t *event) {
    if (!event) return -1;

    if (pthread_mutex_init(&event->posix.mutex, NULL) != 0) {
        event->initialized = false;
        return -1;
    }

    if (pthread_cond_init(&event->posix.cond, NULL) != 0) {
        pthread_mutex_destroy(&event->posix.mutex);
        event->initialized = false;
        return -1;
    }

    event->posix.signaled = false;
    event->initialized = true;
    return 0;
}

void rb_event_signal(rb_event_t *event) {
    if (!event || !event->initialized) return;

    pthread_mutex_lock(&event->posix.mutex);
    event->posix.signaled = true;
    pthread_cond_signal(&event->posix.cond);
    pthread_mutex_unlock(&event->posix.mutex);
}

void rb_event_wait(rb_event_t *event) {
    if (!event || !event->initialized) return;

    pthread_mutex_lock(&event->posix.mutex);
    while (!event->posix.signaled) {
        pthread_cond_wait(&event->posix.cond, &event->posix.mutex);
    }
    event->posix.signaled = false;  /* Auto-reset */
    pthread_mutex_unlock(&event->posix.mutex);
}

bool rb_event_wait_timeout(rb_event_t *event, uint32_t timeout_ms) {
    if (!event || !event->initialized) return false;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    pthread_mutex_lock(&event->posix.mutex);
    while (!event->posix.signaled) {
        int rc = pthread_cond_timedwait(&event->posix.cond, &event->posix.mutex, &ts);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&event->posix.mutex);
            return false;
        }
    }
    event->posix.signaled = false;  /* Auto-reset */
    pthread_mutex_unlock(&event->posix.mutex);
    return true;
}

void rb_event_destroy(rb_event_t *event) {
    if (!event || !event->initialized) return;

    pthread_cond_destroy(&event->posix.cond);
    pthread_mutex_destroy(&event->posix.mutex);
    event->initialized = false;
}

#endif
