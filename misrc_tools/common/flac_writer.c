/*
 * MISRC FLAC Writer - Implementation
 *
 * Shared FLAC encoding library for CLI and GUI tools.
 */

#include "flac_writer.h"
#include <stdlib.h>
#include <string.h>

#if LIBFLAC_ENABLED == 1

#include "FLAC/stream_encoder.h"
#include "FLAC/metadata.h"

/* ============================================================================
 * Internal Writer Structure
 * ============================================================================ */
struct flac_writer {
    FLAC__StreamEncoder *encoder;
    FLAC__StreamMetadata *seektable;
    FILE *output_file;
    bool use_stream_callbacks;       // Stream mode vs FILE mode

    // Configuration (copy)
    flac_writer_config_t config;

    // Statistics
    uint64_t samples_written;
    uint64_t bytes_written;

    // Error state
    flac_writer_error_t last_error;
    char error_message[256];

    // Conversion buffer for int16->int32
    int32_t *conv_buffer;
    size_t conv_buffer_size;
};

/* ============================================================================
 * Thread count status strings (for FLAC API v14+)
 * ============================================================================ */
#if defined(FLAC_API_VERSION_CURRENT) && FLAC_API_VERSION_CURRENT >= 14
static const char* flac_thread_status_string(uint32_t status) {
    switch (status) {
        case FLAC__STREAM_ENCODER_SET_NUM_THREADS_OK:
            return "OK";
        case FLAC__STREAM_ENCODER_SET_NUM_THREADS_NOT_COMPILED_WITH_MULTITHREADING_ENABLED:
            return "Not compiled with multithreading";
        case FLAC__STREAM_ENCODER_SET_NUM_THREADS_ALREADY_INITIALIZED:
            return "Already initialized";
        case FLAC__STREAM_ENCODER_SET_NUM_THREADS_TOO_MANY_THREADS:
            return "Too many threads";
        default:
            return "Unknown error";
    }
}
#endif

/* ============================================================================
 * Stream Callbacks (for stream mode)
 * These callbacks enable libflac to update metadata (STREAMINFO, seektable)
 * after encoding is complete, which is required for proper file loading.
 * ============================================================================ */

