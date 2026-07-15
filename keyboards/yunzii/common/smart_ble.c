// Copyright 2024 QMK
// SPDX-License-Identifier: GPL-2.0-or-later

#include "quantum.h"
#include "uart.h"
#include "ch.h"
#include "hal.h"
#include "host.h"
#include "host_driver.h"
#include "report.h"
#include "smart_ble.h"
#include "al68.h"
#include "print.h"
#include <string.h>

#define WIRELESS_MODULE_WAKE_UP_BYTES_NUM 60

// Stock firmware BLE name — module requires this exact format
static const char ble_name[] = "YUNZII AL68 BT";

uint8_t  ble_led_state = 0;
bool     wireless_connected = false;
uint8_t  last_wireless_mode = 0;
bool     suspend_24g = false;

// Activity timers — reset in report functions and by al68.c on keypress
uint32_t idle_timer;
uint32_t keepalive_timer;
uint8_t  keepalive_count = 0;
uint8_t  sleep_first_flag = 0;

extern enum kb_mode_t kb_mode;

static uint32_t keyboard_test_time;
static uint32_t time_test1;

static host_driver_t *last_host_driver = NULL;

/* -------------------- Static Function Prototypes -------------------------- */
static uint8_t sc_ble_leds(void);
static void    sc_ble_mouse(report_mouse_t *report);
static void    sc_ble_extra(report_extra_t *report);
static void    sc_ble_keyboard(report_keyboard_t *report);
static void    sc_send_nkro(report_nkro_t *report);

static host_driver_t sc_ble_driver = {
    sc_ble_leds, sc_ble_keyboard, sc_send_nkro, sc_ble_mouse, sc_ble_extra
};

/* -------------------- Rate limiting helper -------------------------------- */
static void rate_limit(void) {
    if (timer_elapsed32(keyboard_test_time) < 8 && timer_elapsed32(keyboard_test_time) > 0) {
        time_test1 = 8 - timer_elapsed32(keyboard_test_time);
        if (kb_mode != KB_MODE_24G)
            wait_ms(time_test1);
        else
            wait_ms(2);
    } else {
        if (kb_mode != KB_MODE_24G)
            wait_ms(8);
        else
            wait_ms(2);
    }
    keyboard_test_time = timer_read32();
}

/* -------------------- Activity reset helper ------------------------------- */
static void reset_activity(void) {
    idle_timer = timer_read32();
    keepalive_timer = timer_read32();
    keepalive_count = 0;
}

/* -------------------- Wake-up helper -------------------------------------- */
static void wireless_wake(uint16_t delay_ms) {
    for (int i = 0; i < WIRELESS_MODULE_WAKE_UP_BYTES_NUM; i++) {
        uart_write(0x00);
    }
    wait_ms(delay_ms);
}

/* -------------------- Module wakeup from auto-sleep ----------------------- */
void smart_ble_wakeup(void) {
    if (sleep_first_flag) {
        sleep_first_flag = 0;
        for (int i = 0; i < WIRELESS_MODULE_WAKE_UP_BYTES_NUM; i++) {
            uart_write(0x00);
        }
        wait_ms(10);
    }
}

/* -------------------- Driver switch --------------------------------------- */
static void smart_ble_startup(void) {
    if (host_get_driver() == &sc_ble_driver) {
        return;
    }
    clear_keyboard();
    last_host_driver = host_get_driver();
    ble_led_state = host_keyboard_leds();
    host_set_driver(&sc_ble_driver);
}

static void smart_ble_disconnect(void) {
    if (host_get_driver() != &sc_ble_driver) {
        return;
    }
    clear_keyboard();
    host_set_driver(last_host_driver);
}

