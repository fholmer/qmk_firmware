// Copyright 2026 karamanliev (@karamanliev)
// Copyright 2026 Frode Holmer (@fholmer)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Bare-metal ADC driver for GD32F103 battery monitoring on B1 (channel 9).
// Uses GD32-specific ADC_OVSAMPCTL register for 10-bit resolution.

#include "adc.h"
#include "quantum.h"

// GD32-specific oversampling control register (not present on real STM32F103)
#define REG32(addr)              (*(volatile uint32_t *)(uint32_t)(addr))
#define BITS(start, end)         ((0xFFFFFFFFUL << (start)) & (0xFFFFFFFFUL >> (31U - (uint32_t)(end))))
#define ADC0_BASE                ((uint32_t)0x40012400U)
#define ADC_OVSAMPCTL(adcx)      REG32((adcx) + 0x80U)
#define ADC_OVSAMPCTL_DRES       BITS(12, 13)
#define OVSAMPCTL_DRES(regval)   (BITS(12, 13) & ((uint32_t)(regval) << 12))
#define ADC_RESOLUTION_10B       OVSAMPCTL_DRES(1)

#define ADC_SAMPLES 10

uint16_t battery_value = 0;

void battery_adc_init(void) {
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN;
    palSetLineMode(B1, PAL_MODE_INPUT_ANALOG);

    ADC1->CR2 |= (1 << 23);             // Temperature sensor enable (also enables VREF)
    RCC->CFGR &= ~(3 << 14);
    RCC->CFGR |= 3 << 14;              // ADC prescaler /8 → 9 MHz

    ADC1->CR1 &= ~(0xF << 16);         // Independent mode
    ADC1->CR1 &= ~(1 << 8);            // Non-scan mode
    ADC_OVSAMPCTL(ADC0_BASE) &= ~((uint32_t)ADC_OVSAMPCTL_DRES);
    ADC_OVSAMPCTL(ADC0_BASE) |= (uint32_t)ADC_RESOLUTION_10B;

    ADC1->CR2 &= ~(1 << 1);            // Single conversion
    ADC1->CR2 &= ~(7 << 17);
    ADC1->CR2 |= 7 << 17;              // SWSTART trigger
    ADC1->CR2 |= 1 << 20;              // External trigger enable
    ADC1->CR2 &= ~(1 << 11);           // Right-aligned

    ADC1->SQR1 &= ~(0xF << 20);        // 1 conversion in sequence

    ADC1->CR2 |= 1 << 0;               // ADC ON
    ADC1->CR2 |= 1 << 3;               // Reset calibration
    while (ADC1->CR2 & (1 << 3));
    ADC1->CR2 |= 1 << 2;               // Start calibration
    while (ADC1->CR2 & (1 << 2));
}

static void adc_channel_set(uint8_t ch) {
    if (ch < 10) {
        ADC1->SMPR2 &= ~(7 << (3 * ch));
        ADC1->SMPR2 |= 7 << (3 * ch);  // 239.5 cycles sample time
    } else {
        ADC1->SMPR1 &= ~(7 << (3 * (ch - 10)));
        ADC1->SMPR1 |= 7 << (3 * (ch - 10));
    }
}

static uint32_t adc_get_result(uint8_t ch) {
    adc_channel_set(ch);
    ADC1->SQR3 &= ~(0x1F << 0);
    ADC1->SQR3 |= ch << 0;
    ADC1->CR2 |= 1 << 22;              // Start conversion
    while (!(ADC1->SR & (1 << 1)));     // Wait for EOC
    return ADC1->DR;
}

// Median-filtered ADC read: take N samples, sort, average middle N-2
uint16_t get_adc_value(void) {
    uint16_t buf[ADC_SAMPLES];
    for (uint8_t i = 0; i < ADC_SAMPLES; i++) {
        buf[i] = adc_get_result(9);     // Channel 9 = B1
    }
    // Bubble sort
    for (uint8_t j = 0; j < ADC_SAMPLES - 1; j++) {
        for (uint8_t i = 0; i < ADC_SAMPLES - j - 1; i++) {
            if (buf[i] > buf[i + 1]) {
                uint16_t tmp = buf[i];
                buf[i] = buf[i + 1];
                buf[i + 1] = tmp;
            }
        }
    }
    uint32_t sum = 0;
    for (uint8_t i = 1; i < ADC_SAMPLES - 1; i++) {
        sum += buf[i];
    }
    return sum / (ADC_SAMPLES - 2);
}

// Read internal VREF (channel 17) with same filtering
uint16_t get_adc_vref(void) {
    uint16_t buf[ADC_SAMPLES];
    for (uint8_t i = 0; i < ADC_SAMPLES; i++) {
        buf[i] = adc_get_result(17);
    }
    for (uint8_t j = 0; j < ADC_SAMPLES - 1; j++) {
        for (uint8_t i = 0; i < ADC_SAMPLES - j - 1; i++) {
            if (buf[i] > buf[i + 1]) {
                uint16_t tmp = buf[i];
                buf[i] = buf[i + 1];
                buf[i + 1] = tmp;
            }
        }
    }
    uint32_t sum = 0;
    for (uint8_t i = 1; i < ADC_SAMPLES - 1; i++) {
        sum += buf[i];
    }
    return sum / (ADC_SAMPLES - 2);
}

uint8_t batt_level(void) {
    uint8_t level = 100;

    if (battery_value > BATT_OFF && battery_value <= BATT_5) {
        level = ((battery_value - BATT_OFF) * 5) / (BATT_5 - BATT_OFF);
    } else if (battery_value > BATT_5 && battery_value <= BATT_10) {
        level = ((battery_value - BATT_5) * 5) / (BATT_10 - BATT_5) + 5;
    } else if (battery_value > BATT_10 && battery_value <= BATT_40) {
        level = ((battery_value - BATT_10) * 30) / (BATT_40 - BATT_10) + 10;
    } else if (battery_value > BATT_40 && battery_value <= BATT_60) {
        level = ((battery_value - BATT_40) * 20) / (BATT_60 - BATT_40) + 40;
    } else if (battery_value > BATT_60 && battery_value <= BATT_80) {
        level = ((battery_value - BATT_60) * 20) / (BATT_80 - BATT_60) + 60;
    } else if (battery_value > BATT_80 && battery_value <= BATT_85) {
        level = ((battery_value - BATT_80) * 5) / (BATT_85 - BATT_80) + 80;
    } else if (battery_value > BATT_85 && battery_value <= BATT_99) {
        level = ((battery_value - BATT_85) * 14) / (BATT_99 - BATT_85) + 85;
    } else if (battery_value > BATT_99) {
        level = 100;
    } else if (battery_value < BATT_OFF) {
        level = 0;
    }

    return level;
}
