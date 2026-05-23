/**
 * settings.c — NapBuster Settings Screen
 *
 * Five settings rows with virtual scrolling (3 visible at once):
 *
 *   ROW 0  Guard          ON / OFF
 *   ROW 1  Weekdays only  ON / OFF   (skip Sat + Sun)
 *   ROW 2  No-nap from    HH:00      (wrapping hour 0-23)
 *   ROW 3  No-nap until   HH:00      (wrapping hour 0-23)
 *   ROW 4  Wake vibration Gentle / Medium / Strong
 *
 * Navigation:
 *   UP / DOWN  — scroll through rows (idle)  |  adjust value (editing)
 *   SELECT     — enter / confirm edit
 *   BACK       — save all to flash, notify worker, close
 *
 * Layout: title bar (top) + 3 visible rows × ROW_HEIGHT + hint bar (bottom).
 * Works on all Pebble screen sizes (144×168 basalt/chalk up to 200×228 emery).
 */

#include "settings.h"
#include "common.h"

// ─── Row definitions ──────────────────────────────────────────────────────────

typedef enum {
    ROW_ENABLED = 0,
    ROW_WEEKDAYS_ONLY,
    ROW_START_HOUR,
    ROW_END_HOUR,
    ROW_VIBE_STRENGTH,
    ROW_COUNT
} SettingsRow;

static const char * const ROW_LABELS[ROW_COUNT] = {
    "Guard",
    "Weekdays only",
    "No-nap from",
    "No-nap until",
    "Wake vibration"
};

// ─── Layout ───────────────────────────────────────────────────────────────────

#define TITLE_HEIGHT    28
#define HINT_HEIGHT     20
#define VISIBLE_ROWS     3
#define ROW_HEIGHT      44
#define ROW_PADDING_X    6
#define VAL_WIDTH       68

// ─── State ───────────────────────────────────────────────────────────────────

static Window    *s_win        = NULL;
static TextLayer *s_title      = NULL;
static TextLayer *s_hint       = NULL;
static TextLayer *s_row_lbl[VISIBLE_ROWS];   // label layers (3 visible slots)
static TextLayer *s_row_val[VISIBLE_ROWS];   // value layers (3 visible slots)
static Layer     *s_highlight  = NULL;

// Local copies of all settings (committed on BACK)
static bool s_enabled;
static bool s_weekdays_only;
static int  s_start_hour;
static int  s_end_hour;
static int  s_vibe_strength;

static int  s_selected_row  = 0;   // which row (0..ROW_COUNT-1) is selected
static int  s_scroll_offset = 0;   // index of the topmost visible row
static bool s_editing       = false;

// Per-visible-slot string buffers
static char s_val_buf[VISIBLE_ROWS][16];

// ─── Helpers ─────────────────────────────────────────────────────────────────

static int wrap_int(int val, int max) {
    if (val < 0)   return max;
    if (val > max) return 0;
    return val;
}

/** Return display string for a given row index into the provided buffer. */
static void prv_format_value(int row, char *buf, size_t len) {
    switch (row) {
        case ROW_ENABLED:
            snprintf(buf, len, "%s", s_enabled ? "ON" : "OFF");
            break;
        case ROW_WEEKDAYS_ONLY:
            snprintf(buf, len, "%s", s_weekdays_only ? "ON" : "OFF");
            break;
        case ROW_START_HOUR:
            snprintf(buf, len, "%02d:00", s_start_hour);
            break;
        case ROW_END_HOUR:
            snprintf(buf, len, "%02d:00", s_end_hour);
            break;
        case ROW_VIBE_STRENGTH:
            snprintf(buf, len, "%s",
                VIBE_STRENGTH_LABELS[s_vibe_strength]);
            break;
        default:
            buf[0] = '\0';
            break;
    }
}

