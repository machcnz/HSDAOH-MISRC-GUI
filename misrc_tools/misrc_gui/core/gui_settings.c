/*
 * MISRC GUI - Settings Persistence
 *
 * Handles loading/saving settings to JSON file and provides defaults
 */

#include "../core/gui_app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

// Settings file location
static const char* get_settings_file_path(void) {
    static char settings_path[512];
    static bool initialized = false;
    
    if (!initialized) {
#ifdef __APPLE__
        // Use ~/Library/Preferences on macOS
        const char* home = getenv("HOME");
        if (home) {
            snprintf(settings_path, sizeof(settings_path), 
                    "%s/Library/Preferences/com.misrc.gui.json", home);
        } else {
            strcpy(settings_path, "./misrc_gui_settings.json");
        }
#else
        // Use ~/.config on Linux, current dir on Windows
        const char* home = getenv("HOME");
        if (home) {
            snprintf(settings_path, sizeof(settings_path), 
                    "%s/.config/misrc_gui_settings.json", home);
        } else {
            strcpy(settings_path, "./misrc_gui_settings.json");
        }
#endif
        initialized = true;
    }
    
    return settings_path;
}

const char* gui_settings_get_desktop_path(void) {
    static char desktop_path[512];
    static bool initialized = false;
    
    if (!initialized) {
#ifdef __APPLE__
        const char* home = getenv("HOME");
        if (home) {
            snprintf(desktop_path, sizeof(desktop_path), "%s/Desktop", home);
        } else {
            strcpy(desktop_path, ".");
        }
#else
        const char* home = getenv("HOME");
        if (home) {
            snprintf(desktop_path, sizeof(desktop_path), "%s/Desktop", home);
        } else {
            strcpy(desktop_path, ".");
        }
#endif
        initialized = true;
    }
    
    return desktop_path;
}

void gui_settings_init_defaults(gui_settings_t *settings) {
    if (!settings) return;
    
    // Zero out the structure
    memset(settings, 0, sizeof(gui_settings_t));
    
    // Basic settings
    settings->device_index = 0;
    settings->capture_a = true;
    settings->capture_b = true;
    
    // Set default save path to Desktop
    strncpy(settings->output_path, gui_settings_get_desktop_path(), MAX_FILENAME_LEN - 1);
    settings->output_path[MAX_FILENAME_LEN - 1] = '\0';
    
    // Default filenames
    strcpy(settings->output_filename_a, "capture_a.flac");
    strcpy(settings->output_filename_b, "capture_b.flac");
    strcpy(settings->aux_filename, "aux_data.bin");
    strcpy(settings->raw_filename, "raw_data.bin");
    strcpy(settings->audio_4ch_filename, "audio_4ch.wav");
    strcpy(settings->audio_2ch_12_filename, "audio_12.wav");
    strcpy(settings->audio_2ch_34_filename, "audio_34.wav");
    
    // Individual channel filenames
    for (int i = 0; i < 4; i++) {
        snprintf(settings->audio_1ch_filenames[i], MAX_FILENAME_LEN, "audio_ch%d.wav", i + 1);
    }
    
    // Capture control defaults
    settings->sample_count = 0; // Infinite
    strcpy(settings->capture_time, ""); // Empty = infinite
    settings->overwrite_files = false;
    
    // Processing options
    settings->pad_lower_bits = false;
    settings->show_peak_levels = true;
    settings->suppress_clip_a = false;
    settings->suppress_clip_b = false;
    settings->reduce_8bit_a = false;
    settings->reduce_8bit_b = false;
    
    // Resampling defaults
    settings->enable_resample_a = false;
    settings->enable_resample_b = false;
    settings->resample_rate_a = 4000.0f;  // 4 MHz
    settings->resample_rate_b = 4000.0f;  // 4 MHz
    settings->resample_quality_a = 3;     // High quality
    settings->resample_quality_b = 3;     // High quality
    settings->resample_gain_a = 0.0f;     // No gain
    settings->resample_gain_b = 0.0f;     // No gain
    
    // FLAC defaults
    settings->use_flac = true;
    settings->flac_12bit = false;
    settings->flac_level = 4;             // Balanced compression
    settings->flac_verification = false;  // Faster
    settings->flac_threads = 0;           // Auto
    
    // Audio output defaults
    settings->enable_audio_4ch = false;
    settings->enable_audio_2ch_12 = false;
    settings->enable_audio_2ch_34 = false;
    for (int i = 0; i < 4; i++) {
        settings->enable_audio_1ch[i] = false;
    }
    
    // Display settings
    settings->show_grid = true;
    settings->time_scale = 1.0f;
    settings->amplitude_scale = 1.0f;
}

