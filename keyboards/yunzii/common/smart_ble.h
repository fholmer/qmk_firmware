// Copyright 2024 QMK
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <stdint.h>
#include <stdbool.h>

void sc_ble_battary(uint8_t batt_level);
void WIRELESS_START(uint32_t mode);
void WIRELESS_STOP(void);
void WIRELESS_STOPOWER(void);
void WIRELESS_PAIR(uint32_t mode);
void smart_ble_wakeup(void);

extern uint8_t  ble_led_state;
extern bool     wireless_connected;
extern uint8_t  last_wireless_mode;
extern bool     suspend_24g;

// Activity timers — reset by report sends and keypress, used by al68.c for sleep
extern uint32_t idle_timer;
extern uint32_t keepalive_timer;
extern uint8_t  keepalive_count;
extern uint8_t  sleep_first_flag;
