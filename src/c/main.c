/**
 * main.c — NapBuster Foreground Application
 *
 * Home screen shows live status based on current settings and time:
 *
 *   GUARDING   Green bg  — inside window, on an active day, enabled
 *   OFF-HOURS  Dark bg   — enabled but outside window or not an active day
 *   SNOOZED    Amber bg  — snooze is active, shows minutes remaining
 *   DISABLED   Dark bg   — master toggle is OFF
 *
 * The display refreshes every minute via TickTimerService and on
 * window_appear (so settings changes show immediately on return).
 *
 * Alarm screen (red bg) takes over when sleep is detected.
 *
 * Button map:
 *   Idle:  long-press SELECT → settings
 *   Alarm: SELECT=dismiss  UP=snooze 10min  DOWN=snooze 30min
 */

#include <pebble.h>
#include "common.h"
#include "settings.h"
#include "days_window.h"

// ─── Constants ───────────────────────────────────────────────────────────────

#define VIBE_REPEAT_INTERVAL_MS  3000

// ─── UI state ────────────────────────────────────────────────────────────────

typedef enum {
    HOME_STATE_GUARDING,
    HOME_STATE_OFF_HOURS,
    HOME_STATE_SNOOZED,
    HOME_STATE_DISABLED,
} HomeState;

// ─── Layers ──────────────────────────────────────────────────────────────────

static Window    *s_win          = NULL;
static Layer     *s_state_bar    = NULL;  // coloured top strip
static TextLayer *s_state_label  = NULL;  // "GUARDING" / "OFF-HOURS" etc.
static TextLayer *s_time_label   = NULL;  // current time HH:MM
static TextLayer *s_detail_label = NULL;  // schedule / snooze info
static TextLayer *s_days_label   = NULL;  // days summary
static TextLayer *s_hint_label   = NULL;  // bottom hint
static TextLayer *s_debug_label  = NULL;  // debug telemetry (GUARDING only)

// ── Alarm side-button labels (shown only during alarm, on red bg) ──
static TextLayer *s_up_label     = NULL;  // UP button label
static TextLayer *s_sel_label    = NULL;  // SELECT button label
static TextLayer *s_dn_label     = NULL;  // DOWN button label

// ─── Runtime state ───────────────────────────────────────────────────────────

static AppTimer *s_vibe_timer  = NULL;
static bool      s_is_alarming = false;

// Colour used by the state bar — set in update_home_screen()
static GColor s_bar_color;

// ─── Forward declarations ────────────────────────────────────────────────────

static void start_alarm(void);
static void stop_alarm(void);
static void schedule_snooze(int minutes);
static void update_home_screen(void);

// ─── Wakeup ──────────────────────────────────────────────────────────────────

static void cancel_existing_wakeup(void) {
    if (persist_exists(PERSIST_KEY_WAKEUP_ID_SNOOZE)) {
        WakeupId wid = (WakeupId)persist_read_int(PERSIST_KEY_WAKEUP_ID_SNOOZE);
        if (wakeup_query(wid, NULL)) wakeup_cancel(wid);
        persist_delete(PERSIST_KEY_WAKEUP_ID_SNOOZE);
    }
}

static void wakeup_handler(WakeupId id, int32_t cookie) {
    if (cookie == WAKEUP_REASON_SNOOZE) {
        persist_write_int(PERSIST_KEY_SNOOZE_UNTIL, 0);
        persist_delete(PERSIST_KEY_WAKEUP_ID_SNOOZE);
        start_alarm();
    }
}

// ─── Vibration ───────────────────────────────────────────────────────────────

static void fire_vibe_pattern(void) {
    VibePattern pat;
    switch (settings_get_vibe_strength()) {
        case 0:  pat = (VibePattern){ VIBE_GENTLE, VIBE_GENTLE_LEN }; break;
        case 2:  pat = (VibePattern){ VIBE_STRONG, VIBE_STRONG_LEN }; break;
        default: pat = (VibePattern){ VIBE_MEDIUM, VIBE_MEDIUM_LEN }; break;
    }
    vibes_enqueue_custom_pattern(pat);
}

