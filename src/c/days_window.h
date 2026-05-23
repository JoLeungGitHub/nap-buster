/**
 * days_window.h — NapBuster Day Picker
 *
 * Push a sub-screen where the user can toggle each day of the week
 * on or off. Works exactly like a standard alarm clock day selector.
 *
 * Active days are stored as a bitmask in persist (PERSIST_KEY_ACTIVE_DAYS):
 *   bit 0 = Sunday, bit 1 = Monday, ..., bit 6 = Saturday  (tm_wday order)
 *   0x7F (127) = every day  |  0x3E (62) = Mon-Fri  |  0x41 (65) = Sat+Sun
 */

#pragma once
#include <pebble.h>

/** Push the day-picker window onto the navigation stack. */
void days_window_push(void);

/** Return a human-readable summary of the current active-days bitmask.
 *  buf must be at least 12 bytes. Examples: "Every day", "Weekdays",
 *  "Weekends", "5 days". */
void days_summary(char *buf, size_t len);
