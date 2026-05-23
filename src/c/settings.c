/**
 * settings.c — NapBuster Settings Screen
 *
 * Three rows:
 *   1. Guard (ON/OFF) — master enable toggle
 *   2. No-nap from    — window start hour (0–23, wraps)
 *   3. No-nap until   — window end hour   (0–23, wraps)
 *
 * Navigation:
 *   UP / DOWN  — navigate rows (idle)  |  adjust value (editing)
 *   SELECT     — enter / confirm edit mode for selected row
 *   BACK       — save all settings and close
 *
 * Hours wrap: pressing UP on 23 gives 0, pressing DOWN on 0 gives 23.
 * Values display as HH:00 (hour granularity is sufficient for sleep windows).
 * Settings are written to flash on BACK and the worker is notified.
 */

#include "settings.h"
#include "common.h"

// ─── Row definitions ──────────────────────────────────────────────────────────

typedef enum {
    ROW_ENABLED = 0,
    ROW_START_HOUR,
    ROW_END_HOUR,
    ROW_COUNT
} SettingsRow;

static const char *ROW_LABELS[ROW_COUNT] = {
    "Guard",
    "No-nap from",
    "No-nap until"
};

// ─── State ───────────────────────────────────────────────────────────────────

static Window    *s_settings_window = NULL;
static TextLayer *s_title_layer     = NULL;
static TextLayer *s_hint_layer      = NULL;  // bottom hint text
static TextLayer *s_row_layers[ROW_COUNT];
static TextLayer *s_val_layers[ROW_COUNT];
static Layer     *s_highlight_layer = NULL;

static bool s_enabled;
static int  s_start_hour;
static int  s_end_hour;
static int  s_selected_row = 0;
static bool s_editing      = false;

// String buffers for value display (written once per refresh)
static char s_buf_enabled[8];
static char s_buf_start[6];
static char s_buf_end[6];

// ─── Layout ──────────────────────────────────────────────────────────────────

#define TITLE_HEIGHT   28
#define ROW_HEIGHT     48
#define ROW_PADDING_X   6
#define VAL_WIDTH      64
#define HINT_HEIGHT    22

// ─── Helpers ─────────────────────────────────────────────────────────────────

/** Wrap an integer within [0, max] inclusive. */
static int wrap_int(int val, int max) {
    if (val < 0)   return max;
    if (val > max) return 0;
    return val;
}

/** Refresh all text layers from current local state. */
static void prv_refresh_display(void) {
    snprintf(s_buf_enabled, sizeof(s_buf_enabled),
             "%s", s_enabled ? "ON" : "OFF");
    text_layer_set_text(s_val_layers[ROW_ENABLED], s_buf_enabled);

    snprintf(s_buf_start, sizeof(s_buf_start), "%02d:00", s_start_hour);
    text_layer_set_text(s_val_layers[ROW_START_HOUR], s_buf_start);

    snprintf(s_buf_end, sizeof(s_buf_end), "%02d:00", s_end_hour);
    text_layer_set_text(s_val_layers[ROW_END_HOUR], s_buf_end);

    // Hint changes based on edit mode
    text_layer_set_text(s_hint_layer,
        s_editing ? "UP/DN: change  SEL: done" : "SEL: edit  BACK: save");

    layer_mark_dirty(s_highlight_layer);
}

/** Persist local state to flash and notify worker. */
static void prv_commit(void) {
    persist_write_int(PERSIST_KEY_ENABLED,    (int)s_enabled);
    persist_write_int(PERSIST_KEY_START_HOUR, s_start_hour);
    persist_write_int(PERSIST_KEY_END_HOUR,   s_end_hour);

    AppWorkerMessage msg = { .data0 = APP_MSG_SETTINGS_CHANGED };
    app_worker_send_message(APP_MSG_SETTINGS_CHANGED, &msg);
}

// ─── Highlight Layer ─────────────────────────────────────────────────────────

static void prv_highlight_update(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    int y = TITLE_HEIGHT + s_selected_row * ROW_HEIGHT;

    graphics_context_set_fill_color(ctx,
        s_editing ? GColorChromeYellow : GColorCobaltBlue);
    graphics_fill_rect(ctx, GRect(0, y, bounds.size.w, ROW_HEIGHT),
                       0, GCornerNone);
}

// ─── Click Handlers ──────────────────────────────────────────────────────────

static void prv_up_click(ClickRecognizerRef recognizer, void *ctx) {
    if (s_editing) {
        switch (s_selected_row) {
            case ROW_ENABLED:
                s_enabled = !s_enabled;
                break;
            case ROW_START_HOUR:
                s_start_hour = wrap_int(s_start_hour + 1, 23);
                break;
            case ROW_END_HOUR:
                s_end_hour = wrap_int(s_end_hour + 1, 23);
                break;
            default: break;
        }
    } else {
        // Navigate up through rows (wrap)
        s_selected_row = wrap_int(s_selected_row - 1, ROW_COUNT - 1);
    }
    prv_refresh_display();
}