static void vibe_timer_callback(void *ctx) {
    s_vibe_timer = NULL;
    if (s_is_alarming) {
        fire_vibe_pattern();
        s_vibe_timer = app_timer_register(VIBE_REPEAT_INTERVAL_MS,
                                          vibe_timer_callback, NULL);
    }
}

// ─── Alarm Control ───────────────────────────────────────────────────────────

static void start_alarm(void) {
    if (s_is_alarming) return;
    s_is_alarming = true;
    persist_write_int(PERSIST_KEY_ALARMING, 1);

    window_set_background_color(s_win, GColorRed);

    text_layer_set_text(s_state_label,  "WAKE UP!");
    text_layer_set_text(s_time_label,   "(>_<)");
    text_layer_set_text(s_detail_label, "You dozed off!");
    text_layer_set_text(s_days_label,   "");
    text_layer_set_text(s_hint_label,   "");  // side labels take over

    // ── Alarm side-button labels (white on red background) ──
    text_layer_set_text_color(s_up_label,  GColorWhite);
    text_layer_set_text_color(s_sel_label, GColorWhite);
    text_layer_set_text_color(s_dn_label,  GColorWhite);
    text_layer_set_text(s_up_label,  "snooze 10m \xe2\x96\xb2");
    text_layer_set_text(s_sel_label, "dismiss \xe2\x97\x8f");
    text_layer_set_text(s_dn_label,  "snooze 30m \xe2\x96\xbc");

    s_bar_color = GColorRed;
    layer_mark_dirty(s_state_bar);

    fire_vibe_pattern();
    s_vibe_timer = app_timer_register(VIBE_REPEAT_INTERVAL_MS,
                                      vibe_timer_callback, NULL);
}

static void stop_alarm(void) {
    s_is_alarming = false;
    if (s_vibe_timer) {
        app_timer_cancel(s_vibe_timer);
        s_vibe_timer = NULL;
    }
    vibes_cancel();
    persist_write_int(PERSIST_KEY_ALARMING, 0);
    persist_write_int(PERSIST_KEY_SNOOZE_UNTIL, 0);
    cancel_existing_wakeup();
    update_home_screen();
}

static void schedule_snooze(int minutes) {
    if (s_vibe_timer) {
        app_timer_cancel(s_vibe_timer);
        s_vibe_timer = NULL;
    }
    vibes_cancel();
    s_is_alarming = false;

    cancel_existing_wakeup();

    time_t snooze_until = time(NULL) + (minutes * 60);
    persist_write_int(PERSIST_KEY_SNOOZE_UNTIL, (int)snooze_until);
    persist_write_int(PERSIST_KEY_ALARMING, 0);

    WakeupId wid = wakeup_schedule(snooze_until, WAKEUP_REASON_SNOOZE, true);
    if (wid >= 0) persist_write_int(PERSIST_KEY_WAKEUP_ID_SNOOZE, (int)wid);

    AppWorkerMessage msg = {
        .data0 = (minutes == 10) ? APP_MSG_SNOOZE_10 : APP_MSG_SNOOZE_30
    };
    app_worker_send_message(msg.data0, &msg);

    update_home_screen();
}

// ─── Home Screen Update ───────────────────────────────────────────────────────

static void prv_state_bar_update(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    graphics_context_set_fill_color(ctx, s_bar_color);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
}

