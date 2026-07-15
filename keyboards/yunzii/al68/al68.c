// Copyright 2024 QMK
// SPDX-License-Identifier: GPL-2.0-or-later

#include "quantum.h"
#include "uart.h"
#include "print.h"
#include "usb_util.h"
#include "usb_main.h"
#include "common/smart_ble.h"
#include "al68.h"
#include "adc.h"

// ─────────────────────────────────────────────────────────────────────────────
// RGB LED configuration — 68 LEDs, physical WS2812 chain order
// ─────────────────────────────────────────────────────────────────────────────

#define __ NO_LED

led_config_t g_led_config = {
    {
        // Key Matrix to LED Index
        // Chain positions 3-4 are the indicator bar, not keys: the two
        // bottom-row keys that have no LED of their own map to NO_LED.
        {  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  __ },
        {  53,  52,  51,  50,  49,  48,  47,  46,  45,  44,  43,  42,  41,  40,  39 },
        {  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  __,  37,  38 },
        {  24,  __,  20,  19,  18,  17,  16,  15,  14,  13,  12,  11,  10,   9,   8 },
        {  23,  22,  21,  __,  __,   0,  __,  __,  __,  __,   1,   2,   5,   6,   7 },
    },
    {
        // LED Index to Physical Position (3-4 = indicator bar at x=187)
        {89,64}, {150,64}, {170,64}, {187,55}, {187,59}, {194,64}, {209,64}, {224,64}, {224,48},
        {224,48}, {189,48}, {168,48}, {153,48}, {138,48}, {123,48}, {108,48}, {93,48}, {78,48},
        {63,48}, {48,48}, {33,48}, {39,48}, {19,64}, {2,64}, {8,48}, {6,32}, {26,32}, {41,32},
        {56,32}, {71,32}, {86,32}, {101,32}, {116,32}, {131,32}, {146,32}, {161,32}, {176,32},
        {201,32}, {224,32}, {224,15}, {207,15}, {187,15}, {172,15}, {157,15}, {142,15}, {127,15},
        {112,15}, {97,15}, {82,15}, {67,15}, {52,15}, {37,15}, {22,15}, {4,15}, {0,0}, {15,0},
        {30,0}, {45,0}, {60,0}, {75,0}, {90,0}, {105,0}, {120,0}, {135,0}, {150,0},
        {165,0}, {180,0}, {203,0}
    },
    {
        // LED Flags (4 = LED_FLAG_KEYLIGHT; 0 on the indicator bar at 3-4
        // so RGB effects leave it alone)
        4, 4, 4, 0, 0, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
        4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
        4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
        4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    }
};

// Indicator bar: 2 LEDs mid-chain (position 3 = upper, 4 = lower half)
#define IND_LED_UPPER 3
#define IND_LED_LOWER 4

#undef __

extern void stm32_clock_init(void);

enum kb_mode_t kb_mode = KB_MODE_DEFAULT;
static enum kb_mode_t prev_kb_mode = KB_MODE_DEFAULT;

// Long-press tracking for BLE pairing
static uint32_t ble_key_timer = 0;
static uint8_t  ble_key_mode = 0;

// Indicator-bar pairing state (rendered in indicator_bar_render)
static bool     ind_pairing = false;
static uint32_t ind_pairing_timer = 0;

// Battery
static uint8_t  battery_level_pct = 100;
static uint8_t  battery_level_smooth = 100;
static uint32_t battery_timer = 0;
static bool     battery_initialized = false;
static bool     battery_display_active = false;

// Connection timeout
static uint32_t connect_timer = 0;

// LED idle tracking
static bool     leds_off_for_idle = false;

// Connection state transition tracking
static bool     prev_wireless_connected = false;

// Debug heartbeat
static uint32_t debug_timer = 0;

// Deferred EEPROM save (flash write stalls CPU ~20ms, breaks UART)
static bool     ble_save_pending = false;
static uint32_t ble_save_timer = 0;

// ─────────────────────────────────────────────────────────────────────────────
// Mode detection
// ─────────────────────────────────────────────────────────────────────────────

