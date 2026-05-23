/**
 * days_window.c — NapBuster Day Picker
 *
 * A scrollable 7-row window, one row per day of the week.
 * Each row shows the day name and a checkmark if active.
 *
 *   UP / DOWN  — navigate rows (wraps)
 *   SELECT     — toggle the selected day on / off
 *   BACK       — save bitmask to flash, notify worker, close
 *
 * Bitmask convention (matches tm_wday):
 *   bit 0 = Sunday ... bit 6 = Saturday
 */

#include "days_window.h"
#include "common.h"

// ─── Layout ───────────────────────────────────────────────────────────────────

#define TITLE_HEIGHT   28
#define HINT_HEIGHT    20
#define VISIBLE_ROWS    3
#define ROW_HEIGHT     44
#define ROW_PADDING_X   6
#define CHECK_WIDTH    28
#define SCROLLBAR_W     3   // width reserved for checkmark on the right

static const char * const DAY_NAMES[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday",
    "Thursday", "Friday", "Saturday"
};

// ─── State ───────────────────────────────────────────────────────────────────

static Window    *s_win           = NULL;
static TextLayer *s_title         = NULL;
static TextLayer *s_hint          = NULL;
static TextLayer *s_lbl[VISIBLE_ROWS];
static TextLayer *s_chk[VISIBLE_ROWS];
static Layer     *s_highlight     = NULL;
static Layer     *s_scrollbar     = NULL;

static uint8_t s_days;           // working copy of bitmask
static int     s_selected_row = 0;
static int     s_scroll_offset = 0;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static bool prv_day_active(int day) {
    return (s_days >> day) & 1;
}

static void prv_toggle_day(int day) {
    s_days ^= (1 << day);
    // Never allow all days deselected — re-enable the day if that would happen
    if (s_days == 0) s_days = (1 << day);
}

static void prv_ensure_visible(void) {
    if (s_selected_row < s_scroll_offset)
        s_scroll_offset = s_selected_row;
    else if (s_selected_row >= s_scroll_offset + VISIBLE_ROWS)
        s_scroll_offset = s_selected_row - VISIBLE_ROWS + 1;
}

static void prv_refresh(void) {
    for (int slot = 0; slot < VISIBLE_ROWS; slot++) {
        int day = s_scroll_offset + slot;
        if (day < 7) {
            text_layer_set_text(s_lbl[slot], DAY_NAMES[day]);
            // Use filled/empty circle — works in all Pebble system fonts
            // and matches the style of the built-in alarm app
            text_layer_set_text(s_chk[slot],
                prv_day_active(day) ? "(o)" : "( )");
            bool selected = (day == s_selected_row);
            GColor tc = selected ? GColorWhite : GColorLightGray;
            GColor cc = prv_day_active(day)
                ? (selected ? GColorWhite : GColorMalachite)
                : GColorDarkGray;
            text_layer_set_text_color(s_lbl[slot], tc);
            text_layer_set_text_color(s_chk[slot], cc);
        } else {
            text_layer_set_text(s_lbl[slot], "");
            text_layer_set_text(s_chk[slot], "");
        }
    }
    layer_mark_dirty(s_highlight);
    layer_mark_dirty(s_scrollbar);
}

static void prv_commit(void) {
    persist_write_int(PERSIST_KEY_ACTIVE_DAYS, (int)s_days);
    AppWorkerMessage msg = { .data0 = APP_MSG_SETTINGS_CHANGED };
    app_worker_send_message(APP_MSG_SETTINGS_CHANGED, &msg);
}

// ─── Public: summary string ───────────────────────────────────────────────────

void days_summary(char *buf, size_t len) {
    uint8_t days = persist_exists(PERSIST_KEY_ACTIVE_DAYS)
        ? (uint8_t)persist_read_int(PERSIST_KEY_ACTIVE_DAYS)
        : DEFAULT_ACTIVE_DAYS;

    if (days == 0x7F) {           // all 7 days
        snprintf(buf, len, "Every day");
    } else if (days == 0x3E) {    // Mon-Fri (bits 1-5)
        snprintf(buf, len, "Weekdays");
    } else if (days == 0x41) {    // Sat+Sun (bits 0+6)
        snprintf(buf, len, "Weekends");
    } else {
        // Count active days
        int count = 0;
        for (int i = 0; i < 7; i++) if ((days >> i) & 1) count++;
        snprintf(buf, len, "%d days", count);
    }
}

// ─── Highlight Layer ─────────────────────────────────────────────────────────

static void prv_highlight_update(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    int slot = s_selected_row - s_scroll_offset;
    if (slot < 0 || slot >= VISIBLE_ROWS) return;
    int y = TITLE_HEIGHT + slot * ROW_HEIGHT;
    graphics_context_set_fill_color(ctx, GColorOxfordBlue);
    graphics_fill_rect(ctx, GRect(0, y, bounds.size.w - SCROLLBAR_W, ROW_HEIGHT),
                       0, GCornerNone);
}

