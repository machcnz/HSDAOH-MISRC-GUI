/*
 * hsdaoh - High Speed Data Acquisition over MS213x USB3 HDMI capture sticks
 *
 * Copyright (C) 2024 by Steve Markgraf <steve@steve-m.de>
 * Enhanced with clipping detection, status display, and level monitoring - 2025
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>

#ifndef _WIN32
#include <unistd.h>
#include <getopt.h>
#else
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <getopt.h>
#define usleep(t) Sleep((t)/1000)
#endif

#include "FLAC/metadata.h"
#include "FLAC/stream_encoder.h"

#include "hsdaoh.h"

#define FD_NUMS				6
#define LEVEL_WINDOW_MS		250  // 250ms window for level calculations

/* Statistics per stream */
typedef struct stream_stats {
	uint64_t sample_count;
	uint64_t clip_count;
	double last_clip_time;
	bool active;
	
	/* Level monitoring */
	double sum_squares;      // For RMS calculation
	int64_t sum_samples;     // For DC offset calculation
	uint64_t window_samples; // Samples in current window
	int32_t peak_value;      // Peak in current window
	int bits_per_samp;
	bool is_signed;
	uint32_t sample_rate;
} stream_stats_t;

static bool do_exit = false;
static hsdaoh_dev_t *dev = NULL;
static uint32_t flac_level = 5;
static uint32_t flac_nthreads = 4;
static bool level_check_mode = false;

static stream_stats_t stream_stats[FD_NUMS] = {0};
static struct timeval start_time;
static double last_status_update = 0.0;
static double last_level_update = 0.0;

typedef struct file_ctx {
	FILE *files[FD_NUMS];
	bool use_flac[FD_NUMS];
	FLAC__StreamEncoder *encoder[FD_NUMS];
	FLAC__StreamMetadata *seektable[FD_NUMS];
} file_ctx_t;

void usage(void)
{
    fprintf(stderr,
        "hsdaoh_file, HDMI data acquisition tool\n\n"
        "Usage:\n"
        "\t[-d device_index (default: 0)]\n"
        "\t[-b maximum number of buffers (default: 96)]\n"
        "\t[-l FLAC compression level (default: 5)]\n"
#ifdef FLAC__STREAM_ENCODER_SET_NUM_THREADS_OK
        "\t[-t number of threads for FLAC encoding (default: 4)]\n"
#endif
        "\t[--level-check] monitor signal levels without capturing (same as -L)\n"
        "\t[-0 to -5 filename of stream 0 to stream 5 (a '-' dumps samples to stdout)]\n"
        "\tfilename (of stream 0) (a '-' dumps samples to stdout)\n\n"
        "\tFilenames with the extension .flac will enable FLAC encoding\n\n");
    exit(1);
}


#ifdef _WIN32
BOOL WINAPI
sighandler(int signum)
{
	if (CTRL_C_EVENT == signum) {
		fprintf(stderr, "\nSignal caught, exiting!\n");
		do_exit = true;
		hsdaoh_stop_stream(dev);
		return TRUE;
	}
	return FALSE;
}
#else
static void sighandler(int signum)
{
	signal(SIGPIPE, SIG_IGN);
	fprintf(stderr, "\nSignal caught, exiting!\n");
	do_exit = true;
	hsdaoh_stop_stream(dev);
}
#endif

static double get_elapsed_time(void)
{
	struct timeval now;
	gettimeofday(&now, NULL);
	return (now.tv_sec - start_time.tv_sec) + 
	       (now.tv_usec - start_time.tv_usec) / 1000000.0;
}

static inline bool is_clipped(uint32_t sample, int bits_per_samp, bool is_signed)
{
	if (is_signed) {
		int32_t val = (int32_t)sample;
		int32_t max_val = (1 << (bits_per_samp - 1)) - 1;
		int32_t min_val = -(1 << (bits_per_samp - 1));
		return (val == max_val || val == min_val);
	} else {
		uint32_t max_val = (1 << bits_per_samp) - 1;
		return (sample == 0 || sample == max_val);
	}
}