static void get_mode(void) {
    if (!gpio_read_pin(BLE_PIN)) {
        kb_mode = KB_MODE_BLE;
    } else if (!gpio_read_pin(TWO_MODE_PIN)) {
        kb_mode = KB_MODE_24G;
    } else {
        kb_mode = KB_MODE_USB;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Battery
// ─────────────────────────────────────────────────────────────────────────────

static void battery_update(void) {
    uint16_t adc_val = get_adc_value();
    uint16_t adc_vref = get_adc_vref();
    battery_value = (adc_val * 1764) / adc_vref;
    battery_level_pct = batt_level();

    // USB plugged in with very low ADC → no battery installed, report 100%
    if (gpio_read_pin(PLUG_IN_PIN) && battery_value < 1000) {
        battery_level_pct = 100;
    }

    // Smooth: limit change to ±1% per check interval
    if (!battery_initialized) {
        battery_level_smooth = battery_level_pct;
        battery_initialized = true;
    } else {
        bool charging = gpio_read_pin(PLUG_IN_PIN);
        if (charging) {
            if (battery_level_pct > battery_level_smooth + 1) {
                battery_level_smooth++;
            } else {
                battery_level_smooth = battery_level_pct;
            }
        } else {
            if (battery_level_pct < battery_level_smooth - 1) {
                battery_level_smooth--;
            } else if (battery_level_pct <= battery_level_smooth) {
                battery_level_smooth = battery_level_pct;
            }
            // Don't increase on battery (noise/LED state change)
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Deep sleep (MCU STOP mode via WFI)
// ─────────────────────────────────────────────────────────────────────────────

static void enter_deep_sleep(void) {
    uprintf("SLEEP\n");

    // Flush any pending EEPROM save before sleep
    if (ble_save_pending) {
        ble_save_pending = false;
        eeconfig_update_kb((uint32_t)(last_wireless_mode & 3));
    }

    // Turn off LEDs before sleep — flush black data to WS2812 chain.
    // The flush is an async PWM/DMA transfer taking ~2.3 ms for 68 LEDs;
    // entering STOP mid-transfer freezes the driver in a busy state and
    // corrupts the WS2812 signal on wake (all LEDs latch white). Wait for
    // the full frame + reset pulse to finish.
    rgb_matrix_set_color_all(0, 0, 0);
    rgb_matrix_driver.flush();
    rgb_matrix_disable_noeeprom();
    wait_ms(5);

    // Stop wireless module (keep BLE driver for easy re-start)
    WIRELESS_STOPOWER();
    wait_ms(10);

    // Stop USB
    usb_disconnect();
    palSetLineMode(RENUM_PIN, PAL_MODE_INPUT_ANALOG);

    // Float USB data pins if no cable
    if (!gpio_read_pin(PLUG_IN_PIN)) {
        palSetLineMode(A11, PAL_MODE_INPUT_ANALOG);
        palSetLineMode(A12, PAL_MODE_INPUT_ANALOG);
    }

    // Configure matrix for wake detection (ROW2COL)
    static const pin_t col_pins[] = MATRIX_COL_PINS;
    static const pin_t row_pins[] = MATRIX_ROW_PINS;
    for (uint8_t i = 0; i < MATRIX_COLS; i++) {
        if (col_pins[i] != NO_PIN) {
            gpio_set_pin_output(col_pins[i]);
            gpio_write_pin_low(col_pins[i]);
        }
    }
    for (uint8_t i = 0; i < MATRIX_ROWS; i++) {
        if (row_pins[i] != NO_PIN) {
            gpio_set_pin_input_high(row_pins[i]);
            palEnableLineEvent(row_pins[i], PAL_EVENT_MODE_FALLING_EDGE);
        }
    }

    // Mode switch and USB plug wake sources
    gpio_set_pin_input(BLE_PIN);
    gpio_set_pin_input(TWO_MODE_PIN);
    palEnableLineEvent(BLE_PIN, PAL_EVENT_MODE_BOTH_EDGES);
    palEnableLineEvent(TWO_MODE_PIN, PAL_EVENT_MODE_BOTH_EDGES);
    palEnableLineEvent(PLUG_IN_PIN, PAL_EVENT_MODE_BOTH_EDGES);

    // Encoder wake sources
    gpio_set_pin_input(ENCODER_PIN_A);
    gpio_set_pin_input(ENCODER_PIN_B);
    palEnableLineEvent(ENCODER_PIN_A, PAL_EVENT_MODE_RISING_EDGE);
    palEnableLineEvent(ENCODER_PIN_B, PAL_EVENT_MODE_RISING_EDGE);

    // Disable ADC to save power
    ADC1->CR2 &= ~ADC_CR2_ADON;

    // Enable PWR clock for STOP mode access
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;

    // Enter STOP mode (low-power regulator, wake on any EXTI)
    PWR->CR |= (1 << 0) | (1 << 10) | (1 << 11) | (3 << 18);
    PWR->CR |= PWR_CR_CWUF | PWR_CR_CSBF;
    SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
    __WFI();
    SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;

    // ─── WOKE UP ───

    // Disable all EXTI events we configured (lines 0-15) and clear any
    // pending flags so a stale edge can't fire once interrupts resume
    // (the reference firmware clears PR here too)
    EXTI->IMR  &= ~0xFFFF;
    EXTI->EMR  &= ~0xFFFF;
    EXTI->RTSR &= ~0xFFFF;
    EXTI->FTSR &= ~0xFFFF;
    EXTI->PR    = 0xFFFF;

    // Re-init clocks (after STOP, MCU runs on HSI 8MHz)
    stm32_clock_init();

    // Re-init pins
    gpio_set_pin_input(BLE_PIN);
    gpio_set_pin_input(TWO_MODE_PIN);
    gpio_set_pin_input(PLUG_IN_PIN);
    gpio_set_pin_output(RENUM_PIN);
    gpio_write_pin_high(RENUM_PIN);

    // Re-init USB
    init_usb_driver(&USB_DRIVER);

    // Re-init matrix
    matrix_init();

    // Re-init ADC
    battery_adc_init();

    // Re-init UART to the BLE module and drop any stale key state
    // (reference firmware does clear_keyboard() on wake)
    uart_init(460800);
    clear_keyboard();

    // Check mode and start wireless
    get_mode();
    if (kb_mode == KB_MODE_BLE) {
        last_wireless_mode &= 3;
        if (last_wireless_mode == 0) last_wireless_mode = 1;
        WIRELESS_START(last_wireless_mode);
    } else if (kb_mode == KB_MODE_24G) {
        WIRELESS_START(4);
    }

    // Re-enable LEDs after sleep
    rgb_matrix_enable_noeeprom();

    // Reset all timers
    idle_timer = timer_read32();
    keepalive_timer = timer_read32();
    keepalive_count = 0;
    connect_timer = timer_read32();
    battery_timer = timer_read32();
    sleep_first_flag = 1;
    leds_off_for_idle = false;
    suspend_24g = false;
    prev_kb_mode = kb_mode;
    prev_wireless_connected = false;
    debug_timer = timer_read32();

    uprintf("WAKE mode=%u\n", kb_mode);
}

// ─────────────────────────────────────────────────────────────────────────────
// Init
// ─────────────────────────────────────────────────────────────────────────────

// The weak upstream bootloader_jump() is a bare NVIC_SystemReset(); it relies
// on the DFU flag already being set by board.c. Since we clear that flag in
// keyboard_pre_init_kb(), set it here so QK_BOOT still enters the bootloader.
void bootloader_jump(void) {
    BKP->DR10 = RTC_BOOTLOADER_FLAG;
    NVIC_SystemReset();
}

void keyboard_pre_init_kb(void) {
    // board.c (STM32_F103_STM32DUINO) sets BKP->DR10 = RTC_BOOTLOADER_FLAG on
    // every boot. BKP registers are battery-backed, so on this board the flag
    // survives a replug and the bootloader stays in DFU mode. Clear it so a
    // normal reboot works.
    BKP->DR10 = 0;

    // Free JTAG pins for matrix use (GD32F103 requires two-step write)
    AFIO->MAPR = (AFIO->MAPR & ~AFIO_MAPR_SWJ_CFG_Msk);
    AFIO->MAPR |= AFIO_MAPR_SWJ_CFG_DISABLE;

    gpio_set_pin_output(RENUM_PIN);

    gpio_set_pin_input(BLE_PIN);
    gpio_set_pin_input(TWO_MODE_PIN);
    gpio_set_pin_input(PLUG_IN_PIN);

    // Only enable USB transceiver if in USB mode or USB cable is present.
    // If wireless mode on battery, disable USB so QMK never enters USB suspend.
    bool wireless_switch = !gpio_read_pin(BLE_PIN) || !gpio_read_pin(TWO_MODE_PIN);
    if (!wireless_switch || gpio_read_pin(PLUG_IN_PIN)) {
        gpio_write_pin_high(RENUM_PIN);
    } else {
        gpio_write_pin_low(RENUM_PIN);
    }

    uart_init(460800);
    wait_ms(600);
}

void eeconfig_init_kb(void) {
    eeconfig_update_kb(1);  // Default: BLE profile 1
    eeconfig_init_user();
}

void keyboard_post_init_kb(void) {
    battery_adc_init();
    battery_update();

    // Restore last BLE profile from EEPROM
    uint8_t stored = eeconfig_read_kb() & 0xFF;
    if (stored >= 1 && stored <= 3) {
        last_wireless_mode = stored;
    }

    // Init activity timers
    idle_timer = timer_read32();
    keepalive_timer = timer_read32();
    connect_timer = timer_read32();
    battery_timer = timer_read32();

    keyboard_post_init_user();
}

// ─────────────────────────────────────────────────────────────────────────────
// Housekeeping — mode switch, keepalive, battery, sleep
// ─────────────────────────────────────────────────────────────────────────────

void housekeeping_task_kb(void) {
    get_mode();

    // Persist the BLE channel a few seconds after switching. Previously this
    // was only flushed on deep sleep, so a power-cycle before the keyboard
    // ever slept lost the switch and boot always came back on channel 1.
    if (ble_save_pending && timer_elapsed32(ble_save_timer) > 3000) {
        ble_save_pending = false;
        eeconfig_update_kb((uint32_t)(last_wireless_mode & 3));
        uprintf("BLE profile %u saved\n", last_wireless_mode & 3);
    }

    // ── Mode change handling ──
    if (kb_mode != prev_kb_mode) {
        uprintf("MODE: %u -> %u (pins: BLE=%u 24G=%u PLUG=%u)\n",
                prev_kb_mode, kb_mode,
                (unsigned)gpio_read_pin(BLE_PIN), (unsigned)gpio_read_pin(TWO_MODE_PIN),
                (unsigned)gpio_read_pin(PLUG_IN_PIN));
        prev_kb_mode = kb_mode;

        if (kb_mode == KB_MODE_BLE) {
            if (!gpio_read_pin(PLUG_IN_PIN)) {
                gpio_write_pin_low(RENUM_PIN);
            }
            // Reference firmware: NO WIRELESS_STOP for wireless→wireless.
            // Just call WIRELESS_START directly — module handles transitions.
            last_wireless_mode &= 3;
            if (last_wireless_mode == 0) last_wireless_mode = 1;
            WIRELESS_START(last_wireless_mode);
        } else if (kb_mode == KB_MODE_24G) {
            if (!gpio_read_pin(PLUG_IN_PIN)) {
                gpio_write_pin_low(RENUM_PIN);
            }
            WIRELESS_START(4);
        } else if (kb_mode == KB_MODE_USB) {
            gpio_write_pin_high(RENUM_PIN);
            WIRELESS_STOP();
            if (USB_DRIVER.state == USB_STOP) {
                init_usb_driver(&USB_DRIVER);
            }
        }

        // Reset connection timeout on mode change
        connect_timer = timer_read32();
        idle_timer = timer_read32();
        keepalive_timer = timer_read32();
        keepalive_count = 0;
        leds_off_for_idle = false;
        suspend_24g = false;
    }

    // Everything below only applies to wireless modes
    if (kb_mode != KB_MODE_BLE && kb_mode != KB_MODE_24G) {
        return;
    }

    // ── Connection state transition handling ──
    if (wireless_connected != prev_wireless_connected) {
        uprintf("CONN: %u->%u mode=%u lwm=%u\n",
                prev_wireless_connected, wireless_connected, kb_mode, last_wireless_mode);
        if (wireless_connected) {
            // Just connected — reset all activity/keepalive timers
            idle_timer = timer_read32();
            keepalive_timer = timer_read32();
            keepalive_count = 0;
        }
        // Reset connect_timer on any transition so 20s counts from state change
        connect_timer = timer_read32();
        prev_wireless_connected = wireless_connected;
    }

    // ── Keepalive: 5 bytes every 8s when connected (max 15 packets) ──
    if (wireless_connected) {
        if (timer_elapsed32(keepalive_timer) >= KEEPALIVE_INTERVAL_MS &&
            keepalive_count < KEEPALIVE_MAX_PACKETS) {
            keepalive_timer = timer_read32();
            keepalive_count++;
            for (int i = 0; i < 5; i++) {
                uart_write(0x00);
            }
        }
        // After max keepalive packets, module will auto-sleep (~30s).
        // Set flag so next report wakes it.
        if (keepalive_count >= KEEPALIVE_MAX_PACKETS) {
            sleep_first_flag = 1;
        }
    }

    // ── Battery monitoring (every 30s) ──
    if (timer_elapsed32(battery_timer) > BATTERY_CHECK_INTERVAL_MS) {
        battery_timer = timer_read32();
        battery_update();
        // Send battery level over BLE when idle
        if (wireless_connected && kb_mode == KB_MODE_BLE) {
            sc_ble_battary(battery_level_smooth);
        }
    }

    // ── LED idle timeout: turn off after 3 min ──
    if (!leds_off_for_idle &&
        timer_elapsed32(idle_timer) > WIRELESS_LED_TIMEOUT_MS) {
        leds_off_for_idle = true;
        rgb_matrix_disable_noeeprom();
    }

    // ── Periodic debug heartbeat ──
    if (timer_elapsed32(debug_timer) > DEBUG_INTERVAL_MS) {
        debug_timer = timer_read32();
        uprintf("DBG: m=%u c=%u batt=%u%% bv=%u idle=%lus ka=%u slp=%u plug=%u\n",
                kb_mode, wireless_connected, battery_level_smooth,
                battery_value, timer_elapsed32(idle_timer) / 1000,
                keepalive_count, sleep_first_flag,
                (unsigned)gpio_read_pin(PLUG_IN_PIN));
    }

    // ── Sleep conditions ──
    bool should_sleep = false;

    // Connection timeout: not connected for 20s → sleep
    if (!wireless_connected &&
        timer_elapsed32(connect_timer) > CONNECT_TIMEOUT_MS) {
        uprintf("SLEEP: connect timeout\n");
        should_sleep = true;
    }

    // Connected idle sleep: idle on battery → sleep (after LEDs already off)
    if (wireless_connected && !gpio_read_pin(PLUG_IN_PIN) &&
        timer_elapsed32(idle_timer) > CONNECTED_IDLE_SLEEP_MS) {
        uprintf("SLEEP: idle %lus\n", timer_elapsed32(idle_timer) / 1000);
        should_sleep = true;
    }

    // Battery empty → sleep
    if (!gpio_read_pin(PLUG_IN_PIN) && battery_level_smooth == 0 && battery_initialized) {
        uprintf("SLEEP: battery empty\n");
        should_sleep = true;
    }

    // 2.4G host suspended → sleep
    if (suspend_24g) {
        suspend_24g = false;
        uprintf("SLEEP: 2.4G suspend\n");
        should_sleep = true;
    }

    if (should_sleep) {
        enter_deep_sleep();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// USB suspend handling
// ─────────────────────────────────────────────────────────────────────────────

// Called from QMK's USB suspend loop. If the physical switch is in wireless
// position, break out of suspend so housekeeping can handle the mode change.
void suspend_power_down_kb(void) {
    // Read physical pins directly — kb_mode may be stale during USB suspend
    bool wireless_switch = !gpio_read_pin(BLE_PIN) || !gpio_read_pin(TWO_MODE_PIN);
    if (wireless_switch) {
        gpio_write_pin_low(RENUM_PIN);
        usb_disconnect();  // USB_DRIVER.state → USB_STOP, exits suspend loop
        return;
    }
    suspend_power_down_user();
}

// ─────────────────────────────────────────────────────────────────────────────
// Keypress handling
// ─────────────────────────────────────────────────────────────────────────────

bool process_record_kb(uint16_t keycode, keyrecord_t *record) {
    if (record->event.pressed) {
        // Reset all activity timers on keypress
        idle_timer = timer_read32();
        keepalive_timer = timer_read32();
        connect_timer = timer_read32();
        keepalive_count = 0;

        // Wake LEDs if they were turned off for idle
        if (leds_off_for_idle) {
            leds_off_for_idle = false;
            rgb_matrix_enable_noeeprom();
        }

        // Send keepalive bytes if not connected (helps module detect activity)
        if ((kb_mode == KB_MODE_BLE || kb_mode == KB_MODE_24G) && !wireless_connected) {
            for (int i = 0; i < 5; i++) {
                uart_write(0x00);
            }
        }
    }

    switch (keycode) {
        case KC_BLE1:
        case KC_BLE2:
        case KC_BLE3:
            if (kb_mode == KB_MODE_BLE) {
                uint8_t profile = keycode - KC_USB;  // 1, 2, or 3
                if (record->event.pressed) {
                    ble_key_timer = timer_read32();
                    ble_key_mode = profile;
                    if (last_wireless_mode != profile) {
                        WIRELESS_START(profile);
                        ble_save_pending = true;
                        ble_save_timer = timer_read32();
                        connect_timer = timer_read32();
                    }
                } else {
                    // On release: if held long enough, enter pairing mode
                    if (ble_key_mode == profile &&
                        timer_elapsed32(ble_key_timer) > BLE_PAIR_HOLD_MS) {
                        uprintf("BLE PAIR mode=%u\n", profile);
                        WIRELESS_PAIR(profile);
                        connect_timer = timer_read32();
                        ind_pairing = true;
                        ind_pairing_timer = timer_read32();
                    }
                    ble_key_mode = 0;
                }
            }
            return false;


        case KC_BAT:
            battery_display_active = record->event.pressed;
            return false;
    }
    return process_record_user(keycode, record);
}

// ─────────────────────────────────────────────────────────────────────────────
// RGB indicators — battery display, wireless status
// ─────────────────────────────────────────────────────────────────────────────

// LED indices for number keys 1-0
#define LED_NUM_1 55
#define LED_NUM_COUNT 10

// ─────────────────────────────────────────────────────────────────────────────
// Indicator bar — wireless status on the elongated lamp (chain LEDs 3-4)
//
// USB mode:            off
// Wireless, connected: solid channel color for a few seconds, then off
// Wireless, searching: slow blink in channel color
// Pairing (long-press): fast blink in channel color
// Channel colors: BT1 blue, BT2 cyan, BT3 magenta, 2.4G green
// ─────────────────────────────────────────────────────────────────────────────

#define IND_CONNECTED_SHOW_MS 3000
#define IND_PAIRING_WINDOW_MS 30000
#define IND_BLINK_SLOW_MS 500
#define IND_BLINK_FAST_MS 150

static void indicator_bar_render(void) {
    static bool     prev_conn = false;
    static uint32_t conn_show_timer = 0;

    if (kb_mode != KB_MODE_BLE && kb_mode != KB_MODE_24G) {
        rgb_matrix_set_color(IND_LED_UPPER, 0, 0, 0);
        rgb_matrix_set_color(IND_LED_LOWER, 0, 0, 0);
        prev_conn = wireless_connected;
        return;
    }

    // Channel color
    uint8_t r = 0, g = 0, b = 0;
    if (kb_mode == KB_MODE_24G) {
        g = 255;
    } else {
        switch (last_wireless_mode) {
            case 1: b = 255; break;              // BT1 blue
            case 2: g = 255; b = 255; break;     // BT2 cyan
            default: r = 255; b = 255; break;    // BT3 magenta
        }
    }

    bool show;
    if (wireless_connected) {
        ind_pairing = false;
        if (!prev_conn) conn_show_timer = timer_read32();
        show = timer_elapsed32(conn_show_timer) < IND_CONNECTED_SHOW_MS;
    } else {
        if (ind_pairing && timer_elapsed32(ind_pairing_timer) > IND_PAIRING_WINDOW_MS) {
            ind_pairing = false;
        }
        uint32_t period = ind_pairing ? IND_BLINK_FAST_MS : IND_BLINK_SLOW_MS;
        show = (timer_read32() / period) & 1;
    }
    prev_conn = wireless_connected;

    if (!show) r = g = b = 0;
    rgb_matrix_set_color(IND_LED_UPPER, r, g, b);
    rgb_matrix_set_color(IND_LED_LOWER, r, g, b);
}

bool rgb_matrix_indicators_advanced_kb(uint8_t led_min, uint8_t led_max) {
    // Battery display when KC_BAT is held
    if (battery_display_active) {
        rgb_matrix_set_color_all(0, 0, 0);

        uint8_t count = battery_level_smooth / 10;

        // Color: red ≤30%, yellow 40-70%, green ≥80%
        uint8_t r, g, b;
        if (count <= 3) {
            r = 255; g = 0; b = 0;
        } else if (count <= 7) {
            r = 255; g = 255; b = 0;
        } else {
            r = 0; g = 255; b = 0;
        }

        for (uint8_t i = 0; i < count && i < LED_NUM_COUNT; i++) {
            rgb_matrix_set_color(LED_NUM_1 + i, r, g, b);
        }

        return false;  // Skip user indicators during battery display
    }

    // Blank LEDs when wireless but not connected (saves battery, visual feedback)
    if (!wireless_connected && (kb_mode == KB_MODE_BLE || kb_mode == KB_MODE_24G)) {
        rgb_matrix_set_color_all(0, 0, 0);
    }

    indicator_bar_render();

    return rgb_matrix_indicators_advanced_user(led_min, led_max);
}