// Write callback - writes encoded data to file and tracks bytes
static FLAC__StreamEncoderWriteStatus stream_write_callback(
    const FLAC__StreamEncoder *encoder,
    const FLAC__byte buffer[],
    size_t bytes,
    uint32_t samples,
    uint32_t current_frame,
    void *client_data)
{
    (void)encoder; (void)samples; (void)current_frame;
    flac_writer_t *writer = (flac_writer_t *)client_data;

    size_t written = fwrite(buffer, 1, bytes, writer->output_file);
    if (written != bytes) {
        return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
    }

    writer->bytes_written += bytes;

    // Call user callback if provided
    if (writer->config.bytes_cb) {
        writer->config.bytes_cb(writer->config.callback_user_data, bytes);
    }

    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

// Seek callback - allows libflac to seek back and update metadata
static FLAC__StreamEncoderSeekStatus stream_seek_callback(
    const FLAC__StreamEncoder *encoder,
    FLAC__uint64 absolute_byte_offset,
    void *client_data)
{
    (void)encoder;
    flac_writer_t *writer = (flac_writer_t *)client_data;

    if (fseek(writer->output_file, (long)absolute_byte_offset, SEEK_SET) < 0) {
        return FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
    }
    return FLAC__STREAM_ENCODER_SEEK_STATUS_OK;
}

// Tell callback - reports current file position to libflac
static FLAC__StreamEncoderTellStatus stream_tell_callback(
    const FLAC__StreamEncoder *encoder,
    FLAC__uint64 *absolute_byte_offset,
    void *client_data)
{
    (void)encoder;
    flac_writer_t *writer = (flac_writer_t *)client_data;

    long pos = ftell(writer->output_file);
    if (pos < 0) {
        return FLAC__STREAM_ENCODER_TELL_STATUS_ERROR;
    }
    *absolute_byte_offset = (FLAC__uint64)pos;
    return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
}

/* ============================================================================
 * Error Reporting Helper
 * ============================================================================ */
static void report_error(flac_writer_t *writer, flac_writer_error_t error, const char *msg) {
    writer->last_error = error;
    strncpy(writer->error_message, msg, sizeof(writer->error_message) - 1);
    writer->error_message[sizeof(writer->error_message) - 1] = '\0';

    if (writer->config.error_cb) {
        writer->config.error_cb(writer->config.callback_user_data, error, msg);
    }
}

/* ============================================================================
 * Default Configuration
 * ============================================================================ */
flac_writer_config_t flac_writer_default_config(void) {
    flac_writer_config_t config = {
        .sample_rate = 40000,
        .bits_per_sample = 16,
        .compression_level = 1,
        .verify = false,
        .num_threads = 0,  // Auto-detect
        .enable_seektable = true,
        .seektable_spacing = 1 << 18,  // ~6.5 seconds at 40kHz
        .error_cb = NULL,
        .bytes_cb = NULL,
        .callback_user_data = NULL
    };
    return config;
}

/* ============================================================================
 * Internal: Configure Encoder (common setup for both modes)
 * ============================================================================ */
static flac_writer_error_t configure_encoder(flac_writer_t *writer) {
    FLAC__StreamEncoder *enc = writer->encoder;
    FLAC__bool ok = true;

    ok &= FLAC__stream_encoder_set_verify(enc, writer->config.verify);
    ok &= FLAC__stream_encoder_set_compression_level(enc, writer->config.compression_level);
    ok &= FLAC__stream_encoder_set_channels(enc, 1);  // Always mono for MISRC
    ok &= FLAC__stream_encoder_set_bits_per_sample(enc, writer->config.bits_per_sample);
    ok &= FLAC__stream_encoder_set_sample_rate(enc, writer->config.sample_rate);
    ok &= FLAC__stream_encoder_set_total_samples_estimate(enc, 0);  // Unknown length

    if (!ok) {
        report_error(writer, FLAC_WRITER_ERR_CONFIG, "Failed to configure FLAC encoder parameters");
        return FLAC_WRITER_ERR_CONFIG;
    }

    // Multi-threading (FLAC API v14+)
#if defined(FLAC_API_VERSION_CURRENT) && FLAC_API_VERSION_CURRENT >= 14
    if (writer->config.num_threads != 1) {  // 1 means explicitly single-threaded
        uint32_t threads = writer->config.num_threads;
        uint32_t status = FLAC__stream_encoder_set_num_threads(enc, threads);
        if (status != FLAC__STREAM_ENCODER_SET_NUM_THREADS_OK) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Failed to set FLAC threads: %s",
                     flac_thread_status_string(status));
            report_error(writer, FLAC_WRITER_ERR_THREADS, msg);
            // Non-fatal - continue with default threading
        }
    }
#endif

    // Seektable
    if (writer->config.enable_seektable) {
        writer->seektable = FLAC__metadata_object_new(FLAC__METADATA_TYPE_SEEKTABLE);
        if (!writer->seektable) {
            report_error(writer, FLAC_WRITER_ERR_SEEKTABLE, "Failed to allocate seektable");
            return FLAC_WRITER_ERR_SEEKTABLE;
        }

        uint32_t spacing = writer->config.seektable_spacing;
        if (spacing == 0) spacing = 1 << 18;

        // Estimate for very long recordings (up to ~1.5 years at 40kHz)
        if (!FLAC__metadata_object_seektable_template_append_spaced_points(
                writer->seektable, spacing, (uint64_t)1 << 41) ||
            !FLAC__stream_encoder_set_metadata(enc, &writer->seektable, 1)) {
            report_error(writer, FLAC_WRITER_ERR_SEEKTABLE, "Failed to configure seektable");
            FLAC__metadata_object_delete(writer->seektable);
            writer->seektable = NULL;
            return FLAC_WRITER_ERR_SEEKTABLE;
        }
    }

    return FLAC_WRITER_OK;
}

/* ============================================================================
 * Create Writer (FILE mode)
 * ============================================================================ */
flac_writer_t *flac_writer_create_file(FILE *output_file, const flac_writer_config_t *config) {
    flac_writer_t *writer = calloc(1, sizeof(flac_writer_t));
    if (!writer) return NULL;

    writer->config = *config;
    writer->output_file = output_file;
    writer->use_stream_callbacks = false;

    writer->encoder = FLAC__stream_encoder_new();
    if (!writer->encoder) {
        report_error(writer, FLAC_WRITER_ERR_ALLOC, "Failed to allocate FLAC encoder");
        free(writer);
        return NULL;
    }

    flac_writer_error_t err = configure_encoder(writer);
    if (err != FLAC_WRITER_OK) {
        FLAC__stream_encoder_delete(writer->encoder);
        if (writer->seektable) FLAC__metadata_object_delete(writer->seektable);
        free(writer);
        return NULL;
    }

    FLAC__StreamEncoderInitStatus init_status =
        FLAC__stream_encoder_init_FILE(writer->encoder, output_file, NULL, NULL);

    if (init_status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        char msg[256];
        snprintf(msg, sizeof(msg), "FLAC init failed: %s",
                 FLAC__StreamEncoderInitStatusString[init_status]);
        report_error(writer, FLAC_WRITER_ERR_INIT, msg);
        FLAC__stream_encoder_delete(writer->encoder);
        if (writer->seektable) FLAC__metadata_object_delete(writer->seektable);
        free(writer);
        return NULL;
    }

    return writer;
}

