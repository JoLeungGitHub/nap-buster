/**
 * settings.c — NapBuster Settings Screen
 *
 * Five settings rows with dynamic visible count + scrollbar:
 *
 *   ROW 0  Guard          ON / OFF
 *   ROW 1  Active days    Every day / Weekdays / N days  → opens day picker
 *   ROW 2  No-nap from    HH:00  (wrapping 0-23)
 *   ROW 3  No-nap until   HH:00  (wrapping 0-23)
 *   ROW 4  Wake vibration Gentle / Medium / Strong + test buzz on change
 *
 * Navigation:
 *   UP / DOWN  — scroll rows (idle)  |  adjust value (editing)
 *   SELECT     — enter / confirm edit  |  Active days → push day picker
 *   BACK       — save to flash, notify worker, close
 *
 * Layout adapts to screen height:
 *   emery  200x228 → 4 visible rows (no blank row wasted)
 *   basalt/diorite 144x168 → 3 visible rows
 *   chalk  180x180 → 3 visible rows
 *
 * Scrollbar: 3px wide on right edge, thumb position reflects scroll offset.
 * Selected row text is always white regardless of highlight colour.
 */

#include "settings.h"
#include "common.h"
#include "days_window.h"

// ─── Row definitions ──────────────────────────────────────────────────────────

typedef enum {
    ROW_ENABLED = 0,
    ROW_ACTIVE_DAYS,
    ROW_START_HOUR,
    ROW_END_HOUR,
    ROW_VIBE_STRENGTH,
    ROW_COUNT
} SettingsRow;

static const char * const ROW_LABELS[ROW_COUNT] = {
    "Guard",
    "Active days",
    "No-nap from",
    "No-nap until",
    "Wake vibration"
};

// ─── Layout constants ─────────────────────────────────────────────────────────

#define TITLE_HEIGHT    28
#define HINT_HEIGHT     20
#define ROW_PADDING_X    6
#define VAL_WIDTH       68
#define SCROLLBAR_W      3
#define ROW_HEIGHT      40   // slightly tighter to fit 4 rows on emery

// Computed at load time from screen height
static int s_visible_rows = 3;

// ─── State ───────────────────────────────────────────────────────────────────

static Window    *s_win = NULL;
static TextLayer *s_title   = NULL;
static TextLayer *s_hint    = NULL;
// Dynamic slot arrays — allocated in window_load
static TextLayer **s_row_lbl = NULL;
static TextLayer **s_row_val = NULL;
static Layer     *s_highlight = NULL;
static Layer     *s_scrollbar = NULL;

static bool s_enabled;
static int  s_start_hour;
static int  s_end_hour;
static int  s_vibe_strength;

static int  s_selected_row  = 0;
static int  s_scroll_offset = 0;
static bool s_editing       = false;

static char s_val_buf[ROW_COUNT][16];

// ─── Helpers ─────────────────────────────────────────────────────────────────

static int wrap_int(int val, int max) {
    if (val < 0)   return max;
    if (val > max) return 0;
    return val;
}

static void prv_format_value(int row, char *buf, size_t len) {
    switch (row) {
        case ROW_ENABLED:
            snprintf(buf, len, "%s", s_enabled ? "ON" : "OFF");
            break;
        case ROW_ACTIVE_DAYS:
            days_summary(buf, len);
            break;
        case ROW_START_HOUR:
            snprintf(buf, len, "%02d:00", s_start_hour);
            break;
        case ROW_END_HOUR:
            snprintf(buf, len, "%02d:00", s_end_hour);
            break;
        case ROW_VIBE_STRENGTH:
            snprintf(buf, len, "%s", VIBE_STRENGTH_LABELS[s_vibe_strength]);
            break;
        default:
            buf[0] = '\0';
            break;
    }
}

static void prv_ensure_visible(void) {
    if (s_selected_row < s_scroll_offset)
        s_scroll_offset = s_selected_row;
    else if (s_selected_row >= s_scroll_offset + s_visible_rows)
        s_scroll_offset = s_selected_row - s_visible_rows + 1;
}

