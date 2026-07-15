// Copyright 2024 QMK
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <stdint.h>

// Battery voltage thresholds (raw ADC units after vref normalization)
#define BATT_OFF 3000
#define BATT_5   3130
#define BATT_10  3180
#define BATT_40  3630
#define BATT_60  3760
#define BATT_80  3930
#define BATT_85  3980
#define BATT_99  4150

extern uint16_t battery_value;

void     battery_adc_init(void);
uint16_t get_adc_value(void);
uint16_t get_adc_vref(void);
uint8_t  batt_level(void);
