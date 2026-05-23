/**
 * main.c — NapBuster Foreground Application
 *
 * Responsibilities:
 *  - Display "Wake Up!" alarm screen when launched by the worker or wakeup
 *  - Drive repeating vibration while alarming
 *  - SELECT = dismiss, UP = snooze 10 min, DOWN = snooze 30 min
 *  - Long-press SELECT (idle) → opens Settings screen
 *  - Schedule wakeup via Wakeup API for snooze re-arm
 *
 * Launch contexts:
 *  1. Normal user launch      → show status screen, start worker
 *  2. Wakeup launch           → immediately show alarm (snooze expired)
 *  3. Worker-launched         → immediately show alarm (sleep detected)
 */

#include <pebble.h>
#include "common.h"
#include "settings.h"

// ─── Constants ───────────────────────────────────────────────────────────────

// How often to re-fire the vibe pattern while alarming (ms)
#define VIBE_REPEAT_INTERVAL_MS  3000

// ─── State ───────────────────────────────────────────────────────────────────

static Window    *s_main_window  = NULL;
static TextLayer *s_wake_label   = NULL;   // "Wake Up!" / "NapBuster"
static TextLayer *s_icon_label   = NULL;   // ASCII face / emoji
static TextLayer *s_status_label = NULL;   // window status / snooze info
static TextLayer *s_hint_label   = NULL;   // button hints

static AppTimer  *s_vibe_timer   = NULL;
static bool       s_is_alarming  = false;

// ─── Forward Declarations ────────────────────────────────────────────────────

static void start_alarm(void);
static void stop_alarm(void);
static void schedule_snooze(int minutes);
static void update_status_display(void);

// ─── Wakeup ──────────────────────────────────────────────────────────────────

static void cancel_existing_wakeup(void) {
    if (persist_exists(PERSIST_KEY_WAKEUP_ID_SNOOZE)) {
        WakeupId wid = (WakeupId)persist_read_int(PERSIST_KEY_WAKEUP_ID_SNOOZE);
        if (wakeup_query(wid, NULL)) {
            wakeup_cancel(wid);
        }
        persist_delete(PERSIST_KEY_WAKEUP_ID_SNOOZE);
    }
}

static void wakeup_handler(WakeupId id, int32_t cookie) {
    if (cookie == WAKEUP_REASON_SNOOZE) {
        persist_write_int(PERSIST_KEY_SNOOZE_UNTIL, 0);
        persist_delete(PERSIST_KEY_WAKEUP_ID);
        start_alarm();
    }
}

// ─── Vibration ───────────────────────────────────────────────────────────────

static void fire_vibe_pattern(void) {
    VibePattern pat = {
        .durations    = VIBE_SEGMENTS,
        .num_segments = VIBE_PATTERN_SEGMENTS_LEN
    };
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

    text_layer_set_text(s_wake_label,  "Wake Up!");
    text_layer_set_text(s_icon_label,  "(>_<)");
    text_layer_set_text(s_hint_label,  "SEL: dismiss\nUP: +10min\nDN: +30min");
    text_layer_set_text(s_status_label, "");

    window_set_background_color(s_main_window, GColorRed);

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

    persist_write_int(PERSIST_KEY_ALARMING,     0);
    persist_write_int(PERSIST_KEY_SNOOZE_UNTIL, 0);
    cancel_existing_wakeup();

    window_set_background_color(s_main_window, GColorBlack);
    update_status_display();
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

    WakeupId wid = wakeup_schedule(snooze_until, WAKEUP_REASON_SNOOZE, true);
    if (wid >= 0) {
        persist_write_int(PERSIST_KEY_WAKEUP_ID_SNOOZE, (int)wid);
    }

    window_set_background_color(s_main_window, GColorBlack);

    static char snooze_buf[32];
    snprintf(snooze_buf, sizeof(snooze_buf), "Snoozed\n%d min", minutes);
    text_layer_set_text(s_wake_label,   snooze_buf);
    text_layer_set_text(s_icon_label,   "~ z Z");
    text_layer_set_text(s_hint_label,   "");
    text_layer_set_text(s_status_label, "");

    // Notify worker
    AppWorkerMessage msg = { .data0 = (minutes == 10) ? APP_MSG_SNOOZE_10 : APP_MSG_SNOOZE_30 };
    app_worker_send_message(msg.data0, &msg);
}

// ─── Status Display ──────────────────────────────────────────────────────────

static void update_status_display(void) {
    if (s_is_alarming) return;

    bool enabled    = settings_get_enabled();
    int  start_hour = settings_get_start_hour();
    int  end_hour   = settings_get_end_hour();
    bool in_window  = is_in_no_nap_window();

    text_layer_set_text(s_wake_label, "NapBuster");

    static char status_buf[64];
    if (!enabled) {
        snprintf(status_buf, sizeof(status_buf), "DISABLED\n\nHold SEL\nfor settings");
    } else {
        snprintf(status_buf, sizeof(status_buf),
            "%s\n%02d:00 – %02d:00\n\nHold SEL = settings",
            in_window ? "GUARDING" : "outside window",
            start_hour, end_hour);
    }
    text_layer_set_text(s_status_label, status_buf);
    text_layer_set_text(s_icon_label,   enabled ? "(^_^)" : "(-_-)");
    text_layer_set_text(s_hint_label,   "");
}

// ─── Worker Message Handler ───────────────────────────────────────────────────

static void worker_message_handler(uint16_t type, AppWorkerMessage *msg) {
    switch (type) {
        case WORKER_MSG_SLEEP_DETECTED:
        case WORKER_MSG_SNOOZE_EXPIRED:
            start_alarm();
            break;
        default:
            break;
    }
}

