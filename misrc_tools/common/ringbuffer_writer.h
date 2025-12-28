/*
 * MISRC Common - Ringbuffer to File Writer
 *
 * Generic ringbuffer-to-file writer thread utilities.
 * Provides a reusable pattern for draining a ringbuffer to a file.
 */

#ifndef MISRC_RINGBUFFER_WRITER_H
#define MISRC_RINGBUFFER_WRITER_H

#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "ringbuffer.h"

/*-----------------------------------------------------------------------------
 * Callback Types
 *-----------------------------------------------------------------------------*/

/* Check if writer should exit
 *
 * @param user_ctx      User context
 * @return true if writer should exit after draining remaining data
 */
typedef bool (*rb_writer_should_exit_cb_t)(void *user_ctx);

/* Called after each write operation
 *
 * @param user_ctx      User context
 * @param bytes_written Number of bytes written in this operation
 */
typedef void (*rb_writer_progress_cb_t)(void *user_ctx, size_t bytes_written);

/*-----------------------------------------------------------------------------
 * Writer Configuration
 *-----------------------------------------------------------------------------*/

typedef struct {
    ringbuffer_t *rb;           /* Ringbuffer to read from */
    FILE *file;                 /* File to write to */
    size_t read_size;           /* Bytes to read per iteration */
    int sleep_ms;               /* Milliseconds to sleep when no data available */

    /* Exit condition - use ONE of these methods: */
    atomic_bool *exit_flag;     /* Atomic flag to check (simple method) */
    rb_writer_should_exit_cb_t should_exit_cb;  /* Callback method (flexible) */

    /* Optional callbacks */
    rb_writer_progress_cb_t progress_cb;  /* Called after each write */
    void *user_ctx;             /* User context for callbacks */
} rb_writer_config_t;

/*-----------------------------------------------------------------------------
 * Writer Functions
 *-----------------------------------------------------------------------------*/

/* Initialize writer config with defaults
 *
 * @param config        Config to initialize
 * @param rb            Ringbuffer to read from
 * @param file          File to write to
 * @param read_size     Bytes to read per iteration
 */
void rb_writer_config_init(rb_writer_config_t *config,
                           ringbuffer_t *rb,
                           FILE *file,
                           size_t read_size);

/* Run the writer loop (blocking)
 *
 * @param config        Writer configuration
 * @return Total bytes written
 *
 * This function blocks until the exit condition is met.
 * On exit, it drains any remaining data from the ringbuffer.
 */
size_t rb_writer_run(rb_writer_config_t *config);

/* Thread entry point wrapper
 *
 * @param ctx           Pointer to rb_writer_config_t
 * @return 0
 *
 * Convenience wrapper for use with thrd_create().
 * Calls rb_writer_run() and returns 0.
 */
int rb_writer_thread(void *ctx);

#endif /* MISRC_RINGBUFFER_WRITER_H */