/* ============================================================================
 * Create Writer (Stream callback mode)
 * ============================================================================ */
flac_writer_t *flac_writer_create_stream(FILE *output_file, const flac_writer_config_t *config) {
    flac_writer_t *writer = calloc(1, sizeof(flac_writer_t));
    if (!writer) return NULL;

    writer->config = *config;
    writer->output_file = output_file;
    writer->use_stream_callbacks = true;

    writer->encoder = FLAC__stream_encoder_new();
    if (!writer->encoder) {
        report_error(writer, FLAC_WRITER_ERR_ALLOC, "Failed to allocate FLAC encoder");
        free(writer);
        return NULL;
    }

    flac_writer_error_t err = configure_encoder(writer);
    if (err != FLAC_WRITER_OK) {
        FLAC__stream_encoder_delete(writer->encoder);
        if (writer->seektable) FLAC__metadata_object_delete(writer->seektable);
        free(writer);
        return NULL;
    }

    // Use stream mode with seek/tell callbacks so libflac can update
    // STREAMINFO and seektable metadata after encoding completes.
    // This is critical for proper file loading in players like Audacity.
    FLAC__StreamEncoderInitStatus init_status =
        FLAC__stream_encoder_init_stream(
            writer->encoder,
            stream_write_callback,
            stream_seek_callback,   // enables metadata updates
            stream_tell_callback,   // enables metadata updates
            NULL,                   // metadata callback (not needed with seek/tell)
            writer
        );

    if (init_status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        char msg[256];
        snprintf(msg, sizeof(msg), "FLAC stream init failed: %s",
                 FLAC__StreamEncoderInitStatusString[init_status]);
        report_error(writer, FLAC_WRITER_ERR_INIT, msg);
        FLAC__stream_encoder_delete(writer->encoder);
        if (writer->seektable) FLAC__metadata_object_delete(writer->seektable);
        free(writer);
        return NULL;
    }

    return writer;
}

/* ============================================================================
 * Process Samples (int32_t)
 * ============================================================================ */
int flac_writer_process(flac_writer_t *writer, const int32_t *samples, uint32_t num_samples) {
    if (!writer || !samples || num_samples == 0) return -1;

    // FLAC expects pointer to array of pointers (for multi-channel)
    // For mono, we pass address of our single pointer
    const FLAC__int32 *channel_ptrs[1] = { samples };

    FLAC__bool ok = FLAC__stream_encoder_process(writer->encoder, channel_ptrs, num_samples);
    if (!ok) {
        char msg[256];
        snprintf(msg, sizeof(msg), "FLAC process error: %s",
                 FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(writer->encoder)]);
        report_error(writer, FLAC_WRITER_ERR_PROCESS, msg);
        return -1;
    }

    writer->samples_written += num_samples;
    return (int)num_samples;
}

/* ============================================================================
 * Process Samples (int16_t convenience)
 * ============================================================================ */
int flac_writer_process_int16(flac_writer_t *writer, const int16_t *samples, uint32_t num_samples) {
    if (!writer || !samples || num_samples == 0) return -1;

    // Ensure conversion buffer is large enough
    if (writer->conv_buffer_size < num_samples) {
        int32_t *new_buf = realloc(writer->conv_buffer, num_samples * sizeof(int32_t));
        if (!new_buf) {
            report_error(writer, FLAC_WRITER_ERR_ALLOC, "Failed to allocate conversion buffer");
            return -1;
        }
        writer->conv_buffer = new_buf;
        writer->conv_buffer_size = num_samples;
    }

    // Convert int16 to int32 (sign-extend)
    for (uint32_t i = 0; i < num_samples; i++) {
        writer->conv_buffer[i] = samples[i];
    }

    return flac_writer_process(writer, writer->conv_buffer, num_samples);
}