/** Refresh the 3 visible row slots from current scroll_offset. */
static void prv_refresh(void) {
    for (int slot = 0; slot < VISIBLE_ROWS; slot++) {
        int row = s_scroll_offset + slot;
        if (row < ROW_COUNT) {
            text_layer_set_text(s_row_lbl[slot], ROW_LABELS[row]);
            prv_format_value(row, s_val_buf[slot], sizeof(s_val_buf[slot]));
            text_layer_set_text(s_row_val[slot], s_val_buf[slot]);
        } else {
            // Empty slot below last row
            text_layer_set_text(s_row_lbl[slot], "");
            text_layer_set_text(s_row_val[slot], "");
        }
    }

    text_layer_set_text(s_hint,
        s_editing ? "UP/DN: change  SEL: done" : "SEL: edit  BACK: save");

    layer_mark_dirty(s_highlight);
}

/** Commit all settings to flash and notify worker. */
static void prv_commit(void) {
    persist_write_int(PERSIST_KEY_ENABLED,       (int)s_enabled);
    persist_write_int(PERSIST_KEY_WEEKDAYS_ONLY, (int)s_weekdays_only);
    persist_write_int(PERSIST_KEY_START_HOUR,    s_start_hour);
    persist_write_int(PERSIST_KEY_END_HOUR,      s_end_hour);
    persist_write_int(PERSIST_KEY_VIBE_STRENGTH, s_vibe_strength);

    AppWorkerMessage msg = { .data0 = APP_MSG_SETTINGS_CHANGED };
    app_worker_send_message(APP_MSG_SETTINGS_CHANGED, &msg);
}

// ─── Scroll Logic ─────────────────────────────────────────────────────────────

/** Ensure s_scroll_offset keeps the selected row visible. */
static void prv_ensure_visible(void) {
    if (s_selected_row < s_scroll_offset) {
        s_scroll_offset = s_selected_row;
    } else if (s_selected_row >= s_scroll_offset + VISIBLE_ROWS) {
        s_scroll_offset = s_selected_row - VISIBLE_ROWS + 1;
    }
}

// ─── Highlight Layer ─────────────────────────────────────────────────────────

static void prv_highlight_update(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    // Which visible slot is the selected row in?
    int slot = s_selected_row - s_scroll_offset;
    if (slot < 0 || slot >= VISIBLE_ROWS) return;

    int y = TITLE_HEIGHT + slot * ROW_HEIGHT;
    graphics_context_set_fill_color(ctx,
        s_editing ? GColorChromeYellow : GColorCobaltBlue);
    graphics_fill_rect(ctx, GRect(0, y, bounds.size.w, ROW_HEIGHT),
                       0, GCornerNone);
}

// ─── Increment/Decrement a Row Value ─────────────────────────────────────────

static void prv_adjust_value(int row, int delta) {
    switch (row) {
        case ROW_ENABLED:
            s_enabled = !s_enabled;
            break;
        case ROW_WEEKDAYS_ONLY:
            s_weekdays_only = !s_weekdays_only;
            break;
        case ROW_START_HOUR:
            s_start_hour = wrap_int(s_start_hour + delta, 23);
            break;
        case ROW_END_HOUR:
            s_end_hour = wrap_int(s_end_hour + delta, 23);
            break;
        case ROW_VIBE_STRENGTH:
            s_vibe_strength = wrap_int(
                s_vibe_strength + delta, VIBE_STRENGTH_COUNT - 1);
            break;
        default: break;
    }
}

// ─── Click Handlers ──────────────────────────────────────────────────────────

static void prv_up_click(ClickRecognizerRef r, void *ctx) {
    if (s_editing) {
        prv_adjust_value(s_selected_row, +1);
    } else {
        s_selected_row = wrap_int(s_selected_row - 1, ROW_COUNT - 1);
        prv_ensure_visible();
    }
    prv_refresh();
}

static void prv_down_click(ClickRecognizerRef r, void *ctx) {
    if (s_editing) {
        prv_adjust_value(s_selected_row, -1);
    } else {
        s_selected_row = wrap_int(s_selected_row + 1, ROW_COUNT - 1);
        prv_ensure_visible();
    }
    prv_refresh();
}

static void prv_select_click(ClickRecognizerRef r, void *ctx) {
    s_editing = !s_editing;
    prv_refresh();
}

static void prv_click_config(void *ctx) {
    window_single_click_subscribe(BUTTON_ID_UP,     prv_up_click);
    window_single_click_subscribe(BUTTON_ID_DOWN,   prv_down_click);
    window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click);
}