static void prv_down_click(ClickRecognizerRef recognizer, void *ctx) {
    if (s_editing) {
        switch (s_selected_row) {
            case ROW_ENABLED:
                s_enabled = !s_enabled;
                break;
            case ROW_START_HOUR:
                s_start_hour = wrap_int(s_start_hour - 1, 23);
                break;
            case ROW_END_HOUR:
                s_end_hour = wrap_int(s_end_hour - 1, 23);
                break;
            default: break;
        }
    } else {
        s_selected_row = wrap_int(s_selected_row + 1, ROW_COUNT - 1);
    }
    prv_refresh_display();
}

static void prv_select_click(ClickRecognizerRef recognizer, void *ctx) {
    s_editing = !s_editing;
    prv_refresh_display();
}

static void prv_click_config(void *ctx) {
    window_single_click_subscribe(BUTTON_ID_UP,     prv_up_click);
    window_single_click_subscribe(BUTTON_ID_DOWN,   prv_down_click);
    window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click);
    // BACK is handled automatically to pop the window
}

// ─── Window Lifecycle ────────────────────────────────────────────────────────

static void prv_window_load(Window *window) {
    // Load persisted settings into local state
    s_enabled      = settings_get_enabled();
    s_start_hour   = settings_get_start_hour();
    s_end_hour     = settings_get_end_hour();
    s_selected_row = 0;
    s_editing      = false;

    Layer *root    = window_get_root_layer(window);
    GRect  bounds  = layer_get_bounds(root);

    // ── Title bar ──
    s_title_layer = text_layer_create(GRect(0, 0, bounds.size.w, TITLE_HEIGHT));
    text_layer_set_text(s_title_layer, "NapBuster Settings");
    text_layer_set_background_color(s_title_layer, GColorDarkGray);
    text_layer_set_text_color(s_title_layer, GColorWhite);
    text_layer_set_text_alignment(s_title_layer, GTextAlignmentCenter);
    text_layer_set_font(s_title_layer,
        fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
    layer_add_child(root, text_layer_get_layer(s_title_layer));

    // ── Highlight layer (drawn behind row text) ──
    s_highlight_layer = layer_create(bounds);
    layer_set_update_proc(s_highlight_layer, prv_highlight_update);
    layer_add_child(root, s_highlight_layer);

    // ── Rows ──
    for (int i = 0; i < ROW_COUNT; i++) {
        int y = TITLE_HEIGHT + i * ROW_HEIGHT;

        // Label (left side)
        s_row_layers[i] = text_layer_create(
            GRect(ROW_PADDING_X,
                  y + 10,
                  bounds.size.w - VAL_WIDTH - ROW_PADDING_X,
                  ROW_HEIGHT - 10));
        text_layer_set_text(s_row_layers[i], ROW_LABELS[i]);
        text_layer_set_background_color(s_row_layers[i], GColorClear);
        text_layer_set_text_color(s_row_layers[i], GColorWhite);
        text_layer_set_font(s_row_layers[i],
            fonts_get_system_font(FONT_KEY_GOTHIC_18));
        layer_add_child(root, text_layer_get_layer(s_row_layers[i]));

        // Value (right side)
        s_val_layers[i] = text_layer_create(
            GRect(bounds.size.w - VAL_WIDTH - ROW_PADDING_X,
                  y + 10,
                  VAL_WIDTH,
                  ROW_HEIGHT - 10));
        text_layer_set_background_color(s_val_layers[i], GColorClear);
        text_layer_set_text_color(s_val_layers[i], GColorYellow);
        text_layer_set_text_alignment(s_val_layers[i], GTextAlignmentRight);
        text_layer_set_font(s_val_layers[i],
            fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
        layer_add_child(root, text_layer_get_layer(s_val_layers[i]));
    }

    // ── Bottom hint bar ──
    int hint_y = bounds.size.h - HINT_HEIGHT;
    s_hint_layer = text_layer_create(
        GRect(0, hint_y, bounds.size.w, HINT_HEIGHT));
    text_layer_set_background_color(s_hint_layer, GColorDarkGray);
    text_layer_set_text_color(s_hint_layer, GColorLightGray);
    text_layer_set_text_alignment(s_hint_layer, GTextAlignmentCenter);
    text_layer_set_font(s_hint_layer,
        fonts_get_system_font(FONT_KEY_GOTHIC_14));
    layer_add_child(root, text_layer_get_layer(s_hint_layer));

    window_set_click_config_provider(window, prv_click_config);
    prv_refresh_display();
}

static void prv_window_unload(Window *window) {
    // Save to flash when the user presses BACK
    prv_commit();

    text_layer_destroy(s_title_layer);
    text_layer_destroy(s_hint_layer);
    layer_destroy(s_highlight_layer);
    for (int i = 0; i < ROW_COUNT; i++) {
        text_layer_destroy(s_row_layers[i]);
        text_layer_destroy(s_val_layers[i]);
    }

    window_destroy(s_settings_window);
    s_settings_window = NULL;
}

// ─── Public API ──────────────────────────────────────────────────────────────

void settings_window_push(void) {
    if (s_settings_window) return;  // guard against double-push

    s_settings_window = window_create();
    window_set_background_color(s_settings_window, GColorBlack);

    WindowHandlers handlers = {
        .load   = prv_window_load,
        .unload = prv_window_unload
    };
    window_set_window_handlers(s_settings_window, handlers);
    window_stack_push(s_settings_window, true /* animated */);
}
