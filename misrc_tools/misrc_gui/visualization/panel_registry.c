/*
 * MISRC GUI - Panel Registry
 *
 * Manages registration of panel vtables and provides factory functions
 * for creating panel instances.
 */

#include "panel_interface.h"
#include <string.h>

//-----------------------------------------------------------------------------
// Registry Storage
//-----------------------------------------------------------------------------

static const panel_vtable_t *s_vtables[PANEL_VIEW_COUNT] = {0};
static bool s_initialized = false;

//-----------------------------------------------------------------------------
// Registry API
//-----------------------------------------------------------------------------

void panel_registry_init(void) {
    if (s_initialized) return;

    memset(s_vtables, 0, sizeof(s_vtables));
    s_initialized = true;
}

void panel_register(panel_view_type_t type, const panel_vtable_t *vtable) {
    if (!s_initialized) {
        panel_registry_init();
    }

    if (type >= 0 && type < PANEL_VIEW_COUNT) {
        s_vtables[type] = vtable;
    }
}

const panel_vtable_t *panel_get_vtable(panel_view_type_t type) {
    if (!s_initialized || type < 0 || type >= PANEL_VIEW_COUNT) {
        return NULL;
    }
    return s_vtables[type];
}

//-----------------------------------------------------------------------------
// Panel Instance Lifecycle
//-----------------------------------------------------------------------------

panel_instance_t panel_instance_create(panel_view_type_t type) {
    panel_instance_t instance = {0};
    instance.type = type;
    instance.vtable = panel_get_vtable(type);
    instance.state = NULL;
    instance.last_bounds = (Rectangle){0};

    if (instance.vtable && instance.vtable->create) {
        instance.state = instance.vtable->create();
    }

    return instance;
}

void panel_instance_destroy(panel_instance_t *instance) {
    if (!instance) return;

    if (instance->vtable && instance->vtable->destroy && instance->state) {
        instance->vtable->destroy(instance->state);
    }

    instance->state = NULL;
    instance->vtable = NULL;
    instance->type = PANEL_VIEW_WAVEFORM;
    instance->last_bounds = (Rectangle){0};
}

void panel_instance_clear(panel_instance_t *instance) {
    if (!instance || !instance->vtable || !instance->vtable->clear || !instance->state) {
        return;
    }
    instance->vtable->clear(instance->state);
}

//-----------------------------------------------------------------------------
// Panel Instance Operations
//-----------------------------------------------------------------------------

void panel_instance_process(panel_instance_t *instance, const int16_t *samples, size_t count, uint32_t sample_rate) {
    if (!instance || !instance->vtable || !instance->vtable->process || !instance->state) {
        return;
    }
    instance->vtable->process(instance->state, samples, count, sample_rate);
}

void panel_instance_render(panel_instance_t *instance, gui_app_t *app, int channel,
                           Rectangle bounds, Color channel_color) {
    if (!instance) return;

    // Cache bounds for click handling
    instance->last_bounds = bounds;

    if (!instance->vtable || !instance->vtable->render) {
        // No vtable or no render function - draw placeholder
        DrawRectangleRec(bounds, (Color){20, 20, 20, 255});
        DrawRectangleLinesEx(bounds, 1, (Color){60, 60, 60, 255});
        return;
    }

    instance->vtable->render(instance->state, app, channel, bounds, channel_color);
}

void panel_instance_render_overlay(panel_instance_t *instance, Rectangle bounds) {
    if (!instance || !instance->vtable || !instance->vtable->render_overlay) {
        return;
    }
    instance->vtable->render_overlay(instance->state, bounds);
}

bool panel_instance_handle_click(panel_instance_t *instance, gui_app_t *app, int channel,
                                 Vector2 click, Rectangle bounds) {
    if (!instance || !instance->vtable || !instance->vtable->handle_click || !instance->state) {
        return false;
    }
    return instance->vtable->handle_click(instance->state, app, channel, click, bounds);
}

bool panel_instance_handle_scroll(panel_instance_t *instance, float delta, Rectangle bounds) {
    if (!instance || !instance->vtable || !instance->vtable->handle_scroll || !instance->state) {
        return false;
    }
    return instance->vtable->handle_scroll(instance->state, delta, bounds);
}

size_t panel_instance_get_menu_count(panel_instance_t *instance) {
    if (!instance || !instance->vtable || !instance->vtable->get_menu_count || !instance->state) {
        return 0;
    }
    return instance->vtable->get_menu_count(instance->state);
}

panel_menu_t panel_instance_get_menu(panel_instance_t *instance, size_t index) {
    panel_menu_t empty = {0};

    if (!instance || !instance->vtable || !instance->vtable->get_menu || !instance->state) {
        return empty;
    }

    size_t count = panel_instance_get_menu_count(instance);
    if (index >= count) {
        return empty;
    }

    return instance->vtable->get_menu(instance->state, index);
}

//-----------------------------------------------------------------------------
// Batch Processing
//-----------------------------------------------------------------------------

// Helper to process a single panel config's panels
static void process_config_panels(channel_panel_config_t *config,
                                  const int16_t *samples, size_t count,
                                  uint32_t sample_rate) {
    if (!config || !samples || count == 0) return;

    // Get vtable for left panel and process if it has a process function
    const panel_vtable_t *left_vtable = panel_get_vtable(config->left_view);
    if (left_vtable && left_vtable->process && config->left_state) {
        left_vtable->process(config->left_state, samples, count, sample_rate);
    }

    // Process right panel if in split mode
    if (config->split) {
        const panel_vtable_t *right_vtable = panel_get_vtable(config->right_view);
        if (right_vtable && right_vtable->process && config->right_state) {
            right_vtable->process(config->right_state, samples, count, sample_rate);
        }
    }
}

static inline void panel_cfg_lock(gui_app_t *app) {
    while (atomic_flag_test_and_set(&app->panel_config_lock)) {
        /* spin */
    }
}

static inline void panel_cfg_unlock(gui_app_t *app) {
    atomic_flag_clear(&app->panel_config_lock);
}

void panel_process_all(gui_app_t *app,
                       const int16_t *samples_a,
                       const int16_t *samples_b,
                       size_t count,
                       uint32_t sample_rate) {
    if (!app || count == 0) return;

    panel_cfg_lock(app);

    // Process channel A panels
    if (samples_a) {
        process_config_panels(&app->panel_config_a, samples_a, count, sample_rate);
    }

    // Process channel B panels
    if (samples_b) {
        process_config_panels(&app->panel_config_b, samples_b, count, sample_rate);
    }

    panel_cfg_unlock(app);
}