// ─── Window Lifecycle ────────────────────────────────────────────────────────

static void prv_window_load(Window *window) {
    s_enabled       = settings_get_enabled();
    s_weekdays_only = settings_get_weekdays_only();
    s_start_hour    = settings_get_start_hour();
    s_end_hour      = settings_get_end_hour();
    s_vibe_strength = settings_get_vibe_strength();
    s_selected_row  = 0;
    s_scroll_offset = 0;
    s_editing       = false;

    Layer *root   = window_get_root_layer(window);
    GRect  bounds = layer_get_bounds(root);

    // ── Title bar ──
    s_title = text_layer_create(GRect(0, 0, bounds.size.w, TITLE_HEIGHT));
    text_layer_set_text(s_title, "NapBuster Settings");
    text_layer_set_background_color(s_title, GColorDarkGray);
    text_layer_set_text_color(s_title, GColorWhite);
    text_layer_set_text_alignment(s_title, GTextAlignmentCenter);
    text_layer_set_font(s_title,
        fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
    layer_add_child(root, text_layer_get_layer(s_title));

    // ── Highlight layer (drawn first so text renders on top) ──
    s_highlight = layer_create(bounds);
    layer_set_update_proc(s_highlight, prv_highlight_update);
    layer_add_child(root, s_highlight);

    // ── 3 visible row slots ──
    for (int slot = 0; slot < VISIBLE_ROWS; slot++) {
        int y = TITLE_HEIGHT + slot * ROW_HEIGHT;

        s_row_lbl[slot] = text_layer_create(
            GRect(ROW_PADDING_X,
                  y + 8,
                  bounds.size.w - VAL_WIDTH - ROW_PADDING_X,
                  ROW_HEIGHT - 8));
        text_layer_set_background_color(s_row_lbl[slot], GColorClear);
        text_layer_set_text_color(s_row_lbl[slot], GColorWhite);
        text_layer_set_font(s_row_lbl[slot],
            fonts_get_system_font(FONT_KEY_GOTHIC_18));
        layer_add_child(root, text_layer_get_layer(s_row_lbl[slot]));

        s_row_val[slot] = text_layer_create(
            GRect(bounds.size.w - VAL_WIDTH - ROW_PADDING_X,
                  y + 8,
                  VAL_WIDTH,
                  ROW_HEIGHT - 8));
        text_layer_set_background_color(s_row_val[slot], GColorClear);
        text_layer_set_text_color(s_row_val[slot], GColorYellow);
        text_layer_set_text_alignment(s_row_val[slot], GTextAlignmentRight);
        text_layer_set_font(s_row_val[slot],
            fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
        layer_add_child(root, text_layer_get_layer(s_row_val[slot]));
    }

    // ── Hint bar (bottom) ──
    s_hint = text_layer_create(
        GRect(0, bounds.size.h - HINT_HEIGHT, bounds.size.w, HINT_HEIGHT));
    text_layer_set_background_color(s_hint, GColorDarkGray);
    text_layer_set_text_color(s_hint, GColorLightGray);
    text_layer_set_text_alignment(s_hint, GTextAlignmentCenter);
    text_layer_set_font(s_hint,
        fonts_get_system_font(FONT_KEY_GOTHIC_14));
    layer_add_child(root, text_layer_get_layer(s_hint));

    window_set_click_config_provider(window, prv_click_config);
    prv_refresh();
}

static void prv_window_unload(Window *window) {
    prv_commit();  // save on BACK

    text_layer_destroy(s_title);
    text_layer_destroy(s_hint);
    layer_destroy(s_highlight);
    for (int slot = 0; slot < VISIBLE_ROWS; slot++) {
        text_layer_destroy(s_row_lbl[slot]);
        text_layer_destroy(s_row_val[slot]);
    }

    window_destroy(s_win);
    s_win = NULL;
}

// ─── Public API ──────────────────────────────────────────────────────────────

void settings_window_push(void) {
    if (s_win) return;

    s_win = window_create();
    window_set_background_color(s_win, GColorBlack);

    WindowHandlers h = {
        .load   = prv_window_load,
        .unload = prv_window_unload
    };
    window_set_window_handlers(s_win, h);
    window_stack_push(s_win, true);
}