static void update_level_stats(stream_stats_t *stats, int32_t sample, bool is_signed, int bits_per_samp)
{
	/* For unsigned data, center it first by subtracting midpoint */
	int32_t centered_sample = sample;
	if (!is_signed) {
		int32_t midpoint = 1 << (bits_per_samp - 1);
		centered_sample = sample - midpoint;
	}
	
	/* Update RMS (sum of squares) - use centered value */
	stats->sum_squares += (double)centered_sample * (double)centered_sample;
	
	/* Update DC offset (sum of samples) - use centered value */
	stats->sum_samples += centered_sample;
	
	/* Update peak - use centered value */
	int32_t abs_sample = abs(centered_sample);
	if (abs_sample > stats->peak_value)
		stats->peak_value = abs_sample;
	
	stats->window_samples++;
}

static void display_levels(void)
{
	double elapsed = get_elapsed_time();
	
	/* Update at ~4 Hz for smooth display */
	if (elapsed - last_level_update < 0.25)
		return;
	
	last_level_update = elapsed;
	
	char status[1024];
	int pos = 0;
	
	pos += snprintf(status + pos, sizeof(status) - pos, "\r[ %.1fs ]", elapsed);
	
	for (int i = 0; i < FD_NUMS; i++) {
		if (!stream_stats[i].active || stream_stats[i].window_samples == 0)
			continue;
		
		stream_stats_t *s = &stream_stats[i];
		
		/* Calculate RMS in dBFS */
		double rms = sqrt(s->sum_squares / s->window_samples);
		int32_t full_scale = (1 << (s->bits_per_samp - 1));
		double rms_dbfs = 20.0 * log10(rms / full_scale);
		if (rms_dbfs < -96.0) rms_dbfs = -96.0;  // Floor at -96dBFS
		
		/* Calculate Peak in dBFS */
		double peak_dbfs = 20.0 * log10((double)s->peak_value / full_scale);
		if (peak_dbfs < -96.0) peak_dbfs = -96.0;
		
		/* Calculate DC offset as percentage */
		double dc_avg = (double)s->sum_samples / s->window_samples;
		double dc_percent = (dc_avg / full_scale) * 100.0;
		
		pos += snprintf(status + pos, sizeof(status) - pos,
		               " Stream-%d: RMS:%.1fdB Peak:%.1fdB DC:%+.1f%%",
		               i, rms_dbfs, peak_dbfs, dc_percent);
		
		pos += snprintf(status + pos, sizeof(status) - pos, " |");
		
		/* Reset window */
		s->sum_squares = 0.0;
		s->sum_samples = 0;
		s->peak_value = 0;
		s->window_samples = 0;
	}
	
	write(2, status, strlen(status));
}

static void update_status_display(void)
{
	double elapsed = get_elapsed_time();
	
	/* Only update every 1 second to reduce overhead */
	if (elapsed - last_status_update < 1.0)
		return;
	
	last_status_update = elapsed;
	
	char status[512];
	int pos = 0;
	
	pos += snprintf(status + pos, sizeof(status) - pos, "\r[ %.1fs ]", elapsed);
	
	for (int i = 0; i < FD_NUMS; i++) {
		if (!stream_stats[i].active)
			continue;
		
		pos += snprintf(status + pos, sizeof(status) - pos,
		               " Stream-%d: Samples:%lu Clips:%lu",
		               i,
		               (unsigned long)stream_stats[i].sample_count,
		               (unsigned long)stream_stats[i].clip_count);
		
		if (stream_stats[i].clip_count > 0) {
			pos += snprintf(status + pos, sizeof(status) - pos,
			               " (last:%.1fs)", stream_stats[i].last_clip_time);
		}
		
		pos += snprintf(status + pos, sizeof(status) - pos, " |");
	}
	
	write(2, status, strlen(status));
}