/** Determine current state and refresh all home screen layers. */
static void update_home_screen(void) {
    if (s_is_alarming) return;

    // ── Clear alarm side-button labels and restore normal colors ──
    text_layer_set_text_color(s_up_label,  GColorLightGray);
    text_layer_set_text_color(s_sel_label, GColorLightGray);
    text_layer_set_text_color(s_dn_label,  GColorLightGray);
    text_layer_set_text(s_up_label,  "");
    text_layer_set_text(s_sel_label, "");
    text_layer_set_text(s_dn_label,  "");

    // ── Read current settings ──
    bool enabled     = settings_get_enabled();
    int  start_hour  = settings_get_start_hour();
    int  end_hour    = settings_get_end_hour();
    bool in_window   = is_in_no_nap_window();  // checks days + hours + enabled

    // ── Check snooze ──
    bool snoozed = false;
    int  snooze_mins_left = 0;
    if (persist_exists(PERSIST_KEY_SNOOZE_UNTIL)) {
        time_t snooze_until = (time_t)persist_read_int(PERSIST_KEY_SNOOZE_UNTIL);
        time_t now = time(NULL);
        if (snooze_until > now) {
            snoozed = true;
            snooze_mins_left = (int)((snooze_until - now) / 60) + 1;
        }
    }

    // ── Determine state ──
    HomeState state;
    if (!enabled) {
        state = HOME_STATE_DISABLED;
    } else if (snoozed) {
        state = HOME_STATE_SNOOZED;
    } else if (in_window) {
        state = HOME_STATE_GUARDING;
    } else {
        state = HOME_STATE_OFF_HOURS;
    }

    // ── Current time string (unused now, keeping for potential future use) ──

    // ── Days summary ──
    static char days_buf[12];
    days_summary(days_buf, sizeof(days_buf));

    // ── Schedule line e.g. "11:00 – 23:00" ──
    static char sched_buf[16];
    snprintf(sched_buf, sizeof(sched_buf), "%02d:00 – %02d:00",
             start_hour, end_hour);

    // ── Per-state content ──
    static char detail_buf[48];

    switch (state) {
        case HOME_STATE_GUARDING:
            s_bar_color = GColorIslamicGreen;
            window_set_background_color(s_win, GColorBlack);
            text_layer_set_text_color(s_state_label, GColorIslamicGreen);
            text_layer_set_text(s_state_label,  "GUARDING");
            text_layer_set_text(s_time_label,   "(^_^)");
            snprintf(detail_buf, sizeof(detail_buf), "%s", sched_buf);
            text_layer_set_text(s_detail_label, detail_buf);
            text_layer_set_text(s_days_label,   days_buf);
            text_layer_set_text(s_hint_label,   "Hold SEL: settings");
            break;

        case HOME_STATE_OFF_HOURS: {
            // Work out when the next active period begins
            time_t now = time(NULL);
            struct tm *t = localtime(&now);
            static char next_buf[32];

            // Find next day that is active
            uint8_t active_days = settings_get_active_days();
            int days_ahead = 0;
            for (int i = 1; i <= 7; i++) {
                int candidate_wday = (t->tm_wday + i) % 7;
                if ((active_days >> candidate_wday) & 1) {
                    days_ahead = i;
                    break;
                }
            }
            bool today_active = (active_days >> t->tm_wday) & 1;

            if (today_active && t->tm_hour < start_hour) {
                // Guard starts later today
                snprintf(next_buf, sizeof(next_buf),
                         "Starts at %02d:00", start_hour);
            } else if (days_ahead == 1) {
                snprintf(next_buf, sizeof(next_buf),
                         "Next: tomorrow %02d:00", start_hour);
            } else if (days_ahead > 1) {
                // Name the next active day
                const char *day_names[] = {
                    "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
                int next_wday = (t->tm_wday + days_ahead) % 7;
                snprintf(next_buf, sizeof(next_buf),
                         "Next: %s %02d:00", day_names[next_wday], start_hour);
            } else {
                snprintf(next_buf, sizeof(next_buf), "%s", sched_buf);
            }

            s_bar_color = GColorDarkGray;
            window_set_background_color(s_win, GColorBlack);
            text_layer_set_text_color(s_state_label, GColorLightGray);
            text_layer_set_text(s_state_label,  "OFF-HOURS");
            text_layer_set_text(s_time_label,   "(-_-)");
            text_layer_set_text(s_detail_label, next_buf);
            text_layer_set_text(s_days_label,   days_buf);
            text_layer_set_text(s_hint_label,   "Hold SEL: settings");
            break;
        }

        case HOME_STATE_SNOOZED:
            s_bar_color = GColorChromeYellow;
            window_set_background_color(s_win, GColorBlack);
            text_layer_set_text_color(s_state_label, GColorChromeYellow);
            text_layer_set_text(s_state_label, "SNOOZED");
            text_layer_set_text(s_time_label,  "(- . -)z");
            snprintf(detail_buf, sizeof(detail_buf),
                     "Resuming in\n%d min", snooze_mins_left);
            text_layer_set_text(s_detail_label, detail_buf);
            text_layer_set_text(s_days_label,   "");
            text_layer_set_text(s_hint_label,   "Hold SEL: settings");
            break;

        case HOME_STATE_DISABLED:
            s_bar_color = GColorDarkGray;
            window_set_background_color(s_win, GColorBlack);
            text_layer_set_text_color(s_state_label, GColorDarkGray);
            text_layer_set_text(s_state_label,  "DISABLED");
            text_layer_set_text(s_time_label,   "(._.)");
            snprintf(detail_buf, sizeof(detail_buf), "%s\n%s",
                     sched_buf, days_buf);
            text_layer_set_text(s_detail_label, detail_buf);
            text_layer_set_text(s_days_label,   "");
            text_layer_set_text(s_hint_label,   "Hold SEL: settings");
            break;
    }

    layer_mark_dirty(s_state_bar);

    // ── Debug telemetry display (GUARDING state only) ──
    static char debug_buf[32];
    if (state == HOME_STATE_GUARDING) {
        int16_t dbg_hr  = persist_exists(PERSIST_KEY_DEBUG_HR)
                          ? (int16_t)persist_read_int(PERSIST_KEY_DEBUG_HR) : 0;
        int16_t dbg_avg = persist_exists(PERSIST_KEY_DEBUG_AVG)
                          ? (int16_t)persist_read_int(PERSIST_KEY_DEBUG_AVG) : 0;
        int32_t dbg_acc = persist_exists(PERSIST_KEY_DEBUG_ACCEL)
                          ? persist_read_int(PERSIST_KEY_DEBUG_ACCEL) : 0;
        uint8_t streak  = persist_exists(PERSIST_KEY_TRIGGER_STREAK)
                          ? (uint8_t)persist_read_int(PERSIST_KEY_TRIGGER_STREAK) : 0;
        if (dbg_hr > 0) {
            snprintf(debug_buf, sizeof(debug_buf),
                     "HR:%d avg:%d d:%d x%d",
                     (int)dbg_hr, (int)dbg_avg, (int)dbg_acc, (int)streak);
        } else {
            snprintf(debug_buf, sizeof(debug_buf), "HR: warming up...");
        }
        text_layer_set_text(s_debug_label, debug_buf);
    } else {
        text_layer_set_text(s_debug_label, "");
    }
}

// ─── Tick handler (kept as stub in case minute updates needed later) ─────────

static void worker_message_handler(uint16_t type, AppWorkerMessage *msg) {
    if (type == WORKER_MSG_SLEEP_DETECTED) start_alarm();
}

// ─── Click handlers ──────────────────────────────────────────────────────────

static void select_click(ClickRecognizerRef r, void *ctx) {
    if (s_is_alarming) stop_alarm();
}

static void select_long_click(ClickRecognizerRef r, void *ctx) {
    if (s_is_alarming) stop_alarm();
    settings_window_push();
}

static void up_click(ClickRecognizerRef r, void *ctx) {
    if (s_is_alarming) schedule_snooze(10);
}

static void down_click(ClickRecognizerRef r, void *ctx) {
    if (s_is_alarming) schedule_snooze(30);
}

static void click_config_provider(void *ctx) {
    window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
    window_single_click_subscribe(BUTTON_ID_UP,     up_click);
    window_single_click_subscribe(BUTTON_ID_DOWN,   down_click);
    window_long_click_subscribe(BUTTON_ID_SELECT, 700, select_long_click, NULL);
}

// ─── Window lifecycle ────────────────────────────────────────────────────────

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    update_home_screen();
}