// ─── Click Handlers ──────────────────────────────────────────────────────────

static void select_click_handler(ClickRecognizerRef recognizer, void *ctx) {
    if (s_is_alarming) {
        stop_alarm();
    }
}

static void select_long_click_handler(ClickRecognizerRef recognizer, void *ctx) {
    if (s_is_alarming) stop_alarm();
    settings_window_push();
}

static void up_click_handler(ClickRecognizerRef recognizer, void *ctx) {
    if (s_is_alarming) schedule_snooze(10);
}

static void down_click_handler(ClickRecognizerRef recognizer, void *ctx) {
    if (s_is_alarming) schedule_snooze(30);
}

static void click_config_provider(void *ctx) {
    window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
    window_single_click_subscribe(BUTTON_ID_UP,     up_click_handler);
    window_single_click_subscribe(BUTTON_ID_DOWN,   down_click_handler);
    window_long_click_subscribe(BUTTON_ID_SELECT, 700,
                                select_long_click_handler, NULL);
}

// ─── Window Lifecycle ────────────────────────────────────────────────────────

static void main_window_load(Window *window) {
    Layer *root = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(root);

    // Title / wake label
    s_wake_label = text_layer_create(GRect(0, 20, bounds.size.w, 50));
    text_layer_set_background_color(s_wake_label, GColorClear);
    text_layer_set_text_color(s_wake_label, GColorWhite);
    text_layer_set_text_alignment(s_wake_label, GTextAlignmentCenter);
    text_layer_set_font(s_wake_label, fonts_get_system_font(FONT_KEY_BITHAM_30_BLACK));
    layer_add_child(root, text_layer_get_layer(s_wake_label));

    // ASCII face / icon
    s_icon_label = text_layer_create(GRect(0, 78, bounds.size.w, 36));
    text_layer_set_background_color(s_icon_label, GColorClear);
    text_layer_set_text_color(s_icon_label, GColorYellow);
    text_layer_set_text_alignment(s_icon_label, GTextAlignmentCenter);
    text_layer_set_font(s_icon_label, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
    layer_add_child(root, text_layer_get_layer(s_icon_label));

    // Status / snooze info
    s_status_label = text_layer_create(GRect(4, 118, bounds.size.w - 8, 76));
    text_layer_set_background_color(s_status_label, GColorClear);
    text_layer_set_text_color(s_status_label, GColorLightGray);
    text_layer_set_text_alignment(s_status_label, GTextAlignmentCenter);
    text_layer_set_font(s_status_label, fonts_get_system_font(FONT_KEY_GOTHIC_14));
    layer_add_child(root, text_layer_get_layer(s_status_label));

    // Button hints (bottom)
    s_hint_label = text_layer_create(GRect(4, bounds.size.h - 52, bounds.size.w - 8, 50));
    text_layer_set_background_color(s_hint_label, GColorClear);
    text_layer_set_text_color(s_hint_label, GColorWhite);
    text_layer_set_text_alignment(s_hint_label, GTextAlignmentCenter);
    text_layer_set_font(s_hint_label, fonts_get_system_font(FONT_KEY_GOTHIC_14));
    layer_add_child(root, text_layer_get_layer(s_hint_label));

    window_set_click_config_provider(window, click_config_provider);
}

static void main_window_unload(Window *window) {
    text_layer_destroy(s_wake_label);
    text_layer_destroy(s_icon_label);
    text_layer_destroy(s_status_label);
    text_layer_destroy(s_hint_label);
}

// ─── App Lifecycle ────────────────────────────────────────────────────────────

static void app_init(void) {
    wakeup_service_subscribe(wakeup_handler);
    app_worker_message_subscribe(worker_message_handler);

    s_main_window = window_create();
    window_set_background_color(s_main_window, GColorBlack);

    WindowHandlers wh = {
        .load   = main_window_load,
        .unload = main_window_unload
    };
    window_set_window_handlers(s_main_window, wh);
    window_stack_push(s_main_window, true);

    // Ensure background worker is running
    AppWorkerResult res = app_worker_launch();
    if (res != APP_WORKER_RESULT_SUCCESS &&
        res != APP_WORKER_RESULT_ALREADY_RUNNING) {
        APP_LOG(APP_LOG_LEVEL_WARNING,
            "NapBuster: worker launch failed (result=%d)", res);
    }

    // Handle wakeup launch (snooze expired)
    if (launch_reason() == APP_LAUNCH_WAKEUP) {
        WakeupId wid;
        int32_t  cookie;
        if (wakeup_get_launch_event(&wid, &cookie)) {
            if (cookie == WAKEUP_REASON_SNOOZE) {
                // Snooze expired → ring the alarm
                persist_write_int(PERSIST_KEY_SNOOZE_UNTIL, 0);
                persist_delete(PERSIST_KEY_WAKEUP_ID_SNOOZE);
                start_alarm();
                return;
            }
        }
    }

    // Handle worker launch (sleep detected)
    if (launch_reason() == APP_LAUNCH_WORKER) {
        start_alarm();
        return;
    }

    // Normal user launch → show status
    update_status_display();
}

static void app_deinit(void) {
    if (s_vibe_timer) {
        app_timer_cancel(s_vibe_timer);
        s_vibe_timer = NULL;
    }
    vibes_cancel();
    app_worker_message_unsubscribe();
    window_destroy(s_main_window);
}

// ─── Entry Point ─────────────────────────────────────────────────────────────

int main(void) {
    app_init();
    app_event_loop();
    app_deinit();
    return 0;
}