static void hsdaoh_callback(hsdaoh_data_info_t *data_info)
{
	size_t nbytes = 0;
	uint32_t len = data_info->len;

	if (!data_info->ctx || do_exit)
		return;

	if (data_info->stream_id >= FD_NUMS)
		return;
       
	file_ctx_t *f = (file_ctx_t *)data_info->ctx;
	
	/* In level check mode, don't require files */
	FILE *file = NULL;
	if (!level_check_mode) {
		file = f->files[data_info->stream_id];
		if (!file)
			return;
	}

	if (!stream_stats[data_info->stream_id].active) {
		stream_stats[data_info->stream_id].active = true;
		stream_stats[data_info->stream_id].bits_per_samp = data_info->bits_per_samp;
		stream_stats[data_info->stream_id].is_signed = data_info->is_signed;
		stream_stats[data_info->stream_id].sample_rate = data_info->srate;
	}

	int bytes_per_samp = ((data_info->bits_per_samp - 1) / 8) + 1;
	int nsamps = len / bytes_per_samp;
	
	stream_stats[data_info->stream_id].sample_count += nsamps;
	
	uint64_t clips_in_buffer = 0;
	
	if (bytes_per_samp == 1) {
		uint8_t *dat = (uint8_t *)data_info->buf;
		for (int i = 0; i < nsamps; i++) {
			int32_t sample = data_info->is_signed ? (int8_t)dat[i] : dat[i];
			if (level_check_mode)
				update_level_stats(&stream_stats[data_info->stream_id], sample, 
				                  data_info->is_signed, data_info->bits_per_samp);
			if (is_clipped(dat[i], data_info->bits_per_samp, data_info->is_signed))
				clips_in_buffer++;
		}
	} else if (bytes_per_samp == 2) {
		uint16_t *dat = (uint16_t *)data_info->buf;
		for (int i = 0; i < nsamps; i++) {
			int32_t sample = data_info->is_signed ? (int16_t)dat[i] : dat[i];
			if (level_check_mode)
				update_level_stats(&stream_stats[data_info->stream_id], sample,
				                  data_info->is_signed, data_info->bits_per_samp);
			if (is_clipped(dat[i], data_info->bits_per_samp, data_info->is_signed))
				clips_in_buffer++;
		}
	} else if (bytes_per_samp == 3) {
		uint8_t *dat = (uint8_t *)data_info->buf;
		int j = 0;
		for (int i = 0; i < nsamps; i++) {
			int32_t samp = (int32_t)((dat[j+2] << 24) | (dat[j+1] << 16) | (dat[j] << 8));
			samp >>= (32 - data_info->bits_per_samp);
			if (level_check_mode)
				update_level_stats(&stream_stats[data_info->stream_id], samp,
				                  data_info->is_signed, data_info->bits_per_samp);
			if (is_clipped(samp, data_info->bits_per_samp, data_info->is_signed))
				clips_in_buffer++;
			j += 3;
		}
	} else {
		uint32_t *dat = (uint32_t *)data_info->buf;
		for (int i = 0; i < nsamps; i++) {
			int32_t sample = data_info->is_signed ? (int32_t)dat[i] : dat[i];
			if (level_check_mode)
				update_level_stats(&stream_stats[data_info->stream_id], sample,
				                  data_info->is_signed, data_info->bits_per_samp);
			if (is_clipped(dat[i], data_info->bits_per_samp, data_info->is_signed))
				clips_in_buffer++;
		}
	}
	
	if (clips_in_buffer > 0) {
		stream_stats[data_info->stream_id].clip_count += clips_in_buffer;
		stream_stats[data_info->stream_id].last_clip_time = get_elapsed_time();
	}
	
	/* Display appropriate status */
	if (level_check_mode) {
		display_levels();
		return;  // Don't write to files in level check mode
	} else {
		update_status_display();
	}

	/* File writing (only in normal capture mode) */
	FLAC__StreamEncoder *encoder = f->encoder[data_info->stream_id];
	if (f->use_flac[data_info->stream_id] && !encoder) {
		FLAC__bool ret = true;
		FLAC__StreamEncoderInitStatus init_status;

		if ((encoder = FLAC__stream_encoder_new()) == NULL) {
			fprintf(stderr, "\nERROR: failed allocating FLAC encoder\n");
			do_exit = true;
			return;
		}

		ret &= FLAC__stream_encoder_set_verify(encoder, false);
		ret &= FLAC__stream_encoder_set_compression_level(encoder, flac_level);
		ret &= FLAC__stream_encoder_set_sample_rate(encoder,
							   data_info->srate > FLAC__MAX_SAMPLE_RATE ?
							   data_info->srate/1000 : data_info->srate);

		ret &= FLAC__stream_encoder_set_channels(encoder, data_info->nchans);
		ret &= FLAC__stream_encoder_set_bits_per_sample(encoder, data_info->bits_per_samp);
		ret &= FLAC__stream_encoder_set_total_samples_estimate(encoder, 0);
		ret &= FLAC__stream_encoder_set_streamable_subset(encoder, false);

#ifdef FLAC__STREAM_ENCODER_SET_NUM_THREADS_OK
		if (FLAC__stream_encoder_set_num_threads(encoder, flac_nthreads) != FLAC__STREAM_ENCODER_SET_NUM_THREADS_OK)
			ret = false;
#endif

		if (!ret) {
			fprintf(stderr, "\nERROR: failed initializing FLAC encoder\n");
			do_exit = true;
			return;
		}

		init_status = FLAC__stream_encoder_init_FILE(encoder, f->files[data_info->stream_id], NULL, NULL);
		if (init_status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
			fprintf(stderr, "\nERROR: failed initializing FLAC encoder: %s\n", FLAC__StreamEncoderInitStatusString[init_status]);
			do_exit = true;
			return;
		}

		f->encoder[data_info->stream_id] = encoder;
	}

	if (f->use_flac[data_info->stream_id]) {
		FLAC__bool ok = false;
		FLAC__int32 offset = data_info->is_signed ? 0 : 1 << (data_info->bits_per_samp - 1);
		FLAC__int32 *out = malloc(nsamps * sizeof(FLAC__int32));
		int i = 0;

		if (bytes_per_samp == 1) {
			uint8_t *dat = (uint8_t *)data_info->buf;
			for (; i < nsamps; i++)
				out[i] = dat[i] - offset;
		} else if (bytes_per_samp == 2) {
			uint16_t *dat = (uint16_t *)data_info->buf;
			for (; i < nsamps; i++)
				out[i] = dat[i] - offset;
		} else if (bytes_per_samp == 3) {
			uint8_t *dat = (uint8_t *)data_info->buf;
			int j = 0;
			for (; i < nsamps; i++) {
				FLAC__int32 samp = (FLAC__int32)((dat[j+2] << 24) | (dat[j+1] << 16) | (dat[j] << 8));
				samp >>= (32 - data_info->bits_per_samp);
				j += 3;
				out[i] = samp - offset;
			}
		} else {
			uint32_t *dat = (uint32_t *)data_info->buf;
			for (; i < nsamps; i++)
				out[i] = dat[i] - offset;
		}

		ok = FLAC__stream_encoder_process_interleaved(f->encoder[data_info->stream_id], out, nsamps / data_info->nchans);
		free(out);

		if (!ok && encoder) {
			fprintf(stderr, "\nERROR: FLAC encoder could not process data: %s\n",
					FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(encoder)]);
			hsdaoh_stop_stream(dev);
		}
	} else {
		while (nbytes < len) {
			nbytes += fwrite(data_info->buf + nbytes, 1, len - nbytes, file);
			if (ferror(file)) {
				fprintf(stderr, "\nError writing file, samples lost, exiting!\n");
				hsdaoh_stop_stream(dev);
				break;
			}
		}
	}
}

