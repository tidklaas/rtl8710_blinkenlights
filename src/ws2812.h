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
#ifndef __WS2812_H__
#define __WS2812_H__
#include "main.h"

#include <stdbool.h>
#include <spi_api.h>
#include <spi_ex_api.h>
#include "semphr.h"
#include "event_groups.h"

#define WS2812_MAX_LEDS         500
#define WS2812_RESET_LEN        (50 / 2)
#define WS2812_DMABUF_LEN(x)    ((x) * 3 * 4 + WS2812_RESET_LEN)

/* we send two WS2812-bits per byte, one bit per nibble. */
#define WS_BITS_00              0x88
#define WS_BITS_01              0x8e
#define WS_BITS_10              0xe8
#define WS_BITS_11              0xee

typedef struct {
    uint8_t     red;
    uint8_t     green;
    uint8_t     blue;
} rgbValue_t;

typedef struct {
    uint8_t     hue;
    uint8_t     saturation;
    uint8_t     value;
} hsvValue_t;

typedef struct {
    spi_t               spi_master;
    EventGroupHandle_t *events;
    SemaphoreHandle_t  *mutex;
    volatile uint8_t   *dma_buff;
    volatile size_t     buff_len;
    uint16_t            strip_len;
} ws2812_t;

extern ws2812_t *ws2812_init(uint16_t strip_len);
extern int ws2812_set_len(ws2812_t *cfg, uint16_t strip_len);
extern int ws2812_deinit(ws2812_t *cfg);
extern int ws2812_update(ws2812_t *cfg, hsvValue_t hsv_values[],
                         unsigned int strip_len, uint16_t delay);

/* scale uint8 value from range 2-255 to range 0-scale */
static inline uint8_t scale(uint8_t value, uint8_t scale)
{
    uint32_t tmp;

    tmp = value * scale;
    tmp /= 256;

    return (uint8_t) tmp;
}



#endif