/* -------------------- Build connect command frame ------------------------- */
static void build_connect_frame(uint8_t *buf, uint32_t mode) {
    memset(buf, 0, 22);
    buf[0] = 0x55;       // sync
    buf[1] = 20;          // payload length
    buf[2] = 0;           // command: connect
    buf[3] = mode;        // mode 1-3=BLE, 4=2.4G
    memcpy(buf + 4, ble_name, sizeof(ble_name));  // 15 bytes including null
    buf[18] = '0' + mode;
    buf[19] = 0;
}

/* -------------------- Public Functions ------------------------------------ */

void WIRELESS_START(uint32_t mode) {
    smart_ble_startup();
    wireless_connected = false;

    if (mode < 1 || mode > 4) {
        mode = 1;
    }

    if (mode == 4) {
        last_wireless_mode = last_wireless_mode | 4;
    } else {
        last_wireless_mode = mode;
    }

    uprintf("W_START mode=%u lwm=%u\n", (unsigned)mode, last_wireless_mode);

    wireless_wake(350);

    uint8_t ble_command[22];
    build_connect_frame(ble_command, mode);

    uart_transmit(ble_command, sizeof(ble_command));
    wait_ms(10);
    uart_transmit(ble_command, sizeof(ble_command));
    wait_ms(10);
}

void WIRELESS_PAIR(uint32_t mode) {
    smart_ble_startup();
    wireless_connected = false;

    if (mode < 1 || mode > 4) {
        mode = 1;
    }

    if (mode == 4) {
        last_wireless_mode = last_wireless_mode | 4;
    } else {
        last_wireless_mode = mode;
    }

    uprintf("W_PAIR mode=%u\n", (unsigned)mode);

    wireless_wake(350);

    // Pair command: 0x55 | 0x03 | 0x00 | mode | 0x01
    uint8_t cmd[5] = {0x55, 0x03, 0x00, mode, 0x01};
    uart_transmit(cmd, sizeof(cmd));
    wait_ms(10);
    uart_transmit(cmd, sizeof(cmd));
    wait_ms(10);
}

void WIRELESS_STOP(void) {
    wireless_connected = false;

    uprintf("W_STOP\n");

    for (int i = 0; i < WIRELESS_MODULE_WAKE_UP_BYTES_NUM; i++) {
        uart_write(0x00);
    }
    wait_ms(100);

    smart_ble_disconnect();
    wait_ms(20);

    uint8_t cmd[4] = {0x55, 0x02, 0x00, 0x00};
    uart_transmit(cmd, sizeof(cmd));
}

// Like WIRELESS_STOP but keeps the BLE host driver active.
// Used before deep sleep so the module can be re-woken easily.
void WIRELESS_STOPOWER(void) {
    wireless_connected = false;

    uprintf("W_STOPOWER\n");

    for (int i = 0; i < WIRELESS_MODULE_WAKE_UP_BYTES_NUM; i++) {
        uart_write(0x00);
    }
    wait_ms(100);
    // NOTE: no smart_ble_disconnect() — keep BLE driver active
    wait_ms(20);

    uint8_t cmd[4] = {0x55, 0x02, 0x00, 0x00};
    uart_transmit(cmd, sizeof(cmd));
}

void sc_ble_battary(uint8_t batt_level) {
    if (wireless_connected) {
        uart_write(0x55);
        uart_write(0x02);
        uart_write(0x09);
        uart_write(batt_level);
        wait_ms(10);
    }
}