static void prv_commit(void) {
    persist_write_int(PERSIST_KEY_ENABLED,       (int)s_enabled);
    persist_write_int(PERSIST_KEY_START_HOUR,    s_start_hour);
    persist_write_int(PERSIST_KEY_END_HOUR,      s_end_hour);
    persist_write_int(PERSIST_KEY_VIBE_STRENGTH, s_vibe_strength);
    // PERSIST_KEY_ACTIVE_DAYS written by days_window.c directly

    AppWorkerMessage msg = { .data0 = APP_MSG_SETTINGS_CHANGED };
    app_worker_send_message(APP_MSG_SETTINGS_CHANGED, &msg);
}

static void prv_refresh(void) {
    for (int slot = 0; slot < s_visible_rows; slot++) {
        int row = s_scroll_offset + slot;
        bool selected = (row == s_selected_row);

        if (row < ROW_COUNT) {
            text_layer_set_text(s_row_lbl[slot], ROW_LABELS[row]);
            prv_format_value(row, s_val_buf[row], sizeof(s_val_buf[row]));
            text_layer_set_text(s_row_val[slot], s_val_buf[row]);
        } else {
            text_layer_set_text(s_row_lbl[slot], "");
            text_layer_set_text(s_row_val[slot], "");
        }

        // Selected row: always white text so it reads on any highlight colour
        GColor text_col = selected ? GColorWhite : GColorLightGray;
        GColor val_col  = selected ? GColorWhite : GColorChromeYellow;
        text_layer_set_text_color(s_row_lbl[slot], text_col);
        text_layer_set_text_color(s_row_val[slot], val_col);
    }

    text_layer_set_text(s_hint,
        s_editing ? "UP/DN: change  SEL: done" : "SEL: edit  BACK: save");

    layer_mark_dirty(s_highlight);
    layer_mark_dirty(s_scrollbar);
}

// ─── Highlight Layer ─────────────────────────────────────────────────────────

static void prv_highlight_update(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    int slot = s_selected_row - s_scroll_offset;
    if (slot < 0 || slot >= s_visible_rows) return;

    int y = TITLE_HEIGHT + slot * ROW_HEIGHT;
    graphics_context_set_fill_color(ctx,
        s_editing ? GColorCobaltBlue : GColorOxfordBlue);
    graphics_fill_rect(ctx, GRect(0, y, bounds.size.w - SCROLLBAR_W, ROW_HEIGHT),
                       0, GCornerNone);
}

// ─── Scrollbar Layer ──────────────────────────────────────────────────────────

static void prv_scrollbar_update(Layer *layer, GContext *ctx) {
    if (ROW_COUNT <= s_visible_rows) return;  // no scrollbar needed

    GRect bounds = layer_get_bounds(layer);
    int content_h = bounds.size.h - TITLE_HEIGHT - HINT_HEIGHT;

    // Track (background)
    graphics_context_set_fill_color(ctx, GColorDarkGray);
    graphics_fill_rect(ctx,
        GRect(bounds.size.w - SCROLLBAR_W, TITLE_HEIGHT,
              SCROLLBAR_W, content_h),
        0, GCornerNone);

    // Thumb
    int thumb_h = (content_h * s_visible_rows) / ROW_COUNT;
    int thumb_y = TITLE_HEIGHT +
        (content_h * s_scroll_offset) / ROW_COUNT;

    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx,
        GRect(bounds.size.w - SCROLLBAR_W, thumb_y,
              SCROLLBAR_W, thumb_h),
        0, GCornerNone);
}

// ─── Adjust value + test vibe for strength ───────────────────────────────────

static void prv_adjust_value(int row, int delta) {
    switch (row) {
        case ROW_ENABLED:
            s_enabled = !s_enabled;
            break;
        case ROW_ACTIVE_DAYS:
            break;
        case ROW_START_HOUR:
            s_start_hour = wrap_int(s_start_hour + delta, 23);
            break;
        case ROW_END_HOUR:
            s_end_hour = wrap_int(s_end_hour + delta, 23);
            break;
        case ROW_VIBE_STRENGTH: {
            s_vibe_strength = wrap_int(
                s_vibe_strength + delta, VIBE_STRENGTH_COUNT - 1);
            // Fire a short test buzz so the user can feel the difference
            VibePattern pat;
            switch (s_vibe_strength) {
                case 0:
                    pat = (VibePattern){
                        .durations = VIBE_GENTLE,
                        .num_segments = VIBE_GENTLE_LEN };
                    break;
                case 2:
                    pat = (VibePattern){
                        .durations = VIBE_STRONG,
                        .num_segments = VIBE_STRONG_LEN };
                    break;
                default:
                    pat = (VibePattern){
                        .durations = VIBE_MEDIUM,
                        .num_segments = VIBE_MEDIUM_LEN };
                    break;
            }
            vibes_cancel();
            vibes_enqueue_custom_pattern(pat);
            break;
        }
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
    if (s_selected_row == ROW_ACTIVE_DAYS) {
        prv_commit();
        days_window_push();
        return;
    }
    s_editing = !s_editing;
    prv_refresh();
}

static void prv_click_config(void *ctx) {
    window_single_click_subscribe(BUTTON_ID_UP,     prv_up_click);
    window_single_click_subscribe(BUTTON_ID_DOWN,   prv_down_click);
    window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click);
}

