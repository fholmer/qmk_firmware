// Copyright 2024 Jeff (@yunziikeyboard)
// Copyright 2026 karamanliev (@karamanliev)
// Copyright 2026 Frode Holmer (@fholmer)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Hardware pin definitions
#define BLE_PIN      C15
#define TWO_MODE_PIN C14
#define PLUG_IN_PIN  B9
#define RENUM_PIN    A8
#define ENCODER_PIN_A A7
#define ENCODER_PIN_B A6

// Timing constants
#define BLE_PAIR_HOLD_MS          3000
#define KEEPALIVE_INTERVAL_MS     8000
#define KEEPALIVE_MAX_PACKETS     15
#define WIRELESS_LED_TIMEOUT_MS   180000   // 3 min — turn off LEDs when idle
#define CONNECTED_IDLE_SLEEP_MS   185000   // ~3 min + 5s — sleep after LEDs already off
#define CONNECT_TIMEOUT_MS        20000    // 20s — sleep if wireless not connected
#define PAIRING_TIMEOUT_MS        60000    // 60s — stay awake after a pairing request
#define BATTERY_CHECK_INTERVAL_MS 30000    // 30s
#define DEBUG_INTERVAL_MS         10000    // 10s — periodic debug heartbeat

enum kb_mode_t {
    KB_MODE_USB = 0,
    KB_MODE_BLE,
    KB_MODE_24G,
    KB_MODE_DEFAULT
};

enum custom_keycodes {
    KC_USB = QK_KB_0,
    KC_BLE1,
    KC_BLE2,
    KC_BLE3,
    KC_24G,
    KC_BAT
};