// Simple JSON-like format for settings
void gui_settings_save(const gui_settings_t *settings) {
    if (!settings) return;
    
    const char* path = get_settings_file_path();
    FILE* f = fopen(path, "w");
    if (!f) return;
    
    fprintf(f, "{\n");
    fprintf(f, "  \"device_index\": %d,\n", settings->device_index);
    fprintf(f, "  \"output_path\": \"%s\",\n", settings->output_path);
    fprintf(f, "  \"output_filename_a\": \"%s\",\n", settings->output_filename_a);
    fprintf(f, "  \"output_filename_b\": \"%s\",\n", settings->output_filename_b);
    fprintf(f, "  \"capture_a\": %s,\n", settings->capture_a ? "true" : "false");
    fprintf(f, "  \"capture_b\": %s,\n", settings->capture_b ? "true" : "false");
    fprintf(f, "  \"sample_count\": %llu,\n", (unsigned long long)settings->sample_count);
    fprintf(f, "  \"capture_time\": \"%s\",\n", settings->capture_time);
    fprintf(f, "  \"overwrite_files\": %s,\n", settings->overwrite_files ? "true" : "false");
    fprintf(f, "  \"aux_filename\": \"%s\",\n", settings->aux_filename);
    fprintf(f, "  \"raw_filename\": \"%s\",\n", settings->raw_filename);

    // Audio filenames + enables (mirror CLI options)
    fprintf(f, "  \"audio_4ch_filename\": \"%s\",\n", settings->audio_4ch_filename);
    fprintf(f, "  \"audio_2ch_12_filename\": \"%s\",\n", settings->audio_2ch_12_filename);
    fprintf(f, "  \"audio_2ch_34_filename\": \"%s\",\n", settings->audio_2ch_34_filename);
    fprintf(f, "  \"audio_1ch_1_filename\": \"%s\",\n", settings->audio_1ch_filenames[0]);
    fprintf(f, "  \"audio_1ch_2_filename\": \"%s\",\n", settings->audio_1ch_filenames[1]);
    fprintf(f, "  \"audio_1ch_3_filename\": \"%s\",\n", settings->audio_1ch_filenames[2]);
    fprintf(f, "  \"audio_1ch_4_filename\": \"%s\",\n", settings->audio_1ch_filenames[3]);

    fprintf(f, "  \"enable_audio_4ch\": %s,\n", settings->enable_audio_4ch ? "true" : "false");
    fprintf(f, "  \"enable_audio_2ch_12\": %s,\n", settings->enable_audio_2ch_12 ? "true" : "false");
    fprintf(f, "  \"enable_audio_2ch_34\": %s,\n", settings->enable_audio_2ch_34 ? "true" : "false");
    fprintf(f, "  \"enable_audio_1ch_1\": %s,\n", settings->enable_audio_1ch[0] ? "true" : "false");
    fprintf(f, "  \"enable_audio_1ch_2\": %s,\n", settings->enable_audio_1ch[1] ? "true" : "false");
    fprintf(f, "  \"enable_audio_1ch_3\": %s,\n", settings->enable_audio_1ch[2] ? "true" : "false");
    fprintf(f, "  \"enable_audio_1ch_4\": %s,\n", settings->enable_audio_1ch[3] ? "true" : "false");

    fprintf(f, "  \"pad_lower_bits\": %s,\n", settings->pad_lower_bits ? "true" : "false");
    fprintf(f, "  \"show_peak_levels\": %s,\n", settings->show_peak_levels ? "true" : "false");
    fprintf(f, "  \"suppress_clip_a\": %s,\n", settings->suppress_clip_a ? "true" : "false");
    fprintf(f, "  \"suppress_clip_b\": %s,\n", settings->suppress_clip_b ? "true" : "false");
    fprintf(f, "  \"reduce_8bit_a\": %s,\n", settings->reduce_8bit_a ? "true" : "false");
    fprintf(f, "  \"reduce_8bit_b\": %s,\n", settings->reduce_8bit_b ? "true" : "false");
    fprintf(f, "  \"enable_resample_a\": %s,\n", settings->enable_resample_a ? "true" : "false");
    fprintf(f, "  \"enable_resample_b\": %s,\n", settings->enable_resample_b ? "true" : "false");
    fprintf(f, "  \"resample_rate_a\": %.1f,\n", settings->resample_rate_a);
    fprintf(f, "  \"resample_rate_b\": %.1f,\n", settings->resample_rate_b);
    fprintf(f, "  \"resample_quality_a\": %d,\n", settings->resample_quality_a);
    fprintf(f, "  \"resample_quality_b\": %d,\n", settings->resample_quality_b);
    fprintf(f, "  \"resample_gain_a\": %.1f,\n", settings->resample_gain_a);
    fprintf(f, "  \"resample_gain_b\": %.1f,\n", settings->resample_gain_b);
    fprintf(f, "  \"use_flac\": %s,\n", settings->use_flac ? "true" : "false");
    fprintf(f, "  \"flac_12bit\": %s,\n", settings->flac_12bit ? "true" : "false");
    fprintf(f, "  \"flac_level\": %d,\n", settings->flac_level);
    fprintf(f, "  \"flac_verification\": %s,\n", settings->flac_verification ? "true" : "false");
    fprintf(f, "  \"flac_threads\": %d,\n", settings->flac_threads);
    fprintf(f, "  \"enable_resample_a\": %s,\n", settings->enable_resample_a ? "true" : "false");
    fprintf(f, "  \"enable_resample_b\": %s,\n", settings->enable_resample_b ? "true" : "false");
    fprintf(f, "  \"resample_rate_a\": %.1f,\n", settings->resample_rate_a);
    fprintf(f, "  \"resample_rate_b\": %.1f,\n", settings->resample_rate_b);
    fprintf(f, "  \"resample_quality_a\": %d,\n", settings->resample_quality_a);
    fprintf(f, "  \"resample_quality_b\": %d,\n", settings->resample_quality_b);
    fprintf(f, "  \"resample_gain_a\": %.1f,\n", settings->resample_gain_a);
    fprintf(f, "  \"resample_gain_b\": %.1f,\n", settings->resample_gain_b);
    fprintf(f, "  \"overwrite_files\": %s,\n", settings->overwrite_files ? "true" : "false");
    fprintf(f, "  \"show_grid\": %s,\n", settings->show_grid ? "true" : "false");
    fprintf(f, "  \"time_scale\": %.2f,\n", settings->time_scale);
    fprintf(f, "  \"amplitude_scale\": %.2f,\n", settings->amplitude_scale);
    fprintf(f, "  \"playback_file_a\": \"%s\",\n", settings->playback_file_a);
    fprintf(f, "  \"playback_file_b\": \"%s\"\n", settings->playback_file_b);
    fprintf(f, "}\n");
    
    fclose(f);
}

