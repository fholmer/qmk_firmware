// Copyright 2024 Jeff (@yunziikeyboard)
// Copyright 2026 karamanliev (@karamanliev)
// Copyright 2026 Frode Holmer (@fholmer)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#define HAL_USE_PWM    TRUE
#define PAL_USE_CALLBACKS TRUE
#define SERIAL_BUFFERS_SIZE 64

#include_next <halconf.h>