static void prv_scrollbar_update(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    int content_h = bounds.size.h - TITLE_HEIGHT - HINT_HEIGHT;

    // Track
    graphics_context_set_fill_color(ctx, GColorDarkGray);
    graphics_fill_rect(ctx,
        GRect(bounds.size.w - SCROLLBAR_W, TITLE_HEIGHT,
              SCROLLBAR_W, content_h), 0, GCornerNone);

    // Thumb
    int thumb_h = (content_h * VISIBLE_ROWS) / 7;
    int thumb_y = TITLE_HEIGHT + (content_h * s_scroll_offset) / 7;
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx,
        GRect(bounds.size.w - SCROLLBAR_W, thumb_y,
              SCROLLBAR_W, thumb_h), 0, GCornerNone);
}

// ─── Click Handlers ──────────────────────────────────────────────────────────

static void prv_up_click(ClickRecognizerRef r, void *ctx) {
    s_selected_row = (s_selected_row + 6) % 7;  // wrap upward
    prv_ensure_visible();
    prv_refresh();
}

static void prv_down_click(ClickRecognizerRef r, void *ctx) {
    s_selected_row = (s_selected_row + 1) % 7;  // wrap downward
    prv_ensure_visible();
    prv_refresh();
}

static void prv_select_click(ClickRecognizerRef r, void *ctx) {
    prv_toggle_day(s_selected_row);
    prv_refresh();
}

static void prv_click_config(void *ctx) {
    window_single_click_subscribe(BUTTON_ID_UP,     prv_up_click);
    window_single_click_subscribe(BUTTON_ID_DOWN,   prv_down_click);
    window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click);
}

// ─── Window Lifecycle ────────────────────────────────────────────────────────

static void prv_window_load(Window *window) {
    s_days = persist_exists(PERSIST_KEY_ACTIVE_DAYS)
        ? (uint8_t)persist_read_int(PERSIST_KEY_ACTIVE_DAYS)
        : DEFAULT_ACTIVE_DAYS;
    s_selected_row  = 1;   // start on Monday
    s_scroll_offset = 0;

    Layer *root   = window_get_root_layer(window);
    GRect  bounds = layer_get_bounds(root);

    // Title
    s_title = text_layer_create(GRect(0, 0, bounds.size.w, TITLE_HEIGHT));
    text_layer_set_text(s_title, "Active Days");
    text_layer_set_background_color(s_title, GColorDarkGray);
    text_layer_set_text_color(s_title, GColorWhite);
    text_layer_set_text_alignment(s_title, GTextAlignmentCenter);
    text_layer_set_font(s_title,
        fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
    layer_add_child(root, text_layer_get_layer(s_title));

    // Highlight behind rows
    s_highlight = layer_create(bounds);
    layer_set_update_proc(s_highlight, prv_highlight_update);
    layer_add_child(root, s_highlight);

    // 3 visible row slots
    int chk_w = 36;  // wide enough for "(o)"
    for (int slot = 0; slot < VISIBLE_ROWS; slot++) {
        int y = TITLE_HEIGHT + slot * ROW_HEIGHT;

        s_lbl[slot] = text_layer_create(
            GRect(ROW_PADDING_X,
                  y + 10,
                  bounds.size.w - chk_w - SCROLLBAR_W - ROW_PADDING_X,
                  ROW_HEIGHT - 10));
        text_layer_set_background_color(s_lbl[slot], GColorClear);
        text_layer_set_font(s_lbl[slot],
            fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
        layer_add_child(root, text_layer_get_layer(s_lbl[slot]));

        s_chk[slot] = text_layer_create(
            GRect(bounds.size.w - chk_w - SCROLLBAR_W - ROW_PADDING_X,
                  y + 10,
                  chk_w,
                  ROW_HEIGHT - 10));
        text_layer_set_background_color(s_chk[slot], GColorClear);
        text_layer_set_text_alignment(s_chk[slot], GTextAlignmentRight);
        text_layer_set_font(s_chk[slot],
            fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
        layer_add_child(root, text_layer_get_layer(s_chk[slot]));
    }

    // Scrollbar (drawn on top)
    s_scrollbar = layer_create(bounds);
    layer_set_update_proc(s_scrollbar, prv_scrollbar_update);
    layer_add_child(root, s_scrollbar);

    // Hint bar
    s_hint = text_layer_create(
        GRect(0, bounds.size.h - HINT_HEIGHT, bounds.size.w, HINT_HEIGHT));
    text_layer_set_background_color(s_hint, GColorDarkGray);
    text_layer_set_text_color(s_hint, GColorLightGray);
    text_layer_set_text_alignment(s_hint, GTextAlignmentCenter);
    text_layer_set_font(s_hint, fonts_get_system_font(FONT_KEY_GOTHIC_14));
    text_layer_set_text(s_hint, "SEL: toggle  BACK: save");
    layer_add_child(root, text_layer_get_layer(s_hint));

    window_set_click_config_provider(window, prv_click_config);
    prv_refresh();
}

static void prv_window_unload(Window *window) {
    prv_commit();

    text_layer_destroy(s_title);
    text_layer_destroy(s_hint);
    layer_destroy(s_highlight);
    layer_destroy(s_scrollbar);
    for (int slot = 0; slot < VISIBLE_ROWS; slot++) {
        text_layer_destroy(s_lbl[slot]);
        text_layer_destroy(s_chk[slot]);
    }

    window_destroy(s_win);
    s_win = NULL;
}

// ─── Public API ──────────────────────────────────────────────────────────────

void days_window_push(void) {
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
