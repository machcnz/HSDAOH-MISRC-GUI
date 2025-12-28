/*
 * MISRC Common - Ringbuffer Event Primitives
 *
 * Platform-agnostic event signaling for producer/consumer synchronization.
 * Used to efficiently wake threads waiting for ringbuffer data without polling.
 */

#ifndef MISRC_RB_EVENT_H
#define MISRC_RB_EVENT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
  /* Forward declare HANDLE to avoid including windows.h */
  typedef void* rb_event_handle_t;
#else
  #include <pthread.h>
  typedef struct {
      pthread_mutex_t mutex;
      pthread_cond_t cond;
      bool signaled;
  } rb_event_posix_t;
#endif

/* Platform-agnostic event type */
typedef struct {
#ifdef _WIN32
    rb_event_handle_t handle;
#else
    rb_event_posix_t posix;
#endif
    bool initialized;
} rb_event_t;

/*
 * Initialize an event
 *
 * @param event     Event to initialize
 * @return 0 on success, -1 on failure
 */
int rb_event_init(rb_event_t *event);

/*
 * Signal the event (wake waiting thread)
 *
 * @param event     Event to signal
 *
 * This is safe to call from any thread, including signal handlers on POSIX.
 */
void rb_event_signal(rb_event_t *event);

/*
 * Wait for the event to be signaled (blocking)
 *
 * @param event     Event to wait on
 *
 * Blocks until the event is signaled. The event is automatically reset
 * after waking (auto-reset behavior).
 */
void rb_event_wait(rb_event_t *event);

/*
 * Wait for the event with a timeout
 *
 * @param event     Event to wait on
 * @param timeout_ms Maximum time to wait in milliseconds
 * @return true if event was signaled, false if timeout expired
 *
 * The event is automatically reset after waking if signaled.
 */
bool rb_event_wait_timeout(rb_event_t *event, uint32_t timeout_ms);

/*
 * Destroy the event and free resources
 *
 * @param event     Event to destroy
 */
void rb_event_destroy(rb_event_t *event);

#endif /* MISRC_RB_EVENT_H */