// Simple parser for our JSON-like format
static char* find_value(const char* content, const char* key) {
    static char value[512];
    char search_key[256];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);
    
    char* pos = strstr(content, search_key);
    if (!pos) return NULL;
    
    pos += strlen(search_key);
    while (*pos == ' ' || *pos == '\t') pos++; // Skip whitespace
    
    if (*pos == '"') {
        // String value
        pos++;
        char* end = strchr(pos, '"');
        if (!end) return NULL;
        
        size_t len = end - pos;
        if (len >= sizeof(value)) len = sizeof(value) - 1;
        strncpy(value, pos, len);
        value[len] = '\0';
        return value;
    } else {
        // Number or boolean
        char* end = strpbrk(pos, ",\n}");
        if (!end) return NULL;
        
        size_t len = end - pos;
        if (len >= sizeof(value)) len = sizeof(value) - 1;
        strncpy(value, pos, len);
        value[len] = '\0';
        
        // Trim trailing whitespace
        while (len > 0 && (value[len-1] == ' ' || value[len-1] == '\t')) {
            value[--len] = '\0';
        }
        
        return value;
    }
}

// macOS folder picker using osascript. Returns true if output_path changed.
bool gui_settings_choose_output_folder(gui_settings_t *settings) {
    if (!settings) return false;

#ifdef __APPLE__
    // Use AppleScript choose folder dialog and return POSIX path.
    // Note: This will prompt the user and block until a selection is made.
    const char *cmd = "osascript -e 'POSIX path of (choose folder with prompt \"Select output folder for MISRC captures\")'";
    FILE *fp = popen(cmd, "r");
    if (!fp) return false;

    char buf[512] = {0};
    if (!fgets(buf, sizeof(buf), fp)) {
        pclose(fp);
        return false;
    }
    int rc = pclose(fp);
    (void)rc;

    // Trim newline
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }
    if (len == 0) return false;

    // Remove trailing slash
    while (len > 1 && buf[len - 1] == '/') {
        buf[--len] = '\0';
    }

    if (strncmp(settings->output_path, buf, MAX_FILENAME_LEN) == 0) {
        return false; // no change
    }

    strncpy(settings->output_path, buf, MAX_FILENAME_LEN - 1);
    settings->output_path[MAX_FILENAME_LEN - 1] = '\0';
    return true;