/* -------------------- UART receive state machine -------------------------- */
static uint8_t sc_ble_leds(void) {
    static enum {
        UART_READY,
        UART_0X55_RECEIVED,
        UART_LENS_RECEIVED,
        UART_WORKMODE,
        UART_REPORT_ID_RECEIVED
    } uart_state = UART_READY;
    static uint8_t uart_command[40];
    static uint8_t uart_lens = 0;
    static uint8_t uart_workmode = 0;
    static uint8_t uart_buff_index = 0;
    uint8_t c;

    while (uart_available()) {
        c = uart_read();
        switch (uart_state) {
            case UART_READY:
                if (c == 0x55) {
                    uart_state = UART_0X55_RECEIVED;
                    uart_buff_index = 0;
                    uart_command[uart_buff_index++] = c;
                }
                break;

            case UART_0X55_RECEIVED:
                if (c == 0x55)
                    break;
                else if (c == 0x03) {
                    uart_lens = c;
                    uart_command[uart_buff_index++] = c;
                    uart_state = UART_LENS_RECEIVED;
                } else {
                    uart_state = UART_READY;
                }
                break;

            case UART_LENS_RECEIVED:
                if (c <= 2) {
                    uart_command[uart_buff_index++] = c;
                    uart_state = UART_WORKMODE;
                } else {
                    uart_state = UART_READY;
                }
                break;

            case UART_WORKMODE:
                if (c <= 4) {
                    uart_workmode = c;
                    uart_command[uart_buff_index++] = c;
                    uart_state = UART_REPORT_ID_RECEIVED;
                } else {
                    uart_state = UART_READY;
                }
                break;

            case UART_REPORT_ID_RECEIVED:
                uart_command[uart_buff_index++] = c;
                if (uart_buff_index >= uart_lens + 2) {
                    switch (uart_command[2]) {
                        case 0:  // Connection state
                            if (last_wireless_mode <= 3) {
                                if (last_wireless_mode == uart_workmode) {
                                    wireless_connected = (c == 0);
                                    uprintf("BLE conn=%u\n", wireless_connected);
                                }
                            } else {
                                if (uart_workmode == 4) {
                                    wireless_connected = (c == 0);
                                    uprintf("24G conn=%u\n", wireless_connected);
                                }
                            }
                            break;
                        case 1:  // LED state
                            if (last_wireless_mode <= 3) {
                                if (last_wireless_mode == uart_workmode) {
                                    ble_led_state = c;
                                }
                            } else {
                                if (uart_workmode == 4) {
                                    ble_led_state = c;
                                }
                            }
                            break;
                        case 2:  // 2.4G suspend state
                            if (last_wireless_mode > 3 && uart_workmode == 4) {
                                if (c == 0xAA) {
                                    suspend_24g = true;
                                } else if (c == 0xBB) {
                                    suspend_24g = false;
                                }
                            }
                            break;
                        default:
                            break;
                    }
                    uart_state = UART_READY;
                }
                break;

            default:
                uart_state = UART_READY;
                break;
        }
    }

    return ble_led_state;
}

/* -------------------- Report send functions ------------------------------- */

static void sc_ble_keyboard(report_keyboard_t *report) {
    if (!wireless_connected) return;

    smart_ble_wakeup();
    uart_write(0x55);
    uart_write(0x09);
    uart_write(0x01);
    uart_transmit((uint8_t *)report, KEYBOARD_REPORT_SIZE);
    reset_activity();
    rate_limit();
}

static void sc_send_nkro(report_nkro_t *report) {
    if (!wireless_connected) return;

    smart_ble_wakeup();
    uart_write(0x55);
    uart_write(0x12);
    uart_transmit((uint8_t *)report, 0x12);
    reset_activity();
    rate_limit();
}

static void sc_ble_mouse(report_mouse_t *report) {
    static uint8_t last_report[sizeof(report_mouse_t)] = {0};

    if (!wireless_connected) return;
    if (!has_mouse_report_changed((report_mouse_t *)last_report, report)) return;

    smart_ble_wakeup();
    memcpy(last_report, report, sizeof(report_mouse_t));
    uart_write(0x55);
    uart_write(sizeof(report_mouse_t));
    uart_transmit(last_report, sizeof(report_mouse_t));
    reset_activity();
    rate_limit();
}

static void sc_ble_extra(report_extra_t *report) {
    if (!wireless_connected) return;

    smart_ble_wakeup();
    uart_write(0x55);
    uart_write(sizeof(report_extra_t));
    uart_transmit((uint8_t *)report, sizeof(report_extra_t));
    reset_activity();
    rate_limit();
}