int main(int argc, char **argv)
{
	setbuf(stderr, NULL);
#ifndef _WIN32
	struct sigaction sigact;
#endif
	char *filenames[FD_NUMS] = { NULL, };
	int n_read;
	int r, opt;
	file_ctx_t f = { 0 };
	int dev_index = 0;
	unsigned int num_bufs = 0;
	bool fname0_used = false;
	bool have_file = false;

	static struct option long_options[] = {
		{"level-check", no_argument, 0, 'L'},
		{0, 0, 0, 0}
	};

	int option_index = 0;
	while ((opt = getopt_long(argc, argv, "0:1:2:3:4:5:d:b:l:t:", long_options, &option_index)) != -1) {
		if (isdigit(opt)) {
			int num = opt - 0x30;
			have_file = true;
			filenames[num] = optarg;
			if (num == 0)
				fname0_used = true;
			continue;
		}

		switch (opt) {
		case 'd':
			dev_index = (uint32_t)atoi(optarg);
			break;
		case 'b':
			num_bufs = (unsigned int)atoi(optarg);
			break;
		case 'l':
			flac_level = atoi(optarg);
			break;
		case 't':
			flac_nthreads = atoi(optarg);
			break;
		case 'L':
			level_check_mode = true;
			break;
		default:
			usage();
			break;
		}
	}

	if (!level_check_mode) {
		if (!fname0_used) {
			if (argc <= optind) {
				if (!have_file)
					usage();
			} else
				filenames[0] = argv[optind];
		}
	}

	if (dev_index < 0)
		exit(1);

	r = hsdaoh_open(&dev, (uint32_t)dev_index);
	if (r < 0) {
		fprintf(stderr, "Failed to open hsdaoh device #%d.\n", dev_index);
		exit(1);
	}
#ifndef _WIN32
	sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);
	sigaction(SIGPIPE, &sigact, NULL);