static void main_window_appear(Window *window) {
    // Refresh whenever window comes to front — catches settings changes
    update_home_screen();
    // Subscribe to minute ticks while visible so emu-set-time refreshes the display
    tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
}

static void main_window_disappear(Window *window) {
    tick_timer_service_unsubscribe();
}

static void main_window_load(Window *window) {
    Layer *root   = window_get_root_layer(window);
    GRect  bounds = layer_get_bounds(root);
    int    w      = bounds.size.w;
    int    h      = bounds.size.h;

    s_bar_color = GColorDarkGray;

    // ── Coloured state bar (top strip, 6px tall) ──
    s_state_bar = layer_create(GRect(0, 0, w, 6));
    layer_set_update_proc(s_state_bar, prv_state_bar_update);
    layer_add_child(root, s_state_bar);

    // ── State label e.g. "GUARDING" ──
    s_state_label = text_layer_create(GRect(0, 8, w, 28));
    text_layer_set_background_color(s_state_label, GColorClear);
    text_layer_set_text_alignment(s_state_label, GTextAlignmentCenter);
    text_layer_set_font(s_state_label,
        fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
    layer_add_child(root, text_layer_get_layer(s_state_label));

    // ── Divider line ──
    // (drawn as a 1px-tall layer with a dark background)
    Layer *divider = layer_create(GRect(8, 38, w - 16, 1));
    // We can't draw on it without update_proc, so just leave it as black gap

    // ── ASCII face (replaces clock — not a watchface!) ──
    s_time_label = text_layer_create(GRect(0, 38, w, 52));
    text_layer_set_background_color(s_time_label, GColorClear);
    text_layer_set_text_color(s_time_label, GColorWhite);
    text_layer_set_text_alignment(s_time_label, GTextAlignmentCenter);
    text_layer_set_font(s_time_label,
        fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
    layer_add_child(root, text_layer_get_layer(s_time_label));

    // ── Detail line (schedule / snooze countdown / next active time) ──
    s_detail_label = text_layer_create(GRect(4, 98, w - 8, 46));
    text_layer_set_background_color(s_detail_label, GColorClear);
    text_layer_set_text_color(s_detail_label, GColorLightGray);
    text_layer_set_text_alignment(s_detail_label, GTextAlignmentCenter);
    text_layer_set_font(s_detail_label,
        fonts_get_system_font(FONT_KEY_GOTHIC_18));
    layer_add_child(root, text_layer_get_layer(s_detail_label));

    // ── Days summary (smaller, below detail) ──
    s_days_label = text_layer_create(GRect(4, 146, w - 8, 20));
    text_layer_set_background_color(s_days_label, GColorClear);
    text_layer_set_text_color(s_days_label, GColorDarkGray);
    text_layer_set_text_alignment(s_days_label, GTextAlignmentCenter);
    text_layer_set_font(s_days_label,
        fonts_get_system_font(FONT_KEY_GOTHIC_14));
    layer_add_child(root, text_layer_get_layer(s_days_label));

    // ── Debug telemetry line (subtle, between days and hint bar) ──
    // Positioned 38px from bottom: 18px hint + 2px gap + 18px debug
    s_debug_label = text_layer_create(GRect(4, h - 38, w - 8, 18));
    text_layer_set_background_color(s_debug_label, GColorClear);
    text_layer_set_text_color(s_debug_label, GColorDarkGray);
    text_layer_set_text_alignment(s_debug_label, GTextAlignmentCenter);
    text_layer_set_font(s_debug_label,
        fonts_get_system_font(FONT_KEY_GOTHIC_14));
    layer_add_child(root, text_layer_get_layer(s_debug_label));

    // ── Hint bar (bottom) ──
    s_hint_label = text_layer_create(GRect(0, h - 18, w, 18));
    text_layer_set_background_color(s_hint_label, GColorDarkGray);
    text_layer_set_text_color(s_hint_label, GColorLightGray);
    text_layer_set_text_alignment(s_hint_label, GTextAlignmentCenter);
    text_layer_set_font(s_hint_label,
        fonts_get_system_font(FONT_KEY_GOTHIC_14));
    layer_add_child(root, text_layer_get_layer(s_hint_label));

    // ── Alarm side-button labels (right edge, one per physical button) ──
    // Pebble Time 2 (emery): 200×228px
    // Button vertical centres: UP≈57, SEL≈114, DN≈171
    s_up_label = text_layer_create(GRect(w - 80, 57 - 9, 80, 18));
    text_layer_set_background_color(s_up_label, GColorClear);
    text_layer_set_text_color(s_up_label, GColorLightGray);
    text_layer_set_text_alignment(s_up_label, GTextAlignmentRight);
    text_layer_set_font(s_up_label,
        fonts_get_system_font(FONT_KEY_GOTHIC_14));
    layer_add_child(root, text_layer_get_layer(s_up_label));

    s_sel_label = text_layer_create(GRect(w - 80, 114 - 9, 80, 18));
    text_layer_set_background_color(s_sel_label, GColorClear);
    text_layer_set_text_color(s_sel_label, GColorLightGray);
    text_layer_set_text_alignment(s_sel_label, GTextAlignmentRight);
    text_layer_set_font(s_sel_label,
        fonts_get_system_font(FONT_KEY_GOTHIC_14));
    layer_add_child(root, text_layer_get_layer(s_sel_label));

    s_dn_label = text_layer_create(GRect(w - 80, 171 - 9, 80, 18));
    text_layer_set_background_color(s_dn_label, GColorClear);
    text_layer_set_text_color(s_dn_label, GColorLightGray);
    text_layer_set_text_alignment(s_dn_label, GTextAlignmentRight);
    text_layer_set_font(s_dn_label,
        fonts_get_system_font(FONT_KEY_GOTHIC_14));
    layer_add_child(root, text_layer_get_layer(s_dn_label));

    window_set_click_config_provider(window, click_config_provider);
    (void)divider;  // suppress unused variable warning
}

static void main_window_unload(Window *window) {
    text_layer_destroy(s_state_label);
    text_layer_destroy(s_time_label);
    text_layer_destroy(s_detail_label);
    text_layer_destroy(s_days_label);
    text_layer_destroy(s_hint_label);
    text_layer_destroy(s_debug_label);
    text_layer_destroy(s_up_label);
    text_layer_destroy(s_sel_label);
    text_layer_destroy(s_dn_label);
    layer_destroy(s_state_bar);
}

// ─── App lifecycle ────────────────────────────────────────────────────────────

static void app_init(void) {
    wakeup_service_subscribe(wakeup_handler);
    app_worker_message_subscribe(worker_message_handler);

    // No minute tick needed — face doesn't change on time, only on state changes
    // tick_timer_service_subscribe removed to save battery

    s_win = window_create();
    window_set_background_color(s_win, GColorBlack);

    WindowHandlers wh = {
        .load      = main_window_load,
        .appear    = main_window_appear,
        .disappear = main_window_disappear,
        .unload    = main_window_unload
    };
    window_set_window_handlers(s_win, wh);
    window_stack_push(s_win, true);

    // Ensure background worker is running
    AppWorkerResult res = app_worker_launch();
    if (res != APP_WORKER_RESULT_SUCCESS &&
        res != APP_WORKER_RESULT_ALREADY_RUNNING) {
        APP_LOG(APP_LOG_LEVEL_WARNING,
            "NapBuster: worker launch failed (result=%d)", res);
    }

    // Wakeup launch → snooze expired
    if (launch_reason() == APP_LAUNCH_WAKEUP) {
        WakeupId wid;
        int32_t  cookie;
        if (wakeup_get_launch_event(&wid, &cookie) &&
            cookie == WAKEUP_REASON_SNOOZE) {
            persist_write_int(PERSIST_KEY_SNOOZE_UNTIL, 0);
            persist_delete(PERSIST_KEY_WAKEUP_ID_SNOOZE);
            start_alarm();
            return;
        }
    }

    // Worker launch → sleep detected
    if (launch_reason() == APP_LAUNCH_WORKER) {
        start_alarm();
        return;
    }

    // Normal launch → draw home screen (appear handler fires after load)
}

static void app_deinit(void) {
    if (s_vibe_timer) {
        app_timer_cancel(s_vibe_timer);
        s_vibe_timer = NULL;
    }
    vibes_cancel();
    app_worker_message_unsubscribe();
    window_destroy(s_win);
}

// ─── Entry point ─────────────────────────────────────────────────────────────

int main(void) {
    app_init();
    app_event_loop();
    app_deinit();
    return 0;
}
