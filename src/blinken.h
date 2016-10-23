/**
 * RTL8710 Blinkenlights. WiFi-controlled WS2812B LED strip.
 * Copyright (C) 2016  Tido Klaassen <tido@4gh.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#ifndef __BLINKEN_H__
#define __BLINKEN_H__

#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include "ws2812.h"

#define min(a,b)            ((a) < (b) ? (a) : (b))
#define max(a,b)            ((a) > (b) ? (a) : (b))
#define scale_up(x)         ((x) << 8)
#define scale_down(x)       ((x) >> 8)

#define BLINKEN_CFG_MAGIC   0x424C4E4B // "BLNK"
#define BLINKEN_CFG_VER     0x1
#define BLINKEN_MAX_LEDS    WS2812_MAX_LEDS
#define BLINKEN_MAX_STEPS   500

struct cfg_rainbow {
    uint32_t valid;
    uint32_t hue_min;
    uint32_t hue_max;
    uint32_t hue_steps;
    uint32_t cycle_steps;
} __attribute__((packed));

struct cfg_fade {
    uint32_t valid;
    uint32_t min;
    uint32_t max;
    uint32_t steps;
} __attribute__((packed));

struct cfg_flicker {
    uint32_t valid;
    uint32_t rate;
} __attribute__((packed));

struct cfg_eye {
    uint32_t valid;
    uint32_t rate;
} __attribute__((packed));

struct blinken_cfg {
    uint32_t magic;
    uint32_t version;
    uint32_t strip_len;
    uint32_t delay; 
    uint32_t brightness;

    struct cfg_rainbow rainbow;
    struct cfg_fade    fade;
    struct cfg_flicker flicker;
    struct cfg_eye     eye;
} __attribute__((packed));

extern struct blinken_cfg *blinken_get_config(void);
extern int blinken_set_config(struct blinken_cfg *cfg);

#endif