#else
	SetConsoleCtrlHandler( (PHANDLER_ROUTINE) sighandler, TRUE );
#endif

	if (!level_check_mode) {
		for (int i = 0; i < FD_NUMS; i++) {
			f.files[i] = NULL;
			f.encoder[i] = NULL;

			if (!filenames[i])
				continue;

			if (strcmp(filenames[i], "-") == 0) {
				f.files[i] = stdout;
#ifdef _WIN32
				_setmode(_fileno(stdin), _O_BINARY);
#endif
			} else {
				f.files[i] = fopen(filenames[i], "wb");
				if (!f.files[i]) {
					fprintf(stderr, "Failed to open %s\n", filenames[i]);
					goto out;
				}

				char *dot = strrchr(filenames[i], '.');
				if (dot && !strcmp(dot, ".flac"))
					f.use_flac[i] = true;
				else
					f.use_flac[i] = false;
			}
		}
	}

	gettimeofday(&start_time, NULL);
	
	if (level_check_mode) {
		fprintf(stderr, "Level monitor mode - press Ctrl+C to exit\n");
	} else {
		fprintf(stderr, "Starting capture...\n");
	}

	r = hsdaoh_start_stream(dev, hsdaoh_callback, (void *)&f, num_bufs);

	while (!do_exit)
		usleep(50000);

	if (level_check_mode) {
		fprintf(stderr, "\n\nMonitoring stopped.\n");
	} else {
		fprintf(stderr, "\n\nCapture complete.\n");
		fprintf(stderr, "\nFinal Statistics:\n");
		for (int i = 0; i < FD_NUMS; i++) {
			if (stream_stats[i].active) {
				fprintf(stderr, "  Stream-%d: %lu samples, %lu clips",
				        i,
				        (unsigned long)stream_stats[i].sample_count,
				        (unsigned long)stream_stats[i].clip_count);
				if (stream_stats[i].clip_count > 0) {
					fprintf(stderr, " (last at %.1fs)", stream_stats[i].last_clip_time);
				}
				fprintf(stderr, "\n");
			}
		}
	}
	
	hsdaoh_close(dev);

	if (!level_check_mode) {
		for (int i = 0; i < FD_NUMS; i++) {
			if (!f.files[i])
				continue;

			if (f.use_flac[i] && f.encoder[i]) {
				FLAC__bool ret = FLAC__stream_encoder_finish(f.encoder[i]);
				if (!ret)
					fprintf(stderr, "ERROR: FLAC encoder did not finish correctly: %s\n",
							FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(f.encoder[i])]);
				FLAC__stream_encoder_delete(f.encoder[i]);
			} else if (f.files[i] != stdout)
				fclose(f.files[i]);
		}
	}

out:
	return r >= 0 ? r : -r;
}