// ─── Window Lifecycle ────────────────────────────────────────────────────────

static void prv_window_appear(Window *window) {
    prv_refresh();
}

static void prv_window_load(Window *window) {
    s_enabled       = settings_get_enabled();
    s_start_hour    = settings_get_start_hour();
    s_end_hour      = settings_get_end_hour();
    s_vibe_strength = settings_get_vibe_strength();
    s_selected_row  = 0;
    s_scroll_offset = 0;
    s_editing       = false;

    Layer *root   = window_get_root_layer(window);
    GRect  bounds = layer_get_bounds(root);

    // Compute how many rows fit on this screen
    int content_h = bounds.size.h - TITLE_HEIGHT - HINT_HEIGHT;
    s_visible_rows = content_h / ROW_HEIGHT;
    if (s_visible_rows < 1) s_visible_rows = 1;
    if (s_visible_rows > ROW_COUNT) s_visible_rows = ROW_COUNT;

    // Allocate slot arrays
    s_row_lbl = (TextLayer **)malloc(s_visible_rows * sizeof(TextLayer *));
    s_row_val = (TextLayer **)malloc(s_visible_rows * sizeof(TextLayer *));

    // Title
    s_title = text_layer_create(GRect(0, 0, bounds.size.w, TITLE_HEIGHT));
    text_layer_set_text(s_title, "NapBuster Settings");
    text_layer_set_background_color(s_title, GColorDarkGray);
    text_layer_set_text_color(s_title, GColorWhite);
    text_layer_set_text_alignment(s_title, GTextAlignmentCenter);
    text_layer_set_font(s_title,
        fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
    layer_add_child(root, text_layer_get_layer(s_title));

    // Highlight
    s_highlight = layer_create(bounds);
    layer_set_update_proc(s_highlight, prv_highlight_update);
    layer_add_child(root, s_highlight);

    // Row slots
    for (int slot = 0; slot < s_visible_rows; slot++) {
        int y = TITLE_HEIGHT + slot * ROW_HEIGHT;
        int lbl_w = bounds.size.w - VAL_WIDTH - SCROLLBAR_W - ROW_PADDING_X;

        s_row_lbl[slot] = text_layer_create(
            GRect(ROW_PADDING_X, y + 8, lbl_w, ROW_HEIGHT - 8));
        text_layer_set_background_color(s_row_lbl[slot], GColorClear);
        text_layer_set_font(s_row_lbl[slot],
            fonts_get_system_font(FONT_KEY_GOTHIC_18));
        layer_add_child(root, text_layer_get_layer(s_row_lbl[slot]));

        s_row_val[slot] = text_layer_create(
            GRect(bounds.size.w - VAL_WIDTH - SCROLLBAR_W - ROW_PADDING_X,
                  y + 8, VAL_WIDTH, ROW_HEIGHT - 8));
        text_layer_set_background_color(s_row_val[slot], GColorClear);
        text_layer_set_text_alignment(s_row_val[slot], GTextAlignmentRight);
        text_layer_set_font(s_row_val[slot],
            fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
        layer_add_child(root, text_layer_get_layer(s_row_val[slot]));
    }

    // Scrollbar
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
    for (int slot = 0; slot < s_visible_rows; slot++) {
        text_layer_destroy(s_row_lbl[slot]);
        text_layer_destroy(s_row_val[slot]);
    }
    free(s_row_lbl);
    free(s_row_val);

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
        .appear = prv_window_appear,
        .unload = prv_window_unload
    };
    window_set_window_handlers(s_win, h);
    window_stack_push(s_win, true);
}