/* ============================================================================
 * Finish Encoding
 * ============================================================================ */
flac_writer_error_t flac_writer_finish(flac_writer_t *writer) {
    if (!writer) return FLAC_WRITER_ERR_ALLOC;

    // Sort seektable before finishing
    if (writer->seektable) {
        FLAC__metadata_object_seektable_template_sort(writer->seektable, false);

        // Fix seektable for libflac < 1.5 (set unused points to placeholder)
#if !defined(FLAC_API_VERSION_CURRENT) || FLAC_API_VERSION_CURRENT < 14
        for (int i = (int)writer->seektable->data.seek_table.num_points - 1; i >= 0; i--) {
            if (writer->seektable->data.seek_table.points[i].stream_offset != 0) break;
            writer->seektable->data.seek_table.points[i].sample_number = 0xFFFFFFFFFFFFFFFFULL;
        }
#endif
    }

    FLAC__bool ok = FLAC__stream_encoder_finish(writer->encoder);
    flac_writer_error_t result = FLAC_WRITER_OK;

    if (!ok) {
        char msg[256];
        snprintf(msg, sizeof(msg), "FLAC finish error: %s",
                 FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(writer->encoder)]);
        report_error(writer, FLAC_WRITER_ERR_FINISH, msg);
        result = FLAC_WRITER_ERR_FINISH;
    }

    // Cleanup
    if (writer->seektable) FLAC__metadata_object_delete(writer->seektable);
    FLAC__stream_encoder_delete(writer->encoder);
    if (writer->conv_buffer) free(writer->conv_buffer);
    free(writer);

    return result;
}

/* ============================================================================
 * Abort (cleanup without finalizing)
 * ============================================================================ */
void flac_writer_abort(flac_writer_t *writer) {
    if (!writer) return;

    if (writer->seektable) FLAC__metadata_object_delete(writer->seektable);
    FLAC__stream_encoder_delete(writer->encoder);
    if (writer->conv_buffer) free(writer->conv_buffer);
    free(writer);
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */
const char *flac_writer_get_error_string(flac_writer_t *writer) {
    return writer ? writer->error_message : "NULL writer";
}

uint64_t flac_writer_get_samples_written(flac_writer_t *writer) {
    return writer ? writer->samples_written : 0;
}

uint64_t flac_writer_get_bytes_written(flac_writer_t *writer) {
    return writer ? writer->bytes_written : 0;
}

bool flac_writer_available(void) {
    return true;
}

const char *flac_writer_get_flac_version(void) {
    return FLAC__VERSION_STRING;
}

bool flac_writer_multithreading_available(void) {
#if defined(FLAC_API_VERSION_CURRENT) && FLAC_API_VERSION_CURRENT >= 14
    return true;
#else
    return false;
#endif
}

#else // LIBFLAC_ENABLED != 1

/* ============================================================================
 * Stub implementations when FLAC is disabled
 * ============================================================================ */

flac_writer_config_t flac_writer_default_config(void) {
    flac_writer_config_t config = {0};
    return config;
}

flac_writer_t *flac_writer_create_file(FILE *f, const flac_writer_config_t *c) {
    (void)f; (void)c;
    return NULL;
}

flac_writer_t *flac_writer_create_stream(FILE *f, const flac_writer_config_t *c) {
    (void)f; (void)c;
    return NULL;
}

int flac_writer_process(flac_writer_t *w, const int32_t *s, uint32_t n) {
    (void)w; (void)s; (void)n;
    return -1;
}

int flac_writer_process_int16(flac_writer_t *w, const int16_t *s, uint32_t n) {
    (void)w; (void)s; (void)n;
    return -1;
}

flac_writer_error_t flac_writer_finish(flac_writer_t *w) {
    (void)w;
    return FLAC_WRITER_ERR_DISABLED;
}

void flac_writer_abort(flac_writer_t *w) { (void)w; }

const char *flac_writer_get_error_string(flac_writer_t *w) {
    (void)w;
    return "FLAC support not compiled in";
}

uint64_t flac_writer_get_samples_written(flac_writer_t *w) { (void)w; return 0; }
uint64_t flac_writer_get_bytes_written(flac_writer_t *w) { (void)w; return 0; }
bool flac_writer_available(void) { return false; }
const char *flac_writer_get_flac_version(void) { return "N/A"; }
bool flac_writer_multithreading_available(void) { return false; }

#endif // LIBFLAC_ENABLED
