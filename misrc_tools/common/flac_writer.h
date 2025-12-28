/*
 * MISRC FLAC Writer - Shared FLAC encoding library
 *
 * Provides a unified API for FLAC encoding used by both CLI and GUI tools.
 * Supports configurable bit depth, compression, multi-threading, and seektables.
 */

#ifndef FLAC_WRITER_H
#define FLAC_WRITER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>

/* ============================================================================
 * Error Codes
 * ============================================================================ */
typedef enum {
    FLAC_WRITER_OK = 0,
    FLAC_WRITER_ERR_ALLOC,           // Failed to allocate encoder
    FLAC_WRITER_ERR_CONFIG,          // Failed to configure encoder
    FLAC_WRITER_ERR_THREADS,         // Failed to set thread count
    FLAC_WRITER_ERR_SEEKTABLE,       // Failed to create seektable
    FLAC_WRITER_ERR_INIT,            // Failed to initialize encoder
    FLAC_WRITER_ERR_PROCESS,         // Failed during encoding
    FLAC_WRITER_ERR_FINISH,          // Failed during finalization
    FLAC_WRITER_ERR_DISABLED         // FLAC support not compiled in
} flac_writer_error_t;

/* ============================================================================
 * Callback Types
 * ============================================================================ */

// Error reporting callback - allows consumer to handle errors their own way
// CLI: fprintf(stderr, ...)
// GUI: gui_app_set_status(app, ...)
typedef void (*flac_error_callback_t)(
    void *user_data,
    flac_writer_error_t error,
    const char *message
);

// Bytes written callback - for tracking compressed output size
// GUI uses this to update atomic counter for compression ratio display
typedef void (*flac_bytes_written_callback_t)(
    void *user_data,
    size_t bytes_written
);

/* ============================================================================
 * Configuration Structure
 * ============================================================================ */
typedef struct {
    // Core encoder settings
    uint32_t sample_rate;            // Sample rate in Hz (default: 40000)
    uint8_t bits_per_sample;         // 8, 12, or 16 (default: 16)
    uint8_t compression_level;       // 0-8 (default: 1)
    bool verify;                     // Enable verification (default: false)

    // Multi-threading (FLAC API v14+, requires libflac >= 1.5.0)
    uint32_t num_threads;            // 0 = auto-detect, 1 = single-threaded

    // Seektable configuration
    bool enable_seektable;           // Generate seektable metadata (default: true)
    uint32_t seektable_spacing;      // Samples between seek points (0 = default: 1<<18)

    // Callbacks (all optional - NULL disables)
    flac_error_callback_t error_cb;
    flac_bytes_written_callback_t bytes_cb;
    void *callback_user_data;        // Passed to all callbacks
} flac_writer_config_t;

/* ============================================================================
 * Writer Context (Opaque)
 * ============================================================================ */
typedef struct flac_writer flac_writer_t;

/* ============================================================================
 * API Functions
 * ============================================================================ */

// Create default configuration with sensible defaults
// Returns config: 40kHz, 16-bit, level 1, no verify, auto threads, seektable enabled
flac_writer_config_t flac_writer_default_config(void);

// Create writer and initialize encoder for FILE* output
// Uses FLAC__stream_encoder_init_FILE (CLI-style, simpler)
// The FILE* is NOT owned by the writer - caller must close it
flac_writer_t *flac_writer_create_file(
    FILE *output_file,
    const flac_writer_config_t *config
);

// Create writer and initialize encoder with stream callbacks
// Uses FLAC__stream_encoder_init_stream (GUI-style, enables byte tracking)
// The FILE* is NOT owned by the writer - caller must close it
flac_writer_t *flac_writer_create_stream(
    FILE *output_file,
    const flac_writer_config_t *config
);

// Process samples - samples are in int32_t format
// For 12-bit: samples should be sign-extended 12-bit values in int32_t
// For 16-bit: samples should be sign-extended 16-bit values in int32_t
// Returns number of samples successfully processed, or -1 on error
int flac_writer_process(
    flac_writer_t *writer,
    const int32_t *samples,
    uint32_t num_samples
);

// Process int16_t samples (convenience function)
// Internally converts to int32_t and calls flac_writer_process
// Use this when your buffer contains int16_t data
int flac_writer_process_int16(
    flac_writer_t *writer,
    const int16_t *samples,
    uint32_t num_samples
);

// Finalize encoding and clean up (writes final frames, fixes seektable)
// Returns FLAC_WRITER_OK on success
// After this call, the writer is destroyed and should not be used
flac_writer_error_t flac_writer_finish(flac_writer_t *writer);

// Abort without finalizing (useful for error paths)
// Does not write final frames or fix seektable
void flac_writer_abort(flac_writer_t *writer);

// Get last error message (for detailed diagnostics)
const char *flac_writer_get_error_string(flac_writer_t *writer);

// Get total samples written so far
uint64_t flac_writer_get_samples_written(flac_writer_t *writer);

// Get total compressed bytes written so far (stream mode only)
uint64_t flac_writer_get_bytes_written(flac_writer_t *writer);

// Check if FLAC support is compiled in
bool flac_writer_available(void);

// Get FLAC library version string (for diagnostics)
const char *flac_writer_get_flac_version(void);

// Check if multi-threading is available (requires FLAC API v14+)
bool flac_writer_multithreading_available(void);

#endif // FLAC_WRITER_H