#else
    (void)settings;
    return false;
#endif
}

void gui_settings_load(gui_settings_t *settings) {
    if (!settings) return;
    
    // Start with defaults
    gui_settings_init_defaults(settings);
    
    const char* path = get_settings_file_path();
    FILE* f = fopen(path, "r");
    if (!f) return; // No settings file, use defaults
    
    // Read entire file
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size <= 0 || size > 32768) { // Sanity check
        fclose(f);
        return;
    }
    
    char* content = malloc(size + 1);
    if (!content) {
        fclose(f);
        return;
    }
    
    size_t read_size = fread(content, 1, size, f);
    content[read_size] = '\0';
    fclose(f);
    
    // Parse values
    char* value;
    
    if ((value = find_value(content, "device_index")) != NULL) {
        settings->device_index = atoi(value);
    }
    
    if ((value = find_value(content, "output_path")) != NULL) {
        strncpy(settings->output_path, value, MAX_FILENAME_LEN - 1);
        settings->output_path[MAX_FILENAME_LEN - 1] = '\0';
    }
    
    if ((value = find_value(content, "output_filename_a")) != NULL) {
        strncpy(settings->output_filename_a, value, MAX_FILENAME_LEN - 1);
        settings->output_filename_a[MAX_FILENAME_LEN - 1] = '\0';
    }
    
    if ((value = find_value(content, "output_filename_b")) != NULL) {
        strncpy(settings->output_filename_b, value, MAX_FILENAME_LEN - 1);
        settings->output_filename_b[MAX_FILENAME_LEN - 1] = '\0';
    }
    
    if ((value = find_value(content, "capture_a")) != NULL) {
        settings->capture_a = (strcmp(value, "true") == 0);
    }
    
    if ((value = find_value(content, "capture_b")) != NULL) {
        settings->capture_b = (strcmp(value, "true") == 0);
    }
    
    if ((value = find_value(content, "use_flac")) != NULL) {
        settings->use_flac = (strcmp(value, "true") == 0);
    }

    if ((value = find_value(content, "flac_12bit")) != NULL) {
        settings->flac_12bit = (strcmp(value, "true") == 0);
    }

    if ((value = find_value(content, "flac_verification")) != NULL) {
        settings->flac_verification = (strcmp(value, "true") == 0);
    }

    if ((value = find_value(content, "flac_threads")) != NULL) {
        settings->flac_threads = atoi(value);
    }

    if ((value = find_value(content, "flac_level")) != NULL) {
        settings->flac_level = atoi(value);
    }

    if ((value = find_value(content, "enable_resample_a")) != NULL) {
        settings->enable_resample_a = (strcmp(value, "true") == 0);
    }

    if ((value = find_value(content, "enable_resample_b")) != NULL) {
        settings->enable_resample_b = (strcmp(value, "true") == 0);
    }

    if ((value = find_value(content, "resample_rate_a")) != NULL) {
        settings->resample_rate_a = (float)atof(value);
    }

    if ((value = find_value(content, "resample_rate_b")) != NULL) {
        settings->resample_rate_b = (float)atof(value);
    }

    if ((value = find_value(content, "resample_quality_a")) != NULL) {
        settings->resample_quality_a = atoi(value);
    }

    if ((value = find_value(content, "resample_quality_b")) != NULL) {
        settings->resample_quality_b = atoi(value);
    }

    if ((value = find_value(content, "resample_gain_a")) != NULL) {
        settings->resample_gain_a = (float)atof(value);
    }

    if ((value = find_value(content, "resample_gain_b")) != NULL) {
        settings->resample_gain_b = (float)atof(value);
    }

    if ((value = find_value(content, "overwrite_files")) != NULL) {
        settings->overwrite_files = (strcmp(value, "true") == 0);
    }
    
    if ((value = find_value(content, "show_grid")) != NULL) {
        settings->show_grid = (strcmp(value, "true") == 0);
    }
    
    if ((value = find_value(content, "time_scale")) != NULL) {
        settings->time_scale = (float)atof(value);
    }
    
    if ((value = find_value(content, "amplitude_scale")) != NULL) {
        settings->amplitude_scale = (float)atof(value);
    }

    if ((value = find_value(content, "reduce_8bit_a")) != NULL) {
        settings->reduce_8bit_a = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "reduce_8bit_b")) != NULL) {
        settings->reduce_8bit_b = (strcmp(value, "true") == 0);
    }

    // Audio filenames + enables
    if ((value = find_value(content, "audio_4ch_filename")) != NULL) {
        strncpy(settings->audio_4ch_filename, value, MAX_FILENAME_LEN - 1);
        settings->audio_4ch_filename[MAX_FILENAME_LEN - 1] = '\0';
    }
    if ((value = find_value(content, "audio_2ch_12_filename")) != NULL) {
        strncpy(settings->audio_2ch_12_filename, value, MAX_FILENAME_LEN - 1);
        settings->audio_2ch_12_filename[MAX_FILENAME_LEN - 1] = '\0';
    }
    if ((value = find_value(content, "audio_2ch_34_filename")) != NULL) {
        strncpy(settings->audio_2ch_34_filename, value, MAX_FILENAME_LEN - 1);
        settings->audio_2ch_34_filename[MAX_FILENAME_LEN - 1] = '\0';
    }
    if ((value = find_value(content, "audio_1ch_1_filename")) != NULL) {
        strncpy(settings->audio_1ch_filenames[0], value, MAX_FILENAME_LEN - 1);
        settings->audio_1ch_filenames[0][MAX_FILENAME_LEN - 1] = '\0';
    }
    if ((value = find_value(content, "audio_1ch_2_filename")) != NULL) {
        strncpy(settings->audio_1ch_filenames[1], value, MAX_FILENAME_LEN - 1);
        settings->audio_1ch_filenames[1][MAX_FILENAME_LEN - 1] = '\0';
    }
    if ((value = find_value(content, "audio_1ch_3_filename")) != NULL) {
        strncpy(settings->audio_1ch_filenames[2], value, MAX_FILENAME_LEN - 1);
        settings->audio_1ch_filenames[2][MAX_FILENAME_LEN - 1] = '\0';
    }
    if ((value = find_value(content, "audio_1ch_4_filename")) != NULL) {
        strncpy(settings->audio_1ch_filenames[3], value, MAX_FILENAME_LEN - 1);
        settings->audio_1ch_filenames[3][MAX_FILENAME_LEN - 1] = '\0';
    }

    if ((value = find_value(content, "enable_audio_4ch")) != NULL) {
        settings->enable_audio_4ch = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "enable_audio_2ch_12")) != NULL) {
        settings->enable_audio_2ch_12 = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "enable_audio_2ch_34")) != NULL) {
        settings->enable_audio_2ch_34 = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "enable_audio_1ch_1")) != NULL) {
        settings->enable_audio_1ch[0] = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "enable_audio_1ch_2")) != NULL) {
        settings->enable_audio_1ch[1] = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "enable_audio_1ch_3")) != NULL) {
        settings->enable_audio_1ch[2] = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "enable_audio_1ch_4")) != NULL) {
        settings->enable_audio_1ch[3] = (strcmp(value, "true") == 0);
    }

    // Playback files
    if ((value = find_value(content, "playback_file_a")) != NULL) {
        strncpy(settings->playback_file_a, value, MAX_FILENAME_LEN - 1);
        settings->playback_file_a[MAX_FILENAME_LEN - 1] = '\0';
    }
    if ((value = find_value(content, "playback_file_b")) != NULL) {
        strncpy(settings->playback_file_b, value, MAX_FILENAME_LEN - 1);
        settings->playback_file_b[MAX_FILENAME_LEN - 1] = '\0';
    }

    free(content);
}
